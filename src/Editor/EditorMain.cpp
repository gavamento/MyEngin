#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <Windows.h>
#include <shellapi.h>

#include "Editor/EditorApp.h"
#include "Editor/EditorGlobalSettings.h"
#include "Editor/GameFlowSelfTest.h"
#include "Editor/PartSelfTest.h"
#include "Engine/Engine/Asset/CookedCacheSelfTest.h"
#include "Engine/Engine/SchemaSelfTest.h"
#include "Editor/ProjectManager.h"
#include "Editor/ProjectRegistry.h"
#include "Editor/ProjectTemplates.h"
#include "Editor/UndoSelfTest.h"
#include "Engine/Core/EcsSelfTest.h"
#include "Engine/Core/JobSystemSelfTest.h"
#include "Engine/Core/Localization.h"
#include "Engine/Core/LocalizationSelfTest.h"
#include "Engine/Core/Log.h"
#include "Editor/AssetOpsSelfTest.h"
#include "Editor/TerrainSelfTest.h"
#include "Engine/Engine/AnimatorControllerSelfTest.h"
#include "Engine/Engine/AssetDatabaseSelfTest.h"
#include "Engine/Engine/Audio/AudioSelfTest.h"
#include "Engine/Engine/EngineLoop.h"
#include "Engine/Engine/ParticleSelfTest.h"
#include "Engine/Engine/PhysicsSelfTest.h"
#include "Engine/Engine/RayTracing/RtSelfTest.h"
#include "Engine/Engine/Project.h"
#include "Engine/Engine/Replay/Replay.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Replay/SimSnapshotSelfTest.h"
#include "Engine/Engine/Replay/TimeTravelSelfTest.h"
#include "Engine/Engine/Replay/WorldHasherSelfTest.h"
#include "Engine/Engine/SceneSelfTest.h"
#include "Engine/Engine/SkeletonSelfTest.h"
#include "Engine/Engine/FontSelfTest.h"
#include "Engine/Engine/UI/UISelfTest.h"
#include "Engine/Engine/VfxSelfTest.h"
#include "Engine/Engine/Net/NetSelfTest.h"
#include "Engine/Engine/Replay/CrashRingSelfTest.h"
#include "Engine/Platform/CrashHandler.h"
#include "Engine/Platform/InputActionsSelfTest.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/ImageDiff.h"
#include "Engine/Renderer/ImageDiffSelfTest.h"
#include "Engine/Renderer/RenderSelfTest.h"
#include "Engine/Renderer/TextureCookSelfTest.h"

namespace {

// コンソールから起動された場合に標準出力をそのコンソールへ繋ぐ
// (CLI モード --replay-verify (M6) や スモークテストのログ確認用)。
// 既にリダイレクトされている場合 (パイプ/ファイル) は CRT が起動時に束縛済みなので触らない
// — ここで CONOUT$ を開くとリダイレクトを上書きしてしまう。
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

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    AttachParentConsole();

