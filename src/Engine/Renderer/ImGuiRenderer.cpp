#include "Engine/Renderer/ImGuiRenderer.h"

#include "Engine/Core/Localization.h"
#include "Engine/Core/Log.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Platform/Win32Window.h"
#include "Engine/Renderer/GraphicsDevice.h"
#include "Engine/Renderer/ImGuiTheme.h"

#include <Windows.h>

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

// imgui_impl_win32.h のコメントで指示されている前方宣言
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace mye {

bool ImGuiRenderer::Init(Win32Window& window, GraphicsDevice& device, const ImGuiInitOptions& opts)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // multi-viewport は初期スコープ外 (DX11 との組み合わせで工数が膨らむため封印)

    // ini の置き場 (M26): プロジェクト起動時は <project>\.mye\imgui.ini に固定して CWD 揺れを防ぐ
    if (opts.disableIni) {
        io.IniFilename = nullptr;
    } else if (!opts.iniPath.empty()) {
        iniPathUtf8_ = WideToUtf8(opts.iniPath);
        io.IniFilename = iniPathUtf8_.c_str();
    }

    ImGui::StyleColorsDark();
    ApplyEditorTheme(ImGui::GetStyle()); // テーマ第 3 世代 (M27a の UE5 風を置き換え)
    // M47a: 日本語グリフが無い環境 (英語版 Windows の最小構成など) で日本語 UI にすると
    // 画面全体が豆腐になるので、フォント側の実情に合わせて英語へ落とす
    if (!SetupEditorFonts() && CurrentLanguage() == Lang::Ja) {
        MYE_LOG_WARN("no Japanese font available - falling back to English UI");
        SetLanguage(Lang::En);
    }
    // 高 DPI (テーマ第 3 世代): PerMonitorV2 なので OS は拡大してくれない。スタイル寸法は
    // ここで実寸へ、フォントは FontScaleDpi (1.92 の動的アトラスが実寸で再ラスタライズする
    // = ビットマップ拡大のにじみは出ない)。★ScaleAllSizes はテーマ適用より後に呼ぶこと
    if (opts.dpiScale > 1.0f) {
        ImGui::GetStyle().ScaleAllSizes(opts.dpiScale);
        ImGui::GetStyle().FontScaleDpi = opts.dpiScale;
    }

    if (!ImGui_ImplWin32_Init(window.Hwnd())) {
        MYE_LOG_ERROR("ImGui_ImplWin32_Init failed");
        return false;
    }
    if (!ImGui_ImplDX11_Init(device.Device(), device.Context())) {
        MYE_LOG_ERROR("ImGui_ImplDX11_Init failed");
        ImGui_ImplWin32_Shutdown();
        return false;
    }

    window.AddMsgHandler([](void* hwnd, uint32_t msg, uint64_t wparam, int64_t lparam, int64_t& result) {
        const LRESULT r = ImGui_ImplWin32_WndProcHandler(
            static_cast<HWND>(hwnd), msg, static_cast<WPARAM>(wparam), static_cast<LPARAM>(lparam));
        if (r != 0) {
            result = r;
            return true;
        }
        return false;
    });

    initialized_ = true;
    MYE_LOG_INFO("ImGui initialized (%s, docking)", IMGUI_VERSION);
    return true;
}

void ImGuiRenderer::Shutdown()
{
    if (!initialized_) {
        return;
    }
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    initialized_ = false;
}

void ImGuiRenderer::BeginFrame()
{
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

void ImGuiRenderer::EndFrame()
{
    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

} // namespace mye
