// ============================================================================
//                          TwoBoneIkSystem.cpp
// ============================================================================
// 2 ボーン IK の解決段 (M89l)。TwoBoneIKComponent → SkinnedMesh.poseIk。
// ============================================================================
#include "Engine/Engine/Animation/TwoBoneIkSystem.h"

#include <cstring>
#include <string_view>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Core/Jobs/JobSystem.h"
#include "Engine/Renderer/Device/GpuResources.h"
#include "Engine/Renderer/Mesh/Skeleton.h"

namespace mye {

using namespace DirectX;

XMMATRIX LocalChainWorldMatrix(World& world, EntityID entity)
{
    XMMATRIX m = XMMatrixIdentity();
    for (EntityID cur = entity; !cur.IsNull(); cur = world.GetParent(cur)) {
        const LocalTransform* lt = world.GetComponent<LocalTransform>(cur);
        if (lt == nullptr) {
            break;
        }
        const XMMATRIX local = XMMatrixScaling(lt->scale.x, lt->scale.y, lt->scale.z)
                               * XMMatrixRotationQuaternion(XMLoadFloat4(&lt->rotation))
                               * XMMatrixTranslation(lt->position.x, lt->position.y, lt->position.z);
        m = XMMatrixMultiply(m, local);
    }
    return m;
}

namespace {

// 並列化の最小単位 (SkinnedMesh の数)。1 体ぶんは行列の逆と鎖数本で軽い
constexpr size_t kIkGrain = 64;

// 鎖の目標 (位置と回転) をメッシュのエンティティ空間で。目標のエンティティが無い / 消えていればワールドの値
void ResolveGoal(World& world, const TwoBoneIKComponent::Chain& chain, const XMMATRIX& worldToMesh,
                 SkinnedMeshComponent::PoseIkChain& out)
{
    XMMATRIX goal = XMMatrixRotationQuaternion(XMQuaternionNormalize(XMLoadFloat4(&chain.targetRotation)))
                    * XMMatrixTranslation(chain.targetPosition.x, chain.targetPosition.y, chain.targetPosition.z);
    if (!chain.target.IsNull() && world.IsAlive(chain.target)) {
        goal = XMMatrixMultiply(goal, LocalChainWorldMatrix(world, chain.target));
    }
    goal = XMMatrixMultiply(goal, worldToMesh);
    XMVECTOR scale, rotation, translation;
    if (!XMMatrixDecompose(&scale, &rotation, &translation, goal)) {
        // 拡大 0 の軸がある等で回転が取れない: 位置だけ使う
        rotation = XMQuaternionIdentity();
        translation = goal.r[3];
    }
    XMFLOAT3 t;
    XMFLOAT4 r;
    XMStoreFloat3(&t, translation);
    XMStoreFloat4(&r, rotation);
    out.target[0] = t.x;
    out.target[1] = t.y;
    out.target[2] = t.z;
    out.rotation[0] = r.x;
    out.rotation[1] = r.y;
    out.rotation[2] = r.z;
    out.rotation[3] = r.w;
}

} // namespace

void TwoBoneIkSystem::Update(World& world, const RenderResources& resources)
{
    // 段 1 (直列): 対象の収集。アーキタイプの走査は World の内部状態 (反復の深さ・クエリキャッシュ) を
    // 触るので並列段へ持ち込まない
    struct Item {
        SkinnedMeshComponent* sm;
        EntityID entity;
        uint32_t missingJointMask; // 先端ジョイントが見つからなかった鎖 (bit = 鎖の番号)。WARN は段 3 で出す
    };
    std::vector<Item> items;
    const ComponentTypeId req[] = { SkinnedMeshComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int si = arch.FindTypeIndex(SkinnedMeshComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            items.push_back({ static_cast<SkinnedMeshComponent*>(arch.GetPtr(si, row)), arch.EntityAt(row), 0u });
        }
    });

