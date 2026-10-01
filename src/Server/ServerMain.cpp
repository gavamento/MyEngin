//====================================================================================
//                          ServerMain.cpp
//  MyEngin/ 秋田蓮音                                                     10/02/2026
//                                          ヘッドレス専用サーバ (窓・GPU・音声出力を作らない)
//====================================================================================
#include <cstdio>
#include <filesystem>
#include <string>

#include <Windows.h>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Engine/App/EngineCli.h"
#include "Engine/Engine/Demo/ShowcaseScenes.h"
#include "Engine/Engine/Loop/EngineLoop.h"
#include "Engine/Engine/Loop/HeadlessSim.h"
#include "Engine/Engine/Replay/Replay.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Platform/PathUtil.h"

namespace {

// 終了前に「ロードされていてはならない DLL」。Server.exe は Engine.lib を通じてこれらの import を
// 持つが、すべて /DELAYLOAD なので、GPU・音声・コントローラの関数を呼ばない限りロードされない。
// ロードされていたらヘッドレスの約束 (デバイスを作らない) が破れている
constexpr const wchar_t* kForbiddenModules[] = {
    L"d3d11.dll", L"dxgi.dll", L"d3dcompiler_47.dll", L"xaudio2_9.dll", L"xinput9_1_0.dll",
    L"nethost.dll", // C# ホスト (CoreCLR) の起動。サーバは C# レーンを持たない
};

// ロードされていた DLL の数を返す (0 = 約束どおり)
int CheckForbiddenModules()
{
    int loaded = 0;
    for (const wchar_t* name : kForbiddenModules) {
        if (GetModuleHandleW(name) != nullptr) {
            MYE_LOG_ERROR("[headless] %s is LOADED - the server created a GPU / audio / input device",
                          mye::WideToUtf8(name).c_str());
            ++loaded;
        }
    }
    if (loaded == 0) {
        MYE_LOG_INFO("[headless] module check OK: d3d11 / dxgi / d3dcompiler_47 / xaudio2_9 / xinput "
                     "/ nethost are not loaded");
    }
    return loaded;
}

// Editor / Runtime のシャドウコピー先 (<repo>\cache\hot) と同じ場所を使っていないことを確かめる
bool ShadowCopyIsIsolated(const mye::HeadlessSim& sim)
{
    const std::filesystem::path shadow = std::filesystem::path(sim.ShadowCopyDir()).lexically_normal();
    const std::filesystem::path shared =
        (std::filesystem::path(sim.AssetsRoot()).parent_path() / L"cache" / L"hot").lexically_normal();
    return shadow != shared;
}

void PrintUsage()
{
    std::fprintf(stderr,
                 "usage: Server.exe --replay-verify <file.rep> [--*-demo | --scene <path>] "
                 "[--project <dir>] [--local-players N] [--no-jobs] [--no-sim-cache]\n"
                 "  --replay-verify  Replays a .rep headlessly and compares every tick's world hash.\n"
                 "                   exit 0 = all ticks identical, 1 = mismatch / could not run, "
                 "2 = a GPU / audio module was loaded.\n"
                 "  (the live-server mode is not implemented yet)\n");
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    mye::HeadlessSimSetup setup;
    setup.config.title = L"MyEngine Server";
    mye::EngineCliExtras cli;

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        // Editor / Runtime と同じ意味のフラグは表で読む (EngineCli.cpp)。Server は GPU 系のフラグ
        // (--warp 等) も受け取るが読み捨てる — CI が全 exe に同じ MYE_EXTRA_ARGS を後置するため
        const mye::CliParse shared = mye::ParseEngineCliFlag(argc, argv, i, setup.config, cli);
        if (shared == mye::CliParse::Error) {
            return 1;
        }
        if (shared == mye::CliParse::Consumed) {
            continue;
        }
        if (arg == L"--scene" && i + 1 < argc) {
            setup.scene.scenePath = argv[++i];
        } else if (const mye::ShowcaseDef* s = mye::FindShowcase(arg, /*editor=*/true)) {
            // golden は Editor で録っているので、Editor 専用の行 (--parts-demo / --flow-demo) も引く
            setup.scene.showcase = mye::PickShowcase(setup.scene.showcase, s);
        } else if (arg == L"--terrain-lod" && i + 1 < argc) {
            setup.scene.showcaseOptions.terrainLodDistance = static_cast<float>(_wtof(argv[++i]));
        } else if (arg == L"--terrain-skirt" && i + 1 < argc) {
            setup.scene.showcaseOptions.terrainSkirtDepth = static_cast<float>(_wtof(argv[++i]));
        } else if (arg == L"--project" && i + 1 < argc) {
            setup.config.projectRoot = std::filesystem::absolute(argv[++i]).wstring();
        } else {
            std::fprintf(stderr, "unknown or incomplete argument: %s\n", mye::WideToUtf8(arg).c_str());
            PrintUsage();
            return 1;
        }
    }
    setup.config.developmentRun = !setup.config.projectRoot.empty(); // Runtime と同じ規則 (ABI v18)

