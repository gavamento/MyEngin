//====================================================================================
//                          ComputeServiceSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                       10/12/2026
//                                          Compute.ForEach のエンジン核 (ストア・判断・Fx GPU) の回帰テスト
//====================================================================================
#include "Engine/Engine/Compute/ComputeServiceSelfTest.h"

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Jobs/JobSystem.h"
#include "Engine/Core/Util/Hash.h"
#include "Engine/Engine/Compute/ComputeBufferStore.h"
#include "Engine/Engine/Compute/ComputeDecision.h"
#include "Engine/Engine/Compute/ComputeService.h"
#include "Engine/Engine/Compute/KernelHash.h"
#include "Engine/Engine/Compute/KernelRange.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/Device/GraphicsDevice.h"
#include "Engine/Renderer/Shader/ShaderManager.h"
#include "Shared/KernelCompat.h"

// KernelSelfTest.cpp と同じカーネルを取り込む。名前空間を変えて、そちらの <Name>_Cpu と重複定義にしない
namespace mye::kernel::cpu::ServiceSaxpy {
#include "../../../../assets/shaders/kernels/SelfTestSaxpy.kernel.hlsl"
}
namespace mye::kernel::cpu::ServiceMix {
#include "../../../../assets/shaders/kernels/SelfTestMix.kernel.hlsl"
}
namespace mye::kernel::cpu::ServiceUnused {
#include "../../../../assets/shaders/kernels/SelfTestUnused.kernel.hlsl"
}

namespace mye {
namespace {

using namespace compute;
namespace fs = std::filesystem;
namespace sx = kernel::cpu::ServiceSaxpy;
namespace mx = kernel::cpu::ServiceMix;
namespace ux = kernel::cpu::ServiceUnused;

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

constexpr uint32_t kCount = 10007;
constexpr sx::SelfTestSaxpyParams kParams = { 1.7f };
constexpr const char* kScript = "TestScript";
constexpr const char* kCsScript = "TestCsScript";
constexpr const char* kSaxpy = "SelfTestSaxpy";
constexpr const char* kMix = "SelfTestMix";
constexpr const char* kUnused = "SelfTestUnused";

float InputValue(uint32_t i, uint32_t salt)
{
    return kernel::HashToUnitFloat(kernel::HashU32(i * 2654435761u + salt)) * 2.0f - 1.0f;
}

struct SaxpyData {
    std::vector<float> x;
    std::vector<float> y0;
};

SaxpyData MakeData(uint32_t n)
{
    SaxpyData d;
    d.x.resize(n);
    d.y0.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        d.x[i] = InputValue(i, 1u) * 100.0f;
        d.y0[i] = InputValue(i, 2u) * 10.0f;
    }
    return d;
}

// 参照: 型つきの C++ 版を直接回す (ComputeService を通さない)
std::vector<float> Reference(jobs::JobSystem& js, const SaxpyData& d, int times)
{
    std::vector<float> y = d.y0;
    for (int t = 0; t < times; ++t) {
        kernel::RunKernelCpu(js, &sx::SelfTestSaxpy_Cpu, kParams, kernel::RunOptions{},
                             kernel::RWView<float>(y.data(), static_cast<uint32_t>(y.size())),
                             kernel::ROView<float>(d.x.data(), static_cast<uint32_t>(d.x.size())));
    }
    return y;
}

int64_t UlpDistance(float a, float b)
{
    auto key = [](float f) -> int64_t {
        int32_t i;
        std::memcpy(&i, &f, sizeof(i));
        return i < 0 ? static_cast<int64_t>(INT32_MIN) - i : i;
    };
    const int64_t d = key(a) - key(b);
    return d < 0 ? -d : d;
}

int64_t MaxUlp(const std::vector<float>& a, const std::vector<float>& b)
{
    int64_t m = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        m = std::max(m, UlpDistance(a[i], b[i]));
    }
    return m;
}

