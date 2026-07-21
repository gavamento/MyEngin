#include <algorithm>
#include <cmath>
#include <vector>

#include "Engine/Core/Components.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/Physics/PhysicsSystem.h"
#include "Engine/Engine/Physics/Shapes.h"

// 空間クエリ (M28c、ABI OverlapSphere/OverlapBox/SphereCast の実装本体)。
// 決定論契約: 全て scalar float 演算、収集は entity.index 昇順、反復は固定回数。
// RaycastWorld と同じく WorldMatrix ベースでトリガー含む全コライダーが対象 (汎用クエリ)。

namespace mye {
namespace {

struct QueryTarget {
    EntityID entity;
    ShapePose pose;
};

// 全コライダーのワールドポーズを index 昇順で収集 (RaycastWorld と同一規約)
void CollectTargets(World& world, std::vector<QueryTarget>& out)
{
    const ComponentTypeId req[] = { ColliderComponent::sTypeId, WorldMatrixComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int ci = arch.FindTypeIndex(ColliderComponent::sTypeId);
        const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            if (!IsEntityActive(world, e)) {
                continue;
            }
            const auto* col = static_cast<const ColliderComponent*>(arch.GetPtr(ci, row));
            const auto* wm = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row));
            out.push_back({ e, shapes::MakePoseFromMatrix(*col, wm->value) });
        }
    });
    std::sort(out.begin(), out.end(), [](const QueryTarget& a, const QueryTarget& b) {
        return a.entity.index < b.entity.index;
    });
}

// probe と重なる全エンティティを列挙する共通本体
int OverlapWorld(World& world, const ShapePose& probe, MyeEntityId* outEntities, int maxCount)
{
    std::vector<QueryTarget> targets;
    CollectTargets(world, targets);
    int total = 0;
    for (const QueryTarget& t : targets) {
        if (!shapes::Overlap(probe, t.pose)) {
            continue;
        }
        if (outEntities && total < maxCount) {
            outEntities[total] = { t.entity.index, t.entity.generation };
        }
        ++total; // 戻り値は切り捨て前の総ヒット数
    }
    return total;
}

} // namespace

int OverlapSphereWorld(World& world, MyeVec3 center, float radius, MyeEntityId* outEntities,
                       int maxCount)
{
    ColliderComponent probe;
    probe.shape = 0;
    probe.radius = radius;
    const ShapePose pose = shapes::MakePose(probe, { center.x, center.y, center.z },
                                            { 0, 0, 0, 1 }, { 1, 1, 1 });
    return OverlapWorld(world, pose, outEntities, maxCount);
}

int OverlapBoxWorld(World& world, MyeVec3 center, MyeVec3 halfExtents, MyeQuat rotation,
                    MyeEntityId* outEntities, int maxCount)
{
    ColliderComponent probe;
    probe.shape = 1;
    probe.halfExtents = { halfExtents.x, halfExtents.y, halfExtents.z };
    const ShapePose pose = shapes::MakePose(probe, { center.x, center.y, center.z },
                                            { rotation.x, rotation.y, rotation.z, rotation.w },
                                            { 1, 1, 1 });
    return OverlapWorld(world, pose, outEntities, maxCount);
}

int SphereCastWorld(World& world, MyeVec3 origin, MyeVec3 dir, float radius, float maxDist,
                    MyeRaycastHit* outHit)
{
    const float dlen = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (dlen < 1e-8f || radius <= 0.0f) {
        // 半径 0 は通常の Raycast と等価
        return (radius <= 0.0f) ? RaycastWorld(world, origin, dir, maxDist, outHit) : 0;
    }
    const float dx = dir.x / dlen, dy = dir.y / dlen, dz = dir.z / dlen;
    if (maxDist <= 0.0f) {
        maxDist = 1e9f;
    }

    std::vector<QueryTarget> targets;
    CollectTargets(world, targets);

    float bestT = maxDist;
    bool hit = false;
    EntityID bestE = kNullEntity;
    float bpx = 0, bpy = 0, bpz = 0;
    float bnx = 0, bny = 0, bnz = 0;

    for (const QueryTarget& t : targets) {
        if (t.pose.shape == 0 || t.pose.shape == 2) {
            // sphere / capsule: 半径を radius だけ膨らませた形状へのレイ = 掃引球の解析解
            ShapePose inflated = t.pose;
            inflated.radius += radius;
            float ht, nx, ny, nz;
            if (!shapes::Raycast(inflated, origin.x, origin.y, origin.z, dx, dy, dz, bestT, ht,
                                 nx, ny, nz)
                || ht >= bestT) { // 昇順走査 + 厳密 < なので同 t は低 index が残る (決定論)
                continue;
            }
            bestT = ht;
            bestE = t.entity;
            // 掃引球の中心位置から元形状の表面点を復元 (法線は膨張面と同方向)
            const float cx = origin.x + dx * ht, cy = origin.y + dy * ht,
                        cz = origin.z + dz * ht;
            bpx = cx - nx * radius;
            bpy = cy - ny * radius;
            bpz = cz - nz * radius;
            bnx = nx; bny = ny; bnz = nz;
            hit = true;
        } else {
            // box: 保守的前進 (固定 32 回 = 決定論。ヒット後も回数は消化する)
            float ct = 0.0f;
            bool found = false;
            float foundT = 0.0f;
            for (int step = 0; step < 32; ++step) {
                if (found || ct > bestT) {
                    continue; // 固定回数消化 (早期 break しない)
                }
                const float px = origin.x + dx * ct;
                const float py = origin.y + dy * ct;
                const float pz = origin.z + dz * ct;
                const float d = shapes::DistanceToShape(t.pose, px, py, pz) - radius;
                if (d < 1e-4f) {
                    found = true;
                    foundT = ct;
                } else {
                    ct += d;
                }
            }
            if (!found || foundT >= bestT) {
                continue;
            }
            const float px = origin.x + dx * foundT;
            const float py = origin.y + dy * foundT;
            const float pz = origin.z + dz * foundT;
            float qx, qy, qz;
            shapes::ClosestPointOnShape(t.pose, px, py, pz, qx, qy, qz);
            const float vx = px - qx, vy = py - qy, vz = pz - qz;
            const float vlen = std::sqrt(vx * vx + vy * vy + vz * vz);
            if (vlen < 1e-6f) {
                continue; // 始点が形状内 (縮退) は報告しない
            }
            bestT = foundT;
            bestE = t.entity;
            bpx = qx; bpy = qy; bpz = qz;
            bnx = vx / vlen; bny = vy / vlen; bnz = vz / vlen;
            hit = true;
        }
    }
    if (!hit) {
        return 0;
    }
    if (outHit) {
        outHit->entity = { bestE.index, bestE.generation };
        outHit->point = { bpx, bpy, bpz };
        outHit->normal = { bnx, bny, bnz };
        outHit->distance = bestT;
    }
    return 1;
}

} // namespace mye
