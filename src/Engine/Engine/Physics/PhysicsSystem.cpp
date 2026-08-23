#include "Engine/Engine/Physics/PhysicsSystem.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/Components.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/Physics/AeroSampling.h" // M59c: 面サンプリング
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
// 速度ソルバの固定反復回数 (収束判定による早期終了はしない = 決定論)。M28b から 8 のまま。
// ★tick あたりのコストは M59g1 で**下がっている** — M28b は反復のたびに CollideManifold を
//   呼んでいた (8 回) のに対し、いまは生成 1 回 + 位置補正 8 回。速度反復そのものは
//   有効質量も再計算しない安い計算になった
constexpr int kSolverIterations = 8;
// 位置補正のパス数 (M59g1)。**ここだけは真の貫通量が要るので毎回 CollideManifold を呼ぶ**
// (速度ソルバと違い、接触点を反復間で持ち越す必要が無い)。M28b が速度ソルバと同じループで
// 8 回押し出していたのに合わせてある。**4 に減らすと 10 段スタックが床を突き抜けた** (実測)
constexpr int kPositionIterations = 8;
// サブステップ数の上限 (M59g2)。PhysicsEnvironment のフィールドをここでクランプする
constexpr int kMaxSubsteps = 16;
// ジャイロ項の Newton 反復数 (M59f1)。収束判定による早期終了はしない = 決定論。
// ★**2 以上でないと陰的中点にならない** — 1 反復目は ω̄ = ω₀ なので後退 Euler と同じ式に
//   なり、保存性が出てこない (実測: 1 反復だと無トルクの箱の |L| が 4 秒で 7.3% 落ちる)
constexpr int kGyroIterations = 3;
constexpr float kPenetrationSlop = 0.0005f; // 微小めり込みは許容 (ジッタ抑制)
// |vn| がこの閾値未満の接触は反発 0 扱い (micro-bounce 除去 = 静止安定の柱。~2g·dt)
constexpr float kRestitutionVelThreshold = 0.3f;
// ブロードフェーズ AABB の膨張量 (M28d)。ソルバ内の位置補正移動を保守的にカバーする。
// 仮に候補から漏れても「次 tick で解決」に留まり、候補列は決定論なのでハッシュ一致性は不変
constexpr float kBroadphaseMargin = 0.1f;
// **陽的な**項 (マグヌス M59b / 浮力 M59b2) の 1 **ステップ**あたり Delta-v 上限
// (M59g2 以降サブステップごとに効くので、実効上限は 100*substeps m/s)。SpringJoint の
// 100 m/s 前例と同じ「発散防止の決定論的クランプ」— 物理的に届くことはまず無く、
// 係数を極端にしたオーサリングミスでシーンが吹き飛ぶのを止めるための防波堤。
// 抗力系は閉形式 implicit なのでクランプを要らない (除算しかしないため発散し得ない)
constexpr float kExplicitMaxDeltaV = 100.0f;
// 面サンプリング (M59c) のトルク側の同じ防波堤 [rad/s]
constexpr float kExplicitMaxDeltaOmega = 100.0f;

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
    // M59b: 質量/面積の導出に使う形状。**トリガーも含む** (形状とソリッド性は独立 —
    // M59a2 の質量導出と同じ規約) が、動的剛体の mesh (shape=3) は除外済み
    const ColliderComponent* shapeCol = nullptr;
    bool solid = false;            // 衝突解決に参加するか (isTrigger==0)
    bool freezeRot = true;         // 回転積分・角応答をしない (静的 / kinematic / freezeRotation)
    ShapePose pose;                // 形状 + 作業用ワールド位置 (pose.px/py/pz をソルバが更新)
    XMFLOAT3 scale = { 1, 1, 1 };  // ワールドスケール (pose 再構築用)
    float qx = 0, qy = 0, qz = 0, qw = 1; // 作業用姿勢 (ワールド)
    float vx = 0, vy = 0, vz = 0;  // 作業用速度 (ワールド)
    float wx = 0, wy = 0, wz = 0;  // 作業用角速度 (rad/s、ワールド)
    float invMass = 0;             // 0 = 不動 (静的 / kinematic)
    float invI[3][3] = {};         // ワールド逆慣性テンソル (freezeRot は零行列)
    // M59f1: 主軸ローカルの対角慣性。ジャイロ項は逆テンソルでは書けない (ω×Iω に I 自身が
    // 要る) ので invI とは別に持つ。freezeRot / kinematic は 0 のまま
    float Ilx = 0, Ily = 0, Ilz = 0;
    // M59f1: 質量中心。comL* = ローカル (スケール適用済み)、com* = 形状原点からのワールド
    // オフセット。**hasCom が false のあいだ com* は必ず +0.0f** — 腕の計算が
    // `p - pose.p - com` で従来とビット同一になるのはこの前提に依る (x - (+0.0f) == x)
    bool hasCom = false;
    bool gyro = false;
    float comLx = 0, comLy = 0, comLz = 0;
    float comx = 0, comy = 0, comz = 0;
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

