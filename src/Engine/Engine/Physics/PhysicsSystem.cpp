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
// ---- CCD (M59j) ----
// 掃引を起動するしきい値: 1 サブステップの移動量が「外接球半径 × これ」を超えたら掃引する。
// ★このゲートは速度のためだけでなく**正しさのため**にある — 低速でも掃引してしまうと、
//   壁際で静止した CCD ボディが毎サブステップ「触れる手前」で止められ、貫通が生まれず
//   接触が作られず、通常ソルバに引き継げないまま壁の前で浮き続ける。速いあいだだけ
//   CCD が持ち、止まったら離散ソルバへ返す、という受け渡しがこの値で決まっている
constexpr float kCcdMotionRatio = 0.5f;
// 保守的前進の固定反復数 (SphereCastWorld と同じ流儀。ヒット後も回数は消化する = 決定論)
constexpr int kCcdAdvanceSteps = 32;
// 保守的前進の停止距離 [m] (SphereCastWorld と同値)
constexpr float kCcdTouchEps = 1e-4f;
// TOI で止めるときに残す隙間 [m]。0 にすると次サブステップの掃引が距離 0 から始まり、
// 「既に触れている」判定に落ちて掃引が無効化される
constexpr float kCcdSkin = 0.001f;
// ---- 関節 (M60a) ----
// 等式行のクランプ幅 = 実質無限大。∞ を持ち回らないのは、inf 同士の引き算で NaN が
// 生まれる経路をソルバの中に一切作らないため (これを超える λ は物理的に到達しない)
constexpr float kJointRowUnbounded = 1e30f;

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
    // M59h: スリープ。眠っているあいだ invMass / freezeRot を「不動」へ倒して
    // ソルバの全帯から自然に外す (専用の分岐を各帯に撒かないための作り)。
    // 起こすときに戻す値を awake* に控えておく
    bool sleeping = false;
    float awakeInvMass = 0.0f;
    bool awakeFreezeRot = true;
    // M59j: CCD が「このサブステップの移動量」を差し替えたか。★スケール係数ではなく
    // **移動量そのもの**を持つ — CCD は止めると同時に速度へインパルスを入れるので、
    // 位置積分の時点の b.v* は掃引に使った速度と別物になっている
    bool ccdClamped = false;
    float ccdDx = 0, ccdDy = 0, ccdDz = 0;
    float comLx = 0, comLy = 0, comLz = 0;
    float comx = 0, comy = 0, comz = 0;
    float restitution = 0;
    float friction = 0.5f;         // クーロン摩擦係数 (Collider から。ペアは sqrt(μa·μb))
    // M59f2: 静止摩擦と転がり抵抗。材料未割当なら frictionS == friction / roll == 0 で、
    // ソルバの新しい分岐が全て従来側へ畳まれる
    float frictionS = 0.5f;
    float roll = 0.0f;
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

