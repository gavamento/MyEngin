#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <Windows.h>
#include <shellapi.h>

#include "Editor/EditorApp.h"
#include "Editor/EditorGlobalSettings.h"
#include "Editor/PartSelfTest.h"
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
#include "Engine/Engine/AnimatorControllerSelfTest.h"
#include "Engine/Engine/AssetDatabaseSelfTest.h"
#include "Engine/Engine/Audio/AudioSelfTest.h"
#include "Engine/Engine/EngineLoop.h"
#include "Engine/Engine/ParticleSelfTest.h"
#include "Engine/Engine/PhysicsSelfTest.h"
#include "Engine/Engine/RayTracing/RtSelfTest.h"
#include "Engine/Engine/Project.h"
#include "Engine/Engine/SceneSelfTest.h"
#include "Engine/Engine/SkeletonSelfTest.h"
#include "Engine/Engine/FontSelfTest.h"
#include "Engine/Engine/UI/UISelfTest.h"
#include "Engine/Engine/VfxSelfTest.h"
#include "Engine/Platform/PathUtil.h"
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
    std::wstring editActorPath;  // --edit-actor PATH (M48k)
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
            } else if (arg == L"--edit-actor" && i + 1 < argc) {
                editActorPath = argv[++i]; // M48k: 起動直後にミニシーン編集モードで開く
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

    // 自動化 (CI/検証) 起動かどうか。既存の CI/検証コマンド列 (--frames / --screenshot /
    // --scene / --replay-* 等) は従来のレガシー動作 (リポジトリ assets) を維持する
    const bool automation = config.maxFrames > 0 || !config.screenshotPath.empty()
                            || !config.replayRecordPath.empty() || !config.replayVerifyPath.empty()
                            || !sceneOverride.empty() || autoPlay || saveSceneOnStart
                            || pickTestFrame >= 0 || !selectName.empty() || perfRate > 0.0f
                            || !editActorPath.empty();

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
            && mye::RunPartSelfTest() && mye::RunSchemaSelfTest();
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
    app.editActorPath = editActorPath;
    app.perfRate = perfRate;
    app.perfMax = perfMax;
    app.startDeferred = startDeferred;
    app.selectName = selectName;
    app.pickTestFrame = pickTestFrame;
    app.sceneOverride = sceneOverride;
    mye::EngineLoop loop;
    return loop.Run(config, app);
}
