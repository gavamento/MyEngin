#include <algorithm>
#include <cmath>
#include <vector>

#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Physics/Rigid/PhysicsSystem.h"
#include "Engine/Engine/Physics/Rigid/Shapes.h"

// 空間クエリ (M28c、ABI OverlapSphere/OverlapBox/SphereCast の実装本体)。
// 決定論契約: 全て scalar float 演算、収集は entity.index 昇順、反復は固定回数。
// RaycastWorld と同じく WorldMatrix ベースでトリガー含む全コライダーが対象 (汎用クエリ)。

namespace mye {
namespace {

struct QueryTarget {
    EntityID entity;
    ShapePose pose;
};

// 全コライダーのワールドポーズを index 昇順で収集 (RaycastWorld と同一規約)。
// mask (M36a): マスク外レイヤーは収集段階で除外 (既定 = 全レイヤー = 従来挙動)
void CollectTargets(World& world, uint32_t mask, std::vector<QueryTarget>& out)
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
            if (!shapes::LayerHit(mask, col->layer)) {
                continue;
            }
            const auto* wm = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row));
            const ShapePose pose = shapes::MakePoseFromMatrix(*col, wm->value);
            if (col->shape == collidershape::kEdge2D) {
                // M94h: 辺の列は線分ごとの箱になる。同じ持ち主の箱は線分の並びで隣り合う
                std::vector<ShapePose> boxes;
                shapes::ExpandEdge2D(*col, pose, boxes);
                for (const ShapePose& box : boxes) {
                    out.push_back({ e, box });
                }
                continue;
            }
            out.push_back({ e, pose });
        }
    });
    // 安定ソート: 同じ持ち主の箱 (M94h) が収集順のまま残る。持ち主が一意なら std::sort と同じ並び
    std::stable_sort(out.begin(), out.end(), [](const QueryTarget& a, const QueryTarget& b) {
        return a.entity.index < b.entity.index;
    });
}

// probe と重なる全エンティティを列挙する共通本体
int OverlapWorld(World& world, const ShapePose& probe, MyeEntityId* outEntities, int maxCount,
                 uint32_t mask)
{
    std::vector<QueryTarget> targets;
    CollectTargets(world, mask, targets);
    int total = 0;
    EntityID lastHit = kNullEntity;
    for (const QueryTarget& t : targets) {
        if (!shapes::Overlap(probe, t.pose)) {
            continue;
        }
        if (total > 0 && t.entity.index == lastHit.index && t.entity.generation == lastHit.generation) {
            continue; // 辺の列 (M94h) は複数の箱で同じ持ち主を二重に数えない
        }
        lastHit = t.entity;
        if (outEntities && total < maxCount) {
            outEntities[total] = { t.entity.index, t.entity.generation };
        }
        ++total; // 戻り値は切り捨て前の総ヒット数
    }
    return total;
}

} // namespace

int OverlapSphereWorld(World& world, MyeVec3 center, float radius, MyeEntityId* outEntities,
                       int maxCount, uint32_t mask)
{
    ColliderComponent probe;
    probe.shape = collidershape::kSphere;
    probe.radius = radius;
    const ShapePose pose = shapes::MakePose(probe, { center.x, center.y, center.z },
                                            { 0, 0, 0, 1 }, { 1, 1, 1 });
    return OverlapWorld(world, pose, outEntities, maxCount, mask);
}

int OverlapBoxWorld(World& world, MyeVec3 center, MyeVec3 halfExtents, MyeQuat rotation,
                    MyeEntityId* outEntities, int maxCount, uint32_t mask)
{
    ColliderComponent probe;
    probe.shape = collidershape::kBox;
    probe.halfExtents = { halfExtents.x, halfExtents.y, halfExtents.z };
    const ShapePose pose = shapes::MakePose(probe, { center.x, center.y, center.z },
                                            { rotation.x, rotation.y, rotation.z, rotation.w },
                                            { 1, 1, 1 });
    return OverlapWorld(world, pose, outEntities, maxCount, mask);
}

