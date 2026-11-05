#include "Editor/SelfTest/TwoBoneIkAutoSetupSelfTest.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <DirectXMath.h>

#include "Editor/Scene/Selection.h"
#include "Editor/Scene/TwoBoneIkAutoSetup.h"
#include "Editor/Undo/UndoStack.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Animation/TwoBoneIkSystem.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Renderer/Device/GpuResources.h"
#include "Engine/Renderer/Mesh/Skeleton.h"

using namespace DirectX;

namespace mye {
namespace {

struct JointSpec {
    const char* name;
    int32_t parent;
    XMFLOAT3 t;
};

SkinnedModel MakeModel(const std::vector<JointSpec>& specs)
{
    SkinnedModel m;
    for (const JointSpec& s : specs) {
        SkeletonJoint j;
        j.parent = s.parent;
        j.name = s.name;
        j.bindT = s.t;
        m.joints.push_back(j);
    }
    return m;
}

// Mixamo 風 (接頭辞 "mixamorig:"、Left / Right が前)。指 (LeftHandIndex1) とつま先が手首・足首の子に居る。
// 左前腕にはバインド回転を入れ、目標の回転が恒等で済んでしまう試験にしない
//   期待: 左手 = 4、右手 = 8、左足 = 11、右足 = 15
SkinnedModel MakeMixamoRig()
{
    SkinnedModel m = MakeModel({
        { "mixamorig:Hips", -1, { 0.0f, 1.0f, 0.0f } },              // 0
        { "mixamorig:Spine", 0, { 0.0f, 0.3f, 0.0f } },              // 1
        { "mixamorig:LeftArm", 1, { -0.2f, 0.3f, 0.0f } },           // 2
        { "mixamorig:LeftForeArm", 2, { -0.3f, 0.0f, 0.0f } },       // 3
        { "mixamorig:LeftHand", 3, { -0.25f, 0.0f, 0.0f } },         // 4
        { "mixamorig:LeftHandIndex1", 4, { -0.08f, 0.0f, 0.0f } },   // 5
        { "mixamorig:RightArm", 1, { 0.2f, 0.3f, 0.0f } },           // 6
        { "mixamorig:RightForeArm", 6, { 0.3f, 0.0f, 0.0f } },       // 7
        { "mixamorig:RightHand", 7, { 0.25f, 0.0f, 0.0f } },         // 8
        { "mixamorig:LeftUpLeg", 0, { -0.1f, -0.05f, 0.0f } },       // 9
        { "mixamorig:LeftLeg", 9, { 0.0f, -0.45f, 0.0f } },          // 10
        { "mixamorig:LeftFoot", 10, { 0.0f, -0.42f, 0.0f } },        // 11
        { "mixamorig:LeftToeBase", 11, { 0.0f, -0.05f, 0.12f } },    // 12
        { "mixamorig:RightUpLeg", 0, { 0.1f, -0.05f, 0.0f } },       // 13
        { "mixamorig:RightLeg", 13, { 0.0f, -0.45f, 0.0f } },        // 14
        { "mixamorig:RightFoot", 14, { 0.0f, -0.42f, 0.0f } },       // 15
    });
    XMStoreFloat4(&m.joints[3].bindR, XMQuaternionRotationAxis(XMVectorSet(0, 0, 1, 0), 0.6f));
    return m;
}

// 書き方が混ざったリグ。拾うべき形と、拾ってはいけない囮を同じスケルトンに置く
//   - 左手 "Hand_L" (深さ 3) と、その子 "Hand_L_02" (深さ 4、名前は有効) → 浅い方 (3)
//   - "Hand_L_end" は末端の語で除外、"ik_hand_r" (ルート直下の IK 補助、深さ 2) は ik の語で除外
//   - 右手 "wristR" (小→大の切り替わりで wrist / r)、左足 "Foot.L"、右足 "AnkleR"
//   - "foot_r" はルートの直子 (深さ 1) なので鎖にならない → 右足は深い方の AnkleR
//   期待: 左手 = 3、右手 = 8、左足 = 11、右足 = 14
SkinnedModel MakeMixedRig()
{
    return MakeModel({
        { "root", -1, { 0.0f, 0.0f, 0.0f } },          // 0
        { "UpperArm_L", 0, { -0.2f, 1.4f, 0.0f } },    // 1
        { "Forearm_L", 1, { -0.3f, 0.0f, 0.0f } },     // 2
        { "Hand_L", 2, { -0.25f, 0.0f, 0.0f } },       // 3
        { "Hand_L_02", 3, { -0.05f, 0.0f, 0.0f } },    // 4
        { "Hand_L_end", 4, { -0.05f, 0.0f, 0.0f } },   // 5
        { "upperArmR", 0, { 0.2f, 1.4f, 0.0f } },      // 6
        { "lowerArmR", 6, { 0.3f, 0.0f, 0.0f } },      // 7
        { "wristR", 7, { 0.25f, 0.0f, 0.0f } },        // 8
        { "Thigh.L", 0, { -0.1f, 0.9f, 0.0f } },       // 9
        { "Shin.L", 9, { 0.0f, -0.45f, 0.0f } },       // 10
        { "Foot.L", 10, { 0.0f, -0.42f, 0.0f } },      // 11
        { "ThighR", 0, { 0.1f, 0.9f, 0.0f } },         // 12
        { "KneeR", 12, { 0.0f, -0.45f, 0.0f } },       // 13
        { "AnkleR", 13, { 0.0f, -0.42f, 0.0f } },      // 14
        { "foot_r", 0, { 0.1f, 0.0f, 0.0f } },         // 15
        { "ik_hand_root", 0, { 0.0f, 0.0f, 0.0f } },   // 16
        { "ik_hand_r", 16, { 0.5f, 1.4f, 0.0f } },     // 17
    });
}

// CesiumMan 型: 左右の手がかりはあるが手・足の語が無い → 何も拾わない (形から推し量らない)
SkinnedModel MakeUnnamedRig()
{
    return MakeModel({
        { "Skeleton_torso_joint_1", -1, { 0.0f, 1.0f, 0.0f } },
        { "Skeleton_arm_joint_L__2_", 0, { -0.2f, 0.3f, 0.0f } },
        { "Skeleton_arm_joint_L__3_", 1, { -0.3f, 0.0f, 0.0f } },
        { "Skeleton_arm_joint_L__4_", 2, { -0.25f, 0.0f, 0.0f } },
        { "leg_joint_L_1", 0, { -0.1f, -0.05f, 0.0f } },
        { "leg_joint_L_2", 4, { 0.0f, -0.45f, 0.0f } },
        { "leg_joint_L_5", 5, { 0.0f, -0.42f, 0.0f } },
    });
}

bool Near3(const XMFLOAT3& a, const XMFLOAT3& b, float eps)
{
    return std::fabs(a.x - b.x) <= eps && std::fabs(a.y - b.y) <= eps && std::fabs(a.z - b.z) <= eps;
}

bool SameJoints(const IkLimbJoints& got, int32_t hl, int32_t hr, int32_t fl, int32_t fr)
{
    return got.joint[0] == hl && got.joint[1] == hr && got.joint[2] == fl && got.joint[3] == fr;
}

// SkinnedMesh を、平行移動・回転・拡大の入ったエンティティに付ける。
// 恒等の変換だと「目標がメッシュの空間で手首に重なる」の確認が空振りする
GameObject MakeSkin(Scene& s, AssetID model)
{
    GameObject skin = s.CreateGameObjectTracked("Skin");
    auto* sm = skin.AddComponent<SkinnedMeshComponent>();
    sm->model = model;
    sm->clip = -1;
    sm->playing = false;
    if (auto* lt = skin.GetComponent<LocalTransform>()) {
        lt->position = { 3.0f, 0.5f, -2.0f };
        XMStoreFloat4(&lt->rotation, XMQuaternionRotationAxis(XMVectorSet(0, 1, 0, 0), 0.8f));
        lt->scale = { 1.5f, 1.5f, 1.5f };
    }
    s.GetWorld().ApplyStructuralChanges();
    return skin;
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

bool ChainIs(const TwoBoneIKComponent::Chain& c, const char* joint, int32_t mode, float weight)
{
    return std::strcmp(c.endJoint, joint) == 0 && c.mode == mode && c.weight == weight;
}

} // namespace

bool RunTwoBoneIkAutoSetupSelfTest()
{
    MYE_LOG_INFO("==== TwoBoneIK auto setup self test ====");
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

    // ---- 骨名の判定 ----
    {
        const IkLimbJoints mixamo = FindIkLimbJoints(MakeMixamoRig());
        check(SameJoints(mixamo, 4, 8, 11, 15),
              "find: mixamorig:LeftHand / RightHand / LeftFoot / RightFoot (prefix dropped, fingers and toes skipped)");
        const IkLimbJoints mixed = FindIkLimbJoints(MakeMixedRig());
        MYE_LOG_INFO("  [find] mixed rig -> %d %d %d %d", mixed.joint[0], mixed.joint[1], mixed.joint[2],
                     mixed.joint[3]);
        check(SameJoints(mixed, 3, 8, 11, 14),
              "find: Hand_L / wristR / Foot.L / AnkleR; the shallower Hand_L beats Hand_L_02, "
              "ik_hand_r and Hand_L_end are excluded, the depth-1 foot_r cannot form a chain");
        const IkLimbJoints none = FindIkLimbJoints(MakeUnnamedRig());
        check(SameJoints(none, -1, -1, -1, -1), "find: joints without hand / foot words are left unset");
    }

    RenderResources res; // Init しない = GPU バッファを作らない
    const AssetID modelId = res.skinnedModels.Register("ik_auto_mixamo", MakeMixamoRig());
    const SkinnedModel* model = res.skinnedModels.Get(modelId);
    if (model == nullptr) {
        MYE_LOG_ERROR("==== TwoBoneIK auto setup self test: FAIL (setup) ====");
        return false;
    }

    // ---- 空の TwoBoneIK を埋める ----
    {
        Scene s;
        World& w = s.GetWorld();
        GameObject skin = MakeSkin(s, modelId);
        skin.AddComponent<TwoBoneIKComponent>();
        w.ApplyStructuralChanges();
        const TwoBoneIkAutoSetupResult r = ApplyTwoBoneIkAutoSetup(s, skin.Id(), *model);
        check(r.filledMask == 0xFu && r.missingMask == 0u && r.createdTargets == 2,
              "apply: all four chains filled, two hand targets created");
        const auto* ik = w.GetComponent<TwoBoneIKComponent>(skin.Id());
        check(ik != nullptr && ChainIs(ik->chains[2], "mixamorig:LeftFoot", twoboneikmode::kGround, 1.0f)
                  && ChainIs(ik->chains[3], "mixamorig:RightFoot", twoboneikmode::kGround, 1.0f)
                  && ik->chains[2].target.IsNull() && ik->chains[3].target.IsNull(),
              "apply: feet = ground with weight 1 and no target (works as soon as it is added)");
        check(ik != nullptr && ChainIs(ik->chains[0], "mixamorig:LeftHand", twoboneikmode::kPosition, 0.0f)
                  && ChainIs(ik->chains[1], "mixamorig:RightHand", twoboneikmode::kPosition, 0.0f),
              "apply: hands = position with weight 0 (standby; the animation is untouched)");
        bool targetsOk = ik != nullptr;
        for (int k = 0; targetsOk && k < 2; ++k) {
            const EntityID t = ik->chains[k].target;
            const auto* lt = t.IsNull() ? nullptr : w.GetComponent<LocalTransform>(t);
            XMFLOAT3 bind;
            XMStoreFloat3(&bind, ComputeJointGlobal(*model, -1, 0.0f, k == 0 ? 4 : 8).r[3]);
            targetsOk = lt != nullptr && w.GetParent(t) == skin.Id() && Near3(lt->position, bind, 1e-5f)
                        && std::strcmp(w.GetName(t), k == 0 ? "IK Target Hand L" : "IK Target Hand R") == 0;
        }
        check(targetsOk, "apply: each hand target is a named direct child placed on the wrist's bind pose");

        // 目標を解決段に通すと、メッシュの空間で手首の位置そのものになる (重みを上げた後の見え方の担保)
        if (auto* editable = w.GetComponent<TwoBoneIKComponent>(skin.Id())) {
            editable->chains[0].weight = 1.0f;
        }
        TwoBoneIkSystem ikSystem;
        ikSystem.Update(w, res);
        const auto* sm = w.GetComponent<SkinnedMeshComponent>(skin.Id());
        XMFLOAT3 wrist;
        XMStoreFloat3(&wrist, ComputeJointGlobal(*model, -1, 0.0f, 4).r[3]);
        check(sm != nullptr && sm->poseIkCount >= 1 && sm->poseIk[0].endJoint == 4
                  && Near3({ sm->poseIk[0].target[0], sm->poseIk[0].target[1], sm->poseIk[0].target[2] }, wrist, 1e-4f),
              "apply: with weight 1 the resolved goal sits on the bind wrist in mesh space (scaled / rotated entity)");

        const int children = CountChildren(w, skin.Id());
        const TwoBoneIkAutoSetupResult again = ApplyTwoBoneIkAutoSetup(s, skin.Id(), *model);
        check(again.filledMask == 0u && again.createdTargets == 0 && CountChildren(w, skin.Id()) == children,
              "apply: running again changes nothing and creates no extra targets");
    }

    // ---- 手で入れた鎖は残す ----
    {
        Scene s;
        World& w = s.GetWorld();
        GameObject skin = MakeSkin(s, modelId);
        skin.AddComponent<TwoBoneIKComponent>();
        w.ApplyStructuralChanges();
        if (auto* ik = w.GetComponent<TwoBoneIKComponent>(skin.Id())) {
            std::snprintf(ik->chains[0].endJoint, sizeof(ik->chains[0].endJoint), "%s", "mixamorig:LeftForeArm");
            ik->chains[0].mode = twoboneikmode::kPositionRotation;
            // 右足を鎖 1 に入れてある (鎖 3 は空)。同じ骨を鎖 3 にも入れて二重に解かない
            std::snprintf(ik->chains[1].endJoint, sizeof(ik->chains[1].endJoint), "%s", "mixamorig:RightFoot");
            ik->chains[1].mode = twoboneikmode::kGround;
        }
        const TwoBoneIkAutoSetupResult r = ApplyTwoBoneIkAutoSetup(s, skin.Id(), *model);
        const auto* ik = w.GetComponent<TwoBoneIKComponent>(skin.Id());
        check(r.filledMask == (1u << 2) && r.createdTargets == 0 && ik != nullptr
                  && ChainIs(ik->chains[0], "mixamorig:LeftForeArm", twoboneikmode::kPositionRotation, 1.0f)
                  && ChainIs(ik->chains[1], "mixamorig:RightFoot", twoboneikmode::kGround, 1.0f)
                  && ik->chains[3].endJoint[0] == '\0' && CountChildren(w, skin.Id()) == 0,
              "apply: hand-set chains are kept and a joint already used by another chain is not filled twice");
    }

    // ---- Add Component と同じ囲み方で、Undo 1 回で目標の子ごと戻る ----
    {
        Scene s;
        World& w = s.GetWorld();
        UndoStack undo;
        Selection sel;
        GameObject skin = MakeSkin(s, modelId);
        const uint64_t fid = s.EnsureFileId(skin.Id());
        const uint64_t before = HashWorld(w);
        undo.BeginRecord("Add Component", sel);
        undo.CaptureBefore(s, fid);
        w.AddComponentRaw(skin.Id(), TwoBoneIKComponent::sTypeId);
        w.ApplyStructuralChanges();
        ApplyTwoBoneIkAutoSetup(s, skin.Id(), *model);
        undo.CaptureAfter(s, fid);
        undo.EndRecord(sel);
        check(CountChildren(w, skin.Id()) == 2 && HashWorld(w) != before,
              "undo: the add + auto setup changed the world (guard against a vacuous round trip)");
        undo.Undo(s, sel);
        w.ApplyStructuralChanges();
        check(HashWorld(w) == before && CountChildren(w, skin.Id()) == 0
                  && w.GetComponent<TwoBoneIKComponent>(skin.Id()) == nullptr,
              "undo: one Undo removes the component and both target children");
        undo.Redo(s, sel);
        w.ApplyStructuralChanges();
        const auto* ik = w.GetComponent<TwoBoneIKComponent>(skin.Id());
        check(ik != nullptr && CountChildren(w, skin.Id()) == 2 && !ik->chains[0].target.IsNull()
                  && w.GetParent(ik->chains[0].target) == skin.Id(),
              "undo: Redo brings back the chains with the target reference pointing at the recreated child");
    }

    if (failCount == 0) {
        MYE_LOG_INFO("==== TwoBoneIK auto setup self test: PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== TwoBoneIK auto setup self test: FAIL (%d) ====", failCount);
    return false;
}

} // namespace mye
