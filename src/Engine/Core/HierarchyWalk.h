#pragma once
#include <cstdint>

#include "Engine/Core/Components.h"
#include "Engine/Core/World.h"

namespace mye {

// ForEachInSubtree の visit が返す「この先どう辿るか」
enum class WalkStep {
    Continue,     // このエンティティの子も辿る
    SkipChildren, // 子は辿らず、次の兄弟へ進む (プレハブの入れ子インスタンスの境界など)
    Stop,         // 走査全体をここで打ち切る (最初の 1 個を探す場合など)
};

// root を含むサブツリーを前順 (親 → 子を兄弟順) に辿る。
// visit(EntityID e, uint32_t siblingIndex) -> WalkStep。siblingIndex は親の子リスト内の位置で、
// root には rootSiblingIndex を渡す (シーン保存が兄弟順を書き出すのに使う)。
// ★兄弟の次は**訪問の前に**控える — 訪問中にその子を破棄・付け替えしても走査が飛ばない。
// ★戻り値は、どこかで Stop が返ったら Stop。それ以外は Continue
template <typename Visit>
WalkStep ForEachInSubtree(World& world, EntityID root, Visit&& visit, uint32_t rootSiblingIndex = 0)
{
    const WalkStep step = visit(root, rootSiblingIndex);
    if (step == WalkStep::Stop) {
        return WalkStep::Stop;
    }
    if (step == WalkStep::SkipChildren) {
        return WalkStep::Continue;
    }
    const auto* h = world.GetComponent<HierarchyComponent>(root);
    EntityID child = h ? h->firstChild : kNullEntity;
    uint32_t index = 0;
    while (!child.IsNull()) {
        const auto* ch = world.GetComponent<HierarchyComponent>(child);
        const EntityID next = ch ? ch->nextSibling : kNullEntity;
        if (ForEachInSubtree(world, child, visit, index++) == WalkStep::Stop) {
            return WalkStep::Stop;
        }
        child = next;
    }
    return WalkStep::Continue;
}

} // namespace mye
