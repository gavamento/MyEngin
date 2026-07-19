#include "Editor/Windows/ProjectSettingsWindow.h"

#include <string>

#include "Editor/EditorSettings.h"
#include "Editor/ShortcutHub.h"
#include "Engine/Renderer/RenderPath.h"

#include "imgui.h"

namespace mye {

void ProjectSettingsWindow::OnImGui(EngineContext& ctx, EditorSettings& settings,
                                    ShortcutHub& shortcuts)
{
    if (!open) {
        return;
    }
    if (!ImGui::Begin("Project Settings", &open)) {
        ImGui::End();
        return;
    }

    // ---- レンダリング ----
    if (ImGui::CollapsingHeader("Rendering", ImGuiTreeNodeFlags_DefaultOpen)) {
        const bool isForward = (ctx.renderPath == ctx.renderPathForward);
        ImGui::Text("Active render path: %s", ctx.renderPath ? ctx.renderPath->Name() : "?");
        if (ImGui::RadioButton("Forward", isForward)) {
            ctx.renderPath = ctx.renderPathForward;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Deferred", !isForward)) {
            ctx.renderPath = ctx.renderPathDeferred;
        }
    }

    // ---- エディタ設定 (editor_settings.json) ----
    if (ImGui::CollapsingHeader("Editor", ImGuiTreeNodeFlags_DefaultOpen)) {
        char cmd[256];
        std::snprintf(cmd, sizeof(cmd), "%s", settings.externalEditorCmd.c_str());
        if (ImGui::InputText("External editor cmd", cmd, sizeof(cmd))) {
            settings.externalEditorCmd = cmd;
        }
        ImGui::TextDisabled("  {file} / {line} が Console のソースジャンプで置換されます");
        ImGui::DragFloat("Snap: translate", &settings.snapTranslate, 0.01f, 0.0f, 100.0f);
        ImGui::DragFloat("Snap: rotate (deg)", &settings.snapRotateDeg, 0.5f, 0.0f, 180.0f);
        ImGui::DragFloat("Snap: scale", &settings.snapScale, 0.01f, 0.0f, 10.0f);
        ImGui::Checkbox("Grid visible", &settings.gridVisible);
        if (ImGui::Button("Save Settings")) {
            settings.Save();
        }
    }

    // ---- ショートカット一覧 (読み取り専用) ----
    if (ImGui::CollapsingHeader("Shortcuts")) {
        static const char* kNames[] = {
            "Save", "Undo", "Redo", "Duplicate", "Delete",
            "Focus", "Rename", "Copy", "Cut", "Paste",
        };
        if (ImGui::BeginTable("##sc", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Action");
            ImGui::TableSetupColumn("Key");
            ImGui::TableHeadersRow();
            for (int i = 0; i < static_cast<int>(Shortcut::Count); ++i) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(kNames[i]);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(shortcuts.Label(static_cast<Shortcut>(i)));
            }
            ImGui::EndTable();
        }
    }

    ImGui::End();
}

} // namespace mye