    mye::EngineConfig config;
    config.title = L"MyEngine Editor";
    config.renderSceneToBackbuffer = false; // シーンは SceneView/GameView の RT に描く
    bool selftest = false;
    bool saveSceneOnStart = false;
    bool autoPlay = false;
    float perfRate = 0.0f;
    bool rtShowcase = false; // --rt-demo (M46i)
    bool partsShowcase = false; // --parts-demo (M48g: 部位追従のリプレイ被覆シーン)
    bool flowShowcase = false;  // --flow-demo (M51j: ゲームフロー統合デモ)
    bool localDemo = false;     // --local-demo (M52g: ローカルマルチプレイの入力レーンデモ)
    bool netDemo = false;       // --net-demo (M52i: 2 人ネット対戦のデモ)
    bool renderShowcase = false; // --render-demo (M54a: 描画ロードマップのショーケース)
    bool terrainShowcase = false; // --terrain-demo (M58c: 地形のショーケース)
    std::wstring editActorPath;  // --edit-actor PATH (M48k)
    std::wstring packageDir;     // --package DIR (M51j: CLI パッケージ)
    bool packageDds = false;     // --package-dds
    bool packageZip = false;     // --package-zip
    std::string packageBoot;     // --package-boot <scene.json>
    int perfMax = 0;
    bool startDeferred = false;
    std::string selectName;
    int pickTestFrame = -1;
    std::wstring sceneOverride;
    std::wstring projectDir;              // --project <dir> (M26)
    std::wstring createProjectDir;        // --create-project <dir> (ヘッドレス生成)
    std::wstring templateName = L"empty"; // --template <empty|demo>
    int managerFrames = 0;                // --manager-frames N (Hub を N フレームで自動終了、CI 用)
    std::wstring managerShot;             // --manager-shot <path> (Hub のスクリーンショット)
    std::wstring langOverride;            // --lang <ja|en> (M47a。保存設定と自動化既定の両方に優先)
    std::wstring crashTestArg;            // --crash-test <kind> (M52f)
    std::wstring repDiffA;
    std::wstring repDiffB;
    std::wstring hashDiffA;               // --hash-diff A B (M52a: ダンプ 2 本の差分)
    std::wstring hashDiffB;
    std::wstring imgDiffA;                // --img-diff A B (M52c: スクショ回帰の判定)
    std::wstring imgDiffB;
    std::wstring imgDiffOut;              // --diff-out PNG (差分ヒートマップ)
    int imgTolerance = 0;                 // --tol N (チャンネル差の許容)
    int64_t imgFailPixels = 0;            // --fail-pixels N (許容を超えてよい画素数)

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
            } else if (arg == L"--selftest") {
                selftest = true;
            } else if (arg == L"--save-scene-on-start") {
                saveSceneOnStart = true;
            } else if (arg == L"--autoplay") {
                autoPlay = true;
            } else if (arg == L"--perf-rate" && i + 1 < argc) {
                perfRate = static_cast<float>(_wtof(argv[++i]));
            } else if (arg == L"--perf-max" && i + 1 < argc) {
                perfMax = _wtoi(argv[++i]);
            } else if (arg == L"--replay-record" && i + 1 < argc) {
                config.replayRecordPath = argv[++i];
                autoPlay = true;
                config.vsync = false;
            } else if (arg == L"--replay-verify" && i + 1 < argc) {
                config.replayVerifyPath = argv[++i];
                autoPlay = true;
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
                // M52e: N tick 進めてから複数の K で巻き戻し + 再シムし、ハッシュを照合する。
                // tick 数は省略可 (既定 400)。**--autoplay を一緒に立てる** — エディタは
                // Play 中しか sim を進めないので、これが無いとリングが空のまま終わる
                config.timeTravelProbeTicks = 400;
                if (i + 1 < argc && argv[i + 1][0] != L'-') {
                    config.timeTravelProbeTicks = _wtoi64(argv[++i]);
                }
                autoPlay = true;
                config.vsync = false;
            } else if (arg == L"--crash-test" && i + 1 < argc) {
                // M52f: 意図的に落としてクラッシュバンドルを検証する。
                // ★Play 中でなくても tick は進む (ポーズ tick) ので --autoplay は要らない。
                //   綴り違いは下で弾く (黙って無視すると「落ちない」だけで原因が見えない)
                crashTestArg = argv[++i];
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
                hashDiffA = argv[++i]; // M52a: 2 つのダンプを突き合わせて終了
                hashDiffB = argv[++i];
            } else if (arg == L"--img-diff" && i + 2 < argc) {
                imgDiffA = argv[++i]; // M52c: PNG 2 枚を突き合わせて終了
                imgDiffB = argv[++i];
            } else if (arg == L"--tol" && i + 1 < argc) {
                imgTolerance = _wtoi(argv[++i]);
            } else if (arg == L"--fail-pixels" && i + 1 < argc) {
                imgFailPixels = _wtoi64(argv[++i]);
            } else if (arg == L"--diff-out" && i + 1 < argc) {
                imgDiffOut = argv[++i];
            } else if (arg == L"--font-embedded") {
                config.fontEmbedded = true; // M52c: 撮影のフォントを機種非依存に固定
            } else if (arg == L"--shot-realtime") {
                config.shotRealtime = true; // M52c: 決定的撮影を解除して実時間で回す
            } else if (arg == L"--deferred") {
                startDeferred = true;
            } else if (arg == L"--select" && i + 1 < argc) {
                selectName = mye::WideToUtf8(argv[++i]);
            } else if (arg == L"--pick-test") {
                pickTestFrame = 20;
            } else if (arg == L"--scene" && i + 1 < argc) {
                sceneOverride = argv[++i];
            } else if (arg == L"--postfx-mode" && i + 1 < argc) {
                config.postFxTonemap = _wtoi(argv[++i]); // 0=passthrough 1=ACES 2=Reinhard
            } else if (arg == L"--no-postfx") {
                config.postFx = false;
            } else if (arg == L"--no-audio") {
                config.audio = false; // M45: XAudio2 を初期化しない (端末の無い CI / 撮影専用実行)
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
            } else if (arg == L"--rt-demo") {
                rtShowcase = true; // M46i: コーネル箱のショーケースシーンを構築
            } else if (arg == L"--parts-demo") {
                partsShowcase = true; // M48g: 部位追従の被覆シーンを構築
            } else if (arg == L"--flow-demo") {
                flowShowcase = true; // M51j: ゲームフロー統合デモ (タイトル⇄ゲームの 2 シーン)
            } else if (arg == L"--local-demo") {
                localDemo = true; // M52g: 入力レーンのローカルマルチプレイデモ
            } else if (arg == L"--net-demo") {
                netDemo = true; // M52i: 2 人ネット対戦のデモシーン
            } else if (arg == L"--render-demo") {
                renderShowcase = true; // M54a: 描画ショーケース (局所ライト/反射/フォグ/遠景)
            } else if (arg == L"--terrain-demo") {
                terrainShowcase = true; // M58c: 地形ショーケース (golden demo_terrain_deferred)
            } else if (arg == L"--local-players" && i + 1 < argc) {
                config.localPlayers = _wtoi(argv[++i]); // M52g: 消費する入力レーン数
            } else if (arg == L"--synth-input") {
                config.synthInput = true; // M52g: レーンごとの合成入力 (検証用)
            } else if (arg == L"--edit-actor" && i + 1 < argc) {
                editActorPath = argv[++i]; // M48k: 起動直後にミニシーン編集モードで開く
            } else if (arg == L"--package" && i + 1 < argc) {
                packageDir = argv[++i]; // M51j: BuildSettings パイプラインを CLI 実行
            } else if (arg == L"--package-dds") {
                packageDds = true;
            } else if (arg == L"--package-zip") {
                packageZip = true;
            } else if (arg == L"--package-boot" && i + 1 < argc) {
                packageBoot = mye::WideToUtf8(argv[++i]);
            } else if (arg == L"--project" && i + 1 < argc) {
                projectDir = argv[++i];
            } else if (arg == L"--create-project" && i + 1 < argc) {
                createProjectDir = argv[++i];
            } else if (arg == L"--template" && i + 1 < argc) {
                templateName = argv[++i];
            } else if (arg == L"--manager-frames" && i + 1 < argc) {
                managerFrames = _wtoi(argv[++i]);
            } else if (arg == L"--manager-shot" && i + 1 < argc) {
                managerShot = argv[++i];
            } else if (arg == L"--lang" && i + 1 < argc) {
                langOverride = argv[++i]; // M47a: UI 言語を明示指定 (検証用の A/B)
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

    // 自動化 (CI/検証) 起動かどうか。既存の CI/検証コマンド列 (--frames / --screenshot /
    // --scene / --replay-* 等) は従来のレガシー動作 (リポジトリ assets) を維持する
    const bool automation = config.maxFrames > 0 || !config.screenshotPath.empty()
                            || !config.replayRecordPath.empty() || !config.replayVerifyPath.empty()
                            || !sceneOverride.empty() || autoPlay || saveSceneOnStart
                            || pickTestFrame >= 0 || !selectName.empty() || perfRate > 0.0f
                            || !editActorPath.empty() || !packageDir.empty()
                            || !config.hashDumpPath.empty() || config.crashTest != 0
                            || config.netRole != 0; // M52h: ネット起動もレガシー経路で回す

    // UI 言語 (M47a)。Hub はプロジェクト未確定のまま描かれる別プロセスなので、
    // 設定はプロジェクト配下ではなく %LOCALAPPDATA%\MyEngine\editor_global.json から読む。
    // 自動化/セルフテスト時は環境に依存しないよう英語に固定する (--lang で上書き可)
    if (!langOverride.empty()) {
        mye::SetLanguage(langOverride == L"en" ? mye::Lang::En : mye::Lang::Ja);
    } else if (selftest || automation) {
        mye::SetLanguage(mye::Lang::En);
    } else {
        mye::EditorGlobalSettings globals;
        globals.Load();
        mye::SetLanguage(globals.uiLanguage);
    }

    // --create-project: ヘッドレスでプロジェクトを生成して終了 (M26。検証/CI 用)
    if (!createProjectDir.empty()) {
        const std::wstring dir = std::filesystem::absolute(createProjectDir).wstring();
        const mye::ProjectTemplate tmpl = (templateName == L"demo") ? mye::ProjectTemplate::Demo3D
                                                                    : mye::ProjectTemplate::Empty;
        std::string err;
        const bool ok = mye::CreateProject(dir, std::string(), tmpl, mye::FindAssetsRoot(), &err);
        if (!ok) {
            std::fprintf(stderr, "create-project failed: %s\n", err.c_str());
        }
        return ok ? 0 : 1;
    }

    // --hash-diff A B: ワールドハッシュのフィールド単位ダンプを突き合わせて終了 (M52a)。
    // 同一なら exit 0、1 フィールドでも食い違えば exit 1
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

    // --img-diff A B [--tol N] [--fail-pixels N] [--diff-out PNG]: スクショ回帰の判定 (M52c)。
    // 一致 (許容内) なら exit 0、差があれば exit 1、そもそも比較できなければ exit 2。
    // ★「差が無い」と「比べられなかった」を同じ終了コードにしない — 寸法違いや読み込み失敗を
    //   PASS に混ぜると、撮影が壊れた日に回帰テストが静かに緑になる
    if (!imgDiffA.empty() && !imgDiffB.empty()) {
        const mye::ImageDiffResult r =
            mye::CompareImageFiles(imgDiffA, imgDiffB, imgTolerance, imgDiffOut);
        if (!r.valid) {
            std::fprintf(stderr, "[img-diff] ERROR: %s\n", r.error.c_str());
            return 2;
        }
        const bool pass = r.diffPixels <= imgFailPixels;
        std::printf("[img-diff] %s: %dx%d maxDiff=%d diffPixels=%lld (tol=%d, allow=%lld) "
                    "anyDiff=%lld/%lld\n",
                    pass ? "PASS" : "FAIL", r.width, r.height, r.maxChannelDiff,
                    static_cast<long long>(r.diffPixels), imgTolerance,
                    static_cast<long long>(imgFailPixels),
                    static_cast<long long>(r.diffPixelsAny),
                    static_cast<long long>(r.totalPixels));
        if (!pass) {
            std::printf("[img-diff]   worst pixel at (%d, %d)\n", r.worstX, r.worstY);
            std::printf("[img-diff]   A = %s\n", mye::WideToUtf8(imgDiffA).c_str());
            std::printf("[img-diff]   B = %s\n", mye::WideToUtf8(imgDiffB).c_str());
            if (!imgDiffOut.empty()) {
                std::printf("[img-diff]   heat map = %s\n", mye::WideToUtf8(imgDiffOut).c_str());
            }
        }
        return pass ? 0 : 1;
    }

    if (selftest) {
        // ウィンドウ/D3D 不要のヘッドレス回帰テスト
        const bool ok = mye::RunEcsSelfTest() && mye::RunSceneSerializerSelfTest()
            && mye::RunUndoSelfTest() && mye::RunRenderSelfTest() && mye::RunPhysicsSelfTest()
            && mye::RunUISelfTest() && mye::RunAnimatorControllerSelfTest()
            && mye::RunAssetDatabaseSelfTest() && mye::RunTextureCookSelfTest()
            && mye::RunJobSystemSelfTest() && mye::RunVfxSelfTest()
            && mye::RunParticleSelfTest() && mye::RunAssetOpsSelfTest()
            && mye::RunFontSelfTest() && mye::RunAudioSelfTest() && mye::RunRtSelfTest()
            && mye::RunLocalizationSelfTest() && mye::RunSkeletonSelfTest()
            && mye::RunPartSelfTest() && mye::RunSchemaSelfTest()
            && mye::RunCookedCacheSelfTest() && mye::RunInputActionsSelfTest()
            && mye::RunGameFlowSelfTest() && mye::RunWorldHasherSelfTest()
            && mye::RunSimSnapshotSelfTest() && mye::RunTimeTravelSelfTest()
            && mye::RunCrashRingSelfTest() && mye::RunImageDiffSelfTest()
            && mye::RunNetSelfTest()
            // M58b: 連鎖の**末尾** (統合契約の予約 7)。短絡なので位置がそのまま実行順
            && mye::RunTerrainSelfTest();
        return ok ? 0 : 1;
    }

    // 裸起動 (プロジェクト未指定 + 自動化フラグなし) はプロジェクトマネージャへ (M26b)
    if (managerFrames > 0 || (projectDir.empty() && !automation)) {
        const mye::ProjectManagerOutcome outcome = mye::RunProjectManager(managerFrames, managerShot);
        if (outcome.action == mye::ProjectManagerAction::OpenProject) {
            mye::RelaunchSelfWithProject(outcome.projectRoot);
        }
        return 0;
    }

    // --project: プロジェクトを検証して注入 (M26)。失敗はダイアログ + exit 1
    if (!projectDir.empty()) {
        const std::wstring dir = std::filesystem::absolute(projectDir).wstring();
        mye::ProjectManifest manifest;
        if (!mye::IsProjectRoot(dir) || !mye::LoadProjectManifest(dir, manifest)) {
            std::fprintf(stderr, "invalid project: %s\n", mye::WideToUtf8(dir).c_str());
            MessageBoxW(nullptr, (L"プロジェクトが見つかりません:\n" + dir).c_str(),
                        L"MyEngine Editor", MB_ICONERROR | MB_OK);
            return 1;
        }
        config.projectRoot = dir;
        if (!manifest.name.empty()) {
            config.title += L" - " + mye::Utf8ToWide(manifest.name);
        }
        mye::ProjectRegistry registry;
        registry.Load();
        registry.Touch(dir, manifest.name);
    }

    mye::EditorApp app;
    app.saveSceneOnStart = saveSceneOnStart;
    app.autoPlay = autoPlay;
    app.rtShowcase = rtShowcase;
    app.partsShowcase = partsShowcase;
    app.flowShowcase = flowShowcase;
    app.localDemo = localDemo;
    app.netDemo = netDemo;
    app.renderShowcase = renderShowcase;
    app.terrainShowcase = terrainShowcase; // M58c
    app.editActorPath = editActorPath;
    app.packageDir = packageDir;
    app.packageDds = packageDds;
    app.packageZip = packageZip;
    app.packageBoot = packageBoot;
    app.perfRate = perfRate;
    app.perfMax = perfMax;
    app.startDeferred = startDeferred;
    app.selectName = selectName;
    app.pickTestFrame = pickTestFrame;
    app.sceneOverride = sceneOverride;
    mye::EngineLoop loop;
    const int rc = loop.Run(config, app);
    // --package の成否は終了コードへ載せる (M52b。エンジン自体の失敗が優先)
    return rc != 0 ? rc : app.packageExitCode;
}
