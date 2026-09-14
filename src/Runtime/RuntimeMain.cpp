#include <cstdio>
#include <filesystem>
#include <string>

#include <Windows.h>
#include <shellapi.h>

#include "Engine/Core/Log.h"
#include "Engine/Engine/DemoContent.h"
#include "Engine/Engine/EngineCli.h"
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
    bool rtShowcase = false; // --rt-demo (M46i: コーネル箱のショーケース)
    bool localDemo = false;  // --local-demo (M52g: ローカルマルチプレイの入力レーンデモ)
    bool netDemo = false;    // --net-demo (M52i: 2 人ネット対戦のデモ)
    bool renderShowcase = false; // --render-demo (M54a: 描画ロードマップのショーケース)
    bool terrainShowcase = false; // --terrain-demo (M58c: 地形のショーケース)
    bool physicsShowcase = false; // --physics-demo (M59d: 物理のショーケース。M59l で golden 13 枚目)
    bool jointShowcase = false;   // --joint-demo (M60i: 関節と機構のショーケース)
    bool fogShowcase = false;     // --fog-demo (M57追補: 霧 + GPU 粒子 + VFX。golden 15 枚目)
    bool particleShowcase = false; // --particle-demo (M63a: 粒子表現。golden 16/17 枚目)
    bool acousticShowcase = false; // --acoustic-demo (M65b: 音響伝播。replay 7 ペア目)
    bool uiShowcase = false;       // --ui-demo (M75c: ゲーム内 UI。golden 25 枚目)
    float terrainLodDistance = 0.0f; // --terrain-lod DIST (M58e: 0 = LOD 無効)
    float terrainSkirtDepth = 0.0f;  // --terrain-skirt D (M58e: 0 = 自動 / < 0 = 無し)

    void OnStart(mye::EngineContext& ctx) override
    {
        ctx.shaders->Load("forward_lit");
        mye::RegisterDemoContent(ctx);   // Editor と同じ実体登録 (AssetID 解決)
        mye::RegisterAssetLibraries(ctx); // .prefab / .anim を登録
        if (scenePath.empty() && uiShowcase) {
            // M75c: render-demo と同じ理由でコードから毎回組む (bat が撮影前に消す)
            scenePath = L"cache\\ui_showcase.scene.json";
        } else if (scenePath.empty() && fogShowcase) {
            // M57追補: joint / physics と同じ理由でコードから毎回組む (bat が撮影前に消す)
            scenePath = L"cache\\fog_showcase.scene.json";
        } else if (scenePath.empty() && jointShowcase) {
            // M60i: physics と同じ理由でコードから毎回組む (bat が撮影前に消す)
            scenePath = L"cache\\joint_showcase.scene.json";
        } else if (scenePath.empty() && acousticShowcase) {
            // M65b: joint と同じ理由でコードから毎回組む (bat が撮影前に消す)。★以前は枝が無く
            // main.scene.json へ落ちていた (保存すると既定シーンを潰し、あるとショーケースを組まずに読む)
            scenePath = L"cache\\acoustic_showcase.scene.json";
        } else if (scenePath.empty() && particleShowcase) {
            // M63a: 同上
            scenePath = L"cache\\particle_showcase.scene.json";
        } else if (scenePath.empty() && physicsShowcase) {
            // M59l: render/terrain と同じ理由でコードから毎回組む (bat が撮影前に消す)
            scenePath = L"cache\\physics_showcase.scene.json";
        } else if (scenePath.empty() && terrainShowcase) {
            // M58c: render-demo と同じ理由でコードから毎回組む (bat が撮影前に消す)
            scenePath = L"cache\\terrain_showcase.scene.json";
        } else if (scenePath.empty() && renderShowcase) {
            // M54a: コードから毎回組む (local-demo と同じ理由)。**shot_verify はこの経路で
            // 撮る**ので、cache\ に保存済みが残っていると exists() 側へ落ちて golden が
            // 静かに変わる — bat 側で撮影前に消している
            scenePath = L"cache\\render_showcase.scene.json";
        } else if (scenePath.empty() && netDemo) {
            // M52i: コードから毎回組む (local-demo と同じ理由 — 保存済みが残っていると
            // ロード経路に落ちてコード側の正解と食い違う)
            scenePath = L"cache\\net_duel.scene.json";
        } else if (scenePath.empty() && localDemo) {
            // M52g: コードから毎回組む (ファイルは作らない)。パスだけ cache\ に振っておくと
            // 万一保存されても main.scene.json を潰さない
            scenePath = L"cache\\local_players.scene.json";
        } else if (scenePath.empty() && rtShowcase) {
            // M46i: ショーケースはブートシーンと別枠。保存済みファイルがあればそれを読み、
            // 無ければコードから組む (main.scene.json には一切触らない)
            scenePath = ctx.assetsRoot + L"\\scenes\\rt_showcase.scene.json";
        } else if (scenePath.empty()) {
            scenePath = ctx.assetsRoot + L"\\scenes\\main.scene.json";
            mye::ProjectManifest manifest; // ブートシーンはマニフェスト優先 (M26)
            if (!ctx.projectRoot.empty() && mye::LoadProjectManifest(ctx.projectRoot, manifest)) {
                scenePath = mye::ProjectBootScenePath(ctx.projectRoot, manifest);
            }
        }
        // ショーケース材質は無条件で登録する (M50a)。--scene で保存済みショーケースを
        // 直接開く経路でも実体が揃う。
        // Runtime には --parts-demo が無いので、ゲートしたままだと parts 材質は常に欠落する
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
        } else if (uiShowcase) {
            mye::BuildUiShowcaseScene(ctx); // M75c
        } else if (jointShowcase) {
            mye::BuildJointShowcaseScene(ctx); // M60i
        } else if (acousticShowcase) {
            mye::BuildAcousticShowcaseScene(ctx); // M65b
        } else if (particleShowcase) {
            mye::BuildParticleShowcaseScene(ctx); // M63a
        } else if (fogShowcase) {
            mye::BuildFogShowcaseScene(ctx); // M57追補
        } else if (physicsShowcase) {
            mye::BuildPhysicsShowcaseScene(ctx); // M59d
        } else if (terrainShowcase) {
            mye::BuildTerrainShowcaseScene(ctx, terrainLodDistance, terrainSkirtDepth); // M58c/e
        } else if (renderShowcase) {
            mye::BuildRenderShowcaseScene(ctx); // M54a
        } else if (netDemo) {
            mye::BuildNetDuelScene(ctx); // M52i
        } else if (localDemo) {
            mye::BuildLocalPlayersScene(ctx); // M52g
        } else if (rtShowcase) {
            mye::BuildRtShowcaseScene(ctx); // M46i
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
            } else if (arg == L"--rt-demo") {
                app.rtShowcase = true; // M46i: コーネル箱のショーケースシーンを構築
            } else if (arg == L"--local-demo") {
                app.localDemo = true; // M52g: 入力レーンのローカルマルチプレイデモ
            } else if (arg == L"--net-demo") {
                app.netDemo = true; // M52i: 2 人ネット対戦のデモシーン
            } else if (arg == L"--render-demo") {
                app.renderShowcase = true; // M54a: 描画ショーケース (shot_verify の 6/7 枚目)
            } else if (arg == L"--terrain-demo") {
                app.terrainShowcase = true; // M58c: 地形ショーケース (shot_verify の 8 枚目)
            } else if (arg == L"--physics-demo") {
                app.physicsShowcase = true; // M59d: 物理ショーケース (shot_verify の 13 枚目)
            } else if (arg == L"--joint-demo") {
                app.jointShowcase = true; // M60i: 関節ショーケース (M60k で 14 枚目)
            } else if (arg == L"--fog-demo") {
                // M57追補: 霧 + GPU 粒子 + VFX のショーケース (golden 15 枚目の被写体)
                app.fogShowcase = true;
            } else if (arg == L"--particle-demo") {
                // M63a: 粒子表現のショーケース (golden 16/17 枚目 = CPU/GPU の突き合わせ)
                app.particleShowcase = true;
            } else if (arg == L"--acoustic-demo") {
                // M65b: 音響伝播のショーケース (replay 7 ペア目の被写体)
                app.acousticShowcase = true;
            } else if (arg == L"--ui-demo") {
                app.uiShowcase = true; // M75c: ゲーム内 UI のショーケース (golden 25 枚目)
            } else if (arg == L"--terrain-lod" && i + 1 < argc) {
                // M58e: 地形 LOD の切替距離。**golden は LOD 無しのまま**で、
                // クラック A/B のときだけ点ける
                app.terrainLodDistance = static_cast<float>(_wtof(argv[++i]));
            } else if (arg == L"--terrain-skirt" && i + 1 < argc) {
                app.terrainSkirtDepth = static_cast<float>(_wtof(argv[++i])); // 負値 = 無し
            } else if (arg == L"--project" && i + 1 < argc) {
                // M26: プロジェクト指定。dist 配布物は従来どおり exe 隣の assets を自動発見する
                config.projectRoot = std::filesystem::absolute(argv[++i]).wstring();
            }
        }
        LocalFree(argv);
    }

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