    // --hash-diff / --rep-diff は Runtime と同じ差分ツール (窓も sim も要らない)
    if (!cli.hashDiffA.empty() && !cli.hashDiffB.empty()) {
        mye::HashDump a;
        mye::HashDump b;
        if (!mye::ReadHashDump(cli.hashDiffA, a) || !mye::ReadHashDump(cli.hashDiffB, b)) {
            return 2;
        }
        return mye::DiffHashDumps(a, b).Same() ? 0 : 1;
    }
    if (!cli.repDiffA.empty() && !cli.repDiffB.empty()) {
        const mye::ReplayDiffResult r = mye::DiffReplayFiles(cli.repDiffA, cli.repDiffB);
        std::fprintf(stdout, "[rep-diff] %s\n", r.summary.c_str());
        if (r.same) {
            return 0;
        }
        return r.summary.find("could not be loaded") != std::string::npos ? 2 : 1;
    }
    if (!cli.writeContentManifest.empty()) {
        return mye::RunWriteContentManifestCli(setup.config.projectRoot, cli.writeContentManifest);
    }

    if (setup.config.replayVerifyPath.empty()) {
        PrintUsage();
        return 1;
    }

    int exitCode = 0;
    {
        mye::HeadlessSim sim;
        if (!sim.Init(setup)) {
            MYE_LOG_ERROR("[headless] could not start the simulation");
            return 1;
        }
        if (!ShadowCopyIsIsolated(sim)) {
            MYE_LOG_ERROR("[headless] the shadow-copy directory is shared with Editor / Runtime");
            return 2;
        }
        MYE_LOG_INFO("[headless] isolation: cook cache disabled, shadow copy = %s",
                     mye::WideToUtf8(sim.ShadowCopyDir()).c_str());
        const mye::HeadlessVerifyResult r = sim.VerifyReplay();
        if (!r.ran) {
            MYE_LOG_ERROR("[headless] replay verification could not run");
            exitCode = 1;
        } else if (r.passed) {
            MYE_LOG_INFO("[headless] verified %llu ticks - VERIFY PASS: hash-identical%s "
                         "(%.1f ms, %.3f ms/tick)",
                         static_cast<unsigned long long>(r.verifiedTicks),
                         r.unverifiedTicks > 0 ? " (plus ticks with no expected hash)" : "",
                         r.elapsedMs,
                         r.totalTicks > 0 ? r.elapsedMs / static_cast<double>(r.totalTicks) : 0.0);
        } else {
            MYE_LOG_ERROR("[headless] verified %llu ticks - VERIFY FAIL: first mismatch at tick %llu",
                          static_cast<unsigned long long>(r.verifiedTicks),
                          static_cast<unsigned long long>(r.firstMismatchTick));
            exitCode = 1;
        }
        // 終了前の H2 検査 (sim がまだ生きている = 遅延ロードされた DLL があればここで見える)
        if (CheckForbiddenModules() != 0) {
            return 2;
        }
    }
    return exitCode;
}
