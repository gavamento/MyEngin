#include "Editor/Windows/HierarchyWindow.h"

#include <vector>

#include "Engine/Core/Components.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Renderer/GpuResources.h"

#include "imgui.h"

namespace mye {
namespace {

constexpr const char* kDragPayload = "MYE_ENTITY";

GameObject CreateCube(EngineContext& ctx, const char* name)
{
    GameObject obj = ctx.scene->CreateGameObject(name);
    auto* mr = obj.AddComponent<MeshRendererComponent>();
    mr->mesh = ctx.resources->meshes.Cube();
    mr->material = ctx.resources->materials.Default(*ctx.shaders, ctx.resources->textures);
    return obj;
}

} // namespace

void HierarchyWindow::OnImGui(EngineContext& ctx, Selection& selection)
{
    if (!ImGui::Begin("Hierarchy")) {
        ImGui::End();
        return;
    }
    World& world = ctx.scene->GetWorld();

    // ルート (parent 無し) を集めてから描画 (描画中の構造変更と分離)
    std::vector<EntityID> roots;
    {
        const ComponentTypeId req[] = { HierarchyComponent::sTypeId };
        world.ForEachArchetype(req, [&](Archetype& arch) {
            const int hi = arch.FindTypeIndex(HierarchyComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                if (static_cast<const HierarchyComponent*>(arch.GetPtr(hi, row))->parent.IsNull()) {
                    roots.push_back(arch.EntityAt(row));
                }
            }
        });
    }
    for (EntityID e : roots) {
        DrawEntityNode(ctx, world, e, selection);
    }

    // 空き領域: 背景コンテキストメニュー + ルート化ドロップ
    ImGui::Dummy(ImGui::GetContentRegionAvail());
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kDragPayload)) {
            const EntityID src = *static_cast<const EntityID*>(payload->Data);
            world.SetParent(src, kNullEntity);
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::BeginPopupContextWindow("##hierarchy_bg",
                                       ImGuiPopupFlags_MouseButtonRight
                                           | ImGuiPopupFlags_NoOpenOverItems)) {
        if (ImGui::MenuItem("Create Empty")) {
            selection.entity = ctx.scene->CreateGameObject("GameObject").Id();
        }
        if (ImGui::MenuItem("Create Cube")) {
            selection.entity = CreateCube(ctx, "Cube").Id();
        }
        ImGui::EndPopup();
    }
    ImGui::End();
}

void HierarchyWindow::DrawEntityNode(EngineContext& ctx, World& world, EntityID e,
                                     Selection& selection)
{
    if (!world.IsAlive(e)) {
        return;
    }
    auto* h = world.GetComponent<HierarchyComponent>(e);
    const bool leaf = (h == nullptr) || h->firstChild.IsNull();

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick
        | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (leaf) {
        flags |= ImGuiTreeNodeFlags_Leaf;
    }
    if (selection.entity == e) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }

    ImGui::PushID(static_cast<int>(e.index));
    const bool open = ImGui::TreeNodeEx("##node", flags, "%s", world.GetName(e));

    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
        selection.entity = e;
    }

    if (ImGui::BeginPopupContextItem("##entity_ctx")) {
        selection.entity = e;
        if (ImGui::MenuItem("Create Child")) {
            GameObject child = ctx.scene->CreateGameObject("GameObject");
            world.SetParent(child.Id(), e);
            selection.entity = child.Id();
        }
        if (ImGui::MenuItem("Create Child Cube")) {
            GameObject child = CreateCube(ctx, "Cube");
            world.SetParent(child.Id(), e);
            selection.entity = child.Id();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Delete")) {
            world.DestroyEntity(e); // 子孫ごと tick 末に破棄
            if (selection.entity == e) {
                selection.entity = kNullEntity;
            }
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload(kDragPayload, &e, sizeof(e));
        ImGui::TextUnformatted(world.GetName(e));
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kDragPayload)) {
            const EntityID src = *static_cast<const EntityID*>(payload->Data);
            if (src != e) {
                world.SetParent(src, e); // 循環は World 側で拒否される
            }
        }
        ImGui::EndDragDropTarget();
    }

    if (open) {
        if (h) {
            EntityID child = h->firstChild;
            while (!child.IsNull()) {
                auto* ch = world.GetComponent<HierarchyComponent>(child);
                const EntityID next = ch ? ch->nextSibling : kNullEntity;
                DrawEntityNode(ctx, world, child, selection);
                child = next;
            }
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

} // namespace mye
