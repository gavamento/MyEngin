// ============================================================================
//                          FootIkSystem.cpp
// ============================================================================
// 足の接地 (M89m)。TwoBoneIK の kGround の鎖 → SkinnedMesh.poseIk / poseIkPelvis*。
// 足首の向きを地面の傾きに揃える (M89q)。
// ============================================================================
#include "Engine/Engine/Animation/FootIkSystem.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Animation/SkinningSystem.h"
#include "Engine/Engine/Animation/TwoBoneIkSystem.h"
#include "Engine/Engine/Physics/Rigid/PhysicsSystem.h"
#include "Engine/Renderer/Device/GpuResources.h"
#include "Engine/Renderer/Mesh/Skeleton.h"

namespace mye {

using namespace DirectX;

namespace {

// ancestor が e 自身か e の祖先か
bool IsSelfOrAncestor(World& world, EntityID ancestor, EntityID e)
{
    for (EntityID cur = e; !cur.IsNull(); cur = world.GetParent(cur)) {
        if (cur == ancestor) {
            return true;
        }
    }
    return false;
}

// 自分の体 = メッシュの祖先 (CharacterController のカプセル等) と子孫 (骨に付いた部位) のコライダーは地面にしない
bool SkipOwnBody(World& world, EntityID entity, const void* user)
{
    const EntityID mesh = *static_cast<const EntityID*>(user);
    return IsSelfOrAncestor(world, entity, mesh) || IsSelfOrAncestor(world, mesh, entity);
}

std::string_view FixedName(const char (&name)[64])
{
    return std::string_view(name, strnlen(name, sizeof(name)));
}

// つま先が足首からこれより水平に離れていないと、足の前後の向きが決まらない (m、ワールド)
constexpr float kMinToeReach = 1e-3f;
// 法線の上向き成分がこれ以下の面 (壁・裏面) には足裏を揃えない
constexpr float kMinGroundUpY = 1e-4f;

// joint の子のうち index が最小のもの (足首ならつま先の側)。無ければ -1
int32_t FirstChildJoint(const SkinnedModel& model, int32_t joint)
{
    for (size_t j = 0; j < model.joints.size(); ++j) {
        if (model.joints[j].parent == joint) {
            return static_cast<int32_t>(j);
        }
    }
    return -1;
}

// 足裏を揃える向き (ワールド、長さは問わない、上向き成分は正)。heelHit は足首の真下の当たり。
// つま先 (足首の最初の子) の真下も当たれば、前後の傾きは 2 点の高さの差、左右の傾きは足首の下の面の法線から取る
// (段の縁をまたぐ足は、足首の下の面ではなく 2 点を結ぶ向きに寝る)。つま先が無い / 当たらない / 足首のほぼ真下なら
// 足首の下の面の法線だけ。最後に勾配を maxTilt で切る。揃えられない面 (壁・裏面) は false
bool FootGroundUp(World& world, const TwoBoneIKComponent& ik, EntityID meshEntity, const SkinnedModel& model,
                  const std::vector<XMMATRIX>& locals, const XMMATRIX& meshWorld, int32_t ankleJoint, XMVECTOR ankle,
                  const MyeRaycastHit& heelHit, float rayOriginY, float rayLength, XMFLOAT3& outUp)
{
    const MyeVec3 n = heelHit.normal;
    if (!(n.y > kMinGroundUpY)) {
        return false;
    }
    XMFLOAT3 up = { n.x, n.y, n.z };
    if (const int32_t toeJoint = FirstChildJoint(model, ankleJoint); toeJoint >= 0) {
        const XMVECTOR toe = XMMatrixMultiply(JointGlobalFromLocals(model, locals, toeJoint), meshWorld).r[3];
        const float fx = XMVectorGetX(toe) - XMVectorGetX(ankle);
        const float fz = XMVectorGetZ(toe) - XMVectorGetZ(ankle);
        MyeRaycastHit toeHit = {};
        if (fx * fx + fz * fz > kMinToeReach * kMinToeReach
            && RaycastWorld(world, { XMVectorGetX(toe), rayOriginY, XMVectorGetZ(toe) }, { 0.0f, -1.0f, 0.0f },
                            rayLength, &toeHit, ik.groundLayerMask, SkipOwnBody, &meshEntity) != 0) {
            // 前後 = かかと → つま先の当たり。横 = 上 × 前後の水平成分 (fz, 0, -fx) を、足首の下の面に沿わせたもの
            const XMVECTOR forward = XMVectorSet(fx, toeHit.point.y - heelHit.point.y, fz, 0.0f);
            const XMVECTOR side = XMVectorSet(fz, -(n.x * fz - n.z * fx) / n.y, -fx, 0.0f);
            // 上向き成分は fx² + fz² なので必ず正
            XMStoreFloat3(&up, XMVector3Cross(forward, side));
        }
    }
    const float maxTilt = ik.footMaxTilt;
    const float tiltSq = (up.x * up.x + up.z * up.z) / (up.y * up.y);
    if (tiltSq > maxTilt * maxTilt) {
        const float keep = maxTilt / std::sqrt(tiltSq);
        up.x *= keep;
        up.z *= keep;
    }
    outUp = up;
    return true;
}

} // namespace

void FootIkSystem::Update(World& world, const RenderResources& resources)
{
    const ComponentTypeId req[] = { SkinnedMeshComponent::sTypeId, TwoBoneIKComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int si = arch.FindTypeIndex(SkinnedMeshComponent::sTypeId);
        const int ii = arch.FindTypeIndex(TwoBoneIKComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            auto* sm = static_cast<SkinnedMeshComponent*>(arch.GetPtr(si, row));
            const auto* ik = static_cast<const TwoBoneIKComponent*>(arch.GetPtr(ii, row));
            const EntityID e = arch.EntityAt(row);
            bool anyGround = false;
            for (const TwoBoneIKComponent::Chain& chain : ik->chains) {
                anyGround = anyGround || (chain.mode == twoboneikmode::kGround && chain.weight > 0.0f);
            }
            if (!anyGround) {
                continue;
            }
            if (const RagdollComponent* rag = world.GetComponent<RagdollComponent>(e); rag != nullptr && rag->active) {
                continue;
            }
            const SkinnedModel* model = resources.skinnedModels.Get(sm->model);
            if (model == nullptr) {
                continue;
            }

            // アニメだけのポーズ (他の鎖・骨盤のずらしを入れる前) で足首の位置を引く
            SkinnedMeshComponent animOnly = *sm;
            animOnly.poseIkCount = 0;
            animOnly.poseIkPelvisJoint = -1;
            std::vector<XMMATRIX> locals;
            SampleSkinnedLocals(*model, animOnly, locals);

            const XMMATRIX meshWorld = LocalChainWorldMatrix(world, e);
            const XMMATRIX worldToMesh = XMMatrixInverse(nullptr, meshWorld);
            const float baseY = XMVectorGetY(meshWorld.r[3]);
            const float probe = std::max(ik->groundProbe, 0.0f);

            struct Placed {
                int32_t chain = 0;
                int32_t joint = -1;
                XMFLOAT3 target = {};
                bool alignRotation = false;                        // true: 足首の回転を rotation にする
                XMFLOAT4 rotation = { 0.0f, 0.0f, 0.0f, 1.0f };    // メッシュの空間
            };
            Placed placed[kMaxTwoBoneIkChains];
            int32_t placedCount = 0;
            float lowest = 0.0f; // 足元の面から見た、置いた足の地面の最も低い高さ (0 より上は骨盤に効かない)
            for (int32_t i = 0; i < kMaxTwoBoneIkChains; ++i) {
                const TwoBoneIKComponent::Chain& chain = ik->chains[i];
                if (chain.mode != twoboneikmode::kGround || !(chain.weight > 0.0f)) {
                    continue;
                }
                const std::string_view name = FixedName(chain.endJoint);
                const int32_t joint = model->FindJointByName(name);
                if (joint < 0) {
                    if (warned_.insert((uint64_t(e.index) << 8) | uint64_t(i)).second) {
                        MYE_LOG_WARN("[ik] '%s': ground chain %d end joint '%.*s' not found in the skinned model",
                                     world.GetName(e), i, static_cast<int>(name.size()), name.data());
                    }
                    continue;
                }
                const XMVECTOR ankle =
                    XMMatrixMultiply(JointGlobalFromLocals(*model, locals, joint), meshWorld).r[3];
                const MyeVec3 origin = { XMVectorGetX(ankle), baseY + probe, XMVectorGetZ(ankle) };
                MyeRaycastHit hit = {};
                if (probe <= 0.0f
                    || RaycastWorld(world, origin, { 0.0f, -1.0f, 0.0f }, 2.0f * probe, &hit, ik->groundLayerMask,
                                    SkipOwnBody, &e) == 0) {
                    continue; // 地面が見つからない足はアニメのまま
                }
                const float ground = hit.point.y - baseY;
                lowest = std::min(lowest, ground);
                Placed& p = placed[placedCount++];
                p.chain = i;
                p.joint = joint;
                XMStoreFloat3(&p.target, XMVectorAdd(ankle, XMVectorSet(0.0f, ground, 0.0f, 0.0f)));

                // 足首の向き (M89q): アニメだけのポーズの回転を、上向きが地面の向きになるだけ傾ける。
                // ソルバは先端の回転を置き換えるので、アニメの回転に傾きを掛けた値を渡す (つま先の振りはアニメのまま残る)
                XMFLOAT3 groundUp = {};
                XMVECTOR animScale = XMVectorZero();
                XMVECTOR animRotation = XMQuaternionIdentity();
                XMVECTOR animTranslation = XMVectorZero();
                if (ik->footMaxTilt > 0.0f
                    && FootGroundUp(world, *ik, e, *model, locals, meshWorld, joint, ankle, hit, baseY + probe,
                                    2.0f * probe, groundUp)
                    && XMMatrixDecompose(&animScale, &animRotation, &animTranslation,
                                         JointGlobalFromLocals(*model, locals, joint))) {
                    const XMVECTOR tilt =
                        ShortestArc(XMVector3TransformNormal(XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f), worldToMesh),
                                    XMVector3TransformNormal(XMLoadFloat3(&groundUp), worldToMesh));
                    p.alignRotation = true;
                    XMStoreFloat4(&p.rotation, XMQuaternionNormalize(XMQuaternionMultiply(animRotation, tilt)));
                }
            }
            if (placedCount == 0) {
                continue;
            }

            const float drop = std::min(-lowest, std::max(ik->pelvisMaxDrop, 0.0f));
            if (drop > 0.0f) {
                const std::string_view pelvisName = FixedName(ik->pelvisJoint);
                const int32_t pelvis = pelvisName.empty() ? FindRootJoint(*model) : model->FindJointByName(pelvisName);
                if (pelvis >= 0) {
                    XMFLOAT3 offset;
                    XMStoreFloat3(&offset, XMVector3TransformNormal(XMVectorSet(0.0f, -drop, 0.0f, 0.0f), worldToMesh));
                    sm->poseIkPelvisJoint = pelvis;
                    sm->poseIkPelvisOffset[0] = offset.x;
                    sm->poseIkPelvisOffset[1] = offset.y;
                    sm->poseIkPelvisOffset[2] = offset.z;
                } else if (warned_.insert((uint64_t(e.index) << 8) | 0xFFu).second) {
                    MYE_LOG_WARN("[ik] '%s': pelvis joint '%.*s' not found in the skinned model", world.GetName(e),
                                 static_cast<int>(pelvisName.size()), pelvisName.data());
                }
            }
            for (int32_t k = 0; k < placedCount && sm->poseIkCount < SkinnedMeshComponent::kMaxPoseIkChains; ++k) {
                const TwoBoneIKComponent::Chain& chain = ik->chains[placed[k].chain];
                SkinnedMeshComponent::PoseIkChain& out = sm->poseIk[sm->poseIkCount++];
                out.endJoint = placed[k].joint;
                out.weight = chain.weight;
                XMFLOAT3 t;
                XMStoreFloat3(&t, XMVector3TransformCoord(XMLoadFloat3(&placed[k].target), worldToMesh));
                out.target[0] = t.x;
                out.target[1] = t.y;
                out.target[2] = t.z;
                if (placed[k].alignRotation) {
                    out.useRotation = 1;
                    out.rotation[0] = placed[k].rotation.x;
                    out.rotation[1] = placed[k].rotation.y;
                    out.rotation[2] = placed[k].rotation.z;
                    out.rotation[3] = placed[k].rotation.w;
                }
                const bool hasPole = chain.poleHint.x != 0.0f || chain.poleHint.y != 0.0f || chain.poleHint.z != 0.0f;
                out.hasPole = hasPole ? 1 : 0;
                out.pole[0] = chain.poleHint.x;
                out.pole[1] = chain.poleHint.y;
                out.pole[2] = chain.poleHint.z;
            }
        }
    });
}

} // namespace mye
