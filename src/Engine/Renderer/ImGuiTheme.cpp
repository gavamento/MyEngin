#include "Engine/Renderer/ImGuiTheme.h"

#include <cfloat>
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
// 値はヘッダの配色ルール 1 の帯 (S 0.35〜0.60 / V 0.65〜0.85) から。原色は置かない
const ImVec4 Accent = ImVec4(0.34f, 0.52f, 0.80f, 1.00f);
const ImVec4 AccentSoft = ImVec4(0.20f, 0.30f, 0.47f, 1.00f);
const ImVec4 PlayAccent = ImVec4(0.78f, 0.50f, 0.24f, 1.00f);
const ImVec4 Success = ImVec4(0.45f, 0.72f, 0.52f, 1.00f);
const ImVec4 Warning = ImVec4(0.83f, 0.69f, 0.38f, 1.00f);
const ImVec4 Error = ImVec4(0.82f, 0.44f, 0.40f, 1.00f);
const ImVec4 Prefab = ImVec4(0.52f, 0.68f, 0.90f, 1.00f);
} // namespace themeColor

void ApplyEditorTheme(ImGuiStyle& style)
{
    // テーマ第 3 世代: 「ImGui っぽさ」の正体は
    //   ① 入力欄がパネルより**明るい**灰色の箱 (既定スタイルの署名そのもの)
    //   ② ホバーが不透明な灰色の差し替え (面ごとに色が跳ぶ)
    //   ③ ウィンドウタブ左の折りたたみ三角と、常時見える閉じる × の群れ
    // なので、面を 3 段 (最奥 bgApp < パネル bgPanel < ポップアップ) に整理し、
    // 入力欄は逆に**沈める** (bgPanel より暗い bgInput)。ホバー/押下は白の半透明
    // オーバーレイで統一して、どの面の上でも同じ「光り方」になるようにする。
    const ImVec4 accent = themeColor::Accent;
    const ImVec4 text = ImVec4(0.90f, 0.91f, 0.93f, 1.00f);
    const ImVec4 textDim = ImVec4(0.50f, 0.52f, 0.56f, 1.00f);
    const ImVec4 bgApp = ImVec4(0.055f, 0.058f, 0.068f, 1.00f);  // タイトル/メニュー/ドック地
    const ImVec4 bgPanel = ImVec4(0.105f, 0.110f, 0.125f, 1.00f); // ウィンドウ面
    const ImVec4 bgPopup = ImVec4(0.130f, 0.135f, 0.152f, 0.98f);
    const ImVec4 bgInput = ImVec4(0.050f, 0.052f, 0.062f, 1.00f); // 沈み込み (パネルより暗く)
    const ImVec4 hoverWhite = ImVec4(1.00f, 1.00f, 1.00f, 0.06f);
    const auto withAlpha = [](const ImVec4& c, float a) { return ImVec4(c.x, c.y, c.z, a); };

    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = text;
    c[ImGuiCol_TextDisabled] = textDim;
    c[ImGuiCol_WindowBg] = bgPanel;
    c[ImGuiCol_ChildBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_PopupBg] = bgPopup;
    c[ImGuiCol_Border] = ImVec4(1.00f, 1.00f, 1.00f, 0.07f);
    c[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    // 入力欄 (テキスト/ドラッグ/コンボ)。ホバーは色の差し替えではなく「少し浮く」だけ
    c[ImGuiCol_FrameBg] = bgInput;
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.095f, 0.098f, 0.112f, 1.00f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.125f, 0.130f, 0.148f, 1.00f);
    c[ImGuiCol_TitleBg] = bgApp;
    c[ImGuiCol_TitleBgActive] = bgApp;
    c[ImGuiCol_TitleBgCollapsed] = bgApp;
    c[ImGuiCol_MenuBarBg] = bgApp;
    // スクロールバー: 溝を描かずつまみだけ浮かせる (溝がある時点で ImGui の顔になる)
    c[ImGuiCol_ScrollbarBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(1.00f, 1.00f, 1.00f, 0.14f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1.00f, 1.00f, 1.00f, 0.24f);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(1.00f, 1.00f, 1.00f, 0.32f);
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = ImVec4(0.62f, 0.64f, 0.68f, 1.00f);
    c[ImGuiCol_SliderGrabActive] = accent;
    c[ImGuiCol_Button] = ImVec4(1.00f, 1.00f, 1.00f, 0.06f);
    c[ImGuiCol_ButtonHovered] = ImVec4(1.00f, 1.00f, 1.00f, 0.11f);
    c[ImGuiCol_ButtonActive] = withAlpha(accent, 0.55f);
    // Header = CollapsingHeader 兼 選択行。薄いアクセントの帯にして
    // 「選択されている」ことと「見出しである」ことを同じ言語で示す
    c[ImGuiCol_Header] = withAlpha(accent, 0.16f);
    c[ImGuiCol_HeaderHovered] = withAlpha(accent, 0.26f);
    c[ImGuiCol_HeaderActive] = withAlpha(accent, 0.36f);
    c[ImGuiCol_Separator] = ImVec4(1.00f, 1.00f, 1.00f, 0.06f);
    c[ImGuiCol_SeparatorHovered] = withAlpha(accent, 0.60f);
    c[ImGuiCol_SeparatorActive] = accent;
    // リサイズグリップ (右下の三角) は ImGui の署名なので消す。ドッキング中は元々出ない
    c[ImGuiCol_ResizeGrip] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_ResizeGripHovered] = withAlpha(accent, 0.50f);
    c[ImGuiCol_ResizeGripActive] = accent;
    // タブ: 非選択は最奥 (bgApp) に沈め、選択タブだけパネル色 = 「窓とタブが地続き」に見せる
    c[ImGuiCol_Tab] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_TabHovered] = hoverWhite;
    c[ImGuiCol_TabSelected] = bgPanel;
    c[ImGuiCol_TabSelectedOverline] = accent;
    c[ImGuiCol_TabDimmed] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_TabDimmedSelected] = ImVec4(0.085f, 0.089f, 0.101f, 1.00f);
    c[ImGuiCol_TabDimmedSelectedOverline] = withAlpha(accent, 0.35f);
    c[ImGuiCol_DockingPreview] = withAlpha(accent, 0.55f);
    c[ImGuiCol_DockingEmptyBg] = bgApp;
    c[ImGuiCol_PlotLines] = ImVec4(0.55f, 0.57f, 0.61f, 1.00f);
    c[ImGuiCol_PlotLinesHovered] = themeColor::PlayAccent;
    c[ImGuiCol_PlotHistogram] = withAlpha(accent, 0.75f);
    c[ImGuiCol_PlotHistogramHovered] = accent;
    c[ImGuiCol_TableHeaderBg] = bgApp;
    c[ImGuiCol_TableBorderStrong] = ImVec4(1.00f, 1.00f, 1.00f, 0.08f);
    c[ImGuiCol_TableBorderLight] = ImVec4(1.00f, 1.00f, 1.00f, 0.04f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1.00f, 1.00f, 1.00f, 0.02f);
    c[ImGuiCol_TextSelectedBg] = withAlpha(accent, 0.35f);
    c[ImGuiCol_DragDropTarget] = themeColor::PlayAccent;
    c[ImGuiCol_NavCursor] = accent;
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.60f);

    style.WindowRounding = 6.0f;
    style.ChildRounding = 4.0f;
    style.FrameRounding = 4.0f;
    style.PopupRounding = 6.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 5.0f;
    style.ScrollbarRounding = 8.0f;
    style.WindowPadding = ImVec2(10.0f, 10.0f);
    style.FramePadding = ImVec2(10.0f, 5.0f); // 縦 5 = 入力欄とメニューバーを一段ゆったり
    style.ItemSpacing = ImVec2(8.0f, 6.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    style.CellPadding = ImVec2(6.0f, 4.0f);
    style.IndentSpacing = 18.0f;
    style.ScrollbarSize = 12.0f;
    style.GrabMinSize = 10.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.PopupBorderSize = 1.0f;
    style.TabBarBorderSize = 0.0f;
    style.TabBarOverlineSize = 2.0f;
    style.DockingSeparatorSize = 2.0f;
    // SeparatorText は「左寄せの小見出し + 細い線」へ (既定の太線 + 中央線は ImGui の顔)
    style.SeparatorTextBorderSize = 1.0f;
    style.SeparatorTextAlign = ImVec2(0.0f, 0.5f);
    style.SeparatorTextPadding = ImVec2(0.0f, 4.0f);
    // ウィンドウタブ左の折りたたみ三角は表示しない (機能はタブ右クリックに全部ある)
    style.WindowMenuButtonPosition = ImGuiDir_None;
    // 閉じる × は選択中のタブにだけ出す (全タブに × が並ぶのを避ける)
    style.TabCloseButtonMinWidthUnselected = FLT_MAX;
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

// 見出しフォント (テーマ第 3 世代)。コンテキスト再作成で無効になるので
// SetupEditorFonts のたびに取り直す
ImFont* g_headingFont = nullptr;

} // namespace

