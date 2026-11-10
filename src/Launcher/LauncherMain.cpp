//====================================================================================
//                          LauncherMain.cpp
//  MyEngine/ 秋田蓮音                                                      11/10/2026
//                                          MyEngineLauncher.exe の入口 (引数・単一起動・描画ループ)
//====================================================================================
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <Windows.h>
#include <shellapi.h>

#include "nlohmann/json.hpp"

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Localization/Localization.h"
#include "Engine/Platform/CrashHandler.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Platform/Win32Window.h"
#include "Engine/Renderer/Device/GraphicsDevice.h"
#include "Engine/Renderer/Device/SwapChain.h"
#include "Engine/Renderer/ImGui/ImGuiRenderer.h"
#include "Engine/Renderer/ImGui/ImGuiTheme.h"
#include "Launcher/LauncherSelf.h"
#include "Launcher/LauncherSelfTest.h"
#include "Launcher/LauncherUi.h"

#include "imgui.h"

namespace {

constexpr int kWindowWidth = 820;
constexpr int kWindowHeight = 560;
// 単一起動の印。2 つ目の Launcher は残骸の片付けだけを飛ばす (相手が展開中の staging を消さないため)
constexpr const wchar_t* kSingleInstanceMutex = L"Local\\MyEngineLauncher";

// EditorMain と同じ: コンソールから起動されたら (--selftest の結果を見るため) 標準出力を繋ぐ
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

// エディタと同じ言語で出す (%LOCALAPPDATA%\MyEngine\editor_global.json の uiLanguage)。
// Launcher は Editor 層に依存しないので、キー 1 つだけを直接読む
mye::Lang LoadUiLanguage()
{
    wchar_t* localAppData = nullptr;
    size_t len = 0;
    std::wstring base;
    if (_wdupenv_s(&localAppData, &len, L"LOCALAPPDATA") == 0 && localAppData) {
        base = localAppData;
        free(localAppData);
    }
    std::ifstream f(std::filesystem::path(base + L"\\MyEngine\\editor_global.json"), std::ios::binary);
    if (!f) {
        return mye::Lang::Ja;
    }
    const nlohmann::json root = nlohmann::json::parse(f, nullptr, false);
    if (!root.is_object()) {
        return mye::Lang::Ja;
    }
    return mye::LangFromString(root.value("uiLanguage", std::string("ja")).c_str());
}

struct LauncherArgs
{
    bool selftest = false;
    bool update = false;
    bool includePrerelease = false;
    bool warp = false;
    int frames = 0;
    std::wstring launchProject;
    std::wstring releaseSource;
    std::wstring installRoot;
    std::wstring screenshot;
    std::wstring lang;
};

LauncherArgs ParseArgs()
{
    LauncherArgs a;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        const std::wstring arg = argv[i];
        const bool hasNext = i + 1 < argc;
        if (arg == L"--selftest") {
            a.selftest = true;
        } else if (arg == L"--update") {
            a.update = true;
        } else if (arg == L"--include-prerelease") {
            a.includePrerelease = true;
        } else if (arg == L"--warp") {
            a.warp = true;
        } else if (arg == L"--launch-project" && hasNext) {
            a.launchProject = argv[++i];
        } else if (arg == L"--release-source" && hasNext) {
            a.releaseSource = std::filesystem::absolute(argv[++i]).wstring();
        } else if (arg == L"--install-root" && hasNext) {
            a.installRoot = std::filesystem::absolute(argv[++i]).wstring();
        } else if (arg == L"--screenshot" && hasNext) {
            a.screenshot = std::filesystem::absolute(argv[++i]).wstring();
        } else if (arg == L"--frames" && hasNext) {
            a.frames = _wtoi(argv[++i]);
        } else if (arg == L"--lang" && hasNext) {
            a.lang = argv[++i];
        } else {
            MYE_LOG_WARN("launcher: unknown argument %s", mye::WideToUtf8(arg).c_str());
        }
    }
    LocalFree(argv);
    return a;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    AttachParentConsole();
    mye::SuppressCrtDialogs();
    const LauncherArgs args = ParseArgs();

