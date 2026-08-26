#include "Editor/RagdollBuilder.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/Components.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/EntityNaming.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/PartFollowSystem.h" // DecomposeRowMajorTRS (追従システムと共有)
#include "Engine/Engine/Scene.h"
#include "Engine/Renderer/Skeleton.h"

using namespace DirectX;

namespace mye {
namespace ragdoll_build {
namespace {

constexpr float kPi = 3.14159265358979323846f;

// ---- scalar クォータニオン演算 ----
// ★式は `PhysicsSystem.cpp` の同名関数の写し。**ソルバが restRotation をこの規約で
//   読む**ので、ここで積の向きを変えると生成したラグドールが rest で捻れる。
//   `XMQuaternionMultiply` は引数順が逆 (Q1 のあと Q2) なので使わない。

// Hamilton 積 a ⊗ b
void QuatMul(float ax, float ay, float az, float aw, float bx, float by, float bz, float bw,
             float& ox, float& oy, float& oz, float& ow)
{
    ox = aw * bx + ax * bw + ay * bz - az * by;
    oy = aw * by - ax * bz + ay * bw + az * bx;
    oz = aw * bz + ax * by - ay * bx + az * bw;
    ow = aw * bw - ax * bx - ay * by - az * bz;
}

// 単位クォータニオンでベクトルを回転
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

// +Y をローカル方向 d (単位) へ向ける最短弧クォータニオン。
// ★分岐は 2 つとも**厳密比較ではなくしきい値**だが、入力は同じ計算から出る同じ値なので
//   同一入力に対する分岐は一意 (g2-1 のビット同一はこれで足りる)。
// ★真後ろ (d == -Y) は最短弧が一意に決まらないので **X 軸まわり 180 度**に決め打つ
//   (決めておかないと「たまたま選ばれた軸」で階層が変わる)
XMFLOAT4 QuatFromYTo(const XMFLOAT3& d)
{
    const float c = d.y; // dot((0,1,0), d)
    if (c > 0.999999f) {
        return { 0.0f, 0.0f, 0.0f, 1.0f };
    }
    if (c < -0.999999f) {
        return { 1.0f, 0.0f, 0.0f, 0.0f };
    }
    // axis = (0,1,0) × d
    const float axx = d.z, axy = 0.0f, axz = -d.x;
    const float s = std::sqrt((1.0f + c) * 2.0f);
    const float inv = 1.0f / s;
    return { axx * inv, axy * inv, axz * inv, s * 0.5f };
}

// カプセルの体積 (半径 r / 全高 h)。円柱 + 両端の半球。h <= 2r なら球へ潰れる
float CapsuleVolume(float r, float h)
{
    const float cyl = (h > 2.0f * r) ? (h - 2.0f * r) : 0.0f;
    return kPi * r * r * cyl + (4.0f / 3.0f) * kPi * r * r * r;
}

// 1 本の骨から作る部位の設計図。**ECS へ触る前に全部確定させる** —
// 生成中に World を読みながら値を決めると、走査順や既存の子の有無で結果が変わりうる
struct BonePlan {
    int32_t joint = -1;      // 対象ジョイント index
    int32_t parentPart = -1; // 部位を持つ最も近い祖先ジョイント (-1 = ルート部位 = Joint 無し)
    float len = 0.0f;        // 骨長 [m]
    float radius = 0.0f;     // カプセル半径 [m]
    XMFLOAT3 dir = { 0.0f, 1.0f, 0.0f };   // 骨方向 (ジョイントのローカル、単位)
    XMFLOAT3 t = { 0.0f, 0.0f, 0.0f };     // 部位の LocalTransform (= バインドの jointGlobal)
    XMFLOAT4 r = { 0.0f, 0.0f, 0.0f, 1.0f };
    XMFLOAT3 s = { 1.0f, 1.0f, 1.0f };
    XMFLOAT3 connAnchor = { 0.0f, 0.0f, 0.0f };   // 親部位ローカルでの自分の原点
    XMFLOAT4 restRot = { 0.0f, 0.0f, 0.0f, 1.0f }; // conj(自分) ⊗ 親 (ソルバの規約)
};

// 骨名が `PartComponent.joint` (char[64]) に収まり、`FindJointByName` で引けるか。
// 空名は検索対象外、63 バイト超は切り詰められて別名になる = どちらも部位にできない
bool JointNameUsable(const std::string& n)
{
    return !n.empty() && n.size() < 64;
}

std::vector<BonePlan> PlanRagdoll(const SkinnedModel& model, const Options& opt)
{
    std::vector<BonePlan> out;
    const size_t n = model.joints.size();
    if (n == 0) {
        return out;
    }
    // バインドポーズ (clip < 0) の局所行列。**アニメの現在時刻を使わない** —
    // 生成物がいつ押したかで変わってはいけない (g2-1)
    std::vector<XMMATRIX> locals;
    ComputeJointLocals(model, -1, 0.0f, locals);
    if (locals.size() != n) {
        return out;
    }

    // 各骨が「どの子へ伸びるか」を**決め打ちで固定**する: index 昇順で最初に見つかる
    // **長さのある**子。★「index 最小の子」ではない — 長さ 0 の補助ジョイント (IK の
    // ターゲットや向き合わせの nub) が先頭に来ているだけで親の骨まで消えてしまうため
    std::vector<int32_t> boneChild(n, -1);
    for (size_t c = 0; c < n; ++c) {
        const int32_t p = model.joints[c].parent;
        if (p < 0 || static_cast<size_t>(p) >= n || boneChild[static_cast<size_t>(p)] >= 0) {
            continue;
        }
        XMFLOAT4X4 cf;
        XMStoreFloat4x4(&cf, locals[c]);
        const float l2 = cf._41 * cf._41 + cf._42 * cf._42 + cf._43 * cf._43;
        if (l2 >= opt.minBoneLength * opt.minBoneLength) {
            boneChild[static_cast<size_t>(p)] = static_cast<int32_t>(c);
        }
    }

    // ---- 1) 骨ごとの向きと長さ ----
    std::vector<uint8_t> hasPart(n, 0);
    std::vector<float> lens(n, 0.0f);
    std::vector<XMFLOAT3> dirs(n, XMFLOAT3{ 0.0f, 1.0f, 0.0f });
    std::vector<XMFLOAT3> lt(n, XMFLOAT3{ 0.0f, 0.0f, 0.0f });
    std::vector<XMFLOAT4> lr(n, XMFLOAT4{ 0.0f, 0.0f, 0.0f, 1.0f });
    std::vector<XMFLOAT3> ls(n, XMFLOAT3{ 1.0f, 1.0f, 1.0f });
    for (size_t j = 0; j < n; ++j) {
        if (!JointNameUsable(model.joints[j].name)) {
            continue;
        }
        XMFLOAT3 d = { 0.0f, 0.0f, 0.0f };
        const int32_t fc = boneChild[j];
        if (fc >= 0) {
            // 骨の先端 = 子ジョイントの原点。行ベクトル規約なので平行移動は第 4 行
            XMFLOAT4X4 cf;
            XMStoreFloat4x4(&cf, locals[static_cast<size_t>(fc)]);
            d = { cf._41, cf._42, cf._43 };
        } else if (model.joints[j].parent >= 0) {
            // 末端骨は子が居ないので伸ばす先が無い。**親からの距離をそのまま流用**し、
            // 向きは「親 → 自分」を自分のローカル空間へ移したもの = 手足の延長になる。
            // ★スケールが非一様な骨では厳密でない (conj(回転) しか掛けていない)。
            //   ラグドール rig で骨に非一様スケールが入ることは無いので v1 は割り切る
            XMFLOAT4X4 sf;
            XMStoreFloat4x4(&sf, locals[j]);
            const float px = sf._41, py = sf._42, pz = sf._43;
            const float L = std::sqrt(px * px + py * py + pz * pz);
            if (L > opt.minBoneLength) {
                XMFLOAT3 dt;
                XMFLOAT4 dr;
                XMFLOAT3 ds;
                DecomposeRowMajorTRS(locals[j], dt, dr, ds);
                const float inv = 1.0f / L;
                float ox, oy, oz;
                QuatRotate(-dr.x, -dr.y, -dr.z, dr.w, px * inv, py * inv, pz * inv, ox, oy, oz);
                d = { ox * L, oy * L, oz * L };
            }
        }
        const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        if (len < opt.minBoneLength) {
            continue; // 長さ 0 の骨にカプセルは張れない
        }
        const float inv = 1.0f / len;
        hasPart[j] = 1;
        lens[j] = len;
        dirs[j] = { d.x * inv, d.y * inv, d.z * inv };
        DecomposeRowMajorTRS(JointGlobalFromLocals(model, locals, static_cast<int32_t>(j)), lt[j],
                             lr[j], ls[j]);
    }

    // 半径の下限と「短すぎる骨」の足切りは、どちらも **rig の最長骨**を基準に決める
    // (Options::minRadiusRatio / minBoneRatio のコメントが正本)。全部の骨を見てからでないと
    // 決まらないので、ここで 1 回だけ最長を採る
    float maxLen = 0.0f;
    for (size_t j = 0; j < n; ++j) {
        if (hasPart[j] && lens[j] > maxLen) {
            maxLen = lens[j];
        }
    }
    const float minRadius = maxLen * opt.minRadiusRatio;
    const float minLen = maxLen * opt.minBoneRatio;
    for (size_t j = 0; j < n; ++j) {
        if (hasPart[j] && lens[j] < minLen) {
            hasPart[j] = 0; // 短すぎる骨は落とす。子は「部位を持つ最も近い祖先」へ繋がる
        }
    }

    // ---- 2) 親子の結び (部位を持つ最も近い祖先) と関節の値 ----
    for (size_t j = 0; j < n; ++j) {
        if (!hasPart[j]) {
            continue;
        }
        BonePlan bp;
        bp.joint = static_cast<int32_t>(j);
        bp.len = lens[j];
        bp.radius = lens[j] * opt.radiusRatio;
        if (bp.radius < minRadius) {
            bp.radius = minRadius;
        }
        // 半径が全高の半分を超えると線分が消えて球になる。**そこで止める** —
        // 球になった時点でそれ以上太らせても慣性は増えるが形が骨から外れる
        if (bp.radius > bp.len * opt.lengthRatio * 0.5f) {
            bp.radius = bp.len * opt.lengthRatio * 0.5f;
        }
        bp.dir = dirs[j];
        bp.t = lt[j];
        bp.r = lr[j];
        bp.s = ls[j];
        int32_t a = model.joints[j].parent;
        while (a >= 0 && !hasPart[static_cast<size_t>(a)]) {
            a = model.joints[static_cast<size_t>(a)].parent;
        }
        bp.parentPart = a;
        if (a >= 0) {
            // 自分の原点を**祖先 a のローカル空間**で表す。局所行列を j から a の直子まで
            // 掛け合わせるだけで出るので、逆行列を 1 つも作らずに済む
            // (行ベクトル規約: 点 p のワールドは p * locals[j] * locals[親] * ...)
            XMMATRIX m = locals[j];
            int32_t cur = model.joints[j].parent;
            while (cur >= 0 && cur != a) {
                m = XMMatrixMultiply(m, locals[static_cast<size_t>(cur)]);
                cur = model.joints[static_cast<size_t>(cur)].parent;
            }
            XMFLOAT4X4 mf;
            XMStoreFloat4x4(&mf, m);
            bp.connAnchor = { mf._41, mf._42, mf._43 };
            // restRotation = conj(qOwner) ⊗ qConnected (JointComponent の規約)。
            // ★両者は同じ親フレーム (Skin) の直子なので、ワールド回転で書いても
            //   `ApplyFrame` が q_world = q_frame ⊗ q_local で合成する以上
            //   親フレームはこの積で打ち消える = **ローカル回転だけで正しい**
            const XMFLOAT4& qo = lr[j];
            const XMFLOAT4& qc = lr[static_cast<size_t>(a)];
            QuatMul(-qo.x, -qo.y, -qo.z, qo.w, qc.x, qc.y, qc.z, qc.w, bp.restRot.x, bp.restRot.y,
                    bp.restRot.z, bp.restRot.w);
        }
        out.push_back(bp);
    }
    return out;
}

} // namespace

int CountParts(const SkinnedModel& model, const Options& opt)
{
    return static_cast<int>(PlanRagdoll(model, opt).size());
}

int Build(Scene& scene, EntityID skin, const SkinnedModel& model, const Options& opt)
{
    World& world = scene.GetWorld();
    if (skin.IsNull() || world.GetComponent<SkinnedMeshComponent>(skin) == nullptr) {
        return 0;
    }
    const std::vector<BonePlan> plan = PlanRagdoll(model, opt);
    if (plan.empty()) {
        return 0;
    }

    std::vector<EntityID> partOf(model.joints.size(), kNullEntity);

    // ---- 1) 部位 + 形状の子を骨ごとに作る ----
    for (const BonePlan& bp : plan) {
        const std::string& jointName = model.joints[static_cast<size_t>(bp.joint)].name;
        char fallback[32];
        std::snprintf(fallback, sizeof(fallback), "Bone%d", bp.joint);
        // エンティティ名は骨名から '/' 等を落としたもの。**`Part.joint` には元の骨名を
        // そのまま入れる** — 名前解決は `FindJointByName` なので、正規化した名前を
        // 入れると骨が引けなくなる
        const std::string base = SanitizeEntityName(jointName, fallback);
        const std::string unique = MakeUniqueSiblingName(world, skin, base, kNullEntity);
        GameObject part = scene.CreateGameObjectTracked(unique.c_str());
        world.SetParent(part.Id(), skin);

        // 構造変更 (AddComponent) を先に済ませてからポインタを取り直す。
        // ★アーキタイプ移動で既存のコンポーネントポインタは死ぬ (M59h の申し送り 11)
        part.AddComponent<PartComponent>();
        part.AddComponent<PartBoundsComponent>();
        part.AddComponent<RigidbodyComponent>();

        if (auto* lt = part.GetComponent<LocalTransform>()) {
            // バインドポーズの jointGlobal そのもの = PartFollowSystem が書くのと同じ値。
            // ここがズレていると Play を押した瞬間に部位が飛ぶ
            lt->position = bp.t;
            lt->rotation = bp.r;
            lt->scale = bp.s;
        }
        if (auto* pc = part.GetComponent<PartComponent>()) {
            // 生バイトがワールドハッシュ対象なので末尾までゼロ埋めしてから書く
            std::memset(pc->joint, 0, sizeof(pc->joint));
            std::snprintf(pc->joint, sizeof(pc->joint), "%s", jointName.c_str());
            pc->source = kNullEntity; // 直子なので「最も近い SkinnedMesh 祖先」で解決される
        }
        if (auto* pb = part.GetComponent<PartBoundsComponent>()) {
            // 選択・レイ判定用の範囲。PartBounds は回転を持てないので、カプセルを包む
            // 軸平行の箱にする (中心はカプセルと同じ骨の中点)
            const float hx = bp.dir.x * bp.len * 0.5f;
            const float hy = bp.dir.y * bp.len * 0.5f;
            const float hz = bp.dir.z * bp.len * 0.5f;
            pb->shape = 0; // 箱
            pb->center = { hx, hy, hz };
            pb->halfExtents = { std::fabs(hx) + bp.radius, std::fabs(hy) + bp.radius,
                                std::fabs(hz) + bp.radius };
        }
        if (auto* rb = part.GetComponent<RigidbodyComponent>()) {
            rb->mass = CapsuleVolume(bp.radius, bp.len * opt.lengthRatio) * opt.density;
            rb->linearDamping = opt.linearDamping;
            rb->angularDamping = opt.angularDamping;
            // ★子の Collider を自分の形状として吸う (M60e)。部位自身はコライダーを
            //   持たない — 持たせるとカプセルが関節ピボット中心・+Y 固定になってしまう
            rb->compoundColliders = true;
        }

        // 形状の子。**回転と平行移動をここで持つのが 2 段構成の目的**
        const std::string shapeName = unique + "_Shape";
        GameObject shape = scene.CreateGameObjectTracked(shapeName.c_str());
        world.SetParent(shape.Id(), part.Id());
        shape.AddComponent<ColliderComponent>();
        if (auto* slt = shape.GetComponent<LocalTransform>()) {
            slt->position = { bp.dir.x * bp.len * 0.5f, bp.dir.y * bp.len * 0.5f,
                              bp.dir.z * bp.len * 0.5f };
            slt->rotation = QuatFromYTo(bp.dir);
        }
        if (auto* col = shape.GetComponent<ColliderComponent>()) {
            col->shape = 2; // カプセル (ローカル Y 軸)
            col->radius = bp.radius;
            // 全高は骨長より**短くする**。骨の中央に置くので両端の関節に隙間が空き、
            // 隣の骨のカプセルと食い合わない (Options::lengthRatio のコメントが正本)
            col->height = bp.len * opt.lengthRatio;
        }

        // 次の骨の `MakeUniqueSiblingName` に今作った兄弟を見せるため、ここで適用する
        // (SetParent は遅延なので、適用しないと同名の骨が重複名のまま並ぶ)
        world.ApplyStructuralChanges();
        partOf[static_cast<size_t>(bp.joint)] = part.Id();
    }

    // ---- 2) 関節 (Cone) を張る。全部位が揃ってからでないと相手の EntityID が無い ----
    for (const BonePlan& bp : plan) {
        if (bp.parentPart < 0) {
            continue; // 部位を持つ祖先が居ない = ラグドールのルート (自由に飛ぶ)
        }
        GameObject part(&world, partOf[static_cast<size_t>(bp.joint)]);
        auto* jc = part.AddComponent<JointComponent>();
        if (!jc) {
            continue;
        }
        jc->type = 4; // Cone (swing + twist)
        jc->connectedEntity = partOf[static_cast<size_t>(bp.parentPart)];
        jc->anchor = { 0.0f, 0.0f, 0.0f }; // 部位の原点 = 骨の根元 = 関節ピボットそのもの
        jc->connectedAnchor = bp.connAnchor;
        jc->axis = bp.dir; // 円錐の中心軸 = 骨が伸びる向き
        jc->useLimit = true;
        jc->limitMin = -opt.twistLimitDeg;
        jc->limitMax = opt.twistLimitDeg;
        jc->swingLimitDeg = opt.swingLimitDeg;
        jc->restRotation = bp.restRot;
    }

    // ---- 3) SkinnedMesh 側の札 ----
    // **既定は休止 (active=false)** — 生成直後のシーンは従来どおりアニメで動く
    if (world.GetComponent<RagdollComponent>(skin) == nullptr) {
        if (auto* rc = world.AddComponent<RagdollComponent>(skin)) {
            rc->active = false;
        }
    }
    world.ApplyStructuralChanges();

    MYE_LOG_INFO("[ragdoll] built %d parts from %zu joints", static_cast<int>(plan.size()),
                 model.joints.size());
    return static_cast<int>(plan.size());
}

} // namespace ragdoll_build
} // namespace mye
