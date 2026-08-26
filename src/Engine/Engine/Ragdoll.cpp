#include "Engine/Engine/Ragdoll.h"

#include "Engine/Core/Components.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/Parts.h" // ResolvePartSource (物理 / PartFollow / Inspector と共用)
#include "Engine/Renderer/Skeleton.h"

using namespace DirectX;

namespace mye {
namespace ragdoll {

bool AnyRagdoll(World& world)
{
    bool any = false;
    const ComponentTypeId req[] = { RagdollComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        if (arch.Count() != 0) {
            any = true;
        }
    });
    return any;
}

bool IsSourceDriven(World& world, EntityID source)
{
    if (source.IsNull()) {
        return false;
    }
    const auto* rag = world.GetComponent<RagdollComponent>(source);
    return rag != nullptr && rag->active;
}

bool IsPartHeld(World& world, EntityID part)
{
    const auto* p = world.GetComponent<PartComponent>(part);
    if (!p || p->joint[0] == '\0') {
        return false; // 部位でない / 静的ソケット = ラグドールとは無関係
    }
    const EntityID src = Parts::ResolvePartSource(world, part, p->source);
    if (src.IsNull()) {
        return false;
    }
    const auto* rag = world.GetComponent<RagdollComponent>(src);
    // ★ラグドールが「有る」ことまで要求する — Ragdoll を持たないふつうの骨追従部位に
    //   剛体が載っているシーン (M48g 以来ありうる) の挙動を変えないため
    return rag != nullptr && !rag->active;
}

void BuildBonePalette(World& world, EntityID skinnedMesh, const SkinnedModel& model, int clip,
                      float timeSec, std::vector<XMFLOAT4X4>& out)
{
    const size_t n = model.joints.size();

    // 1) アニメのポーズ。部位を持たない骨 (指先など、剛体を作らなかったところ) は
    //    これを親から合成して埋める
    std::vector<XMMATRIX> locals;
    ComputeJointLocals(model, clip, timeSec, locals);

    // 2) 部位 → 骨の override を集める。**直子だけ**を見るのが M48g の v1 規約で、
    //    そこを緩めるとワールド行列 (= 前 tick の値) を読む必要が出てしまう
    std::vector<uint8_t> hasOverride(n, 0u);
    std::vector<XMMATRIX> overrides(n, XMMatrixIdentity());

    const auto* rootH = world.GetComponent<HierarchyComponent>(skinnedMesh);
    EntityID child = rootH ? rootH->firstChild : kNullEntity;
    while (!child.IsNull()) {
        const auto* p = world.GetComponent<PartComponent>(child);
        const auto* lt = world.GetComponent<LocalTransform>(child);
        if (p && lt && p->joint[0] != '\0') {
            const int32_t j = model.FindJointByName(p->joint);
            // 同じ骨に 2 つの部位が刺さっていたら**先勝ち** — 兄弟順に依らず答えを
            // 一意にするため (兄弟順はワールドハッシュに載っていないので、後勝ちにすると
            // 「ハッシュは同じなのに絵が違う」が起きうる)
            if (j >= 0 && static_cast<size_t>(j) < n && hasOverride[static_cast<size_t>(j)] == 0) {
                // partLocal = S * R * T (ComputeJointLocals と同じ積列) = jointGlobal
                const XMMATRIX m =
                    XMMatrixScaling(lt->scale.x, lt->scale.y, lt->scale.z)
                    * XMMatrixRotationQuaternion(XMLoadFloat4(&lt->rotation))
                    * XMMatrixTranslation(lt->position.x, lt->position.y, lt->position.z);
                overrides[static_cast<size_t>(j)] = m;
                hasOverride[static_cast<size_t>(j)] = 1u;
            }
        }
        const auto* h = world.GetComponent<HierarchyComponent>(child);
        child = h ? h->nextSibling : kNullEntity;
    }

    ComputeBonePaletteWithOverrides(model, locals, hasOverride, overrides, out);
}

} // namespace ragdoll
} // namespace mye
