#include "Editor/Windows/SearchWindow.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "Engine/Core/ComponentRegistry.h"
#include "Engine/Core/Components.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/Animation.h"
#include "Engine/Engine/Prefab.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Renderer/GpuResources.h"

#include "imgui.h"

namespace mye {
namespace {

std::string Lower(std::string s)
{
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool Contains(const std::string& hay, const std::string& needle)
{
    return needle.empty() || Lower(hay).find(needle) != std::string::npos;
}

uint64_t FidOf(World& w, EntityID e)
{
    auto* f = w.GetComponent<FileIdComponent>(e);
    return f ? f->value : 0;
}

} // namespace

void SearchWindow::OnImGui(EngineContext& ctx, Selection& selection)
{
    if (!open) {
        return;
    }
    if (!ImGui::Begin("Search", &open)) {
        ImGui::End();
        return;
    }
    World& world = ctx.scene->GetWorld();
    const ComponentRegistry& reg = ComponentRegistry::Get();

    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##q", "Search entities / assets...", query_, sizeof(query_));
    const std::string needle = Lower(query_);
    ImGui::Separator();

    // ---- エンティティ (名前 or コンポーネント型名で一致) ----
    if (ImGui::CollapsingHeader("Entities", ImGuiTreeNodeFlags_DefaultOpen)) {
        const ComponentTypeId req[] = { NameComponent::sTypeId };
        int shown = 0;
        world.ForEachArchetype(req, [&](Archetype& arch) {
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                const EntityID e = arch.EntityAt(row);
                const std::string name = world.GetName(e);
                bool match = Contains(name, needle);
                if (!match && !needle.empty()) {
                    if (const Archetype* a = world.GetArchetype(e)) {
                        for (ComponentTypeId t : a->Types()) {
                            if (Contains(reg.Desc(t).name, needle)) {
                                match = true;
                                break;
                            }
                        }
                    }
                }
                if (!match || shown >= 200) {
                    continue;
                }
                ++shown;
                const uint64_t fid = FidOf(world, e);
                ImGui::PushID(static_cast<int>(e.index));
                if (ImGui::Selectable(name.c_str(), fid != 0 && selection.Contains(fid))) {
                    selection.SelectOnly(ctx.scene->EnsureFileId(e));
                }
                ImGui::PopID();
            }
        });
        if (shown == 0) {
            ImGui::TextDisabled("  (none)");
        }
    }

    // ---- アセット (名前一致) ----
    if (ImGui::CollapsingHeader("Assets", ImGuiTreeNodeFlags_DefaultOpen)) {
        auto listAssets = [&](const char* kind, const std::vector<AssetEntry>& entries) {
            for (const AssetEntry& a : entries) {
                if (Contains(a.name, needle)) {
                    ImGui::BulletText("%s: %s", kind, a.name.c_str());
                }
            }
        };
        listAssets("mesh", ctx.resources->meshes.Enumerate());
        listAssets("material", ctx.resources->materials.Enumerate());
        listAssets("texture", ctx.resources->textures.Enumerate());
        for (const PrefabEntry& p : ctx.prefabs->Enumerate()) {
            if (Contains(p.name, needle)) {
                ImGui::BulletText("prefab: %s", p.name.c_str());
            }
        }
        for (const AnimClipEntry& c : ctx.anims->Enumerate()) {
            if (Contains(c.name, needle)) {
                ImGui::BulletText("anim: %s", c.name.c_str());
            }
        }
    }

    // ---- 参照逆引き (選択エンティティを EntityRef で指しているエンティティ) ----
    if (ImGui::CollapsingHeader("References to selection", ImGuiTreeNodeFlags_DefaultOpen)) {
        GameObject sel = ctx.scene->FindByFileId(selection.primary);
        if (!sel) {
            ImGui::TextDisabled("  (select an entity to find who references it)");
        } else {
            const EntityID target = sel.Id();
            ImGui::Text("who references '%s':", world.GetName(target));
            int found = 0;
            const ComponentTypeId req[] = { NameComponent::sTypeId };
            world.ForEachArchetype(req, [&](Archetype& arch) {
                for (uint32_t row = 0; row < arch.Count(); ++row) {
                    const EntityID e = arch.EntityAt(row);
                    const Archetype* a = world.GetArchetype(e);
                    if (!a) {
                        continue;
                    }
                    bool refs = false;
                    for (ComponentTypeId t : a->Types()) {
                        const ComponentDesc& d = reg.Desc(t);
                        if (d.flags & kComponentNoSerialize) {
                            continue; // Hierarchy 等の内部 EntityRef は除外
                        }
                        const void* comp = world.GetComponentRaw(e, t);
                        for (const FieldDesc& f : d.fields) {
                            if (f.type != FieldType::EntityRef) {
                                continue;
                            }
                            const EntityID ref = *reinterpret_cast<const EntityID*>(
                                static_cast<const uint8_t*>(comp) + f.offset);
                            if (ref == target) {
                                refs = true;
                            }
                        }
                    }
                    if (refs) {
                        ++found;
                        ImGui::PushID(static_cast<int>(e.index));
                        if (ImGui::Selectable(world.GetName(e))) {
                            selection.SelectOnly(ctx.scene->EnsureFileId(e));
                        }
                        ImGui::PopID();
                    }
                }
            });
            if (found == 0) {
                ImGui::TextDisabled("  (no references)");
            }
        }
    }

    ImGui::End();
}

} // namespace mye
