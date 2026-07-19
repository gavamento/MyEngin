#include <cstdio>
#include <cstdlib>
#include <string>

#include <Windows.h>
#include <shellapi.h>

#include "Engine/Core/Log.h"
#include "Engine/Engine/EngineLoop.h"

#include "imgui.h"

namespace {

class EditorApp : public mye::IEngineApp {
public:
    void OnStart(mye::EngineContext&) override
    {
        MYE_LOG_INFO("EditorApp started");
    }

    void OnImGui(mye::EngineContext& ctx) override
    {
        // M2 で dockspace + 各ウィンドウに置き換える。M0 は動作確認用の最小 UI
        ImGui::ShowDemoWindow(&showDemo_);

        if (ImGui::Begin("Engine Stats")) {
            const ImGuiIO& io = ImGui::GetIO();
            ImGui::Text("FPS: %.1f (%.3f ms)", io.Framerate, 1000.0f / io.Framerate);
            ImGui::Text("Frame: %llu", static_cast<unsigned long long>(ctx.frameIndex));
            ImGui::Text("Tick:  %llu", static_cast<unsigned long long>(ctx.tickIndex));
            ImGui::Text("Mouse: (%d, %d)", ctx.input.mouseX, ctx.input.mouseY);
        }
        ImGui::End();
    }

private:
    bool showDemo_ = true;
};

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
            }
        }
        LocalFree(argv);
    }

    EditorApp app;
    mye::EngineLoop loop;
    return loop.Run(config, app);
}
