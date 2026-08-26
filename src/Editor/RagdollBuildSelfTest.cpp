#include "Editor/RagdollBuildSelfTest.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <DirectXMath.h>

#include "Editor/Selection.h"
#include "Editor/Undo/UndoStack.h"
#include "Engine/Core/Components.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/DebugDraw.h"
#include "Engine/Engine/PartFollowSystem.h"
#include "Engine/Engine/RagdollBuilder.h"
#include "Engine/Engine/Physics/PhysicsDebugDraw.h"
#include "Engine/Engine/Physics/PhysicsSystem.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/Skeleton.h"

using namespace DirectX;

namespace mye {
namespace {

constexpr float kDt = 1.0f / 60.0f;

// 生成器の選別と幾何を一度に踏むための手組みスケルトン。**実アセットに依存しない** —
// 検証したいのは「スケルトンの形から出る階層」であってローダではない (M60g1 と同じ判断)。
//
//   idx name    parent  bindT             狙い
//    0  root      -1    (0,0,0)           ルート部位 (Joint を持たない側)
//    1  spine      0    (0, 1, 0)         +Y のふつうの骨
//    2  armL       1    (-0.4,0.6,0) R    子が 2 本ある骨 (先頭の子は使えないので 2 本目を選ぶ)
//    3  armR       1    ( 0.4,0.6,0)      **末端骨** (親からの距離を流用する経路)
//    4  nub        2    (0,0,0)           **長さ 0 かつ子なし** → 部位にならず方向源にもならない
//    5  elbowL     2    (-0.8,0,0)        armL の方向源 (nub を飛ばして選ばれる)
//    6  ""         5    (0,0,-0.5)        **無名** → 部位にならない (elbowL の方向源にはなる)
//    7  handL      6    (0,0,-0.3)  R     親が部位無し → Joint は elbowL まで遡る
//    8  ""         7    (0,0,-0.2)        **無名** → 部位にならない (handL の方向源にはなる)
//
// ★「長さ 0 の骨」と「部位にならない骨」は別物: 4 は自分から伸びる先が無いので部位に
//   ならないが、6 は伸びる先があっても**名前が無い**から部位にならない (`Part.joint` は
//   名前でしか骨を指せない)。中間の骨が抜ける経路を踏むのは 6 のほう。
//
// 期待される部位は root / spine / armL / armR / elbowL / handL の 6 本。
constexpr int kJointCount = 9;
const char* const kNames[kJointCount] = { "root", "spine", "armL",  "armR", "nub",
                                          "elbowL", "", "handL", "" };
constexpr int kPartCount = 6;
const char* const kPartNames[kPartCount] = { "root", "spine", "armL", "armR", "elbowL", "handL" };

SkinnedModel MakeRigSkeleton(bool plain = false)
{
    const int parents[kJointCount] = { -1, 0, 1, 1, 2, 2, 5, 6, 7 };
    const XMFLOAT3 offs[kJointCount] = {
        { 0.0f, 0.0f, 0.0f },  { 0.0f, 1.0f, 0.0f },   { -0.4f, 0.6f, 0.0f },
        { 0.4f, 0.6f, 0.0f },  { 0.0f, 0.0f, 0.0f },   { -0.8f, 0.0f, 0.0f },
        { 0.0f, 0.0f, -0.5f }, { 0.0f, 0.0f, -0.3f },  { 0.0f, 0.0f, -0.2f },
    };
    SkinnedModel m;
    for (int i = 0; i < kJointCount; ++i) {
        SkeletonJoint j;
        j.parent = parents[i];
        j.name = (plain && i == 6) ? "wristL" : kNames[i];
        j.bindT = offs[i];
        m.joints.push_back(j);
    }
    // armL と handL にはバインド回転を入れる。**恒等だらけにしない**のが要点 —
    // restRotation が単位のままでも通ってしまう試験になる
    XMStoreFloat4(&m.joints[2].bindR, XMQuaternionRotationAxis(XMVectorSet(0, 0, 1, 0), 0.436f));
    XMStoreFloat4(&m.joints[7].bindR, XMQuaternionRotationAxis(XMVectorSet(1, 0, 0, 0), 0.262f));
    {
        std::vector<XMMATRIX> locals;
        ComputeJointLocals(m, -1, 0.0f, locals);
        for (size_t i = 0; i < m.joints.size(); ++i) {
            const XMMATRIX g = JointGlobalFromLocals(m, locals, static_cast<int32_t>(i));
            XMStoreFloat4x4(&m.joints[i].inverseBind, XMMatrixInverse(nullptr, g));
        }
    }
    // アニメ 1 本 (spine を回す)。生成直後のシーンが従来どおり再生することの被写体
    SkeletalClip c;
    c.name = "sway";
    c.duration = 1.0f;
    c.tracks.resize(kJointCount);
    JointTrack& tr = c.tracks[1];
    tr.rTimes = { 0.0f, 1.0f };
    XMFLOAT4 q0, q1;
    XMStoreFloat4(&q0, XMQuaternionIdentity());
    XMStoreFloat4(&q1, XMQuaternionRotationAxis(XMVectorSet(0, 0, 1, 0), 0.9f));
    tr.rVals = { q0, q1 };
    m.clips.push_back(std::move(c));
    return m;
}

// SkinnedMesh だけのシーンを作る (ラグドールはまだ無い)
GameObject MakeSkin(Scene& s, AssetID model, int clip, int timeTicks)
{
    GameObject skin = s.CreateGameObjectTracked("Skin");
    auto* sm = skin.AddComponent<SkinnedMeshComponent>();
    sm->model = model;
    sm->clip = clip;
    sm->timeTicks = timeTicks;
    sm->playing = 0; // SkinningSystem は回さない (試験が時刻を明示的に置く)
    s.GetWorld().ApplyStructuralChanges();
    return skin;
}

// 直子を名前で引く (兄弟リンクを辿る。Ragdoll.cpp のパレット構築と同じ走査)
EntityID FindChild(World& world, EntityID parent, const char* name)
{
    const auto* h = world.GetComponent<HierarchyComponent>(parent);
    EntityID child = h ? h->firstChild : kNullEntity;
    while (!child.IsNull()) {
        const char* n = world.GetName(child);
        if (n && std::strcmp(n, name) == 0) {
            return child;
        }
        const auto* ch = world.GetComponent<HierarchyComponent>(child);
        child = ch ? ch->nextSibling : kNullEntity;
    }
    return kNullEntity;
}

int CountChildren(World& world, EntityID parent)
{
    const auto* h = world.GetComponent<HierarchyComponent>(parent);
    EntityID child = h ? h->firstChild : kNullEntity;
    int n = 0;
    while (!child.IsNull()) {
        ++n;
        const auto* ch = world.GetComponent<HierarchyComponent>(child);
        child = ch ? ch->nextSibling : kNullEntity;
    }
    return n;
}

// 短い骨の足切りを外した設定。**前半の試験は「骨の形から出る階層」を見たい**ので、
// 足切り (既定 0.25) が邪魔になる。足切り自身は専用の項目で見る
ragdoll_build::Options KeepShortBones()
{
    ragdoll_build::Options o;
    o.minBoneRatio = 0.0f;
    return o;
}

bool Near(float a, float b, float eps)
{
    return std::fabs(a - b) <= eps;
}

// 生成物の「EntityID に依らない指紋」。**ワールドハッシュは使えない** —
// あちらは entity の index/generation を混ぜるので、Undo で消して Redo で作り直すと
// 中身が同じでも必ず変わる (UndoSelfTest の Phase 2 が構造と値で検証しているのと同じ理由)
struct PartFingerprint {
    LocalTransform lt{};
    PartBoundsComponent bounds{};
    LocalTransform shapeLt{};
    ColliderComponent col{};
    JointComponent joint{}; // connectedEntity は名前で見る (作り直しで ID が変わる)
    bool hasJoint = false;
    std::string connName;
    bool valid = false;
};

PartFingerprint Fingerprint(World& world, EntityID skin, const char* name)
{
    PartFingerprint f;
    const EntityID part = FindChild(world, skin, name);
    if (part.IsNull()) {
        return f;
    }
    const auto* lt = world.GetComponent<LocalTransform>(part);
    const auto* pb = world.GetComponent<PartBoundsComponent>(part);
    const EntityID shape = FindChild(world, part, (std::string(name) + "_Shape").c_str());
    const auto* slt = shape.IsNull() ? nullptr : world.GetComponent<LocalTransform>(shape);
    const auto* col = shape.IsNull() ? nullptr : world.GetComponent<ColliderComponent>(shape);
    if (!lt || !pb || !slt || !col) {
        return f;
    }
    f.lt = *lt;
    f.bounds = *pb;
    f.shapeLt = *slt;
    f.col = *col;
    if (const auto* jc = world.GetComponent<JointComponent>(part)) {
        f.hasJoint = true;
        f.joint = *jc;
        const char* cn = world.GetName(jc->connectedEntity);
        f.connName = cn ? cn : "";
    }
    f.valid = true;
    return f;
}

// ★**構造体まるごとの memcmp で比べてはいけない**。`bool` の後ろの詰め物は
//   ワールドハッシュにもシリアライズにも乗らない (どちらもフィールド単位) ので、
//   Undo が JSON から作り直したコンポーネントでは値が違いうる。
//   実際 Debug では通って Release だけ落ちた。**フィールドを 1 本ずつ見る**
bool SameCollider(const ColliderComponent& a, const ColliderComponent& b)
{
    return a.shape == b.shape && a.radius == b.radius && a.halfExtents.x == b.halfExtents.x
        && a.halfExtents.y == b.halfExtents.y && a.halfExtents.z == b.halfExtents.z
        && a.isTrigger == b.isTrigger && a.height == b.height && a.friction == b.friction
        && a.layer == b.layer && a.mask == b.mask;
}

bool SameJoint(const JointComponent& a, const JointComponent& b)
{
    return a.type == b.type && a.anchor.x == b.anchor.x && a.anchor.y == b.anchor.y
        && a.anchor.z == b.anchor.z && a.connectedAnchor.x == b.connectedAnchor.x
        && a.connectedAnchor.y == b.connectedAnchor.y
        && a.connectedAnchor.z == b.connectedAnchor.z && a.axis.x == b.axis.x
        && a.axis.y == b.axis.y && a.axis.z == b.axis.z && a.useLimit == b.useLimit
        && a.limitMin == b.limitMin && a.limitMax == b.limitMax
        && a.swingLimitDeg == b.swingLimitDeg && a.motorTargetVelocity == b.motorTargetVelocity
        && a.motorMaxForce == b.motorMaxForce && a.breakForce == b.breakForce
        && a.breakTorque == b.breakTorque && a.broken == b.broken
        && a.restRotation.x == b.restRotation.x && a.restRotation.y == b.restRotation.y
        && a.restRotation.z == b.restRotation.z && a.restRotation.w == b.restRotation.w;
}

bool SameBounds(const PartBoundsComponent& a, const PartBoundsComponent& b)
{
    return a.shape == b.shape && a.center.x == b.center.x && a.center.y == b.center.y
        && a.center.z == b.center.z && a.halfExtents.x == b.halfExtents.x
        && a.halfExtents.y == b.halfExtents.y && a.halfExtents.z == b.halfExtents.z;
}

bool SameFingerprint(const PartFingerprint& a, const PartFingerprint& b)
{
    return a.valid && b.valid && a.hasJoint == b.hasJoint && a.connName == b.connName
        && std::memcmp(&a.lt, &b.lt, sizeof(LocalTransform)) == 0 // 全部 float = 詰め物なし
        && std::memcmp(&a.shapeLt, &b.shapeLt, sizeof(LocalTransform)) == 0
        && SameBounds(a.bounds, b.bounds) && SameCollider(a.col, b.col)
        && (!a.hasJoint || SameJoint(a.joint, b.joint));
}

} // namespace

bool RunRagdollBuildSelfTest()
{
    MYE_LOG_INFO("==== Ragdoll builder self test ====");
    RegisterBuiltinComponents(); // sTypeId 解決 (冪等)
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    RenderResources res; // Init しない = GPU バッファを作らない
    const AssetID model = res.skinnedModels.Register("ragdoll_rig", MakeRigSkeleton());
    const SkinnedModel* skel = res.skinnedModels.Get(model);
    check(skel != nullptr && skel->joints.size() == kJointCount,
          "setup: the hand-built 9-joint rig registers headlessly");
    if (!skel) {
        MYE_LOG_ERROR("==== Ragdoll builder self test: FAIL ====");
        return false;
    }

    // ---- 選別: 長さ 0 の骨と無名の骨は部位にならない ----
    {
        const int n = ragdoll_build::CountParts(*skel, KeepShortBones());
        MYE_LOG_INFO("  [build] CountParts = %d (joints = %d)", n, kJointCount);
        check(n == kPartCount,
              "select: zero-length and unnamed joints get no part (a nub in the middle of a "
              "chain does not silently swallow the bone above it)");

        Scene s;
        GameObject skin = MakeSkin(s, model, -1, 0);
        const int built = ragdoll_build::Build(s, skin.Id(), *skel, KeepShortBones());
        check(built == n, "select: Build creates exactly as many parts as CountParts promised "
                          "(the menu item never lies about being enabled)");
        World& w = s.GetWorld();
        bool allFound = true;
        for (int i = 0; i < kPartCount; ++i) {
            const EntityID e = FindChild(w, skin.Id(), kPartNames[i]);
            allFound = allFound && !e.IsNull() && w.GetComponent<PartComponent>(e) != nullptr
                       && w.GetComponent<RigidbodyComponent>(e) != nullptr
                       && w.GetComponent<PartBoundsComponent>(e) != nullptr;
        }
        check(allFound, "select: every expected bone became a Part + PartBounds + Rigidbody child "
                        "named after the joint");
        check(FindChild(w, skin.Id(), "nub").IsNull() && CountChildren(w, skin.Id()) == kPartCount,
              "select: the zero-length nub and the unnamed joints produced no entity at all");
        const auto* rc = w.GetComponent<RagdollComponent>(skin.Id());
        check(rc != nullptr && !rc->active,
              "select: the SkinnedMesh gets a Ragdoll tag and it starts held (active=false)");

        // 部位を持たない骨を挟んだ先の関節は、部位を持つ最も近い祖先へ張られる
        const EntityID elbow = FindChild(w, skin.Id(), "elbowL");
        const EntityID hand = FindChild(w, skin.Id(), "handL");
        const auto* hj = hand.IsNull() ? nullptr : w.GetComponent<JointComponent>(hand);
        check(hj != nullptr && hj->type == 4 && hj->connectedEntity == elbow,
              "joints: a bone whose parent bone was skipped connects to the nearest ancestor "
              "that does have a part (the chain never breaks)");
        const EntityID root = FindChild(w, skin.Id(), "root");
        check(!root.IsNull() && w.GetComponent<JointComponent>(root) == nullptr,
              "joints: the ragdoll root carries no Joint (it is free to fly)");
    }

    // ---- (g2-1) 同じスケルトンから 2 回生成 → 全フィールドがビット同一 ----
    {
        Scene a, b;
        GameObject sa = MakeSkin(a, model, -1, 0);
        GameObject sb = MakeSkin(b, model, -1, 0);
        ragdoll_build::Build(a, sa.Id(), *skel, KeepShortBones());
        ragdoll_build::Build(b, sb.Id(), *skel, KeepShortBones());
        const uint64_t ha = HashWorld(a.GetWorld(), nullptr);
        const uint64_t hb = HashWorld(b.GetWorld(), nullptr);
        MYE_LOG_INFO("  [build] world hash %016llX / %016llX", static_cast<unsigned long long>(ha),
                     static_cast<unsigned long long>(hb));
        check(ha == hb, "determinism: two runs over the same skeleton produce bit-identical "
                        "hierarchies (names, transforms, colliders, joints, masses)");
    }

    // ---- 配置: 生成した部位のポーズは PartFollowSystem がバインドで書く値とビット一致 ----
    // ★これがズレていると、Play を押した最初の tick で部位がカクッと飛ぶ。
    //   生成器と追従システムが同じ `DecomposeRowMajorTRS` を通していることの機械証明
    {
        Scene s;
        GameObject skin = MakeSkin(s, model, -1, 0); // clip<0 = バインドポーズ
        ragdoll_build::Build(s, skin.Id(), *skel, KeepShortBones());
        World& w = s.GetWorld();
        std::vector<LocalTransform> before;
        for (int i = 0; i < kPartCount; ++i) {
            before.push_back(*w.GetComponent<LocalTransform>(FindChild(w, skin.Id(), kPartNames[i])));
        }
        PartFollowSystem pf;
        pf.Update(w, res);
        bool same = true;
        for (int i = 0; i < kPartCount; ++i) {
            const auto* lt = w.GetComponent<LocalTransform>(FindChild(w, skin.Id(), kPartNames[i]));
            same = same && lt && std::memcmp(lt, &before[i], sizeof(LocalTransform)) == 0;
        }
        check(same, "placement: the generated part poses are bit-identical to what "
                    "PartFollowSystem writes at bind pose (no jump when play starts)");
    }

    // ---- 形状: カプセルが骨をちょうど覆う ----
    // ★`ColliderComponent` は center も rotation も持たないので、この向きと位置は
    //   **子エンティティの LocalTransform** が担いでいる (M60e の複合コライダー)。
    //   端点の一方が関節ピボットに乗り、全長が骨長に一致することが「骨に乗った」の定義
    {
        Scene s;
        GameObject skin = MakeSkin(s, model, -1, 0);
        ragdoll_build::Build(s, skin.Id(), *skel, KeepShortBones());
        World& w = s.GetWorld();
        bool ok = true;
        bool compound = true;
        for (int i = 0; i < kPartCount; ++i) {
            const EntityID part = FindChild(w, skin.Id(), kPartNames[i]);
            const auto* rb = w.GetComponent<RigidbodyComponent>(part);
            compound = compound && rb && rb->compoundColliders && rb->mass > 0.0f
                       && w.GetComponent<ColliderComponent>(part) == nullptr;
            const EntityID shape = FindChild(w, part, (std::string(kPartNames[i]) + "_Shape").c_str());
            const auto* col = shape.IsNull() ? nullptr : w.GetComponent<ColliderComponent>(shape);
            const auto* lt = shape.IsNull() ? nullptr : w.GetComponent<LocalTransform>(shape);
            if (!col || !lt || col->shape != 2) {
                ok = false;
                continue;
            }
            // カプセルのローカル Y をエンティティの回転で回し、両端 (± 全高/2) を出す
            const XMVECTOR q = XMLoadFloat4(&lt->rotation);
            XMFLOAT3 ydir;
            XMStoreFloat3(&ydir, XMVector3Rotate(XMVectorSet(0, 1, 0, 0), q));
            const float h = col->height * 0.5f;
            const XMFLOAT3 e0 = { lt->position.x - ydir.x * h, lt->position.y - ydir.y * h,
                                  lt->position.z - ydir.z * h };
            const XMFLOAT3 e1 = { lt->position.x + ydir.x * h, lt->position.y + ydir.y * h,
                                  lt->position.z + ydir.z * h };
            const float d0 = std::sqrt(e0.x * e0.x + e0.y * e0.y + e0.z * e0.z);
            const float d1 = std::sqrt(e1.x * e1.x + e1.y * e1.y + e1.z * e1.z);
            const float near0 = (d0 < d1) ? d0 : d1;
            const float far0 = (d0 < d1) ? d1 : d0;
            // 両端は部位の原点から見て骨の上に一直線に並ぶ (= 軸が骨に乗っている)。
            // その差が全高、和が骨長、中点が骨の中央 — この 3 つで配置が一意に決まる
            const XMFLOAT3 cr = { e0.y * e1.z - e0.z * e1.y, e0.z * e1.x - e0.x * e1.z,
                                  e0.x * e1.y - e0.y * e1.x };
            const auto* pb = w.GetComponent<PartBoundsComponent>(part);
            const float boneLen = near0 + far0;
            const float bcen = pb ? std::sqrt(pb->center.x * pb->center.x
                                              + pb->center.y * pb->center.y
                                              + pb->center.z * pb->center.z)
                                  : -1.0f;
            ok = ok && Near(cr.x, 0.0f, 1e-5f) && Near(cr.y, 0.0f, 1e-5f)
                 && Near(cr.z, 0.0f, 1e-5f) && Near(far0 - near0, col->height, 1e-4f)
                 && Near(bcen, boneLen * 0.5f, 1e-4f) && near0 > 0.0f && col->radius > 0.0f
                 // 半径の下限に当たった短い骨は球へ潰れる (全高 == 直径)。**裏返らない**
                 // ことだけを要求する — 潰れること自体は仕様
                 && col->height >= 2.0f * col->radius - 1e-6f;
        }
        check(ok, "shape: each capsule sits on the bone axis, centred on the bone, with a gap at "
                  "both joints (the offset and the Y-to-bone rotation both landed)");
        check(compound, "shape: the part itself carries no collider and aggregates the child "
                        "capsule through Rigidbody.compoundColliders (M60e)");
    }

    // ---- (g2-2) 生成直後のシーンは従来どおりアニメで動く ----
    // active=false なので、120 tick 回しても部位はアニメの骨の位置から 1 ビットも動かない
    {
        Scene s;
        GameObject skin = MakeSkin(s, model, 0, 20);
        ragdoll_build::Build(s, skin.Id(), *skel, KeepShortBones());
        World& w = s.GetWorld();
        PartFollowSystem pf;
        PhysicsSystem phys;
        for (int i = 0; i < 120; ++i) {
            pf.Update(w, res);
            phys.Update(w, kDt);
        }
        std::vector<XMMATRIX> animLocals;
        ComputeJointLocals(*skel, 0, 20.0f / 60.0f, animLocals);
        bool held = true;
        for (int i = 0; i < kPartCount; ++i) {
            const int32_t ji = skel->FindJointByName(kPartNames[i]);
            XMFLOAT4X4 f;
            XMStoreFloat4x4(&f, JointGlobalFromLocals(*skel, animLocals, ji));
            const auto* lt = w.GetComponent<LocalTransform>(FindChild(w, skin.Id(), kPartNames[i]));
            held = held && lt && lt->position.x == f._41 && lt->position.y == f._42
                   && lt->position.z == f._43;
        }
        check(held, "held: a freshly generated ragdoll plays the animation exactly as before "
                    "(120 ticks of gravity move nothing)");
    }

    // ---- (g2-3) Undo 1 回で生成前へ完全に戻り、Redo で同一物が出る ----
    {
        Scene s;
        UndoStack undo;
        Selection sel;
        GameObject skin = MakeSkin(s, model, -1, 0);
        const uint64_t fid = s.EnsureFileId(skin.Id());
        const uint64_t before = HashWorld(s.GetWorld(), nullptr);

        undo.BeginRecord("Create Ragdoll", sel);
        undo.CaptureBefore(s, fid);
        ragdoll_build::Build(s, skin.Id(), *skel, KeepShortBones());
        s.GetWorld().ApplyStructuralChanges();
        sel.SelectOnly(fid);
        undo.CaptureAfter(s, fid);
        undo.EndRecord(sel);
        const uint64_t after = HashWorld(s.GetWorld(), nullptr);
        check(before != after, "undo: the generation actually changed the world (guard against a "
                               "vacuously passing round trip)");
        std::vector<PartFingerprint> fpAfter;
        for (int i = 0; i < kPartCount; ++i) {
            fpAfter.push_back(Fingerprint(s.GetWorld(), skin.Id(), kPartNames[i]));
        }

        undo.Undo(s, sel);
        s.GetWorld().ApplyStructuralChanges();
        check(HashWorld(s.GetWorld(), nullptr) == before,
              "undo: one Undo removes the whole ragdoll (the subtree diff of the SkinnedMesh "
              "catches every sibling the generator scattered)");
        undo.Redo(s, sel);
        s.GetWorld().ApplyStructuralChanges();
        // ★ここでワールドハッシュを比べてはいけない — あちらは entity の index/generation を
        //   混ぜるので、消して作り直した時点で必ず変わる。見るべきは中身
        bool redoSame = CountChildren(s.GetWorld(), skin.Id()) == kPartCount;
        for (int i = 0; i < kPartCount; ++i) {
            redoSame = redoSame
                && SameFingerprint(fpAfter[i], Fingerprint(s.GetWorld(), skin.Id(), kPartNames[i]));
        }
        check(redoSame, "undo: Redo restores every part with bit-identical transforms, capsules "
                        "and joints (the connections point at the same bones again)");
    }

    // ---- 足切り: 既定では最長骨に対して短すぎる骨を部位にしない ----
    {
        const int full = ragdoll_build::CountParts(*skel, KeepShortBones());
        const int trimmed = ragdoll_build::CountParts(*skel);
        MYE_LOG_INFO("  [build] parts %d -> %d with the default short-bone cut", full, trimmed);
        check(trimmed == full - 1,
              "select: the default settings drop the bone that is short relative to the rig "
              "(handL at 20% of the longest bone)");
        Scene s;
        GameObject skin = MakeSkin(s, model, -1, 0);
        ragdoll_build::Build(s, skin.Id(), *skel);
        World& w = s.GetWorld();
        check(FindChild(w, skin.Id(), "handL").IsNull()
                  && !FindChild(w, skin.Id(), "elbowL").IsNull(),
              "select: the dropped bone leaves no entity, and the bone above it still does");
    }

    // ---- (g2-4) 生成物を作動させると崩れて揃って眠る (g1 の試験を生成器の出力で再走) ----
    // ★**substeps は 16 を使う**。4 や 8 では「床に触れながら関節に吊られている」部位が
    //   微振動を続けて sleepTicks が伸びきらず、島の全員が静まるまで眠らない仕組みの
    //   ために**ラグドール全体が一生眠らない** (実測: substeps 4 で maxSleepTicks 4〜18 /
    //   8 で末端だけ 15 / 16 で全員 30 に到達して tick 395 に揃って入眠)。
    //   M59g2-7 の「剛性はサブステップで買える」がそのまま関節にも効いている
    {
        Scene s;
        {
            GameObject g = s.CreateGameObjectTracked("Floor");
            g.SetLocalPosition(0.0f, -0.5f, 0.0f);
            auto* col = g.AddComponent<ColliderComponent>();
            col->shape = 1;
            col->halfExtents = { 20.0f, 0.5f, 20.0f };
        }
        {
            GameObject e = s.CreateGameObjectTracked("Env");
            auto* env = e.AddComponent<PhysicsEnvironmentComponent>();
            env->substeps = 16;
            env->sleepDelayTicks = 30;
        }
        GameObject skin = MakeSkin(s, model, -1, 0);
        skin.SetLocalPosition(0.0f, 3.0f, 0.0f);
        const int built = ragdoll_build::Build(s, skin.Id(), *skel);
        World& w = s.GetWorld();
        std::vector<EntityID> parts;
        {
            const auto* h = w.GetComponent<HierarchyComponent>(skin.Id());
            EntityID c = h ? h->firstChild : kNullEntity;
            while (!c.IsNull()) {
                parts.push_back(c);
                const auto* ch = w.GetComponent<HierarchyComponent>(c);
                c = ch ? ch->nextSibling : kNullEntity;
            }
        }
        check(static_cast<int>(parts.size()) == built,
              "driven: every generated part is a direct child of the SkinnedMesh (the v1 rule "
              "the palette override depends on)");

        PartFollowSystem pf;
        PhysicsSystem phys;
        pf.Update(w, res); // アニメのポーズへ置いてから作動させる (実運用の手順)
        w.GetComponent<RagdollComponent>(skin.Id())->active = true;
        const float startY = w.GetComponent<LocalTransform>(parts[0])->position.y;

        std::vector<int> sleptAt(parts.size(), -1);
        for (int tick = 0; tick < 2000; ++tick) {
            pf.Update(w, res);
            phys.Update(w, kDt);
            int done = 0;
            for (size_t i = 0; i < parts.size(); ++i) {
                const auto* rb = w.GetComponent<RigidbodyComponent>(parts[i]);
                if (sleptAt[i] < 0 && rb && rb->isSleeping) {
                    sleptAt[i] = tick;
                }
                done += (sleptAt[i] >= 0) ? 1 : 0;
            }
            if (done == static_cast<int>(parts.size())) {
                break;
            }
        }
        bool allSlept = true;
        bool together = true;
        for (size_t i = 0; i < parts.size(); ++i) {
            allSlept = allSlept && sleptAt[i] >= 0;
            together = together && sleptAt[i] == sleptAt[0];
        }
        MYE_LOG_INFO("  [build] slept at tick %d (%d parts)", sleptAt[0],
                     static_cast<int>(parts.size()));
        check(allSlept, "driven: the generated ragdoll collapses onto the floor and falls asleep");
        check(together, "driven: every generated bone sleeps on the same tick (the Cone joints "
                        "the generator wrote are wired into the islands)");
        check(w.GetComponent<LocalTransform>(parts[0])->position.y < startY - 2.0f,
              "driven: it really fell (the root dropped from the pose it was authored in)");
        const auto* rootRb = w.GetComponent<RigidbodyComponent>(parts[0]);
        check(rootRb != nullptr && rootRb->velocity.y == 0.0f && rootRb->angularVelocity.y == 0.0f,
              "driven: a sleeping bone holds exactly zero velocity");
    }

    // ---- 可動域の可視化 (M60c の申し送り 9 をここで消化) ----
    // ★絵そのものは目で見るしかないが、**幾何は機械で測れる**: コーンの母線は
    //   「相手が担いでいる rest 軸」から swingLimitDeg だけ開いた向きに出ているはず。
    //   ここが軸とずれていると「枠は見えるのに関節はその外へ行く」嘘の絵になる
    {
        Scene s;
        World& w = s.GetWorld();
        GameObject b = s.CreateGameObjectTracked("B");
        GameObject a = s.CreateGameObjectTracked("A");
        auto* jc = a.AddComponent<JointComponent>();
        jc->type = 4; // Cone
        jc->connectedEntity = b.Id();
        jc->axis = { 0.0f, 1.0f, 0.0f };
        jc->swingLimitDeg = 30.0f;
        jc->limitMin = -20.0f;
        jc->limitMax = 20.0f;
        // WorldMatrix は TransformSystem を回さずに恒等で置く (可視化は行列しか読まない)
        for (GameObject g : { a, b }) {
            auto* wm = g.AddComponent<WorldMatrixComponent>();
            wm->value = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        }
        w.ApplyStructuralChanges();

        const std::vector<SolidContact> noContacts;
        PhysicsDebugFlags flags;
        flags.joints = true;
        std::vector<DebugLineCmd> off, on;
        jc = a.GetComponent<JointComponent>();
        jc->useLimit = false;
        BuildPhysicsDebugLines(w, noContacts, flags, off);
        jc->useLimit = true;
        BuildPhysicsDebugLines(w, noContacts, flags, on);
        check(on.size() > off.size(),
              "gizmo: turning the limit on adds lines, turning it off draws none (the picture "
              "follows the same gate the solver rows do)");

        // 母線 = アンカー (原点) から出ている線。軸との角が swingLimitDeg に一致すること
        int spokes = 0;
        int twistArms = 0;
        bool allOnCone = true;
        float maxLen = 0.0f;
        for (size_t i = off.size(); i < on.size(); ++i) {
            const DebugLineCmd& c = on[i];
            const float d = std::sqrt(c.bx * c.bx + c.by * c.by + c.bz * c.bz);
            if (d > maxLen) {
                maxLen = d;
            }
            if (c.ax != 0.0f || c.ay != 0.0f || c.az != 0.0f || d < 1e-6f) {
                continue; // 原点発でない = リムかツイスト円弧
            }
            // 軸は +Y。原点発の線は 2 種類ある —
            //   円錐の母線 (軸から swingLimitDeg 開く = cos 0.866) と
            //   ツイスト円弧の両端の腕 (軸に**垂直** = cos 0)。前者だけを数える
            const float cosang = c.by / d;
            if (cosang > 0.5f) {
                ++spokes;
                allOnCone = allOnCone && Near(cosang, std::cos(30.0f * 3.14159265f / 180.0f), 1e-3f);
            } else {
                ++twistArms;
            }
        }
        MYE_LOG_INFO("  [build] cone spokes = %d, twist arms = %d, max radius = %.3f", spokes,
                     twistArms, maxLen);
        check(spokes > 0 && allOnCone,
              "gizmo: every cone spoke leaves the anchor at exactly the swing limit angle from "
              "the rest axis");
        check(twistArms == 2, "gizmo: the twist range is drawn as an arc with an arm at each end");
        check(maxLen < 1.0f, "gizmo: the gizmo stays a fixed size (nothing runs off to infinity)");
    }

    // ---- (g2-5) 並走ハッシュ ----
    {
        Scene a, b;
        GameObject sa = MakeSkin(a, model, -1, 0);
        GameObject sb = MakeSkin(b, model, -1, 0);
        sa.SetLocalPosition(0.0f, 2.0f, 0.0f);
        sb.SetLocalPosition(0.0f, 2.0f, 0.0f);
        ragdoll_build::Build(a, sa.Id(), *skel);
        ragdoll_build::Build(b, sb.Id(), *skel);
        a.GetWorld().GetComponent<RagdollComponent>(sa.Id())->active = true;
        b.GetWorld().GetComponent<RagdollComponent>(sb.Id())->active = true;
        PartFollowSystem pfa, pfb;
        PhysicsSystem pa, pb;
        bool same = true;
        for (int i = 0; i < 120 && same; ++i) {
            pfa.Update(a.GetWorld(), res);
            pa.Update(a.GetWorld(), kDt);
            pfb.Update(b.GetWorld(), res);
            pb.Update(b.GetWorld(), kDt);
            same = HashWorld(a.GetWorld(), nullptr) == HashWorld(b.GetWorld(), nullptr);
        }
        check(same, "determinism: two generated ragdolls run bit-identical for 120 ticks");
    }

    if (failCount == 0) {
        MYE_LOG_INFO("==== Ragdoll builder self test: PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== Ragdoll builder self test: FAIL (%d) ====", failCount);
    return false;
}

} // namespace mye
