#include "Editor/Windows/ProjectSettingsWindow.h"

#include <string>

#include "Editor/EditorSettings.h"
#include "Editor/PartTagNames.h"
#include "Editor/PhysicsLayerNames.h"
#include "Editor/ShortcutHub.h"
#include "Engine/Core/Localization.h"
#include "Engine/Renderer/RenderPath.h"

#include "imgui.h"

namespace mye {

void ProjectSettingsWindow::OnImGui(EngineContext& ctx, EditorSettings& settings,
                                    ShortcutHub& shortcuts)
{
    if (!open) {
        return;
    }
    if (!ImGui::Begin(Tr(StrId::Win_ProjectSettings), &open)) {
        ImGui::End();
        return;
    }

    // ---- レンダリング ----
    if (ImGui::CollapsingHeader(Tr(StrId::PrjSet_Rendering), ImGuiTreeNodeFlags_DefaultOpen)) {
        const bool isForward = (ctx.renderPath == ctx.renderPathForward);
        ImGui::Text(Tr(StrId::PrjSet_ActivePath), ctx.renderPath ? ctx.renderPath->Name() : "?");
        if (ImGui::RadioButton(Tr(StrId::Menu_Forward), isForward)) {
            ctx.renderPath = ctx.renderPathForward;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton(Tr(StrId::Menu_Deferred), !isForward)) {
            ctx.renderPath = ctx.renderPathDeferred;
        }
    }

    // ---- エディタ設定 (editor_settings.json) ----
    if (ImGui::CollapsingHeader(Tr(StrId::PrjSet_Editor), ImGuiTreeNodeFlags_DefaultOpen)) {
        char cmd[256];
        std::snprintf(cmd, sizeof(cmd), "%s", settings.externalEditorCmd.c_str());
        if (ImGui::InputText(Tr(StrId::PrjSet_ExternalCmd), cmd, sizeof(cmd))) {
            settings.externalEditorCmd = cmd;
        }
        ImGui::TextDisabled("%s", Tr(StrId::PrjSet_CmdHint));
        ImGui::DragFloat(Tr(StrId::PrjSet_SnapTranslate), &settings.snapTranslate, 0.01f, 0.0f, 100.0f);
        ImGui::DragFloat(Tr(StrId::PrjSet_SnapRotate), &settings.snapRotateDeg, 0.5f, 0.0f, 180.0f);
        ImGui::DragFloat(Tr(StrId::PrjSet_SnapScale), &settings.snapScale, 0.01f, 0.0f, 10.0f);
        ImGui::Checkbox(Tr(StrId::PrjSet_GridVisible), &settings.gridVisible);
        if (ImGui::Button(Tr(StrId::PrjSet_SaveSettings))) {
            settings.Save();
        }
    }

    // ---- 物理レイヤー名 (M36a、assets\project_settings.json の physicsLayers) ----
    if (ImGui::CollapsingHeader(Tr(StrId::PrjSet_PhysicsLayers))) {
        PhysicsLayerNames& ln = PhysicsLayerNames::Get();
        ln.Load(ctx.assetsRoot);
        ImGui::TextDisabled("%s", Tr(StrId::PrjSet_LayerHint));
        for (int i = 0; i < PhysicsLayerNames::kCount; ++i) {
            ImGui::PushID(i);
            ImGui::SetNextItemWidth(160.0f);
            char label[16];
            std::snprintf(label, sizeof(label), "%2d", i);
            ImGui::InputText(label, ln.EditBuffer(i), 32);
            ImGui::PopID();
            if ((i % 2) == 0) {
                ImGui::SameLine(240.0f);
            }
        }
        if (ImGui::Button(Tr(StrId::PrjSet_SaveLayers))) {
            if (ln.Save(ctx.assetsRoot)) {
                ln.Load(ctx.assetsRoot, true);
            }
        }
    }

    // ---- 部位タグ名 (M48f、assets\project_settings.json の partTags) ----
    // 物理レイヤーと違い **名前のハッシュが ID の実体** なので、行数可変 + 注意書きつき
    if (ImGui::CollapsingHeader(Tr(StrId::PrjSet_PartTags))) {
        PartTagNames& pt = PartTagNames::Get();
        pt.Load(ctx.assetsRoot);
        ImGui::TextWrapped("%s", Tr(StrId::PrjSet_PartTagHint));
        for (int i = 0; i < pt.Count(); ++i) {
            ImGui::PushID(i);
            ImGui::SetNextItemWidth(200.0f);
            char label[16];
            std::snprintf(label, sizeof(label), "%2d", i);
            ImGui::InputText(label, pt.EditBuffer(i), PartTagNames::kNameCapacity);
            ImGui::SameLine();
            ImGui::TextDisabled("0x%016llx", static_cast<unsigned long long>(pt.Id(i)));
            ImGui::PopID();
        }
        if (ImGui::Button(Tr(StrId::PrjSet_AddPartTag))) {
            pt.SetCount(pt.Count() + 1);
        }
        ImGui::SameLine();
        if (ImGui::Button(Tr(StrId::PrjSet_SavePartTags))) {
            if (pt.Save(ctx.assetsRoot)) {
                pt.Load(ctx.assetsRoot, true); // 空欄が落ちた結果を読み直す
            }
        }
    }

    // ---- ショートカット一覧 (読み取り専用) ----
    if (ImGui::CollapsingHeader(Tr(StrId::PrjSet_Shortcuts))) {
        static const char* kNames[] = {
            "Save", "Undo", "Redo", "Duplicate", "Delete",
            "Focus", "Rename", "Copy", "Cut", "Paste",
        };
        if (ImGui::BeginTable("##sc", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn(Tr(StrId::PrjSet_ColAction));
            ImGui::TableSetupColumn(Tr(StrId::PrjSet_ColKey));
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