    if (args.selftest) {
        mye::SetLanguage(mye::Lang::En);
        return mye::RunLauncherSelfTest() ? 0 : 1;
    }
    if (!args.lang.empty()) {
        mye::SetLanguage(args.lang == L"en" ? mye::Lang::En : mye::Lang::Ja);
    } else {
        mye::SetLanguage(LoadUiLanguage());
    }

    mye::LauncherState st;
    st.options.source = mye::ResolveReleaseSource(args.releaseSource);
    st.options.includePrerelease = args.includePrerelease;
    st.options.root = args.installRoot.empty() ? mye::InstallRoot() : args.installRoot;
    st.options.updateMode = args.update;
    st.options.launchProject = args.launchProject;

    const HANDLE instanceMutex = CreateMutexW(nullptr, FALSE, kSingleInstanceMutex);
    const bool firstInstance = instanceMutex && GetLastError() != ERROR_ALREADY_EXISTS;
    const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); // ShellExecute / ショートカット用
    if (firstInstance) {
        mye::CleanupStaleInstallWork(st.options.root);
        mye::CleanupOldLauncherFiles(mye::RunningLauncherPath());
        // 自分は複製しない (ADR-037 D7)。エディタの「更新して再起動」が辿れるよう、置かれた場所だけを記録する
        mye::RecordLauncherLocation(st.options.root, mye::RunningLauncherPath());
    }
    mye::Win32Window window;
    mye::GraphicsDevice device;
    mye::SwapChain swapChain;
    mye::ImGuiRenderer imgui;
    mye::WindowDesc wd;
    wd.title = L"MyEngine Launcher";
    wd.width = kWindowWidth;
    wd.height = kWindowHeight;
    int exitCode = 0;
    if (!window.Create(wd) || !device.Init(args.warp)
        || !swapChain.Init(device, window.Hwnd(), window.Width(), window.Height())) {
        MessageBoxW(nullptr, L"MyEngine Launcher: Direct3D 11 could not be initialized.", L"MyEngine Launcher",
                    MB_ICONERROR | MB_OK);
        exitCode = 1;
    } else {
        mye::ImGuiInitOptions imguiOpts;
        imguiOpts.disableIni = true;
        imguiOpts.dpiScale = args.screenshot.empty() ? window.DpiScale() : 1.0f;
        if (imgui.Init(window, device, imguiOpts)) {
            mye::LauncherBegin(st);
            int frame = 0;
            bool running = true;
            while (running) {
                if (!window.PumpMessages()) {
                    break;
                }
                if (window.ConsumeResize()) {
                    swapChain.Resize(window.Width(), window.Height());
                }
                running = mye::LauncherTick(st);
                if (window.IsMinimized()) {
                    Sleep(10);
                    continue;
                }
                const ImVec4& bg = mye::themeColor::BgApp;
                const float clear[4] = { bg.x, bg.y, bg.z, 1.0f };
                swapChain.BindAndClearBackbuffer(clear);
                imgui.BeginFrame();
                running = mye::DrawLauncherUi(st) && running;
                imgui.EndFrame();
                ++frame;
                const bool lastTestFrame = args.frames > 0 && frame >= args.frames;
                if (!args.screenshot.empty() && (lastTestFrame || !running)) {
                    swapChain.SaveBackbufferPng(args.screenshot);
                }
                swapChain.Present(true);
                if (lastTestFrame) {
                    running = false;
                }
            }
            imgui.Shutdown();
        } else {
            exitCode = 1;
        }
    }
    st.job.Cancel();
    swapChain.Shutdown();
    window.Destroy();
    if (coInit == S_OK || coInit == S_FALSE) {
        CoUninitialize();
    }
    if (instanceMutex) {
        CloseHandle(instanceMutex);
    }
    return exitCode;
}
