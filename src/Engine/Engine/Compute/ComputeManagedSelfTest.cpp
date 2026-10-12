//====================================================================================
//                          ComputeManagedSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                       10/12/2026
//                                          C# の Compute.ForEach と C# の CPU 版の回帰テスト
//====================================================================================
#include "Engine/Engine/Compute/ComputeManagedSelfTest.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/ComponentRegistry.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Core/Jobs/JobSystem.h"
#include "Engine/Engine/Compute/ComputeService.h"
#include "Engine/Engine/Compute/KernelHash.h"
#include "Engine/Engine/Compute/KernelRange.h"
#include "Engine/Engine/Demo/ShowcaseScenes.h"
#include "Engine/Engine/HotReload/ReloadHub.h"
#include "Engine/Engine/Loop/HeadlessSim.h"
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Engine/Script/ManagedHost.h"
#include "Engine/Platform/Input.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/Shader/ShaderManager.h"
#include "Shared/KernelCompat.h"
#include "Shared/KernelRangeFn.h"

// C++ の CPU 版。ComputeScriptSelfTest.cpp などとは別の名前空間で同じカーネルを取り込む
namespace mye::kernel::cpu::MgdSaxpy {
#include "../../../../assets/shaders/kernels/SelfTestSaxpy.kernel.hlsl"
}
namespace mye::kernel::cpu::MgdMix {
#include "../../../../assets/shaders/kernels/SelfTestMix.kernel.hlsl"
}
namespace mye::kernel::cpu::MgdAll {
#include "../../../../assets/shaders/kernels/SelfTestAllFuncs.kernel.hlsl"
}

namespace mye {
namespace {

using namespace compute;
namespace fs = std::filesystem;
namespace sx = kernel::cpu::MgdSaxpy;
namespace mx = kernel::cpu::MgdMix;
namespace ax = kernel::cpu::MgdAll;

int g_fails = 0;

void Check(bool cond, const char* msg)
{
    if (cond) {
        MYE_LOG_INFO("  PASS: %s", msg);
    } else {
        MYE_LOG_ERROR("  FAIL: %s", msg);
        ++g_fails;
    }
}

constexpr uint32_t kSaxpyN = 100000;
constexpr uint32_t kMixN = 20000;
constexpr uint32_t kAllN = 20000;
constexpr uint32_t kRepeat = 10;       // 同じ ForEach を繰り返す回数 (tiered JIT の切り替わりをまたぐため)
constexpr int kRepeatSleepMs = 90;     // 繰り返しの間隔。TC_CallCountingDelayMs (既定 100 ms) に届かせる
constexpr uint32_t kPerfN = 1000000;
constexpr const char* kProbe = "CsComputeProbe";

// C# 側のスクリプト。入力は C# が決定的に作って C# のストアに置き、出力のハッシュと、バッファのハンドルを public フィールドに出す。
// ハンドルを受け取ったテストが C# のストアから入力と出力を読み、C++ の CPU 版で同じ入力を計算して比べる
constexpr const char* kProbeSource = R"CS(
using System;
using System.Diagnostics;
using MyeScripting;
using MyeScripting.KernelCompat;
using MyeKernels.SelfTestSaxpy;
using MyeKernels.SelfTestMix;
using MyeKernels.SelfTestAllFuncs;
using MyeKernels.SelfTestUnused;

[ComputeKernels("SelfTestSaxpy", "SelfTestMix", "SelfTestAllFuncs")]
public unsafe class CsComputeProbe : MyeScript
{
    const uint kSaxpyN = 100000, kMixN = 20000, kAllN = 20000;

    public ulong hSaxpyY, hSaxpyYIn, hSaxpyX, hMixOut, hMixProbes, hAllOut, hAllSrc;
    public ulong hashSaxpy, hashMix, hashAll;
    public ulong startJob, startUndeclaredJob, updateUndeclaredJob, wrongCaseJob, jobSaxpy, jobMix, jobAll;
    public uint startUndeclaredTouched, updateUndeclaredTouched, runs, sugarOk, perfN;
    public float perfSaxpyMs, perfMixMs;

    float[] _yInit, _x;
    SelfTestMixProbe[] _probes;
    float4[] _src;
    Buffer<float> _y, _yIn, _xb;
    Buffer<SelfTestMixElem> _mixOut;
    Buffer<SelfTestMixProbe> _mixProbes;
    Buffer<SelfTestAllFuncsElem> _allOut;
    Buffer<float4> _allSrc;

    static uint Next(ref uint s) { s = s * 1664525u + 1013904223u; return s; }
    static float Rand(ref uint s, float lo, float hi) { return lo + (Next(ref s) >> 8) * (1.0f / 16777216.0f) * (hi - lo); }

    static ulong Fnv<T>(T[] a) where T : unmanaged
    {
        ulong h = 14695981039346656037UL;
        fixed (T* p = a)
        {
            byte* b = (byte*)p;
            int n = a.Length * sizeof(T);
            for (int i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211UL; }
        }
        return h;
    }