int SphereCastWorld(World& world, MyeVec3 origin, MyeVec3 dir, float radius, float maxDist,
                    MyeRaycastHit* outHit, uint32_t mask)
{
    const float dlen = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (dlen < 1e-8f || radius <= 0.0f) {
        // 半径 0 は通常の Raycast と等価
        return (radius <= 0.0f) ? RaycastWorld(world, origin, dir, maxDist, outHit, mask) : 0;
    }
    const float dx = dir.x / dlen, dy = dir.y / dlen, dz = dir.z / dlen;
    if (maxDist <= 0.0f) {
        maxDist = 1e9f;
    }

    std::vector<QueryTarget> targets;
    CollectTargets(world, mask, targets);

    float bestT = maxDist;
    bool hit = false;
    EntityID bestE = kNullEntity;
    float bpx = 0, bpy = 0, bpz = 0;
    float bnx = 0, bny = 0, bnz = 0;

    for (const QueryTarget& t : targets) {
        if (t.pose.shape == collidershape::kSphere || t.pose.shape == collidershape::kCapsule) {
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

int BoxCastWorld(World& world, MyeVec3 center, MyeVec3 halfExtents, MyeVec3 dir, float maxDist,
                 MyeRaycastHit* outHit, uint32_t mask, RaycastSkipFn skip, const void* skipUser)
{
    const float dlen = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (dlen < 1e-8f) {
        return 0;
    }
    if (!(halfExtents.x > 0.0f && halfExtents.y > 0.0f && halfExtents.z > 0.0f)) {
        // 体積の無い箱は通常のレイキャストと等価 (SphereCastWorld の半径 0 と同じ扱い)
        return RaycastWorld(world, center, dir, maxDist, outHit, mask, skip, skipUser);
    }
    const float dx = dir.x / dlen, dy = dir.y / dlen, dz = dir.z / dlen;
    if (maxDist <= 0.0f) {
        maxDist = 1e9f;
    }

    std::vector<QueryTarget> targets;
    CollectTargets(world, mask, targets);

    ColliderComponent probe;
    probe.shape = collidershape::kBox;
    probe.halfExtents = { halfExtents.x, halfExtents.y, halfExtents.z };
    const ShapePose startBox = shapes::MakePose(probe, { center.x, center.y, center.z }, { 0, 0, 0, 1 },
                                                { 1, 1, 1 });

    float bestT = maxDist;
    bool hit = false;
    EntityID bestE = kNullEntity;
    float bpx = 0, bpy = 0, bpz = 0;
    float bnx = 0, bny = 0, bnz = 0;
    for (const QueryTarget& t : targets) {
        if (skip != nullptr && skip(world, t.entity, skipUser)) {
            continue;
        }
        if (shapes::Overlap(startBox, t.pose)) {
            // 始点で既に重なっている: 距離 0・法線は進行方向の逆 (Unity の BoxCast と同じ)。
            // 昇順走査 + 厳密 < なので、0 に最初に届いた低 index が残る
            if (0.0f < bestT) {
                bestT = 0.0f;
                bestE = t.entity;
                bpx = center.x; bpy = center.y; bpz = center.z;
                bnx = -dx; bny = -dy; bnz = -dz;
                hit = true;
            }
            continue;
        }
        // 箱の 8 頂点・12 辺の中点・6 面の中心・中心の 27 点から進行方向へレイを撃ち、最初に当たった距離を採る。
        // 形状の突端が撃つ点の隙間に入るケースは取りこぼす近似 (固定 27 本 = 決定論)
        for (int ix = -1; ix <= 1; ++ix) {
            for (int iy = -1; iy <= 1; ++iy) {
                for (int iz = -1; iz <= 1; ++iz) {
                    const float ox = center.x + static_cast<float>(ix) * halfExtents.x;
                    const float oy = center.y + static_cast<float>(iy) * halfExtents.y;
                    const float oz = center.z + static_cast<float>(iz) * halfExtents.z;
                    float ht, nx, ny, nz;
                    if (!shapes::Raycast(t.pose, ox, oy, oz, dx, dy, dz, bestT, ht, nx, ny, nz)
                        || ht >= bestT) {
                        continue;
                    }
                    bestT = ht;
                    bestE = t.entity;
                    bpx = ox + dx * ht; bpy = oy + dy * ht; bpz = oz + dz * ht;
                    bnx = nx; bny = ny; bnz = nz;
                    hit = true;
                }
            }
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
