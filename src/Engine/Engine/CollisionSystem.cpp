#include "Engine/Engine/CollisionSystem.h"

#include <algorithm>
#include <cmath>
#include <iterator>

#include "Engine/Core/Components.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/Physics/Shapes.h"
#include "Engine/Engine/Script/ManagedHost.h"
#include "Engine/Engine/Script/ScriptHost.h"

using namespace DirectX;

namespace mye {
namespace {

// 判定は Physics/Shapes.cpp に統合 (M28a)。sphere/box/capsule + 回転 (OBB) 対応。
// 境界 (ちょうど接触 = 距離が厳密に一致) はソリッド判定と同じ「重なりのみ true」に統一
// (M7 は境界含みだったが float 同値の測度ゼロ事象のため実挙動差なし)
struct Body {
    EntityID entity;
    ShapePose pose;
};

} // namespace

void CollisionSystem::Update(World& world, ScriptHost* scripts, ManagedHost* managed)
{
    // ---- 収集 (index 昇順 = 決定論) ----
    std::vector<Body> bodies;
    const ComponentTypeId req[] = { ColliderComponent::sTypeId, WorldMatrixComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int ci = arch.FindTypeIndex(ColliderComponent::sTypeId);
        const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            if (!IsEntityActive(world, arch.EntityAt(row))) {
                continue; // 無効エンティティのコライダーは判定から除外 (M10)
            }
            const auto* col = static_cast<const ColliderComponent*>(arch.GetPtr(ci, row));
            const auto* wm = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row));
            bodies.push_back({ arch.EntityAt(row), shapes::MakePoseFromMatrix(*col, wm->value) });
        }
    });
    std::sort(bodies.begin(), bodies.end(),
              [](const Body& a, const Body& b) { return a.entity.index < b.entity.index; });

    // ---- 総当たり判定 → 現 tick のペア集合 ----
    std::vector<uint64_t> pairs;
    for (size_t i = 0; i < bodies.size(); ++i) {
        for (size_t j = i + 1; j < bodies.size(); ++j) {
            if (shapes::Overlap(bodies[i].pose, bodies[j].pose)) {
                pairs.push_back((static_cast<uint64_t>(bodies[i].entity.index) << 32)
                                | bodies[j].entity.index);
            }
        }
    }
    std::sort(pairs.begin(), pairs.end());

    // ---- 差分 → enter / exit イベント (昇順配信 = 決定論) ----
    std::vector<uint64_t> entered, exited;
    std::set_difference(pairs.begin(), pairs.end(), prevPairs_.begin(), prevPairs_.end(),
                        std::back_inserter(entered));
    std::set_difference(prevPairs_.begin(), prevPairs_.end(), pairs.begin(), pairs.end(),
                        std::back_inserter(exited));
    prevPairs_ = std::move(pairs);

    if (!scripts && !managed) {
        return;
    }
    auto resolve = [&world](uint32_t index) {
        // index から現世代のハンドルを引く (死んでいれば null)
        for (const auto& arch : world.Archetypes()) {
            for (uint32_t row = 0; row < arch->Count(); ++row) {
                if (arch->EntityAt(row).index == index) {
                    return arch->EntityAt(row);
                }
            }
        }
        return kNullEntity;
    };
    auto dispatch = [&](const std::vector<uint64_t>& list, bool enter) {
        for (uint64_t key : list) {
            const EntityID a = resolve(static_cast<uint32_t>(key >> 32));
            const EntityID b = resolve(static_cast<uint32_t>(key & 0xFFFFFFFFu));
            if (!a.IsNull()) {
                if (scripts) { scripts->DispatchTrigger(a, b, enter); }
                if (managed) { managed->DispatchTrigger(a, b, enter); }
            }
            if (!b.IsNull()) {
                if (scripts) { scripts->DispatchTrigger(b, a, enter); }
                if (managed) { managed->DispatchTrigger(b, a, enter); }
            }
        }
    };
    dispatch(entered, true);
    dispatch(exited, false);
}

} // namespace mye
