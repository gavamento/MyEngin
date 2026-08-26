#include "Editor/EditorWidgets.h"

#include <algorithm>

#include "Engine/Renderer/ImGuiTheme.h"

#include "imgui.h"
#include "imgui_internal.h" // SeparatorEx (縦区切り)

namespace mye {

namespace {

ImVec4 Lighten(const ImVec4& c, float k)
{
    return ImVec4((std::min)(c.x * k, 1.0f), (std::min)(c.y * k, 1.0f), (std::min)(c.z * k, 1.0f),
                  c.w);
}

} // namespace

bool ToolbarToggle(const char* label, bool on, const char* tooltip, bool mode)
{
    int pushed = 0;
    if (on) {
        // ON の面色。テーマの ButtonHovered/Active は白の半透明オーバーレイなので、
        // Button だけ差し替えるとホバーの瞬間に ON の色が消える — 3 状態とも押す
        const ImVec4 base = mode
            ? ImVec4(themeColor::PlayAccent.x * 0.55f, themeColor::PlayAccent.y * 0.55f,
                     themeColor::PlayAccent.z * 0.55f, 1.0f)
            : themeColor::AccentSoft;
        ImGui::PushStyleColor(ImGuiCol_Button, base);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Lighten(base, 1.25f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, Lighten(base, 1.45f));
        pushed = 3;
    }
    const bool pressed = ImGui::Button(label);
    ImGui::PopStyleColor(pushed);
    if (tooltip != nullptr && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tooltip);
    }
    return pressed;
}

void ToolbarSeparator()
{
    ImGui::SameLine();
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical, 1.0f);
    ImGui::SameLine();
}

bool BeginToolbarOverlay(const char* id, float height, const ImVec4* tint)
{
    ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_PopupBg];
    if (tint != nullptr) {
        const float k = 0.30f; // 面へ薄く混ぜるだけ (tint をそのまま塗ると文字が沈む)
        bg = ImVec4(bg.x + (tint->x - bg.x) * k, bg.y + (tint->y - bg.y) * k,
                    bg.z + (tint->z - bg.z) * k, bg.w);
    }
    bg.w = 0.92f; // ビューポートが薄く透ける (完全不透明だとパネルではなく「板」に見える)
    ImGui::PushStyleColor(ImGuiCol_ChildBg, bg);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
    if (height <= 0.0f) {
        height = ImGui::GetFrameHeight() + 12.0f; // 上下 padding 6px ぶん
    }
    return ImGui::BeginChild(id, ImVec2(0.0f, height),
                             ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_Borders,
                             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
}

void EndToolbarOverlay()
{
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

// ---- ToolbarFlow (M47b追補) ----

int ToolbarFlow::BeginFrame(float limitWidth)
{
    int rows = 1;
    float x = 0.0f;
    for (int i = 0; i < groupCount_ && i < kMaxGroups; ++i) {
        const float need = widths_[i] + (i > 0 ? sepWidth_ : 0.0f);
        if (i > 0 && x + need > limitWidth) {
            breakBefore_[i] = true;
            x = widths_[i];
            ++rows;
        } else {
            breakBefore_[i] = false;
            x += need;
        }
    }
    return rows;
}

void ToolbarFlow::FirstGroup()
{
    group_ = 0;
    groupStartX_ = ImGui::GetCursorScreenPos().x;
}

void ToolbarFlow::Separator()
{
    // ★カーソル X ではなく「最後のアイテムの右端 (スクリーン座標)」で幅を測る —
    //   スライダのように SameLine で終わらないグループはカーソルが既に次行へ
    //   落ちていて、カーソル基準だと幅がゼロや負に化けるため
    if (group_ < kMaxGroups) {
        widths_[group_] =
            ImGui::GetItemRectMax().x - groupStartX_ + ImGui::GetStyle().ItemSpacing.x;
    }
    ImGui::SameLine(); // SameLine で終わっていないグループも行内へ戻して起点を揃える
    ++group_;
    if (group_ < kMaxGroups && breakBefore_[group_]) {
        ImGui::NewLine(); // 直前の SameLine を打ち消して次の行頭へ (ImGui の規約)
    } else {
        const float before = ImGui::GetCursorScreenPos().x;
        ToolbarSeparator();
        sepWidth_ = ImGui::GetCursorScreenPos().x - before; // テーマ余白依存なので実測で追随
    }
    groupStartX_ = ImGui::GetCursorScreenPos().x;
}

void ToolbarFlow::EndFrame()
{
    if (group_ < kMaxGroups) {
        widths_[group_] =
            ImGui::GetItemRectMax().x - groupStartX_ + ImGui::GetStyle().ItemSpacing.x;
    }
    groupCount_ = group_ + 1;
}

float ToolbarFlow::OverlayHeight(int rows)
{
    // BeginToolbarOverlay の既定 (フレーム高 + 上下 padding 6px) の行数拡張
    return ImGui::GetFrameHeight() * static_cast<float>(rows)
        + ImGui::GetStyle().ItemSpacing.y * static_cast<float>(rows - 1) + 12.0f;
}

void DrawItemIconLabel(const char* icon, const ImVec4& iconColor, const char* label, bool framed)
{
    // TreeNodeBehavior のラベル開始位置を再現する (imgui 1.92:
    // text_offset_x = FontSize + FramePadding.x * (framed ? 3 : 2)、
    // text_offset_y = framed ? FramePadding.y : 0)。imgui 更新で描画がズレたらここを疑う
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 mn = ImGui::GetItemRectMin();
    const ImVec2 mx = ImGui::GetItemRectMax();
    ImVec2 pos(mn.x + ImGui::GetFontSize() + style.FramePadding.x * (framed ? 3.0f : 2.0f),
               mn.y + (framed ? style.FramePadding.y : 0.0f));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec4 clip(mn.x, mn.y, mx.x, mx.y);
    ImFont* font = ImGui::GetFont();
    const float size = ImGui::GetFontSize();
    dl->AddText(font, size, pos, ImGui::GetColorU32(iconColor), icon, nullptr, 0.0f, &clip);
    pos.x += ImGui::CalcTextSize(icon).x + style.ItemInnerSpacing.x;
    dl->AddText(font, size, pos, ImGui::GetColorU32(ImGuiCol_Text), label, nullptr, 0.0f, &clip);
}

} // namespace mye
