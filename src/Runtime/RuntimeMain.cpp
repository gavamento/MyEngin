#include <cstdio>
#include <filesystem>
#include <string>

#include <Windows.h>
#include <shellapi.h>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Engine/Demo/StartScene.h"
#include "Engine/Engine/App/EngineCli.h"
#include "Engine/Engine/Physics/Fracture/FractureSystem.h" // PreloadFractureAssets (シーンロード直後の破片資産先読み)
#include "Engine/Engine/Demo/ShowcaseScenes.h"
#include "Engine/Engine/Loop/EngineLoop.h"
#include "Engine/Engine/Scene/Prefab.h"
#include "Engine/Engine/App/Project.h"
#include "Engine/Engine/Replay/Replay.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Engine/Scene/SceneSerializer.h"
#include "Engine/Platform/CrashHandler.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/Shader/ShaderManager.h"

namespace {

// コンソールから起動された場合に標準出力を繋ぐ (CLI/リプレイ検証用)。EditorMain と同じ方針
void AttachParentConsole()
{
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    const bool redirected = (out != nullptr && out != INVALID_HANDLE_VALUE);
    if (!redirected && AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
}

// エディタ UI 無しの薄いランタイム (engine_spec.md 1.4 / M15)。
// リソース + シーンを読み、常時シミュレートしてバックバッファへ直接描画する。
// エンジン (EngineLoop) と GameLogic.dll は Editor と完全に共有 —
// よって同一シーンのシミュレーションは Editor とビット単位で一致する (リプレイ検証で実証)。
class RuntimeApp : public mye::IEngineApp {
public:
    std::wstring scenePath;
    bool startDeferred = false;
    // --*-demo (ShowcaseScenes.cpp の表の 1 行。Editor 専用の行は引かない)。複数渡したら表の上の行が勝つ
    const mye::ShowcaseDef* showcase = nullptr;
    mye::ShowcaseOptions showcaseOptions; // --terrain-lod / --terrain-skirt (M58e)

    void OnStart(mye::EngineContext& ctx) override
    {
        // 起動シーンの用意は Editor / ヘッドレス Server と共有 (StartScene.cpp)
        mye::StartSceneOptions options;
        options.scenePath = scenePath;
        options.showcase = showcase;
        options.showcaseOptions = showcaseOptions;
        scenePath = mye::PrepareStartScene(ctx, options);
        if (startDeferred) {
            ctx.renderPath = ctx.renderPathDeferred;
        }
        MYE_LOG_INFO("RuntimeApp started (%u entities, scene=%s)",
                     ctx.scene->GetWorld().AliveCount(), mye::WideToUtf8(scenePath).c_str());
    }

    // ランタイムは常時シミュレート (Editor の Play 相当)
    void OnTick(mye::EngineContext& ctx) override { ctx.simulateScripts = true; }
};

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    AttachParentConsole();

    mye::EngineConfig config;
    config.title = L"MyEngine Runtime";
    config.renderSceneToBackbuffer = true; // シーンをバックバッファへ直接描画
    config.enableImGui = false;            // エディタ UI 無し

    RuntimeApp app;
    mye::EngineCliExtras cli; // --crash-test / --rep-diff / --hash-diff (Editor と共通の CLI。EngineCli.h)

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            const std::wstring arg = argv[i];
            // Editor と同じ意味のフラグは表で読む (EngineCli.cpp)。この下に残すのは Runtime だけのフラグ
            const mye::CliParse shared = mye::ParseEngineCliFlag(argc, argv, i, config, cli);
            if (shared == mye::CliParse::Error) {
                return 1;
            }
            if (shared == mye::CliParse::Consumed) {
                continue;
            }
            if (arg == L"--scene" && i + 1 < argc) {
                app.scenePath = argv[++i];
            } else if (arg == L"--deferred") {
                app.startDeferred = true;
            } else if (const mye::ShowcaseDef* s = mye::FindShowcase(arg, /*editor=*/false)) {
                app.showcase = mye::PickShowcase(app.showcase, s); // --*-demo (ShowcaseScenes.cpp の表)
            } else if (arg == L"--terrain-lod" && i + 1 < argc) {
                // M58e: 地形 LOD の切替距離。**golden は LOD 無しのまま**で、
                // クラック A/B のときだけ点ける
                app.showcaseOptions.terrainLodDistance = static_cast<float>(_wtof(argv[++i]));
            } else if (arg == L"--terrain-skirt" && i + 1 < argc) {
                app.showcaseOptions.terrainSkirtDepth = static_cast<float>(_wtof(argv[++i])); // 負値 = 無し
            } else if (arg == L"--project" && i + 1 < argc) {
                // M26: プロジェクト指定。指定が無ければ dist 配布物は exe 隣の assets を自動発見する
                config.projectRoot = std::filesystem::absolute(argv[++i]).wstring();
            }
        }
        LocalFree(argv);
    }
    // ABI v18 IsDevelopmentRun: --project 付き = 開発中 (verify.bat のヘッドレス検証もここ)、無し = 配布物。
    // 引数を全部読んでから決める (--project の位置に依らない)
    config.developmentRun = !config.projectRoot.empty();
    // ABI v19 SetWindowMode: 窓を動かすのは Runtime だけ (検証やバッチの実行はエンジン側で除外する)
    config.applyWindowMode = true;

    // --crash-test の綴り違いを黙って無視しない (M52f)。
    // 「落とすつもりで走らせたのに何も起きない」を 1 時間追いかける事故を潰す
    if (!cli.crashTestArg.empty()) {
        const mye::CrashTestKind kind = mye::ParseCrashTestKind(cli.crashTestArg.c_str());
        if (kind == mye::CrashTestKind::None) {
            std::fprintf(stderr,
                         "unknown --crash-test kind: %s "
                         "(av | purecall | terminate | invalidparam | stackoverflow)\n",
                         mye::WideToUtf8(cli.crashTestArg).c_str());
            return 2;
        }
        config.crashTest = static_cast<int>(kind);
    }

    // --hash-diff A B: ダンプ 2 本を突き合わせて終了 (M52a)。同一なら 0、食い違えば 1
    if (!cli.hashDiffA.empty() && !cli.hashDiffB.empty()) {
        mye::HashDump a;
        mye::HashDump b;
        if (!mye::ReadHashDump(cli.hashDiffA, a) || !mye::ReadHashDump(cli.hashDiffB, b)) {
            return 2;
        }
        return mye::DiffHashDumps(a, b).Same() ? 0 : 1;
    }

    // --rep-diff A B: .rep 2 本を突き合わせて終了 (M52h)。一致なら 0、食い違えば 1、
    // そもそも読めなければ 2。ネットの 2 プロセスが**同じ tick 列を回した**ことの機械証明で、
    // 割れたときは「どの tick の どのレーンの どのフィールドか」まで 1 行で出る
    if (!cli.repDiffA.empty() && !cli.repDiffB.empty()) {
        const mye::ReplayDiffResult r = mye::DiffReplayFiles(cli.repDiffA, cli.repDiffB);
        std::fprintf(stdout, "[rep-diff] %s\n", r.summary.c_str());
        if (r.same) {
            return 0;
        }
        return r.summary.find("could not be loaded") != std::string::npos ? 2 : 1;
    }

    // --write-content-manifest PATH: assets の content_manifest.json を書いて終了 (M81c)
    if (!cli.writeContentManifest.empty()) {
        return mye::RunWriteContentManifestCli(config.projectRoot, cli.writeContentManifest);
    }

    mye::EngineLoop loop;
    return loop.Run(config, app);
}
