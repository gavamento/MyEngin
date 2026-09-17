//====================================================================================
//                          Tags.cpp
//  MyEngin/ 秋田蓮音                                                       09/17/2026
//                                          汎用タグの判定と検索の実装
//====================================================================================
#include "Engine/Engine/Tags.h"

#include <algorithm>
#include <array>

#include "Engine/Core/World.h"

namespace mye::Tags {

uint64_t OwnMask(World& world, EntityID e)
{
    const auto* t = world.GetComponent<TagComponent>(e);
    return (t != nullptr) ? t->mask : 0ull;
}

uint64_t EffectiveMask(World& world, EntityID e)
{
    uint64_t mask = 0;
    for (EntityID cur = e; !cur.IsNull(); cur = world.GetParent(cur)) {
        mask |= OwnMask(world, cur);
    }
    return mask;
}

void FindEntitiesWithTag(World& world, int32_t tagIndex, std::vector<EntityID>& out)
{
    out.clear();
    const uint64_t bit = BitOf(tagIndex);
    if (bit == 0) {
        return;
    }
    const std::array<ComponentTypeId, 1> req = { TagComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int ti = arch.FindTypeIndex(TagComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const auto* t = static_cast<const TagComponent*>(arch.GetPtr(ti, row));
            if ((t->mask & bit) != 0) {
                out.push_back(arch.EntityAt(row));
            }
        }
    });
    // 並びは index 昇順 (理由は Tags.h)。index は生存中に一意なのでタイブレークは要らない
    std::sort(out.begin(), out.end(),
              [](const EntityID& a, const EntityID& b) { return a.index < b.index; });
}

} // namespace mye::Tags
