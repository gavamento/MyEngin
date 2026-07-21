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
// 形状判定は Physics/Shapes.cpp に統合 (M28a)。M28b で回転剛体 (角速度・慣性テンソル)、
// クーロン摩擦、接触マニフォールド (最大 4 点)、反発速度閾値を追加。
constexpr float kGravity = -9.81f;      // m/s^2 (重力加速度、-Y)
constexpr int kSolverIterations = 8;    // 固定反復回数 (収束判定による早期終了はしない = 決定論)
constexpr float kPenetrationSlop = 0.0005f; // 微小めり込みは許容 (ジッタ抑制)
// |vn| がこの閾値未満の接触は反発 0 扱い (micro-bounce 除去 = 静止安定の柱。~2g·dt)
constexpr float kRestitutionVelThreshold = 0.3f;

struct Body {
    EntityID entity;
    LocalTransform* lt = nullptr;   // 位置/回転書き込み先 (ルート = ワールド位置)
    RigidbodyComponent* rb = nullptr; // null = 静的コライダー (動かない衝突面)
    const ColliderComponent* col = nullptr; // ソリッド形状 (null = コライダー無し動的ボディ)
    bool solid = false;            // 衝突解決に参加するか (isTrigger==0)
    bool freezeRot = true;         // 回転積分・角応答をしない (静的 / kinematic / freezeRotation)
    ShapePose pose;                // 形状 + 作業用ワールド位置 (pose.px/py/pz をソルバが更新)
    XMFLOAT3 scale = { 1, 1, 1 };  // pose 再構築用 (回転積分後に基底を作り直す)
    float qx = 0, qy = 0, qz = 0, qw = 1; // 作業用姿勢 (ワールド)
    float vx = 0, vy = 0, vz = 0;  // 作業用速度
    float wx = 0, wy = 0, wz = 0;  // 作業用角速度 (rad/s、ワールド)
    float invMass = 0;             // 0 = 不動 (静的 / kinematic)
    float invI[3][3] = {};         // ワールド逆慣性テンソル (freezeRot は零行列)
    float restitution = 0;
    float friction = 0.5f;         // クーロン摩擦係数 (Collider から。ペアは sqrt(μa·μb))
};

// 形状のローカル主軸慣性 (対角、質量 m)。col null は半径 0.5 の球扱い。
// pose の寸法はワールドスケール適用済みなのでそのまま使う
void LocalInertiaDiag(const ColliderComponent* col, const ShapePose& pose, float m, float& ix,
                      float& iy, float& iz)
{
    if (!col || pose.shape == 0) {
        const float r = col ? pose.radius : 0.5f;
        const float i = 0.4f * m * r * r; // 2/5 m r²
        ix = iy = iz = i;
        return;
    }
    if (pose.shape == 1) {
        // box (全辺 = 2h): I = m/12 (d1² + d2²) = m/3 (h1² + h2²)
        ix = m * (pose.hy * pose.hy + pose.hz * pose.hz) / 3.0f;
        iy = m * (pose.hx * pose.hx + pose.hz * pose.hz) / 3.0f;
        iz = m * (pose.hx * pose.hx + pose.hy * pose.hy) / 3.0f;
        return;
    }
    // capsule: 円柱 + 半球×2 の合成 (体積比で質量配分)。軸はローカル Y
    const float r = pose.radius;
    const float H = 2.0f * pose.halfSeg;
    const float r2 = r * r;
    const float vc = 3.14159265f * r2 * H;  // 円柱体積
    const float vs = 4.18879020f * r2 * r;  // 球体積 (4/3 π r³)
    const float vt = vc + vs;
    const float mc = (vt > 1e-12f) ? m * (vc / vt) : 0.0f;
    const float ms = m - mc;
    const float axial = mc * r2 * 0.5f + ms * r2 * 0.4f;
    const float trans = mc * (H * H / 12.0f + r2 * 0.25f)
                      + ms * (r2 * 0.4f + H * H * 0.25f + 0.375f * H * r);
    ix = trans;
    iy = axial;
    iz = trans;
}

