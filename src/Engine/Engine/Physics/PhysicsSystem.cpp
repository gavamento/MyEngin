#include "Engine/Engine/Physics/PhysicsSystem.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/Components.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/Physics/Broadphase.h"
#include "Engine/Engine/Physics/PhysMatLibrary.h" // M59a2: physmat::Resolve (材料解決)
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
// ブロードフェーズ AABB の膨張量 (M28d)。ソルバ内の位置補正移動を保守的にカバーする。
// 仮に候補から漏れても「次 tick で解決」に留まり、候補列は決定論なのでハッシュ一致性は不変
constexpr float kBroadphaseMargin = 0.1f;

// ---- scalar クォータニオン演算 (親子合成用。XMVECTOR 禁止 = 決定論契約) ----

// Hamilton 積 a ⊗ b
void QuatMul(float ax, float ay, float az, float aw, float bx, float by, float bz, float bw,
             float& ox, float& oy, float& oz, float& ow)
{
    ox = aw * bx + ax * bw + ay * bz - az * by;
    oy = aw * by - ax * bz + ay * bw + az * bx;
    oz = aw * bz + ax * by - ay * bx + az * bw;
    ow = aw * bw - ax * bx - ay * by - az * bz;
}

// 単位クォータニオンでベクトルを回転: v' = v + 2·q.w·(q×v) + 2·q×(q×v)
void QuatRotate(float qx, float qy, float qz, float qw, float vx, float vy, float vz, float& ox,
                float& oy, float& oz)
{
    const float tx = 2.0f * (qy * vz - qz * vy);
    const float ty = 2.0f * (qz * vx - qx * vz);
    const float tz = 2.0f * (qx * vy - qy * vx);
    ox = vx + qw * tx + (qy * tz - qz * ty);
    oy = vy + qw * ty + (qz * tx - qx * tz);
    oz = vz + qw * tz + (qx * ty - qy * tx);
}

// ワールドフレーム (親チェーンの合成結果)。シアーは無視の近似 (基底正規化と同じ流儀)
struct WorldFrame {
    float px = 0, py = 0, pz = 0;
    float qx = 0, qy = 0, qz = 0, qw = 1;
    float sx = 1, sy = 1, sz = 1;
    bool identity = true; // 親なし (ルート) — 既存コードパスをビット同一で通す fast-path
};

// e の親チェーンを LocalTransform から scalar 合成する (M28d)。
// WorldMatrix は使わない — 物理はフェーズ 3.6 = TransformSystem 前で、現 tick の
// スクリプト/アニメ結果を反映した LocalTransform チェーンが正となる。
// 「動的剛体の子」は親を運動学的フレームとして扱う (同 tick の親の積分結果は伝播しない)
WorldFrame ComposeParentFrame(World& world, EntityID e)
{
    WorldFrame f;
    EntityID cur = world.GetParent(e);
    while (!cur.IsNull()) {
        const auto* plt = world.GetComponent<LocalTransform>(cur);
        if (!plt) {
            break;
        }
        // f' = T_parent ∘ f
        const float px = plt->position.x, py = plt->position.y, pz = plt->position.z;
        const float qx = plt->rotation.x, qy = plt->rotation.y, qz = plt->rotation.z,
                    qw = plt->rotation.w;
        const float sx = plt->scale.x, sy = plt->scale.y, sz = plt->scale.z;
        float rx, ry, rz;
        QuatRotate(qx, qy, qz, qw, sx * f.px, sy * f.py, sz * f.pz, rx, ry, rz);
        f.px = px + rx;
        f.py = py + ry;
        f.pz = pz + rz;
        float nqx, nqy, nqz, nqw;
        QuatMul(qx, qy, qz, qw, f.qx, f.qy, f.qz, f.qw, nqx, nqy, nqz, nqw);
        f.qx = nqx; f.qy = nqy; f.qz = nqz; f.qw = nqw;
        f.sx *= sx; f.sy *= sy; f.sz *= sz;
        f.identity = false;
        cur = world.GetParent(cur);
    }
    return f;
}