// 3x3 の線形方程式 A x = b を Cramer で解く (M59f1)。除算しか増やさないので決定論的。
// 行列式がほぼ 0 なら false を返して呼び側が「何もしない」を選べるようにする
bool Solve3x3(const float a[3][3], const float b[3], float out[3])
{
    const float c00 = a[1][1] * a[2][2] - a[1][2] * a[2][1];
    const float c01 = a[1][2] * a[2][0] - a[1][0] * a[2][2];
    const float c02 = a[1][0] * a[2][1] - a[1][1] * a[2][0];
    const float det = a[0][0] * c00 + a[0][1] * c01 + a[0][2] * c02;
    if (det > -1e-12f && det < 1e-12f) {
        return false;
    }
    const float inv = 1.0f / det;
    const float c10 = a[0][2] * a[2][1] - a[0][1] * a[2][2];
    const float c11 = a[0][0] * a[2][2] - a[0][2] * a[2][0];
    const float c12 = a[0][1] * a[2][0] - a[0][0] * a[2][1];
    const float c20 = a[0][1] * a[1][2] - a[0][2] * a[1][1];
    const float c21 = a[0][2] * a[1][0] - a[0][0] * a[1][2];
    const float c22 = a[0][0] * a[1][1] - a[0][1] * a[1][0];
    out[0] = (c00 * b[0] + c10 * b[1] + c20 * b[2]) * inv;
    out[1] = (c01 * b[0] + c11 * b[1] + c21 * b[2]) * inv;
    out[2] = (c02 * b[0] + c12 * b[1] + c22 * b[2]) * inv;
    return true;
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

// サブステップ 1 回ぶんの接触列 (key 昇順) を tick 全体の列へ合流させる (M59g2)。
// 同じペアが複数のサブステップで出たら**インパルスを足し、幾何は後のもので上書き**する。
// どちらも key 昇順なので線形マージで済む (順序が結果のビットを決めるので手順は固定)
void MergeSubstepContacts(const std::vector<SolidContact>& sub, std::vector<SolidContact>& acc)
{
    if (sub.empty()) {
        return;
    }
    if (acc.empty()) {
        acc = sub;
        return;
    }
    std::vector<SolidContact> merged;
    merged.reserve(acc.size() + sub.size());
    size_t i = 0, j = 0;
    while (i < acc.size() && j < sub.size()) {
        if (acc[i].key < sub[j].key) {
            merged.push_back(acc[i++]);
        } else if (sub[j].key < acc[i].key) {
            merged.push_back(sub[j++]);
        } else {
            SolidContact c = sub[j];          // 幾何は後のサブステップを採る
            c.impulse += acc[i].impulse;      // インパルスは足す
            merged.push_back(c);
            ++i;
            ++j;
        }
    }
    while (i < acc.size()) {
        merged.push_back(acc[i++]);
    }
    while (j < sub.size()) {
        merged.push_back(sub[j++]);
    }
    acc.swap(merged);
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
            b.shapeCol = col; // M59b: 空力の基準面積も同じ「形状」の読みを共有する
            const float mass = ResolveBodyMass(*rb, col, mat, wscale.x, wscale.y, wscale.z);
            b.invMass = rb->isKinematic ? 0.0f : (1.0f / mass);
            b.restitution = SelectRestitution(col, rb, mat);
            b.freezeRot = rb->freezeRotation || rb->isKinematic;
            // M59f1: ジャイロ項と質量中心オフセット。**どちらも既定は無効**で、
            // 無効のあいだは以降の分岐が全て従来側へ落ちる (ビット同一)
            b.gyro = rb->gyroscopic;
            if (rb->centerOfMass.x != 0.0f || rb->centerOfMass.y != 0.0f
                || rb->centerOfMass.z != 0.0f) {
                // ワールドスケールを掛けてローカルオフセットを作る (形状の寸法と同じ扱い)
                b.comLx = rb->centerOfMass.x * wscale.x;
                b.comLy = rb->centerOfMass.y * wscale.y;
                b.comLz = rb->centerOfMass.z * wscale.z;
                b.hasCom = true;
            }
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

    // ---- 物理環境の解決 (M59b)。不在 (= 既存シーンの全部) なら以降は全て従来経路 ----
    const PhysicsEnvironmentComponent* env = ResolvePhysicsEnvironment(world);

    // ---- サブステップ (M59g2) ----
    // 1 tick を substeps 回に割って「積分 → 制約生成 → 解決 → 位置補正 → 前進」を繰り返す。
    // 反復回数を増やすより効く — 接触が生まれてから解かれるまでの時間が短くなるので、
    // 反発の頂点保存も貫通も改善する。**env が無ければ 1 = M59g1 までと同一経路**
    // (存在ゲート。文の並びも変えていないのでサブステップ 1 は構造的にそのまま)。
    int substeps = 1;
    if (env) {
        substeps = env->substeps;
        if (substeps < 1) {
            substeps = 1;
        } else if (substeps > kMaxSubsteps) {
            substeps = kMaxSubsteps;
        }
    }
    const float h = dt / static_cast<float>(substeps);
    // 反発の速度閾値は「重力が 1 ステップで与える速度の ~2 倍」という設計 (kGravity の
    // すぐ上のコメント参照)。刻みが細かくなれば閾値も比例して下げないと、
    // サブステップを増やすほど跳ねなくなるという逆転が起きる
    const float restitutionVelThreshold = kRestitutionVelThreshold * (h / dt);
    std::vector<SolidContact> subContacts; // サブステップ 1 回ぶんの接触 (合算前)
    for (int sub = 0; sub < substeps; ++sub) {
        // ---- 速度積分 (動的・非 kinematic のみ)。位置はまだ動かさない ----
        // M28b で「速度積分 → ソルバ → 位置積分」の順に変更 (Box2D 流)。摩擦や法線インパルスで
        // 静止した速度がそのまま位置積分に使われるため、静止接触の毎 tick クリープが出ない
        for (Body& b : bodies) {
            if (b.invMass == 0.0f || !b.rb) {
                continue;
            }
            // M59b: env は**存在ゲート**。不在なら従来式を 1 文字も変えずに通す。
            // gravity=(0,-9.81,0) でも無条件のベクトル加算にすると vx += -0.0f * s * h が走り、
            // vx が -0.0f のときに +0.0f へ化けてワールドハッシュが動く
            // (= 「係数 0 なら中立」という値ゲートが float では成立しない理由。決定台帳 1)
            if (env) {
                const float gs = b.rb->gravityScale;
                b.vx += env->gravity.x * gs * h;
                b.vy += env->gravity.y * gs * h;
                b.vz += env->gravity.z * gs * h;
            } else {
                b.vy += kGravity * b.rb->gravityScale * h;
            }
            // ★減衰は**毎 tick の率**なのでサブステップごとに掛けてはいけない (N 乗になる)。
            //   最初のサブステップで 1 回だけ適用する — substeps=1 なら文の並びも M59g1 と同一
            if (sub == 0) {
                float damp = 1.0f - b.rb->linearDamping;
                if (damp < 0.0f) { damp = 0.0f; }
                b.vx *= damp; b.vy *= damp; b.vz *= damp;
                if (!b.freezeRot) {
                    float adamp = 1.0f - b.rb->angularDamping;
                    if (adamp < 0.0f) { adamp = 0.0f; }
                    b.wx *= adamp; b.wy *= adamp; b.wz *= adamp;
                }
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
                b.Ilx = ix; // M59f1: ジャイロ項は I 自身が要る
                b.Ily = iy;
                b.Ilz = iz;
            } // freezeRot / kinematic は零行列のまま = 角応答なし
            if (b.hasCom) {
                // 形状原点 → 質量中心のワールドオフセット。姿勢が変わるたび取り直す
                QuatRotate(b.qx, b.qy, b.qz, b.qw, b.comLx, b.comLy, b.comLz, b.comx, b.comy,
                           b.comz);
            }
        }

        // ---- ConstantForce (M29a): 定常な力/トルク (M59g2 以降は h 刻みで積む)。opt-in — 非所持ボディは
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
            b.vx += fx * b.invMass * h;
            b.vy += fy * b.invMass * h;
            b.vz += fz * b.invMass * h;
            // 角: freezeRot / kinematic は invI が零行列なので自然に無効
            float ax, ay, az;
            MulInvI(b.invI, tx * h, ty * h, tz * h, ax, ay, az);
            b.wx += ax;
            b.wy += ay;
            b.wz += az;
        }

        // ---- 空力 (M59b): 等方抗力 / マグヌス / 角抗力。ConstantForce の直後 = 「その tick の
        //      力を足し終えた速度」に効かせる。**AeroComponent 非所持ボディはルックアップのみで
        //      fp 演算ゼロ** = 既存シーンとビット同一 (ConstantForce 帯と同じ opt-in の作り) ----
        for (Body& b : bodies) {
            if (!b.rb || b.invMass == 0.0f) {
                continue;
            }
            const auto* aero = world.GetComponent<AeroComponent>(b.entity);
            if (!aero) {
                continue;
            }
            // 基準面積 = Cauchy の平均投影面積 x 演出用倍率。0 以下 (半径 0 / 面積倍率 0) は
            // 空力が定義できないので項ごと落とす (入力だけに依存する決定論的分岐)
            const float area = MeanProjectedAreaWorld(b.shapeCol, b.scale.x, b.scale.y, b.scale.z)
                             * aero->areaScale;
            if (area <= 0.0f) {
                continue;
            }
            const float rho = env ? env->airDensity : kDefaultAirDensity;
            // Cd: Aero の上書き (>0) → 材料の既定 → 球の 0.47。Cd は形状特性なので
            // Aero 側が正で、材料値はあくまで既定 (PhysMatLibrary.h の換算表コメント参照)
            float cd = aero->dragCoefficient;
            if (cd <= 0.0f) {
                const PhysMat* mat = b.shapeCol ? physmat::Resolve(b.shapeCol->physMaterial) : nullptr;
                cd = mat ? mat->dragCoefficient : kDefaultDragCoefficient;
            }
            // 面積から作る等価半径 (球なら実半径に一致)。マグヌスと角抗力の「腕の長さ」
            const float rEq = std::sqrt(area / XM_PI);
            // ---- 並進: 風に対する相対速度に効かせ、最後に風を足し戻す ----
            // ★両方 OFF のときは相対速度への往復自体を走らせない — v - w + w は float では
            //   元の v に戻るとは限らず、「全部 OFF の Aero を付けただけで挙動が動く」を避ける。
            // M59c: 面モデルを使うときは抗力を等方経路から外す (「抗力を出すか」= enableDrag と
            //       「どう出すか」= surfaceModel の 2 段。マグヌスは常に等方経路のまま)
            const bool isoDrag = aero->enableDrag && !aero->surfaceModel;
            if (isoDrag || aero->enableMagnus) {
                const float wndX = env ? env->windVelocity.x : 0.0f;
                const float wndY = env ? env->windVelocity.y : 0.0f;
                const float wndZ = env ? env->windVelocity.z : 0.0f;
                float rvx = b.vx - wndX, rvy = b.vy - wndY, rvz = b.vz - wndZ;
                if (isoDrag) {
                    // F = k |v| v (k = 0.5 rho Cd A) を**閉形式 implicit** で解く:
                    //   v' = v / (1 + (k |v| / m) h)。除算のみなので k をいくら大きくしても
                    //   符号が反転せず無条件安定 (陽的 v -= k|v|v/m h は簡単に発散する)。
                    // 終端速度は mg = k v_t^2 → v_t = sqrt(mg/k) に収束する (selftest が断言)
                    const float speed = std::sqrt(rvx * rvx + rvy * rvy + rvz * rvz);
                    if (speed > 0.0f) {
                        const float k = 0.5f * rho * cd * area;
                        const float scale = 1.0f / (1.0f + (k * speed * b.invMass) * h);
                        rvx *= scale;
                        rvy *= scale;
                        rvz *= scale;
                    }
                }
                if (aero->enableMagnus) {
                    // F = S (omega x v_rel)、S = magnus * 0.5 rho A r。符号の確認:
                    // +X へ進み +Y 軸まわりに回る球は omega x v = -Z を向く — +Z 側の表面が
                    // 流れに逆らって動き圧力が上がる側なので、力は -Z で物理的に正しい
                    // (PhysicsSelfTest がこの配置そのままで符号を断言する)
                    float mx, my, mz;
                    Cross(b.wx, b.wy, b.wz, rvx, rvy, rvz, mx, my, mz);
                    const float sMag = aero->magnusCoefficient * 0.5f * rho * area * rEq;
                    float dvx = mx * sMag * b.invMass * h;
                    float dvy = my * sMag * b.invMass * h;
                    float dvz = mz * sMag * b.invMass * h;
                    const float d2 = dvx * dvx + dvy * dvy + dvz * dvz;
                    if (d2 > kExplicitMaxDeltaV * kExplicitMaxDeltaV) {
                        const float clamp = kExplicitMaxDeltaV / std::sqrt(d2);
                        dvx *= clamp;
                        dvy *= clamp;
                        dvz *= clamp;
                    }
                    rvx += dvx;
                    rvy += dvy;
                    rvz += dvz;
                }
                b.vx = rvx + wndX;
                b.vy = rvy + wndY;
                b.vz = rvz + wndZ;
            }
            // ---- 面サンプリング空力 (M59c): 向きを見る抗力・揚力・風見安定 ----
            // 等方抗力の置き換え。面ごとの陽的な力なので Delta-v / Delta-omega をクランプする
            if (aero->enableDrag && aero->surfaceModel) {
                ShapePose ap = b.pose;
                if (b.shapeCol) {
                    ap = shapes::MakePose(*b.shapeCol, { b.pose.px, b.pose.py, b.pose.pz },
                                          { b.qx, b.qy, b.qz, b.qw }, b.scale);
                } else {
                    ap.shape = 0;
                    ap.radius = 0.5f; // 慣性・等方空力と同じ既定
                    ap.identityRot = 1;
                }
                AeroCoeffs ac;
                ac.density = rho;
                ac.windX = env ? env->windVelocity.x : 0.0f;
                ac.windY = env ? env->windVelocity.y : 0.0f;
                ac.windZ = env ? env->windVelocity.z : 0.0f;
                // 正対した平板の抗力が 1/2 rho Cd A u^2 と一致するのは Cn = Cd/2 のとき
                ac.normalCoeff = cd * 0.5f;
                ac.tangentCoeff = aero->skinFriction;
                AeroAccum acm;
                // ★M59f1: カーネルは「ap の原点まわり」で速度もトルクも組む。質量中心が
                //   ずれているときは (a) 原点における速度 v - ω×com を渡し、
                //   (b) 返ってきたトルクを τ - com×F で質量中心まわりへ移す。
                //   これで面の速度 v + ω×(p - com) とモーメント腕の両方が正しくなる
                float avx = b.vx, avy = b.vy, avz = b.vz;
                if (b.hasCom) {
                    float wcx, wcy, wcz;
                    Cross(b.wx, b.wy, b.wz, b.comx, b.comy, b.comz, wcx, wcy, wcz);
                    avx -= wcx;
                    avy -= wcy;
                    avz -= wcz;
                }
                AccumulateShapeAero(ap, avx, avy, avz, b.wx, b.wy, b.wz, ac, acm);
                if (b.hasCom) {
                    float ccx, ccy, ccz;
                    Cross(b.comx, b.comy, b.comz, acm.fx, acm.fy, acm.fz, ccx, ccy, ccz);
                    acm.tx -= ccx;
                    acm.ty -= ccy;
                    acm.tz -= ccz;
                }
                // 面積倍率は力もトルクも線形なので後掛けでよい
                const float as = aero->areaScale;
                float dvx = acm.fx * as * b.invMass * h;
                float dvy = acm.fy * as * b.invMass * h;
                float dvz = acm.fz * as * b.invMass * h;
                const float d2 = dvx * dvx + dvy * dvy + dvz * dvz;
                if (d2 > kExplicitMaxDeltaV * kExplicitMaxDeltaV) {
                    const float clamp = kExplicitMaxDeltaV / std::sqrt(d2);
                    dvx *= clamp;
                    dvy *= clamp;
                    dvz *= clamp;
                }
                b.vx += dvx;
                b.vy += dvy;
                b.vz += dvz;
                // トルク (freezeRot / kinematic は invI が零行列なので自然に無効)
                float dwx, dwy, dwz;
                MulInvI(b.invI, acm.tx * as * h, acm.ty * as * h, acm.tz * as * h, dwx, dwy, dwz);
                const float w2 = dwx * dwx + dwy * dwy + dwz * dwz;
                if (w2 > kExplicitMaxDeltaOmega * kExplicitMaxDeltaOmega) {
                    const float clamp = kExplicitMaxDeltaOmega / std::sqrt(w2);
                    dwx *= clamp;
                    dwy *= clamp;
                    dwz *= clamp;
                }
                b.wx += dwx;
                b.wy += dwy;
                b.wz += dwz;
            }

            // ---- 角速度の二次抗力 (同じ閉形式 implicit) ----
            // これが無いとマグヌスで回り始めた球が永遠に回り続ける (angularDamping は
            // 非物理の定率なので、空力を使うシーンでは 0 にしてこちらへ寄せるのが推奨)。
            // 慣性は invI の対角平均 = 逆テンソルの等方読み。freezeRot / kinematic は
            // 零行列なので invIbar が 0 になり自然に無効化される
            if (aero->enableAngularDrag) {
                const float invIbar = (b.invI[0][0] + b.invI[1][1] + b.invI[2][2]) / 3.0f;
                const float wlen = std::sqrt(b.wx * b.wx + b.wy * b.wy + b.wz * b.wz);
                if (invIbar > 0.0f && wlen > 0.0f) {
                    const float kw = 0.5f * rho * aero->angularDragCoefficient * area * rEq * rEq
                                   * rEq;
                    const float scale = 1.0f / (1.0f + (kw * wlen * invIbar) * h);
                    b.wx *= scale;
                    b.wy *= scale;
                    b.wz *= scale;
                }
            }
        }

        // ---- 翼面 (M59d): 子エンティティに置いた翼パネルが「最も近い Rigidbody 祖先」へ
        //      力とトルクを入れる。**子に置くことでレバー腕が生まれる**のが設計の核心 —
        //      M59c で確かめたとおり、対称形状の幾何中心まわりのトルクは原理的に 0 で、
        //      風見安定は圧力中心と質量中心のずれからしか出てこない ----
        {
            struct Panel {
                EntityID owner;
                const AeroSurfaceComponent* surf = nullptr;
                const LocalTransform* lt = nullptr;
            };
            std::vector<Panel> panels;
            const ComponentTypeId asReq[] = { AeroSurfaceComponent::sTypeId, LocalTransform::sTypeId };
            world.ForEachArchetype(asReq, [&](Archetype& arch) {
                const int si = arch.FindTypeIndex(AeroSurfaceComponent::sTypeId);
                const int li = arch.FindTypeIndex(LocalTransform::sTypeId);
                for (uint32_t row = 0; row < arch.Count(); ++row) {
                    const EntityID e = arch.EntityAt(row);
                    if (!IsEntityActive(world, e)) {
                        continue;
                    }
                    panels.push_back({ e,
                                       static_cast<const AeroSurfaceComponent*>(arch.GetPtr(si, row)),
                                       static_cast<const LocalTransform*>(arch.GetPtr(li, row)) });
                }
            });
            if (!panels.empty()) {
                std::sort(panels.begin(), panels.end(),
                          [](const Panel& a, const Panel& b) { return a.owner.index < b.owner.index; });
                // bodies は index 昇順ソート済 → 二分探索 (SpringJoint と同じ流儀)
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
                const float rhoAir = env ? env->airDensity : kDefaultAirDensity;
                const float wndX = env ? env->windVelocity.x : 0.0f;
                const float wndY = env ? env->windVelocity.y : 0.0f;
                const float wndZ = env ? env->windVelocity.z : 0.0f;
                constexpr float kDeg2Rad = 3.14159265f / 180.0f;
                for (const Panel& p : panels) {
                    if (p.surf->area <= 0.0f) {
                        continue;
                    }
                    // 力の入り先 = 自分 → 祖先の順に最初に見つかった動的剛体
                    Body* body = nullptr;
                    for (EntityID cur = p.owner; !cur.IsNull(); cur = world.GetParent(cur)) {
                        Body* b = findBody(cur);
                        if (b && b->rb && b->invMass > 0.0f) {
                            body = b;
                            break;
                        }
                    }
                    if (!body) {
                        continue; // 静的 / kinematic 祖先しかいない = 翼は何も動かせない
                    }
                    // パネルのワールド姿勢 (剛体と同じ scalar 合成)
                    const WorldFrame pf = ComposeParentFrame(world, p.owner);
                    XMFLOAT3 wpos;
                    XMFLOAT4 wrot;
                    XMFLOAT3 wscale;
                    ApplyFrame(pf, *p.lt, wpos, wrot, wscale);
                    float nx, ny, nz;
                    QuatRotate(wrot.x, wrot.y, wrot.z, wrot.w, p.surf->normal.x, p.surf->normal.y,
                               p.surf->normal.z, nx, ny, nz);
                    const float nlen2 = nx * nx + ny * ny + nz * nz;
                    if (nlen2 < 1e-12f) {
                        continue; // 法線が定義できない (決定論的分岐)
                    }
                    const float ninv = 1.0f / std::sqrt(nlen2);
                    nx *= ninv;
                    ny *= ninv;
                    nz *= ninv;
                    // パネル点の速度 (親剛体の剛体運動) から見た相対風
                    // M59f1: 腕は**質量中心から**測る。com は hasCom が false なら +0.0f
                    // 固定なので、この 3 行は従来とビット同一のまま
                    const float rx = wpos.x - body->pose.px - body->comx;
                    const float ry = wpos.y - body->pose.py - body->comy;
                    const float rz = wpos.z - body->pose.pz - body->comz;
                    float wr0, wr1, wr2;
                    Cross(body->wx, body->wy, body->wz, rx, ry, rz, wr0, wr1, wr2);
                    const float ux = body->vx + wr0 - wndX;
                    const float uy = body->vy + wr1 - wndY;
                    const float uz = body->vz + wr2 - wndZ;
                    const float sp2 = ux * ux + uy * uy + uz * uz;
                    if (sp2 < 1e-8f) {
                        continue; // 流れが無い
                    }
                    const float sp = std::sqrt(sp2);
                    const float ihx = ux / sp, ihy = uy / sp, ihz = uz / sp;
                    const float ndotu = nx * ihx + ny * ihy + nz * ihz;
                    const float sinA = -ndotu; // 正 = 法線側から風を受ける = 正の迎角
                    // 揚力方向 = 流れに垂直な法線成分 (正の迎角で法線側を向く)
                    float lx = nx - ndotu * ihx;
                    float ly = ny - ndotu * ihy;
                    float lz = nz - ndotu * ihz;
                    const float l2 = lx * lx + ly * ly + lz * lz;
                    float cl = 0.0f;
                    if (l2 > 1e-12f) {
                        const float linv = 1.0f / std::sqrt(l2);
                        lx *= linv;
                        ly *= linv;
                        lz *= linv;
                        // CL: 失速角までは薄翼理論の線形、超えたら平板 (2 sin cos) へ落ちる。
                        // ★平板側を「失速点で連続」になるよう正規化しては**いけない** —
                        //   2 sin cos は 45 度で最大なので、正規化すると失速後に CL が
                        //   CLmax の 2 倍まで**増えて**しまい失速の意味が逆転する。
                        //   素の平板値は失速点で CLmax より低いので、そこへ向かって落とすのが正しい
                        //   (= 揚力の崩壊そのもの)。段差でびびらないよう失速角の 0.5 倍の幅で
                        //   線形に混ぜる (三角関数を増やさず滑らかにするための最小の細工)
                        const float sStall = std::sin(p.surf->stallAngleDeg * kDeg2Rad);
                        const float aAbs = std::fabs(sinA);
                        if (aAbs <= sStall) {
                            cl = p.surf->liftSlope * sinA;
                        } else {
                            const float c2 = 1.0f - sinA * sinA;
                            const float cosA = (c2 > 0.0f) ? std::sqrt(c2) : 0.0f;
                            const float flat = 2.0f * aAbs * cosA;
                            const float clMax = p.surf->liftSlope * sStall;
                            const float wnd = sStall * 0.5f;
                            float t = (wnd > 1e-8f) ? (aAbs - sStall) / wnd : 1.0f;
                            if (t > 1.0f) {
                                t = 1.0f;
                            }
                            const float sign = (sinA >= 0.0f) ? 1.0f : -1.0f;
                            cl = sign * (clMax * (1.0f - t) + flat * t);
                        }
                    } else {
                        lx = 0.0f;
                        ly = 0.0f;
                        lz = 0.0f; // 流れが法線と平行 = 揚力の向きが定義できない
                    }
                    const float cd = p.surf->dragCoefficient + p.surf->inducedDrag * cl * cl
                                   + p.surf->stalledDrag * sinA * sinA;
                    const float q = 0.5f * rhoAir * sp2 * p.surf->area;
                    const float fx = q * (cl * lx - cd * ihx);
                    const float fy = q * (cl * ly - cd * ihy);
                    const float fz = q * (cl * lz - cd * ihz);
                    // 陽的な力なので Delta-v / Delta-omega に決定論的な頭打ちを掛ける
                    float dvx = fx * body->invMass * h;
                    float dvy = fy * body->invMass * h;
                    float dvz = fz * body->invMass * h;
                    const float d2 = dvx * dvx + dvy * dvy + dvz * dvz;
                    if (d2 > kExplicitMaxDeltaV * kExplicitMaxDeltaV) {
                        const float clampV = kExplicitMaxDeltaV / std::sqrt(d2);
                        dvx *= clampV;
                        dvy *= clampV;
                        dvz *= clampV;
                    }
                    body->vx += dvx;
                    body->vy += dvy;
                    body->vz += dvz;
                    float tx, ty, tz;
                    Cross(rx, ry, rz, fx, fy, fz, tx, ty, tz);
                    float dwx, dwy, dwz;
                    MulInvI(body->invI, tx * h, ty * h, tz * h, dwx, dwy, dwz);
                    const float w2 = dwx * dwx + dwy * dwy + dwz * dwz;
                    if (w2 > kExplicitMaxDeltaOmega * kExplicitMaxDeltaOmega) {
                        const float clampW = kExplicitMaxDeltaOmega / std::sqrt(w2);
                        dwx *= clampW;
                        dwy *= clampW;
                        dwz *= clampW;
                    }
                    body->wx += dwx;
                    body->wy += dwy;
                    body->wz += dwz;
                }
            }
        }

        // ---- 浮力 (M59b2): 水面より下の排除体積ぶんの上向き力 + 水中抗力。空力の直後に
        //      置くのは、浮力 (陽的な復元力) で付いた速度をその tick のうちに水中抗力が
        //      減衰させるため。**非所持ボディはルックアップのみで fp 演算ゼロ** ----
        for (Body& b : bodies) {
            if (!b.rb || b.invMass == 0.0f) {
                continue;
            }
            const auto* buoy = world.GetComponent<BuoyancyComponent>(b.entity);
            if (!buoy || buoy->volumeScale <= 0.0f) {
                continue;
            }
            const float planeY = env ? env->waterPlaneY : kDefaultWaterPlaneY;
            const float rhoW = env ? env->waterDensity : kDefaultWaterDensity;
            if (rhoW <= 0.0f) {
                continue;
            }
            // 形状は shapeCol から組み直す — b.pose はソリッドなコライダーのときしか
            // 形状が入っていない (トリガー併用のボディでも浮きたいので自前で作る)
            ShapePose bp = b.pose;
            if (b.shapeCol) {
                bp = shapes::MakePose(*b.shapeCol, { b.pose.px, b.pose.py, b.pose.pz },
                                      { b.qx, b.qy, b.qz, b.qw }, b.scale);
            } else {
                bp.shape = 0;
                bp.radius = 0.5f; // 慣性・空力と同じ「半径 0.5 の球」既定
                bp.identityRot = 1;
            }
            float centroidY = bp.py;
            const float frac = SubmergedFractionWorld(bp, planeY, centroidY);
            if (frac <= 0.0f) {
                continue; // 完全に水面より上 = 何も足さない (陸上シーンは従来経路のまま)
            }
            float total = b.shapeCol ? ShapeVolumeWorld(*b.shapeCol, b.scale.x, b.scale.y, b.scale.z)
                                     : 0.0f;
            if (total <= 0.0f) {
                total = (4.0f / 3.0f) * XM_PI * 0.125f; // 半径 0.5 の球 (mesh / コライダー無し)
            }
            const float vSub = total * buoy->volumeScale * frac;
            // 浮力 = rho_w * V_sub * |g|、向きは **+Y 固定**。水面が軸平行 (ワールド Y) である
            // 以上、重力ベクトルを傾けたときに浮力だけ傾けても意味を成さない (M59 の割り切り)
            float gMag = -kGravity; // env 不在は従来定数の大きさ
            if (env) {
                gMag = std::sqrt(env->gravity.x * env->gravity.x + env->gravity.y * env->gravity.y
                                 + env->gravity.z * env->gravity.z);
            }
            float jy = rhoW * vSub * gMag * h;
            const float maxJ = kExplicitMaxDeltaV / b.invMass;
            if (jy > maxJ) {
                jy = maxJ;
            }
            // 作用点は浮力中心 (没水部分の体積重心)。腕は **質量中心から** 測る。
            // ★M59f1 でここが効き出した: centerOfMass を下げた浮体は r が水平成分を持ち、
            //   r × (0, jy, 0) が**復原モーメント**になる (式は M59b2 から 1 文字も変えていない)。
            //   質量中心が形状原点のままなら r は +Y のみ = 外積が恒等 0 で従来どおり
            ApplyImpulse(b, -b.comx, centroidY - bp.py - b.comy, -b.comz, 0.0f, jy, 0.0f, 1.0f);
            // 水中抗力: 没水割合で按分した閉形式 implicit (静水前提 = 流れの場は持たない)
            if (buoy->linearDrag > 0.0f) {
                const float scale = 1.0f / (1.0f + buoy->linearDrag * frac * h);
                b.vx *= scale;
                b.vy *= scale;
                b.vz *= scale;
            }
            if (buoy->angularDrag > 0.0f) {
                const float scale = 1.0f / (1.0f + buoy->angularDrag * frac * h);
                b.wx *= scale;
                b.wy *= scale;
                b.wz *= scale;
            }
        }

        // ---- ジャイロ項 ω×Iω (M59f1): **陰的**に解く。opt-in (Rigidbody.gyroscopic) ----
        // 剛体の回転方程式は I ω̇ + ω×Iω = τ。従来はこの第 2 項を丸ごと落としていた
        // (= 対称でない物体が回っても軸が動かない)。陽的に足すとエネルギーが単調に増えて
        // 必ず発散するので、後退 Euler を Newton で解く:
        //   f(ω) = I ω - I ω₀ + h (ω̄ × I ω̄) = 0    (ω̄ = (ω + ω₀)/2)
        //   J     = I + (h/2) ( skew(ω̄)·I - skew(I ω̄) )
        // ★中点を使う (**後退 Euler ではない**)。後退 Euler は無条件安定だが保存則を
        //   1 つも持たず、実測で無トルクの箱の |L| が 4 秒に 7.3% 落ちた。陰的中点は
        //   この系の二次不変量 (|L|²・回転エネルギー) を保つので、同じコストで
        //   「無トルクなら L は保存する」を試験に書けるようになる。
        //   ※1 反復目は ω̄ = ω₀ で後退 Euler と同一 — 中点が効くのは 2 反復目から
        // 主軸ローカル (I が対角) で解いてワールドへ戻す。反復数は固定 (収束判定なし)
        for (Body& b : bodies) {
            if (!b.rb || !b.gyro || b.freezeRot || b.invMass == 0.0f) {
                continue;
            }
            const float* B[3] = { b.pose.bx, b.pose.by, b.pose.bz };
            const float I[3] = { b.Ilx, b.Ily, b.Ilz };
            // ワールド → 主軸ローカル (基底は正規直交なので転置が逆行列)
            float w[3] = { b.wx * B[0][0] + b.wy * B[0][1] + b.wz * B[0][2],
                           b.wx * B[1][0] + b.wy * B[1][1] + b.wz * B[1][2],
                           b.wx * B[2][0] + b.wy * B[2][1] + b.wz * B[2][2] };
            const float Iw0[3] = { I[0] * w[0], I[1] * w[1], I[2] * w[2] };
            const float w0[3] = { w[0], w[1], w[2] };
            const float hh = h * 0.5f;
            for (int it = 0; it < kGyroIterations; ++it) {
                // 中点の角速度で ω×Iω を評価する
                const float wm[3] = { (w[0] + w0[0]) * 0.5f, (w[1] + w0[1]) * 0.5f,
                                      (w[2] + w0[2]) * 0.5f };
                const float Iwm[3] = { I[0] * wm[0], I[1] * wm[1], I[2] * wm[2] };
                float cx, cy, cz;
                Cross(wm[0], wm[1], wm[2], Iwm[0], Iwm[1], Iwm[2], cx, cy, cz);
                const float f[3] = { I[0] * w[0] - Iw0[0] + h * cx,
                                     I[1] * w[1] - Iw0[1] + h * cy,
                                     I[2] * w[2] - Iw0[2] + h * cz };
                // J = diag(I) + (h/2) ( skew(ω̄)diag(I) - skew(Iω̄) )。行 i の非対角は
                // 共通因子 (I_{i+2} - I_{i+1}) を持つ — 主軸慣性が全て等しい球では
                // J が対角 = f も恒等 0 になり、**球には何も起こらない**のが構造から言える
                const float d0 = I[2] - I[1];
                const float d1 = I[0] - I[2];
                const float d2 = I[1] - I[0];
                const float J[3][3] = {
                    { I[0], hh * wm[2] * d0, hh * wm[1] * d0 },
                    { hh * wm[2] * d1, I[1], hh * wm[0] * d1 },
                    { hh * wm[1] * d2, hh * wm[0] * d2, I[2] },
                };
                float dw[3];
                if (!Solve3x3(J, f, dw)) {
                    break; // 退化 (慣性 0 等) — 何もしないのが安全側
                }
                w[0] -= dw[0];
                w[1] -= dw[1];
                w[2] -= dw[2];
            }
            b.wx = w[0] * B[0][0] + w[1] * B[1][0] + w[2] * B[2][0];
            b.wy = w[0] * B[0][1] + w[1] * B[1][1] + w[2] * B[2][1];
            b.wz = w[0] * B[0][2] + w[1] * B[1][2] + w[2] * B[2][2];
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
                    // λ = −(k·stretch + c·vrel)·h / (1 + c·h·Σinvm)。分母が implicit damping
                    float lambda = -(k * stretch + c * vrel) * h / (1.0f + c * h * invSum);
                    // 極端な剛性でも 1 ステップの Δv を 100 m/s に制限 (発散防止の決定論的
                    // クランプ)。★M59g2 からこれは**サブステップごと**なので実効上限は
                    // 100*substeps m/s へ緩む — 防波堤としての役割は変わらない
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
                const float mx = std::fabs(bodies[i].vx) * h + kBroadphaseMargin;
                const float my = std::fabs(bodies[i].vy) * h + kBroadphaseMargin;
                const float mz = std::fabs(bodies[i].vz) * h + kBroadphaseMargin;
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

        // ---- 接触制約の生成 (M59g1): マニフォールドは **1 ステップに 1 回だけ**作る ----
        // M28b は反復のたびに CollideManifold を呼び直していた。それをやめたのは、
        // **蓄積インパルスの前提が「接触点が反復をまたいで同じであること」**だから —
        // 毎回作り直すと「この接触点にこれまで何 N*s 入れたか」を持ち越す先が消える。
        //
        // ★**解き方の「形」は M28b のまま**にした (中央法線 1 発 → 点毎 Jacobi → 重心摩擦)。
        //   点ごとの逐次 (Gauss-Seidel) へ作り替える方が教科書的だが、**warm starting 無しでは
        //   スタックが目に見えて歩く** (実測: 3 段タワーの 600 tick ドリフトが 0.7mm → 75mm)。
        //   中央法線インパルスが「並進を全質量で 1 発で解く」役をしていて、それが 8 反復しか
        //   回さないソルバの安定性を支えている。warm starting は M59h の計測ゲート付き別コミット
        //   なので、ここでは**蓄積とクランプだけ**を足す。
        // ★物理モデルは 1 つも変えていない: mu = sqrt(mu_a * mu_b) / e = min(e_a, e_b) /
        //   kRestitutionVelThreshold / kPenetrationSlop はそのまま。
        struct ContactPoint {
            float ra[3] = { 0, 0, 0 }; // 接触点 - A 重心 (生成時に固定)
            float rb[3] = { 0, 0, 0 };
            float depth = 0.0f;
            float massN = 0.0f;   // 法線方向の有効質量
            float lambdaN = 0.0f; // 蓄積法線インパルス (>= 0)
        };
        struct ContactConstraint {
            uint32_t ai = 0, bi = 0;
            float nx = 0, ny = 1, nz = 0;
            float t1[3] = { 1, 0, 0 };
            float t2[3] = { 0, 0, 1 };
            float mu = 0.0f;
            int count = 0;
            float cpx = 0, cpy = 0, cpz = 0; // 代表点 = マニフォールド重心 (M59e の出力)
            float raC[3] = { 0, 0, 0 };      // 重心の r (中央インパルスと摩擦の作用点)
            float rbC[3] = { 0, 0, 0 };
            float massNc = 0.0f;             // 重心での法線有効質量
            float massT1 = 0.0f, massT2 = 0.0f;
            float biasC = 0.0f;              // 反発の目標法線速度 (閾値適用済み)
            float lambdaNc = 0.0f;           // 蓄積: 中央法線
            float lambdaT1 = 0.0f, lambdaT2 = 0.0f; // 蓄積: 重心摩擦
            ContactPoint pts[4];
        };
        std::vector<ContactConstraint> constraints;
        constraints.reserve(candidates.size());
        for (const uint64_t pairKey : candidates) {
            const uint32_t ai = static_cast<uint32_t>(pairKey >> 32);
            const uint32_t bi = static_cast<uint32_t>(pairKey & 0xFFFFFFFFu);
            Body& A = bodies[ai];
            Body& B = bodies[bi];
            if (A.invMass + B.invMass == 0.0f) {
                continue; // 両方不動 (静的 / kinematic 同士)
            }
            shapes::Manifold m;
            if (!shapes::CollideManifold(A.pose, B.pose, m)) {
                continue;
            }
            ContactConstraint c;
            c.ai = ai;
            c.bi = bi;
            c.nx = m.nx;
            c.ny = m.ny;
            c.nz = m.nz;
            c.mu = std::sqrt(A.friction * B.friction);
            c.count = m.count;
            // 法線から接線基底を決定論的に作る (分岐は入力だけに依存)。
            // 0.57735 = 1/sqrt(3) — 最も長い成分を避けて正規化の桁落ちを防ぐ古典手法。
            // ★接線が**固定**なのが M28b との違い: 蓄積するには方向が動いてはいけない
            //   (旧実装は毎反復その場の滑り方向を測っていたので蓄積できなかった)
            {
                float ax, ay, az;
                if (std::fabs(c.nx) >= 0.57735f) {
                    ax = c.ny; ay = -c.nx; az = 0.0f;
                } else {
                    ax = 0.0f; ay = c.nz; az = -c.ny;
                }
                const float al = std::sqrt(ax * ax + ay * ay + az * az);
                if (al > 1e-8f) {
                    c.t1[0] = ax / al; c.t1[1] = ay / al; c.t1[2] = az / al;
                }
                Cross(c.nx, c.ny, c.nz, c.t1[0], c.t1[1], c.t1[2], c.t2[0], c.t2[1], c.t2[2]);
            }
            const float e = std::min(A.restitution, B.restitution);
            const float invCount = 1.0f / static_cast<float>(m.count);
            for (int k = 0; k < m.count; ++k) {
                ContactPoint& p = c.pts[k];
                // M59f1: 腕は質量中心から。com* は hasCom が false なら +0.0f 固定なので
                // 「x - (+0.0f) == x」でビット同一 (-0.0f も保つ = 従来経路そのまま)
                p.ra[0] = m.pts[k].px - A.pose.px - A.comx;
                p.ra[1] = m.pts[k].py - A.pose.py - A.comy;
                p.ra[2] = m.pts[k].pz - A.pose.pz - A.comz;
                p.rb[0] = m.pts[k].px - B.pose.px - B.comx;
                p.rb[1] = m.pts[k].py - B.pose.py - B.comy;
                p.rb[2] = m.pts[k].pz - B.pose.pz - B.comz;
                p.depth = m.pts[k].depth;
                c.cpx += m.pts[k].px * invCount;
                c.cpy += m.pts[k].py * invCount;
                c.cpz += m.pts[k].pz * invCount;
                const float kn = EffectiveMassInv(A, p.ra[0], p.ra[1], p.ra[2], c.nx, c.ny, c.nz)
                               + EffectiveMassInv(B, p.rb[0], p.rb[1], p.rb[2], c.nx, c.ny, c.nz);
                p.massN = (kn > 0.0f) ? 1.0f / kn : 0.0f;
            }
            c.raC[0] = c.cpx - A.pose.px - A.comx;
            c.raC[1] = c.cpy - A.pose.py - A.comy;
            c.raC[2] = c.cpz - A.pose.pz - A.comz;
            c.rbC[0] = c.cpx - B.pose.px - B.comx;
            c.rbC[1] = c.cpy - B.pose.py - B.comy;
            c.rbC[2] = c.cpz - B.pose.pz - B.comz;
            {
                const float kn = EffectiveMassInv(A, c.raC[0], c.raC[1], c.raC[2], c.nx, c.ny, c.nz)
                               + EffectiveMassInv(B, c.rbC[0], c.rbC[1], c.rbC[2], c.nx, c.ny, c.nz);
                c.massNc = (kn > 0.0f) ? 1.0f / kn : 0.0f;
                const float kt1 = EffectiveMassInv(A, c.raC[0], c.raC[1], c.raC[2], c.t1[0], c.t1[1],
                                                   c.t1[2])
                                + EffectiveMassInv(B, c.rbC[0], c.rbC[1], c.rbC[2], c.t1[0], c.t1[1],
                                                   c.t1[2]);
                c.massT1 = (kt1 > 0.0f) ? 1.0f / kt1 : 0.0f;
                const float kt2 = EffectiveMassInv(A, c.raC[0], c.raC[1], c.raC[2], c.t2[0], c.t2[1],
                                                   c.t2[2])
                                + EffectiveMassInv(B, c.rbC[0], c.rbC[1], c.rbC[2], c.t2[0], c.t2[1],
                                                   c.t2[2]);
                c.massT2 = (kt2 > 0.0f) ? 1.0f / kt2 : 0.0f;
            }
            // 反発のバイアスは**生成時の接近速度から 1 回だけ**決める。反復ごとに測り直すと
            // 自分が入れたインパルスで接近速度が消えていくので、反発が二重に効いたり
            // 消えたりする (蓄積インパルスで反発を扱うときの定石)
            {
                float wax, way, waz, wbx, wby, wbz;
                Cross(A.wx, A.wy, A.wz, c.raC[0], c.raC[1], c.raC[2], wax, way, waz);
                Cross(B.wx, B.wy, B.wz, c.rbC[0], c.rbC[1], c.rbC[2], wbx, wby, wbz);
                const float rvx = (A.vx + wax) - (B.vx + wbx);
                const float rvy = (A.vy + way) - (B.vy + wby);
                const float rvz = (A.vz + waz) - (B.vz + wbz);
                const float vn = rvx * c.nx + rvy * c.ny + rvz * c.nz;
                // 低速接触は e=0 扱い (micro-bounce 除去 = 静止安定の柱。M28b から不変)
                if (vn < 0.0f && -vn >= restitutionVelThreshold) {
                    c.biasC = -e * vn;
                }
            }
            constraints.push_back(c);
        }

        // ---- 接触解決 (固定反復・生成順 = 候補ペアの (小,大) 昇順 = 決定論) ----
        // 3 段構成は M28b のまま。違うのは各段が**蓄積量 lambda を持ちクランプする**こと:
        //   1. 重心での中央法線インパルス (反発込み) — 並進を全質量で 1 発。lambda >= 0
        //   2. 点毎 Jacobi 法線インパルス (同一速度から一括計算・点数分配) — 回転の不均衡だけ。
        //      対称接触では自動的にゼロになる = スタックが歩かない性質はここから来ている
        //   3. 重心でのクーロン摩擦 (固定接線 2 方向)。上限は**その時点の蓄積法線インパルス合計**
        //      — 旧実装の「その反復ぶんの法線インパルス」より正しい Coulomb 境界になっている
        for (int iter = 0; iter < kSolverIterations; ++iter) {
            for (ContactConstraint& c : constraints) {
                Body& A = bodies[c.ai];
                Body& B = bodies[c.bi];
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
                if (c.massNc > 0.0f) {
                    float rvx, rvy, rvz;
                    relVelAt(c.raC, c.rbC, rvx, rvy, rvz);
                    const float vn = rvx * c.nx + rvy * c.ny + rvz * c.nz;
                    float dl = -(vn - c.biasC) * c.massNc;
                    const float old = c.lambdaNc;
                    float now = old + dl;
                    if (now < 0.0f) {
                        now = 0.0f; // 接触は押すだけ (引っ張らない)
                    }
                    dl = now - old;
                    c.lambdaNc = now;
                    if (dl != 0.0f) {
                        ApplyImpulse(A, c.raC[0], c.raC[1], c.raC[2], c.nx * dl, c.ny * dl, c.nz * dl,
                                     1.0f);
                        ApplyImpulse(B, c.rbC[0], c.rbC[1], c.rbC[2], c.nx * dl, c.ny * dl, c.nz * dl,
                                     -1.0f);
                    }
                }
                // ---- 2. 点毎 法線インパルス (反発なし。回転の不均衡だけを担当) ----
                // ★M28b はここを 1/count に緩和した Jacobi にしていた。**蓄積インパルスでは
                //   その緩和が収束の律速になる** — 4 点マニフォールドだと毎反復 1/4 しか
                //   進まないので、8 反復では回転の残差が消えず 2 段タワーすら静定しない
                //   (実測。旧方式は毎反復ゼロから解き直していたので緩和が効いていた)。
                //   蓄積 + 0 クランプがあれば緩和無しでも暴れないので、緩和を外した。
                //   一括適用 (Jacobi) は維持している — 点を順に適用すると走査順の非対称が
                //   トルクとして残り、対称なスタックが歩き出すため
                float totalJn = c.lambdaNc;
                if (c.count > 1) {
                    float dls[4] = {};
                    for (int k = 0; k < c.count; ++k) { // 同一速度から一括計算 = Jacobi
                        ContactPoint& p = c.pts[k];
                        if (p.massN <= 0.0f) {
                            continue;
                        }
                        float rvx, rvy, rvz;
                        relVelAt(p.ra, p.rb, rvx, rvy, rvz);
                        const float vn = rvx * c.nx + rvy * c.ny + rvz * c.nz;
                        float dl = -vn * p.massN;
                        const float old = p.lambdaN;
                        float now = old + dl;
                        if (now < 0.0f) {
                            now = 0.0f;
                        }
                        dls[k] = now - old;
                        p.lambdaN = now;
                    }
                    for (int k = 0; k < c.count; ++k) { // まとめて適用
                        if (dls[k] != 0.0f) {
                            const ContactPoint& p = c.pts[k];
                            ApplyImpulse(A, p.ra[0], p.ra[1], p.ra[2], c.nx * dls[k], c.ny * dls[k],
                                         c.nz * dls[k], 1.0f);
                            ApplyImpulse(B, p.rb[0], p.rb[1], p.rb[2], c.nx * dls[k], c.ny * dls[k],
                                         c.nz * dls[k], -1.0f);
                        }
                    }
                }
                for (int k = 0; k < c.count; ++k) {
                    totalJn += c.pts[k].lambdaN;
                }
                // ---- 3. 重心でのクーロン摩擦 (固定接線 2 方向、蓄積してクランプ) ----
                if (totalJn > 0.0f) {
                    const float maxF = c.mu * totalJn;
                    for (int ti = 0; ti < 2; ++ti) {
                        const float* d = (ti == 0) ? c.t1 : c.t2;
                        const float mass = (ti == 0) ? c.massT1 : c.massT2;
                        float& acc = (ti == 0) ? c.lambdaT1 : c.lambdaT2;
                        if (mass <= 0.0f) {
                            continue;
                        }
                        float rvx, rvy, rvz;
                        relVelAt(c.raC, c.rbC, rvx, rvy, rvz);
                        const float vt = rvx * d[0] + rvy * d[1] + rvz * d[2];
                        float dl = -vt * mass;
                        const float old = acc;
                        float now = old + dl;
                        if (now > maxF) {
                            now = maxF;
                        } else if (now < -maxF) {
                            now = -maxF;
                        }
                        dl = now - old;
                        acc = now;
                        if (dl != 0.0f) {
                            ApplyImpulse(A, c.raC[0], c.raC[1], c.raC[2], d[0] * dl, d[1] * dl,
                                         d[2] * dl, 1.0f);
                            ApplyImpulse(B, c.rbC[0], c.rbC[1], c.rbC[2], d[0] * dl, d[1] * dl,
                                         d[2] * dl, -1.0f);
                        }
                    }
                }
            }
        }

        // ---- 位置補正 (M59g1): 速度ソルバから**切り離した別パス** ----
        // 速度を解いている最中に姿勢を動かすと、生成時に固定した接触点・有効質量・法線と
        // 食い違っていく (M28b は毎反復マニフォールドを作り直していたので問題にならなかった。
        // 実測: 分離しないと 2 段タワーですら跳ね続け、下の接触インパルスが 2*m*g*dt の
        // 2.3 倍に膨らんだ)。押し出しだけは**その場の真の貫通量**が要るので、ここでだけ
        // CollideManifold を呼び直す。回転補正はしない (簡易ソルバの発散防止。M28b から不変)
        for (int pass = 0; pass < kPositionIterations; ++pass) {
            for (const uint64_t pairKey : candidates) {
                Body& A = bodies[static_cast<size_t>(pairKey >> 32)];
                Body& B = bodies[static_cast<size_t>(pairKey & 0xFFFFFFFFu)];
                const float tim = A.invMass + B.invMass;
                if (tim == 0.0f) {
                    continue;
                }
                shapes::Manifold m;
                if (!shapes::CollideManifold(A.pose, B.pose, m)) {
                    continue;
                }
                float maxDepth = 0.0f;
                for (int k = 0; k < m.count; ++k) {
                    if (m.pts[k].depth > maxDepth) {
                        maxDepth = m.pts[k].depth;
                    }
                }
                const float corr = std::max(maxDepth - kPenetrationSlop, 0.0f);
                const float ci = corr * A.invMass / tim;
                const float cj = corr * B.invMass / tim;
                A.pose.px += m.nx * ci;
                A.pose.py += m.ny * ci;
                A.pose.pz += m.nz * ci;
                B.pose.px -= m.nx * cj;
                B.pose.py -= m.ny * cj;
                B.pose.pz -= m.nz * cj;
            }
        }

        // ---- 接触ペアの出力 (M28c、M59e で代表点と法線インパルス、M59g2 でサブステップ合算) ----
        // 生成順 = 候補ペアの (小,大) 昇順なので key も自動的に昇順のまま
        // (CollisionSystem の二分探索 FindContactNormal の前提)。
        // ★impulse は**真の蓄積 lambda の合計**で、サブステップをまたいで足す — 消費者から見た
        //   「その tick に入った法線インパルス」の意味を substeps に依らず保つため
        //   (静止した質量 m の物体はサブステップ数に関係なく m*g*dt になる)。
        // ★出力は各サブステップの**和集合**。1 度でも触れたペアは報告される — Enter/Exit の
        //   意味論として「この tick に接触したか」が欲しいのは CollisionSystem 側の要求
        if (outContacts) {
            for (const ContactConstraint& c : constraints) {
                SolidContact sc;
                sc.key = (static_cast<uint64_t>(bodies[c.ai].entity.index) << 32)
                       | bodies[c.bi].entity.index;
                sc.nx = c.nx;
                sc.ny = c.ny;
                sc.nz = c.nz;
                sc.px = c.cpx;
                sc.py = c.cpy;
                sc.pz = c.cpz;
                float total = c.lambdaNc;
                for (int k = 0; k < c.count; ++k) {
                    total += c.pts[k].lambdaN;
                }
                sc.impulse = total;
                subContacts.push_back(sc);
            }
            MergeSubstepContacts(subContacts, *outContacts);
            subContacts.clear();
        }

        // ---- 位置・姿勢積分 (解決後の速度で前進) ----
        for (Body& b : bodies) {
            if (b.invMass == 0.0f || !b.rb) {
                continue;
            }
            // ★M59f1: 回転の中心は**質量中心**であって形状原点ではない。オフセットが
            //   あるときは「質量中心を v·h だけ前進 → 姿勢を積分 → 新しい姿勢で形状原点を
            //   逆算」の順に組む。オフセットが無いときは従来の 3 行をそのまま通す —
            //   `pose.p + 0.0f` が -0.0f を +0.0f に化けさせるので、共通化してはいけない
            float comWx = 0.0f, comWy = 0.0f, comWz = 0.0f;
            if (b.hasCom) {
                comWx = b.pose.px + b.comx + b.vx * h;
                comWy = b.pose.py + b.comy + b.vy * h;
                comWz = b.pose.pz + b.comz + b.vz * h;
            } else {
                b.pose.px += b.vx * h;
                b.pose.py += b.vy * h;
                b.pose.pz += b.vz * h;
            }
            if (!b.freezeRot) {
                // q += 0.5·h·(ω_quat ⊗ q)、その後正規化 (全て scalar)
                const float hx = b.wx * 0.5f * h, hy = b.wy * 0.5f * h, hz = b.wz * 0.5f * h;
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
            if (b.hasCom) {
                // 新しい姿勢でのオフセットを引いて形状原点を戻す (com* 自体は次のサブ
                // ステップ頭の pose 確定で取り直される)
                float ox, oy, oz;
                QuatRotate(b.qx, b.qy, b.qz, b.qw, b.comLx, b.comLy, b.comLz, ox, oy, oz);
                b.pose.px = comWx - ox;
                b.pose.py = comWy - oy;
                b.pose.pz = comWz - oz;
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

const PhysicsEnvironmentComponent* ResolvePhysicsEnvironment(World& world)
{
    // Skybox / Fog と同じ「entity.index 最小の active な 1 個」。アーキタイプの走査順に
    // 依存させないため、早期 return せず全件見てから最小 index を採る
    const PhysicsEnvironmentComponent* best = nullptr;
    uint32_t bestIndex = 0xFFFFFFFFu;
    const ComponentTypeId req[] = { PhysicsEnvironmentComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int ei = arch.FindTypeIndex(PhysicsEnvironmentComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            if (e.index >= bestIndex || !IsEntityActive(world, e)) {
                continue;
            }
            bestIndex = e.index;
            best = static_cast<const PhysicsEnvironmentComponent*>(arch.GetPtr(ei, row));
        }
    });
    return best;
}

float MeanProjectedAreaWorld(const ColliderComponent* col, float sx, float sy, float sz)
{
    // 負スケールは絶対値 (ShapeVolumeWorld / shapes::ApplyScaledExtents と同一規約)
    sx = std::fabs(sx);
    sy = std::fabs(sy);
    sz = std::fabs(sz);
    // コライダー無し / 動的 mesh は慣性導出 (LocalInertiaDiag) と同じ「半径 0.5 の球」既定。
    // ここを 0 にすると「空力が黙って効かない」になるので、既定の代表形状へ落とす
    if (!col || col->shape == 3) {
        return XM_PI * 0.25f;
    }
    switch (col->shape) {
    case 0: { // 球 = 最大成分スケール。表面積 4 pi r^2 の 1/4 = pi r^2
        const float r = col->radius * std::max(sx, std::max(sy, sz));
        return XM_PI * r * r;
    }
    case 1: { // box = 成分別スケール。表面積 8(hx hy + hy hz + hz hx) の 1/4
        const float hx = col->halfExtents.x * sx;
        const float hy = col->halfExtents.y * sy;
        const float hz = col->halfExtents.z * sz;
        return 2.0f * (hx * hy + hy * hz + hz * hx);
    }
    default: { // capsule = 側面 2 pi r (2 halfSeg) + 両端の球面 4 pi r^2、の 1/4
        const float wr = col->radius * std::max(sx, sz);
        const float wh = col->height * 0.5f * sy;
        const float halfSeg = (wh > wr) ? (wh - wr) : 0.0f;
        return XM_PI * wr * halfSeg + XM_PI * wr * wr;
    }
    }
}

float SubmergedFractionWorld(const ShapePose& pose, float planeY, float& outCentroidY)
{
    outCentroidY = pose.py;
    if (pose.shape == 0) {
        // 球冠。中心を原点に取り t = planeY - 中心Y。t <= -R は完全に水面上、t >= R は完全没水。
        // V(t) = pi(R^2 t - t^3/3 + 2R^3/3)、M(t) = pi(R^2 t^2/2 - t^4/4 - R^4/4)
        // (どちらも多項式 — 球冠の体積・重心に三角関数は要らない)
        const float R = pose.radius;
        if (R <= 0.0f) {
            return 0.0f;
        }
        const float t = planeY - pose.py;
        if (t <= -R) {
            return 0.0f;
        }
        if (t >= R) {
            return 1.0f; // 完全没水。重心は中心のまま
        }
        const float R2 = R * R;
        const float t2 = t * t;
        const float v = R2 * t - t2 * t / 3.0f + 2.0f * R2 * R / 3.0f; // pi は約分で消える
        if (v <= 0.0f) {
            return 0.0f;
        }
        const float m = R2 * t2 * 0.5f - t2 * t2 * 0.25f - R2 * R2 * 0.25f;
        outCentroidY = pose.py + m / v;
        return v / (4.0f * R2 * R / 3.0f); // V_cap / V_sphere
    }
    // box / capsule: 保守 AABB の高さ比近似。傾いた箱の没水重心が水平にずれるのを
    // 拾えない (= 復原モーメントが出ない) のが v1 の既知の限界
    float minX, minY, minZ, maxX, maxY, maxZ;
    shapes::ComputeAabb(pose, minX, minY, minZ, maxX, maxY, maxZ);
    const float h = maxY - minY;
    if (h <= 0.0f || planeY <= minY) {
        return 0.0f;
    }
    const float top = (planeY < maxY) ? planeY : maxY;
    outCentroidY = (minY + top) * 0.5f;
    return (top - minY) / h;
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