bool SetupEditorFonts(float sizePx)
{
    ImGuiIO& io = ImGui::GetIO();
    g_headingFont = nullptr;
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

    // 見出し: Segoe UI Semibold + 日本語 Bold のマージ (テーマ第 3 世代)。
    // 「全部同じ太さ・同じサイズの文字」がフラット UI を安く見せる最大要因なので、
    // 見出しだけ太い別書体を用意する。サイズは呼び出し側が PushFont(font, size) で変える
    // (imgui 1.92 の動的アトラスはどのサイズでも再ラスタライズできる)。
    // ★Semibold のファイル名は seguisb.ttf (segoeui〜 ではない)。無ければ Bold で代用、
    //   それも無ければベースで代用 — 呼び出し側はサイズ差だけで階層を出す
    ImFont* heading = TryAddFont(fonts + L"seguisb.ttf", sizePx, nullptr);
    if (!heading) {
        heading = TryAddFont(fonts + L"segoeuib.ttf", sizePx, nullptr);
    }
    if (heading) {
        // 日本語見出し: Bold 系 (YuGothB → meiryob) → 無ければベースと同じ Medium 連鎖。
        // マージ無しだと日本語の見出し (コンポーネント表示名など) が全部豆腐になる
        const wchar_t* jpBoldCandidates[] = { L"YuGothB.ttc", L"meiryob.ttc", L"YuGothM.ttc",
                                              L"meiryo.ttc", L"msgothic.ttc" };
        for (const wchar_t* name : jpBoldCandidates) {
            if (TryAddFont(fonts + name, sizePx, &jpCfg)) {
                break;
            }
        }
        io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(fa_solid_900_ttf),
                                       static_cast<int>(fa_solid_900_ttf_size), sizePx * 0.85f,
                                       &faCfg);
        g_headingFont = heading;
    }

    MYE_LOG_INFO("editor fonts: base=%s jp=%s icons=%s heading=%s", base ? "Segoe UI" : "default",
                 jpLoaded ? jpLoaded : "(none)", fa ? "FontAwesome6" : "(none)",
                 g_headingFont ? "Segoe UI Semibold" : "(base)");
    return jpLoaded != nullptr; // M47a: UI 言語を日本語にしてよいか
}

ImFont* EditorHeadingFont()
{
    return g_headingFont; // null = 呼び出し側で PushFont(nullptr, size) = 書体はそのまま
}

} // namespace mye
