#include "Engine/Engine/Physics/PhysicsSystem.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/Components.h"
#include "Engine/Core/World.h"

using namespace DirectX;

namespace mye {
namespace {

// 決定論のため全て scalar float 演算 (XMVECTOR SIMD を使わない = Debug/Release で同一ビット)。
constexpr float kGravity = -9.81f;      // m/s^2 (重力加速度、-Y)
constexpr int kSolverIterations = 8;    // 固定反復回数 (収束判定による早期終了はしない = 決定論)
constexpr float kPenetrationSlop = 0.0005f; // 微小めり込みは許容 (ジッタ抑制)

struct Body {
    EntityID entity;
    LocalTransform* lt = nullptr;   // 位置書き込み先 (ルート = ワールド位置)
    RigidbodyComponent* rb = nullptr; // null = 静的コライダー (動かない衝突面)
    int32_t shape = 0;              // 0=sphere 1=aabb
    bool solid = false;            // 衝突解決に参加するか (isTrigger==0)
    float px = 0, py = 0, pz = 0;  // 作業用ワールド位置
    float vx = 0, vy = 0, vz = 0;  // 作業用速度
    float radius = 0;              // ワールドスケール適用済み (sphere)
    float hx = 0, hy = 0, hz = 0;  // ワールドスケール適用済み half extents (aabb)
    float invMass = 0;             // 0 = 不動 (静的 / kinematic)
    float restitution = 0;
};

// col の shape/extents を lt のワールドスケールで拡大して b に書く
void FillShape(Body& b, const ColliderComponent* col, const LocalTransform* lt)
{
    b.shape = col->shape;
    const float sx = std::fabs(lt->scale.x);
    const float sy = std::fabs(lt->scale.y);
    const float sz = std::fabs(lt->scale.z);
    b.radius = col->radius * std::max(sx, std::max(sy, sz));
    b.hx = col->halfExtents.x * sx;
    b.hy = col->halfExtents.y * sy;
    b.hz = col->halfExtents.z * sz;
}

// a と b の貫通を判定。ヒットで true、normal は b→a 方向 (a を +normal へ押し出す)、depth は貫通量。
bool Collide(const Body& a, const Body& b, float& nx, float& ny, float& nz, float& depth)
{
    // sphere-sphere
    if (a.shape == 0 && b.shape == 0) {
        const float dx = a.px - b.px, dy = a.py - b.py, dz = a.pz - b.pz;
        const float r = a.radius + b.radius;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 >= r * r) {
            return false;
        }
        const float d = std::sqrt(d2);
        if (d > 1e-6f) {
            nx = dx / d; ny = dy / d; nz = dz / d;
        } else {
            nx = 0; ny = 1; nz = 0; // 中心一致: 任意方向 (上向き)
        }
        depth = r - d;
        return true;
    }
    // aabb-aabb (ワールド軸平行、回転無視)
    if (a.shape == 1 && b.shape == 1) {
        const float ox = (a.hx + b.hx) - std::fabs(a.px - b.px);
        const float oy = (a.hy + b.hy) - std::fabs(a.py - b.py);
        const float oz = (a.hz + b.hz) - std::fabs(a.pz - b.pz);
        if (ox <= 0 || oy <= 0 || oz <= 0) {
            return false;
        }
        // 最小オーバーラップ軸を分離軸に選ぶ
        nx = ny = nz = 0;
        if (ox <= oy && ox <= oz) {
            nx = (a.px >= b.px) ? 1.0f : -1.0f;
            depth = ox;
        } else if (oy <= oz) {
            ny = (a.py >= b.py) ? 1.0f : -1.0f;
            depth = oy;
        } else {
            nz = (a.pz >= b.pz) ? 1.0f : -1.0f;
            depth = oz;
        }
        return true;
    }
    // sphere-aabb (混在)。s=球, box=箱。normal は box→s 方向で求め、a が箱なら反転する。
    const bool aIsSphere = (a.shape == 0);
    const Body& s = aIsSphere ? a : b;
    const Body& box = aIsSphere ? b : a;
    const float minx = box.px - box.hx, maxx = box.px + box.hx;
    const float miny = box.py - box.hy, maxy = box.py + box.hy;
    const float minz = box.pz - box.hz, maxz = box.pz + box.hz;
    const float cx = std::clamp(s.px, minx, maxx);
    const float cy = std::clamp(s.py, miny, maxy);
    const float cz = std::clamp(s.pz, minz, maxz);
    const float dx = s.px - cx, dy = s.py - cy, dz = s.pz - cz;
    const float d2 = dx * dx + dy * dy + dz * dz;
    if (d2 >= s.radius * s.radius) {
        return false;
    }
    float snx, sny, snz; // box→sphere 方向
    if (d2 > 1e-12f) {
        const float d = std::sqrt(d2);
        snx = dx / d; sny = dy / d; snz = dz / d;
        depth = s.radius - d;
    } else {
        // 球中心が箱内部: 最も近い面へ押し出す
        const float dxp = maxx - s.px, dxn = s.px - minx;
        const float dyp = maxy - s.py, dyn = s.py - miny;
        const float dzp = maxz - s.pz, dzn = s.pz - minz;
        float best = dxp; snx = 1; sny = 0; snz = 0;
        auto consider = [&](float dist, float ax, float ay, float az) {
            if (dist < best) { best = dist; snx = ax; sny = ay; snz = az; }
        };
        consider(dxn, -1, 0, 0);
        consider(dyp, 0, 1, 0);
        consider(dyn, 0, -1, 0);
        consider(dzp, 0, 0, 1);
        consider(dzn, 0, 0, -1);
        depth = s.radius + best;
    }
    // normal を b→a に揃える: a が球なら box→sphere = b→a のまま、a が箱なら反転
    if (aIsSphere) {
        nx = snx; ny = sny; nz = snz;
    } else {
        nx = -snx; ny = -sny; nz = -snz;
    }
    return true;
}

// ---- レイキャストの形状交差 ----

bool RaySphere(float ox, float oy, float oz, float dx, float dy, float dz,
               float cx, float cy, float cz, float r, float maxDist, float& outT,
               float& nx, float& ny, float& nz)
{
    const float lx = ox - cx, ly = oy - cy, lz = oz - cz;
    const float b = lx * dx + ly * dy + lz * dz;
    const float c = lx * lx + ly * ly + lz * lz - r * r;
    const float disc = b * b - c;
    if (disc < 0) {
        return false;
    }
    const float sq = std::sqrt(disc);
    float t = -b - sq;
    if (t < 0) {
        t = -b + sq; // 内部始点
    }
    if (t < 0 || t > maxDist) {
        return false;
    }
    outT = t;
    const float hx = ox + dx * t, hy = oy + dy * t, hz = oz + dz * t;
    const float inv = (r > 1e-6f) ? 1.0f / r : 0.0f;
    nx = (hx - cx) * inv; ny = (hy - cy) * inv; nz = (hz - cz) * inv;
    return true;
}

bool RayAabb(float ox, float oy, float oz, float dx, float dy, float dz,
             float cx, float cy, float cz, float hx, float hy, float hz, float maxDist,
             float& outT, float& nx, float& ny, float& nz)
{
    const float o[3] = { ox, oy, oz };
    const float d[3] = { dx, dy, dz };
    const float lo[3] = { cx - hx, cy - hy, cz - hz };
    const float hi[3] = { cx + hx, cy + hy, cz + hz };
    float tmin = 0.0f, tmax = maxDist;
    int axis = 0;
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(d[i]) < 1e-8f) {
            if (o[i] < lo[i] || o[i] > hi[i]) {
                return false; // レイが軸に平行でスラブ外
            }
            continue;
        }
        const float inv = 1.0f / d[i];
        float t0 = (lo[i] - o[i]) * inv;
        float t1 = (hi[i] - o[i]) * inv;
        if (t0 > t1) {
            std::swap(t0, t1);
        }
        if (t0 > tmin) {
            tmin = t0;
            axis = i;
        }
        if (t1 < tmax) {
            tmax = t1;
        }
        if (tmin > tmax) {
            return false;
        }
    }
    outT = tmin;
    nx = ny = nz = 0;
    const float sign = (d[axis] > 0) ? -1.0f : 1.0f; // 入射面の外向き法線
    if (axis == 0) { nx = sign; } else if (axis == 1) { ny = sign; } else { nz = sign; }
    return true;
}

