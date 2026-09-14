#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <Windows.h>
#include <shellapi.h>

#include "Editor/EditorApp.h"
#include "Editor/EditorGlobalSettings.h"
#include "Editor/GameFlowSelfTest.h"
#include "Editor/PartSelfTest.h"
#include "Editor/RagdollBuildSelfTest.h"
#include "Engine/Engine/Asset/CookedCacheSelfTest.h"
#include "Engine/Engine/Asset/SubAssetKeySelfTest.h"
#include "Engine/Engine/Asset/SubAssetMigration.h"
#include "Engine/Engine/UI/UIFontMetricsCook.h" // M75d: --cook-font-metrics
#include "Engine/Engine/SchemaSelfTest.h"
#include "Editor/ProjectManager.h"
#include "Editor/ProjectRegistry.h"
#include "Editor/ProjectTemplates.h"
#include "Editor/LightSelectionSelfTest.h"
#include "Editor/UndoSelfTest.h"
#include "Engine/Core/EcsSelfTest.h"
#include "Engine/Core/JobSystemSelfTest.h"
#include "Engine/Core/Localization.h"
#include "Engine/Core/LocalizationSelfTest.h"
#include "Engine/Core/Log.h"
#include "Editor/AssetOpsSelfTest.h"
#include "Editor/TerrainSelfTest.h"
#include "Editor/DecalSelfTest.h"
#include "Editor/HzbSelfTest.h"
#include "Editor/SsrSelfTest.h"
#include "Editor/SourceControl/SourceControlSelfTest.h"
#include "Editor/ProbeBakerSelfTest.h"
#include "Editor/CameraPilotSelfTest.h"
#include "Engine/Engine/AnimatorControllerSelfTest.h"
#include "Engine/Engine/AssetDatabaseSelfTest.h"
#include "Engine/Engine/Audio/AudioSelfTest.h"
#include "Engine/Engine/EngineLoop.h"
#include "Engine/Engine/ParticleSelfTest.h"
#include "Engine/Engine/Physics/ConvexSelfTest.h"
#include "Engine/Engine/Physics/RagdollSelfTest.h"
#include "Engine/Engine/Physics/XpbdSelfTest.h"
#include "Engine/Engine/Physics/PhysMatSelfTest.h"
#include "Engine/Engine/PhysicsSelfTest.h"
#include "Engine/Engine/RayTracing/RtSelfTest.h"
#include "Engine/Engine/Project.h"
#include "Engine/Engine/Replay/Replay.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Acoustic/AcousticSelfTest.h"
#include "Engine/Engine/Audio/AcousticAudioSelfTest.h"
#include "Engine/Engine/Audio/ImpactSynthSelfTest.h"
#include "Engine/Engine/Replay/SimSnapshotSelfTest.h"
#include "Engine/Engine/Replay/TimeTravelSelfTest.h"
#include "Engine/Engine/Replay/WorldHasherSelfTest.h"
#include "Engine/Engine/SceneSelfTest.h"
#include "Engine/Engine/SkeletonSelfTest.h"
#include "Engine/Engine/FontSelfTest.h"
#include "Engine/Engine/UI/UISelfTest.h"
#include "Engine/Engine/VfxSelfTest.h"
#include "Engine/Engine/Net/NetSelfTest.h"
#include "Engine/Engine/HotReload/DllReloaderSelfTest.h"
#include "Engine/Engine/HotReload/ReloadHubSelfTest.h"
#include "Engine/Engine/EngineCli.h"
#include "Engine/Engine/EngineCliSelfTest.h"
#include "Engine/Engine/ShowcaseScenes.h"
#include "Engine/Engine/Replay/CrashRingSelfTest.h"
#include "Engine/Platform/CrashHandler.h"
#include "Engine/Platform/InputActionsSelfTest.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/ImageDiff.h"
#include "Engine/Renderer/ImageDiffSelfTest.h"
#include "Engine/Renderer/RenderSelfTest.h"
#include "Engine/Renderer/TextureCookSelfTest.h"
#include "Engine/Renderer/VolumeTexture.h"

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
    bool openTimeline = false; // M72c
    float perfRate = 0.0f;
    const mye::ShowcaseDef* showcase = nullptr; // --*-demo (ShowcaseScenes.h。複数なら表の上の行)
    mye::ShowcaseOptions showcaseOptions;       // --terrain-lod DIST / --terrain-skirt D (M58e)
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
    mye::EngineCliExtras cli; // --crash-test / --rep-diff / --hash-diff (Runtime と共通の CLI。EngineCli.h)
    std::wstring imgDiffA;                // --img-diff A B (M52c: スクショ回帰の判定)
    std::wstring imgDiffB;
    std::wstring imgDiffOut;              // --diff-out PNG (差分ヒートマップ)
    int imgTolerance = 0;                 // --tol N (チャンネル差の許容)
    int64_t imgFailPixels = 0;            // --fail-pixels N (許容を超えてよい画素数)
    int froxelProbeIters = 0;             // --froxel-probe [N] (M57a: 3D テクスチャの実測)
    bool migrateSubAssetIds = false;      // --migrate-subasset-ids (M74b: 旧 ID → guid:// の ID)
    std::vector<std::wstring> legacyRoots; // --legacy-root DIR (繰り返し可。旧 clone 先)
    bool migrateDryRun = false;           // --dry-run (数えるだけで書かない)
    bool cookFontMetrics = false;         // --cook-font-metrics (M75d: フォント計測表を作る)

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            const std::wstring arg = argv[i];
            // Runtime と同じ意味のフラグ (ウィンドウ / 撮影 / リプレイ / クラッシュ / ネット / 描画 / RT / 音響) は
            // 表で読む (EngineCli.cpp)。この下に残すのは Editor だけのフラグ
            const mye::CliParse shared = mye::ParseEngineCliFlag(argc, argv, i, config, cli);
            if (shared == mye::CliParse::Error) {
                return 1;
            }
            if (shared == mye::CliParse::Consumed) {
                // ★Editor は Play 中しか sim を進めないので、sim を回して検証するフラグは Play も一緒に立てる
                //   (Runtime は常に sim を進めるので要らない)
                if (arg == L"--replay-record" || arg == L"--replay-verify" || arg == L"--timetravel-selftest") {
                    autoPlay = true;
                } else if (arg == L"--whatif-selftest") {
                    autoPlay = true;
                    openTimeline = true;
                }
                continue;
            }
            if (arg == L"--selftest") {
                selftest = true;
            } else if (arg == L"--cook-font-metrics") {
                cookFontMetrics = true;
            } else if (arg == L"--migrate-subasset-ids") {
                migrateSubAssetIds = true;
            } else if (arg == L"--legacy-root" && i + 1 < argc) {
                legacyRoots.emplace_back(argv[++i]);
            } else if (arg == L"--dry-run") {
                migrateDryRun = true;
            } else if (arg == L"--save-scene-on-start") {
                saveSceneOnStart = true;
            } else if (arg == L"--autoplay") {
                autoPlay = true;
            } else if (arg == L"--perf-rate" && i + 1 < argc) {
                perfRate = static_cast<float>(_wtof(argv[++i]));
            } else if (arg == L"--perf-max" && i + 1 < argc) {
                perfMax = _wtoi(argv[++i]);
            } else if (arg == L"--img-diff" && i + 2 < argc) {
                imgDiffA = argv[++i]; // M52c: PNG 2 枚を突き合わせて終了
                imgDiffB = argv[++i];
            } else if (arg == L"--tol" && i + 1 < argc) {
                imgTolerance = _wtoi(argv[++i]);
            } else if (arg == L"--fail-pixels" && i + 1 < argc) {
                imgFailPixels = _wtoi64(argv[++i]);
            } else if (arg == L"--diff-out" && i + 1 < argc) {
                imgDiffOut = argv[++i];
            } else if (arg == L"--froxel-probe") {
                // M57a: フロクセルの 3D テクスチャ基盤を裸の D3D デバイスだけで計測する。
                // 回数は省略可 (既定 64)。--warp と組み合わせて CI と同じ絵の環境で測る
                froxelProbeIters = 64;
                if (i + 1 < argc && argv[i + 1][0] != L'-') {
                    froxelProbeIters = _wtoi(argv[++i]);
                }
            } else if (arg == L"--deferred") {
                startDeferred = true;
            } else if (arg == L"--select" && i + 1 < argc) {
                selectName = mye::WideToUtf8(argv[++i]);
            } else if (arg == L"--pick-test") {
                pickTestFrame = 20;
            } else if (arg == L"--scene" && i + 1 < argc) {
                sceneOverride = argv[++i];
            } else if (const mye::ShowcaseDef* s = mye::FindShowcase(arg, /*editor=*/true)) {
                showcase = mye::PickShowcase(showcase, s); // --*-demo (ShowcaseScenes.cpp の表)
            } else if (arg == L"--terrain-lod" && i + 1 < argc) {
                // M58e: 地形 LOD の切替距離。**golden は LOD 無しのまま**で、
                // クラック A/B のときだけ点ける
                showcaseOptions.terrainLodDistance = static_cast<float>(_wtof(argv[++i]));
            } else if (arg == L"--terrain-skirt" && i + 1 < argc) {
                showcaseOptions.terrainSkirtDepth = static_cast<float>(_wtof(argv[++i])); // M58e (負値 = 無し)
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

    // --froxel-probe [N] [--warp]: フロクセル用 3D テクスチャの実測 (M57a)。
    // ウィンドウも sim も作らず、D3D デバイス + ShaderManager だけで
    // 「typed 3D UAV が本当に書けるか」と「空の CS 1 回の壁時計」を出して終了する。
    // 数字が設計の入力になるので、実装より先にこれを回すのが M57a の手順そのもの。
    // exit 0 = 全候補で UAV ストア一致 / 1 = 食い違い / 2 = 計測に至らなかった
    if (froxelProbeIters > 0) {
        mye::FroxelProbeOptions probe;
        probe.forceWarp = config.forceWarp;
        probe.iterations = froxelProbeIters;
        // シェーダは EngineLoop と同じ 2 ルート解決 (プロジェクト側 → エンジン組込み)。
        // 裸起動では両者が同一になるので 1 本に畳む
        const std::wstring assetsRoot = mye::FindAssetsRoot();
        if (!assetsRoot.empty()) {
            probe.shaderDirs.push_back(assetsRoot + L"\\shaders");
        }
        const std::wstring engineShaders = mye::FindEngineShaderDir();
        if (!engineShaders.empty()
            && (probe.shaderDirs.empty()
                || mye::NormalizePathKey(engineShaders)
                    != mye::NormalizePathKey(probe.shaderDirs.front()))) {
            probe.shaderDirs.push_back(engineShaders);
        }
        return mye::RunFroxelVolumeProbe(probe);
    }

    // --cook-font-metrics [--project DIR] (M75d):
    // 描画フォント (assets\fonts\*.ttf/.ttc の名前順の先頭) の送り幅を
    // assets\fonts\<stem>.fontmetrics.json へ書いて終了する (ウィンドウも D3D も作らない)。
    // 生成物は cache ではなく assets = **コミットする** (sim が読む入力なので全員が同じ表を持つ)。
    // --project 無しはエンジンリポジトリの assets が対象。exit 0 = 書いた / 最新、1 = 失敗、2 = フォントなし
    if (cookFontMetrics) {
        const std::wstring assetsRoot = projectDir.empty()
            ? mye::FindAssetsRoot()
            : (std::filesystem::absolute(projectDir) / L"assets").wstring();
        return mye::uitext::RunFontMetricsCookCli(assetsRoot);
    }

    // --migrate-subasset-ids [--project DIR] [--legacy-root OLD]... [--dry-run] (M74b):
    // M74a 以前のサブアセット ID (正規化絶対パス由来) を guid:// 由来の ID へ書き換えて終了する。
    // ウィンドウも D3D も作らない (モデルはヘッドレス登録で登録名だけ揃える)。
    // --legacy-root は「そのシーンを保存したマシンの clone 先 = プロジェクトルート」。現在の
    // clone 先は自動で含まれる。--project 無しはエンジンリポジトリの assets が対象
    if (migrateSubAssetIds) {
        const std::wstring assetsRoot = projectDir.empty()
            ? mye::FindAssetsRoot()
            : (std::filesystem::absolute(projectDir) / L"assets").wstring();
        return mye::subasset::RunMigration(assetsRoot, legacyRoots, migrateDryRun);
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
            // 連鎖の**末尾**に append する (統合契約の予約 7)。短絡なので位置がそのまま実行順
            && mye::RunLightSelectionSelfTest() // M54b
            && mye::RunTerrainSelfTest()        // M58b
            && mye::RunDecalSelfTest()          // M56a
            && mye::RunHzbSelfTest()            // M56c
            && mye::RunSsrSelfTest()            // M56d
            && mye::RunProbeBakerSelfTest()     // M56e
            && mye::RunPhysMatSelfTest()        // M59a1
            && mye::RunConvexSelfTest()         // M60f
            && mye::RunRagdollSelfTest()        // M60g1
            && mye::RunRagdollBuildSelfTest()   // M60g2
            && mye::RunCameraPilotSelfTest()    // カメラ操縦 (視錐台 UI 追補)
            && mye::RunDllReloaderSelfTest()    // DLL 書き込み完了プローブ (M52h 追補)
            && mye::RunXpbdSelfTest()           // M60'b
            && mye::RunAcousticSelfTest()       // M65a
            && mye::RunSourceControlSelfTest()  // M66a
            && mye::RunAcousticAudioSelfTest()  // M68a
            && mye::RunSubAssetKeySelfTest()    // M74a / M74b
            && mye::RunImpactSynthSelfTest()    // ImpactSynth (計画 ImpactSoundDesign)
            && mye::RunReloadHubSelfTest()      // ホットリロードの資産の種類表
            && mye::RunEngineCliSelfTest();     // 両 Main 共通の CLI フラグ表
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
    app.openTimeline = openTimeline;
    app.showcase = showcase;
    app.showcaseOptions = showcaseOptions;
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