    public override void Start()
    {
        uint s = 12345u;
        _yInit = new float[kSaxpyN];
        _x = new float[kSaxpyN];
        for (int i = 0; i < kSaxpyN; i++) { _yInit[i] = Rand(ref s, -50f, 50f); _x[i] = Rand(ref s, -50f, 50f); }
        _probes = new SelfTestMixProbe[kMixN];
        for (int i = 0; i < kMixN; i++)
        {
            _probes[i].dir = new float3(Rand(ref s, -1f, 1f), Rand(ref s, -1f, 1f), Rand(ref s, -1f, 1f));
            _probes[i].weight = Rand(ref s, 0f, 2f);
        }
        _src = new float4[kAllN];
        for (int i = 0; i < kAllN; i++)
        {
            _src[i] = new float4(Rand(ref s, -20f, 20f), Rand(ref s, -20f, 20f), Rand(ref s, -20f, 20f), Rand(ref s, -20f, 20f));
        }

        _y = Compute.CreateBuffer<float>(kSaxpyN, BufferKind.State);
        _yIn = Compute.CreateBuffer<float>(kSaxpyN, BufferKind.State);
        _xb = Compute.CreateBuffer<float>(kSaxpyN, BufferKind.State);
        _mixOut = Compute.CreateBuffer<SelfTestMixElem>(kMixN, BufferKind.State);
        _mixProbes = Compute.CreateBuffer<SelfTestMixProbe>(kMixN, BufferKind.State);
        _allOut = Compute.CreateBuffer<SelfTestAllFuncsElem>(kAllN, BufferKind.State);
        _allSrc = Compute.CreateBuffer<float4>(kAllN, BufferKind.State);
        _yIn.Write(_yInit);
        _xb.Write(_x);
        _mixProbes.Write(_probes);
        _allSrc.Write(_src);
        hSaxpyY = _y.Handle; hSaxpyYIn = _yIn.Handle; hSaxpyX = _xb.Handle;
        hMixOut = _mixOut.Handle; hMixProbes = _mixProbes.Handle;
        hAllOut = _allOut.Handle; hAllSrc = _allSrc.Handle;

        startJob = RunSaxpy().Handle;
        startUndeclaredJob = CallUndeclared(out startUndeclaredTouched);
    }

    Job RunSaxpy()
    {
        _y.Write(_yInit);
        return Compute.ForEach("SelfTestSaxpy", Execution.Auto, new SelfTestSaxpyParams { a = 1.7f }, _y, _xb);
    }

    // 宣言していないカーネル。何も実行されず、出力バッファは 0 のまま
    ulong CallUndeclared(out uint touched)
    {
        var o = Compute.CreateBuffer<float>(8, BufferKind.State);
        var a = Compute.CreateBuffer<float>(8, BufferKind.State);
        var b = Compute.CreateBuffer<float>(8, BufferKind.State);
        var c = Compute.CreateBuffer<float>(8, BufferKind.State);
        float[] ones = { 1, 1, 1, 1, 1, 1, 1, 1 };
        a.Write(ones); b.Write(ones); c.Write(ones);
        ulong job = Compute.ForEach("SelfTestUnused", Execution.Auto, new SelfTestUnusedParams { unused = 0f }, o, a, b, c).Handle;
        float[] got = new float[8];
        o.Read(got);
        touched = 0;
        for (int i = 0; i < 8; i++) { if (got[i] != 0f) touched = 1; }
        o.Release(); a.Release(); b.Release(); c.Release();
        return job;
    }

    public override void Update(float dt)
    {
        jobSaxpy = RunSaxpy().Handle;
        var outY = new float[kSaxpyN];
        _y.Read(outY);
        hashSaxpy = Fnv(outY);

        jobMix = Compute.ForEach("SelfTestMix", Execution.Auto,
            new SelfTestMixParams { time = 2.5f, axis = new float3(0.2f, 0.9f, -0.4f), seed = 0x1234ABCDu },
            _mixOut, _mixProbes).Handle;
        var outMix = new SelfTestMixElem[kMixN];
        _mixOut.Read(outMix);
        hashMix = Fnv(outMix);

        jobAll = Compute.ForEach("SelfTestAllFuncs", Execution.Auto,
            new SelfTestAllFuncsParams { time = 0.75f, axis = new float3(0.6f, -0.3f, 0.7f), seed = 777u, count = 5 },
            _allOut, _allSrc).Handle;
        var outAll = new SelfTestAllFuncsElem[kAllN];
        _allOut.Read(outAll);
        hashAll = Fnv(outAll);

        updateUndeclaredJob = CallUndeclared(out updateUndeclaredTouched);
        // 名前は大文字小文字を区別する
        wrongCaseJob = Compute.ForEach("selftestsaxpy", Execution.Auto, new SelfTestSaxpyParams { a = 1.7f }, _y, _xb).Handle;

        if (runs == 0) SugarCheck();
        if (perfN != 0) { Perf(perfN); perfN = 0; }
        runs++;
    }

    // 配列を直接渡す糖衣: y = a * x + y を C# の素の float 演算と比べる
    void SugarCheck()
    {
        var arr = (float[])_yInit.Clone();
        Job job = Compute.ForEach(arr, "SelfTestSaxpy", Execution.Auto, new SelfTestSaxpyParams { a = 1.7f }, _xb);
        sugarOk = job.Ok ? 1u : 0u;
        for (int i = 0; i < kSaxpyN; i++)
        {
            float expect = 1.7f * _x[i] + _yInit[i];
            if (BitConverter.SingleToUInt32Bits(arr[i]) != BitConverter.SingleToUInt32Bits(expect)) { sugarOk = 0; break; }
        }
    }

