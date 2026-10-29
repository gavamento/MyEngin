// ABI の AddTorque / AddForceAtPosition / SampleTerrainHeight の実装本体 (M28b / M59k)。契約は PhysicsSystem.h。
// 慣性の導出はソルバと同じ scalar ヘルパー (PhysicsSystemInternal.h) を通す = 二義にならない
#include "Engine/Engine/Physics/Rigid/PhysicsSystem.h"
#include "Engine/Engine/Physics/Rigid/PhysicsSystemInternal.h"

#include <algorithm>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Physics/Rigid/Shapes.h"

namespace mye {

int ApplyTorqueWorld(World& world, EntityID e, MyeVec3 torque, float dt)
{
    auto* rb = world.GetComponent<RigidbodyComponent>(e);
    auto* lt = world.GetComponent<LocalTransform>(e);
    if (!rb || !lt || rb->isKinematic || rb->freezeRotation) {
        return 0;
    }
    rb->isSleeping = false; // M59h: トルクも起床トリガ
    rb->sleepTicks = 0;
    const auto* col = world.GetComponent<ColliderComponent>(e);
    // M59a2: 密度導出質量をソルバ収集と同じ関数で解決 (質量を二義にしない)
    const float mass = EffectiveMassWorld(world, e, *rb);
    ShapePose pose;
    if (col) {
        pose = shapes::MakePose(*col, lt->position, lt->rotation, lt->scale);
    }
    float ix, iy, iz;
    LocalInertiaDiag(col, pose, mass, ix, iy, iz);
    float invI[3][3];
    InvInertiaWorld(pose, ix, iy, iz, invI);
    float ox, oy, oz;
    MulInvI(invI, torque.x * dt, torque.y * dt, torque.z * dt, ox, oy, oz);
    rb->angularVelocity.x += ox;
    rb->angularVelocity.y += oy;
    rb->angularVelocity.z += oz;
    return 1;
}

int ApplyForceAtWorldPoint(World& world, EntityID e, MyeVec3 force, MyeVec3 worldPoint, float dt)
{
    auto* rb = world.GetComponent<RigidbodyComponent>(e);
    auto* lt = world.GetComponent<LocalTransform>(e);
    if (!rb || !lt || rb->isKinematic) {
        return 0;
    }
    rb->isSleeping = false; // M59h: 力を入れるスロットは必ず起こす
    rb->sleepTicks = 0;
    // M59a2: 質量はソルバと同じ関数で 1 回だけ解決する (並進と回転で食い違わせない)
    const float mass = EffectiveMassWorld(world, e, *rb);
    const float s = dt / mass;
    rb->velocity.x += force.x * s;
    rb->velocity.y += force.y * s;
    rb->velocity.z += force.z * s;
    if (rb->freezeRotation) {
        return 1; // 回転を凍らせたボディは端を押しても回らない (並進だけ入って成功)
    }
    const auto* col = world.GetComponent<ColliderComponent>(e);
    ShapePose pose;
    if (col) {
        pose = shapes::MakePose(*col, lt->position, lt->rotation, lt->scale);
    }
    // 質量中心のワールドオフセット (M59f1)。既定 (0,0,0) は**分岐で外す** —
    // `p + 0.0f` が -0.0f を +0.0f に化けさせるので「0 を足しても同じ」に依存しない
    float comx = 0.0f, comy = 0.0f, comz = 0.0f;
    if (rb->centerOfMass.x != 0.0f || rb->centerOfMass.y != 0.0f || rb->centerOfMass.z != 0.0f) {
        QuatRotate(lt->rotation.x, lt->rotation.y, lt->rotation.z, lt->rotation.w,
                   rb->centerOfMass.x * lt->scale.x, rb->centerOfMass.y * lt->scale.y,
                   rb->centerOfMass.z * lt->scale.z, comx, comy, comz);
    }
    const float rx = worldPoint.x - lt->position.x - comx;
    const float ry = worldPoint.y - lt->position.y - comy;
    const float rz = worldPoint.z - lt->position.z - comz;
    float tx, ty, tz;
    Cross(rx, ry, rz, force.x, force.y, force.z, tx, ty, tz);
    float ix, iy, iz;
    LocalInertiaDiag(col, pose, mass, ix, iy, iz);
    float invI[3][3];
    InvInertiaWorld(pose, ix, iy, iz, invI);
    float ox, oy, oz;
    MulInvI(invI, tx * dt, ty * dt, tz * dt, ox, oy, oz);
    rb->angularVelocity.x += ox;
    rb->angularVelocity.y += oy;
    rb->angularVelocity.z += oz;
    return 1;
}

int SampleTerrainHeightWorld(World& world, float x, float z, float* outHeight, MyeVec3* outNormal)
{
    // 収集 (RaycastWorld と同じ規約: WorldMatrix ベース、index 昇順に整列してから走査)
    struct TerrainTarget {
        EntityID entity;
        ShapePose pose;
    };
    std::vector<TerrainTarget> targets;
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
            if (col->shape != collidershape::kTerrain) {
                continue; // 地形コライダーだけが対象 (レイヤーマスクは見ない — 高さは幾何)
            }
            const auto* wm = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row));
            targets.push_back({ e, shapes::MakePoseFromMatrix(*col, wm->value) });
        }
    });
    std::sort(targets.begin(), targets.end(), [](const TerrainTarget& a, const TerrainTarget& b) {
        return a.entity.index < b.entity.index;
    });

    bool hit = false;
    float bestY = 0.0f, bnx = 0.0f, bny = 1.0f, bnz = 0.0f;
    for (const TerrainTarget& t : targets) {
        float minX, minY, minZ, maxX, maxY, maxZ;
        shapes::ComputeAabb(t.pose, minX, minY, minZ, maxX, maxY, maxZ);
        if (x < minX || x > maxX || z < minZ || z > maxZ) {
            continue; // XZ が地形の外 — 真下に撃っても当たらないので DDA ごと省く
        }
        // 天井の 1m 上から撃つ。ぴったり天井から始めると、最高点の頂点をかすめるレイが
        // 数値誤差で「開始点が既に表面の下」に落ちて外れることがある
        const float startY = maxY + 1.0f;
        const float span = (startY - minY) + 1.0f;
        float ht, nx, ny, nz;
        if (!shapes::Raycast(t.pose, x, startY, z, 0.0f, -1.0f, 0.0f, span, ht, nx, ny, nz)) {
            continue;
        }
        const float hitY = startY - ht;
        // 厳密 > + index 昇順走査 = 同高は低 index が残る (決定論)
        if (!hit || hitY > bestY) {
            hit = true;
            bestY = hitY;
            bnx = nx;
            bny = ny;
            bnz = nz;
        }
    }
    if (!hit) {
        return 0;
    }
    if (outHeight) {
        *outHeight = bestY;
    }
    if (outNormal) {
        *outNormal = { bnx, bny, bnz };
    }
    return 1;
}

} // namespace mye
