// ============================================================================
//                          FootIkSystem.cpp
// ============================================================================
// 足の接地 (M89m)。TwoBoneIK の kGround の鎖 → SkinnedMesh.poseIk / poseIkPelvis*。
// ============================================================================
#include "Engine/Engine/Animation/FootIkSystem.h"

#include <algorithm>
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