    void Perf(uint n)
    {
        var y = Compute.CreateBuffer<float>(n, BufferKind.State);
        var x = Compute.CreateBuffer<float>(n, BufferKind.State);
        var sw = Stopwatch.StartNew();
        Compute.ForEach("SelfTestSaxpy", Execution.Auto, new SelfTestSaxpyParams { a = 1.0f }, y, x);
        sw.Stop();
        perfSaxpyMs = (float)sw.Elapsed.TotalMilliseconds;
        y.Release(); x.Release();

        var o = Compute.CreateBuffer<SelfTestMixElem>(n, BufferKind.State);
        var p = Compute.CreateBuffer<SelfTestMixProbe>(n, BufferKind.State);
        sw.Restart();
        Compute.ForEach("SelfTestMix", Execution.Auto,
            new SelfTestMixParams { time = 1f, axis = new float3(0f, 1f, 0f), seed = 1u }, o, p);
        sw.Stop();
        perfMixMs = (float)sw.Elapsed.TotalMilliseconds;
        o.Release(); p.Release();
    }
}

// スクリプトではない呼び出し元。宣言のあるカーネルでも ForEach は失敗する (U11)
[BtTask]
public class CsComputeProbeTask : MyeBtTask
{
    public override MyeBtStatus OnTick()
    {
        var a = Compute.CreateBuffer<float>(4, BufferKind.State);
        var b = Compute.CreateBuffer<float>(4, BufferKind.State);
        MyeBtStatus r = MyeBtStatus.Failure;
        if (a.Valid && b.Valid)
        {
            Job job = Compute.ForEach("SelfTestSaxpy", Execution.Auto, new SelfTestSaxpyParams { a = 1.0f }, a, b);
            r = job.Ok ? MyeBtStatus.Failure : MyeBtStatus.Success;
        }
        a.Release(); b.Release();
        return r;
    }
}
)CS";

// RW を KernelId 以外で添字する C#。コンパイルエラーになるはず (CS1503)
constexpr const char* kBadIndexSource = R"CS(
using MyeScripting.KernelCompat;
public static unsafe class BadIndexProbe
{
    public static void Run()
    {
        float* p = stackalloc float[4];
        var v = new RWView<float>(p, 4);
        v[1] = 2.0f;
    }
}
)CS";

// 同じ書き方で KernelId なら通る (対照)
constexpr const char* kGoodIndexSource = R"CS(
using MyeScripting.KernelCompat;
public static unsafe class BadIndexProbe
{
    public static void Run()
    {
        float* p = stackalloc float[4];
        var v = new RWView<float>(p, 4);
        v[new KernelId(1)] = 2.0f;
    }
}
)CS";

fs::path KernelDir()
{
    return fs::path(FindEngineShaderDir()) / L"kernels";
}

std::string ReadFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void WriteFile(const fs::path& path, const std::string& text)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream(path, std::ios::binary) << text;
}

uint64_t Fnv1a(const void* data, size_t bytes)
{
    uint64_t h = 14695981039346656037ull;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < bytes; ++i) {
        h = (h ^ p[i]) * 1099511628211ull;
    }
    return h;
}

// ---- C# スクリプトのフィールドの読み書き (ManagedHost の Inspector 用 API を使う) ----

struct ProbeAccess {
    ManagedHost* host = nullptr;
    World* world = nullptr;
    EntityID entity;
    ComponentTypeId type = kInvalidComponentType;
};

