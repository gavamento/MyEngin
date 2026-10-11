//====================================================================================
//                          ComputeScriptSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                       10/12/2026
//                                          GameLogic のカーネル表・COMPUTE_KERNELS・ForEach の ABI の回帰テスト
//====================================================================================
#include "Engine/Engine/Compute/ComputeScriptSelfTest.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/ComponentRegistry.h"
#include "Engine/Core/Jobs/JobSystem.h"
#include "Engine/Core/Util/Hash.h"
#include "Engine/Engine/Compute/ComputeService.h"
#include "Engine/Engine/Compute/KernelHash.h"
#include "Engine/Engine/Compute/KernelRange.h"
#include "Engine/Engine/Demo/ShowcaseScenes.h"
#include "Engine/Engine/Loop/HeadlessSim.h"
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Engine/Script/EngineApiTable.h"
#include "Engine/Platform/Input.h"
#include "Engine/Platform/PathUtil.h"
#include "Shared/KernelCompat.h"
#include "Shared/KernelRangeFn.h"

// ComputeServiceSelfTest.cpp / ComputeStateSelfTest.cpp とは別の名前空間で同じカーネルを取り込む
namespace mye::kernel::cpu::AbiSaxpy {
#include "../../../../assets/shaders/kernels/SelfTestSaxpy.kernel.hlsl"
}

