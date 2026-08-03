#include "Engine/Renderer/ImGuiTheme.h"

#include <filesystem>
#include <string>

#include <Windows.h>

#include "Engine/Core/Log.h"
#include "Engine/Platform/PathUtil.h"

#include "imgui.h"

#include "fontawesome/IconsFontAwesome6.h"
#include "fontawesome/fa_solid_900.h"

namespace mye {

namespace themeColor {
const ImVec4 Accent = ImVec4(0.00f, 0.44f, 0.88f, 1.00f);
const ImVec4 PlayAccent = ImVec4(0.85f, 0.45f, 0.10f, 1.00f);
} // namespace themeColor

void ApplyEditorTheme(ImGuiStyle& style)
{
    // UE5 風: ほぼ黒 (#0D0D0E〜#1A1A1C) のフラット面 + 白文字 + 青アクセント
    const ImVec4 accent = themeColor::Accent;
    const ImVec4 accentDim = ImVec4(accent.x * 0.7f, accent.y * 0.7f, accent.z * 0.7f, 1.0f);
    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = ImVec4(0.92f, 0.92f, 0.92f, 1.00f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.48f, 0.48f, 0.50f, 1.00f);
    c[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.09f, 0.10f, 1.00f);
    c[ImGuiCol_ChildBg] = ImVec4(0.09f, 0.09f, 0.10f, 1.00f);
    c[ImGuiCol_PopupBg] = ImVec4(0.07f, 0.07f, 0.08f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(0.22f, 0.22f, 0.24f, 0.60f);
    c[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.15f, 0.16f, 1.00f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.21f, 0.21f, 0.23f, 1.00f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.25f, 0.25f, 0.27f, 1.00f);
    c[ImGuiCol_TitleBg] = ImVec4(0.06f, 0.06f, 0.07f, 1.00f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.06f, 0.06f, 0.07f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed] = ImVec4(0.06f, 0.06f, 0.07f, 1.00f);
    c[ImGuiCol_MenuBarBg] = ImVec4(0.08f, 0.08f, 0.09f, 1.00f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0.07f, 0.07f, 0.08f, 1.00f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.28f, 0.28f, 0.30f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.36f, 0.36f, 0.38f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.44f, 0.44f, 0.46f, 1.00f);
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = ImVec4(0.55f, 0.55f, 0.58f, 1.00f);
    c[ImGuiCol_SliderGrabActive] = accent;
    c[ImGuiCol_Button] = ImVec4(0.17f, 0.17f, 0.18f, 1.00f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.26f, 0.26f, 0.28f, 1.00f);
    c[ImGuiCol_ButtonActive] = accentDim;
    c[ImGuiCol_Header] = ImVec4(0.16f, 0.16f, 0.17f, 1.00f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.22f, 0.22f, 0.24f, 1.00f);
    c[ImGuiCol_HeaderActive] = ImVec4(accent.x, accent.y, accent.z, 0.60f);
    c[ImGuiCol_Separator] = ImVec4(0.20f, 0.20f, 0.22f, 1.00f);
    c[ImGuiCol_SeparatorHovered] = accentDim;
    c[ImGuiCol_SeparatorActive] = accent;
    c[ImGuiCol_ResizeGrip] = ImVec4(0.22f, 0.22f, 0.24f, 0.60f);
    c[ImGuiCol_ResizeGripHovered] = accentDim;
    c[ImGuiCol_ResizeGripActive] = accent;
    c[ImGuiCol_Tab] = ImVec4(0.10f, 0.10f, 0.11f, 1.00f);
    c[ImGuiCol_TabHovered] = ImVec4(0.24f, 0.24f, 0.26f, 1.00f);
    c[ImGuiCol_TabSelected] = ImVec4(0.19f, 0.19f, 0.21f, 1.00f);
    c[ImGuiCol_TabSelectedOverline] = accent;
    c[ImGuiCol_TabDimmed] = ImVec4(0.08f, 0.08f, 0.09f, 1.00f);
    c[ImGuiCol_TabDimmedSelected] = ImVec4(0.13f, 0.13f, 0.14f, 1.00f);
    c[ImGuiCol_DockingPreview] = ImVec4(accent.x, accent.y, accent.z, 0.55f);
    c[ImGuiCol_DockingEmptyBg] = ImVec4(0.06f, 0.06f, 0.07f, 1.00f);
    c[ImGuiCol_PlotLines] = ImVec4(0.55f, 0.55f, 0.58f, 1.00f);
    c[ImGuiCol_PlotLinesHovered] = themeColor::PlayAccent;
    c[ImGuiCol_PlotHistogram] = accentDim;
    c[ImGuiCol_PlotHistogramHovered] = accent;
    c[ImGuiCol_TableHeaderBg] = ImVec4(0.13f, 0.13f, 0.14f, 1.00f);
    c[ImGuiCol_TableBorderStrong] = ImVec4(0.22f, 0.22f, 0.24f, 1.00f);
    c[ImGuiCol_TableBorderLight] = ImVec4(0.16f, 0.16f, 0.17f, 1.00f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1.00f, 1.00f, 1.00f, 0.03f);
    c[ImGuiCol_TextSelectedBg] = ImVec4(accent.x, accent.y, accent.z, 0.40f);
    c[ImGuiCol_DragDropTarget] = themeColor::PlayAccent;
    c[ImGuiCol_NavCursor] = accent;
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.55f);

    style.WindowRounding = 4.0f;
    style.ChildRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.PopupRounding = 3.0f;
    style.GrabRounding = 3.0f;
    style.TabRounding = 3.0f;
    style.ScrollbarRounding = 6.0f;
    style.WindowPadding = ImVec2(8.0f, 8.0f);
    style.FramePadding = ImVec2(8.0f, 4.0f);
    style.ItemSpacing = ImVec2(8.0f, 5.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.TabBarBorderSize = 1.0f;
    style.DockingSeparatorSize = 1.0f;
}

namespace {

// 存在するファイルだけ AddFontFromFileTTF する (ImGui は missing file でアサートするため)。
// glyph range は指定しない — imgui 1.92 の動的アトラスがオンデマンドでロードする
ImFont* TryAddFont(const std::wstring& path, float sizePx, const ImFontConfig* cfg)
{
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return nullptr;
    }
    return ImGui::GetIO().Fonts->AddFontFromFileTTF(WideToUtf8(path).c_str(), sizePx, cfg);
}

} // namespace

bool SetupEditorFonts(float sizePx)
{
    ImGuiIO& io = ImGui::GetIO();
    wchar_t winDir[MAX_PATH] = {};
    GetWindowsDirectoryW(winDir, MAX_PATH);
    const std::wstring fonts = std::wstring(winDir) + L"\\Fonts\\";

    // ベース: Segoe UI (欧文)。無ければ ImGui 既定フォントに日本語/アイコンをマージする
    ImFont* base = TryAddFont(fonts + L"segoeui.ttf", sizePx, nullptr);
    if (!base) {
        base = io.Fonts->AddFontDefault();
        MYE_LOG_WARN("editor font: segoeui.ttf not found, using ImGui default");
    }

    // 日本語: システムフォントのフォールバック連鎖 (.ttc は FontNo=0 の face)
    ImFontConfig jpCfg;
    jpCfg.MergeMode = true;
    jpCfg.FontNo = 0;
    const wchar_t* jpCandidates[] = { L"YuGothM.ttc", L"meiryo.ttc", L"msgothic.ttc" };
    const char* jpLoaded = nullptr;
    for (const wchar_t* name : jpCandidates) {
        if (TryAddFont(fonts + name, sizePx, &jpCfg)) {
            jpLoaded = (name == jpCandidates[0]) ? "Yu Gothic Medium"
                       : (name == jpCandidates[1]) ? "Meiryo"
                                                   : "MS Gothic";
            break;
        }
    }
    if (!jpLoaded) {
        MYE_LOG_WARN("editor font: no Japanese system font found (JP text will be missing)");
    }

    // アイコン: Font Awesome 6 Solid (埋め込み配列)。等幅化してボタン内で揃える
    ImFontConfig faCfg;
    faCfg.MergeMode = true;
    faCfg.FontDataOwnedByAtlas = false; // 静的配列なので atlas に解放させない
    faCfg.GlyphMinAdvanceX = sizePx;
    const ImFont* fa = io.Fonts->AddFontFromMemoryTTF(
        const_cast<unsigned char*>(fa_solid_900_ttf), static_cast<int>(fa_solid_900_ttf_size),
        sizePx * 0.85f, &faCfg);

    MYE_LOG_INFO("editor fonts: base=%s jp=%s icons=%s", base ? "Segoe UI" : "default",
                 jpLoaded ? jpLoaded : "(none)", fa ? "FontAwesome6" : "(none)");
    return jpLoaded != nullptr; // M47a: UI 言語を日本語にしてよいか
}

} // namespace mye