// I⁻¹_world = Σ_k (1/I_k)·b_k·b_kᵀ (b_k = pose のワールド基底 = 主軸方向)
void InvInertiaWorld(const ShapePose& pose, float ix, float iy, float iz, float out[3][3])
{
    const float inv[3] = { (ix > 1e-12f) ? 1.0f / ix : 0.0f, (iy > 1e-12f) ? 1.0f / iy : 0.0f,
                           (iz > 1e-12f) ? 1.0f / iz : 0.0f };
    const float* B[3] = { pose.bx, pose.by, pose.bz };
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            out[r][c] = inv[0] * B[0][r] * B[0][c] + inv[1] * B[1][r] * B[1][c]
                      + inv[2] * B[2][r] * B[2][c];
        }
    }
}

void MulInvI(const float m[3][3], float x, float y, float z, float& ox, float& oy, float& oz)
{
    ox = m[0][0] * x + m[0][1] * y + m[0][2] * z;
    oy = m[1][0] * x + m[1][1] * y + m[1][2] * z;
    oz = m[2][0] * x + m[2][1] * y + m[2][2] * z;
}

void Cross(float ax, float ay, float az, float bx, float by, float bz, float& ox, float& oy,
           float& oz)
{
    ox = ay * bz - az * by;
    oy = az * bx - ax * bz;
    oz = ax * by - ay * bx;
}

// 接触点方向 d の有効質量逆数: invMass + d·((I⁻¹(r×d))×r)
float EffectiveMassInv(const Body& b, float rx, float ry, float rz, float dx, float dy, float dz)
{
    float cx, cy, cz;
    Cross(rx, ry, rz, dx, dy, dz, cx, cy, cz);
    float ix, iy, iz;
    MulInvI(b.invI, cx, cy, cz, ix, iy, iz);
    float ox, oy, oz;
    Cross(ix, iy, iz, rx, ry, rz, ox, oy, oz);
    return b.invMass + dx * ox + dy * oy + dz * oz;
}

// インパルス j·(dx,dy,dz) を接触点 (r = 点 − 重心) に適用
void ApplyImpulse(Body& b, float rx, float ry, float rz, float jx, float jy, float jz, float sign)
{
    b.vx += jx * b.invMass * sign;
    b.vy += jy * b.invMass * sign;
    b.vz += jz * b.invMass * sign;
    float cx, cy, cz;
    Cross(rx, ry, rz, jx * sign, jy * sign, jz * sign, cx, cy, cz);
    float ix, iy, iz;
    MulInvI(b.invI, cx, cy, cz, ix, iy, iz);
    b.wx += ix;
    b.wy += iy;
    b.wz += iz;
}

} // namespace

