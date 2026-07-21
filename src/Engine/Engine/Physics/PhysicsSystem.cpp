#include "Engine/Engine/Physics/PhysicsSystem.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/Components.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/Physics/Shapes.h"

using namespace DirectX;

namespace mye {
namespace {

// 決定論のため全て scalar float 演算 (XMVECTOR SIMD を使わない = Debug/Release で同一ビット)。
// 形状判定は Physics/Shapes.cpp に統合 (M28a)。無回転 sphere/box は M20 とビット同一の
// fast-path を通る (Shapes.h 参照)。
constexpr float kGravity = -9.81f;      // m/s^2 (重力加速度、-Y)
constexpr int kSolverIterations = 8;    // 固定反復回数 (収束判定による早期終了はしない = 決定論)
constexpr float kPenetrationSlop = 0.0005f; // 微小めり込みは許容 (ジッタ抑制)

struct Body {
    EntityID entity;
    LocalTransform* lt = nullptr;   // 位置書き込み先 (ルート = ワールド位置)
    RigidbodyComponent* rb = nullptr; // null = 静的コライダー (動かない衝突面)
    bool solid = false;            // 衝突解決に参加するか (isTrigger==0)
    ShapePose pose;                // 形状 + 作業用ワールド位置 (pose.px/py/pz をソルバが更新)
    float vx = 0, vy = 0, vz = 0;  // 作業用速度
    float invMass = 0;             // 0 = 不動 (静的 / kinematic)
    float restitution = 0;
};

} // namespace

void PhysicsSystem::Update(World& world, float dt)
{
    // ---- 収集 (動的: Rigidbody + LocalTransform) ----
    std::vector<Body> bodies;
    const ComponentTypeId dynReq[] = { RigidbodyComponent::sTypeId, LocalTransform::sTypeId };
    world.ForEachArchetype(dynReq, [&](Archetype& arch) {
        const int ri = arch.FindTypeIndex(RigidbodyComponent::sTypeId);
        const int li = arch.FindTypeIndex(LocalTransform::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            if (!IsEntityActive(world, e)) {
                continue;
            }
            auto* rb = static_cast<RigidbodyComponent*>(arch.GetPtr(ri, row));
            auto* lt = static_cast<LocalTransform*>(arch.GetPtr(li, row));
            Body b;
            b.entity = e;
            b.lt = lt;
            b.rb = rb;
            b.pose.px = lt->position.x;
            b.pose.py = lt->position.y;
            b.pose.pz = lt->position.z;
            b.vx = rb->velocity.x;
            b.vy = rb->velocity.y;
            b.vz = rb->velocity.z;
            const float mass = (rb->mass > 0.0f) ? rb->mass : 1.0f;
            b.invMass = rb->isKinematic ? 0.0f : (1.0f / mass);
            b.restitution = rb->restitution;
            // コライダーがあり isTrigger==0 ならソリッド (衝突解決に参加)
            auto* col = world.GetComponent<ColliderComponent>(e);
            if (col && col->isTrigger == 0) {
                b.solid = true;
                b.pose = shapes::MakePose(*col, lt->position, lt->rotation, lt->scale);
            }
            bodies.push_back(b);
        }
    });

    // ---- 収集 (静的: Collider(isTrigger==0) + LocalTransform、Rigidbody 非所持) ----
    const ComponentTypeId colReq[] = { ColliderComponent::sTypeId, LocalTransform::sTypeId };
    world.ForEachArchetype(colReq, [&](Archetype& arch) {
        const int ci = arch.FindTypeIndex(ColliderComponent::sTypeId);
        const int li = arch.FindTypeIndex(LocalTransform::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            if (!IsEntityActive(world, e)) {
                continue;
            }
            if (world.HasComponent(e, RigidbodyComponent::sTypeId)) {
                continue; // 動的側で収集済み
            }
            auto* col = static_cast<const ColliderComponent*>(arch.GetPtr(ci, row));
            if (col->isTrigger != 0) {
                continue; // トリガーはソリッド衝突面でない (CollisionSystem がイベントを出す)
            }
            auto* lt = static_cast<const LocalTransform*>(arch.GetPtr(li, row));
            Body b;
            b.entity = e;
            b.solid = true;
            b.invMass = 0.0f; // 静的 = 不動
            b.pose = shapes::MakePose(*col, lt->position, lt->rotation, lt->scale);
            bodies.push_back(b);
        }
    });

    if (bodies.empty()) {
        return;
    }
    std::sort(bodies.begin(), bodies.end(),
              [](const Body& a, const Body& b) { return a.entity.index < b.entity.index; });

    // ---- 積分 (動的・非 kinematic のみ) ----
    for (Body& b : bodies) {
        if (b.invMass == 0.0f || !b.rb) {
            continue;
        }
        b.vy += kGravity * b.rb->gravityScale * dt;
        float damp = 1.0f - b.rb->linearDamping;
        if (damp < 0.0f) { damp = 0.0f; }
        b.vx *= damp; b.vy *= damp; b.vz *= damp;
        b.pose.px += b.vx * dt;
        b.pose.py += b.vy * dt;
        b.pose.pz += b.vz * dt;
    }

    // ---- 接触解決 (固定反復・index 順ペア = 決定論) ----
    const size_t n = bodies.size();
    for (int iter = 0; iter < kSolverIterations; ++iter) {
        for (size_t i = 0; i < n; ++i) {
            if (!bodies[i].solid) {
                continue;
            }
            for (size_t j = i + 1; j < n; ++j) {
                if (!bodies[j].solid) {
                    continue;
                }
                const float tim = bodies[i].invMass + bodies[j].invMass;
                if (tim == 0.0f) {
                    continue; // 両方静的
                }
                float nx, ny, nz, depth;
                if (!shapes::Collide(bodies[i].pose, bodies[j].pose, nx, ny, nz, depth)) {
                    continue;
                }
                // 位置補正 (slop を除いた分を逆質量比で分配)
                const float corr = std::max(depth - kPenetrationSlop, 0.0f);
                const float ci = corr * bodies[i].invMass / tim;
                const float cj = corr * bodies[j].invMass / tim;
                bodies[i].pose.px += nx * ci; bodies[i].pose.py += ny * ci;
                bodies[i].pose.pz += nz * ci;
                bodies[j].pose.px -= nx * cj; bodies[j].pose.py -= ny * cj;
                bodies[j].pose.pz -= nz * cj;
                // 速度応答 (法線方向、接近時のみ)
                const float rvx = bodies[i].vx - bodies[j].vx;
                const float rvy = bodies[i].vy - bodies[j].vy;
                const float rvz = bodies[i].vz - bodies[j].vz;
                const float vn = rvx * nx + rvy * ny + rvz * nz;
                if (vn < 0.0f) {
                    const float e = std::min(bodies[i].restitution, bodies[j].restitution);
                    const float jimp = -(1.0f + e) * vn / tim;
                    const float ii = jimp * bodies[i].invMass;
                    const float jj = jimp * bodies[j].invMass;
                    bodies[i].vx += nx * ii; bodies[i].vy += ny * ii; bodies[i].vz += nz * ii;
                    bodies[j].vx -= nx * jj; bodies[j].vy -= ny * jj; bodies[j].vz -= nz * jj;
                }
            }
        }
    }

    // ---- 書き戻し (動的ボディのみ) ----
    for (Body& b : bodies) {
        if (!b.rb) {
            continue;
        }
        b.lt->position.x = b.pose.px;
        b.lt->position.y = b.pose.py;
        b.lt->position.z = b.pose.pz;
        b.rb->velocity.x = b.vx;
        b.rb->velocity.y = b.vy;
        b.rb->velocity.z = b.vz;
    }
}

int RaycastWorld(World& world, MyeVec3 origin, MyeVec3 dir, float maxDist, MyeRaycastHit* outHit)
{
    // dir を正規化 (ゼロ長は無効)
    float dlen = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (dlen < 1e-8f) {
        return 0;
    }
    const float dx = dir.x / dlen, dy = dir.y / dlen, dz = dir.z / dlen;
    if (maxDist <= 0.0f) {
        maxDist = 1e9f;
    }

    // 収集 (index 昇順、WorldMatrix ベースのワールドポーズ)
    struct RayTarget {
        EntityID entity;
        ShapePose pose;
    };
    std::vector<RayTarget> targets;
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
            targets.push_back({ e, shapes::MakePoseFromMatrix(*col, wm->value) });
        }
    });
    std::sort(targets.begin(), targets.end(),
              [](const RayTarget& a, const RayTarget& b) { return a.entity.index < b.entity.index; });

    float bestT = maxDist;
    bool hit = false;
    EntityID bestE = kNullEntity;
    float bnx = 0, bny = 0, bnz = 0;
    for (const RayTarget& t : targets) {
        float ht, nx, ny, nz;
        const bool ok = shapes::Raycast(t.pose, origin.x, origin.y, origin.z, dx, dy, dz, bestT,
                                        ht, nx, ny, nz);
        if (ok && ht < bestT) { // 昇順走査 + 厳密 < なので同 t は低 index が残る (決定論)
            bestT = ht;
            bestE = t.entity;
            bnx = nx; bny = ny; bnz = nz;
            hit = true;
        }
    }
    if (!hit) {
        return 0;
    }
    if (outHit) {
        outHit->entity = { bestE.index, bestE.generation };
        outHit->point = { origin.x + dx * bestT, origin.y + dy * bestT, origin.z + dz * bestT };
        outHit->normal = { bnx, bny, bnz };
        outHit->distance = bestT;
    }
    return 1;
}

} // namespace mye
