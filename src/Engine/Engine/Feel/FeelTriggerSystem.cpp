//====================================================================================
//                          FeelTriggerSystem.cpp
//  MyEngin/ 秋田蓮音                                                       11/04/2026
//                                          接触で手触りを鳴らす FeelTrigger (M94d、ADR-031) の評価の実装
//====================================================================================
#include "Engine/Engine/Feel/FeelTriggerSystem.h"

#include <algorithm>
#include <cmath>

#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Feel/FeelLibrary.h"
#include "Engine/Engine/Feel/FeelState.h"
#include "Engine/Engine/Loop/GameFlow.h"
#include "Engine/Engine/Physics/Rigid/PhysicsSystem.h" // SolidContact

namespace mye {

namespace {

struct TriggerRow {
    uint32_t index = 0;
    FeelTriggerComponent* comp = nullptr;
};

} // namespace

uint32_t UpdateFeelTriggers(World& world, const std::vector<SolidContact>& contacts, FeelState& feel,
                            TimeControl& time)
{
    std::vector<TriggerRow> rows;
    const ComponentTypeId req[] = { FeelTriggerComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int ti = arch.FindTypeIndex(FeelTriggerComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            rows.push_back({ arch.EntityAt(row).index, static_cast<FeelTriggerComponent*>(arch.GetPtr(ti, row)) });
        }
    });
    if (rows.empty()) {
        return 0;
    }
    // 接触の key は entity index の組なので、index で引けるように並べる (アーキタイプの並びに依らない)
    std::sort(rows.begin(), rows.end(), [](const TriggerRow& a, const TriggerRow& b) { return a.index < b.index; });
    for (TriggerRow& r : rows) {
        if (r.comp->cooldownLeft > 0) {
            --r.comp->cooldownLeft;
        }
    }
    const auto find = [&rows](uint32_t index) -> FeelTriggerComponent* {
        const auto it = std::lower_bound(rows.begin(), rows.end(), index,
                                         [](const TriggerRow& r, uint32_t v) { return r.index < v; });
        return (it != rows.end() && it->index == index) ? it->comp : nullptr;
    };
    uint32_t played = 0;
    for (const SolidContact& c : contacts) {
        const uint32_t sides[2] = { static_cast<uint32_t>(c.key >> 32), static_cast<uint32_t>(c.key & 0xFFFFFFFFu) };
        for (uint32_t side : sides) {
            FeelTriggerComponent* t = find(side);
            if (t == nullptr || t->cooldownLeft > 0 || !(c.impulse >= t->minImpulse)) {
                continue;
            }
            const FeelPreset* preset = feellib::Resolve(t->feel);
            if (preset == nullptr) {
                continue; // 未割り当て / 未登録の資産は鳴らさない (手触りモニタが空の枠を見せる)
            }
            const float full = t->fullImpulse > 0.01f ? t->fullImpulse : 0.01f;
            const float maxI = std::isfinite(t->maxIntensity) ? std::max(0.0f, t->maxIntensity) : 1.0f;
            const float intensity = std::clamp(c.impulse / full, 0.0f, maxI);
            feel.Play(preset->desc, intensity, preset->nameHash, time);
            t->cooldownLeft = std::clamp(t->cooldownTicks, 0, kMaxFeelChannelTicks);
            ++played;
        }
    }
    return played;
}

} // namespace mye