void PhysicsSystem::Update(World& world, float dt, std::vector<SolidContact>* outContacts)
{
    if (outContacts) {
        outContacts->clear();
    }
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
            b.scale = lt->scale;
            b.qx = lt->rotation.x;
            b.qy = lt->rotation.y;
            b.qz = lt->rotation.z;
            b.qw = lt->rotation.w;
            b.vx = rb->velocity.x;
            b.vy = rb->velocity.y;
            b.vz = rb->velocity.z;
            b.wx = rb->angularVelocity.x;
            b.wy = rb->angularVelocity.y;
            b.wz = rb->angularVelocity.z;
            const float mass = (rb->mass > 0.0f) ? rb->mass : 1.0f;
            b.invMass = rb->isKinematic ? 0.0f : (1.0f / mass);
            b.restitution = rb->restitution;
            b.freezeRot = (rb->freezeRotation != 0) || rb->isKinematic;
            // コライダーがあり isTrigger==0 ならソリッド (衝突解決に参加)
            auto* col = world.GetComponent<ColliderComponent>(e);
            if (col && col->isTrigger == 0) {
                b.solid = true;
                b.col = col;
                b.friction = col->friction;
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
            b.friction = col->friction;
            b.pose = shapes::MakePose(*col, lt->position, lt->rotation, lt->scale);
            bodies.push_back(b);
        }
    });

    if (bodies.empty()) {
        return;
    }
    std::sort(bodies.begin(), bodies.end(),
              [](const Body& a, const Body& b) { return a.entity.index < b.entity.index; });

    // ---- 速度積分 (動的・非 kinematic のみ)。位置はまだ動かさない ----
    // M28b で「速度積分 → ソルバ → 位置積分」の順に変更 (Box2D 流)。摩擦や法線インパルスで
    // 静止した速度がそのまま位置積分に使われるため、静止接触の毎 tick クリープが出ない
    for (Body& b : bodies) {
        if (b.invMass == 0.0f || !b.rb) {
            continue;
        }
        b.vy += kGravity * b.rb->gravityScale * dt;
        float damp = 1.0f - b.rb->linearDamping;
        if (damp < 0.0f) { damp = 0.0f; }
        b.vx *= damp; b.vy *= damp; b.vz *= damp;
        if (!b.freezeRot) {
            float adamp = 1.0f - b.rb->angularDamping;
            if (adamp < 0.0f) { adamp = 0.0f; }
            b.wx *= adamp; b.wy *= adamp; b.wz *= adamp;
        }
    }

    // ---- 動的ボディの pose (基底) と慣性を確定 (前 tick 末の姿勢で) ----
    for (Body& b : bodies) {
        if (!b.rb) {
            continue; // 静的は収集時に確定済み
        }
        if (b.col) {
            const XMFLOAT3 pos = { b.pose.px, b.pose.py, b.pose.pz };
            const XMFLOAT4 rot = { b.qx, b.qy, b.qz, b.qw };
            b.pose = shapes::MakePose(*b.col, pos, rot, b.scale);
        }
        if (!b.freezeRot && b.invMass > 0.0f) {
            float ix, iy, iz;
            LocalInertiaDiag(b.col, b.pose, 1.0f / b.invMass, ix, iy, iz);
            InvInertiaWorld(b.pose, ix, iy, iz, b.invI);
        } // freezeRot / kinematic は零行列のまま = 角応答なし
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
                Body& A = bodies[i];
                Body& B = bodies[j];
                const float tim = A.invMass + B.invMass;
                if (tim == 0.0f) {
                    continue; // 両方静的
                }
                shapes::Manifold m;
                if (!shapes::CollideManifold(A.pose, B.pose, m)) {
                    continue;
                }
                const float nx = m.nx, ny = m.ny, nz = m.nz; // b→a (A を押し出す)
                // 最終反復の接触ペアを記録 (M28c)。i<j × index 昇順走査 → key も自動的に昇順
                if (outContacts && iter == kSolverIterations - 1) {
                    outContacts->push_back(
                        { (static_cast<uint64_t>(A.entity.index) << 32) | B.entity.index, nx, ny,
                          nz });
                }
                // 位置補正 (並進のみ = 回転補正はしない: 簡易ソルバの発散防止)。最深点で分配
                float maxDepth = 0.0f;
                for (int k = 0; k < m.count; ++k) {
                    if (m.pts[k].depth > maxDepth) {
                        maxDepth = m.pts[k].depth;
                    }
                }
                const float corr = std::max(maxDepth - kPenetrationSlop, 0.0f);
                const float ci = corr * A.invMass / tim;
                const float cj = corr * B.invMass / tim;
                A.pose.px += nx * ci; A.pose.py += ny * ci; A.pose.pz += nz * ci;
                B.pose.px -= nx * cj; B.pose.py -= ny * cj; B.pose.pz -= nz * cj;
                // 接触点解決の 2 段構成 (非対称トルクと収束不足を両立して解決する):
                //   1. 重心 (接触点平均) での中央法線インパルス — 並進成分を 1 発で解決。
                //      面接触では重心が法線軸上に乗り有効質量 = 全質量 → 沈み込み残留なし
                //   2. 点毎 Jacobi 法線インパルス (同一速度から一括計算・点数分配) —
                //      回転の不均衡のみ担当。対称接触では自動的にゼロ = スタックが歩かない
                //   3. 重心でのクーロン摩擦 (合計法線インパルスでクランプ)
                const float mu = std::sqrt(A.friction * B.friction);
                const float invCount = 1.0f / static_cast<float>(m.count);
                float ra[4][3], rrb[4][3];
                float cpx = 0, cpy = 0, cpz = 0;
                for (int k = 0; k < m.count; ++k) {
                    ra[k][0] = m.pts[k].px - A.pose.px;
                    ra[k][1] = m.pts[k].py - A.pose.py;
                    ra[k][2] = m.pts[k].pz - A.pose.pz;
                    rrb[k][0] = m.pts[k].px - B.pose.px;
                    rrb[k][1] = m.pts[k].py - B.pose.py;
                    rrb[k][2] = m.pts[k].pz - B.pose.pz;
                    cpx += m.pts[k].px;
                    cpy += m.pts[k].py;
                    cpz += m.pts[k].pz;
                }
                cpx *= invCount;
                cpy *= invCount;
                cpz *= invCount;
                const float raC[3] = { cpx - A.pose.px, cpy - A.pose.py, cpz - A.pose.pz };
                const float rbC[3] = { cpx - B.pose.px, cpy - B.pose.py, cpz - B.pose.pz };
                // 接触点 r での相対速度 (回転寄与込み)
                auto relVelAt = [&](const float rA[3], const float rB[3], float& rvx, float& rvy,
                                    float& rvz) {
                    float wax, way, waz, wbx, wby, wbz;
                    Cross(A.wx, A.wy, A.wz, rA[0], rA[1], rA[2], wax, way, waz);
                    Cross(B.wx, B.wy, B.wz, rB[0], rB[1], rB[2], wbx, wby, wbz);
                    rvx = (A.vx + wax) - (B.vx + wbx);
                    rvy = (A.vy + way) - (B.vy + wby);
                    rvz = (A.vz + waz) - (B.vz + wbz);
                };
                // ---- 1. 中央法線インパルス ----
                float totalJn = 0.0f;
                {
                    float rvx, rvy, rvz;
                    relVelAt(raC, rbC, rvx, rvy, rvz);
                    const float vn = rvx * nx + rvy * ny + rvz * nz;
                    if (vn < 0.0f) {
                        // 反発: 低速接触は e=0 (micro-bounce 除去)
                        float e = std::min(A.restitution, B.restitution);
                        if (-vn < kRestitutionVelThreshold) {
                            e = 0.0f;
                        }
                        const float kn = EffectiveMassInv(A, raC[0], raC[1], raC[2], nx, ny, nz)
                                       + EffectiveMassInv(B, rbC[0], rbC[1], rbC[2], nx, ny, nz);
                        if (kn > 0.0f) {
                            const float jc = -(1.0f + e) * vn / kn;
                            ApplyImpulse(A, raC[0], raC[1], raC[2], nx * jc, ny * jc, nz * jc,
                                         1.0f);
                            ApplyImpulse(B, rbC[0], rbC[1], rbC[2], nx * jc, ny * jc, nz * jc,
                                         -1.0f);
                            totalJn = jc;
                        }
                    }
                }
                // ---- 2. 点毎 Jacobi 法線インパルス (回転不均衡の解消。反発なし) ----
                if (m.count > 1) {
                    float jnStored[4] = {};
                    for (int k = 0; k < m.count; ++k) {
                        float rvx, rvy, rvz;
                        relVelAt(ra[k], rrb[k], rvx, rvy, rvz);
                        const float vn = rvx * nx + rvy * ny + rvz * nz;
                        if (vn >= 0.0f) {
                            continue; // 離反中
                        }
                        const float kn = EffectiveMassInv(A, ra[k][0], ra[k][1], ra[k][2], nx, ny,
                                                          nz)
                                       + EffectiveMassInv(B, rrb[k][0], rrb[k][1], rrb[k][2], nx,
                                                          ny, nz);
                        if (kn <= 0.0f) {
                            continue;
                        }
                        jnStored[k] = -vn / kn * invCount;
                    }
                    for (int k = 0; k < m.count; ++k) {
                        if (jnStored[k] <= 0.0f) {
                            continue;
                        }
                        const float jn = jnStored[k];
                        ApplyImpulse(A, ra[k][0], ra[k][1], ra[k][2], nx * jn, ny * jn, nz * jn,
                                     1.0f);
                        ApplyImpulse(B, rrb[k][0], rrb[k][1], rrb[k][2], nx * jn, ny * jn,
                                     nz * jn, -1.0f);
                        totalJn += jn;
                    }
                }
                // ---- 3. 重心でのクーロン摩擦 (|jt| <= μ·合計法線インパルス) ----
                if (totalJn > 0.0f) {
                    float rvx, rvy, rvz;
                    relVelAt(raC, rbC, rvx, rvy, rvz);
                    const float vn2 = rvx * nx + rvy * ny + rvz * nz;
                    float tx = rvx - vn2 * nx;
                    float ty = rvy - vn2 * ny;
                    float tz = rvz - vn2 * nz;
                    const float t2 = tx * tx + ty * ty + tz * tz;
                    if (t2 > 1e-10f) { // 接線速度なしは摩擦スキップ (決定論的分岐)
                        const float tlen = std::sqrt(t2);
                        tx /= tlen; ty /= tlen; tz /= tlen;
                        const float kt = EffectiveMassInv(A, raC[0], raC[1], raC[2], tx, ty, tz)
                                       + EffectiveMassInv(B, rbC[0], rbC[1], rbC[2], tx, ty, tz);
                        if (kt > 0.0f) {
                            float jt = -tlen / kt; // vrel·t̂ = tlen
                            const float maxJt = mu * totalJn;
                            if (jt < -maxJt) { jt = -maxJt; }
                            if (jt > maxJt) { jt = maxJt; }
                            ApplyImpulse(A, raC[0], raC[1], raC[2], tx * jt, ty * jt, tz * jt,
                                         1.0f);
                            ApplyImpulse(B, rbC[0], rbC[1], rbC[2], tx * jt, ty * jt, tz * jt,
                                         -1.0f);
                        }
                    }
                }
            }
        }
    }

    // ---- 位置・姿勢積分 (解決後の速度で前進) ----
    for (Body& b : bodies) {
        if (b.invMass == 0.0f || !b.rb) {
            continue;
        }
        b.pose.px += b.vx * dt;
        b.pose.py += b.vy * dt;
        b.pose.pz += b.vz * dt;
        if (!b.freezeRot) {
            // q += 0.5·dt·(ω_quat ⊗ q)、その後正規化 (全て scalar)
            const float hx = b.wx * 0.5f * dt, hy = b.wy * 0.5f * dt, hz = b.wz * 0.5f * dt;
            const float dqw = -(hx * b.qx + hy * b.qy + hz * b.qz);
            const float dqx = hx * b.qw + hy * b.qz - hz * b.qy;
            const float dqy = hy * b.qw + hz * b.qx - hx * b.qz;
            const float dqz = hz * b.qw + hx * b.qy - hy * b.qx;
            b.qx += dqx; b.qy += dqy; b.qz += dqz; b.qw += dqw;
            const float len2 = b.qx * b.qx + b.qy * b.qy + b.qz * b.qz + b.qw * b.qw;
            if (len2 > 1e-12f) {
                const float inv = 1.0f / std::sqrt(len2);
                b.qx *= inv; b.qy *= inv; b.qz *= inv; b.qw *= inv;
            } else {
                b.qx = 0; b.qy = 0; b.qz = 0; b.qw = 1;
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
        b.lt->rotation = { b.qx, b.qy, b.qz, b.qw };
        b.rb->velocity.x = b.vx;
        b.rb->velocity.y = b.vy;
        b.rb->velocity.z = b.vz;
        b.rb->angularVelocity.x = b.wx;
        b.rb->angularVelocity.y = b.wy;
        b.rb->angularVelocity.z = b.wz;
    }
}

int ApplyTorqueWorld(World& world, EntityID e, MyeVec3 torque, float dt)
{
    auto* rb = world.GetComponent<RigidbodyComponent>(e);
    auto* lt = world.GetComponent<LocalTransform>(e);
    if (!rb || !lt || rb->isKinematic || rb->freezeRotation) {
        return 0;
    }
    const auto* col = world.GetComponent<ColliderComponent>(e);
    const float mass = (rb->mass > 0.0f) ? rb->mass : 1.0f;
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