    // 段 2 (並列): 書くのは自分の SkinnedMesh.poseIk だけ。読むのは LocalTransform の連鎖と不変データ
    jobs::System().ParallelRanges(items.size(), kIkGrain, [&](size_t begin, size_t end) {
        for (size_t n = begin; n < end; ++n) {
            Item& item = items[n];
            SkinnedMeshComponent* sm = item.sm;
            const EntityID e = item.entity;
            // 書かない本数ぶんも既定値へ戻す (snapshot の生バイトを入力だけで決まる形にしておく)
            sm->poseIkCount = 0;
            for (SkinnedMeshComponent::PoseIkChain& c : sm->poseIk) {
                c = {};
            }
            sm->poseIkPelvisJoint = -1;
            sm->poseIkPelvisOffset[0] = sm->poseIkPelvisOffset[1] = sm->poseIkPelvisOffset[2] = 0.0f;
            const TwoBoneIKComponent* ik = world.GetComponent<TwoBoneIKComponent>(e);
            if (ik == nullptr) {
                continue;
            }
            // ラグドールの作動中は骨を物理が決める (IK で動かすと剛体と骨がずれる)
            if (const RagdollComponent* rag = world.GetComponent<RagdollComponent>(e); rag != nullptr && rag->active) {
                continue;
            }
            const SkinnedModel* model = resources.skinnedModels.Get(sm->model);
            if (model == nullptr) {
                continue;
            }
            const XMMATRIX worldToMesh = XMMatrixInverse(nullptr, LocalChainWorldMatrix(world, e));
            for (int32_t i = 0; i < kMaxTwoBoneIkChains; ++i) {
                const TwoBoneIKComponent::Chain& chain = ik->chains[i];
                if (chain.mode == twoboneikmode::kOff || chain.mode == twoboneikmode::kGround || !(chain.weight > 0.0f)) {
                    continue;
                }
                // 名前は 64 バイトの固定長。終端が無くても範囲の外は読まない
                const std::string_view name(chain.endJoint, strnlen(chain.endJoint, sizeof(chain.endJoint)));
                const int32_t joint = model->FindJointByName(name);
                if (joint < 0) {
                    item.missingJointMask |= 1u << i;
                    continue;
                }
                SkinnedMeshComponent::PoseIkChain& out = sm->poseIk[sm->poseIkCount++];
                out.endJoint = joint;
                out.useRotation = chain.mode == twoboneikmode::kPositionRotation ? 1 : 0;
                out.weight = chain.weight;
                ResolveGoal(world, chain, worldToMesh, out);
                const bool hasPole = chain.poleHint.x != 0.0f || chain.poleHint.y != 0.0f || chain.poleHint.z != 0.0f;
                out.hasPole = hasPole ? 1 : 0;
                out.pole[0] = chain.poleHint.x;
                out.pole[1] = chain.poleHint.y;
                out.pole[2] = chain.poleHint.z;
            }
        }
    });

    // 段 3 (直列): WARN と警告済み表は収集順 (= 従来の走査順) に反映する
    for (const Item& item : items) {
        if (item.missingJointMask == 0u) {
            continue;
        }
        const TwoBoneIKComponent* ik = world.GetComponent<TwoBoneIKComponent>(item.entity);
        for (int32_t i = 0; i < kMaxTwoBoneIkChains; ++i) {
            if ((item.missingJointMask & (1u << i)) == 0u) {
                continue;
            }
            if (warned_.insert((uint64_t(item.entity.index) << 8) | uint64_t(i)).second) {
                const char* endJoint = ik->chains[i].endJoint;
                const int nameLength = static_cast<int>(strnlen(endJoint, sizeof(ik->chains[i].endJoint)));
                MYE_LOG_WARN("[ik] '%s': chain %d end joint '%.*s' not found in the skinned model",
                             world.GetName(item.entity), i, nameLength, endJoint);
            }
        }
    }
    foot_.Update(world, resources);
}

} // namespace mye
