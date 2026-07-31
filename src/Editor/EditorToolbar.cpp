#include "Editor/EditorToolbar.h"

#include "Editor/LayoutManager.h"
#include "Editor/Selection.h"
#include "Editor/Undo/UndoStack.h"
#include "Editor/Windows/SceneViewWindow.h"
#include "Engine/Engine/EngineLoop.h"
#include "Engine/Renderer/ImGuiTheme.h"

#include "imgui.h"
#include "imgui_internal.h" // BeginViewportSideBar

#include "fontawesome/IconsFontAwesome6.h"

namespace mye {

namespace {

// アクティブ状態を持つアイコンボタン (ギズモ操作トグル用)
bool ToggleIconButton(const char* icon, bool active, const char* tooltip)
{
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, themeColor::Accent);
    }
    const bool pressed = ImGui::Button(icon);
    if (active) {
        ImGui::PopStyleColor();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tooltip);
    }
    return pressed;
}

} // namespace

bool EditorToolbar::OnImGui(EngineContext& ctx, PlayModeController& playMode, Selection& selection,
                            UndoStack& undo, SceneViewWindow& sceneView, LayoutManager& layouts)
{
    bool resetLayout = false;
    const PlayState state = playMode.State();
    const bool inPlay = state != PlayState::Editing;

    // Play 中の視覚化 (Unity の Playmode Tint 相当): ツールバー背景をオレンジ系へ
    if (inPlay) {
        const ImVec4 a = themeColor::PlayAccent;
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(a.x * 0.32f, a.y * 0.32f, a.z * 0.32f, 1.0f));
    }
    if (ImGui::BeginViewportSideBar("##mye_toolbar", ImGui::GetMainViewport(), ImGuiDir_Up,
                                    ImGui::GetFrameHeight() + 8.0f,
                                    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar
                                        | ImGuiWindowFlags_NoDecoration
                                        | ImGuiWindowFlags_NoDocking)) {
        ImGui::SetCursorPosY(4.0f);

        // ---- 左: ギズモ操作 (SceneView と状態共有。W/E/R ショートカットは SceneView 側) ----
        if (ToggleIconButton(ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT,
                             sceneView.GizmoOp() == ImGuizmo::TRANSLATE, "移動 (W)")) {
            sceneView.GizmoOp() = ImGuizmo::TRANSLATE;
        }
        ImGui::SameLine();
        if (ToggleIconButton(ICON_FA_ROTATE, sceneView.GizmoOp() == ImGuizmo::ROTATE, "回転 (E)")) {
            sceneView.GizmoOp() = ImGuizmo::ROTATE;
        }
        ImGui::SameLine();
        if (ToggleIconButton(ICON_FA_MAXIMIZE, sceneView.GizmoOp() == ImGuizmo::SCALE, "拡縮 (R)")) {
            sceneView.GizmoOp() = ImGuizmo::SCALE;
        }
        ImGui::SameLine();
        const bool local = sceneView.GizmoMode() == ImGuizmo::LOCAL;
        if (ImGui::Button(local ? ICON_FA_CUBE " Local" : ICON_FA_GLOBE " World")) {
            sceneView.GizmoMode() = local ? ImGuizmo::WORLD : ImGuizmo::LOCAL;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("ギズモの座標系を切替");
        }

        // ---- 中央: Play / Pause / Step (旧メニューバー中央から移設) ----
        ImGui::SameLine(ImGui::GetWindowWidth() * 0.5f - 48.0f);
        if (state == PlayState::Editing) {
            if (ImGui::Button(ICON_FA_PLAY)) {
                selection.Clear(); // 復元で EntityID が変わるため選択解除
                undo.BeginPlaySession();
                playMode.Play(*ctx.scene);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Play");
            }
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, themeColor::PlayAccent);
            if (ImGui::Button(ICON_FA_STOP)) {
                selection.Clear();
                playMode.Stop(*ctx.scene);
                undo.EndPlaySession(); // Play 中に積まれた Undo エントリを破棄
            }
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Stop (変更は破棄されます)");
            }
            ImGui::SameLine();
            if (ImGui::Button(state == PlayState::Paused ? ICON_FA_PLAY : ICON_FA_PAUSE)) {
                playMode.TogglePause();
            }
            if (state == PlayState::Paused) {
                ImGui::SameLine();
                if (ImGui::Button(ICON_FA_FORWARD_STEP)) {
                    playMode.Step();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("1 tick 進める");
                }
            }
        }

        // ---- 右: Render Path + レイアウト (最右端、Unity の Layout ドロップダウン相当) ----
        const float layoutBtnW = 38.0f;
        const float rightWidth = 150.0f;
        ImGui::SameLine(ImGui::GetWindowWidth() - rightWidth - layoutBtnW);
        ImGui::SetNextItemWidth(rightWidth - 12.0f);
        int cur = (ctx.renderPath == ctx.renderPathForward) ? 0 : 1;
        if (ImGui::Combo("##renderpath", &cur, "Forward\0Deferred\0")) {
            ctx.renderPath = (cur == 0) ? ctx.renderPathForward : ctx.renderPathDeferred;
        }
        ImGui::SameLine(ImGui::GetWindowWidth() - layoutBtnW);
        resetLayout = layouts.DrawToolbarUi();
    }
    ImGui::End();
    if (inPlay) {
        ImGui::PopStyleColor();
    }

    // Play 中はビューポート全周に 2px のオレンジ枠 (最前面)
    if (inPlay) {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        const ImU32 col = ImGui::ColorConvertFloat4ToU32(themeColor::PlayAccent);
        // imgui 1.92: AddRect は (min, max, col, rounding, thickness) — thickness が 5 番目
        ImGui::GetForegroundDrawList(const_cast<ImGuiViewport*>(vp))
            ->AddRect(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), col, 0.0f,
                      2.0f);
    }
    return resetLayout;
}

} // namespace mye