std::string ReadKernelFile(const wchar_t* leaf)
{
    const std::wstring dir = FindEngineShaderDir();
    std::ifstream in(fs::path(dir) / L"kernels" / leaf, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Saxpy / Mix を登録し、宣言を入れる。Saxpy は C++ 版と C# 版 (同じレンジ関数で代用)、Mix は C++ 版だけ
struct Fixture {
    jobs::JobSystem js;
    ComputeService svc;
    std::string saxpyText;
    std::string mixText;
    uint64_t saxpyHash = 0;
    uint64_t mixHash = 0;
    uint64_t unusedHash = 0;

    void Setup(GraphicsDevice* device, ShaderManager* shaders)
    {
        js.Init();
        svc.Bind(device, shaders, &js);
        saxpyText = ReadKernelFile(L"SelfTestSaxpy.kernel.hlsl");
        mixText = ReadKernelFile(L"SelfTestMix.kernel.hlsl");
        saxpyHash = kernel::HashKernelSource(saxpyText);
        mixHash = kernel::HashKernelSource(mixText);
        ComputeKernelDesc saxpy;
        saxpy.name = kSaxpy;
        saxpy.paramsSize = sizeof(sx::SelfTestSaxpyParams);
        saxpy.args = { { "y", true, sizeof(float) }, { "x", false, sizeof(float) } };
        saxpy.cpp = { kernel::MakeRangeFn<&sx::SelfTestSaxpy_Cpu>(), saxpyHash };
        saxpy.csharp = { kernel::MakeRangeFn<&sx::SelfTestSaxpy_Cpu>(), saxpyHash };
        svc.RegisterKernel(saxpy);
        ComputeKernelDesc mix;
        mix.name = kMix;
        mix.paramsSize = sizeof(mx::SelfTestMixParams);
        mix.args = { { "outv", true, sizeof(mx::SelfTestMixElem) }, { "probes", false, sizeof(mx::SelfTestMixProbe) } };
        mix.cpp = { kernel::MakeRangeFn<&mx::SelfTestMix_Cpu>(), mixHash };
        svc.RegisterKernel(mix);
        const std::string unusedText = ReadKernelFile(L"SelfTestUnused.kernel.hlsl");
        unusedHash = kernel::HashKernelSource(unusedText);
        ComputeKernelDesc unused;
        unused.name = kUnused;
        unused.paramsSize = sizeof(ux::SelfTestUnusedParams);
        unused.args = { { "outv", true, sizeof(float) }, { "a", false, sizeof(float) },
                        { "skipped", false, sizeof(float) }, { "c", false, sizeof(float) } };
        unused.cpp = { kernel::MakeRangeFn<&ux::SelfTestUnused_Cpu>(), unusedHash };
        svc.RegisterKernel(unused);
        svc.UpdateKernelSource("SelfTestUnused.kernel.hlsl", unusedText);
        svc.UpdateKernelSource("SelfTestSaxpy.kernel.hlsl", saxpyText);
        svc.UpdateKernelSource("SelfTestMix.kernel.hlsl", mixText);
        svc.ReplaceDeclarations({ { kScript, { kSaxpy, kMix, kUnused } }, { kCsScript, { kSaxpy, kMix } },
                                  { "OnlyMix", { kMix } } });
        ComputeConfig cfg;
        cfg.gpuMinElements = 64;
        svc.SetConfig(cfg);
    }
    ~Fixture() { js.Shutdown(); }

    ForEachArgs Args(const char* script, CallerKind kind, const char* kernelName, Execution ex,
                     const uint64_t* bufs, uint32_t n) const
    {
        ForEachArgs a;
        a.caller = { kind, script };
        a.kernel = kernelName;
        a.execution = ex;
        a.params = &kParams;
        a.paramsSize = sizeof(kParams);
        a.buffers = bufs;
        a.bufferCount = n;
        return a;
    }

    // y (RW) と x (RO) を作って中身を書く
    void MakeSaxpyBuffers(CallerKind lane, BufferKind yKind, BufferKind xKind, const SaxpyData& d, uint64_t out[2])
    {
        out[0] = svc.CreateBuffer(lane, static_cast<uint32_t>(d.y0.size()), sizeof(float), yKind);
        out[1] = svc.CreateBuffer(lane, static_cast<uint32_t>(d.x.size()), sizeof(float), xKind);
        svc.WriteBuffer(lane, out[0], d.y0.data(), static_cast<uint32_t>(d.y0.size()));
        svc.WriteBuffer(lane, out[1], d.x.data(), static_cast<uint32_t>(d.x.size()));
    }

    std::vector<float> ReadFloats(CallerKind lane, uint64_t h, uint32_t n, bool wait = false)
    {
        std::vector<float> v(n);
        svc.ReadBuffer(lane, h, v.data(), n, wait);
        return v;
    }
};

// ---------------------------------------------------------------------------
// バッファストア
// ---------------------------------------------------------------------------

void TestStore()
{
    MYE_LOG_INFO("-- バッファストア --");
    ComputeBufferStore a(StoreLane::Game);
    ComputeBufferStore b(StoreLane::Script);
    const uint64_t a1 = a.Create(10, 4, BufferKind::State);
    const uint64_t a2 = a.Create(10, 4, BufferKind::Fx);
    const uint64_t a3 = a.Create(10, 8, BufferKind::State);
    Check(a1 != 0 && a2 != 0 && a3 != 0 && a1 != a2 && a2 != a3, "3 本作れて、ハンドルは全部違う");
    Check(a.SlotIndex(a1) == 0 && a.SlotIndex(a2) == 1 && a.SlotIndex(a3) == 2, "最小の空きスロットから順に採番");

    // 同じ操作列から同じハンドル (決定的な採番)
    ComputeBufferStore a2nd(StoreLane::Game);
    const uint64_t r1 = a2nd.Create(10, 4, BufferKind::State);
    const uint64_t r2 = a2nd.Create(10, 4, BufferKind::Fx);
    const uint64_t r3 = a2nd.Create(10, 8, BufferKind::State);
    Check(r1 == a1 && r2 == a2 && r3 == a3, "同じ操作列から同じハンドルが出る");

    Check(a.Release(a2), "解放できる");
    Check(!a.Release(a2), "二重解放は false で何も起きない");
    Check(!a.IsValid(a2), "解放後のハンドルは無効 (世代が古い)");
    const uint64_t a4 = a.Create(5, 4, BufferKind::State);
    Check(a.SlotIndex(a4) == 1 && a4 != a2, "空いたスロットを最小から再利用し、世代は進む");
    Check(!a.IsValid(a2) && a.IsValid(a4), "古いハンドルは再利用スロットを指さない");

    Check(!b.IsValid(a1) && !b.IsValid(a4), "別レーンのストアは他方のハンドルを受け付けない");
    const uint64_t b1 = b.Create(10, 4, BufferKind::State);
    Check(b1 != 0 && b1 != a1 && !a.IsValid(b1), "同じ操作でもレーンごとにハンドルが違う");
    Check(!a.IsValid((static_cast<uint64_t>(1) << 32) | 0u) && !a.IsValid((static_cast<uint64_t>(7) << 32) | 1u),
          "v21 形式 (小さい世代) のハンドルは無効");

    // 上限・無効入力
    Check(a.Create(0, 4, BufferKind::State) == 0, "count 0 は 0");
    Check(a.Create(10, 0, BufferKind::State) == 0, "stride 0 は 0");
    Check(a.Create(10, 6, BufferKind::State) == 0, "stride が 4 の倍数でないと 0");
    Check(a.Create(a.Limits().maxElements + 1, 4, BufferKind::Fx) == 0, "要素数の上限超過は 0");
    Check(a.Create(a.Limits().maxElements, 4, BufferKind::Fx) != 0, "要素数の上限ちょうどは作れる");
    ComputeStoreLimits tinyLimits;
    tinyLimits.stateTotalBytes = 100;
    tinyLimits.maxSlots = 3;
    ComputeBufferStore c(StoreLane::Game, tinyLimits);
    const uint64_t c1 = c.Create(20, 4, BufferKind::State); // 80 バイト
    Check(c1 != 0 && c.Create(10, 4, BufferKind::State) == 0, "State の総バイト枠を超えると 0");
    Check(c.Create(100, 4, BufferKind::Fx) != 0, "Fx は State の総バイト枠に数えない");
    c.Release(c1);
    Check(c.Create(25, 4, BufferKind::State) != 0, "解放した分の枠は戻る");
    c.Create(1, 4, BufferKind::Fx);
    Check(c.Create(1, 4, BufferKind::Fx) == 0, "スロット数の上限で 0");

    // Write / Read
    std::vector<uint8_t> src(40), dst(40, 0xEE);
    for (size_t i = 0; i < src.size(); ++i) {
        src[i] = static_cast<uint8_t>(i);
    }
    Check(a.Write(a1, src.data(), 10) && a.Read(a1, dst.data(), 10) && src == dst, "書いた内容が読める");
    Check(!a.Write(a1, src.data(), 11) && !a.Read(a1, dst.data(), 11), "要素数が大きすぎる Write / Read は false");
    Check(!a.Write(0, src.data(), 1) && !a.Read(0xFFFFFFFF00000000ull, dst.data(), 1) && !a.Write(a1, nullptr, 1),
          "無効ハンドル・null は false で落ちない");
    a.ReleaseAll();
    Check(a.LiveCount() == 0 && a.StateBytes() == 0, "ReleaseAll で全部戻る");
    const uint64_t again = a.Create(10, 4, BufferKind::State);
    Check(a.SlotIndex(again) == 0 && again == a1, "全解放後は採番も初期状態に戻る (新規起動と同じハンドルから始まる)");
}

// ---------------------------------------------------------------------------
// 決定表
// ---------------------------------------------------------------------------

DecisionInput Base()
{
    DecisionInput in;
    in.callerIsScript = true;
    in.declared = true;
    in.lint = LintState::Ok;
    in.cpuVersion = true;
    in.hashMatch = true;
    in.hasState = false;
    in.elementCount = 10000;
    in.execution = Execution::Auto;
    in.deviceAvailable = true;
    in.gpuMinElements = 4096;
    in.gpuBudgetOk = true;
    return in;
}

void Expect(const char* name, const DecisionInput& in, DecisionKind kind, FailReason reason = FailReason::None,
            DecisionNote note = DecisionNote::None)
{
    const Decision d = DecideExecution(in);
    const bool ok = d.kind == kind && d.reason == reason && d.note == note;
    if (!ok) {
        MYE_LOG_ERROR("    got kind=%d reason=%s note=%d", static_cast<int>(d.kind), FailReasonName(d.reason),
                      static_cast<int>(d.note));
    }
    Check(ok, name);
}

void TestDecisionTable()
{
    MYE_LOG_INFO("-- 決定表 (spec 4.1.4) --");
    using K = DecisionKind;
    using F = FailReason;
    using N = DecisionNote;
    DecisionInput in;

    // State を含む行
    in = Base(); in.hasState = true;
    Expect("State + Auto + 合格 → CPU", in, K::Cpu);
    in.execution = Execution::Cpu;
    Expect("State + Cpu + 合格 → CPU", in, K::Cpu);
    in.execution = Execution::Gpu;
    Expect("State + Gpu → 拒否", in, K::Failed, F::StateGpuRejected);
    in = Base(); in.hasState = true; in.lint = LintState::Violation;
    Expect("State + 検査違反 → 失敗", in, K::Failed, F::LintViolation);
    in = Base(); in.hasState = true; in.cpuVersion = false; in.hashMatch = false;
    Expect("State + CPU 版なし → 失敗", in, K::Failed, F::NoCpuVersion);
    in = Base(); in.hasState = true; in.hashMatch = false;
    Expect("State + ハッシュ不一致 → 失敗", in, K::Failed, F::HashMismatch);
    in = Base(); in.hasState = true; in.resim = true;
    Expect("State は resim でも走る", in, K::Cpu);
    in = Base(); in.hasState = true; in.forceCpu = true; in.deviceAvailable = false;
    Expect("State + force-cpu / デバイスなし → CPU", in, K::Cpu);
    in = Base(); in.hasState = true; in.overrideCpu = true;
    Expect("State + 上書き CPU → CPU", in, K::Cpu);
    in = Base(); in.hasState = true; in.writesStateReadsFx = true;
    Expect("State に書くカーネルが Fx を読む → 失敗", in, K::Failed, F::MixedKinds);

    // 全部 Fx + Auto
    in = Base();
    Expect("Fx + Auto + 合格 + 十分な要素数 → GPU", in, K::Gpu);
    in.elementCount = 4095;
    Expect("Fx + Auto + 要素数が下限未満 → CPU", in, K::Cpu);
    in.elementCount = 4096;
    Expect("Fx + Auto + 要素数が下限ちょうど → GPU", in, K::Gpu);
    in = Base(); in.gpuBudgetOk = false;
    Expect("Fx + Auto + tick の枠を超える → CPU", in, K::Cpu);
    in = Base(); in.deviceAvailable = false;
    Expect("Fx + Auto + デバイスなし → CPU", in, K::Cpu);
    in = Base(); in.hashMatch = false;
    Expect("Fx + Auto + ハッシュ不一致 + デバイスあり → GPU 固定 + WARN", in, K::Gpu, F::None, N::HashMismatchGpu);
    in.deviceAvailable = false;
    Expect("Fx + Auto + ハッシュ不一致 + デバイスなし → 失敗", in, K::Failed, F::HashMismatch);
    in = Base(); in.cpuVersion = false; in.hashMatch = false; in.elementCount = 1;
    Expect("Fx + Auto + CPU 版なし + デバイスあり → GPU (下限は CPU 版があるときだけ効く)", in, K::Gpu);
    in.deviceAvailable = false;
    Expect("Fx + Auto + CPU 版なし + デバイスなし → 失敗", in, K::Failed, F::NoCpuVersion);
    in = Base(); in.lint = LintState::Violation;
    Expect("Fx + 検査違反 → 失敗", in, K::Failed, F::LintViolation);
    in = Base(); in.lint = LintState::Unknown;
    Expect("Fx + カーネル不明 → 失敗", in, K::Failed, F::UnknownKernel);

    // 全部 Fx + Cpu
    in = Base(); in.execution = Execution::Cpu;
    Expect("Fx + Cpu + 合格 → CPU", in, K::Cpu);
    in.hashMatch = false;
    Expect("Fx + Cpu + ハッシュ不一致 → 失敗", in, K::Failed, F::HashMismatch);
    in.cpuVersion = false;
    Expect("Fx + Cpu + CPU 版なし → 失敗", in, K::Failed, F::NoCpuVersion);

    // 全部 Fx + Gpu
    in = Base(); in.execution = Execution::Gpu; in.elementCount = 1;
    Expect("Fx + Gpu + デバイスあり → GPU (下限・枠は Auto だけ)", in, K::Gpu);
    in.gpuBudgetOk = false;
    Expect("Fx + Gpu + 枠超過でも明示指定は GPU", in, K::Gpu);
    in = Base(); in.execution = Execution::Gpu; in.deviceAvailable = false;
    Expect("Fx + Gpu + デバイスなし → CPU で代替 + WARN", in, K::Cpu, F::None, N::SubstitutedByCpu);
    in.cpuVersion = false;
    Expect("Fx + Gpu + デバイスなし + CPU 版なし → 失敗", in, K::Failed, F::NoDevice);
    in = Base(); in.execution = Execution::Gpu; in.hashMatch = false;
    Expect("Fx + Gpu + ハッシュ不一致 + デバイスあり → GPU", in, K::Gpu);

    // resim・force-cpu・上書き
    in = Base(); in.resim = true;
    Expect("Fx + resim → Skipped", in, K::Skipped);
    in.execution = Execution::Gpu;
    Expect("Fx + Gpu + resim → Skipped", in, K::Skipped);
    in = Base(); in.forceCpu = true;
    Expect("force-cpu → Fx も CPU", in, K::Cpu);
    in.execution = Execution::Gpu;
    Expect("force-cpu は Gpu 指定にも勝つ (WARN なし)", in, K::Cpu);
    in.cpuVersion = false;
    Expect("force-cpu + CPU 版なし → 失敗", in, K::Failed, F::NoCpuVersion);
    in = Base(); in.overrideCpu = true;
    Expect("上書き CPU 固定は Auto を Cpu として扱う", in, K::Cpu);
    in.execution = Execution::Gpu;
    Expect("上書き CPU 固定は明示の Gpu を変えない", in, K::Gpu);

    // 宣言 (他のすべてより先)
    in = Base(); in.declared = false;
    Expect("宣言なし → 失敗", in, K::Failed, F::NotDeclared);
    in = Base(); in.callerIsScript = false;
    Expect("呼び出し元がスクリプトではない → 失敗", in, K::Failed, F::NotDeclared);
    in = Base(); in.declared = false; in.hasState = true; in.execution = Execution::Gpu;
    Expect("宣言なし + State + Gpu → StateGpuRejected ではなく NotDeclared", in, K::Failed, F::NotDeclared);
    in = Base(); in.declared = false; in.lint = LintState::Violation; in.resim = true; in.hashMatch = false;
    Expect("宣言なし + 検査違反 + resim + 不一致 → NotDeclared", in, K::Failed, F::NotDeclared);
    in = Base(); in.callerIsScript = false; in.hasState = true; in.cpuVersion = false;
    Expect("スクリプト外 + State + CPU 版なし → NotDeclared", in, K::Failed, F::NotDeclared);
    in = Base(); in.declared = false; in.deviceAvailable = false; in.forceCpu = true;
    Expect("宣言なし + force-cpu → 失敗 (CPU でも走らせない)", in, K::Failed, F::NotDeclared);
}

// ---------------------------------------------------------------------------
// ComputeService (CPU)
// ---------------------------------------------------------------------------

void TestServiceCpu()
{
    MYE_LOG_INFO("-- ComputeService: CPU 実行・呼び出し元・失敗時の不変 --");
    Fixture fx;
    fx.Setup(nullptr, nullptr);
    ComputeService& svc = fx.svc;
    const SaxpyData d = MakeData(kCount);
    const std::vector<float> ref = Reference(fx.js, d, 1);

    // State + Auto → CPU。結果は型つきの直接実行と同じ
    uint64_t bufs[2];
    fx.MakeSaxpyBuffers(CallerKind::Cpp, BufferKind::State, BufferKind::State, d, bufs);
    Check(bufs[0] != 0 && bufs[1] != 0, "State バッファを作れた");
    uint64_t job = svc.ForEach(fx.Args(kScript, CallerKind::Cpp, kSaxpy, Execution::Auto, bufs, 2));
    Check(job == kJobDoneCpu && svc.Status(job) == JobStatus::Done && svc.Target(job) == JobTarget::Cpu,
          "State + Auto は CPU で同期完了 (Done / Cpu)");
    Check(fx.ReadFloats(CallerKind::Cpp, bufs[0], kCount) == ref, "結果が型つきの C++ 版の直接実行とビット一致");

    // jobs の有無で結果が同じ
    for (const bool enabled : { false, true }) {
        fx.js.SetEnabled(enabled);
        svc.WriteBuffer(CallerKind::Cpp, bufs[0], d.y0.data(), kCount);
        svc.ForEach(fx.Args(kScript, CallerKind::Cpp, kSaxpy, Execution::Auto, bufs, 2));
        Check(fx.ReadFloats(CallerKind::Cpp, bufs[0], kCount) == ref,
              enabled ? "jobs 有効でも同じ結果" : "jobs 無効でも同じ結果");
    }
    fx.js.SetEnabled(true);

    // 負荷枠 cpuMaxChunks でも同じ結果
    {
        ComputeConfig cfg = svc.Config();
        cfg.cpuMaxChunks = 2;
        svc.SetConfig(cfg);
        svc.WriteBuffer(CallerKind::Cpp, bufs[0], d.y0.data(), kCount);
        svc.ForEach(fx.Args(kScript, CallerKind::Cpp, kSaxpy, Execution::Auto, bufs, 2));
        Check(fx.ReadFloats(CallerKind::Cpp, bufs[0], kCount) == ref, "cpuMaxChunks=2 でも同じ結果");
        cfg.cpuMaxChunks = 0;
        svc.SetConfig(cfg);
    }

    // 宣言なし: 失敗 + バッファ不変 (Cpu / Auto / Gpu のどれでも、State + Gpu でも NotDeclared)
    svc.WriteBuffer(CallerKind::Cpp, bufs[0], d.y0.data(), kCount);
    bool allZero = true;
    bool unchanged = true;
    for (const Execution ex : { Execution::Auto, Execution::Cpu, Execution::Gpu }) {
        const uint64_t j = svc.ForEach(fx.Args("Stranger", CallerKind::Cpp, kSaxpy, ex, bufs, 2));
        allZero = allZero && j == kJobFailed && svc.Status(j) == JobStatus::Failed
            && svc.Target(j) == JobTarget::None;
        unchanged = unchanged && fx.ReadFloats(CallerKind::Cpp, bufs[0], kCount) == d.y0;
    }
    Check(allZero, "宣言していないスクリプト型は 0 (Failed / None)");
    Check(unchanged, "宣言なしで失敗したとき、バッファの中身は変わらない");
    const KernelObservation* stranger = svc.FindObservation("Stranger", kSaxpy);
    Check(stranger && stranger->lastFail == FailReason::NotDeclared && stranger->failures == 3,
          "宣言なし + State + Gpu でも理由は NotDeclared (StateGpuRejected ではない)");
    Check(svc.ForEach(fx.Args("OnlyMix", CallerKind::Cpp, kSaxpy, Execution::Auto, bufs, 2)) == kJobFailed
              && fx.ReadFloats(CallerKind::Cpp, bufs[0], kCount) == d.y0,
          "別のカーネルだけ宣言したスクリプトも失敗 (宣言は (型, カーネル) の組)");
    Check(svc.ForEach(fx.Args("", CallerKind::Cpp, kSaxpy, Execution::Auto, bufs, 2)) == kJobFailed
              && fx.ReadFloats(CallerKind::Cpp, bufs[0], kCount) == d.y0,
          "スクリプトではない呼び出し元 (型名なし) は失敗");
    Check(svc.ForEach(fx.Args("saxpytest", CallerKind::Cpp, "selftestsaxpy", Execution::Auto, bufs, 2)) == kJobFailed,
          "名前は大文字小文字を区別する");

    // State + Gpu
    Check(svc.ForEach(fx.Args(kScript, CallerKind::Cpp, kSaxpy, Execution::Gpu, bufs, 2)) == kJobFailed
              && fx.ReadFloats(CallerKind::Cpp, bufs[0], kCount) == d.y0,
          "State + Gpu は拒否され、バッファは変わらない");
    const KernelObservation* o = svc.FindObservation(kScript, kSaxpy);
    Check(o && o->lastFail == FailReason::StateGpuRejected, "理由は StateGpuRejected");

    // ジョブの中からの呼び出し
    {
        uint64_t inner = 12345;
        fx.js.ParallelRanges(64, 1, [&](size_t begin, size_t) {
            if (begin == 0) {
                inner = svc.ForEach(fx.Args(kScript, CallerKind::Cpp, kSaxpy, Execution::Auto, bufs, 2));
            }
        });
        Check(inner == kJobFailed && fx.ReadFloats(CallerKind::Cpp, bufs[0], kCount) == d.y0,
              "ジョブの中からの ForEach は 0 で、バッファは変わらない");
        const KernelObservation* oi = svc.FindObservation(kScript, kSaxpy);
        Check(oi && oi->lastFail == FailReason::InJob, "理由は InJob");
        Check(svc.ForEach(fx.Args(kScript, CallerKind::Cpp, kSaxpy, Execution::Auto, bufs, 2)) == kJobDoneCpu,
              "ジョブの外では成功に戻る");
        svc.WriteBuffer(CallerKind::Cpp, bufs[0], d.y0.data(), kCount);
    }

    // 引数の不正
    {
        uint64_t one[1] = { bufs[0] };
        Check(svc.ForEach(fx.Args(kScript, CallerKind::Cpp, kSaxpy, Execution::Auto, one, 1)) == kJobFailed,
              "バッファ個数の不一致は失敗");
        uint64_t same[2] = { bufs[0], bufs[0] };
        Check(svc.ForEach(fx.Args(kScript, CallerKind::Cpp, kSaxpy, Execution::Auto, same, 2)) == kJobFailed,
              "同じバッファを RW と RO に渡すと失敗");
        const uint64_t wrongStride = svc.CreateBuffer(CallerKind::Cpp, 16, 8, BufferKind::State);
        uint64_t ws[2] = { wrongStride, bufs[1] };
        Check(svc.ForEach(fx.Args(kScript, CallerKind::Cpp, kSaxpy, Execution::Auto, ws, 2)) == kJobFailed,
              "stride がカーネルの struct と違うと失敗");
        uint64_t invalid[2] = { 0, bufs[1] };
        Check(svc.ForEach(fx.Args(kScript, CallerKind::Cpp, kSaxpy, Execution::Auto, invalid, 2)) == kJobFailed,
              "無効ハンドル (0) は失敗");
        invalid[0] = (static_cast<uint64_t>(1) << 32) | 0u;
        Check(svc.ForEach(fx.Args(kScript, CallerKind::Cpp, kSaxpy, Execution::Auto, invalid, 2)) == kJobFailed,
              "v21 形式のハンドルは失敗");
        ForEachArgs badParams = fx.Args(kScript, CallerKind::Cpp, kSaxpy, Execution::Auto, bufs, 2);
        badParams.paramsSize = 8;
        Check(svc.ForEach(badParams) == kJobFailed, "params の大きさが違うと失敗");
        badParams.paramsSize = 4;
        badParams.params = nullptr;
        Check(svc.ForEach(badParams) == kJobFailed, "params が null だと失敗");
        Check(svc.ForEach(fx.Args(kScript, CallerKind::Cpp, "NoSuchKernel", Execution::Auto, bufs, 2)) == kJobFailed,
              "宣言していても未登録のカーネルは失敗");
        Check(fx.ReadFloats(CallerKind::Cpp, bufs[0], kCount) == d.y0, "引数不正の失敗でバッファは変わらない");
        svc.ReleaseBuffer(CallerKind::Cpp, wrongStride);
    }

    // State に書くカーネルが Fx を読む
    {
        uint64_t mixed[2];
        fx.MakeSaxpyBuffers(CallerKind::Cpp, BufferKind::State, BufferKind::Fx, d, mixed);
        Check(svc.ForEach(fx.Args(kScript, CallerKind::Cpp, kSaxpy, Execution::Auto, mixed, 2)) == kJobFailed,
              "State RW + Fx RO は失敗 (GPU の結果が sim へ入る穴)");
        const KernelObservation* om = svc.FindObservation(kScript, kSaxpy);
        Check(om && om->lastFail == FailReason::MixedKinds, "理由は MixedKinds");
        uint64_t fxOut[2];
        fxOut[0] = svc.CreateBuffer(CallerKind::Cpp, kCount, sizeof(float), BufferKind::Fx);
        fxOut[1] = bufs[1]; // State を読んで Fx に書くのは許す
        svc.WriteBuffer(CallerKind::Cpp, fxOut[0], d.y0.data(), kCount);
        Check(svc.ForEach(fx.Args(kScript, CallerKind::Cpp, kSaxpy, Execution::Auto, fxOut, 2)) == kJobDoneCpu
                  && fx.ReadFloats(CallerKind::Cpp, fxOut[0], kCount) == ref,
              "Fx RW + State RO は許す (デバイスなし → CPU)");
    }

    // C# の呼び出し元: C# レーンのストア、C# 版
    {
        uint64_t cs[2];
        fx.MakeSaxpyBuffers(CallerKind::CSharp, BufferKind::State, BufferKind::State, d, cs);
        Check(cs[0] != 0 && cs[0] != bufs[0], "C# レーンのバッファはレーンが別 (ハンドルが違う)");
        uint64_t csJob = svc.ForEach(fx.Args(kCsScript, CallerKind::CSharp, kSaxpy, Execution::Auto, cs, 2));
        Check(csJob == kJobDoneCpu && fx.ReadFloats(CallerKind::CSharp, cs[0], kCount) == ref,
              "C# の呼び出し元は C# レーンのバッファに C# 版で実行できる");
        Check(svc.ForEach(fx.Args(kCsScript, CallerKind::CSharp, kSaxpy, Execution::Auto, bufs, 2)) == kJobFailed,
              "C# の呼び出し元に C++ レーンのハンドルを渡すと失敗");
        Check(svc.ForEach(fx.Args(kScript, CallerKind::Cpp, kSaxpy, Execution::Auto, cs, 2)) == kJobFailed,
              "C++ の呼び出し元に C# レーンのハンドルを渡すと失敗");
        // Mix は C++ 版だけ → C# の呼び出しは「CPU 版なし」
        const uint64_t mixOut = svc.CreateBuffer(CallerKind::CSharp, 64, sizeof(mx::SelfTestMixElem), BufferKind::State);
        const uint64_t mixIn = svc.CreateBuffer(CallerKind::CSharp, 64, sizeof(mx::SelfTestMixProbe), BufferKind::State);
        uint64_t mb[2] = { mixOut, mixIn };
        mx::SelfTestMixParams mp = {};
        ForEachArgs ma = fx.Args(kCsScript, CallerKind::CSharp, kMix, Execution::Auto, mb, 2);
        ma.params = &mp;
        ma.paramsSize = sizeof(mp);
        Check(svc.ForEach(ma) == kJobFailed, "C# 版の無いカーネルは C# の呼び出しで失敗 (C++ 版には落とさない)");
        const KernelObservation* omx = svc.FindObservation(kCsScript, kMix);
        Check(omx && omx->lastFail == FailReason::NoCpuVersion, "理由は NoCpuVersion");
        const uint64_t cppOut = svc.CreateBuffer(CallerKind::Cpp, 64, sizeof(mx::SelfTestMixElem), BufferKind::State);
        const uint64_t cppIn = svc.CreateBuffer(CallerKind::Cpp, 64, sizeof(mx::SelfTestMixProbe), BufferKind::State);
        uint64_t cb[2] = { cppOut, cppIn };
        ma = fx.Args(kScript, CallerKind::Cpp, kMix, Execution::Auto, cb, 2);
        ma.params = &mp;
        ma.paramsSize = sizeof(mp);
        Check(svc.ForEach(ma) == kJobDoneCpu, "同じカーネルを C++ の呼び出しで実行できる (stride 20 / 16)");
    }

    // ハッシュ不一致・検査違反・上書き
    {
        const KernelSourceInfo good = { LintState::Ok, fx.saxpyHash };
        svc.SetKernelSourceInfo(kSaxpy, { LintState::Ok, fx.saxpyHash ^ 1u });
        Check(svc.ForEach(fx.Args(kScript, CallerKind::Cpp, kSaxpy, Execution::Auto, bufs, 2)) == kJobFailed
                  && fx.ReadFloats(CallerKind::Cpp, bufs[0], kCount) == d.y0,
              "ハッシュ不一致の State は失敗し、バッファは変わらない");
        const KernelObservation* oh = svc.FindObservation(kScript, kSaxpy);
        Check(oh && oh->lastFail == FailReason::HashMismatch, "理由は HashMismatch");
        svc.SetKernelSourceInfo(kSaxpy, good);
        svc.SetKernelSourceInfo(kSaxpy, { LintState::Violation, fx.saxpyHash });
        Check(svc.ForEach(fx.Args(kScript, CallerKind::Cpp, kSaxpy, Execution::Auto, bufs, 2)) == kJobFailed,
              "検査違反のカーネルは実行されない");
        svc.SetKernelSourceInfo(kSaxpy, good);
        Check(svc.ForEach(fx.Args(kScript, CallerKind::Cpp, kSaxpy, Execution::Auto, bufs, 2)) == kJobDoneCpu,
              "戻すと実行できる");
        svc.WriteBuffer(CallerKind::Cpp, bufs[0], d.y0.data(), kCount);

        // 実ファイルの検査: 壊したソースを渡すと Violation になり、その名前のカーネルは実行されない
        svc.UpdateKernelSource("SelfTestSaxpy.kernel.hlsl", "KERNEL garbage {");
        Check(svc.ForEach(fx.Args(kScript, CallerKind::Cpp, kSaxpy, Execution::Auto, bufs, 2)) == kJobFailed,
              "UpdateKernelSource で壊れたソースを入れると失敗");
        svc.UpdateKernelSource("SelfTestSaxpy.kernel.hlsl", fx.saxpyText);
        Check(svc.ForEach(fx.Args(kScript, CallerKind::Cpp, kSaxpy, Execution::Auto, bufs, 2)) == kJobDoneCpu,
              "ソースを戻すと実行できる");
        svc.WriteBuffer(CallerKind::Cpp, bufs[0], d.y0.data(), kCount);
    }

    // resim
    {
        svc.BeginTick(true);
        Check(svc.ForEach(fx.Args(kScript, CallerKind::Cpp, kSaxpy, Execution::Auto, bufs, 2)) == kJobDoneCpu,
              "State は resim 中でも走る");
        svc.WriteBuffer(CallerKind::Cpp, bufs[0], d.y0.data(), kCount);
        uint64_t fxb[2];
        fx.MakeSaxpyBuffers(CallerKind::Cpp, BufferKind::Fx, BufferKind::Fx, d, fxb);
        const uint64_t sk = svc.ForEach(fx.Args(kScript, CallerKind::Cpp, kSaxpy, Execution::Auto, fxb, 2));
        Check(sk == kJobSkipped && svc.Status(sk) == JobStatus::Skipped && svc.Target(sk) == JobTarget::None,
              "Fx は resim 中は Skipped (Skipped / None)");
        Check(fx.ReadFloats(CallerKind::Cpp, fxb[0], kCount) == d.y0, "Skipped のとき Fx バッファは変わらない");
        svc.BeginTick(false);
        const uint64_t run = svc.ForEach(fx.Args(kScript, CallerKind::Cpp, kSaxpy, Execution::Auto, fxb, 2));
        Check(run == kJobDoneCpu && fx.ReadFloats(CallerKind::Cpp, fxb[0], kCount) == ref,
              "resim が終われば Fx も走る (デバイスなし → CPU 版)");
    }

    // Fx + デバイスなし: Gpu 指定は CPU で代替、force-cpu、上書き
    {
        uint64_t fxb[2];
        fx.MakeSaxpyBuffers(CallerKind::Cpp, BufferKind::Fx, BufferKind::Fx, d, fxb);
        const uint64_t sub = svc.ForEach(fx.Args(kScript, CallerKind::Cpp, kSaxpy, Execution::Gpu, fxb, 2));
        const KernelObservation* os = svc.FindObservation(kScript, kSaxpy);
        Check(sub == kJobDoneCpu && os && os->lastNote == DecisionNote::SubstitutedByCpu
                  && fx.ReadFloats(CallerKind::Cpp, fxb[0], kCount) == ref,
              "デバイスなしの Gpu 指定は CPU 版で代替される (代替の印が対応表に残る)");
        svc.WriteBuffer(CallerKind::Cpp, fxb[0], d.y0.data(), kCount);
        svc.SetKernelOverrideCpu(kSaxpy, true);
        Check(svc.ForEach(fx.Args(kScript, CallerKind::Cpp, kSaxpy, Execution::Auto, fxb, 2)) == kJobDoneCpu,
              "上書き CPU 固定で Auto が CPU になる");
        svc.SetKernelOverrideCpu(kSaxpy, false);
    }

    // 対応表
    {
        const KernelObservation* a = svc.FindObservation(kScript, kSaxpy);
        const KernelObservation* b = svc.FindObservation(kCsScript, kSaxpy);
        const KernelObservation* none = svc.FindObservation("Nobody", kSaxpy);
        Check(a && b && none == nullptr && a != b, "対応表は (スクリプト型, カーネル) ごとに別の行");
        Check(a && a->calls > 5 && a->failures > 0 && a->lastElements == kCount && a->lastTarget == JobTarget::Cpu
                  && a->lastKind == DecisionKind::Cpu,
              "行に呼び出し回数・失敗回数・要素数・最終の実行先が積まれる");
        Check(b && b->calls >= 1 && b->lastElements == kCount, "C# の行も積まれる");
        std::string prev;
        bool sorted = true;
        for (const auto& kv : svc.Observations()) {
            const std::string k = kv.first.scriptType + "/" + kv.first.kernel;
            sorted = sorted && (prev.empty() || prev < k);
            prev = k;
        }
        Check(sorted, "対応表は (スクリプト型, カーネル) の昇順で並ぶ");
    }

    // ジョブハンドル
    {
        Check(svc.Status(0) == JobStatus::Failed && svc.Status(7) == JobStatus::Failed
                  && svc.Status(kJobGpuBase) == JobStatus::Failed && svc.Status(kJobGpuBase + 999) == JobStatus::Failed
                  && svc.Status(~0ull) == JobStatus::Failed,
              "未知のジョブハンドルは Failed");
        Check(svc.Target(0) == JobTarget::None && svc.Target(7) == JobTarget::None && svc.Target(kJobGpuBase + 999) == JobTarget::None,
              "未知のジョブハンドルの Target は None");
        Check(svc.Wait(kJobDoneCpu) && svc.Wait(kJobSkipped) && !svc.Wait(0) && !svc.Wait(kJobGpuBase + 999),
              "Wait: 完了済みは true、失敗・未知は false");
    }
}

// ---------------------------------------------------------------------------
// Fx の GPU 経路 (WARP)
// ---------------------------------------------------------------------------

void WaitDone(ComputeService& svc, uint64_t job)
{
    svc.Wait(job);
}

void TestFxGpu()
{
    MYE_LOG_INFO("-- Fx の GPU 経路 (WARP): 遅延 Read・Wait・枠・デバイス消失 --");
    GraphicsDevice device;
    if (!device.Init(true)) {
        Check(false, "WARP device init");
        return;
    }
    const std::wstring shaderDir = FindEngineShaderDir();
    ShaderManager shaders;
    shaders.SetAssetsRoot(fs::path(shaderDir).parent_path().wstring());
    Check(shaders.Init(device, { shaderDir }), "shader manager init");
    shaders.RebuildProjectShaderIndex();

    Fixture fx;
    fx.Setup(&device, &shaders);
    ComputeService& svc = fx.svc;
    const SaxpyData d = MakeData(kCount);
    const std::vector<float> ref1 = Reference(fx.js, d, 1);
    const std::vector<float> ref2 = Reference(fx.js, d, 2);

    // --- 遅延 Read と Wait ---
    uint64_t b[2];
    fx.MakeSaxpyBuffers(CallerKind::Cpp, BufferKind::Fx, BufferKind::Fx, d, b);
    svc.BeginTick(false);
    const uint64_t job = svc.ForEach(fx.Args(kScript, CallerKind::Cpp, kSaxpy, Execution::Auto, b, 2));
    Check(job >= kJobGpuBase && svc.Target(job) == JobTarget::Gpu, "Fx + Auto + 十分な要素数は GPU のジョブ (Target = Gpu)");
    const JobStatus first = svc.Status(job);
    Check(first == JobStatus::Pending || first == JobStatus::Done, "ジョブは Pending か Done");
    const std::vector<float> early = fx.ReadFloats(CallerKind::Cpp, b[0], kCount);
    Check(early == d.y0 || MaxUlp(early, ref1) <= 1, "待たない Read は古い結果か完了済みの結果のどちらかで、途中の値は返らない");
    MYE_LOG_INFO("    待たない Read: %s (状態 %s)", early == d.y0 ? "まだ古い" : "もう新しい",
                 first == JobStatus::Pending ? "Pending" : "Done");
    Check(svc.Wait(job) && svc.Status(job) == JobStatus::Done, "Wait で Done になる");
    const std::vector<float> after = fx.ReadFloats(CallerKind::Cpp, b[0], kCount);
    Check(MaxUlp(after, ref1) <= 1, "Wait の後の Read は最新の結果 (GPU と CPU の差が 1 ULP 以内)");
    const KernelObservation* obs = svc.FindObservation(kScript, kSaxpy);
    Check(obs && obs->lastTarget == JobTarget::Gpu && obs->gpuDispatches == 1
              && obs->lastUploadBytes == 2ull * kCount * sizeof(float)
              && obs->lastReadbackBytes == static_cast<uint64_t>(kCount) * sizeof(float),
          "対応表: GPU の回数・upload バイト (y と x)・readback バイト (y)");

    // --- GPU 常駐: 2 回目は upload 不要、結果は 2 回ぶん ---
    svc.BeginTick(false);
    const uint64_t job2 = svc.ForEach(fx.Args(kScript, CallerKind::Cpp, kSaxpy, Execution::Auto, b, 2));
    Check(job2 > job, "ジョブ番号は単調増加");
    svc.Wait(job2);
    obs = svc.FindObservation(kScript, kSaxpy);
    Check(obs->lastUploadBytes == 0, "Write していなければ 2 回目は upload しない (GPU に常駐)");
    Check(MaxUlp(fx.ReadFloats(CallerKind::Cpp, b[0], kCount), ref2) <= 2, "2 回続けた結果が 2 回ぶん (差 2 ULP 以内)");

    // --- Read(wait=true) は明示ストールで最新 ---
    svc.BeginTick(false);
    svc.ForEach(fx.Args(kScript, CallerKind::Cpp, kSaxpy, Execution::Auto, b, 2));
    const std::vector<float> waited = fx.ReadFloats(CallerKind::Cpp, b[0], kCount, true);
    Check(MaxUlp(waited, Reference(fx.js, d, 3)) <= 3, "Read(wait=true) は未完了でも待って最新を返す");

    // --- CPU 実行の前に GPU の結果と揃える ---
    svc.Wait(svc.ForEach(fx.Args(kScript, CallerKind::Cpp, kSaxpy, Execution::Gpu, b, 2)));
    const uint64_t waitsBefore = svc.FindObservation(kScript, kSaxpy)->waitCount;
    svc.BeginTick(false);
    const uint64_t cpuJob = svc.ForEach(fx.Args(kScript, CallerKind::Cpp, kSaxpy, Execution::Cpu, b, 2));
    Check(cpuJob == kJobDoneCpu, "GPU の後に同じバッファを CPU で走らせられる");
    Check(svc.FindObservation(kScript, kSaxpy)->waitCount > waitsBefore, "GPU の結果が未反映なら CPU 実行の前に待った (waitCount)");
    Check(MaxUlp(fx.ReadFloats(CallerKind::Cpp, b[0], kCount), Reference(fx.js, d, 5)) <= 5,
          "CPU 実行は GPU の結果の続きから計算する (5 回ぶん)");

    // --- Write は GPU から戻る古い結果を捨てる ---
    {
        uint64_t w[2];
        fx.MakeSaxpyBuffers(CallerKind::Cpp, BufferKind::Fx, BufferKind::Fx, d, w);
        svc.BeginTick(false);
        const uint64_t j = svc.ForEach(fx.Args(kScript, CallerKind::Cpp, kSaxpy, Execution::Gpu, w, 2));
        std::vector<float> fresh(kCount, 3.5f);
        svc.WriteBuffer(CallerKind::Cpp, w[0], fresh.data(), kCount);
        svc.Wait(j);
        Check(fx.ReadFloats(CallerKind::Cpp, w[0], kCount, true) == fresh, "実行中に Write すると、後から届いた GPU の結果で上書きされない");
        svc.BeginTick(false);
        svc.Wait(svc.ForEach(fx.Args(kScript, CallerKind::Cpp, kSaxpy, Execution::Gpu, w, 2)));
        std::vector<float> expect(kCount);
        for (uint32_t i = 0; i < kCount; ++i) {
            expect[i] = kParams.a * d.x[i] + 3.5f;
        }
        Check(MaxUlp(fx.ReadFloats(CallerKind::Cpp, w[0], kCount, true), expect) <= 1, "Write したデータが次の GPU 実行に upload される");
    }

    // --- 負荷枠 ---
    {
        ComputeConfig cfg = svc.Config();
        cfg.fxDispatchPerTick = 1;
        svc.SetConfig(cfg);
        uint64_t q[2];
        fx.MakeSaxpyBuffers(CallerKind::Cpp, BufferKind::Fx, BufferKind::Fx, d, q);
        svc.BeginTick(false);
        const uint64_t g1 = svc.ForEach(fx.Args(kScript, CallerKind::Cpp, kSaxpy, Execution::Auto, q, 2));
        const uint64_t g2 = svc.ForEach(fx.Args(kScript, CallerKind::Cpp, kSaxpy, Execution::Auto, q, 2));
        Check(svc.Target(g1) == JobTarget::Gpu && g2 == kJobDoneCpu && svc.TickDispatches() == 1,
              "dispatch の枠 (1 回) を超えた Auto は CPU に落ちる");
        svc.Wait(g1);
        svc.BeginTick(false);
        Check(svc.TickDispatches() == 0 && svc.TickUploadBytes() == 0, "BeginTick で枠のカウンタが戻る");
        const uint64_t g3 = svc.ForEach(fx.Args(kScript, CallerKind::Cpp, kSaxpy, Execution::Auto, q, 2));
        Check(svc.Target(g3) == JobTarget::Gpu, "次の tick では GPU に戻る");
        svc.Wait(g3);
        cfg.fxDispatchPerTick = 64;
        cfg.fxUploadBytesPerTick = 1000;
        svc.SetConfig(cfg);
        uint64_t u[2];
        fx.MakeSaxpyBuffers(CallerKind::Cpp, BufferKind::Fx, BufferKind::Fx, d, u);
        svc.BeginTick(false);
        Check(svc.ForEach(fx.Args(kScript, CallerKind::Cpp, kSaxpy, Execution::Auto, u, 2)) == kJobDoneCpu,
              "upload の枠 (バイト) を超える Auto は CPU に落ちる");
        Check(svc.ForEach(fx.Args(kScript, CallerKind::Cpp, kSaxpy, Execution::Auto, q, 2)) >= kJobGpuBase,
              "upload が要らない (GPU 常駐の) 実行は枠が小さくても GPU");
        cfg.fxUploadBytesPerTick = 8388608;
        cfg.gpuMinElements = 100000;
        svc.SetConfig(cfg);
        svc.BeginTick(false);
        Check(svc.ForEach(fx.Args(kScript, CallerKind::Cpp, kSaxpy, Execution::Auto, u, 2)) == kJobDoneCpu,
              "要素数が gpuMinElements 未満の Auto は CPU");
        svc.WriteBuffer(CallerKind::Cpp, u[0], d.y0.data(), kCount);
        Check(svc.ForEach(fx.Args(kScript, CallerKind::Cpp, kSaxpy, Execution::Gpu, u, 2)) >= kJobGpuBase,
              "明示の Gpu は gpuMinElements を無視する");
        cfg.gpuMinElements = 64;
        svc.SetConfig(cfg);
    }

    // --- State はデバイスがあっても CPU、force-cpu は Fx も CPU ---
    {
        uint64_t s[2];
        fx.MakeSaxpyBuffers(CallerKind::Cpp, BufferKind::State, BufferKind::State, d, s);
        const uint64_t sj = svc.ForEach(fx.Args(kScript, CallerKind::Cpp, kSaxpy, Execution::Auto, s, 2));
        Check(sj == kJobDoneCpu && svc.Target(sj) == JobTarget::Cpu, "State はデバイスがあっても CPU");
        ComputeConfig cfg = svc.Config();
        cfg.forceCpu = true;
        svc.SetConfig(cfg);
        uint64_t f[2];
        fx.MakeSaxpyBuffers(CallerKind::Cpp, BufferKind::Fx, BufferKind::Fx, d, f);
        svc.BeginTick(false);
        const uint64_t fj = svc.ForEach(fx.Args(kScript, CallerKind::Cpp, kSaxpy, Execution::Auto, f, 2));
        Check(fj == kJobDoneCpu && fx.ReadFloats(CallerKind::Cpp, f[0], kCount) == ref1,
              "force-cpu のとき Fx も CPU で、結果は C++ 版とビット一致");
        cfg.forceCpu = false;
        svc.SetConfig(cfg);
    }

    // --- 極小バッファ (SRV+UAV の WARP 不具合の再発確認) ---
    {
        for (const uint32_t n : { 1u, 3u, 64u, 65u }) {
            const SaxpyData td = MakeData(n);
            uint64_t t[2];
            fx.MakeSaxpyBuffers(CallerKind::Cpp, BufferKind::Fx, BufferKind::Fx, td, t);
            svc.BeginTick(false);
            const uint64_t tj = svc.ForEach(fx.Args(kScript, CallerKind::Cpp, kSaxpy, Execution::Gpu, t, 2));
            svc.Wait(tj);
            const std::vector<float> got = fx.ReadFloats(CallerKind::Cpp, t[0], n, true);
            const std::vector<float> want = Reference(fx.js, td, 1);
            char msg[80];
            std::snprintf(msg, sizeof(msg), "極小バッファ (%u 要素) を GPU で走らせて結果が合う", n);
            Check(tj >= kJobGpuBase && MaxUlp(got, want) <= 1, msg);
        }
    }

    // --- 構造体 stride (Mix: 20 / 16 バイト) の GPU 実行 ---
    {
        constexpr uint32_t kMixCount = 4099;
        std::vector<mx::SelfTestMixProbe> probes(4000);
        for (uint32_t i = 0; i < probes.size(); ++i) {
            probes[i].dir = kernel::float3(InputValue(i, 3u), InputValue(i, 4u), InputValue(i, 5u));
            probes[i].weight = InputValue(i, 6u);
        }
        mx::SelfTestMixParams mp;
        mp.time = 2.0f;
        mp.axis = kernel::float3(0.3f, -0.5f, 0.8f);
        mp.seed = 12345u;
        const uint64_t out = svc.CreateBuffer(CallerKind::Cpp, kMixCount, sizeof(mx::SelfTestMixElem), BufferKind::Fx);
        const uint64_t in = svc.CreateBuffer(CallerKind::Cpp, 4000, sizeof(mx::SelfTestMixProbe), BufferKind::Fx);
        svc.WriteBuffer(CallerKind::Cpp, in, probes.data(), 4000);
        uint64_t mb[2] = { out, in };
        ForEachArgs ma = fx.Args(kScript, CallerKind::Cpp, kMix, Execution::Gpu, mb, 2);
        ma.params = &mp;
        ma.paramsSize = sizeof(mp);
        svc.BeginTick(false);
        const uint64_t mj = svc.ForEach(ma);
        svc.Wait(mj);
        std::vector<mx::SelfTestMixElem> gpu(kMixCount);
        svc.ReadBuffer(CallerKind::Cpp, out, gpu.data(), kMixCount, true);
        // CPU 版 (force-cpu 相当) と比べる
        const uint64_t out2 = svc.CreateBuffer(CallerKind::Cpp, kMixCount, sizeof(mx::SelfTestMixElem), BufferKind::Fx);
        uint64_t mb2[2] = { out2, in };
        ma.buffers = mb2;
        ComputeConfig cfg = svc.Config();
        cfg.forceCpu = true;
        svc.SetConfig(cfg);
        svc.ForEach(ma);
        cfg.forceCpu = false;
        svc.SetConfig(cfg);
        std::vector<mx::SelfTestMixElem> cpu(kMixCount);
        svc.ReadBuffer(CallerKind::Cpp, out2, cpu.data(), kMixCount, true);
        float maxAbs = 0.0f;
        size_t tagDiff = 0;
        for (uint32_t i = 0; i < kMixCount; ++i) {
            maxAbs = std::max({ maxAbs, std::fabs(cpu[i].pos.x - gpu[i].pos.x), std::fabs(cpu[i].pos.y - gpu[i].pos.y),
                                std::fabs(cpu[i].pos.z - gpu[i].pos.z), std::fabs(cpu[i].val - gpu[i].val) });
            tagDiff += cpu[i].tag != gpu[i].tag ? 1 : 0;
        }
        MYE_LOG_INFO("    Mix (ComputeService 経由) GPU/CPU: 最大絶対誤差 %g / tag 不一致 %zu", maxAbs, tagDiff);
        Check(mj >= kJobGpuBase && maxAbs <= 1e-4f && tagDiff == 0, "構造体 stride 20 / 16 の Mix が GPU で走り、CPU と許容差内で一致する");
    }

    // --- 未使用の引数 (params と途中の RO) があるカーネル: FXC が詰めても名前で束縛できる ---
    {
        ShaderProgram* up = shaders.Get(shaders.LoadCompute("SelfTestUnused.kernel"));
        Check(up && up->valid, "SelfTestUnused.kernel が FXC で通る");
        if (up && up->valid) {
            Microsoft::WRL::ComPtr<ID3D11ShaderReflection> refl;
            D3DReflect(up->csBytecode.data(), up->csBytecode.size(), IID_ID3D11ShaderReflection,
                       reinterpret_cast<void**>(refl.GetAddressOf()));
            D3D11_SHADER_DESC sd = {};
            refl->GetDesc(&sd);
            bool hasParams = false;
            bool hasSkipped = false;
            bool compactedC = false;
            for (UINT i = 0; i < sd.BoundResources; ++i) {
                D3D11_SHADER_INPUT_BIND_DESC bd = {};
                refl->GetResourceBindingDesc(i, &bd);
                MYE_LOG_INFO("    FXC リフレクション: %s type=%d bind=%u", bd.Name, static_cast<int>(bd.Type), bd.BindPoint);
                hasParams = hasParams || std::string(bd.Name) == "__kernel_params";
                hasSkipped = hasSkipped || std::string(bd.Name) == "skipped";
                compactedC = compactedC || (std::string(bd.Name) == "c" && bd.BindPoint != 3);
            }
            MYE_LOG_INFO("    FXC は未使用の資源: params を%s、skipped を%s。c の t レジスタは%s",
                         hasParams ? "残した" : "落とした", hasSkipped ? "残した" : "落とした",
                         compactedC ? "詰めた (宣言位置 t3 ではない)" : "宣言位置 (t3) のまま");
        }
        const uint32_t n = 1000;
        std::vector<float> ua(n), uc(n), zero(n, 0.0f);
        for (uint32_t i = 0; i < n; ++i) {
            ua[i] = InputValue(i, 7u);
            uc[i] = InputValue(i, 8u);
        }
        uint64_t u[4];
        u[0] = svc.CreateBuffer(CallerKind::Cpp, n, sizeof(float), BufferKind::Fx);
        u[1] = svc.CreateBuffer(CallerKind::Cpp, n, sizeof(float), BufferKind::Fx);
        u[2] = svc.CreateBuffer(CallerKind::Cpp, n, sizeof(float), BufferKind::Fx);
        u[3] = svc.CreateBuffer(CallerKind::Cpp, n, sizeof(float), BufferKind::Fx);
        svc.WriteBuffer(CallerKind::Cpp, u[1], ua.data(), n);
        svc.WriteBuffer(CallerKind::Cpp, u[2], zero.data(), n);
        svc.WriteBuffer(CallerKind::Cpp, u[3], uc.data(), n);
        ux::SelfTestUnusedParams up2 = { 0.0f };
        ForEachArgs ua2 = fx.Args(kScript, CallerKind::Cpp, kUnused, Execution::Gpu, u, 4);
        ua2.params = &up2;
        ua2.paramsSize = sizeof(up2);
        svc.BeginTick(false);
        const uint64_t gj = svc.ForEach(ua2);
        svc.Wait(gj);
        const std::vector<float> gpu = fx.ReadFloats(CallerKind::Cpp, u[0], n, true);
        std::vector<float> want(n);
        for (uint32_t i = 0; i < n; ++i) {
            want[i] = ua[i] + uc[i];
        }
        Check(gj >= kJobGpuBase && MaxUlp(gpu, want) <= 1, "params と途中の RO を使わないカーネルが GPU で走り、結果が CPU 式と一致する");
        ComputeConfig cfg = svc.Config();
        cfg.forceCpu = true;
        svc.SetConfig(cfg);
        svc.WriteBuffer(CallerKind::Cpp, u[0], zero.data(), n);
        Check(svc.ForEach(ua2) == kJobDoneCpu && fx.ReadFloats(CallerKind::Cpp, u[0], n) == want,
              "同じカーネルの CPU 版の結果は式とビット一致");
        cfg.forceCpu = false;
        svc.SetConfig(cfg);
    }

    // --- 決定的撮影・selftest 用: 実行の直後に待つ ---
    {
        ComputeConfig cfg = svc.Config();
        cfg.alwaysWaitGpu = true;
        svc.SetConfig(cfg);
        uint64_t aw[2];
        fx.MakeSaxpyBuffers(CallerKind::Cpp, BufferKind::Fx, BufferKind::Fx, d, aw);
        svc.BeginTick(false);
        const uint64_t aj = svc.ForEach(fx.Args(kScript, CallerKind::Cpp, kSaxpy, Execution::Gpu, aw, 2));
        Check(aj >= kJobGpuBase && svc.Status(aj) == JobStatus::Done
                  && MaxUlp(fx.ReadFloats(CallerKind::Cpp, aw[0], kCount), ref1) <= 1,
              "alwaysWaitGpu: ForEach の直後に Done で、待たない Read が最新を返す");
        cfg.alwaysWaitGpu = false;
        svc.SetConfig(cfg);
    }

    // --- デバイス消失 → 復旧 ---
    {
        uint64_t m[2];
        fx.MakeSaxpyBuffers(CallerKind::Cpp, BufferKind::Fx, BufferKind::Fx, d, m);
        svc.BeginTick(false);
        const uint64_t pre = svc.ForEach(fx.Args(kScript, CallerKind::Cpp, kSaxpy, Execution::Gpu, m, 2));
        svc.Wait(pre);
        const std::vector<float> synced = fx.ReadFloats(CallerKind::Cpp, m[0], kCount, true); // 消失前に ref1 まで反映
        Check(MaxUlp(synced, ref1) <= 1, "消失前: GPU の結果が CPU ミラーに載っている");
        svc.ReleaseGpu();
        svc.BeginTick(false);
        const uint64_t lostAuto = svc.ForEach(fx.Args(kScript, CallerKind::Cpp, kSaxpy, Execution::Auto, m, 2));
        Check(lostAuto == kJobDoneCpu, "消失中の Auto は CPU 版で走る (Fx は止まらない)");
        Check(MaxUlp(fx.ReadFloats(CallerKind::Cpp, m[0], kCount), Reference(fx.js, d, 2)) <= 2,
              "消失中の CPU 実行は CPU ミラーの続きから計算する");
        const uint64_t lostGpu = svc.ForEach(fx.Args(kScript, CallerKind::Cpp, kSaxpy, Execution::Gpu, m, 2));
        const KernelObservation* ol = svc.FindObservation(kScript, kSaxpy);
        Check(lostGpu == kJobDoneCpu && ol->lastNote == DecisionNote::SubstitutedByCpu, "消失中の Gpu 指定は CPU で代替");
        Check(svc.Status(pre) == JobStatus::Done, "消失前のジョブは Done のまま (Pending を残さない)");
        svc.RecreateGpu();
        svc.BeginTick(false);
        const uint64_t back = svc.ForEach(fx.Args(kScript, CallerKind::Cpp, kSaxpy, Execution::Auto, m, 2));
        Check(back >= kJobGpuBase, "復旧後は GPU に戻る");
        svc.Wait(back);
        Check(MaxUlp(fx.ReadFloats(CallerKind::Cpp, m[0], kCount, true), Reference(fx.js, d, 4)) <= 4,
              "復旧後の GPU 実行は CPU ミラーから作り直したバッファの続きから計算する");
        Check(svc.FindObservation(kScript, kSaxpy)->lastUploadBytes == 2ull * kCount * sizeof(float),
              "復旧後の最初の実行で CPU ミラーを upload し直した");
    }

    // --- 解放 ---
    {
        uint64_t r[2];
        fx.MakeSaxpyBuffers(CallerKind::Cpp, BufferKind::Fx, BufferKind::Fx, d, r);
        svc.BeginTick(false);
        const uint64_t rj = svc.ForEach(fx.Args(kScript, CallerKind::Cpp, kSaxpy, Execution::Gpu, r, 2));
        Check(svc.ReleaseBuffer(CallerKind::Cpp, r[0]) && svc.ReleaseBuffer(CallerKind::Cpp, r[1]), "GPU で使ったバッファを解放できる");
        WaitDone(svc, rj);
        std::vector<float> dummy(kCount);
        Check(!svc.ReadBuffer(CallerKind::Cpp, r[0], dummy.data(), kCount) && !svc.ReleaseBuffer(CallerKind::Cpp, r[0]),
              "解放後の Read / 二重解放は false で落ちない");
        Check(svc.ForEach(fx.Args(kScript, CallerKind::Cpp, kSaxpy, Execution::Auto, r, 2)) == kJobFailed, "解放済みハンドルでの ForEach は失敗");
    }
}

} // namespace

bool RunComputeServiceSelfTest()
{
    MYE_LOG_INFO("==== Compute service (M91c) self test ====");
    g_fails = 0;
    TestStore();
    TestDecisionTable();
    TestServiceCpu();
    TestFxGpu();
    MYE_LOG_INFO("==== Compute service self test: %s ====", g_fails == 0 ? "PASS" : "FAIL");
    return g_fails == 0;
}

} // namespace mye