// frame ∘ local を適用したワールド position/rotation/scale
void ApplyFrame(const WorldFrame& f, const LocalTransform& lt, XMFLOAT3& outPos, XMFLOAT4& outRot,
                XMFLOAT3& outScale)
{
    if (f.identity) {
        outPos = lt.position;
        outRot = lt.rotation;
        outScale = lt.scale;
        return;
    }
    float rx, ry, rz;
    QuatRotate(f.qx, f.qy, f.qz, f.qw, f.sx * lt.position.x, f.sy * lt.position.y,
               f.sz * lt.position.z, rx, ry, rz);
    outPos = { f.px + rx, f.py + ry, f.pz + rz };
    float qx, qy, qz, qw;
    QuatMul(f.qx, f.qy, f.qz, f.qw, lt.rotation.x, lt.rotation.y, lt.rotation.z, lt.rotation.w,
            qx, qy, qz, qw);
    outRot = { qx, qy, qz, qw };
    outScale = { f.sx * lt.scale.x, f.sy * lt.scale.y, f.sz * lt.scale.z };
}

struct Body {
    EntityID entity;
    LocalTransform* lt = nullptr;   // 位置/回転書き込み先
    RigidbodyComponent* rb = nullptr; // null = 静的コライダー (動かない衝突面)
    const ColliderComponent* col = nullptr; // ソリッド形状 (null = コライダー無し動的ボディ)
    bool solid = false;            // 衝突解決に参加するか (isTrigger==0)
    bool freezeRot = true;         // 回転積分・角応答をしない (静的 / kinematic / freezeRotation)
    ShapePose pose;                // 形状 + 作業用ワールド位置 (pose.px/py/pz をソルバが更新)
    XMFLOAT3 scale = { 1, 1, 1 };  // ワールドスケール (pose 再構築用)
    float qx = 0, qy = 0, qz = 0, qw = 1; // 作業用姿勢 (ワールド)
    float vx = 0, vy = 0, vz = 0;  // 作業用速度 (ワールド)
    float wx = 0, wy = 0, wz = 0;  // 作業用角速度 (rad/s、ワールド)
    float invMass = 0;             // 0 = 不動 (静的 / kinematic)
    float invI[3][3] = {};         // ワールド逆慣性テンソル (freezeRot は零行列)
    float restitution = 0;
    float friction = 0.5f;         // クーロン摩擦係数 (Collider から。ペアは sqrt(μa·μb))
    int32_t layer = 0;             // 衝突レイヤー (M36a、Collider から複製)
    uint32_t mask = 0xFFFFFFFFu;   // 衝突マスク (既定 = 全レイヤー → 従来挙動)
    // 親のワールドフレーム (M28d)。収集時に合成し運動学的フレームとして固定。
    // identity (ルート) なら書き戻しは従来の直接代入 (ビット同一 fast-path)
    WorldFrame frame;
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

// ---- キャラクターコントローラ (M29b) ----

struct CharBody {
    EntityID entity;
    CharacterControllerComponent* cc = nullptr;
    LocalTransform* lt = nullptr;
    WorldFrame frame;              // 親フレーム (収集時固定)
    float px = 0, py = 0, pz = 0;  // tick 頭のワールド位置
    float radius = 0, halfSeg = 0; // ワールドスケール適用済みのカプセル寸法
    int32_t layer = 0;             // 衝突レイヤー (M36a、併用 Collider から。無ければ既定)
    uint32_t mask = 0xFFFFFFFFu;
};

constexpr int kCharPushPasses = 4; // 固定パス数 (収束早期終了はしない = 決定論)

// move-then-depenetrate: 変位を一括適用してから固定パス × 障害物 index 昇順で押し出す。
// スイープしないため 1 tick で薄い壁を大きく越える速度 (v·dt > 壁厚) では貫通し得るが、
// 60Hz × 常識的な移動速度では起きない (v=30m/s でも 0.5m/tick)。剛体ソルバの後に走るので
// 障害物 (静的 + 解決後の動的剛体) の pose は当該 tick の最終位置。CC が剛体を押す応答は
// 無し (必要ならソリッド capsule Collider を併用し OnCollision + AddForce で組む)。
void SolveCharacters(std::vector<Body>& bodies, std::vector<CharBody>& chars, float dt)
{
    if (chars.empty()) {
        return;
    }
    std::sort(chars.begin(), chars.end(),
              [](const CharBody& a, const CharBody& b) { return a.entity.index < b.entity.index; });
    for (CharBody& c : chars) {
        constexpr float kDeg2Rad = 3.14159265f / 180.0f;
        const float cosSlope = std::cos(c.cc->slopeLimitDeg * kDeg2Rad);
        float vy = c.cc->velocity.y + kGravity * c.cc->gravityScale * dt;
        // 変位一括適用 (水平は moveInput 直接駆動、垂直は重力積分)
        ShapePose pose;
        pose.shape = 2; // capsule (常にワールド Y 軸 = 単位基底)
        pose.identityRot = 1;
        pose.radius = c.radius;
        pose.halfSeg = c.halfSeg;
        pose.px = c.px + c.cc->moveInput.x * dt;
        pose.py = c.py + vy * dt;
        pose.pz = c.pz + c.cc->moveInput.z * dt;
        bool grounded = false;
        // 押し出し: 固定パス × 障害物 index 昇順 (bodies はソート済)。自分の collider は skip
        for (int pass = 0; pass < kCharPushPasses; ++pass) {
            for (const Body& obs : bodies) {
                if (!obs.solid || obs.entity.index == c.entity.index) {
                    continue;
                }
                if (!shapes::CanCollide(c.layer, c.mask, obs.layer, obs.mask)) {
                    continue; // M36a
                }
                float nx, ny, nz, depth;
                if (!shapes::Collide(pose, obs.pose, nx, ny, nz, depth)) {
                    continue;
                }
                pose.px += nx * depth;
                pose.py += ny * depth;
                pose.pz += nz * depth;
                if (ny >= cosSlope) {
                    grounded = true; // 登れる斜面 = 接地
                    if (vy < 0.0f) {
                        vy = 0.0f;
                    }
                } else if (ny <= -cosSlope) {
                    if (vy > 0.0f) {
                        vy = 0.0f; // 天井
                    }
                } // 壁/急斜面は押し出しのみ (滑り落ちは重力が担う)
            }
        }
        // 接地プローブ (状態は変更しない): skinWidth+0.01 下げて接地面を探る
        if (!grounded) {
            ShapePose probe = pose;
            probe.py -= (c.cc->skinWidth + 0.01f);
            for (const Body& obs : bodies) {
                if (!obs.solid || obs.entity.index == c.entity.index) {
                    continue;
                }
                if (!shapes::CanCollide(c.layer, c.mask, obs.layer, obs.mask)) {
                    continue; // M36a
                }
                float nx, ny, nz, depth;
                if (shapes::Collide(probe, obs.pose, nx, ny, nz, depth) && ny >= cosSlope) {
                    grounded = true;
                    break; // 発見のみが目的 (状態を変えないので早期終了しても決定論)
                }
            }
        }
        // ジャンプ: 接地時のみ発火。接地可否に関わらず消費 (バッファリング無し = 予測可能)
        if (c.cc->jumpSpeed > 0.0f) {
            if (grounded) {
                vy = c.cc->jumpSpeed;
            }
            c.cc->jumpSpeed = 0.0f;
        }
        // 書き戻し (剛体と同じ親フレーム逆変換。回転は触らない)
        const float invDt = 1.0f / dt;
        c.cc->velocity.x = (pose.px - c.px) * invDt;
        c.cc->velocity.y = vy;
        c.cc->velocity.z = (pose.pz - c.pz) * invDt;
        c.cc->isGrounded = grounded ? 1 : 0;
        if (c.frame.identity) {
            c.lt->position = { pose.px, pose.py, pose.pz };
        } else {
            const float cqx = -c.frame.qx, cqy = -c.frame.qy, cqz = -c.frame.qz,
                        cqw = c.frame.qw;
            float lx, ly, lz;
            QuatRotate(cqx, cqy, cqz, cqw, pose.px - c.frame.px, pose.py - c.frame.py,
                       pose.pz - c.frame.pz, lx, ly, lz);
            const float isx = (std::fabs(c.frame.sx) > 1e-8f) ? 1.0f / c.frame.sx : 1.0f;
            const float isy = (std::fabs(c.frame.sy) > 1e-8f) ? 1.0f / c.frame.sy : 1.0f;
            const float isz = (std::fabs(c.frame.sz) > 1e-8f) ? 1.0f / c.frame.sz : 1.0f;
            c.lt->position = { lx * isx, ly * isy, lz * isz };
        }
    }
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
            // 親チェーンを合成しワールド姿勢で sim する (M28d。ルートは lt そのまま = 従来通り)
            b.frame = ComposeParentFrame(world, e);
            XMFLOAT3 wpos;
            XMFLOAT4 wrot;
            XMFLOAT3 wscale;
            ApplyFrame(b.frame, *lt, wpos, wrot, wscale);
            b.pose.px = wpos.x;
            b.pose.py = wpos.y;
            b.pose.pz = wpos.z;
            b.scale = wscale;
            b.qx = wrot.x;
            b.qy = wrot.y;
            b.qz = wrot.z;
            b.qw = wrot.w;
            b.vx = rb->velocity.x;
            b.vy = rb->velocity.y;
            b.vz = rb->velocity.z;
            b.wx = rb->angularVelocity.x;
            b.wy = rb->angularVelocity.y;
            b.wz = rb->angularVelocity.z;
            // コライダーがあり isTrigger==0 ならソリッド (衝突解決に参加)。
            // M41: メッシュ (shape=3) は静的/kinematic 専用 — 動的剛体では無視する
            // (慣性テンソルを定義しないため。kinematic は invMass=0 なので許可)
            // M59a2: 質量導出が形状体積を要るためコライダー取得を質量計算の前へ移動
            auto* col = world.GetComponent<ColliderComponent>(e);
            if (col && col->shape == 3 && !rb->isKinematic) {
                col = nullptr;
            }
            const PhysMat* mat = col ? physmat::Resolve(col->physMaterial) : nullptr; // M59a2
            const float mass = ResolveBodyMass(*rb, col, mat, wscale.x, wscale.y, wscale.z);
            b.invMass = rb->isKinematic ? 0.0f : (1.0f / mass);
            b.restitution = SelectRestitution(col, rb, mat);
            b.freezeRot = (rb->freezeRotation != 0) || rb->isKinematic;
            if (col && !col->isTrigger) {
                b.solid = true;
                b.col = col;
                b.friction = SelectFriction(*col, mat);
                b.layer = col->layer; // M36a
                b.mask = col->mask;
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
            if (col->isTrigger) {
                continue; // トリガーはソリッド衝突面でない (CollisionSystem がイベントを出す)
            }
            auto* lt = static_cast<const LocalTransform*>(arch.GetPtr(li, row));
            Body b;
            b.entity = e;
            b.solid = true;
            b.invMass = 0.0f; // 静的 = 不動
            const PhysMat* mat = physmat::Resolve(col->physMaterial); // M59a2
            b.friction = SelectFriction(*col, mat);
            // M59a2: 材料付き静的コライダーは e を主張できる (従来は構造的に 0 = 新規能力。
            // 未割当は mat=nullptr → SelectRestitution が従来どおり 0 を返す)
            b.restitution = SelectRestitution(col, nullptr, mat);
            b.layer = col->layer; // M36a
            b.mask = col->mask;
            // M28d: 親付き静的コライダーもワールド姿勢で判定 (従来は lt 直読みのバグ)
            const WorldFrame f = ComposeParentFrame(world, e);
            XMFLOAT3 wpos;
            XMFLOAT4 wrot;
            XMFLOAT3 wscale;
            ApplyFrame(f, *lt, wpos, wrot, wscale);
            b.pose = shapes::MakePose(*col, wpos, wrot, wscale);
            bodies.push_back(b);
        }
    });

    // ---- 収集 (キャラクターコントローラ: CC + LocalTransform、Rigidbody 非所持) (M29b) ----
    std::vector<CharBody> chars;
    const ComponentTypeId ccReq[] = { CharacterControllerComponent::sTypeId,
                                      LocalTransform::sTypeId };
    world.ForEachArchetype(ccReq, [&](Archetype& arch) {
        const int ci = arch.FindTypeIndex(CharacterControllerComponent::sTypeId);
        const int li = arch.FindTypeIndex(LocalTransform::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            if (!IsEntityActive(world, e)) {
                continue;
            }
            if (world.HasComponent(e, RigidbodyComponent::sTypeId)) {
                continue; // Rigidbody が優先 (CC は無効)
            }
            CharBody c;
            c.entity = e;
            c.cc = static_cast<CharacterControllerComponent*>(arch.GetPtr(ci, row));
            c.lt = static_cast<LocalTransform*>(arch.GetPtr(li, row));
            c.frame = ComposeParentFrame(world, e);
            XMFLOAT3 wpos;
            XMFLOAT4 wrot;
            XMFLOAT3 wscale;
            ApplyFrame(c.frame, *c.lt, wpos, wrot, wscale);
            c.px = wpos.x;
            c.py = wpos.y;
            c.pz = wpos.z;
            // カプセル寸法は MakePose の capsule 規約をミラー (回転は形状に影響しない)
            const float asx = std::fabs(wscale.x);
            const float asy = std::fabs(wscale.y);
            const float asz = std::fabs(wscale.z);
            const float wr = c.cc->radius * std::max(asx, asz);
            const float wh = c.cc->height * 0.5f * asy;
            c.radius = wr;
            c.halfSeg = (wh > wr) ? (wh - wr) : 0.0f;
            // M36a: 併用 Collider があればそのレイヤー/マスクを CC の判定にも使う
            if (const auto* ccol = world.GetComponent<ColliderComponent>(e)) {
                c.layer = ccol->layer;
                c.mask = ccol->mask;
            }
            chars.push_back(c);
        }
    });

    // chars が空なら従来の分岐と同一 = 既存シーンはビット同一パス
    if (bodies.empty() && chars.empty()) {
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

    // ---- ConstantForce (M29a): 毎 tick の定常力/トルク。opt-in — 非所持ボディは
    //      ルックアップのみで fp 演算ゼロ = 既存シーンとビット同一 ----
    for (Body& b : bodies) {
        if (!b.rb || b.invMass == 0.0f) {
            continue;
        }
        const auto* cf = world.GetComponent<ConstantForceComponent>(b.entity);
        if (!cf) {
            continue;
        }
        float fx = cf->force.x, fy = cf->force.y, fz = cf->force.z;
        float tx = cf->torque.x, ty = cf->torque.y, tz = cf->torque.z;
        if (cf->relative != 0) {
            float rx, ry, rz;
            QuatRotate(b.qx, b.qy, b.qz, b.qw, fx, fy, fz, rx, ry, rz);
            fx = rx; fy = ry; fz = rz;
            QuatRotate(b.qx, b.qy, b.qz, b.qw, tx, ty, tz, rx, ry, rz);
            tx = rx; ty = ry; tz = rz;
        }
        b.vx += fx * b.invMass * dt;
        b.vy += fy * b.invMass * dt;
        b.vz += fz * b.invMass * dt;
        // 角: freezeRot / kinematic は invI が零行列なので自然に無効
        float ax, ay, az;
        MulInvI(b.invI, tx * dt, ty * dt, tz * dt, ax, ay, az);
        b.wx += ax;
        b.wy += ay;
        b.wz += az;
    }

    // ---- SpringJoint (M29a): 距離バネ (速度レベル・ステートレス)。owner index 昇順。
    //      broadphase の前に置く = ばねで変わった速度が候補 AABB の margin に反映される ----
    {
        struct Joint {
            EntityID owner;
            const SpringJointComponent* sj = nullptr;
        };
        std::vector<Joint> joints;
        const ComponentTypeId sjReq[] = { SpringJointComponent::sTypeId, LocalTransform::sTypeId };
        world.ForEachArchetype(sjReq, [&](Archetype& arch) {
            const int si = arch.FindTypeIndex(SpringJointComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                const EntityID e = arch.EntityAt(row);
                if (!IsEntityActive(world, e)) {
                    continue;
                }
                joints.push_back(
                    { e, static_cast<const SpringJointComponent*>(arch.GetPtr(si, row)) });
            }
        });
        if (!joints.empty()) {
            std::sort(joints.begin(), joints.end(),
                      [](const Joint& a, const Joint& b) { return a.owner.index < b.owner.index; });
            // bodies は index 昇順ソート済 → 二分探索 (generation も一致確認)
            auto findBody = [&bodies](EntityID e) -> Body* {
                auto it = std::lower_bound(
                    bodies.begin(), bodies.end(), e.index,
                    [](const Body& b, uint32_t idx) { return b.entity.index < idx; });
                if (it != bodies.end() && it->entity.index == e.index
                    && it->entity.generation == e.generation) {
                    return &(*it);
                }
                return nullptr;
            };
            // bodies に居ないエンティティ (コライダー/Rigidbody 無し) は変換から不動アンカー位置
            auto anchorPos = [&world](EntityID e, float& px, float& py, float& pz) -> bool {
                const auto* alt = world.GetComponent<LocalTransform>(e);
                if (!alt) {
                    return false;
                }
                const WorldFrame f = ComposeParentFrame(world, e);
                XMFLOAT3 wpos;
                XMFLOAT4 wrot;
                XMFLOAT3 wscale;
                ApplyFrame(f, *alt, wpos, wrot, wscale);
                px = wpos.x;
                py = wpos.y;
                pz = wpos.z;
                return true;
            };
            for (const Joint& j : joints) {
                const EntityID other = j.sj->connectedEntity;
                if (other.IsNull() || !world.IsAlive(other) || !IsEntityActive(world, other)) {
                    continue;
                }
                Body* ba = findBody(j.owner);
                Body* bb = findBody(other);
                float pax, pay, paz, pbx, pby, pbz;
                if (ba) {
                    pax = ba->pose.px; pay = ba->pose.py; paz = ba->pose.pz;
                } else if (!anchorPos(j.owner, pax, pay, paz)) {
                    continue;
                }
                if (bb) {
                    pbx = bb->pose.px; pby = bb->pose.py; pbz = bb->pose.pz;
                } else if (!anchorPos(other, pbx, pby, pbz)) {
                    continue;
                }
                const float invA = ba ? ba->invMass : 0.0f;
                const float invB = bb ? bb->invMass : 0.0f;
                const float invSum = invA + invB;
                if (invSum == 0.0f) {
                    continue; // 両側不動 = ばねは何も動かせない
                }
                const float dxv = pax - pbx, dyv = pay - pby, dzv = paz - pbz;
                const float dist2 = dxv * dxv + dyv * dyv + dzv * dzv;
                if (dist2 < 1e-16f) {
                    continue; // 同一点はばね方向が定義できない (決定論的分岐)
                }
                const float dist = std::sqrt(dist2);
                const float ux = dxv / dist, uy = dyv / dist, uz = dzv / dist;
                const float k = (j.sj->stiffness > 0.0f) ? j.sj->stiffness : 0.0f;
                const float c = (j.sj->damping > 0.0f) ? j.sj->damping : 0.0f;
                const float stretch = dist - j.sj->restLength;
                const float vax = ba ? ba->vx : 0.0f, vay = ba ? ba->vy : 0.0f,
                            vaz = ba ? ba->vz : 0.0f;
                const float vbx = bb ? bb->vx : 0.0f, vby = bb ? bb->vy : 0.0f,
                            vbz = bb ? bb->vz : 0.0f;
                const float vrel = (vax - vbx) * ux + (vay - vby) * uy + (vaz - vbz) * uz;
                // λ = −(k·stretch + c·vrel)·dt / (1 + c·dt·Σinvm)。分母が implicit damping
                float lambda = -(k * stretch + c * vrel) * dt / (1.0f + c * dt * invSum);
                // 極端な剛性でも 1 tick の Δv を 100 m/s に制限 (発散防止の決定論的クランプ)
                const float maxInv = (invA > invB) ? invA : invB; // invSum>0 → maxInv>0
                const float maxJ = 100.0f / maxInv;
                if (lambda > maxJ) { lambda = maxJ; }
                if (lambda < -maxJ) { lambda = -maxJ; }
                if (ba && invA > 0.0f) {
                    ba->vx += ux * lambda * invA;
                    ba->vy += uy * lambda * invA;
                    ba->vz += uz * lambda * invA;
                }
                if (bb && invB > 0.0f) {
                    bb->vx -= ux * lambda * invB;
                    bb->vy -= uy * lambda * invB;
                    bb->vz -= uz * lambda * invB;
                }
            }
        }
    }

    // ---- ブロードフェーズ (M28d): 候補ペア列挙 (1 軸 sort & sweep、margin 込み) ----
    // 候補列は真の接触ペアの純粋なスーパーセット + 走査順は (小,大) 昇順 = 総当たりと
    // ビット同一の解決結果 (等価性は selftest が総当たりとのハッシュ比較で常時検証)
    const size_t n = bodies.size();
    std::vector<uint64_t> candidates;
    {
        std::vector<BroadphaseEntry> entries;
        entries.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            if (!bodies[i].solid) {
                continue;
            }
            BroadphaseEntry e;
            e.id = static_cast<uint32_t>(i);
            shapes::ComputeAabb(bodies[i].pose, e.minX, e.minY, e.minZ, e.maxX, e.maxY, e.maxZ);
            const float mx = std::fabs(bodies[i].vx) * dt + kBroadphaseMargin;
            const float my = std::fabs(bodies[i].vy) * dt + kBroadphaseMargin;
            const float mz = std::fabs(bodies[i].vz) * dt + kBroadphaseMargin;
            e.minX -= mx; e.maxX += mx;
            e.minY -= my; e.maxY += my;
            e.minZ -= mz; e.maxZ += mz;
            entries.push_back(e);
        }
        if (PhysicsSystem::sDisableBroadphaseForTest) {
            // 等価性テスト用: 全ソリッドペア (昇順) を候補にする
            for (size_t i = 0; i < entries.size(); ++i) {
                for (size_t j = i + 1; j < entries.size(); ++j) {
                    candidates.push_back((static_cast<uint64_t>(entries[i].id) << 32)
                                         | entries[j].id);
                }
            }
        } else {
            ComputeCandidatePairs(entries, candidates);
        }
    }

    // ---- レイヤーフィルタ (M36a): 非マッチペアを候補から除外 (順序保存 = 決定論)。
    //      既定 (layer=0, mask=all) は全ペア通過 = 従来挙動 ----
    {
        size_t w = 0;
        for (const uint64_t key : candidates) {
            const Body& A = bodies[static_cast<size_t>(key >> 32)];
            const Body& B = bodies[static_cast<size_t>(key & 0xFFFFFFFFu)];
            if (shapes::CanCollide(A.layer, A.mask, B.layer, B.mask)) {
                candidates[w++] = key;
            }
        }
        candidates.resize(w);
    }

    // ---- 接触解決 (固定反復・候補ペアを (小,大) 昇順走査 = 決定論) ----
    for (int iter = 0; iter < kSolverIterations; ++iter) {
        for (const uint64_t pairKey : candidates) {
            {
                Body& A = bodies[static_cast<size_t>(pairKey >> 32)];
                Body& B = bodies[static_cast<size_t>(pairKey & 0xFFFFFFFFu)];
                const float tim = A.invMass + B.invMass;
                if (tim == 0.0f) {
                    continue; // 両方不動 (静的 / kinematic 同士)
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

    // ---- 書き戻し (動的・非 kinematic のみ。kinematic は物理が何も変えないので
    //      スキップ = 親付きの world→local 往復変換ドリフトも出ない) ----
    for (Body& b : bodies) {
        if (!b.rb || b.invMass == 0.0f) {
            continue;
        }
        if (b.frame.identity) {
            // ルート: 従来通りの直接代入 (ビット同一 fast-path)
            b.lt->position.x = b.pose.px;
            b.lt->position.y = b.pose.py;
            b.lt->position.z = b.pose.pz;
            b.lt->rotation = { b.qx, b.qy, b.qz, b.qw };
        } else {
            // 親付き: 収集時に固定した親フレームの逆変換でローカルへ (M28d)
            const float cqx = -b.frame.qx, cqy = -b.frame.qy, cqz = -b.frame.qz,
                        cqw = b.frame.qw;
            float lx, ly, lz;
            QuatRotate(cqx, cqy, cqz, cqw, b.pose.px - b.frame.px, b.pose.py - b.frame.py,
                       b.pose.pz - b.frame.pz, lx, ly, lz);
            const float isx = (std::fabs(b.frame.sx) > 1e-8f) ? 1.0f / b.frame.sx : 1.0f;
            const float isy = (std::fabs(b.frame.sy) > 1e-8f) ? 1.0f / b.frame.sy : 1.0f;
            const float isz = (std::fabs(b.frame.sz) > 1e-8f) ? 1.0f / b.frame.sz : 1.0f;
            b.lt->position = { lx * isx, ly * isy, lz * isz };
            float lqx, lqy, lqz, lqw;
            QuatMul(cqx, cqy, cqz, cqw, b.qx, b.qy, b.qz, b.qw, lqx, lqy, lqz, lqw);
            const float len2 = lqx * lqx + lqy * lqy + lqz * lqz + lqw * lqw;
            if (len2 > 1e-12f) {
                const float inv = 1.0f / std::sqrt(len2);
                lqx *= inv; lqy *= inv; lqz *= inv; lqw *= inv;
            }
            b.lt->rotation = { lqx, lqy, lqz, lqw };
        }
        b.rb->velocity.x = b.vx;
        b.rb->velocity.y = b.vy;
        b.rb->velocity.z = b.vz;
        b.rb->angularVelocity.x = b.wx;
        b.rb->angularVelocity.y = b.wy;
        b.rb->angularVelocity.z = b.wz;
    }

    // ---- キャラクターコントローラ解決 (M29b)。剛体解決後の最終 pose を障害物として使う ----
    SolveCharacters(bodies, chars, dt);
}

// ==== 物理マテリアル解決 (M59a2) ====
// 宣言側 (PhysicsSystem.h) のコメントが契約の正本。ここは実装の注意のみ:
// Select* は fp 演算ゼロの値選択に保つこと — 未割当シーンのビット同一はそれだけで自明に立つ

float SelectFriction(const ColliderComponent& col, const PhysMat* mat)
{
    if ((col.materialOverrideBits & kPhysMatOverrideFriction) != 0u) {
        return col.friction;
    }
    return mat ? mat->dynamicFriction : col.friction;
}

float SelectRestitution(const ColliderComponent* col, const RigidbodyComponent* rb,
                        const PhysMat* mat)
{
    // 既存フィールドの格納庫は Rigidbody 側 (静的コライダーは持たない = 従来どおり 0)
    const float legacy = rb ? rb->restitution : 0.0f;
    if (col && (col->materialOverrideBits & kPhysMatOverrideRestitution) != 0u) {
        return legacy;
    }
    return mat ? mat->restitution : legacy;
}

float ShapeVolumeWorld(const ColliderComponent& col, float sx, float sy, float sz)
{
    // MakePose 経由と同じく負スケールは絶対値 (shapes::ApplyScaledExtents 参照)
    sx = std::fabs(sx);
    sy = std::fabs(sy);
    sz = std::fabs(sz);
    switch (col.shape) {
    case 0: { // 球 = 最大成分スケール
        const float r = col.radius * std::max(sx, std::max(sy, sz));
        return (4.0f / 3.0f) * XM_PI * r * r * r;
    }
    case 1: // box = 成分別スケール
        return 8.0f * (col.halfExtents.x * sx) * (col.halfExtents.y * sy)
             * (col.halfExtents.z * sz);
    case 2: { // capsule = 円柱 + 両端半球 (halfSeg 規約は ApplyScaledExtents と同一)
        const float wr = col.radius * std::max(sx, sz);
        const float wh = col.height * 0.5f * sy;
        const float halfSeg = (wh > wr) ? (wh - wr) : 0.0f;
        return XM_PI * wr * wr * (2.0f * halfSeg) + (4.0f / 3.0f) * XM_PI * wr * wr * wr;
    }
    default: // mesh (shape=3) は体積を定義しない → 呼び出し側が mass へフォールバック
        return 0.0f;
    }
}

float ResolveBodyMass(const RigidbodyComponent& rb, const ColliderComponent* col,
                      const PhysMat* mat, float sx, float sy, float sz)
{
    const float base = (rb.mass > 0.0f) ? rb.mass : 1.0f; // 従来の既定 (M20)
    if (!rb.useDensity || !mat || !col || col->shape == 3) {
        return base; // 未割当シーンは常にここ = 従来と同一式
    }
    const float m = mat->density * ShapeVolumeWorld(*col, sx, sy, sz);
    return (m > 0.0f) ? m : base; // 体積 0 (半径 0 等) をゼロ除算にしない
}

float EffectiveMassWorld(World& world, EntityID e, const RigidbodyComponent& rb)
{
    const auto* col = world.GetComponent<ColliderComponent>(e);
    const PhysMat* mat = col ? physmat::Resolve(col->physMaterial) : nullptr;
    float sx = 1.0f, sy = 1.0f, sz = 1.0f;
    if (const auto* lt = world.GetComponent<LocalTransform>(e)) {
        sx = lt->scale.x;
        sy = lt->scale.y;
        sz = lt->scale.z;
    }
    return ResolveBodyMass(rb, col, mat, sx, sy, sz);
}

int ApplyTorqueWorld(World& world, EntityID e, MyeVec3 torque, float dt)
{
    auto* rb = world.GetComponent<RigidbodyComponent>(e);
    auto* lt = world.GetComponent<LocalTransform>(e);
    if (!rb || !lt || rb->isKinematic || rb->freezeRotation) {
        return 0;
    }
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

int RaycastWorld(World& world, MyeVec3 origin, MyeVec3 dir, float maxDist, MyeRaycastHit* outHit,
                 uint32_t mask)
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
            if (!shapes::LayerHit(mask, col->layer)) {
                continue; // M36a: マスク外レイヤーは収集段階で除外
            }
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
