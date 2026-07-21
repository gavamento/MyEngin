#include "Engine/Engine/CollisionSystem.h"

#include <algorithm>
#include <cmath>
#include <iterator>

#include "Engine/Core/Components.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/Script/ManagedHost.h"
#include "Engine/Engine/Script/ScriptHost.h"

using namespace DirectX;

namespace mye {
namespace {

struct Body {
    EntityID entity;
    int32_t shape; // 0=sphere 1=aabb
    XMFLOAT3 center;
    float radius;
    XMFLOAT3 half;
};

bool Overlap(const Body& a, const Body& b)
{
    auto sphereSphere = [](const Body& s0, const Body& s1) {
        const float dx = s0.center.x - s1.center.x;
        const float dy = s0.center.y - s1.center.y;
        const float dz = s0.center.z - s1.center.z;
        const float r = s0.radius + s1.radius;
        return dx * dx + dy * dy + dz * dz <= r * r;
    };
    auto aabbAabb = [](const Body& b0, const Body& b1) {
        return std::fabs(b0.center.x - b1.center.x) <= b0.half.x + b1.half.x
            && std::fabs(b0.center.y - b1.center.y) <= b0.half.y + b1.half.y
            && std::fabs(b0.center.z - b1.center.z) <= b0.half.z + b1.half.z;
    };
    auto sphereAabb = [](const Body& s, const Body& box) {
        const float cx = std::clamp(s.center.x, box.center.x - box.half.x, box.center.x + box.half.x);
        const float cy = std::clamp(s.center.y, box.center.y - box.half.y, box.center.y + box.half.y);
        const float cz = std::clamp(s.center.z, box.center.z - box.half.z, box.center.z + box.half.z);
        const float dx = s.center.x - cx;
        const float dy = s.center.y - cy;
        const float dz = s.center.z - cz;
        return dx * dx + dy * dy + dz * dz <= s.radius * s.radius;
    };

    if (a.shape == 0 && b.shape == 0) {
        return sphereSphere(a, b);
    }
    if (a.shape == 1 && b.shape == 1) {
        return aabbAabb(a, b);
    }
    return (a.shape == 0) ? sphereAabb(a, b) : sphereAabb(b, a);
}

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
            Body b;
            b.entity = arch.EntityAt(row);
            b.shape = col->shape;
            b.center = { wm->value._41, wm->value._42, wm->value._43 };
            // スケール近似: ワールド行列の各行ベクトル長
            const float sx = sqrtf(wm->value._11 * wm->value._11 + wm->value._12 * wm->value._12
                                   + wm->value._13 * wm->value._13);
            const float sy = sqrtf(wm->value._21 * wm->value._21 + wm->value._22 * wm->value._22
                                   + wm->value._23 * wm->value._23);
            const float sz = sqrtf(wm->value._31 * wm->value._31 + wm->value._32 * wm->value._32
                                   + wm->value._33 * wm->value._33);
            b.radius = col->radius * std::max(sx, std::max(sy, sz));
            b.half = { col->halfExtents.x * sx, col->halfExtents.y * sy, col->halfExtents.z * sz };
            bodies.push_back(b);
        }
    });
    std::sort(bodies.begin(), bodies.end(),
              [](const Body& a, const Body& b) { return a.entity.index < b.entity.index; });

    // ---- 総当たり判定 → 現 tick のペア集合 ----
    std::vector<uint64_t> pairs;
    for (size_t i = 0; i < bodies.size(); ++i) {
        for (size_t j = i + 1; j < bodies.size(); ++j) {
            if (Overlap(bodies[i], bodies[j])) {
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