namespace mye {
namespace {

using namespace compute;
namespace fs = std::filesystem;
namespace sx = kernel::cpu::AbiSaxpy;

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

constexpr uint32_t kCount = 100;
constexpr const char* kSaxpy = "SelfTestSaxpy";
constexpr const char* kUnused = "SelfTestUnused";
constexpr const char* kAbiScript = "AbiScript";
constexpr const char* kCsScript = "CsScript";

std::wstring KernelDir()
{
    return (fs::path(FindEngineShaderDir()) / L"kernels").wstring();
}

std::string ReadKernelFile(const wchar_t* name)
{
    std::ifstream in(fs::path(KernelDir()) / name, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// SelfTestSaxpy.kernel.hlsl の ABI 上の表 1 行 (Kernels.gen.cpp と同じ作り方)
constexpr const char* const kSaxpyArgNames[] = { "y", "x" };
constexpr auto kSaxpyArgs = kernel::MakeKernelArgs<&sx::SelfTestSaxpy_Cpu>(kSaxpyArgNames);

MyeKernelDesc SaxpyAbiDesc(uint64_t hash)
{
    return kernel::MakeKernelDesc<&sx::SelfTestSaxpy_Cpu>(kSaxpy, hash, kSaxpyArgs.data());
}

std::vector<float> Floats(uint32_t n, float scale)
{
    std::vector<float> v(n);
    for (uint32_t i = 0; i < n; ++i) {
        v[i] = static_cast<float>(i) * scale;
    }
    return v;
}

// ---------------------------------------------------------------------------
// ABI の往復 (GameLogic.dll なし)
// ---------------------------------------------------------------------------

void TestAbiSlots()
{
    jobs::JobSystem js;
    js.Init();
    {
        ComputeService svc;
        svc.Bind(nullptr, nullptr, &js);
        const std::string text = ReadKernelFile(L"SelfTestSaxpy.kernel.hlsl");
        const uint64_t hash = kernel::HashKernelSource(text);
        const MyeKernelDesc abi = SaxpyAbiDesc(hash);

        Check(abi.paramsSize == sizeof(sx::SelfTestSaxpyParams) && abi.argCount == 2 && abi.args[0].writable == 1
                  && abi.args[1].writable == 0 && abi.args[0].stride == sizeof(float) && std::strcmp(abi.args[1].name, "x") == 0
                  && abi.range != nullptr && abi.sourceHash == hash,
              "MakeKernelDesc が引数の数・RW/RO・stride・HLSL の変数名・params の大きさを型から作る");

        svc.ReplaceKernels(CallerKind::Cpp, { KernelDescFromAbi(abi, CallerKind::Cpp) });
        svc.ReplaceDeclarations(CallerKind::Cpp, { { kAbiScript, { kSaxpy } } });
        svc.UpdateKernelSource("SelfTestSaxpy.kernel.hlsl", text);

        ScriptApiContext ctx;
        ctx.compute = &svc;
        ctx.computeLane = CallerKind::Cpp;
        ctx.runningScript = kAbiScript;
        MyeEngineApi api = {};
        BuildEngineApi(api, &ctx);

        const MyeEntityId owner = { 5, 7 };
        const uint64_t y = api.ComputeCreateBuffer(&ctx, owner, kCount, sizeof(float), 0);
        const uint64_t x = api.ComputeCreateBuffer(&ctx, owner, kCount, sizeof(float), 0);
        Check(y != 0 && x != 0 && y != x, "ComputeCreateBuffer がハンドルを返す");
        const ComputeBufferStore::Slot* slot = svc.Store(CallerKind::Cpp).Find(y);
        Check(slot != nullptr && slot->kind == BufferKind::State && slot->owner.index == 5 && slot->owner.generation == 7,
              "owner のエンティティと種別 (State) がストアへ渡る");
        Check(api.ComputeCreateBuffer(&ctx, owner, 0, sizeof(float), 0) == 0
                  && api.ComputeCreateBuffer(&ctx, owner, 4, 6, 0) == 0 && api.ComputeCreateBuffer(&ctx, owner, 4, 4, 2) == 0,
              "要素数 0 / stride が 4 の倍数でない / 未知の kind は 0");

        const std::vector<float> y0 = Floats(kCount, 0.5f);
        const std::vector<float> x0(kCount, 1.0f);
        Check(api.ComputeWriteBuffer(&ctx, y, y0.data(), kCount) == 1 && api.ComputeWriteBuffer(&ctx, x, x0.data(), kCount) == 1
                  && api.ComputeWriteBuffer(&ctx, y, y0.data(), kCount + 1) == 0 && api.ComputeWriteBuffer(&ctx, 0x1234, y0.data(), 1) == 0,
              "ComputeWriteBuffer は要素数の超過と無効なハンドルを拒む");

        const sx::SelfTestSaxpyParams params = { 2.0f };
        const uint64_t bufs[2] = { y, x };
        const uint64_t job = api.ComputeForEach(&ctx, kSaxpy, 0, &params, sizeof(params), bufs, 2, nullptr);
        Check(job == kJobDoneCpu && api.ComputeJobStatus(&ctx, job) == static_cast<int32_t>(JobStatus::Done)
                  && api.ComputeJobTarget(&ctx, job) == static_cast<int32_t>(JobTarget::Cpu) && api.ComputeJobWait(&ctx, job) == 1,
              "宣言したカーネルの ForEach が CPU で同期完了し、Status / Target / Wait が答える");
        std::vector<float> out(kCount, -1.0f);
        Check(api.ComputeReadBuffer(&ctx, y, out.data(), kCount, 0) == 1 && out[0] == 2.0f && out[kCount - 1] == 0.5f * (kCount - 1) + 2.0f,
              "結果が y = a * x + y になっている");
        Check(api.ComputeReadBuffer(&ctx, y, out.data(), kCount + 1, 0) == 0, "ComputeReadBuffer は要素数の超過を拒む");
        Check(api.ComputeJobStatus(&ctx, 0) == static_cast<int32_t>(JobStatus::Failed) && api.ComputeJobWait(&ctx, 0) == 0,
              "失敗のジョブ (0) は Failed で、Wait は 0");

        // 宣言していない / 呼び出し元がスクリプトではない / 名前違い: 実行されずに 0、バッファは変わらない
        const std::vector<float> before = out;
        ctx.runningScript = "OtherScript";
        const uint64_t notDeclared = api.ComputeForEach(&ctx, kSaxpy, 0, &params, sizeof(params), bufs, 2, nullptr);
        ctx.runningScript = nullptr;
        const uint64_t outsideScript = api.ComputeForEach(&ctx, kSaxpy, 0, &params, sizeof(params), bufs, 2, nullptr);
        ctx.runningScript = kAbiScript;
        const uint64_t wrongCase = api.ComputeForEach(&ctx, "selftestsaxpy", 0, &params, sizeof(params), bufs, 2, nullptr);
        const uint64_t forged = [&] {
            ctx.runningScript = nullptr;
            const uint64_t j = api.ComputeForEach(&ctx, kSaxpy, 0, &params, sizeof(params), bufs, 2, kAbiScript);
            ctx.runningScript = kAbiScript;
            return j;
        }();
        std::vector<float> after(kCount, -1.0f);
        api.ComputeReadBuffer(&ctx, y, after.data(), kCount, 0);
        Check(notDeclared == 0 && outsideScript == 0 && wrongCase == 0 && after == before,
              "宣言のないスクリプト / スクリプトの外 (BT タスクなど) / 大文字小文字違いは、実行されずに 0 を返す");
        Check(forged == 0, "C++ レーンは引数の callerScript を信用しない (エンジンが実行中のスクリプトだけを見る)");
        Check(api.ComputeForEach(&ctx, nullptr, 0, &params, sizeof(params), bufs, 2, nullptr) == 0
                  && api.ComputeForEach(&ctx, kSaxpy, 9, &params, sizeof(params), bufs, 2, nullptr) == 0,
              "kernel が null / 未知の execution は 0");
        const FailReason notDeclaredWhy = svc.FindObservation("OtherScript", kSaxpy) != nullptr
            ? svc.FindObservation("OtherScript", kSaxpy)->lastFail
            : FailReason::None;
        Check(notDeclaredWhy == FailReason::NotDeclared, "観測の対応表に「宣言なしで失敗」が残る");

        // v21 と別のストア: ハンドルを混ぜると失敗
        Check(api.ComputeReleaseBuffer(&ctx, 0xFFFF) == 0 && api.ComputeReleaseBuffer(&ctx, x) == 1 && api.ComputeReleaseBuffer(&ctx, x) == 0,
              "ComputeReleaseBuffer: 無効なハンドルと二重解放は 0");

        // C# レーン: 登録の口は C# だけが使え、呼び出し元の型名は引数で渡る。C++ のハンドルは通らない
        ScriptApiContext csCtx;
        csCtx.compute = &svc;
        csCtx.computeLane = CallerKind::CSharp;
        MyeEngineApi csApi = {};
        BuildEngineApi(csApi, &csCtx);
        const MyeKernelDesc csAbi = SaxpyAbiDesc(hash);
        const char* const csNames[] = { kSaxpy };
        const MyeComputeDecl csDecl = { kCsScript, 1, csNames };
        Check(api.ComputeRegisterManagedKernels(&ctx, &csAbi, 1) == 0 && api.ComputeSetManagedDeclarations(&ctx, &csDecl, 1) == 0,
              "C++ レーンからは C# の登録スロットが何もしない");
        Check(csApi.ComputeRegisterManagedKernels(&csCtx, &csAbi, 1) == 1 && csApi.ComputeSetManagedDeclarations(&csCtx, &csDecl, 1) == 1,
              "C# レーンの登録スロットが通る");
        const uint64_t cy = csApi.ComputeCreateBuffer(&csCtx, owner, kCount, sizeof(float), 0);
        const uint64_t cx = csApi.ComputeCreateBuffer(&csCtx, owner, kCount, sizeof(float), 0);
        csApi.ComputeWriteBuffer(&csCtx, cy, y0.data(), kCount);
        csApi.ComputeWriteBuffer(&csCtx, cx, x0.data(), kCount);
        const uint64_t csBufs[2] = { cy, cx };
        const uint64_t csJob = csApi.ComputeForEach(&csCtx, kSaxpy, 0, &params, sizeof(params), csBufs, 2, kCsScript);
        const uint64_t csNoCaller = csApi.ComputeForEach(&csCtx, kSaxpy, 0, &params, sizeof(params), csBufs, 2, nullptr);
        const uint64_t csUndeclared = csApi.ComputeForEach(&csCtx, kSaxpy, 0, &params, sizeof(params), csBufs, 2, kAbiScript);
        Check(csJob == kJobDoneCpu && csNoCaller == 0 && csUndeclared == 0,
              "C# レーン: 宣言した型は成功、型名なし (スクリプトの外) と C++ の型名は失敗");
        Check(csApi.ComputeForEach(&csCtx, kSaxpy, 0, &params, sizeof(params), bufs, 2, kCsScript) == 0,
              "C++ のストアのハンドルは C# レーンでは通らない");
        const uint64_t cppUsingCs = api.ComputeForEach(&ctx, kSaxpy, 0, &params, sizeof(params), csBufs, 2, nullptr);
        Check(cppUsingCs == 0, "C# のストアのハンドルは C++ レーンでは通らない");

        // ComputeService が無い ctx は全部が 0
        ScriptApiContext none;
        MyeEngineApi noneApi = {};
        BuildEngineApi(noneApi, &none);
        Check(noneApi.ComputeCreateBuffer(&none, owner, 4, 4, 0) == 0 && noneApi.ComputeForEach(&none, kSaxpy, 0, &params, sizeof(params), bufs, 2, nullptr) == 0
                  && noneApi.ComputeJobWait(&none, 1) == 0,
              "ComputeService が繋がっていないと全スロットが 0");
    }
    js.Shutdown();
}

// ---------------------------------------------------------------------------
// ReplaceKernels / 宣言表の丸ごと差し替え
// ---------------------------------------------------------------------------

void TestReplaceTables()
{
    jobs::JobSystem js;
    js.Init();
    {
        ComputeService svc;
        svc.Bind(nullptr, nullptr, &js);
        const MyeKernelDesc abi = SaxpyAbiDesc(0x1111);

        svc.ReplaceKernels(CallerKind::Cpp, { KernelDescFromAbi(abi, CallerKind::Cpp) });
        const ComputeKernelDesc* k = svc.FindKernel(kSaxpy);
        Check(k != nullptr && k->cpp.range != nullptr && k->cpp.sourceHash == 0x1111 && k->csharp.range == nullptr,
              "ReplaceKernels(Cpp) は C++ の CPU 版だけを入れる");
        svc.ReplaceKernels(CallerKind::CSharp, { KernelDescFromAbi(SaxpyAbiDesc(0x2222), CallerKind::CSharp) });
        k = svc.FindKernel(kSaxpy);
        Check(k != nullptr && k->cpp.sourceHash == 0x1111 && k->csharp.sourceHash == 0x2222, "C# の登録は C++ の CPU 版を消さない");
        svc.ReplaceKernels(CallerKind::Cpp, {});
        k = svc.FindKernel(kSaxpy);
        Check(k != nullptr && k->cpp.range == nullptr && k->csharp.range != nullptr, "C++ の表を空にしても C# の CPU 版は残る");
        svc.ReplaceKernels(CallerKind::CSharp, {});
        Check(!svc.HasKernel(kSaxpy), "どちらの CPU 版も無くなったカーネルは表から消える");

        // 引数表が変わったら、もう一方の古い CPU 版は捨てる
        svc.ReplaceKernels(CallerKind::CSharp, { KernelDescFromAbi(abi, CallerKind::CSharp) });
        MyeKernelDesc changed = SaxpyAbiDesc(0x3333);
        changed.paramsSize += 4;
        svc.ReplaceKernels(CallerKind::Cpp, { KernelDescFromAbi(changed, CallerKind::Cpp) });
        k = svc.FindKernel(kSaxpy);
        Check(k != nullptr && k->cpp.sourceHash == 0x3333 && k->csharp.range == nullptr && k->paramsSize == changed.paramsSize,
              "引数表の食い違う再登録は、もう一方の言語の古い CPU 版を捨てる");

        // 不正な表は登録しない (HLSL の変数名が空)
        MyeKernelArg badArgs[1] = { { "", 4, 1 } };
        MyeKernelDesc bad = abi;
        bad.name = "BadKernel";
        bad.argCount = 1;
        bad.args = badArgs;
        svc.ReplaceKernels(CallerKind::Cpp, { KernelDescFromAbi(bad, CallerKind::Cpp) });
        Check(!svc.HasKernel("BadKernel"), "HLSL の変数名が空の引数表は登録されない");

        // 宣言表は言語ごとに丸ごと差し替わる
        svc.ReplaceDeclarations(CallerKind::Cpp, { { "A", { "K1", "K2" } }, { "B", { "K1" } } });
        svc.ReplaceDeclarations(CallerKind::CSharp, { { "A", { "K9" } } });
        Check(svc.IsDeclared(CallerKind::Cpp, "A", "K2") && !svc.IsDeclared(CallerKind::Cpp, "A", "K9")
                  && svc.IsDeclared(CallerKind::CSharp, "A", "K9") && !svc.IsDeclared(CallerKind::CSharp, "A", "K1"),
              "宣言表は言語ごとに別");
        svc.ReplaceDeclarations(CallerKind::Cpp, { { "A", { "K3" } } });
        Check(!svc.IsDeclared(CallerKind::Cpp, "A", "K2") && !svc.IsDeclared(CallerKind::Cpp, "B", "K1")
                  && svc.IsDeclared(CallerKind::Cpp, "A", "K3") && svc.IsDeclared(CallerKind::CSharp, "A", "K9"),
              "差し替えで消えた宣言は残らず、もう一方の言語の宣言には触らない");
    }
    js.Shutdown();
}

// ---------------------------------------------------------------------------
// GameLogic.dll の ComputeProbe を HeadlessSim で走らせる
// ---------------------------------------------------------------------------

bool ReadField(HeadlessSim& sim, const char* host, const char* field, uint64_t& out)
{
    Scene* scene = sim.Refs().scene;
    const GameObject obj = scene->Find(host);
    if (!obj) {
        return false;
    }
    ScriptApiContext apiCtx;
    apiCtx.scene = scene;
    MyeEngineApi api = {};
    BuildEngineApi(api, &apiCtx);
    uint64_t buf = 0;
    const MyeEntityId id = { obj.Id().index, obj.Id().generation };
    if (api.GetComponentField(&apiCtx, id, HashStr("ComputeProbe"), HashStr(field), &buf, sizeof(buf), nullptr) <= 0) {
        return false;
    }
    out = buf;
    return true;
}

uint64_t Field(HeadlessSim& sim, const char* host, const char* field)
{
    uint64_t v = ~0ull;
    return ReadField(sim, host, field, v) ? v : ~0ull;
}

// ComputeProbe.cpp の ChecksumOf と同じ式
uint32_t ChecksumOf(const std::vector<float>& v)
{
    uint32_t h = 2166136261u;
    for (const float f : v) {
        uint32_t bits = 0;
        std::memcpy(&bits, &f, sizeof(bits));
        h = (h ^ bits) * 16777619u;
    }
    return h;
}

// Start で y = i * 0.5 + 2、Update ごとに +2 (ComputeProbe.cpp)
uint32_t ExpectedChecksum(uint32_t calls)
{
    std::vector<float> y(100);
    for (uint32_t i = 0; i < y.size(); ++i) {
        y[i] = static_cast<float>(i) * 0.5f + 2.0f * static_cast<float>(calls);
    }
    return ChecksumOf(y);
}

void Tick(HeadlessSim& sim, uint32_t n = 1)
{
    InputSnapshot lanes[kMaxPlayers] = {};
    for (uint32_t i = 0; i < n; ++i) {
        sim.RunTick(lanes, nullptr, false);
    }
}

GameObject AddProbe(HeadlessSim& sim, const char* name)
{
    Scene* scene = sim.Refs().scene;
    GameObject host = scene->CreateGameObject(name);
    scene->GetWorld().AddComponentRaw(host.Id(), ComponentRegistry::Get().FindByName("ComputeProbe"));
    return host;
}

void TestGameLogicProbe()
{
    HeadlessSimSetup setup;
    setup.config.title = L"MyEngine ComputeScriptSelfTest";
    setup.config.localPlayers = static_cast<int>(kMaxPlayers);
    setup.systemInput = true; // ServerNetSelfTest と同じ構成 (システム入力なしの構成は ForEach の経路に関係しない)
    setup.scene.showcase = FindShowcase(L"--local-demo", /*editor=*/true);
    HeadlessSim sim;
    if (setup.scene.showcase == nullptr || !sim.Init(setup)) {
        Check(false, "ヘッドレス sim が起動する (リポジトリのルートから実行すること)");
        return;
    }
    if (ComponentRegistry::Get().FindByName("ComputeProbe") == kInvalidComponentType) {
        MYE_LOG_WARN("  SKIP: GameLogic.dll に ComputeProbe が無い (GameLogic をビルドしていない)。ABI 経由の GameLogic の検証は未実施");
        return;
    }
    ComputeService& svc = sim.Compute();

    // 読み込み時に表が揃っている (Start より前): カーネル表・宣言・ソースの検査結果
    const ComputeKernelDesc* saxpy = svc.FindKernel(kSaxpy);
    Check(saxpy != nullptr && saxpy->cpp.range != nullptr && saxpy->args.size() == 2 && saxpy->args[0].name == "y"
              && saxpy->args[1].name == "x" && saxpy->args[0].writable && !saxpy->args[1].writable,
          "GameLogic.dll のカーネル表が取り込まれている (HLSL の変数名・RW/RO つき)");
    Check(svc.IsDeclared(CallerKind::Cpp, "ComputeProbe", kSaxpy) && !svc.IsDeclared(CallerKind::Cpp, "ComputeProbe", kUnused),
          "COMPUTE_KERNELS の宣言が取り込まれている (宣言したものだけ)");

    // 生成した表のハッシュ (PowerShell) が、ディスク上のファイルの C++ 版ハッシュと一致する
    bool allMatch = true;
    for (const wchar_t* file : { L"SelfTestMix.kernel.hlsl", L"SelfTestSaxpy.kernel.hlsl", L"SelfTestUnused.kernel.hlsl" }) {
        const std::string name = WideToUtf8(file).substr(0, WideToUtf8(file).size() - std::strlen(".kernel.hlsl"));
        const ComputeKernelDesc* k = svc.FindKernel(name);
        const uint64_t onDisk = kernel::HashKernelSource(ReadKernelFile(file));
        const bool ok = k != nullptr && k->cpp.sourceHash == onDisk;
        if (!ok) {
            MYE_LOG_ERROR("  hash mismatch: %s table=%016llx disk=%016llx", name.c_str(),
                          k != nullptr ? static_cast<unsigned long long>(k->cpp.sourceHash) : 0ull,
                          static_cast<unsigned long long>(onDisk));
        }
        allMatch = allMatch && ok;
    }
    Check(allMatch, "gen_project_files.ps1 が埋めたハッシュが、エンジンの C++ 版ハッシュと全カーネルで一致する");

    // 1 tick 目: Start の呼び出しと最初の Update
    AddProbe(sim, "ComputeProbeHost");
    Tick(sim);
    Check(Field(sim, "ComputeProbeHost", "startDeclaredJob") == kJobDoneCpu && Field(sim, "ComputeProbeHost", "startUndeclaredJob") == 0,
          "Start の中の ForEach: 宣言したカーネルは成功し、宣言していないカーネルは失敗する");
    Check(Field(sim, "ComputeProbeHost", "updateDeclaredJob") == kJobDoneCpu && Field(sim, "ComputeProbeHost", "updateUndeclaredJob") == 0
              && Field(sim, "ComputeProbeHost", "wrongCaseJob") == 0,
          "Update の中の ForEach も同じ判定 (宣言なし・名前の大文字小文字違いは失敗)");
    Check(Field(sim, "ComputeProbeHost", "updates") == 1 && Field(sim, "ComputeProbeHost", "checksum") == ExpectedChecksum(2),
          "宣言したカーネルの結果が GameLogic の C++ 版で計算されている (Start 1 回 + Update 1 回)");
    Check(Field(sim, "ComputeProbeHost", "outvTouched") == 0, "宣言していないカーネルの出力バッファは書き換えられない");
    Tick(sim, 3);
    Check(Field(sim, "ComputeProbeHost", "updates") == 4 && Field(sim, "ComputeProbeHost", "checksum") == ExpectedChecksum(5),
          "tick を重ねても毎回同じ判定で、結果が積み上がる");

    // ソースの版ずれ: ディスクのファイルが変わったことにすると、C++ の State の ForEach が失敗する
    const std::string saxpyText = ReadKernelFile(L"SelfTestSaxpy.kernel.hlsl");
    svc.UpdateKernelSource("SelfTestSaxpy.kernel.hlsl", saxpyText + "\n// edited\n");
    Tick(sim);
    Check(Field(sim, "ComputeProbeHost", "updateDeclaredJob") == 0 && Field(sim, "ComputeProbeHost", "checksum") == ExpectedChecksum(5),
          "ソースを 1 行足すとハッシュ不一致になり、State の ForEach が失敗して結果は変わらない");
    svc.UpdateKernelSource("SelfTestSaxpy.kernel.hlsl", saxpyText);
    Tick(sim);
    Check(Field(sim, "ComputeProbeHost", "updateDeclaredJob") == kJobDoneCpu && Field(sim, "ComputeProbeHost", "checksum") == ExpectedChecksum(6),
          "元のソースに戻すと一致に戻り、ForEach が成功する");

    // ホットリロード直後: 表と宣言はモジュールと同じ世代で差し替わり、最初の tick も同じ判定になる
    const ComputeKernelDesc* before = svc.FindKernel(kSaxpy);
    const kernel::KernelRangeFn oldRange = before != nullptr ? before->cpp.range : nullptr;
    const bool reloaded = sim.ReloadGameLogic();
    const ComputeKernelDesc* after = svc.FindKernel(kSaxpy);
    Check(reloaded && after != nullptr && after->cpp.range != nullptr && after->cpp.range != oldRange,
          "ホットリロードでカーネルの CPU 版が新しい DLL のものに差し替わる");
    Check(svc.IsDeclared(CallerKind::Cpp, "ComputeProbe", kSaxpy) && !svc.IsDeclared(CallerKind::Cpp, "ComputeProbe", kUnused),
          "ホットリロード後も宣言表が同じ世代で揃っている (tick を回す前に)");
    AddProbe(sim, "ComputeProbeHost2");
    Tick(sim);
    Check(Field(sim, "ComputeProbeHost2", "startDeclaredJob") == kJobDoneCpu && Field(sim, "ComputeProbeHost2", "startUndeclaredJob") == 0
              && Field(sim, "ComputeProbeHost", "updateDeclaredJob") == kJobDoneCpu && Field(sim, "ComputeProbeHost", "updateUndeclaredJob") == 0
              && Field(sim, "ComputeProbeHost", "checksum") == ExpectedChecksum(7),
          "ホットリロード後の最初の tick: 新しい Start も既存の Update も同じ判定で成功 / 失敗する");

    // 持ち主 (ABI の owner 引数) の破棄で、そのスクリプトのバッファが解放される
    const uint32_t liveBefore = svc.Store(CallerKind::Cpp).LiveCount();
    Scene* scene = sim.Refs().scene;
    const GameObject host2 = scene->Find("ComputeProbeHost2");
    scene->GetWorld().DestroyEntity(host2.Id());
    Tick(sim);
    Check(liveBefore == 12 && svc.Store(CallerKind::Cpp).LiveCount() == 6,
          "ABI の CreateBuffer に渡した owner が効き、エンティティの破棄で 6 本のバッファが解放される");
}

} // namespace

bool RunComputeScriptSelfTest()
{
    MYE_LOG_INFO("==== Compute script ABI (M91e) self test ====");
    g_fails = 0;
    TestAbiSlots();
    TestReplaceTables();
    TestGameLogicProbe();
    MYE_LOG_INFO("==== Compute script ABI self test: %s ====", g_fails == 0 ? "PASS" : "FAIL");
    return g_fails == 0;
}

} // namespace mye
