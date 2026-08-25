#include "Editor/EditorToolbar.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "Editor/EditorWidgets.h"
#include "Editor/LayoutManager.h"
#include "Editor/Selection.h"
#include "Editor/Undo/UndoStack.h"
#include "Editor/Windows/SceneViewWindow.h"
#include "Engine/Core/Localization.h"
#include "Engine/Engine/Audio/AudioSystem.h"
#include "Engine/Engine/EngineLoop.h"
#include "Engine/Engine/Replay/TimeTravel.h"
#include "Engine/Renderer/ImGuiTheme.h"

#include "imgui.h"
#include "imgui_internal.h" // BeginViewportSideBar

#include "fontawesome/IconsFontAwesome6.h"

namespace mye {

bool EditorToolbar::OnImGui(EngineContext& ctx, PlayModeController& playMode, Selection& selection,
                            UndoStack& undo, SceneViewWindow& sceneView, LayoutManager& layouts,
                            ActorEditBar* actorEdit)
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
        if (ToolbarToggle(ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT,
                          sceneView.GizmoOp() == ImGuizmo::TRANSLATE, Tr(StrId::Tool_TipMove))) {
            sceneView.GizmoOp() = ImGuizmo::TRANSLATE;
        }
        ImGui::SameLine();
        if (ToolbarToggle(ICON_FA_ROTATE, sceneView.GizmoOp() == ImGuizmo::ROTATE,
                          Tr(StrId::Tool_TipRotate))) {
            sceneView.GizmoOp() = ImGuizmo::ROTATE;
        }
        ImGui::SameLine();
        if (ToolbarToggle(ICON_FA_MAXIMIZE, sceneView.GizmoOp() == ImGuizmo::SCALE,
                          Tr(StrId::Tool_TipScale))) {
            sceneView.GizmoOp() = ImGuizmo::SCALE;
        }
        ImGui::SameLine();
        const bool local = sceneView.GizmoMode() == ImGuizmo::LOCAL;
        // アイコンとラベルはコンパイル時連結できない (ラベルが Tr 経由) ので実行時に組む
        char spaceLabel[64];
        std::snprintf(spaceLabel, sizeof(spaceLabel), "%s %s",
                      local ? ICON_FA_CUBE : ICON_FA_GLOBE,
                      Tr(local ? StrId::Tool_SpaceLocal : StrId::Tool_SpaceWorld));
        if (ImGui::Button(spaceLabel)) {
            sceneView.GizmoMode() = local ? ImGuizmo::WORLD : ImGuizmo::LOCAL;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", Tr(StrId::Tool_TipGizmoSpace));
        }

        // ---- ミニシーン編集モードのパンくず (M48k) ----
        // 「今どこを編集しているか」と戻り道を常に見せる。Unity の Prefab Mode と同じ役割
        const bool inActorEdit = (actorEdit != nullptr && actorEdit->name != nullptr);
        if (inActorEdit) {
            ToolbarSeparator();
            if (ImGui::Button(ICON_FA_ARROW_LEFT)) {
                actorEdit->exitRequested = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", Tr(StrId::Tool_TipExitActorEdit));
            }
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding(); // 両隣がボタンの行 — 文字だけ浮かせない
            ImGui::Text(actorEdit->dirty ? "%s *" : "%s", actorEdit->name);
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_FLOPPY_DISK)) {
                actorEdit->saveRequested = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", Tr(StrId::Tool_TipSaveActor));
            }
        }

        // ---- 中央: Play / Pause / Step (旧メニューバー中央から移設) ----
        // 編集モード中は無効化する: Play は Save+Load でシーンを作り直すので、
        // ミニシーン (= アセットの実体) に走らせると編集内容が壊れる
        ImGui::BeginDisabled(inActorEdit);
        ImGui::SameLine(ImGui::GetWindowWidth() * 0.5f - 48.0f);
        if (state == PlayState::Editing) {
            if (ImGui::Button(ICON_FA_PLAY)) {
                selection.Clear(); // 復元で EntityID が変わるため選択解除
                undo.BeginPlaySession();
                playMode.Play(*ctx.scene);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", Tr(StrId::Tool_TipPlay));
            }
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, themeColor::PlayAccent);
            if (ImGui::Button(ICON_FA_STOP)) {
                selection.Clear();
                playMode.Stop(*ctx.scene);
                undo.EndPlaySession(); // Play 中に積まれた Undo エントリを破棄
                // M45: Stop でシーンはスナップショットから戻るが、鳴っている voice は
                // エンジン側の状態なので戻らない。ループ音や BGM が Stop 後も鳴り続けるのを防ぐ。
                // BGM は別レーンなので StopMusic も要る (M45f)
                if (ctx.audio != nullptr) {
                    ctx.audio->StopAll();
                    ctx.audio->StopMusic(kMusicStopFadeSeconds);
                }
            }
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", Tr(StrId::Tool_TipStop));
            }
            ImGui::SameLine();
            if (ImGui::Button(state == PlayState::Paused ? ICON_FA_PLAY : ICON_FA_PAUSE)) {
                playMode.TogglePause();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", Tr(StrId::Tool_TipPause));
            }
            if (state == PlayState::Paused) {
                ImGui::SameLine();
                if (ImGui::Button(ICON_FA_FORWARD_STEP)) {
                    playMode.Step();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", Tr(StrId::Tool_TipStep));
                }
            }
            // ---- 巻き戻し (M52e) ----
            // タイムライン窓を開かずに「今の少し前」へ飛ぶための最短経路。
            // リングが始まる前 (Play 直後の 1 フレーム) は無効
            ImGui::SameLine();
            TimeTravel* const tt = ctx.timeTravel;
            ImGui::BeginDisabled(tt == nullptr || !tt->Enabled());
            if (ImGui::Button(ICON_FA_CLOCK_ROTATE_LEFT) && tt != nullptr && tt->Enabled()) {
                const uint64_t back = 30;
                const uint64_t target = (ctx.tickIndex > tt->FirstTick() + back)
                    ? ctx.tickIndex - back
                    : tt->FirstTick();
                playMode.Pause(); // 巻き戻した先で世界が止まっていないと観察できない
                tt->RequestSeek(target);
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", Tr(StrId::Tool_TipTimeTravel));
            }
        }
        ImGui::EndDisabled();

        // ---- 右: Render Path + レイアウト (最右端、Unity の Layout ドロップダウン相当) ----
        // M47b: 幅は実測にする。訳文の長さでボタンが見切れないようにするため
        const ImGuiStyle& style = ImGui::GetStyle();
        const float layoutBtnW =
            ImGui::CalcTextSize(ICON_FA_TABLE_COLUMNS).x + style.FramePadding.x * 2.0f
            + style.ItemSpacing.x;
        const char* pathItems = Tr(StrId::Tool_RenderPathItems);
        float itemsW = 0.0f;
        for (const char* p = pathItems; *p != '\0'; p += std::strlen(p) + 1) {
            itemsW = (std::max)(itemsW, ImGui::CalcTextSize(p).x);
        }
        // コンボの矢印ぶん (GetFrameHeight) + 左右の余白
        const float rightWidth = itemsW + ImGui::GetFrameHeight() + style.FramePadding.x * 4.0f;
        ImGui::SameLine(ImGui::GetWindowWidth() - rightWidth - layoutBtnW);
        ImGui::SetNextItemWidth(rightWidth);
        int cur = (ctx.renderPath == ctx.renderPathForward) ? 0 : 1;
        if (ImGui::Combo("##renderpath", &cur, pathItems)) {
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
