#include <cstdio>
#include <filesystem>
#include <string>

#include <Windows.h>
#include <shellapi.h>

#include "Engine/Core/Log.h"
#include "Engine/Engine/DemoContent.h"
#include "Engine/Engine/EngineCli.h"
#include "Engine/Engine/ShowcaseScenes.h"
#include "Engine/Engine/EngineLoop.h"
#include "Engine/Engine/Prefab.h"
#include "Engine/Engine/Project.h"
#include "Engine/Engine/Replay/Replay.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Engine/SceneSerializer.h"
#include "Engine/Platform/CrashHandler.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/ShaderManager.h"

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
        ctx.shaders->Load("forward_lit");
        mye::RegisterDemoContent(ctx);   // Editor と同じ実体登録 (AssetID 解決)
        mye::RegisterAssetLibraries(ctx); // .prefab / .anim を登録
        if (scenePath.empty() && showcase != nullptr) {
            // ショーケースは表の保存先 (ShowcaseScenes.cpp)。cache\ の行はコードから毎回組む —
            // **shot_verify はこの経路で撮る**ので、保存済みが残っていると exists() 側へ落ちて
            // golden が静かに変わる。bat 側で撮影前に消している
            scenePath = mye::ShowcaseScenePath(*showcase, ctx.assetsRoot);
        } else if (scenePath.empty()) {
            scenePath = ctx.assetsRoot + L"\\scenes\\main.scene.json";
            mye::ProjectManifest manifest; // ブートシーンはマニフェスト優先 (M26)
            if (!ctx.projectRoot.empty() && mye::LoadProjectManifest(ctx.projectRoot, manifest)) {
                scenePath = mye::ProjectBootScenePath(ctx.projectRoot, manifest);
            }
        }
        // ショーケース材質は無条件で登録する (M50a)。--scene で保存済みショーケースを
        // 直接開く経路でも実体が揃う。
        // Runtime には --parts-demo が無いので、フラグでゲートすると parts 材質は常に欠落する
        mye::RegisterRtShowcaseContent(ctx);
        mye::RegisterPartsShowcaseContent(ctx);
        mye::RegisterFlowShowcaseContent(ctx); // M51j: flow_* 材質 (配布ブートシーンにも使う)
        mye::RegisterLocalPlayersContent(ctx);  // M52g: mp_* 材質 (同上の理由で常時)
        mye::RegisterNetDuelContent(ctx);       // M52i: duel_* 材質 (同上)
        mye::RegisterRenderShowcaseContent(ctx); // M54a: rdemo_* 材質 (同上)
        mye::RegisterTerrainShowcaseContent(ctx); // M58c: tdemo_* 材質 (同上)
        mye::RegisterPhysicsShowcaseContent(ctx); // M59d: pdemo_* 材質 (同上)
        mye::RegisterJointShowcaseContent(ctx);   // M60i: jdemo_* 材質 + 車輪メッシュ (同上)
        mye::RegisterFogShowcaseContent(ctx);     // M57追補: fdemo_* 材質 (同上)
        mye::RegisterParticleShowcaseContent(ctx); // M63a: vdemo_* 材質 + 手続きテクスチャ (同上)
        mye::RegisterAcousticShowcaseContent(ctx); // M65b: adem_* 材質 (同上)
        if (std::filesystem::exists(scenePath)) {
            mye::SceneSerializer::LoadFromFile(*ctx.scene, scenePath);
            // Editor と同じ「ロード直後 1 回」(M48e)。ここを揃えないと Editor で録った .rep と
            // Runtime の verify で初期状態が食い違う
            mye::Prefab::RefreshNonOverridden(*ctx.scene, *ctx.prefabs);
        } else if (showcase != nullptr && showcase->build != nullptr) {
            showcase->build(ctx, showcaseOptions);
        } else {
            mye::BuildDemoScene(ctx); // ブートシーンが無ければデモを構築
        }
        // ランタイムは即 Play 相当。Editor の PlayModeController::Play と同じ Save+Load リロードで
        // EntityID を正規化する — これにより Editor が録った .rep と決定論的に一致する (M8 規約)。
        {
            const nlohmann::json snap = mye::SceneSerializer::SaveToJson(*ctx.scene);
            mye::SceneSerializer::LoadFromJson(*ctx.scene, snap);
        }
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

    mye::EngineLoop loop;
    return loop.Run(config, app);
}