// 3x3 の逆行列 (M60a)。Solve3x3 と同じ余因子を使うが、拘束ブロックは**同じ K で
// 何度も解く**ので逆を 1 回だけ作って持つほうが安い。行列式がほぼ 0 (= 誰も動かせない
// 拘束) なら false を返して呼び側が「ブロックごと捨てる」を選べるようにする
bool Invert3x3(const float a[3][3], float out[3][3])
{
    const float c00 = a[1][1] * a[2][2] - a[1][2] * a[2][1];
    const float c01 = a[1][2] * a[2][0] - a[1][0] * a[2][2];
    const float c02 = a[1][0] * a[2][1] - a[1][1] * a[2][0];
    const float det = a[0][0] * c00 + a[0][1] * c01 + a[0][2] * c02;
    if (det > -1e-12f && det < 1e-12f) {
        return false;
    }
    const float inv = 1.0f / det;
    out[0][0] = c00 * inv;
    out[1][0] = c01 * inv;
    out[2][0] = c02 * inv;
    out[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) * inv;
    out[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) * inv;
    out[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) * inv;
    out[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) * inv;
    out[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) * inv;
    out[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) * inv;
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

// ---- 拘束ブロック (M60a) ----
// 関節を表す**1〜3 自由度の速度拘束**。線形ブロック (共通のアンカー点まわり、方向 d[i]) と
// 角ブロック (軸 d[i]) があり、`angular` で分岐する。
//
// ★接触制約 (ContactConstraint) とは**別物**。共通化しないのは、接触が「マニフォールド
//   4 点 + 中央法線 + 摩擦 + 転がり」という固有の 3 段構成で、M59g1 が測った安定性が
//   その形に依っているため。
// ★**自由度ごとの行を独立に (Gauss-Seidel で) 解いてはいけない**。アンカーが質量中心から
//   離れていると有効質量行列 K が極端に悪条件になる — 半径 0.1 の球を 2m 離れた点で吊ると
//   K の 2x2 小行列の GS 縮小率が 0.967 で、**8 反復しても誤差の 76% が残る**
//   (実測: アンカーが 1 ステップで 37mm ずれた)。ブロックごと K⁻¹ を掛ければ 1 反復で厳密。
//   接触が「1/count 緩和を外したら収束した」(M59g1-1) のと同じ話の、もう一段深いところ。
// ★K⁻¹ は生成時に 1 回だけ作る (M59g1 の「有効質量は生成時に 1 回」と同じ)。
//   count < 3 の余りは単位行列で埋めるので、解く側は常に 3x3 の積で書ける。
// ★clamp (lo/hi) が意味を持つのは **count==1 のときだけ** — 不等式 (リミット) と駆動
//   (モータ) はどちらも 1 自由度なので M60c もこの形に収まる。等式ブロックは
//   ±kJointRowUnbounded のままで、クランプが結果に触れない。
// ★`bias` に**位置誤差を入れない** — 位置補正は速度ソルバから分離した別パスの担当
//   (M59g1-2 の教訓)。bias が入るのはモータの目標速度 (M60c) だけ。
struct ConstraintBlock {
    int32_t ai = -1; // bodies index (-1 = 不動アンカー = ワールド)
    int32_t bi = -1;
    int32_t count = 0;    // 自由度 1..3
    bool angular = false; // true = 角ブロック (ra/rb を使わない)
    float d[3][3] = {};   // 各自由度の方向 / 軸 (単位、互いに直交)
    float ra[3] = {};     // A の質量中心 → アンカー (線形ブロックのみ)
    float rb[3] = {};
    float kinv[3][3] = {}; // 有効質量行列の逆 (余りは単位で埋める)
    float bias[3] = {};
    float lo[3] = { -kJointRowUnbounded, -kJointRowUnbounded, -kJointRowUnbounded };
    float hi[3] = { kJointRowUnbounded, kJointRowUnbounded, kJointRowUnbounded };
    float lambda[3] = {}; // 蓄積 (サブステップ内で閉じる)
};

// 有効質量行列を組んで逆を持たせる。false = 誰も動かせない拘束 (静的同士 / 睡眠中) で、
// 呼び側はブロックごと捨てる
bool FinalizeConstraintBlock(ConstraintBlock& blk, const Body& A, const Body& B)
{
    float K[3][3] = {};
    if (blk.angular) {
        // K_ij = d_i · ((Ia⁻¹ + Ib⁻¹) d_j)
        for (int j = 0; j < blk.count; ++j) {
            float ax, ay, az, bx, by, bz;
            MulInvI(A.invI, blk.d[j][0], blk.d[j][1], blk.d[j][2], ax, ay, az);
            MulInvI(B.invI, blk.d[j][0], blk.d[j][1], blk.d[j][2], bx, by, bz);
            const float sx = ax + bx, sy = ay + by, sz = az + bz;
            for (int i = 0; i < blk.count; ++i) {
                K[i][j] = blk.d[i][0] * sx + blk.d[i][1] * sy + blk.d[i][2] * sz;
            }
        }
    } else {
        // K_ij = (1/ma + 1/mb)(d_i·d_j) + d_i · [(Ia⁻¹(ra×d_j))×ra + (Ib⁻¹(rb×d_j))×rb]
        // (i==j なら EffectiveMassInv と同じ式。非対角がここで初めて出てくる)
        const float invm = A.invMass + B.invMass;
        for (int j = 0; j < blk.count; ++j) {
            float cx, cy, cz, ix, iy, iz;
            float oax, oay, oaz, obx, oby, obz;
            Cross(blk.ra[0], blk.ra[1], blk.ra[2], blk.d[j][0], blk.d[j][1], blk.d[j][2], cx, cy,
                  cz);
            MulInvI(A.invI, cx, cy, cz, ix, iy, iz);
            Cross(ix, iy, iz, blk.ra[0], blk.ra[1], blk.ra[2], oax, oay, oaz);
            Cross(blk.rb[0], blk.rb[1], blk.rb[2], blk.d[j][0], blk.d[j][1], blk.d[j][2], cx, cy,
                  cz);
            MulInvI(B.invI, cx, cy, cz, ix, iy, iz);
            Cross(ix, iy, iz, blk.rb[0], blk.rb[1], blk.rb[2], obx, oby, obz);
            const float sx = oax + obx, sy = oay + oby, sz = oaz + obz;
            for (int i = 0; i < blk.count; ++i) {
                const float dot = blk.d[i][0] * blk.d[j][0] + blk.d[i][1] * blk.d[j][1]
                                + blk.d[i][2] * blk.d[j][2];
                K[i][j] = invm * dot + blk.d[i][0] * sx + blk.d[i][1] * sy + blk.d[i][2] * sz;
            }
        }
    }
    for (int i = blk.count; i < 3; ++i) {
        K[i][i] = 1.0f; // 余りは単位 (K も K⁻¹ もブロック対角になり、余りの自由度が混ざらない)
    }
    return Invert3x3(K, blk.kinv);
}

// 拘束ブロックを 1 つ解く。接触の各段と同じ「Δλ を計算 → 累積してクランプ → 差分を適用」。
// A / B は呼び側が解決済み (不動アンカーは invMass 0 / invI 零行列の番人ボディ)
void SolveConstraintBlock(ConstraintBlock& blk, Body& A, Body& B)
{
    float cdot[3] = { 0.0f, 0.0f, 0.0f };
    if (blk.angular) {
        const float rwx = A.wx - B.wx, rwy = A.wy - B.wy, rwz = A.wz - B.wz;
        for (int i = 0; i < blk.count; ++i) {
            cdot[i] = rwx * blk.d[i][0] + rwy * blk.d[i][1] + rwz * blk.d[i][2];
        }
    } else {
        float wax, way, waz, wbx, wby, wbz;
        Cross(A.wx, A.wy, A.wz, blk.ra[0], blk.ra[1], blk.ra[2], wax, way, waz);
        Cross(B.wx, B.wy, B.wz, blk.rb[0], blk.rb[1], blk.rb[2], wbx, wby, wbz);
        const float rvx = (A.vx + wax) - (B.vx + wbx);
        const float rvy = (A.vy + way) - (B.vy + wby);
        const float rvz = (A.vz + waz) - (B.vz + wbz);
        for (int i = 0; i < blk.count; ++i) {
            cdot[i] = rvx * blk.d[i][0] + rvy * blk.d[i][1] + rvz * blk.d[i][2];
        }
    }
    float rhs[3] = { 0.0f, 0.0f, 0.0f };
    for (int i = 0; i < blk.count; ++i) {
        rhs[i] = -(cdot[i] - blk.bias[i]);
    }
    // 余りは rhs 0 かつ K⁻¹ がブロック対角なので、そのまま 3x3 の積で書ける
    float jx = 0.0f, jy = 0.0f, jz = 0.0f;
    bool any = false;
    for (int i = 0; i < blk.count; ++i) {
        const float dl = blk.kinv[i][0] * rhs[0] + blk.kinv[i][1] * rhs[1] + blk.kinv[i][2] * rhs[2];
        const float old = blk.lambda[i];
        float now = old + dl;
        if (now < blk.lo[i]) {
            now = blk.lo[i];
        } else if (now > blk.hi[i]) {
            now = blk.hi[i];
        }
        const float step = now - old;
        blk.lambda[i] = now;
        if (step != 0.0f) {
            any = true;
            jx += blk.d[i][0] * step;
            jy += blk.d[i][1] * step;
            jz += blk.d[i][2] * step;
        }
    }
    if (!any) {
        return;
    }
    if (blk.angular) {
        // 純粋な角インパルス (並進には効かない)。転がり抵抗 (M59f2) と同じ形
        float ix, iy, iz;
        MulInvI(A.invI, jx, jy, jz, ix, iy, iz);
        A.wx += ix;
        A.wy += iy;
        A.wz += iz;
        MulInvI(B.invI, jx, jy, jz, ix, iy, iz);
        B.wx -= ix;
        B.wy -= iy;
        B.wz -= iz;
    } else {
        ApplyImpulse(A, blk.ra[0], blk.ra[1], blk.ra[2], jx, jy, jz, 1.0f);
        ApplyImpulse(B, blk.rb[0], blk.rb[1], blk.rb[2], jx, jy, jz, -1.0f);
    }
}

// ---- 関節の幾何ヘルパ (M60b) ----

// 単位ベクトル n に直交する 2 方向を決定論的に作る (接触の接線基底と同じ古典手法)
void OrthoBasis(float nx, float ny, float nz, float t1[3], float t2[3])
{
    float ax, ay, az;
    if (std::fabs(nx) >= 0.57735f) { // 1/sqrt(3): 最も長い成分を避けて桁落ちを防ぐ
        ax = ny;
        ay = -nx;
        az = 0.0f;
    } else {
        ax = 0.0f;
        ay = nz;
        az = -ny;
    }
    const float al = std::sqrt(ax * ax + ay * ay + az * az);
    if (al > 1e-8f) {
        t1[0] = ax / al;
        t1[1] = ay / al;
        t1[2] = az / al;
    } else {
        t1[0] = 1.0f;
        t1[1] = 0.0f;
        t1[2] = 0.0f;
    }
    Cross(nx, ny, nz, t1[0], t1[1], t1[2], t2[0], t2[1], t2[2]);
}

// 関節の軸をワールドへ (owner の姿勢で回して正規化)。縮退軸なら false
bool JointAxisWorld(const float q[4], const JointComponent& jc, float out[3])
{
    QuatRotate(q[0], q[1], q[2], q[3], jc.axis.x, jc.axis.y, jc.axis.z, out[0], out[1], out[2]);
    const float l2 = out[0] * out[0] + out[1] * out[1] + out[2] * out[2];
    if (l2 < 1e-12f) {
        return false;
    }
    const float inv = 1.0f / std::sqrt(l2);
    out[0] *= inv;
    out[1] *= inv;
    out[2] *= inv;
    return true;
}

// 相対姿勢の「誤差回転」 qE = qB ⊗ conj(qA ⊗ restRotation) (ワールド左作用) を返す (M60b)。
// 意味は「**B があるべき姿勢からどれだけ余分に回っているか**」。
// ★w の符号を正へ揃えるのは、-q と q が同じ姿勢を表すため — 揃えないと 180 度側へ
//   回そうとして関節が裏返る。**4 成分すべて**を反転する: M60c のツイスト角は
//   (ベクトル部·軸, w) を**半角 sin/cos の 1 組**として読むので、ベクトル部だけ反転すると
//   組が食い違って角度の符号が壊れる (回転ベクトルだけが要る M60b では見えなかった罠)
void JointRelativeQuat(const float qa[4], const float qb[4], const JointComponent& jc,
                       float out[4])
{
    float tx, ty, tz, tw; // qBtarget = qA ⊗ qRest
    QuatMul(qa[0], qa[1], qa[2], qa[3], jc.restRotation.x, jc.restRotation.y, jc.restRotation.z,
            jc.restRotation.w, tx, ty, tz, tw);
    QuatMul(qb[0], qb[1], qb[2], qb[3], -tx, -ty, -tz, tw, out[0], out[1], out[2], out[3]);
    if (out[3] < 0.0f) {
        out[0] = -out[0];
        out[1] = -out[1];
        out[2] = -out[2];
        out[3] = -out[3];
    }
}

// 相対姿勢の誤差をワールドの回転ベクトルで返す (M60b)。
// ★取り出しは **2·(x,y,z)** = 2·sin(θ/2)·軸 の近似で、acos を一度も通さない
//   (決定論の観点でも速度の観点でも有利。位置補正は反復なので過小評価は収束が遅くなるだけ)。
void JointOrientationError(const float qa[4], const float qb[4], const JointComponent& jc,
                           float out[3])
{
    float e[4];
    JointRelativeQuat(qa, qb, jc, e);
    out[0] = 2.0f * e[0];
    out[1] = 2.0f * e[1];
    out[2] = 2.0f * e[2];
}

// 関節角 θ (= **owner が connected に対して軸まわりにどれだけ回っているか**) の
// **半角** sin/cos を返す (M60c)。false = ツイストが縮退 (θ ≈ ±180°) して向きが決まらない。
// ★`acos` も `atan2` も一度も通さない (決定台帳 11)。qE の w を正へ揃えてあるので
//   θ/2 ∈ [-90°, 90°] に収まり、θ の大小比較は **θ/2 の大小比較と同値** (単調)。
//   だからしきい値も半角の sin/cos で持てば、比較は足し算と掛け算だけで書ける。
// ★qE は「B が余分に回っている量」なので、**owner から見た角はその逆** = ベクトル部だけ
//   符号を反転する。ドアを +30° 開いたら関節角も +30°、というオーサリング側の直感に
//   合わせるための符号 — 相手が null のワールド接続では qE がまるごと conj(qOwner) に
//   なるので、ここを間違えるとリミットもモータも軒並み裏返る。
// ★軸成分だけを取り出して単位化するのが swing-twist 分解の twist 側。ヒンジは swing が
//   拘束で消えているので分母はほぼ 1 だが、**コーンでは大きく効く** (swing 込みの
//   半角を twist と読むと、傾けただけでツイストリミットが誤爆する)。
// ★既知の限界: 四元数から取る以上 θ は必ず [-180°, 180°] に折り返る = **回転数を数えない**。
//   リミット行が 1 サブステップで漏らす角は速度×h なので、60Hz で 135° を跨ぐには
//   140 rad/s 級が要る (ドアやラグドールでは届かない)。無限に回すモータへリミットを
//   併用する用途は想定していない
bool JointTwistHalf(const float qe[4], const float ax[3], float& sinHalf, float& cosHalf)
{
    const float s = -(qe[0] * ax[0] + qe[1] * ax[1] + qe[2] * ax[2]);
    const float c = qe[3];
    const float n2 = s * s + c * c;
    if (n2 < 1e-12f) {
        return false;
    }
    const float inv = 1.0f / std::sqrt(n2);
    sinHalf = s * inv;
    cosHalf = c * inv;
    return true;
}

// コーンの「相手側が担いでいる軸」をワールドで返す (M60c)。rest では owner の軸と一致する。
// ★相手ローカルでの表現は conj(restRotation)·axis — 軸フィールドを 2 本持たずに済む理由
//   (M60b の申し送り 1)。swing 角はこの軸と owner 側の軸のなす角そのもの
bool JointConeAxisB(const float qb[4], const JointComponent& jc, float out[3])
{
    float lx, ly, lz;
    QuatRotate(-jc.restRotation.x, -jc.restRotation.y, -jc.restRotation.z, jc.restRotation.w,
               jc.axis.x, jc.axis.y, jc.axis.z, lx, ly, lz);
    QuatRotate(qb[0], qb[1], qb[2], qb[3], lx, ly, lz, out[0], out[1], out[2]);
    const float l2 = out[0] * out[0] + out[1] * out[1] + out[2] * out[2];
    if (l2 < 1e-12f) {
        return false;
    }
    const float inv = 1.0f / std::sqrt(l2);
    out[0] *= inv;
    out[1] *= inv;
    out[2] *= inv;
    return true;
}

// 位置補正パスから姿勢を回す (M60b)。**質量中心まわり**に回すのは位置積分と同じ規約。
// 形状の基底は組み直す — 同じパスの接触判定がこの pose を読むため
void ApplyPoseRotation(Body& b, float ex, float ey, float ez)
{
    float comWx = b.pose.px, comWy = b.pose.py, comWz = b.pose.pz;
    if (b.hasCom) {
        comWx += b.comx;
        comWy += b.comy;
        comWz += b.comz;
    }
    const float hx = ex * 0.5f, hy = ey * 0.5f, hz = ez * 0.5f;
    const float dqw = -(hx * b.qx + hy * b.qy + hz * b.qz);
    const float dqx = hx * b.qw + hy * b.qz - hz * b.qy;
    const float dqy = hy * b.qw + hz * b.qx - hx * b.qz;
    const float dqz = hz * b.qw + hx * b.qy - hy * b.qx;
    b.qx += dqx;
    b.qy += dqy;
    b.qz += dqz;
    b.qw += dqw;
    const float len2 = b.qx * b.qx + b.qy * b.qy + b.qz * b.qz + b.qw * b.qw;
    if (len2 > 1e-12f) {
        const float inv = 1.0f / std::sqrt(len2);
        b.qx *= inv;
        b.qy *= inv;
        b.qz *= inv;
        b.qw *= inv;
    } else {
        b.qx = 0;
        b.qy = 0;
        b.qz = 0;
        b.qw = 1;
    }
    if (b.hasCom) {
        float ox, oy, oz;
        QuatRotate(b.qx, b.qy, b.qz, b.qw, b.comLx, b.comLy, b.comLz, ox, oy, oz);
        b.comx = ox;
        b.comy = oy;
        b.comz = oz;
        b.pose.px = comWx - ox;
        b.pose.py = comWy - oy;
        b.pose.pz = comWz - oz;
    }
    if (b.col) {
        const XMFLOAT3 pos = { b.pose.px, b.pose.py, b.pose.pz };
        const XMFLOAT4 rot = { b.qx, b.qy, b.qz, b.qw };
        b.pose = shapes::MakePose(*b.col, pos, rot, b.scale);
    }
}

// 軸 n まわりに姿勢を回したときの「回しやすさ」 n·I⁻¹·n (位置補正の質量比に使う)
float AngularCorrectionWeight(const Body& b, float nx, float ny, float nz)
{
    float ix, iy, iz;
    MulInvI(b.invI, nx, ny, nz, ix, iy, iz);
    const float k = nx * ix + ny * iy + nz * iz;
    return (k > 0.0f) ? k : 0.0f;
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

// ---- CCD (M59j) ----

// 移動体を掃引するときの**外接球半径**。box / capsule では真の掃引形状より大きいので
// TOI は手前に出る = 「貫通しない」側へ倒れている (保守的で安全な向きの誤差)。
// 手前で止まったぶんは、速度が落ちて起動しきい値を外れた次サブステップ以降に
// 通常の離散ソルバが詰める
float CcdBoundingRadius(const ShapePose& p)
{
    if (p.shape == 0) {
        return p.radius;
    }
    if (p.shape == 1) {
        return std::sqrt(p.hx * p.hx + p.hy * p.hy + p.hz * p.hz);
    }
    if (p.shape == 2) {
        return p.halfSeg + p.radius;
    }
    return 0.0f; // mesh / terrain は動的ボディになれない (収集時に col が外れている)
}

// 外接球を (ox,oy,oz) から方向 (dx,dy,dz) へ maxDist まで掃引し、target に最初に触れる
// 距離を返す。相手が sphere/capsule なら「半径を radius だけ膨らませた形状へのレイ」の
// 解析解、box/mesh/terrain なら DistanceToShape の保守的前進 —
// SphereCastWorld (PhysicsQueries.cpp) と同じ 2 本立て。
// ★SphereCastWorld をそのまま呼べない: あちらは WorldMatrixComponent 基準で、
//   TransformSystem がまだ走っていないこの時点では **1 tick 古い**。おまけにトリガーも
//   拾うしスリープ/kinematic の区別も持たない。掃引の材料はソルバが握っている
//   bodies[] の pose でなければならない
bool CcdSweepTarget(const ShapePose& target, float ox, float oy, float oz, float dx, float dy,
                    float dz, float radius, float maxDist, float& outT, float& onx, float& ony,
                    float& onz, float& opx, float& opy, float& opz)
{
    // ★既に触れている相手は掃引しない。離散ソルバが解いている最中のペアで TOI=0 を
    //   拾うと、壁沿いに高速で滑っているボディがその場に凍りつく
    if (shapes::DistanceToShape(target, ox, oy, oz) - radius <= kCcdTouchEps) {
        return false;
    }
    if (target.shape == 0 || target.shape == 2) {
        ShapePose inflated = target;
        inflated.radius += radius;
        float t, nx, ny, nz;
        if (!shapes::Raycast(inflated, ox, oy, oz, dx, dy, dz, maxDist, t, nx, ny, nz)) {
            return false;
        }
        outT = t;
        onx = nx; ony = ny; onz = nz;
        // 掃引球の中心位置から元形状の表面点を復元 (SphereCastWorld と同じ復元式)
        opx = ox + dx * t - nx * radius;
        opy = oy + dy * t - ny * radius;
        opz = oz + dz * t - nz * radius;
        return true;
    }
    // box / mesh / terrain: 保守的前進 (固定回数、ヒット後も回数は消化する = 決定論)
    float ct = 0.0f;
    bool found = false;
    float foundT = 0.0f;
    for (int step = 0; step < kCcdAdvanceSteps; ++step) {
        if (found || ct > maxDist) {
            continue;
        }
        const float px = ox + dx * ct, py = oy + dy * ct, pz = oz + dz * ct;
        const float d = shapes::DistanceToShape(target, px, py, pz) - radius;
        if (d < kCcdTouchEps) {
            found = true;
            foundT = ct;
        } else {
            ct += d;
        }
    }
    if (!found || foundT > maxDist) {
        return false;
    }
    const float px = ox + dx * foundT, py = oy + dy * foundT, pz = oz + dz * foundT;
    float qx, qy, qz;
    shapes::ClosestPointOnShape(target, px, py, pz, qx, qy, qz);
    const float vx = px - qx, vy = py - qy, vz = pz - qz;
    const float vlen = std::sqrt(vx * vx + vy * vy + vz * vz);
    if (vlen < 1e-6f) {
        return false; // 縮退 (中心が形状内)。上の「既に触れている」判定で普通は届かない
    }
    outT = foundT;
    onx = vx / vlen; ony = vy / vlen; onz = vz / vlen;
    opx = qx; opy = qy; opz = qz;
    return true;
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
            // M59h: 眠っているボディは**不動として収集する**。ブロードフェーズには残る
            // (残さないと起きているボディがすり抜ける) が、重力・積分・書き戻しは
            // invMass==0 の既存分岐でそのまま外れる
            b.awakeInvMass = b.invMass;
            b.awakeFreezeRot = b.freezeRot;
            if (rb->isSleeping && !rb->isKinematic) {
                b.sleeping = true;
                b.invMass = 0.0f;
                b.freezeRot = true;
            }
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
                b.frictionS = SelectStaticFriction(*col, mat);   // M59f2
                b.roll = SelectRollingResistance(*col, mat);     // M59f2
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
            b.frictionS = SelectStaticFriction(*col, mat); // M59f2
            b.roll = SelectRollingResistance(*col, mat);   // M59f2
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

    // ---- CCD の有無 (M59j) ----
    // 1 個も居なければ掃引パスごと通らない = 既存シーンは fp 演算が 1 回も増えない。
    // Rigidbody.ccd は tick 中に変わらないので、サブステップの外で 1 回だけ見ればよい
    bool anyCcd = false;
    for (const Body& b : bodies) {
        if (b.rb && b.rb->ccd && b.solid) {
            anyCcd = true;
            break;
        }
    }

    // ---- 関節の収集 (M60a) ----
    // **1 tick に 1 回だけ**。「誰と誰を繋ぐか」はサブステップで変わらないし、島と起床の
    // 配線にも同じ表を使い回す。行そのものは姿勢が動くのでサブステップごとに作り直す。
    // 1 個も無ければ以降の関節帯は全て空ループ = 既存シーンは fp 演算が 1 回も増えない
    struct JointLink {
        EntityID owner;
        const JointComponent* jc = nullptr;
        int32_t ai = -1; // owner の bodies index (-1 = 不動アンカー)
        int32_t bi = -1; // 相手の bodies index (-1 = 不動アンカー)
        // bodies に居ない側の固定ワールドアンカー (相手 null / 変換だけのエンティティ)
        float wax = 0, way = 0, waz = 0;
        float wbx = 0, wby = 0, wbz = 0;
        // 同じく固定側のワールド回転 (M60b の軸と相対姿勢の基準に要る)。
        // body 側は Body の作業用姿勢が正なのでこちらは使わない
        float waq[4] = { 0, 0, 0, 1 };
        float wbq[4] = { 0, 0, 0, 1 };
        // ---- リミットのしきい値 (M60c) ----
        // ★**tick 頭に 1 回だけ**作る (決定台帳 11)。角度は半角の sin/cos、swing は
        //   cos/sin をそのまま持つので、サブステップの中では三角関数を 1 回も呼ばない。
        // ★limitOn は「useLimit **かつ** 範囲が正順」。逆転した範囲 (min > max) は
        //   満たしようが無いので**行を立てない** = 自由にする — 縮退軸のヒンジを Ball へ
        //   落とすのと同じ「オーサリングミスで物体が飛ばない」側の選択
        bool limitOn = false;
        float sinHalfLo = 0.0f, cosHalfLo = 1.0f; // limitMin/2 (Hinge / Cone twist)
        float sinHalfHi = 0.0f, cosHalfHi = 1.0f; // limitMax/2
        float sinSwing = 0.0f, cosSwing = 1.0f;   // swingLimitDeg (Cone のみ)
    };
    std::vector<JointLink> jointLinks;
    // 不動アンカー用の番人ボディ。invMass 0 / invI 零行列なので、行の適用も有効質量も
    // 「何も起きない」へ自然に落ちる (-1 の分岐をソルバの各段に撒かないための作り)
    Body worldAnchorBody;
    {
        const ComponentTypeId jReq[] = { JointComponent::sTypeId, LocalTransform::sTypeId };
        world.ForEachArchetype(jReq, [&](Archetype& arch) {
            const int ji = arch.FindTypeIndex(JointComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                const EntityID e = arch.EntityAt(row);
                if (!IsEntityActive(world, e)) {
                    continue;
                }
                const auto* jc = static_cast<const JointComponent*>(arch.GetPtr(ji, row));
                if (jc->broken) {
                    continue; // 折れた関節は行を立てない (M60d が立てるフラグ、読みは a から)
                }
                JointLink l;
                l.owner = e;
                l.jc = jc;
                jointLinks.push_back(l);
            }
        });
    }
    if (!jointLinks.empty()) {
        // 決定論の順序: owner の entity.index 昇順 (SpringJoint と同じ流儀)
        std::sort(jointLinks.begin(), jointLinks.end(),
                  [](const JointLink& a, const JointLink& b) { return a.owner.index < b.owner.index; });
        // bodies は index 昇順ソート済 → 二分探索 (generation も一致確認)
        auto findBodyIndex = [&bodies](EntityID e) -> int32_t {
            auto it = std::lower_bound(bodies.begin(), bodies.end(), e.index,
                                       [](const Body& b, uint32_t idx) { return b.entity.index < idx; });
            if (it != bodies.end() && it->entity.index == e.index
                && it->entity.generation == e.generation) {
                return static_cast<int32_t>(it - bodies.begin());
            }
            return -1;
        };
        // bodies に居ないエンティティ (剛体もコライダーも無い) は変換から不動アンカーを作る。
        // 動かないので tick 頭に 1 回計算すれば足りる
        auto fixedAnchor = [&world](EntityID e, const XMFLOAT3& local, float& ox, float& oy,
                                    float& oz, float q[4]) -> bool {
            const auto* alt = world.GetComponent<LocalTransform>(e);
            if (!alt) {
                return false;
            }
            const WorldFrame f = ComposeParentFrame(world, e);
            XMFLOAT3 wpos;
            XMFLOAT4 wrot;
            XMFLOAT3 wscale;
            ApplyFrame(f, *alt, wpos, wrot, wscale);
            float rx, ry, rz;
            QuatRotate(wrot.x, wrot.y, wrot.z, wrot.w, local.x * wscale.x, local.y * wscale.y,
                       local.z * wscale.z, rx, ry, rz);
            ox = wpos.x + rx;
            oy = wpos.y + ry;
            oz = wpos.z + rz;
            q[0] = wrot.x;
            q[1] = wrot.y;
            q[2] = wrot.z;
            q[3] = wrot.w;
            return true;
        };
        size_t w = 0;
        for (JointLink& l : jointLinks) {
            l.ai = findBodyIndex(l.owner);
            if (l.ai < 0 && !fixedAnchor(l.owner, l.jc->anchor, l.wax, l.way, l.waz, l.waq)) {
                continue; // owner の位置すら決まらない = 何もできない
            }
            const EntityID other = l.jc->connectedEntity;
            if (other.IsNull()) {
                // ★相手が null のときだけ connectedAnchor は**ワールド座標**
                l.wbx = l.jc->connectedAnchor.x;
                l.wby = l.jc->connectedAnchor.y;
                l.wbz = l.jc->connectedAnchor.z;
            } else if (other.index == l.owner.index && other.generation == l.owner.generation) {
                continue; // 自分自身への関節は成立しない
            } else if (!world.IsAlive(other) || !IsEntityActive(world, other)) {
                continue;
            } else {
                l.bi = findBodyIndex(other);
                if (l.bi < 0
                    && !fixedAnchor(other, l.jc->connectedAnchor, l.wbx, l.wby, l.wbz, l.wbq)) {
                    continue;
                }
            }
            if (l.ai < 0 && l.bi < 0) {
                continue; // 両側とも不動アンカー = 拘束する相手が居ない
            }
            // リミットのしきい値を先に落としておく (M60c)。**存在ゲート**なので
            // useLimit が false のシーンでは 1 回も三角関数を通らない
            if (l.jc->useLimit && l.jc->limitMin <= l.jc->limitMax) {
                l.limitOn = true;
                constexpr float kDeg2Rad = 3.14159265f / 180.0f;
                const int32_t ty = l.jc->type;
                if (ty == 1 || ty == 4) { // 角度リミット (度) → 半角の sin/cos
                    const float hlo = l.jc->limitMin * kDeg2Rad * 0.5f;
                    const float hhi = l.jc->limitMax * kDeg2Rad * 0.5f;
                    l.sinHalfLo = std::sin(hlo);
                    l.cosHalfLo = std::cos(hlo);
                    l.sinHalfHi = std::sin(hhi);
                    l.cosHalfHi = std::cos(hhi);
                }
                if (ty == 4) { // 円錐半頂角はそのまま cos/sin のしきい値で持つ
                    const float sw = l.jc->swingLimitDeg * kDeg2Rad;
                    l.sinSwing = std::sin(sw);
                    l.cosSwing = std::cos(sw);
                }
                // Slider (ty == 3) の limitMin/Max は**メートル**なので変換しない
            }
            jointLinks[w++] = l;
        }
        jointLinks.resize(w);
    }
    // 関節のアンカー 2 点をワールドで取り直す (姿勢が動くので毎回計算する)
    auto jointAnchorsWorld = [&bodies](const JointLink& l, float& pax, float& pay, float& paz,
                                       float& pbx, float& pby, float& pbz) {
        if (l.ai >= 0) {
            const Body& A = bodies[static_cast<size_t>(l.ai)];
            float ox, oy, oz;
            QuatRotate(A.qx, A.qy, A.qz, A.qw, l.jc->anchor.x * A.scale.x,
                       l.jc->anchor.y * A.scale.y, l.jc->anchor.z * A.scale.z, ox, oy, oz);
            pax = A.pose.px + ox;
            pay = A.pose.py + oy;
            paz = A.pose.pz + oz;
        } else {
            pax = l.wax;
            pay = l.way;
            paz = l.waz;
        }
        if (l.bi >= 0) {
            const Body& B = bodies[static_cast<size_t>(l.bi)];
            float ox, oy, oz;
            QuatRotate(B.qx, B.qy, B.qz, B.qw, l.jc->connectedAnchor.x * B.scale.x,
                       l.jc->connectedAnchor.y * B.scale.y, l.jc->connectedAnchor.z * B.scale.z,
                       ox, oy, oz);
            pbx = B.pose.px + ox;
            pby = B.pose.py + oy;
            pbz = B.pose.pz + oz;
        } else {
            pbx = l.wbx;
            pby = l.wby;
            pbz = l.wbz;
        }
    };
    // 関節の両側のワールド姿勢 (M60b)。body なら作業用姿勢、そうでなければ収集時の固定値
    auto jointQuats = [&bodies](const JointLink& l, float qa[4], float qb[4]) {
        if (l.ai >= 0) {
            const Body& A = bodies[static_cast<size_t>(l.ai)];
            qa[0] = A.qx;
            qa[1] = A.qy;
            qa[2] = A.qz;
            qa[3] = A.qw;
        } else {
            qa[0] = l.waq[0];
            qa[1] = l.waq[1];
            qa[2] = l.waq[2];
            qa[3] = l.waq[3];
        }
        if (l.bi >= 0) {
            const Body& B = bodies[static_cast<size_t>(l.bi)];
            qb[0] = B.qx;
            qb[1] = B.qy;
            qb[2] = B.qz;
            qb[3] = B.qw;
        } else {
            qb[0] = l.wbq[0];
            qb[1] = l.wbq[1];
            qb[2] = l.wbq[2];
            qb[3] = l.wbq[3];
        }
    };
    // 位置補正パスで姿勢のずれを戻す (M60b。M60c のリミットも同じ口を使う)。
    // 引数 e は「**B が e だけ余分に回っている**」というワールドの回転ベクトルで、
    // A を +e 側 / B を -e 側へ寄せると 0 になる。分配の重みは n·I⁻¹·n (方向ごとの回しやすさ)
    auto applyJointAngular = [&bodies](const JointLink& l, float ex, float ey, float ez) {
        const float l2 = ex * ex + ey * ey + ez * ez;
        if (l2 <= 1e-14f) {
            return;
        }
        const float len = std::sqrt(l2);
        const float nx = ex / len, ny = ey / len, nz = ez / len;
        const float kA = (l.ai >= 0)
                             ? AngularCorrectionWeight(bodies[static_cast<size_t>(l.ai)], nx, ny, nz)
                             : 0.0f;
        const float kB = (l.bi >= 0)
                             ? AngularCorrectionWeight(bodies[static_cast<size_t>(l.bi)], nx, ny, nz)
                             : 0.0f;
        const float ksum = kA + kB;
        if (ksum <= 0.0f) {
            return;
        }
        if (kA > 0.0f) {
            const float s = kA / ksum;
            ApplyPoseRotation(bodies[static_cast<size_t>(l.ai)], ex * s, ey * s, ez * s);
        }
        if (kB > 0.0f) {
            const float s = kB / ksum;
            ApplyPoseRotation(bodies[static_cast<size_t>(l.bi)], -ex * s, -ey * s, -ez * s);
        }
    };
    std::vector<ConstraintBlock> jointBlocks; // サブステップごとに作り直す (capacity は使い回す)

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
    // M59h: 島 (union-find) の材料。最後のサブステップの候補ペアを使う — サブステップ
    // ごとに作り直されるが、入眠判定は tick 末の速度で行うので最後のものが正しい
    std::vector<uint64_t> islandPairs;
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

        // ---- 起床 (M59h): 覚醒中のボディに触られたら起きる ----
        // 候補ペア (小,大) 昇順の **1 パス**。A→B→C と伝播する連鎖は 1 パスでは届かない
        // ことがあるが、届かなかったぶんは次 tick へ持ち越される (決定論的で、
        // 1/60 秒の遅れは許容する)。収束まで多パス回すと、走査コストがシーンの形に
        // 依存して読めなくなる
        auto wakeBody = [&](Body& b) {
            if (!b.sleeping) {
                return;
            }
            b.sleeping = false;
            b.rb->isSleeping = false;
            b.rb->sleepTicks = 0;
            b.invMass = b.awakeInvMass;
            b.freezeRot = b.awakeFreezeRot;
            // 眠っていたあいだ pose 確定で飛ばされていた慣性を組み直す
            if (!b.freezeRot && b.invMass > 0.0f) {
                float ix, iy, iz;
                LocalInertiaDiag(b.col, b.pose, 1.0f / b.invMass, ix, iy, iz);
                InvInertiaWorld(b.pose, ix, iy, iz, b.invI);
                b.Ilx = ix;
                b.Ily = iy;
                b.Ilz = iz;
            }
        };
        for (const uint64_t pairKey : candidates) {
            Body& A = bodies[static_cast<size_t>(pairKey >> 32)];
            Body& B = bodies[static_cast<size_t>(pairKey & 0xFFFFFFFFu)];
            // 相手が「動いている剛体」のときだけ起こす — 静的な床の隣で寝ているだけの
            // ボディが永久に起き続けるのを防ぐ (invMass>0 は覚醒中の動的剛体だけが満たす)
            if (A.sleeping && B.invMass > 0.0f) {
                wakeBody(A);
            } else if (B.sleeping && A.invMass > 0.0f) {
                wakeBody(B);
            }
        }

        // ---- 起床 (M60a): 関節で繋がった相手も起こす ----
        // ★M59h の起床は**接触候補ペアの走査**でしか伝播しない。関節は触れていなくても
        //   運動を伝えるので、ここへ明示的に足さないと「繋がった片方だけが眠る」。
        //   眠った側は invMass 0 の不動として扱われるので、振り子なら支点が消えて落ち、
        //   ロープなら途中の 1 節だけが空中に固定される
        for (const JointLink& l : jointLinks) {
            if (l.ai < 0 || l.bi < 0) {
                continue; // 不動アンカー側には起こす相手が居ない
            }
            Body& A = bodies[static_cast<size_t>(l.ai)];
            Body& B = bodies[static_cast<size_t>(l.bi)];
            if (A.sleeping && B.invMass > 0.0f) {
                wakeBody(A);
            } else if (B.sleeping && A.invMass > 0.0f) {
                wakeBody(B);
            }
        }

        // ---- 関節の拘束ブロックを組む (M60a) ----
        // **サブステップごとに作り直す** — 姿勢が変わればアンカーの腕も有効質量も変わる。
        // λ の蓄積はサブステップ内で閉じる (接触の蓄積と同じ寿命。warm starting は M59h と
        // 同じ理由で入れない)。順序 = jointLinks の順 (owner の entity.index 昇順) →
        // 同一関節内はブロック番号昇順、で決定論的に固定する
        // ★type は**どのブロックを立てるかのプリセット**であって別実装ではない (決定台帳 1):
        //     0 Ball   線形 3 (アンカー一致)            / 角 0
        //     1 Hinge  線形 3                           / 角 2 (軸に直交する 2 方向)
        //     2 Fixed  線形 3                           / 角 3 (相対姿勢を丸ごと固定)
        //     3 Slider 線形 2 (軸に直交)                / 角 3
        //     4 Cone   線形 3                           / 角 0 (M60c が swing/twist を足す)
        jointBlocks.clear();
        for (const JointLink& l : jointLinks) {
            const int32_t type = l.jc->type;
            const Body& A = (l.ai >= 0) ? bodies[static_cast<size_t>(l.ai)] : worldAnchorBody;
            const Body& B = (l.bi >= 0) ? bodies[static_cast<size_t>(l.bi)] : worldAnchorBody;
            float qa[4], qb[4];
            jointQuats(l, qa, qb);
            // 軸を使う型 (Hinge / Slider) は縮退軸なら**その型の拘束を諦めて Ball へ落とす**
            // — 何も立てないと関節が黙って消えるので、線形だけは必ず残すほうが親切
            float ax[3] = { 0.0f, 1.0f, 0.0f };
            const bool hasAxis = JointAxisWorld(qa, *l.jc, ax);
            float t1[3], t2[3];
            OrthoBasis(ax[0], ax[1], ax[2], t1, t2);

            // アンカーは線形ブロックとスライダのリミット / モータで共用する (M60c で外へ出した)
            float pax, pay, paz, pbx, pby, pbz;
            jointAnchorsWorld(l, pax, pay, paz, pbx, pby, pbz);

            // ---- 線形ブロック ----
            {
                ConstraintBlock blk;
                blk.ai = l.ai;
                blk.bi = l.bi;
                // 腕は**質量中心から** (M59f1)。com* は hasCom が false なら +0.0f 固定なので
                // 「x - (+0.0f) == x」でビットを崩さない
                blk.ra[0] = pax - A.pose.px - A.comx;
                blk.ra[1] = pay - A.pose.py - A.comy;
                blk.ra[2] = paz - A.pose.pz - A.comz;
                blk.rb[0] = pbx - B.pose.px - B.comx;
                blk.rb[1] = pby - B.pose.py - B.comy;
                blk.rb[2] = pbz - B.pose.pz - B.comz;
                blk.angular = false;
                if (type == 3 && hasAxis) {
                    // Slider は軸方向に自由 → 軸に直交する 2 自由度だけ拘束する
                    blk.count = 2;
                    for (int k = 0; k < 3; ++k) {
                        blk.d[0][k] = t1[k];
                        blk.d[1][k] = t2[k];
                    }
                } else {
                    // アンカー 2 点を一致させる線形 3 自由度。方向はワールド軸に取る —
                    // **ブロックごと K⁻¹ で解くので基底の取り方は結果に効かない**
                    // (行を独立に解いていた頃はここが悪条件の元凶だった)
                    blk.count = 3;
                    blk.d[0][0] = 1.0f;
                    blk.d[1][1] = 1.0f;
                    blk.d[2][2] = 1.0f;
                }
                if (FinalizeConstraintBlock(blk, A, B)) {
                    jointBlocks.push_back(blk); // 失敗 = 誰も動かせない (静的同士 / 睡眠中)
                }
            }

            // ---- 角ブロック (M60b) ----
            // ★相対**角速度**だけを拘束する。姿勢のずれ (積分誤差で必ず溜まる) は
            //   位置補正パスの担当 — 線形と同じ役割分担にしてある
            if (type == 1 || type == 2 || type == 3) {
                ConstraintBlock blk;
                blk.ai = l.ai;
                blk.bi = l.bi;
                blk.angular = true;
                if (type == 1) {
                    if (!hasAxis) {
                        // 軸が縮退したヒンジ = 線形だけ (= Ball) で通す。
                        // ★この continue は**関節 1 個ぶんを飛ばす** — 下のモータ / リミットも
                        //   落ちるが、どちらも hasAxis を要求するので結果は同じ。
                        //   軸を要らない行をこの後ろに足すときはここを分岐へ直すこと
                        continue;
                    }
                    blk.count = 2; // 軸まわりの回転だけを許す
                    for (int k = 0; k < 3; ++k) {
                        blk.d[0][k] = t1[k];
                        blk.d[1][k] = t2[k];
                    }
                } else {
                    blk.count = 3; // 相対姿勢を丸ごと固定 (Fixed / Slider)
                    blk.d[0][0] = 1.0f;
                    blk.d[1][1] = 1.0f;
                    blk.d[2][2] = 1.0f;
                }
                if (FinalizeConstraintBlock(blk, A, B)) {
                    jointBlocks.push_back(blk);
                }
            }

            // ---- モータ行 (M60c): 目標速度を bias に持つ**両側**行 ----
            // ★**bias が入る唯一の行がモータ**。等式行もリミット行も bias 0 のまま
            //   (位置誤差を速度行へ入れないという M59g1-2 の分離は崩さない)。
            // ★クランプは |λ| ≤ maxForce·h。λ は力ではなく**力積**なので、刻みを掛けて
            //   はじめて「最大 N (角なら N·m)」の意味になる。h はサブステップの刻みで、
            //   substeps を増やすと 1 ステップあたりの上限も比例して下がる = 正しい。
            // ★`motorMaxForce <= 0` は**分岐ゲート** (値ゲートで 0 を掛けると -0.0f が
            //   +0.0f へ化けてハッシュが動く。M59f1-4)
            if ((type == 1 || type == 3) && hasAxis && l.jc->motorMaxForce > 0.0f) {
                ConstraintBlock blk;
                blk.ai = l.ai;
                blk.bi = l.bi;
                blk.count = 1;
                blk.angular = (type == 1);
                for (int k = 0; k < 3; ++k) {
                    blk.d[0][k] = ax[k];
                }
                if (!blk.angular) {
                    blk.ra[0] = pax - A.pose.px - A.comx;
                    blk.ra[1] = pay - A.pose.py - A.comy;
                    blk.ra[2] = paz - A.pose.pz - A.comz;
                    blk.rb[0] = pbx - B.pose.px - B.comx;
                    blk.rb[1] = pby - B.pose.py - B.comy;
                    blk.rb[2] = pbz - B.pose.pz - B.comz;
                }
                // d = +軸 なので cdot = (ωA-ωB)·軸 = **関節角の角速度そのもの** (線形なら
                // アンカーの軸方向相対速度)。ソルバは cdot を bias へ寄せるので、
                // 目標速度をそのまま入れれば「+ なら owner が +軸まわりに回る」になる
                blk.bias[0] = l.jc->motorTargetVelocity;
                const float cap = l.jc->motorMaxForce * h;
                blk.lo[0] = -cap;
                blk.hi[0] = cap;
                if (FinalizeConstraintBlock(blk, A, B)) {
                    jointBlocks.push_back(blk);
                }
            }

            // ---- リミット行 (M60c): **片側不等式** (λ を [0, ∞) にクランプ) ----
            // ★**範囲外に出たときだけ行を立てる** = 分岐ゲート。常に立てて lo/hi を動かす
            //   値ゲートにしないのは、範囲内でも K⁻¹ の計算と λ の往復が走って
            //   「リミットを使っていない関節」のビットに触れてしまうから。
            // ★向きの規約: **違反している向きの速度が cdot < 0 になるように d を取る**。
            //   すると λ∈[0,∞) のクランプがそのまま「押し戻す方向にだけ効く」になり、
            //   等式ブロックと同じ SolveConstraintBlock で解ける (不等式用のソルバを書かない)。
            // ★モータ**より後**に積む: Gauss-Seidel は後の行が最後の一声を持つので、
            //   モータがリミットへ突っ込んでも各反復の最後にリミットが勝つ。
            //   逆順だとモータの目標速度がそのまま残って可動域を突き抜ける。
            if (l.limitOn && hasAxis) {
                // -- 角度リミット (Hinge の回転角 / Cone のツイスト角) --
                if (type == 1 || type == 4) {
                    float qe[4];
                    JointRelativeQuat(qa, qb, *l.jc, qe);
                    float sh, ch;
                    if (JointTwistHalf(qe, ax, sh, ch)) {
                        // sin(θ/2 − hi/2) > 0 ⇔ θ > hi (どちらも [-90°,90°] なので単調)
                        const float over = sh * l.cosHalfHi - ch * l.sinHalfHi;
                        const float under = l.sinHalfLo * ch - l.cosHalfLo * sh;
                        float sgn = 0.0f;
                        if (over > 0.0f) {
                            sgn = -1.0f; // θ を増やす速度 (cdot>0) を止めたい → d = -軸
                        } else if (under > 0.0f) {
                            sgn = 1.0f;
                        }
                        if (sgn != 0.0f) {
                            ConstraintBlock blk;
                            blk.ai = l.ai;
                            blk.bi = l.bi;
                            blk.count = 1;
                            blk.angular = true;
                            for (int k = 0; k < 3; ++k) {
                                blk.d[0][k] = sgn * ax[k];
                            }
                            blk.lo[0] = 0.0f; // 押し戻す向きにだけ効く
                            blk.hi[0] = kJointRowUnbounded;
                            if (FinalizeConstraintBlock(blk, A, B)) {
                                jointBlocks.push_back(blk);
                            }
                        }
                    }
                }
                // -- コーンのスイング角 (軸そのものが円錐から出たら止める) --
                if (type == 4) {
                    float axB[3];
                    if (JointConeAxisB(qb, *l.jc, axB)) {
                        const float dot = ax[0] * axB[0] + ax[1] * axB[1] + ax[2] * axB[2];
                        if (dot < l.cosSwing) { // cos は単調減少なので「角が大きい」
                            float nx, ny, nz;
                            Cross(ax[0], ax[1], ax[2], axB[0], axB[1], axB[2], nx, ny, nz);
                            const float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
                            // 真裏 (180°) は回す向きが決められない — 行を立てない。
                            // swingLimitDeg の上限を 179 に切ってあるので通常は届かない
                            if (nl > 1e-6f) {
                                ConstraintBlock blk;
                                blk.ai = l.ai;
                                blk.bi = l.bi;
                                blk.count = 1;
                                blk.angular = true;
                                // B は n まわりに swing 角だけ余分に回っている →
                                // d = n なら「swing を増やす速度」が cdot < 0 になる
                                blk.d[0][0] = nx / nl;
                                blk.d[0][1] = ny / nl;
                                blk.d[0][2] = nz / nl;
                                blk.lo[0] = 0.0f;
                                blk.hi[0] = kJointRowUnbounded;
                                if (FinalizeConstraintBlock(blk, A, B)) {
                                    jointBlocks.push_back(blk);
                                }
                            }
                        }
                    }
                }
                // -- スライダの変位リミット (軸方向の並進) --
                // u = (アンカーA − アンカーB)·軸 = owner が +軸側へ滑った量
                if (type == 3) {
                    const float u = (pax - pbx) * ax[0] + (pay - pby) * ax[1]
                                  + (paz - pbz) * ax[2];
                    float sgn = 0.0f;
                    if (u > l.jc->limitMax) {
                        sgn = -1.0f;
                    } else if (u < l.jc->limitMin) {
                        sgn = 1.0f;
                    }
                    if (sgn != 0.0f) {
                        ConstraintBlock blk;
                        blk.ai = l.ai;
                        blk.bi = l.bi;
                        blk.count = 1;
                        blk.angular = false;
                        for (int k = 0; k < 3; ++k) {
                            blk.d[0][k] = sgn * ax[k];
                        }
                        blk.ra[0] = pax - A.pose.px - A.comx;
                        blk.ra[1] = pay - A.pose.py - A.comy;
                        blk.ra[2] = paz - A.pose.pz - A.comz;
                        blk.rb[0] = pbx - B.pose.px - B.comx;
                        blk.rb[1] = pby - B.pose.py - B.comy;
                        blk.rb[2] = pbz - B.pose.pz - B.comz;
                        blk.lo[0] = 0.0f;
                        blk.hi[0] = kJointRowUnbounded;
                        if (FinalizeConstraintBlock(blk, A, B)) {
                            jointBlocks.push_back(blk);
                        }
                    }
                }
            }
        }

        // 眠っているペアの接触 (M59h)。key 昇順で溜め、出力時に本線へマージする
        std::vector<SolidContact> sleepContacts;

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
            // M59f2: 静止摩擦の上限と転がり抵抗。muS == mu / muRoll == 0 なら従来経路
            float muS = 0.0f;
            float muRoll = 0.0f;
            float rollAxis[3] = { 0, 0, 0 }; // 生成時の相対角速度の向き (蓄積のため固定)
            float rollRadius = 0.0f;
            float massRoll = 0.0f;
            float lambdaRoll = 0.0f;
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
                // ★眠っているペアは「解かないが**報告はする**」(M59h)。ここで落とすと
                //   接触ペア列から消えて CollisionSystem が OnCollisionExit を誤発火し、
                //   起きた瞬間に Enter が再発火する。計画は CollisionSystem 側に
                //   「両者睡眠なら前 tick から Stay を繰り越す」規則を足す案だったが、
                //   こちらのほうが (a) 生存確認が要らない (b) 毎 tick 実際に重なりを
                //   確かめるので嘘をつかない。コストは最後のサブステップの 1 回だけ
                if ((A.sleeping || B.sleeping) && outContacts && sub == substeps - 1) {
                    shapes::Manifold sm;
                    if (shapes::CollideManifold(A.pose, B.pose, sm) && sm.count > 0) {
                        SolidContact sc;
                        sc.key = (static_cast<uint64_t>(A.entity.index) << 32) | B.entity.index;
                        sc.nx = sm.nx;
                        sc.ny = sm.ny;
                        sc.nz = sm.nz;
                        float cx = 0.0f, cy = 0.0f, cz = 0.0f;
                        for (int k = 0; k < sm.count; ++k) {
                            cx += sm.pts[k].px;
                            cy += sm.pts[k].py;
                            cz += sm.pts[k].pz;
                        }
                        const float inv = 1.0f / static_cast<float>(sm.count);
                        sc.px = cx * inv;
                        sc.py = cy * inv;
                        sc.pz = cz * inv;
                        sc.impulse = 0.0f; // 眠っている = この tick に交換した力積は無い
                        sleepContacts.push_back(sc);
                    }
                }
                continue; // 両方不動 (静的 / kinematic 同士 / 睡眠)
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
            // M59f2: μs も同じ結合則。材料未割当なら A.frictionS==A.friction なので
            // muS と mu がビットまで一致し、下の静止/動の分岐が消える
            c.muS = std::sqrt(A.frictionS * B.frictionS);
            // ★転がり抵抗だけ **max** で結合する (摩擦の sqrt(積) ではない)。
            //   転がり抵抗はどちらか一方の材料のヒステリシスで生まれるので、
            //   「素の床に置いたゴム球が転がり続ける」ほうが物理として間違い。
            //   sqrt(積) だと相手が 0 の瞬間に消えてしまう
            c.muRoll = (A.roll > B.roll) ? A.roll : B.roll;
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
            // ---- 転がり抵抗の軸と有効慣性 (M59f2) ----
            // ★軸は**生成時の相対角速度の向きで固定**する。摩擦の接線を法線から作るのと
            //   同じ理由 — 蓄積インパルスは回る軸を追いかけられない。
            //   転がっていない (ω_rel ≈ 0) 接触は軸が定義できないので無効化する
            if (c.muRoll > 0.0f) {
                const float wrx = A.wx - B.wx;
                const float wry = A.wy - B.wy;
                const float wrz = A.wz - B.wz;
                const float wl2 = wrx * wrx + wry * wry + wrz * wrz;
                if (wl2 > 1e-12f) {
                    const float winv = 1.0f / std::sqrt(wl2);
                    c.rollAxis[0] = wrx * winv;
                    c.rollAxis[1] = wry * winv;
                    c.rollAxis[2] = wrz * winv;
                    float iax, iay, iaz, ibx, iby, ibz;
                    MulInvI(A.invI, c.rollAxis[0], c.rollAxis[1], c.rollAxis[2], iax, iay, iaz);
                    MulInvI(B.invI, c.rollAxis[0], c.rollAxis[1], c.rollAxis[2], ibx, iby, ibz);
                    const float kr = c.rollAxis[0] * (iax + ibx) + c.rollAxis[1] * (iay + iby)
                                   + c.rollAxis[2] * (iaz + ibz);
                    c.massRoll = (kr > 0.0f) ? 1.0f / kr : 0.0f;
                    // 転がり半径 = 接触点までの腕の長い側。Crr N R が転がり抵抗トルクの
                    // 教科書的な形なので、無次元の Crr を長さに変換するのに要る
                    const float ra = std::sqrt(c.raC[0] * c.raC[0] + c.raC[1] * c.raC[1]
                                               + c.raC[2] * c.raC[2]);
                    const float rbl = std::sqrt(c.rbC[0] * c.rbC[0] + c.rbC[1] * c.rbC[1]
                                                + c.rbC[2] * c.rbC[2]);
                    c.rollRadius = (ra > rbl) ? ra : rbl;
                } else {
                    c.muRoll = 0.0f; // 回っていない = 転がり抵抗は定義できない
                }
            }
            // ---- 転がり抵抗の軸と有効慣性 (M59f2) ----
            // ★軸は**生成時の相対角速度の向きで固定**する。摩擦の接線を法線から作るのと
            //   同じ理由 — 蓄積インパルスは回る軸を追いかけられない。
            //   転がっていない (ω_rel ≈ 0) 接触は軸が定義できないので無効化する
            if (c.muRoll > 0.0f) {
                const float wrx = A.wx - B.wx;
                const float wry = A.wy - B.wy;
                const float wrz = A.wz - B.wz;
                const float wl2 = wrx * wrx + wry * wry + wrz * wrz;
                if (wl2 > 1e-12f) {
                    const float winv = 1.0f / std::sqrt(wl2);
                    c.rollAxis[0] = wrx * winv;
                    c.rollAxis[1] = wry * winv;
                    c.rollAxis[2] = wrz * winv;
                    float iax, iay, iaz, ibx, iby, ibz;
                    MulInvI(A.invI, c.rollAxis[0], c.rollAxis[1], c.rollAxis[2], iax, iay, iaz);
                    MulInvI(B.invI, c.rollAxis[0], c.rollAxis[1], c.rollAxis[2], ibx, iby, ibz);
                    const float kr = c.rollAxis[0] * (iax + ibx) + c.rollAxis[1] * (iay + iby)
                                   + c.rollAxis[2] * (iaz + ibz);
                    c.massRoll = (kr > 0.0f) ? 1.0f / kr : 0.0f;
                    // 転がり半径 = 接触点までの腕の長い側。Crr N R が転がり抵抗トルクの
                    // 教科書的な形なので、無次元の Crr を長さに変換するのに要る
                    const float ra = std::sqrt(c.raC[0] * c.raC[0] + c.raC[1] * c.raC[1]
                                               + c.raC[2] * c.raC[2]);
                    const float rbl = std::sqrt(c.rbC[0] * c.rbC[0] + c.rbC[1] * c.rbC[1]
                                                + c.rbC[2] * c.rbC[2]);
                    c.rollRadius = (ra > rbl) ? ra : rbl;
                } else {
                    c.muRoll = 0.0f; // 回っていない = 転がり抵抗は定義できない
                }
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
            // ---- 関節 → 接触 の順 (M60 決定台帳 3) ----
            // 各反復の**先頭**で関節を解く。貫通のほうが目に見えるので接触を最後に置く
            // (関節が数 mm ずれるより、箱が床にめり込むほうが絵として壊れて見える)
            for (ConstraintBlock& blk : jointBlocks) {
                SolveConstraintBlock(
                    blk, (blk.ai >= 0) ? bodies[static_cast<size_t>(blk.ai)] : worldAnchorBody,
                    (blk.bi >= 0) ? bodies[static_cast<size_t>(blk.bi)] : worldAnchorBody);
            }
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
                    // ★静止/動摩擦の分離 (M59f2)。まず μs の枠で止められるかを見て、
                    //   はみ出したら「滑っている」と判定して枠を μd へ落とす。
                    //   μs == μd (材料未割当) なら 2 つの枠が同じ値なので、
                    //   下の式は従来の 1 本のクランプにビットまで畳まれる
                    const float maxF = c.mu * totalJn;
                    const float maxS = c.muS * totalJn;
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
                        // 枠は既定で μs。超えたら滑走に転じたので μd の枠を採る
                        float lim = maxS;
                        if (now > lim || now < -lim) {
                            lim = maxF;
                        }
                        if (now > lim) {
                            now = lim;
                        } else if (now < -lim) {
                            now = -lim;
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
                    // ---- 4. 転がり抵抗 (M59f2): 固定軸まわりの**純粋な角インパルス** ----
                    // 上限は Crr · ΣJn · R (教科書の τ = Crr N R を力積の形にしたもの)。
                    // 並進には一切効かない = 転がる球が「押し戻される」ことはなく、
                    // 回転だけが枯れて止まる。M59h のスリープが要求する「本当に静止する」を
                    // 作るのがこの段の役目
                    if (c.muRoll > 0.0f && c.massRoll > 0.0f) {
                        const float wn = (A.wx - B.wx) * c.rollAxis[0]
                                       + (A.wy - B.wy) * c.rollAxis[1]
                                       + (A.wz - B.wz) * c.rollAxis[2];
                        float dl = -wn * c.massRoll;
                        const float old = c.lambdaRoll;
                        float now = old + dl;
                        const float lim = c.muRoll * totalJn * c.rollRadius;
                        if (now > lim) {
                            now = lim;
                        } else if (now < -lim) {
                            now = -lim;
                        }
                        dl = now - old;
                        c.lambdaRoll = now;
                        if (dl != 0.0f) {
                            float ax, ay, az;
                            MulInvI(A.invI, c.rollAxis[0] * dl, c.rollAxis[1] * dl,
                                    c.rollAxis[2] * dl, ax, ay, az);
                            A.wx += ax;
                            A.wy += ay;
                            A.wz += az;
                            MulInvI(B.invI, c.rollAxis[0] * dl, c.rollAxis[1] * dl,
                                    c.rollAxis[2] * dl, ax, ay, az);
                            B.wx -= ax;
                            B.wy -= ay;
                            B.wz -= az;
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
            // ---- 関節の位置補正 (M60a)。接触より先 (決定台帳 3 と同順) ----
            // アンカーのずれを質量比で分けて**並進だけ**で詰める (接触と同じ流儀。
            // 回転補正はしない = 簡易ソルバの発散防止)。
            // ★ボールジョイントについてはこれで**厳密**: アンカーのワールド位置は
            //   「形状原点 + 姿勢で回した局所オフセット」なので、形状原点を誤差ぶん動かせば
            //   誤差はちょうど 0 になる。速度行に bias (Baumgarte) を一切入れず、
            //   ドリフトの始末をここへ全部寄せられるのはこの性質のおかげ (M59g1-2 の教訓)
            for (const JointLink& l : jointLinks) {
                const int32_t type = l.jc->type;
                float qa[4], qb[4];
                jointQuats(l, qa, qb);
                float ax[3] = { 0.0f, 1.0f, 0.0f };
                const bool hasAxis = JointAxisWorld(qa, *l.jc, ax);
                // ---- (1) 姿勢のずれ (M60b)。**並進より先に回す** ----
                // 回すとアンカーが動くので、並進をあとに置いたほうが 1 パスで詰まる。
                // ★角度自由度にはボールジョイントのような「並進だけで厳密に直る」性質が
                //   無い — 速度拘束だけでは軸のずれが積分誤差として累積するので、
                //   ここで姿勢そのものを回して基準へ戻す
                if (type == 1 || type == 2 || type == 3) {
                    float e[3];
                    JointOrientationError(qa, qb, *l.jc, e);
                    if (type == 1 && hasAxis) {
                        // ヒンジは軸まわりの回転が自由 → 軸成分を落として直交成分だけ直す
                        const float d = e[0] * ax[0] + e[1] * ax[1] + e[2] * ax[2];
                        e[0] -= d * ax[0];
                        e[1] -= d * ax[1];
                        e[2] -= d * ax[2];
                    }
                    // A を +e 側へ、B を -e 側へ (誤差は「B があるべき姿勢からどれだけ
                    // 余分に回っているか」なので、両者を寄せると 0 になる)
                    applyJointAngular(l, e[0], e[1], e[2]);
                }
                // ---- (1b) リミットのはみ出しを姿勢で戻す (M60c) ----
                // ★速度行だけでは**範囲外で静止した関節が永久に戻らない** — cdot が 0 なら
                //   λ が立たないので、行があっても何も起きない。位置補正まで通して
                //   はじめて「越えた状態から範囲内へ復帰する」が成立する。
                // ★戻す量は 2·sin(はみ出し/2) (swing は sin(はみ出し))。**常に過小評価**
                //   なので、反復する位置補正では収束が少し遅くなるだけで行き過ぎない
                //   (M60b-2 の「acos を通さない」と同じ手)
                if (l.limitOn && hasAxis) {
                    if (type == 1 || type == 4) {
                        float qe[4];
                        JointRelativeQuat(qa, qb, *l.jc, qe);
                        float sh, ch;
                        if (JointTwistHalf(qe, ax, sh, ch)) {
                            const float over = sh * l.cosHalfHi - ch * l.sinHalfHi;
                            const float under = l.sinHalfLo * ch - l.cosHalfLo * sh;
                            float corr = 0.0f; // 関節角をこれだけ動かしたい (符号つき)
                            if (over > 0.0f) {
                                corr = -2.0f * over;
                            } else if (under > 0.0f) {
                                corr = 2.0f * under;
                            }
                            if (corr != 0.0f) {
                                // 関節角は「owner が軸まわりに回った量」= B から見れば逆符号。
                                // applyJointAngular は B 基準なので corr をそのまま軸に乗せる
                                applyJointAngular(l, corr * ax[0], corr * ax[1], corr * ax[2]);
                            }
                        }
                    }
                    if (type == 4) {
                        float axB[3];
                        if (JointConeAxisB(qb, *l.jc, axB)) {
                            const float dot = ax[0] * axB[0] + ax[1] * axB[1] + ax[2] * axB[2];
                            if (dot < l.cosSwing) {
                                float nx, ny, nz;
                                Cross(ax[0], ax[1], ax[2], axB[0], axB[1], axB[2], nx, ny, nz);
                                const float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
                                if (nl > 1e-6f) {
                                    // sin(θ−L) = sinθ·cosL − cosθ·sinL (nl = sinθ, dot = cosθ)
                                    const float sd = nl * l.cosSwing - dot * l.sinSwing;
                                    if (sd > 0.0f) {
                                        const float s = sd / nl;
                                        applyJointAngular(l, nx * s, ny * s, nz * s);
                                    }
                                }
                            }
                        }
                    }
                }
                // ---- (2) アンカーのずれ (並進のみ) ----
                const float invA = (l.ai >= 0) ? bodies[static_cast<size_t>(l.ai)].invMass : 0.0f;
                const float invB = (l.bi >= 0) ? bodies[static_cast<size_t>(l.bi)].invMass : 0.0f;
                const float tim = invA + invB;
                if (tim == 0.0f) {
                    continue;
                }
                float pax, pay, paz, pbx, pby, pbz;
                jointAnchorsWorld(l, pax, pay, paz, pbx, pby, pbz);
                float ex = pbx - pax;
                float ey = pby - pay;
                float ez = pbz - paz;
                if (type == 3 && hasAxis) {
                    // Slider は軸方向のずれを「ずれ」とみなさない (そこが可動域)
                    const float d = ex * ax[0] + ey * ax[1] + ez * ax[2];
                    ex -= d * ax[0];
                    ey -= d * ax[1];
                    ez -= d * ax[2];
                }
                if (invA > 0.0f) {
                    Body& A = bodies[static_cast<size_t>(l.ai)];
                    const float s = invA / tim;
                    A.pose.px += ex * s;
                    A.pose.py += ey * s;
                    A.pose.pz += ez * s;
                }
                if (invB > 0.0f) {
                    Body& B = bodies[static_cast<size_t>(l.bi)];
                    const float s = invB / tim;
                    B.pose.px -= ex * s;
                    B.pose.py -= ey * s;
                    B.pose.pz -= ez * s;
                }
                // ---- (2b) スライダの変位リミット (M60c): 軸方向のはみ出しを並進で戻す ----
                // ★(2) が動かしたのは**軸に直交する成分だけ**なので、u は上のアンカーから
                //   そのまま測ってよい (軸方向は (2) の対象外 = そこが可動域)
                if (l.limitOn && hasAxis && type == 3) {
                    const float u = (pax - pbx) * ax[0] + (pay - pby) * ax[1]
                                  + (paz - pbz) * ax[2];
                    float corr = 0.0f;
                    if (u > l.jc->limitMax) {
                        corr = l.jc->limitMax - u;
                    } else if (u < l.jc->limitMin) {
                        corr = l.jc->limitMin - u;
                    }
                    if (corr != 0.0f) {
                        if (invA > 0.0f) {
                            Body& A = bodies[static_cast<size_t>(l.ai)];
                            const float s = corr * invA / tim;
                            A.pose.px += ax[0] * s;
                            A.pose.py += ax[1] * s;
                            A.pose.pz += ax[2] * s;
                        }
                        if (invB > 0.0f) {
                            Body& B = bodies[static_cast<size_t>(l.bi)];
                            const float s = corr * invB / tim;
                            B.pose.px -= ax[0] * s;
                            B.pose.py -= ax[1] * s;
                            B.pose.pz -= ax[2] * s;
                        }
                    }
                }
            }
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

        // ---- CCD (M59j): 位置積分の**直前**に掃引して、最初に触れる位置で止める ----
        // 置き場は「位置補正の後・位置積分の前」= このサブステップの最終ポーズが確定し、
        // まだ誰も前進していない唯一の地点。
        // ★位置積分ループの中で 1 体ずつやってはいけない — 相手のポーズが「もう積分した/
        //   まだの」で混ざり、ボディの走査順が結果に載る。掃引を全部先に済ませる。
        // ★相手は**不動として扱う** (掃引でも応答でも)。掃引が相手を止めて見ている以上、
        //   応答だけ 2 体拘束にしても筋が通らない。運動量保存はこの 1 発では成り立たないが、
        //   止まった次のサブステップからは通常の離散ソルバが摩擦も反発も正しく解く。
        // ★CCD は接触**イベント**も報告する — 反発で跳ね返った弾は貫通を作らないまま
        //   離れていくので、ここで出さないと OnCollisionEnter が一度も飛ばない
        std::vector<SolidContact> ccdContacts;
        if (anyCcd) {
            for (Body& b : bodies) {
                b.ccdClamped = false;
            }
            const size_t bn = bodies.size();
            for (size_t i = 0; i < bn; ++i) {
                Body& A = bodies[i];
                if (!A.rb || !A.rb->ccd || !A.solid || A.invMass == 0.0f || A.sleeping) {
                    continue;
                }
                const float mvx = A.vx * h, mvy = A.vy * h, mvz = A.vz * h;
                const float mv = std::sqrt(mvx * mvx + mvy * mvy + mvz * mvz);
                const float R = CcdBoundingRadius(A.pose);
                if (R <= 0.0f || mv <= kCcdMotionRatio * R) {
                    continue; // 起動しきい値 (kCcdMotionRatio のコメントが根拠)
                }
                const float invMv = 1.0f / mv;
                const float dx = mvx * invMv, dy = mvy * invMv, dz = mvz * invMv;
                // 掃引した外接球が占める領域の AABB。相手 AABB と重ならなければ
                // 当たり得ないので落としてよい (保守的 = 結果を変えない枝刈り)
                const float ex = A.pose.px + mvx, ey = A.pose.py + mvy, ez = A.pose.pz + mvz;
                const float sMinX = ((A.pose.px < ex) ? A.pose.px : ex) - R;
                const float sMaxX = ((A.pose.px > ex) ? A.pose.px : ex) + R;
                const float sMinY = ((A.pose.py < ey) ? A.pose.py : ey) - R;
                const float sMaxY = ((A.pose.py > ey) ? A.pose.py : ey) + R;
                const float sMinZ = ((A.pose.pz < ez) ? A.pose.pz : ez) - R;
                const float sMaxZ = ((A.pose.pz > ez) ? A.pose.pz : ez) + R;
                float bestT = mv;
                bool hit = false;
                size_t hitJ = 0;
                float hnx = 0, hny = 0, hnz = 0, hpx = 0, hpy = 0, hpz = 0;
                for (size_t j = 0; j < bn; ++j) {
                    if (j == i) {
                        continue;
                    }
                    const Body& B = bodies[j];
                    if (!B.solid || !shapes::CanCollide(A.layer, A.mask, B.layer, B.mask)) {
                        continue;
                    }
                    float tminX, tminY, tminZ, tmaxX, tmaxY, tmaxZ;
                    shapes::ComputeAabb(B.pose, tminX, tminY, tminZ, tmaxX, tmaxY, tmaxZ);
                    if (tmaxX < sMinX || tminX > sMaxX || tmaxY < sMinY || tminY > sMaxY
                        || tmaxZ < sMinZ || tminZ > sMaxZ) {
                        continue;
                    }
                    float t, nx, ny, nz, cpx, cpy, cpz;
                    if (!CcdSweepTarget(B.pose, A.pose.px, A.pose.py, A.pose.pz, dx, dy, dz, R,
                                        bestT, t, nx, ny, nz, cpx, cpy, cpz)) {
                        continue;
                    }
                    if (t >= bestT) {
                        continue; // 厳密 < = 同距離は低 index が勝つ (決定論)
                    }
                    bestT = t;
                    hit = true;
                    hitJ = j;
                    hnx = nx; hny = ny; hnz = nz;
                    hpx = cpx; hpy = cpy; hpz = cpz;
                }
                if (!hit) {
                    continue;
                }
                const float ccx = A.pose.px + dx * bestT + A.comx;
                const float ccy = A.pose.py + dy * bestT + A.comy;
                const float ccz = A.pose.pz + dz * bestT + A.comz;
                const float rx = hpx - ccx, ry = hpy - ccy, rz = hpz - ccz;
                float pvx, pvy, pvz;
                Cross(A.wx, A.wy, A.wz, rx, ry, rz, pvx, pvy, pvz);
                pvx += A.vx; pvy += A.vy; pvz += A.vz;
                const float vn = pvx * hnx + pvy * hny + pvz * hnz; // 法線は相手→A 向き
                // ★掃引が当たっても、**法線方向の進みが小さいなら止めない**。
                //   これが無いと「接している面に沿って高速で滑るボディが凍る」— 面から
                //   1mm 浮いて僅かに沈み込んでいるだけで掃引は「触れる」を返すので、
                //   TOI≈0 で接線方向の移動量ごと削られてしまう。貫通は法線方向の現象
                //   なので、判定も法線方向の移動量で行うのが筋が通る (しきい値は
                //   起動ゲートと同じ kCcdMotionRatio * R)
                if (vn >= 0.0f || -vn * h <= kCcdMotionRatio * R) {
                    continue; // 離れつつある / かすっているだけ = 離散ソルバの担当
                }
                // 1) このサブステップの移動量を TOI まで詰める (skin ぶん手前で止める)
                float travel = bestT - kCcdSkin;
                if (travel < 0.0f) {
                    travel = 0.0f;
                }
                A.ccdClamped = true;
                A.ccdDx = dx * travel;
                A.ccdDy = dy * travel;
                A.ccdDz = dz * travel;
                // 2) 法線インパルスを 1 発。**止めるだけでは足りない** — 速度が残ったままだと
                //    次サブステップも同じしきい値を超えて同じ TOI で止められ、貫通が
                //    生まれず接触も作られず、壁の手前で永久に浮く
                const Body& B = bodies[hitJ];
                float rest = (A.restitution < B.restitution) ? A.restitution : B.restitution;
                if (vn > -restitutionVelThreshold) {
                    rest = 0.0f; // 通常ソルバと同じ micro-bounce 除去の規約
                }
                const float km = EffectiveMassInv(A, rx, ry, rz, hnx, hny, hnz);
                if (km <= 0.0f) {
                    continue;
                }
                const float jn = -(1.0f + rest) * vn / km;
                ApplyImpulse(A, rx, ry, rz, hnx * jn, hny * jn, hnz * jn, 1.0f);
                // 3) 接触イベント。法線の向きは SolidContact の規約
                //    「大 index 側 → 小 index 側」に合わせる (bodies は index 昇順)
                SolidContact sc;
                const uint32_t ia = A.entity.index;
                const uint32_t ib = B.entity.index;
                if (i < hitJ) {
                    sc.key = (static_cast<uint64_t>(ia) << 32) | ib;
                    sc.nx = hnx; sc.ny = hny; sc.nz = hnz;
                } else {
                    sc.key = (static_cast<uint64_t>(ib) << 32) | ia;
                    sc.nx = -hnx; sc.ny = -hny; sc.nz = -hnz;
                }
                sc.px = hpx; sc.py = hpy; sc.pz = hpz;
                sc.impulse = jn;
                ccdContacts.push_back(sc);
            }
            // 出力は key 昇順が契約。CCD ボディ同士の正面衝突では同じペアが両側から
            // 報告されるので、ここで 1 本に畳む (インパルスは足す = MergeSubstepContacts と同規約)
            std::sort(ccdContacts.begin(), ccdContacts.end(),
                      [](const SolidContact& a, const SolidContact& b) { return a.key < b.key; });
            size_t cw = 0;
            for (size_t k = 0; k < ccdContacts.size(); ++k) {
                if (cw > 0 && ccdContacts[cw - 1].key == ccdContacts[k].key) {
                    ccdContacts[cw - 1].impulse += ccdContacts[k].impulse;
                } else {
                    ccdContacts[cw++] = ccdContacts[k];
                }
            }
            ccdContacts.resize(cw);
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
            // 眠りペア (key 昇順) を先に合流させる。どちらも候補ペア昇順で作られて
            // いるのでキーが衝突することはなく、線形マージで昇順が保たれる
            MergeSubstepContacts(sleepContacts, subContacts);
            MergeSubstepContacts(ccdContacts, subContacts); // M59j
            MergeSubstepContacts(subContacts, *outContacts);
            subContacts.clear();
        }

        if (sub == substeps - 1) {
            islandPairs = candidates; // M59h: 入眠判定に使う (最後のステップのぶん)
            // ★関節ペアも島に入れる (M60a)。接触が 1 つも無くても関節は運動を伝えるので、
            //   ここを忘れると「繋がった 2 体が別々の島になり、片方だけ先に眠る」。
            //   union-find の根は最小 index へ正規化されるので、追記の順序は結果に効かない
            //   (それでも jointLinks は owner index 昇順なので走査自体も決定論的)
            for (const JointLink& l : jointLinks) {
                if (l.ai < 0 || l.bi < 0) {
                    continue;
                }
                const uint32_t ja = static_cast<uint32_t>(l.ai);
                const uint32_t jb = static_cast<uint32_t>(l.bi);
                const uint64_t lo2 = (ja < jb) ? ja : jb;
                const uint64_t hi2 = (ja < jb) ? jb : ja;
                islandPairs.push_back((lo2 << 32) | hi2);
            }
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
            // M59j: CCD が掃引で確定した移動量があればそれを使う。**係数を掛けるのではなく
            // 差し替える** — CCD は止めると同時にインパルスを入れるので、この時点の
            // b.v* は掃引に使った速度と既に別物になっている。ccdClamped が false のあいだは
            // 式が従来の `b.v* * h` そのままで、ビット同一が自明に立つ
            float mvx, mvy, mvz;
            if (b.ccdClamped) {
                mvx = b.ccdDx; mvy = b.ccdDy; mvz = b.ccdDz;
            } else {
                mvx = b.vx * h; mvy = b.vy * h; mvz = b.vz * h;
            }
            float comWx = 0.0f, comWy = 0.0f, comWz = 0.0f;
            if (b.hasCom) {
                comWx = b.pose.px + b.comx + mvx;
                comWy = b.pose.py + b.comy + mvy;
                comWz = b.pose.pz + b.comz + mvz;
            } else {
                b.pose.px += mvx;
                b.pose.py += mvy;
                b.pose.pz += mvz;
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

    // ---- 入眠判定 (M59h): 島の全員が静かなときだけ眠らせる ----
    // ★**閾値は env の中** = 存在ゲート。env が無いシーンはここを 1 文字も通らない。
    // ★判定は int の tick カウンタ (秒の float 累積は加算順で割れるので禁止)。
    // ★1 体だけ静かでも眠らせない — 上に乗っている箱がまだ動いているのに土台が
    //   眠ると、次 tick に起こされて跳ねる「まばたき」が出る。島単位で揃えるのが要点
    if (env && env->sleepDelayTicks > 0) {
        const float linT = env->sleepLinearThreshold;
        const float angT = env->sleepAngularThreshold;
        const int32_t delay = env->sleepDelayTicks;
        // 1) 静けさの計数。眠っているボディは触らない (= sleepTicks が動かない
        //    = 眠っているあいだワールドハッシュが完全に静止する)
        for (Body& b : bodies) {
            if (!b.rb || b.sleeping || b.invMass == 0.0f) {
                continue;
            }
            const float v2 = b.vx * b.vx + b.vy * b.vy + b.vz * b.vz;
            const float w2 = b.wx * b.wx + b.wy * b.wy + b.wz * b.wz;
            if (v2 <= linT * linT && w2 <= angT * angT) {
                if (b.rb->sleepTicks < delay) {
                    b.rb->sleepTicks += 1; // ★閾値でクランプ (眠る前もハッシュを揺らさない)
                }
            } else {
                b.rb->sleepTicks = 0;
            }
        }
        // 2) 島 (union-find)。**根は常に最小 index へ正規化**するので、
        //    ペアの処理順に依らずラベルが一意 = 総当たり実装と一致する
        std::vector<uint32_t> parent(bodies.size());
        for (size_t i = 0; i < parent.size(); ++i) {
            parent[i] = static_cast<uint32_t>(i);
        }
        auto findRoot = [&](uint32_t x) -> uint32_t {
            while (parent[x] != x) {
                parent[x] = parent[parent[x]]; // 経路圧縮 (結果は根なので決定論に影響しない)
                x = parent[x];
            }
            return x;
        };
        auto unite = [&](uint32_t a, uint32_t b) {
            const uint32_t ra = findRoot(a);
            const uint32_t rb2 = findRoot(b);
            if (ra == rb2) {
                return;
            }
            // 小さいほうを根にする = ラベルが入力順に依存しない
            if (ra < rb2) {
                parent[rb2] = ra;
            } else {
                parent[ra] = rb2;
            }
        };
        // 静的/kinematic は繋がない — 繋ぐと床を介して世界中が 1 つの島になる
        auto isDynamic = [&](const Body& b) { return b.rb && !b.rb->isKinematic; };
        for (const uint64_t pairKey : islandPairs) {
            const uint32_t ai = static_cast<uint32_t>(pairKey >> 32);
            const uint32_t bi = static_cast<uint32_t>(pairKey & 0xFFFFFFFFu);
            if (isDynamic(bodies[ai]) && isDynamic(bodies[bi])) {
                unite(ai, bi);
            }
        }
        // 3) 島ごとの最小カウンタ。1 体でも足りなければ島全体が起きたまま
        std::vector<int32_t> islandMin(bodies.size(), delay);
        std::vector<uint8_t> islandHas(bodies.size(), 0);
        for (size_t i = 0; i < bodies.size(); ++i) {
            Body& b = bodies[i];
            if (!b.rb || b.sleeping || b.invMass == 0.0f) {
                continue;
            }
            const uint32_t r = findRoot(static_cast<uint32_t>(i));
            islandHas[r] = 1;
            if (b.rb->sleepTicks < islandMin[r]) {
                islandMin[r] = b.rb->sleepTicks;
            }
        }
        // 4) 入眠。**velocity / angularVelocity を厳密 0.0f へ書く** (書き戻しが拾う)
        for (size_t i = 0; i < bodies.size(); ++i) {
            Body& b = bodies[i];
            if (!b.rb || b.sleeping || b.invMass == 0.0f) {
                continue;
            }
            const uint32_t r = findRoot(static_cast<uint32_t>(i));
            if (!islandHas[r] || islandMin[r] < delay) {
                continue;
            }
            b.rb->isSleeping = true;
            b.rb->sleepTicks = delay;
            b.vx = 0.0f; b.vy = 0.0f; b.vz = 0.0f;
            b.wx = 0.0f; b.wy = 0.0f; b.wz = 0.0f;
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

float SelectStaticFriction(const ColliderComponent& col, const PhysMat* mat)
{
    if ((col.materialOverrideBits & kPhysMatOverrideFriction) != 0u) {
        return col.friction;
    }
    // ★材料なしは **col.friction** — SelectFriction と同じ式・同じ入力なので μs と μd が
    //   ビットまで一致し、ソルバの静止/動の分岐が従来の 1 本のクランプに畳まれる
    return mat ? mat->staticFriction : col.friction;
}

float SelectRollingResistance(const ColliderComponent& col, const PhysMat* mat)
{
    if ((col.materialOverrideBits & kPhysMatOverrideRolling) != 0u) {
        return 0.0f;
    }
    return mat ? mat->rollingResistance : 0.0f;
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
            if (col->shape != 4) {
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
