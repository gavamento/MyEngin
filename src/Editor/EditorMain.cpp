#include <cstdio>
#include <cstdlib>
#include <string>

#include <Windows.h>
#include <shellapi.h>

#include "Editor/EditorApp.h"
#include "Engine/Core/EcsSelfTest.h"
#include "Engine/Core/Log.h"
#include "Engine/Engine/EngineLoop.h"
#include "Engine/Engine/SceneSelfTest.h"

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
            }
        }
        LocalFree(argv);
    }

    if (selftest) {
        // ウィンドウ/D3D 不要のヘッドレス回帰テスト
        const bool ok = mye::RunEcsSelfTest() && mye::RunSceneSerializerSelfTest();
        return ok ? 0 : 1;
    }

    mye::EditorApp app;
    app.saveSceneOnStart = saveSceneOnStart;
    app.autoPlay = autoPlay;
    mye::EngineLoop loop;
    return loop.Run(config, app);
}
