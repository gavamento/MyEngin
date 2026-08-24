#include <cstdio>
#include <filesystem>
#include <string>

#include <Windows.h>
#include <shellapi.h>

#include "Engine/Core/Log.h"
#include "Engine/Engine/DemoContent.h"
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
    float terrainLodDistance = 0.0f; // --terrain-lod DIST (M58e: 0 = LOD 無効)
    float terrainSkirtDepth = 0.0f;  // --terrain-skirt D (M58e: 0 = 自動 / < 0 = 無し)

    void OnStart(mye::EngineContext& ctx) override
    {
        ctx.shaders->Load("forward_lit");
        mye::RegisterDemoContent(ctx);   // Editor と同じ実体登録 (AssetID 解決)
        mye::RegisterAssetLibraries(ctx); // .prefab / .anim を登録
        if (scenePath.empty() && physicsShowcase) {
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
        // 直接開く経路 (replay_verify の Runtime 側 parts 検証もこれ) でも実体が揃う。
        // Runtime には --parts-demo が無いので、ゲートしたままだと parts 材質は常に欠落する
        mye::RegisterRtShowcaseContent(ctx);
        mye::RegisterPartsShowcaseContent(ctx);
        mye::RegisterFlowShowcaseContent(ctx); // M51j: flow_* 材質 (配布ブートシーンにも使う)
        mye::RegisterLocalPlayersContent(ctx);  // M52g: mp_* 材質 (同上の理由で常時)
        mye::RegisterNetDuelContent(ctx);       // M52i: duel_* 材質 (同上)
        mye::RegisterRenderShowcaseContent(ctx); // M54a: rdemo_* 材質 (同上)
        mye::RegisterTerrainShowcaseContent(ctx); // M58c: tdemo_* 材質 (同上)
        mye::RegisterPhysicsShowcaseContent(ctx); // M59d: pdemo_* 材質 (同上)
        if (std::filesystem::exists(scenePath)) {
            mye::SceneSerializer::LoadFromFile(*ctx.scene, scenePath);
            // Editor と同じ「ロード直後 1 回」(M48e)。ここを揃えないと Editor で録った .rep と
            // Runtime の verify で初期状態が食い違う
            mye::Prefab::RefreshNonOverridden(*ctx.scene, *ctx.prefabs);
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
    std::wstring repDiffA;
    std::wstring repDiffB;
    std::wstring hashDiffA; // --hash-diff A B (M52a)
    std::wstring hashDiffB;
    std::wstring crashTestArg; // --crash-test <kind> (M52f)

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            const std::wstring arg = argv[i];
            if (arg == L"--frames" && i + 1 < argc) {
                config.maxFrames = _wtoi64(argv[++i]);
            } else if (arg == L"--width" && i + 1 < argc) {
                config.width = _wtoi(argv[++i]);
            } else if (arg == L"--height" && i + 1 < argc) {
                config.height = _wtoi(argv[++i]);
            } else if (arg == L"--no-vsync") {
                config.vsync = false;
            } else if (arg == L"--screenshot" && i + 1 < argc) {
                config.screenshotPath = argv[++i];
            } else if (arg == L"--shot-frame" && i + 1 < argc) {
                config.screenshotFrame = _wtoi64(argv[++i]);
            } else if (arg == L"--shot-every" && i + 1 < argc) {
                config.screenshotEvery = _wtoi64(argv[++i]);
            } else if (arg == L"--scene" && i + 1 < argc) {
                app.scenePath = argv[++i];
            } else if (arg == L"--replay-record" && i + 1 < argc) {
                config.replayRecordPath = argv[++i];
                config.vsync = false;
            } else if (arg == L"--replay-verify" && i + 1 < argc) {
                config.replayVerifyPath = argv[++i];
                config.vsync = false;
            } else if (arg == L"--replay-ticks" && i + 1 < argc) {
                config.replayTicks = _wtoi64(argv[++i]);
            } else if (arg == L"--rep-snapshot") {
                // M52d: .rep へ記録開始時点の sim 状態を埋め込む (シーン非依存の再生)
                config.replayEmbedSnapshot = true;
            } else if (arg == L"--hash-dump" && i + 1 < argc) {
                config.hashDumpPath = argv[++i]; // M52a: フィールド単位ダンプの出力先
            } else if (arg == L"--hash-dump-tick" && i + 1 < argc) {
                config.hashDumpTick = _wtoi64(argv[++i]);
            } else if (arg == L"--snapshot-stress" && i + 1 < argc) {
                // M52d: N tick ごとにスナップショット往復を挟む (期待ハッシュ不変が合格条件)
                config.snapshotStress = _wtoi64(argv[++i]);
            } else if (arg == L"--timetravel-selftest") {
                // M52e: 巻き戻し + 再シムのハッシュ照合 (tick 数は省略可、既定 400)。
                // Runtime は常に sim を進めるので --autoplay 相当の細工は要らない
                config.timeTravelProbeTicks = 400;
                if (i + 1 < argc && argv[i + 1][0] != L'-') {
                    config.timeTravelProbeTicks = _wtoi64(argv[++i]);
                }
                config.vsync = false;
            } else if (arg == L"--crash-test" && i + 1 < argc) {
                crashTestArg = argv[++i]; // M52f (綴り違いは下で弾く)
                config.vsync = false;
            } else if (arg == L"--crash-at-tick" && i + 1 < argc) {
                config.crashTestTick = _wtoi64(argv[++i]);
            } else if (arg == L"--no-crash-handler") {
                config.crashHandler = false; // M52f: 既定 on を外す (デバッガ下での切り分け用)
            } else if (arg == L"--net-host") {
                // M52h: ホストとして待受 (ポート省略時は 7777)。参加側は --net-join
                config.netRole = 1;
                if (i + 1 < argc && argv[i + 1][0] != L'-') {
                    config.netPort = _wtoi(argv[++i]);
                }
            } else if (arg == L"--net-join" && i + 1 < argc) {
                config.netRole = 2;
                config.netJoinTarget = argv[++i]; // HOST:PORT
            } else if (arg == L"--net-players" && i + 1 < argc) {
                config.netPlayers = _wtoi(argv[++i]); // 現状 2 のみ
            } else if (arg == L"--net-delay" && i + 1 < argc) {
                // 入力遅延 (tick)。**全 peer で一致必須** — 違うとハンドシェイクで弾かれる
                config.netInputDelay = _wtoi(argv[++i]);
            } else if (arg == L"--net-loss" && i + 1 < argc) {
                config.netLossPercent = _wtoi(argv[++i]); // 入力パケットを故意に捨てる (検証用)
            } else if (arg == L"--net-no-rollback") {
                // M52i: 予測ロールバックを切って M52h の素の遅延ロックステップへ落とす
                config.netRollback = false;
            } else if (arg == L"--net-no-halt-on-desync") {
                config.netHaltOnDesync = false; // 検出しても止めずに走り続ける (観察用)
            } else if (arg == L"--net-poke-tick" && i + 1 < argc) {
                // M52i: 片側にだけ渡して意図的に desync を起こす (検出器の実地検証)
                config.netPokeTick = _wtoi64(argv[++i]);
            } else if (arg == L"--rep-diff" && i + 2 < argc) {
                // M52h: .rep 2 本の突き合わせ (ネットの 2 プロセスが同じ tick 列を回したか)
                repDiffA = argv[++i];
                repDiffB = argv[++i];
            } else if (arg == L"--hash-diff" && i + 2 < argc) {
                // M52a: 配布ビルド単体でもクラッシュ報告のダンプを突き合わせられるように
                hashDiffA = argv[++i];
                hashDiffB = argv[++i];
            } else if (arg == L"--deferred") {
                app.startDeferred = true;
            } else if (arg == L"--postfx-mode" && i + 1 < argc) {
                config.postFxTonemap = _wtoi(argv[++i]);
            } else if (arg == L"--no-postfx") {
                config.postFx = false;
            } else if (arg == L"--no-audio") {
                config.audio = false; // M45: XAudio2 を初期化しない (端末の無い CI / 撮影専用実行)
            } else if (arg == L"--font-embedded") {
                config.fontEmbedded = true; // M52c: 撮影のフォントを機種非依存に固定
            } else if (arg == L"--shot-realtime") {
                config.shotRealtime = true; // M52c: 決定的撮影を解除して実時間で回す
            } else if (arg == L"--warp") {
                config.forceWarp = true; // M52b: ソフトウェアラスタライザ固定 (CI / 撮影再現)
            } else if (arg == L"--exposure" && i + 1 < argc) {
                config.postFxExposure = static_cast<float>(_wtof(argv[++i]));
            } else if (arg == L"--no-bloom") {
                config.postFxBloom = false;
            } else if (arg == L"--bloom-threshold" && i + 1 < argc) {
                config.postFxBloomThreshold = static_cast<float>(_wtof(argv[++i]));
            } else if (arg == L"--bloom-intensity" && i + 1 < argc) {
                config.postFxBloomIntensity = static_cast<float>(_wtof(argv[++i]));
            } else if (arg == L"--no-fxaa") {
                config.postFxFxaa = false;
            } else if (arg == L"--no-jobs") {
                config.useJobs = false; // M25: 並列を直列化 (決定論ゲート / 計測比較)
            } else if (arg == L"--no-sim-cache") {
                config.useSimCache = false; // M51a: sim 索引を素通し (決定論ゲート / 切り分け)
            } else if (arg == L"--no-cook-cache") {
                config.useCookCache = false; // M51b: クックを使わず毎回フルパース (切り分け)
            } else if (arg == L"--rt-debug" && i + 1 < argc) {
                config.rtDebugMode = _wtoi(argv[++i]); // M46b (Deferred のみ)
            } else if (arg == L"--velocity-debug") {
                config.velocityDebug = 1; // M55c: GBuffer RT4 の可視化 (Deferred のみ)
            } else if (arg == L"--hzb-debug" && i + 1 < argc) {
                config.hzbDebug = _wtoi(argv[++i]); // M56c: N=ミップ N-1 (Deferred のみ)
            } else if (arg == L"--ssr") {
                config.ssr = true; // M56d: SSR (Deferred のみ。HZB も一緒に組まれる)
            } else if (arg == L"--probe-bake" && i + 1 < argc) {
                // M56e: 反射プローブを 1 回だけ焼く (引数は "X,Y,Z")。**明示指示専用** —
                // 自動ベイクの口はどこにも無い (撮影ごとに焼き上がりが変わると決定的撮影が
                // 壊れるため)。焼いた 6 面は PNG に落ちて継ぎ目の一致が機械判定される
                config.probeBake = true;
                float px = 0.0f, py = 0.0f, pz = 0.0f;
                if (swscanf_s(argv[++i], L"%f,%f,%f", &px, &py, &pz) == 3) {
                    config.probeBakePos[0] = px;
                    config.probeBakePos[1] = py;
                    config.probeBakePos[2] = pz;
                }
            } else if (arg == L"--probe-bake-all") {
                // M56f: シーン中の ReflectionProbeComponent を全部焼いて描画へ載せる。
                // ★撮影に映すならベイクのフレームを --shot-frame より前に置くこと
                //   (ベイクはスクショ保存の後に走る)
                config.probeBakeAll = true;
            } else if (arg == L"--probe-bake-frame" && i + 1 < argc) {
                config.probeBakeFrame = _wtoi(argv[++i]); // M56e (既定 3 = --shot-frame と同じ)
            } else if (arg == L"--probe-bake-png" && i + 1 < argc) {
                config.probeBakePng = argv[++i]; // M56e (既定 testsctual\probe_faces.png)
            } else if (arg == L"--taa") {
                config.postFxTaa = true; // M55d: TAA + カメラジッタ (Deferred のみ)
            } else if (arg == L"--motion-blur" && i + 1 < argc) {
                config.postFxMotionBlur = static_cast<float>(_wtof(argv[++i])); // M55e
            } else if (arg == L"--rt-no-temporal") {
                config.rtTemporal = false; // M46d: 1spp 生のまま (A/B 計測用)
            } else if (arg == L"--rt-freeze-seed") {
                config.rtFreezeSeed = true; // M46d: 乱数列を進めない (決定的スクショ)
            } else if (arg == L"--rt-anim-seed") {
                config.rtAnimSeed = true; // M46d: スクショ時の自動 freeze を解除
            } else if (arg == L"--rt-no-svgf") {
                config.rtSvgf = false; // M46e: 空間フィルタ off (蓄積のみ = A/B 計測用)
            } else if (arg == L"--rt-gi") {
                config.rtGi = true; // M46f: GI を最終画像へ合成 (Deferred のみ)
            } else if (arg == L"--rt-shadow") {
                config.rtShadow = true; // M46g: 平行光の影をレイトレで (Deferred のみ)
            } else if (arg == L"--rt-refl") {
                config.rtRefl = true; // M46h: スペキュラ環境項をレイトレ反射で (Deferred のみ)
            } else if (arg == L"--froxel") {
                // M57b: フロクセルへの注入 (積分 = M57c / 合成 = M57e まで絵は不変)
                config.froxel = true;
            } else if (arg == L"--froxel-no-temporal") {
                config.froxelTemporal = false; // M57c: ジッタと履歴を止める (A/B 用)
                config.froxel = true;
            } else if (arg == L"--froxel-dump" && i + 1 < argc) {
                config.froxelDumpFrame = _wtoi(argv[++i]); // M57b/M57c: 読み戻して検査
                config.froxel = true;
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
            } else if (arg == L"--terrain-lod" && i + 1 < argc) {
                // M58e: 地形 LOD の切替距離。**golden は LOD 無しのまま**で、
                // クラック A/B のときだけ点ける
                app.terrainLodDistance = static_cast<float>(_wtof(argv[++i]));
            } else if (arg == L"--terrain-skirt" && i + 1 < argc) {
                app.terrainSkirtDepth = static_cast<float>(_wtof(argv[++i])); // 負値 = 無し
            } else if (arg == L"--local-players" && i + 1 < argc) {
                config.localPlayers = _wtoi(argv[++i]); // M52g: 消費する入力レーン数
            } else if (arg == L"--synth-input") {
                config.synthInput = true; // M52g: レーンごとの合成入力 (検証用)
            } else if (arg == L"--project" && i + 1 < argc) {
                // M26: プロジェクト指定。dist 配布物は従来どおり exe 隣の assets を自動発見する
                config.projectRoot = std::filesystem::absolute(argv[++i]).wstring();
            }
        }
        LocalFree(argv);
    }

    // --crash-test の綴り違いを黙って無視しない (M52f)。
    // 「落とすつもりで走らせたのに何も起きない」を 1 時間追いかける事故を潰す
    if (!crashTestArg.empty()) {
        const mye::CrashTestKind kind = mye::ParseCrashTestKind(crashTestArg.c_str());
        if (kind == mye::CrashTestKind::None) {
            std::fprintf(stderr,
                         "unknown --crash-test kind: %s "
                         "(av | purecall | terminate | invalidparam | stackoverflow)\n",
                         mye::WideToUtf8(crashTestArg).c_str());
            return 2;
        }
        config.crashTest = static_cast<int>(kind);
    }

    // --hash-diff A B: ダンプ 2 本を突き合わせて終了 (M52a)。同一なら 0、食い違えば 1
    if (!hashDiffA.empty() && !hashDiffB.empty()) {
        mye::HashDump a;
        mye::HashDump b;
        if (!mye::ReadHashDump(hashDiffA, a) || !mye::ReadHashDump(hashDiffB, b)) {
            return 2;
        }
        return mye::DiffHashDumps(a, b).Same() ? 0 : 1;
    }

    // --rep-diff A B: .rep 2 本を突き合わせて終了 (M52h)。一致なら 0、食い違えば 1、
    // そもそも読めなければ 2。ネットの 2 プロセスが**同じ tick 列を回した**ことの機械証明で、
    // 割れたときは「どの tick の どのレーンの どのフィールドか」まで 1 行で出る
    if (!repDiffA.empty() && !repDiffB.empty()) {
        const mye::ReplayDiffResult r = mye::DiffReplayFiles(repDiffA, repDiffB);
        std::fprintf(stdout, "[rep-diff] %s\n", r.summary.c_str());
        if (r.same) {
            return 0;
        }
        return r.summary.find("could not be loaded") != std::string::npos ? 2 : 1;
    }

    mye::EngineLoop loop;
    return loop.Run(config, app);
}
