#pragma once
// PhysicsSystem.cpp / PhysicsCharacters.cpp / PhysicsBodyOps.cpp だけが include する内部ヘッダ。
// 剛体ソルバの作業用ボディ (Body) と、翻訳単位を跨いで使う最小の scalar ヘルパーを置く。
// それ以外のソルバ補助 (接触・関節・CCD・質量特性の合成) は PhysicsSystem.cpp の無名名前空間に残してある —
// Update 以外に使い手が無いものをヘッダへ出すと、読む側に「共有されている」と誤解させるだけなので。
// 決定論のため全て scalar float 演算 (XMVECTOR SIMD を使わない = Debug/Release で同一ビット)。
// inline なのは複数の翻訳単位から同じ式を呼ぶため。/fp:precise では inline の有無で結果は変わらない
// (Debug と Release が replay_verify でビット一致していることがその根拠)
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/EntityID.h"
#include "Engine/Engine/Physics/Collider/ConvexHull.h" // LocalInertiaDiag: 凸包の質量特性
#include "Engine/Engine/Physics/Rigid/PhysicsSystem.h" // SolidContact
#include "Engine/Engine/Physics/Rigid/Shapes.h"

namespace mye {

inline constexpr float kGravity = -9.81f;      // m/s^2 (重力加速度、-Y)

// ---- scalar クォータニオン演算 (親子合成用。XMVECTOR 禁止 = 決定論契約) ----

// Hamilton 積 a ⊗ b
inline void QuatMul(float ax, float ay, float az, float aw, float bx, float by, float bz, float bw,
             float& ox, float& oy, float& oz, float& ow)
{
    ox = aw * bx + ax * bw + ay * bz - az * by;
    oy = aw * by - ax * bz + ay * bw + az * bx;
    oz = aw * bz + ax * by - ay * bx + az * bw;
    ow = aw * bw - ax * bx - ay * by - az * bz;
}

// 単位クォータニオンでベクトルを回転: v' = v + 2·q.w·(q×v) + 2·q×(q×v)
inline void QuatRotate(float qx, float qy, float qz, float qw, float vx, float vy, float vz, float& ox,
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
    DirectX::XMFLOAT3 colliderOffset = {}; // 剛体原点と形状中心を分離する
    DirectX::XMFLOAT3 scale = { 1, 1, 1 };  // ワールドスケール (pose 再構築用)
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
    // M60d: 粘着力 [N]。材料未割当なら 0 で、法線インパルスの下限が従来どおり 0 になる
    float adhesion = 0.0f;
    // M60e: 複合コライダー。**subCount == 0 が非複合** = 形状 1 個 (b.pose) の従来経路。
    // 複合だけ慣性を 3x3 フルで持つ (平行軸で合成すると非対角が出るため)
    // ★`col` は**動的ボディにしか入っていない** (静的は pose だけ作って col を持たない) —
    //   「自分自身の形状を持つか」は専用フラグで表す。これを col の有無で代用すると
    //   静的コライダーが形状ゼロになって世界から床が消える
    bool ownShape = false;
    int32_t subFirst = 0;
    int32_t subCount = 0;
    // M60f: 慣性を 3x3 フルで持つか。**複合 (M60e) と凸包 (M60f) の 2 通りある**ので
    // subCount では判別できない — 「非対角が出る形状か」を専用フラグで表す
    bool fullInertia = false;
    float invILocal[3][3] = {}; // フルテンソル時: 剛体ローカルでの I⁻¹
    // M60g1: kinematic として扱うか = `rb->isKinematic` **または**「休止中のラグドールに
    // 属する部位」。後者は PartFollowSystem がアニメで LocalTransform を書いている最中なので、
    // 物理が動かすと綱引きになる。★`rb->isKinematic` を書き換える案は採らない —
    // Inspector の値を潰すし、ソルバがコンポーネントを書くのは家風でない (決定台帳 14)
    bool kinematic = false;
    int32_t layer = 0;             // 衝突レイヤー (M36a、Collider から複製)
    uint32_t mask = 0xFFFFFFFFu;   // 衝突マスク (既定 = 全レイヤー → 従来挙動)
    // 親のワールドフレーム (M28d)。収集時に合成し運動学的フレームとして固定。
    // identity (ルート) なら書き戻しは従来の直接代入 (ビット同一 fast-path)
    WorldFrame frame;
};

// 形状のローカル主軸慣性 (対角、質量 m)。col null は半径 0.5 の球扱い。
// pose の寸法はワールドスケール適用済みなのでそのまま使う
inline void LocalInertiaDiag(const ColliderComponent* col, const ShapePose& pose, float m, float& ix,
                      float& iy, float& iz)
{
    if (!col || pose.shape == collidershape::kSphere) {
        const float r = col ? pose.radius : 0.5f;
        const float i = 0.4f * m * r * r; // 2/5 m r²
        ix = iy = iz = i;
        return;
    }
    if (pose.shape == collidershape::kConvex) {
        // M60f: 凸包。ソルバ本体は下のフル 3x3 経路 (Body::invILocal) を通るので、
        // ここへ来るのは ABI の AddTorque / AddForceAtPoint と、凸包が未生成で
        // フル経路に載れなかったボディだけ。**非対角を捨てた対角近似**で答える —
        // 厳密ではないが、既定のカプセル式に落ちるよりは遥かにまし
        const ConvexHullData* h = static_cast<const ConvexHullData*>(pose.meshData);
        if (h && h->Valid()) {
            float vol = 0.0f;
            DirectX::XMFLOAT3 com{};
            float I[3][3];
            ConvexMassProperties(*h, pose.sx, pose.sy, pose.sz, vol, com, I);
            if (vol > 1e-12f) {
                const float s = m / vol; // 密度 1 の積分値を実質量へ
                ix = I[0][0] * s;
                iy = I[1][1] * s;
                iz = I[2][2] * s;
                return;
            }
        }
    }
    if (pose.shape == collidershape::kBox) {
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
inline void InvInertiaWorld(const ShapePose& pose, float ix, float iy, float iz, float out[3][3])
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

inline void MulInvI(const float m[3][3], float x, float y, float z, float& ox, float& oy, float& oz)
{
    ox = m[0][0] * x + m[0][1] * y + m[0][2] * z;
    oy = m[1][0] * x + m[1][1] * y + m[1][2] * z;
    oz = m[2][0] * x + m[2][1] * y + m[2][2] * z;
}

inline void Cross(float ax, float ay, float az, float bx, float by, float bz, float& ox, float& oy,
           float& oz)
{
    ox = ay * bz - az * by;
    oy = az * bx - ax * bz;
    oz = ax * by - ay * bx;
}

// ---- キャラクターコントローラ (M29b) ----

struct CharBody {
    EntityID entity;
    CharacterControllerComponent* cc = nullptr;
    LocalTransform* lt = nullptr;
    WorldFrame frame;              // 親フレーム (収集時固定)
    float px = 0, py = 0, pz = 0;  // tick 頭のワールド位置
    float radius = 0, halfSeg = 0; // ワールドスケール適用済みのカプセル寸法
    float stepHeight = 0;          // 登れる段差 (stepOffset x |scale.y|、ワールド m)。0 以下 = 段差登りなし
    int32_t layer = 0;             // 衝突レイヤー (M36a、併用 Collider から。無ければ既定)
    uint32_t mask = 0xFFFFFFFFu;
};

// キャラクターコントローラ解決 (M29b)。PhysicsSystem::Update が剛体を解き終えた最終 pose を障害物に、
// chars を entity.index 昇順で押し出す。outContacts 非 null なら Rigidbody 持ちとの接触を key 昇順で書く
// (M93e、impulse は 0)。実装は PhysicsCharacters.cpp
void SolveCharacters(std::vector<Body>& bodies, std::vector<CharBody>& chars, float dt,
                     std::vector<SolidContact>* outContacts);

} // namespace mye