// レイキャスト対象の 1 コライダー (WorldMatrix ベースのワールド中心 + スケール)
struct RayTarget {
    EntityID entity;
    int32_t shape;
    float cx, cy, cz;
    float radius;
    float hx, hy, hz;
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
            b.px = lt->position.x; b.py = lt->position.y; b.pz = lt->position.z;
            b.vx = rb->velocity.x; b.vy = rb->velocity.y; b.vz = rb->velocity.z;
            const float mass = (rb->mass > 0.0f) ? rb->mass : 1.0f;
            b.invMass = rb->isKinematic ? 0.0f : (1.0f / mass);
            b.restitution = rb->restitution;
            // コライダーがあり isTrigger==0 ならソリッド (衝突解決に参加)
            auto* col = world.GetComponent<ColliderComponent>(e);
            if (col && col->isTrigger == 0) {
                b.solid = true;
                FillShape(b, col, lt);
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
            b.px = lt->position.x; b.py = lt->position.y; b.pz = lt->position.z;
            FillShape(b, col, lt);
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
        b.px += b.vx * dt;
        b.py += b.vy * dt;
        b.pz += b.vz * dt;
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
                if (!Collide(bodies[i], bodies[j], nx, ny, nz, depth)) {
                    continue;
                }
                // 位置補正 (slop を除いた分を逆質量比で分配)
                const float corr = std::max(depth - kPenetrationSlop, 0.0f);
                const float ci = corr * bodies[i].invMass / tim;
                const float cj = corr * bodies[j].invMass / tim;
                bodies[i].px += nx * ci; bodies[i].py += ny * ci; bodies[i].pz += nz * ci;
                bodies[j].px -= nx * cj; bodies[j].py -= ny * cj; bodies[j].pz -= nz * cj;
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
        b.lt->position.x = b.px; b.lt->position.y = b.py; b.lt->position.z = b.pz;
        b.rb->velocity.x = b.vx; b.rb->velocity.y = b.vy; b.rb->velocity.z = b.vz;
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

    // 収集 (index 昇順、WorldMatrix ベースのワールド中心 + スケール)
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
            RayTarget t;
            t.entity = e;
            t.shape = col->shape;
            t.cx = wm->value._41; t.cy = wm->value._42; t.cz = wm->value._43;
            const float sx = std::sqrt(wm->value._11 * wm->value._11 + wm->value._12 * wm->value._12
                                       + wm->value._13 * wm->value._13);
            const float sy = std::sqrt(wm->value._21 * wm->value._21 + wm->value._22 * wm->value._22
                                       + wm->value._23 * wm->value._23);
            const float sz = std::sqrt(wm->value._31 * wm->value._31 + wm->value._32 * wm->value._32
                                       + wm->value._33 * wm->value._33);
            t.radius = col->radius * std::max(sx, std::max(sy, sz));
            t.hx = col->halfExtents.x * sx;
            t.hy = col->halfExtents.y * sy;
            t.hz = col->halfExtents.z * sz;
            targets.push_back(t);
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
        bool ok;
        if (t.shape == 0) {
            ok = RaySphere(origin.x, origin.y, origin.z, dx, dy, dz, t.cx, t.cy, t.cz, t.radius,
                           bestT, ht, nx, ny, nz);
        } else {
            ok = RayAabb(origin.x, origin.y, origin.z, dx, dy, dz, t.cx, t.cy, t.cz, t.hx, t.hy,
                         t.hz, bestT, ht, nx, ny, nz);
        }
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
