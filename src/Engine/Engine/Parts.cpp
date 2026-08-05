#include "Engine/Engine/Parts.h"

#include <cstring>
#include <functional>

#include "Engine/Core/Components.h"
#include "Engine/Core/World.h"

namespace mye::Parts {

EntityID FindPart(World& world, EntityID root, std::string_view utf8Path)
{
    if (!world.IsAlive(root)) {
        return kNullEntity;
    }
    EntityID cur = root;
    size_t i = 0;
    while (i < utf8Path.size()) {
        const size_t sep = utf8Path.find('/', i);
        const size_t end = (sep == std::string_view::npos) ? utf8Path.size() : sep;
        const std::string_view seg = utf8Path.substr(i, end - i);
        i = (sep == std::string_view::npos) ? utf8Path.size() : sep + 1;
        if (seg.empty()) {
            continue; // 先頭/末尾/連続の '/' は読み飛ばす
        }
        auto* h = world.GetComponent<HierarchyComponent>(cur);
        EntityID c = h ? h->firstChild : kNullEntity;
        EntityID hit = kNullEntity;
        while (!c.IsNull()) {
            if (seg == world.GetName(c)) {
                hit = c;
                break; // 最初の一致 (兄弟名の一意化は M48b がエディタ操作時に保証する)
            }
            auto* ch = world.GetComponent<HierarchyComponent>(c);
            c = ch ? ch->nextSibling : kNullEntity;
        }
        if (hit.IsNull()) {
            return kNullEntity;
        }
        cur = hit;
    }
    return cur;
}

void FindPartsByTag(World& world, EntityID root, uint64_t tag, std::vector<EntityID>& out)
{
    if (!world.IsAlive(root)) {
        return;
    }
    // 入れ子インスタンスの境界は見ない (フラット走査) — Parts.h の設計判断
    std::function<void(EntityID)> visit = [&](EntityID e) {
        if (auto* p = world.GetComponent<PartComponent>(e); p && p->tag == tag) {
            out.push_back(e);
        }
        auto* h = world.GetComponent<HierarchyComponent>(e);
        EntityID c = h ? h->firstChild : kNullEntity;
        while (!c.IsNull()) {
            // 次を先に控える (訪問中に破棄されても走査が飛ばない家風)
            auto* ch = world.GetComponent<HierarchyComponent>(c);
            const EntityID next = ch ? ch->nextSibling : kNullEntity;
            visit(c);
            c = next;
        }
    };
    visit(root);
}

EntityID ResolvePartSource(World& world, EntityID part, EntityID explicitSource)
{
    if (!explicitSource.IsNull() && world.IsAlive(explicitSource)
        && world.GetComponent<SkinnedMeshComponent>(explicitSource)) {
        return explicitSource;
    }
    for (EntityID a = world.GetParent(part); !a.IsNull(); a = world.GetParent(a)) {
        if (world.GetComponent<SkinnedMeshComponent>(a)) {
            return a;
        }
    }
    return kNullEntity;
}

bool IsStructureLocked(World& world, EntityID e)
{
    if (!world.IsAlive(e)) {
        return false;
    }
    return world.GetComponent<PartComponent>(e) != nullptr
        && world.GetComponent<PrefabLinkComponent>(e) != nullptr;
}

} // namespace mye::Parts
