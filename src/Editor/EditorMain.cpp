#include <cstdio>
#include <cstdlib>
#include <string>

#include <Windows.h>
#include <shellapi.h>

#include "Editor/EditorApp.h"
#include "Editor/UndoSelfTest.h"
#include "Engine/Core/EcsSelfTest.h"
#include "Engine/Core/JobSystemSelfTest.h"
#include "Engine/Core/Log.h"
#include "Engine/Engine/AnimatorControllerSelfTest.h"
#include "Engine/Engine/AssetDatabaseSelfTest.h"
#include "Engine/Engine/EngineLoop.h"
#include "Engine/Engine/PhysicsSelfTest.h"
#include "Engine/Engine/SceneSelfTest.h"
#include "Engine/Engine/UI/UISelfTest.h"
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
    int perfMax = 0;
    bool startDeferred = false;
    std::string selectName;
    int pickTestFrame = -1;
    std::wstring sceneOverride;

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
            }
        }
        LocalFree(argv);
    }

    if (selftest) {
        // ウィンドウ/D3D 不要のヘッドレス回帰テスト
        const bool ok = mye::RunEcsSelfTest() && mye::RunSceneSerializerSelfTest()
            && mye::RunUndoSelfTest() && mye::RunRenderSelfTest() && mye::RunPhysicsSelfTest()
            && mye::RunUISelfTest() && mye::RunAnimatorControllerSelfTest()
            && mye::RunAssetDatabaseSelfTest() && mye::RunTextureCookSelfTest()
            && mye::RunJobSystemSelfTest();
        return ok ? 0 : 1;
    }

    mye::EditorApp app;
    app.saveSceneOnStart = saveSceneOnStart;
    app.autoPlay = autoPlay;
    app.perfRate = perfRate;
    app.perfMax = perfMax;
    app.startDeferred = startDeferred;
    app.selectName = selectName;
    app.pickTestFrame = pickTestFrame;
    app.sceneOverride = sceneOverride;
    mye::EngineLoop loop;
    return loop.Run(config, app);
}