int FieldIndex(const ProbeAccess& a, const char* name)
{
    const auto* fields = a.host->FieldsForComponent(a.type);
    if (fields == nullptr) {
        return -1;
    }
    for (size_t i = 0; i < fields->size(); ++i) {
        if ((*fields)[i].name == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int32_t InstanceHandle(const ProbeAccess& a)
{
    return a.host->EnsureInstance(a.type, a.entity, a.world->GetComponentRaw(a.entity, a.type));
}

// 値を 8 バイトの入れ物で受ける (uint / ulong のどちらのフィールドでも、下位から埋まる)
uint64_t ReadU64(const ProbeAccess& a, const char* name)
{
    const int idx = FieldIndex(a, name);
    uint64_t v = ~0ull;
    uint64_t buf = 0;
    if (idx >= 0 && a.host->GetFieldValue(InstanceHandle(a), idx, &buf, sizeof(buf))) {
        v = buf;
    } else {
        MYE_LOG_ERROR("  field read failed: %s", name);
    }
    return v;
}

float ReadF32(const ProbeAccess& a, const char* name)
{
    const int idx = FieldIndex(a, name);
    float v = -1.0f;
    if (idx < 0 || !a.host->GetFieldValue(InstanceHandle(a), idx, &v, sizeof(v))) {
        MYE_LOG_ERROR("  field read failed: %s", name);
    }
    return v;
}

bool WriteU32(const ProbeAccess& a, const char* name, uint32_t v)
{
    const int idx = FieldIndex(a, name);
    return idx >= 0 && a.host->SetFieldValue(InstanceHandle(a), idx, &v, sizeof(v));
}

void Tick(ManagedHost& host, uint64_t& tick, uint32_t n = 1)
{
    for (uint32_t i = 0; i < n; ++i) {
        host.SetTickContext(InputSnapshot{}, tick++, 1.0f / 60.0f, 1);
        host.RunStartAndUpdate();
    }
}

// ---- C++ の CPU 版の参照計算 ----

constexpr const char* const kSaxpyArgNames[] = { "y", "x" };
constexpr const char* const kMixArgNames[] = { "outv", "probes" };
constexpr const char* const kAllArgNames[] = { "outv", "src" };
constexpr auto kSaxpyArgs = kernel::MakeKernelArgs<&sx::SelfTestSaxpy_Cpu>(kSaxpyArgNames);
constexpr auto kMixArgs = kernel::MakeKernelArgs<&mx::SelfTestMix_Cpu>(kMixArgNames);
constexpr auto kAllArgs = kernel::MakeKernelArgs<&ax::SelfTestAllFuncs_Cpu>(kAllArgNames);

struct Reference {
    std::vector<float> saxpyY, saxpyX, saxpyOut;
    std::vector<mx::SelfTestMixProbe> probes;
    std::vector<mx::SelfTestMixElem> mixOut;
    std::vector<kernel::float4> src;
    std::vector<ax::SelfTestAllFuncsElem> allOut;
};

// 参照計算用のサービス。C++ レーンに 3 本の CPU 版だけを入れる
void SetupReference(ComputeService& svc, jobs::JobSystem& js)
{
    svc.Bind(nullptr, nullptr, &js);
    const std::string saxpyText = ReadFile(KernelDir() / L"SelfTestSaxpy.kernel.hlsl");
    const std::string mixText = ReadFile(KernelDir() / L"SelfTestMix.kernel.hlsl");
    const std::string allText = ReadFile(KernelDir() / L"SelfTestAllFuncs.kernel.hlsl");
    svc.ReplaceKernels(
        CallerKind::Cpp,
        { KernelDescFromAbi(kernel::MakeKernelDesc<&sx::SelfTestSaxpy_Cpu>("SelfTestSaxpy", kernel::HashKernelSource(saxpyText),
                                                                           kSaxpyArgs.data()), CallerKind::Cpp),
          KernelDescFromAbi(kernel::MakeKernelDesc<&mx::SelfTestMix_Cpu>("SelfTestMix", kernel::HashKernelSource(mixText),
                                                                         kMixArgs.data()), CallerKind::Cpp),
          KernelDescFromAbi(kernel::MakeKernelDesc<&ax::SelfTestAllFuncs_Cpu>("SelfTestAllFuncs", kernel::HashKernelSource(allText),
                                                                              kAllArgs.data()), CallerKind::Cpp) });
    svc.ReplaceDeclarations(CallerKind::Cpp, { { "RefScript", { "SelfTestSaxpy", "SelfTestMix", "SelfTestAllFuncs" } } });
    svc.UpdateKernelSource("SelfTestSaxpy.kernel.hlsl", saxpyText);
    svc.UpdateKernelSource("SelfTestMix.kernel.hlsl", mixText);
    svc.UpdateKernelSource("SelfTestAllFuncs.kernel.hlsl", allText);
}

template <typename T>
uint64_t MakeRefBuffer(ComputeService& ref, const std::vector<T>& init)
{
    const uint64_t h = ref.CreateBuffer(CallerKind::Cpp, static_cast<uint32_t>(init.size()), sizeof(T), BufferKind::State);
    ref.WriteBuffer(CallerKind::Cpp, h, init.data(), static_cast<uint32_t>(init.size()));
    return h;
}

uint64_t RefForEach(ComputeService& ref, const char* kernelName, const void* params, uint32_t paramsSize,
                    const uint64_t* buffers, uint32_t count)
{
    ForEachArgs args;
    args.caller.kind = CallerKind::Cpp;
    args.caller.scriptType = "RefScript";
    args.kernel = kernelName;
    args.execution = Execution::Auto;
    args.params = params;
    args.paramsSize = paramsSize;
    args.buffers = buffers;
    args.bufferCount = count;
    return ref.ForEach(args);
}

// 2 つのバイト列を比べ、最初に違う要素の位置をログに出す
bool BytesEqual(const void* a, const void* b, size_t elemBytes, size_t count, const char* label)
{
    const uint8_t* pa = static_cast<const uint8_t*>(a);
    const uint8_t* pb = static_cast<const uint8_t*>(b);
    for (size_t i = 0; i < count; ++i) {
        if (std::memcmp(pa + i * elemBytes, pb + i * elemBytes, elemBytes) != 0) {
            MYE_LOG_ERROR("  %s: 要素 %zu から違う", label, i);
            return false;
        }
    }
    return true;
}

// ---- 本体 ----

struct Run {
    HeadlessSim* sim = nullptr;
    ProbeAccess probe;
    uint64_t tick = 0;
    Reference ref;
    uint64_t refHashSaxpy = 0, refHashMix = 0, refHashAll = 0;
};

bool ReadStore(ComputeService& svc, uint64_t handle, void* dst, uint32_t count)
{
    return svc.ReadBuffer(CallerKind::CSharp, handle, dst, count);
}

// C# のストアの入力から C++ の CPU 版で出力を作り、C# の出力とバイト単位で比べる
void CompareWithCpp(Run& run, ComputeService& svc)
{
    Reference& r = run.ref;
    r.saxpyY.assign(kSaxpyN, 0.0f);
    r.saxpyX.assign(kSaxpyN, 0.0f);
    r.probes.assign(kMixN, {});
    r.src.assign(kAllN, {});
    bool readOk = ReadStore(svc, ReadU64(run.probe, "hSaxpyYIn"), r.saxpyY.data(), kSaxpyN)
        && ReadStore(svc, ReadU64(run.probe, "hSaxpyX"), r.saxpyX.data(), kSaxpyN)
        && ReadStore(svc, ReadU64(run.probe, "hMixProbes"), r.probes.data(), kMixN)
        && ReadStore(svc, ReadU64(run.probe, "hAllSrc"), r.src.data(), kAllN);
    Check(readOk, "C# のストアから入力を読み出せる");
    if (!readOk) {
        return;
    }

    jobs::JobSystem js;
    js.Init();
    {
        ComputeService ref;
        SetupReference(ref, js);

        const uint64_t y = MakeRefBuffer(ref, r.saxpyY);
        const uint64_t x = MakeRefBuffer(ref, r.saxpyX);
        sx::SelfTestSaxpyParams sp;
        sp.a = 1.7f;
        const uint64_t sb[] = { y, x };
        const uint64_t jobS = RefForEach(ref, "SelfTestSaxpy", &sp, sizeof(sp), sb, 2);
        r.saxpyOut.assign(kSaxpyN, 0.0f);
        ref.ReadBuffer(CallerKind::Cpp, y, r.saxpyOut.data(), kSaxpyN);

        const uint64_t mo = MakeRefBuffer(ref, std::vector<mx::SelfTestMixElem>(kMixN));
        const uint64_t mp = MakeRefBuffer(ref, r.probes);
        mx::SelfTestMixParams mpar;
        mpar.time = 2.5f;
        mpar.axis = kernel::float3(0.2f, 0.9f, -0.4f);
        mpar.seed = 0x1234ABCDu;
        const uint64_t mb[] = { mo, mp };
        const uint64_t jobM = RefForEach(ref, "SelfTestMix", &mpar, sizeof(mpar), mb, 2);
        r.mixOut.assign(kMixN, {});
        ref.ReadBuffer(CallerKind::Cpp, mo, r.mixOut.data(), kMixN);

        const uint64_t ao = MakeRefBuffer(ref, std::vector<ax::SelfTestAllFuncsElem>(kAllN));
        const uint64_t as = MakeRefBuffer(ref, r.src);
        ax::SelfTestAllFuncsParams apar;
        apar.time = 0.75f;
        apar.axis = kernel::float3(0.6f, -0.3f, 0.7f);
        apar.seed = 777u;
        apar.count = 5;
        const uint64_t ab[] = { ao, as };
        const uint64_t jobA = RefForEach(ref, "SelfTestAllFuncs", &apar, sizeof(apar), ab, 2);
        r.allOut.assign(kAllN, {});
        ref.ReadBuffer(CallerKind::Cpp, ao, r.allOut.data(), kAllN);

        Check(jobS == kJobDoneCpu && jobM == kJobDoneCpu && jobA == kJobDoneCpu, "C++ の CPU 版の参照計算が 3 本とも成功する");
    }
    js.Shutdown();

    run.refHashSaxpy = Fnv1a(r.saxpyOut.data(), r.saxpyOut.size() * sizeof(float));
    run.refHashMix = Fnv1a(r.mixOut.data(), r.mixOut.size() * sizeof(mx::SelfTestMixElem));
    run.refHashAll = Fnv1a(r.allOut.data(), r.allOut.size() * sizeof(ax::SelfTestAllFuncsElem));

    // C# の出力そのもの (最後の Update の結果) をバイト単位で比べる
    std::vector<float> csY(kSaxpyN);
    std::vector<mx::SelfTestMixElem> csMix(kMixN);
    std::vector<ax::SelfTestAllFuncsElem> csAll(kAllN);
    const bool outOk = ReadStore(svc, ReadU64(run.probe, "hSaxpyY"), csY.data(), kSaxpyN)
        && ReadStore(svc, ReadU64(run.probe, "hMixOut"), csMix.data(), kMixN)
        && ReadStore(svc, ReadU64(run.probe, "hAllOut"), csAll.data(), kAllN);
    Check(outOk, "C# のストアから出力を読み出せる");
    if (outOk) {
        Check(BytesEqual(csY.data(), r.saxpyOut.data(), sizeof(float), kSaxpyN, "Saxpy"),
              "Saxpy: C# の CPU 版と C++ の CPU 版の出力がバイト一致する");
        Check(BytesEqual(csMix.data(), r.mixOut.data(), sizeof(mx::SelfTestMixElem), kMixN, "Mix"),
              "Mix (sin/cos・while・RO の範囲外読み): C# の CPU 版と C++ の CPU 版の出力がバイト一致する");
        Check(BytesEqual(csAll.data(), r.allOut.data(), sizeof(ax::SelfTestAllFuncsElem), kAllN, "AllFuncs"),
              "AllFuncs (ホワイトリストの全関数): C# の CPU 版と C++ の CPU 版の出力がバイト一致する");
    }
}

void CheckHashesEveryRun(Run& run, uint32_t& mismatches)
{
    if (ReadU64(run.probe, "hashSaxpy") != run.refHashSaxpy || ReadU64(run.probe, "hashMix") != run.refHashMix
        || ReadU64(run.probe, "hashAll") != run.refHashAll) {
        ++mismatches;
    }
}

void TestManaged()
{
    const std::wstring exeDir = GetExecutableDir();
    if (!fs::exists(fs::path(exeDir) / L"MyeScripting.dll")) {
        MYE_LOG_WARN("  SKIP: MyeScripting.dll が実行ファイルの隣に無い (tools\\build_managed.bat を実行していない)。C# の検証は未実施");
        return;
    }

    // 一時プロジェクト: kernels (エンジンのコピー) と scripts
    const fs::path root = fs::temp_directory_path() / L"mye_cs_compute_selftest";
    std::error_code ec;
    fs::remove_all(root, ec);
    for (const fs::directory_entry& e : fs::directory_iterator(KernelDir(), ec)) {
        const std::string name = e.path().filename().string();
        if (name.size() > 12 && name.compare(name.size() - 12, 12, ".kernel.hlsl") == 0) {
            fs::create_directories(root / L"assets" / L"shaders" / L"kernels", ec);
            fs::copy_file(e.path(), root / L"assets" / L"shaders" / L"kernels" / e.path().filename(), ec);
        }
    }
    const fs::path scriptsDir = root / L"assets" / L"scripts";
    WriteFile(scriptsDir / L"CsComputeProbe.cs", kProbeSource);

    HeadlessSimSetup setup;
    setup.config.title = L"MyEngine ComputeManagedSelfTest";
    setup.scene.showcase = FindShowcase(L"--local-demo", /*editor=*/true);
    HeadlessSim sim;
    if (setup.scene.showcase == nullptr || !sim.Init(setup)) {
        Check(false, "ヘッドレス sim が起動する (リポジトリのルートから実行すること)");
        fs::remove_all(root, ec);
        return;
    }
    ComputeService& svc = sim.Compute();
    ManagedHost& host = sim.Managed();
    Scene* scene = sim.Refs().scene;
    ComputeConfig cfg = svc.Config();
    cfg.stateTotalBytes = 256ull * 1024 * 1024; // 1M 要素の計測用
    svc.SetConfig(cfg);

    if (!host.Init(exeDir, scene)) {
        MYE_LOG_WARN("  SKIP: CoreCLR を起動できなかった (.NET ランタイムが無い?)。C# の検証は未実施");
        fs::remove_all(root, ec);
        return;
    }
    {
        const char* tiered = std::getenv("DOTNET_TieredCompilation");
        const char* pgo = std::getenv("DOTNET_TieredPGO");
        MYE_LOG_INFO("  JIT 設定: DOTNET_TieredCompilation=%s DOTNET_TieredPGO=%s", tiered ? tiered : "(既定)", pgo ? pgo : "(既定)");
    }

    // GameLogic.dll のカーネル表 (C++ レーン) を空にする: C# の State の ForEach が GameLogic.dll に頼らないことを示す
    svc.ReplaceKernels(CallerKind::Cpp, {});
    svc.ReplaceDeclarations(CallerKind::Cpp, {});

    Check(host.CompileScripts((root / L"assets" / L"scripts").wstring()), "Kernels.gen.cs を生成して C# スクリプトをコンパイルできる");
    Check(fs::exists(scriptsDir / L"Generated" / L"Kernels.gen.cs"), "assets\\scripts\\Generated\\Kernels.gen.cs が作られる");

    // 登録された C# の CPU 版: ハッシュ・引数表・params の大きさ
    bool tableOk = true;
    for (const char* name : { "SelfTestSaxpy", "SelfTestMix", "SelfTestAllFuncs" }) {
        const ComputeKernelDesc* k = svc.FindKernel(name);
        const std::string text = ReadFile(KernelDir() / (std::string(name) + ".kernel.hlsl"));
        tableOk = tableOk && k != nullptr && k->csharp.range != nullptr && k->cpp.range == nullptr
            && k->csharp.sourceHash == kernel::HashKernelSource(text);
    }
    Check(tableOk, "C# の CPU 版が C# レーンの表に入る (C++ の CPU 版は無い・ディスク上のソースのハッシュと一致)");
    const ComputeKernelDesc* mixDesc = svc.FindKernel("SelfTestMix");
    const ComputeKernelDesc* allDesc = svc.FindKernel("SelfTestAllFuncs");
    Check(mixDesc != nullptr && mixDesc->paramsSize == sizeof(mx::SelfTestMixParams) && mixDesc->args.size() == 2
              && mixDesc->args[0].name == "outv" && mixDesc->args[0].writable && mixDesc->args[0].stride == sizeof(mx::SelfTestMixElem)
              && mixDesc->args[1].name == "probes" && !mixDesc->args[1].writable
              && mixDesc->args[1].stride == sizeof(mx::SelfTestMixProbe),
          "C# の引数表 (HLSL の変数名・RW/RO・stride・params の大きさ) が C++ の struct の大きさと一致する");
    Check(allDesc != nullptr && allDesc->paramsSize == sizeof(ax::SelfTestAllFuncsParams)
              && allDesc->args[0].stride == sizeof(ax::SelfTestAllFuncsElem) && allDesc->args[1].stride == sizeof(kernel::float4),
          "AllFuncs の C# と C++ の struct の大きさが一致する (int2 を含む)");
    Check(svc.IsDeclared(CallerKind::CSharp, kProbe, "SelfTestSaxpy") && svc.IsDeclared(CallerKind::CSharp, kProbe, "SelfTestAllFuncs")
              && !svc.IsDeclared(CallerKind::CSharp, kProbe, "SelfTestUnused") && !svc.IsDeclared(CallerKind::Cpp, kProbe, "SelfTestSaxpy"),
          "[ComputeKernels] の宣言が C# レーンに入る (宣言したものだけ)。C++ レーンの宣言表には入らない");

    // スクリプトを付けて走らせる
    World& world = scene->GetWorld();
    const ComponentTypeId probeType = ComponentRegistry::Get().FindByName(kProbe);
    Check(probeType != kInvalidComponentType, "C# スクリプト型がコンポーネントとして登録される");
    if (probeType == kInvalidComponentType) {
        host.Shutdown();
        fs::remove_all(root, ec);
        return;
    }
    GameObject go = scene->CreateGameObject("CsComputeProbeHost");
    world.AddComponentRaw(go.Id(), probeType);
    Run run;
    run.sim = &sim;
    run.probe = { &host, &world, go.Id(), probeType };

    // Start の中の ForEach と、1 回目の Update
    Tick(host, run.tick);
    Check(ReadU64(run.probe, "startJob") == kJobDoneCpu, "C#: Start の中の ForEach (宣言したカーネル) が C# の CPU 版で成功する");
    Check(ReadU64(run.probe, "startUndeclaredJob") == 0 && ReadU64(run.probe, "startUndeclaredTouched") == 0,
          "C#: Start の中の宣言していないカーネルは実行されずに失敗し、バッファは変わらない");
    Check(ReadU64(run.probe, "jobSaxpy") == kJobDoneCpu && ReadU64(run.probe, "jobMix") == kJobDoneCpu && ReadU64(run.probe, "jobAll") == kJobDoneCpu,
          "C#: Update の中の ForEach が 3 本とも成功する (GameLogic.dll にカーネルが無い状態)");
    Check(ReadU64(run.probe, "updateUndeclaredJob") == 0 && ReadU64(run.probe, "updateUndeclaredTouched") == 0
              && ReadU64(run.probe, "wrongCaseJob") == 0,
          "C#: Update の中の宣言なし・名前の大文字小文字違いは失敗し、バッファは変わらない");
    Check(ReadU64(run.probe, "sugarOk") == 1, "C#: 配列を直接渡す糖衣が成功し、結果が C# の素の演算と一致する");
    Check(svc.Store(CallerKind::CSharp).LiveCount() >= 7 && svc.Store(CallerKind::Cpp).LiveCount() == 0,
          "C# のバッファは C# 用のストアに置かれる (C++ 用のストアは空のまま)");

    CompareWithCpp(run, svc);

    // 繰り返し: 時間を空けて同じ ForEach を重ね、tiered JIT の切り替わりの前後で出力が変わらないことを確かめる
    uint32_t mismatches = 0;
    CheckHashesEveryRun(run, mismatches);
    for (uint32_t i = 0; i < kRepeat; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kRepeatSleepMs));
        Tick(host, run.tick);
        CheckHashesEveryRun(run, mismatches);
    }
    Check(mismatches == 0 && ReadU64(run.probe, "runs") == kRepeat + 1, "C# の出力ハッシュが、繰り返し (tiered JIT の切り替わり前後) で毎回 C++ の CPU 版と一致する");

    // BT タスク (スクリプトではない呼び出し元): 宣言のあるカーネルでも失敗する (U11)
    const int32_t btStatus = host.RunTask(go.Id(), 0, "CsComputeProbeTask", 0, run.tick, "");
    Check(btStatus == 1, "C#: BT タスクの中の ForEach は、宣言のあるカーネルでも実行されずに失敗する (U11)");

    // 速度の計測 (ログのみ)
    {
        run.probe.host = &host;
        WriteU32(run.probe, "perfN", kPerfN);
        Tick(host, run.tick);
        const float csSaxpy = ReadF32(run.probe, "perfSaxpyMs");
        const float csMix = ReadF32(run.probe, "perfMixMs");

        jobs::JobSystem js;
        js.Init();
        double cppSaxpy = 0.0;
        double cppMix = 0.0;
        {
            ComputeService ref;
            SetupReference(ref, js);
            ComputeConfig rc = ref.Config();
            rc.stateTotalBytes = cfg.stateTotalBytes;
            ref.SetConfig(rc);
            using Clock = std::chrono::steady_clock;
            const uint64_t y = ref.CreateBuffer(CallerKind::Cpp, kPerfN, sizeof(float), BufferKind::State);
            const uint64_t x = ref.CreateBuffer(CallerKind::Cpp, kPerfN, sizeof(float), BufferKind::State);
            sx::SelfTestSaxpyParams sp;
            sp.a = 1.0f;
            const uint64_t sb[] = { y, x };
            Clock::time_point t0 = Clock::now();
            RefForEach(ref, "SelfTestSaxpy", &sp, sizeof(sp), sb, 2);
            cppSaxpy = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
            const uint64_t o = ref.CreateBuffer(CallerKind::Cpp, kPerfN, sizeof(mx::SelfTestMixElem), BufferKind::State);
            const uint64_t p = ref.CreateBuffer(CallerKind::Cpp, kPerfN, sizeof(mx::SelfTestMixProbe), BufferKind::State);
            mx::SelfTestMixParams mp;
            mp.time = 1.0f;
            mp.axis = kernel::float3(0.0f, 1.0f, 0.0f);
            mp.seed = 1u;
            const uint64_t mb[] = { o, p };
            t0 = Clock::now();
            RefForEach(ref, "SelfTestMix", &mp, sizeof(mp), mb, 2);
            cppMix = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        }
        js.Shutdown();
        MYE_LOG_INFO("  計測 (%u 要素、ログのみ): Saxpy C++ %.2f ms / C# %.2f ms、Mix C++ %.2f ms / C# %.2f ms", kPerfN, cppSaxpy,
                     csSaxpy, cppMix, csMix);
        Check(csSaxpy > 0.0f && csMix > 0.0f, "C# の 1M 要素の所要時間を計測できた");
    }

    // 再読込: 登録を外して付け直す。直後 (tick の前) から宣言表が揃い、新しい Start も同じ判定になる
    {
        const uint64_t gen0 = host.ReloadGeneration();
        const bool recompiled = host.CompileScripts((root / L"assets" / L"scripts").wstring());
        const ComputeKernelDesc* k = svc.FindKernel("SelfTestSaxpy");
        Check(recompiled && host.ReloadGeneration() == gen0 + 1 && k != nullptr && k->csharp.range != nullptr,
              "C# を再コンパイルすると、C# の CPU 版が付け直される");
        Check(svc.IsDeclared(CallerKind::CSharp, kProbe, "SelfTestSaxpy") && !svc.IsDeclared(CallerKind::CSharp, kProbe, "SelfTestUnused"),
              "再読込直後 (tick を回す前) から宣言表が揃っている");
        Tick(host, run.tick);
        Check(ReadU64(run.probe, "startJob") == kJobDoneCpu && ReadU64(run.probe, "startUndeclaredJob") == 0
                  && ReadU64(run.probe, "updateUndeclaredJob") == 0 && ReadU64(run.probe, "jobAll") == kJobDoneCpu,
              "再読込直後の最初の tick: 新しい Start も Update も、同じ判定で成功 / 失敗する");
        Check(ReadU64(run.probe, "hashSaxpy") == run.refHashSaxpy && ReadU64(run.probe, "hashMix") == run.refHashMix
                  && ReadU64(run.probe, "hashAll") == run.refHashAll,
              "再読込後の出力も C++ の CPU 版と一致する");
    }

    // .kernel.hlsl を編集した: 再コンパイルするまでは版ずれとして State の ForEach が失敗し、再生成 + 再コンパイルで一致に戻る
    {
        const fs::path saxpyPath = root / L"assets" / L"shaders" / L"kernels" / L"SelfTestSaxpy.kernel.hlsl";
        const std::string original = ReadFile(saxpyPath);
        const std::string edited = original + "\n// edited\n";
        svc.UpdateKernelSource("SelfTestSaxpy.kernel.hlsl", edited);
        Tick(host, run.tick);
        Check(ReadU64(run.probe, "jobSaxpy") == 0 && ReadU64(run.probe, "jobMix") == kJobDoneCpu,
              "ソースを編集すると、再コンパイルするまで C# の版ずれで Saxpy の ForEach だけが失敗する");
        WriteFile(saxpyPath, edited);
        Check(host.CompileScripts((root / L"assets" / L"scripts").wstring()), "編集後のカーネルで C# を再生成して再コンパイルできる");
        const ComputeKernelDesc* k = svc.FindKernel("SelfTestSaxpy");
        Check(k != nullptr && k->csharp.sourceHash == kernel::HashKernelSource(edited), "再生成した C# の CPU 版が新しいハッシュで登録される");
        Tick(host, run.tick);
        Check(ReadU64(run.probe, "jobSaxpy") == kJobDoneCpu && ReadU64(run.probe, "hashSaxpy") == run.refHashSaxpy,
              "再コンパイル後は一致に戻り、ForEach が成功する (コメントだけの編集なので結果は同じ)");
    }

    // FileWatcher → ReloadHub の経路 (M91h): ファイルを書き換えるだけで、ソース表の更新と C# の再生成・再コンパイルが自動で走る
    {
        const fs::path kernelsDir = root / L"assets" / L"shaders" / L"kernels";
        const fs::path saxpyPath = kernelsDir / L"SelfTestSaxpy.kernel.hlsl";
        svc.ScanKernelSources(kernelsDir.wstring()); // このテストの一時プロジェクトを kernelDir にする
        ShaderManager shaders; // Init しない (デバイス無し)。索引だけ使う
        shaders.SetAssetsRoot((root / L"assets").wstring());
        shaders.RebuildProjectShaderIndex();
        ReloadHub hub;
        const bool watching = hub.Init(&shaders, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, (root / L"assets").wstring());
        hub.SetKernelHooks(&svc, &host);
        Check(watching, "ReloadHub が一時プロジェクトの assets を監視できる");
        const std::string edited2 = ReadFile(saxpyPath) + "\n// edited by the watcher\n";
        const uint64_t gen0 = host.ReloadGeneration();
        WriteFile(saxpyPath, edited2);
        std::this_thread::sleep_for(std::chrono::milliseconds(600)); // FileWatcher のデバウンス (150 ms) を越える
        hub.Update();
        const ComputeKernelDesc* k = svc.FindKernel("SelfTestSaxpy");
        const KernelSourceInfo* src = svc.FindKernelSource("SelfTestSaxpy");
        Check(src != nullptr && src->hash == kernel::HashKernelSource(edited2),
              "FileWatcher が .kernel.hlsl の変更を拾い、ソース表のハッシュが新しくなる");
        Check(host.ReloadGeneration() == gen0 + 1 && k != nullptr && k->csharp.sourceHash == kernel::HashKernelSource(edited2),
              "C# が自動で再生成・再コンパイルされ、C# の CPU 版が新しいハッシュで一致に戻る");
        Tick(host, run.tick);
        Check(ReadU64(run.probe, "jobSaxpy") == kJobDoneCpu && ReadU64(run.probe, "hashSaxpy") == run.refHashSaxpy,
              "自動で再コンパイルされた後は ForEach が成功する");
        hub.Update(); // 変更が無ければ再コンパイルしない
        Check(host.ReloadGeneration() == gen0 + 1, "もう一度 Update しても (変更が無ければ) 再コンパイルしない");
        hub.Shutdown();
    }

    // RW を KernelId 以外で添字する C# はコンパイルエラーになる。失敗しても前の登録は残る
    {
        WriteFile(scriptsDir / L"BadIndexProbe.cs", kBadIndexSource);
        const bool bad = host.CompileScripts((root / L"assets" / L"scripts").wstring());
        const ComputeKernelDesc* k = svc.FindKernel("SelfTestSaxpy");
        Check(!bad && k != nullptr && k->csharp.range != nullptr, "RW を int で添字する C# はコンパイルエラーになる (前の登録は残る)");
        WriteFile(scriptsDir / L"BadIndexProbe.cs", kGoodIndexSource);
        Check(host.CompileScripts((root / L"assets" / L"scripts").wstring()), "同じ書き方で KernelId を使えばコンパイルできる (対照)");
    }

    // 後始末: ランタイムを閉じる前に C# の CPU 版を表から外す
    host.Shutdown();
    Check(svc.FindKernel("SelfTestSaxpy") == nullptr || svc.FindKernel("SelfTestSaxpy")->csharp.range == nullptr,
          "ManagedHost::Shutdown が C# の CPU 版を表から外す");
    fs::remove_all(root, ec);
}

} // namespace

bool RunComputeManagedSelfTest()
{
    MYE_LOG_INFO("==== Compute C# (M91g) self test ====");
    g_fails = 0;
    TestManaged();
    MYE_LOG_INFO("==== Compute C# self test: %s ====", g_fails == 0 ? "PASS" : "FAIL");
    return g_fails == 0;
}

} // namespace mye
