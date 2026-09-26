//====================================================================================
//                          FractureBuilder.cpp
//  MyEngin/ 秋田蓮音                                                     09/25/2026
//                                          破片エンティティの事前生成の実装
//====================================================================================
#include "Engine/Engine/FractureBuilder.h"

#include <cstdio>
#include <vector>

#include "Engine/Core/Components.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/EntityNaming.h"
#include "Engine/Engine/FractureSystem.h" // FracturePieceIndicesMatchAsset (判定の共有)
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/Physics/FractureLibrary.h"

namespace mye {
namespace {

// root の直子のうち FracturePiece.root == root なものを集める。
// out が非 null なら見つけた (エンティティ, index) を積む
void CollectExistingPieces(World& world, EntityID root, std::vector<EntityID>* outEntities)
{
    const auto* rootH = world.GetComponent<HierarchyComponent>(root);
    for (EntityID c = rootH ? rootH->firstChild : kNullEntity; !c.IsNull();) {
        const auto* fp = world.GetComponent<FracturePieceComponent>(c);
        const auto* ch = world.GetComponent<HierarchyComponent>(c);
        const EntityID next = ch ? ch->nextSibling : kNullEntity;
        if (fp != nullptr && fp->root == root && outEntities != nullptr) {
            outEntities->push_back(c);
        }
        c = next;
    }
}

} // namespace

int BuildFracturePieces(World& world, EntityID root, const FractureAssetHandle& asset)
{
    if (root.IsNull() || !world.IsAlive(root)) {
        return 0;
    }

    // 再生成: 既存の破片を先に消す (DestroyEntity は tick 末適用なので即座に反映させる)
    {
        std::vector<EntityID> existing;
        CollectExistingPieces(world, root, &existing);
        for (EntityID e : existing) {
            world.DestroyEntity(e);
        }
        if (!existing.empty()) {
            world.ApplyStructuralChanges();
        }
    }

    if (asset.pieces.empty()) {
        return 0;
    }

    // root に Rigidbody が無ければ付ける。既にあれば compoundColliders だけ立てる
    if (world.GetComponent<RigidbodyComponent>(root) == nullptr) {
        world.AddComponent<RigidbodyComponent>(root);
    }
    // M80j: スキン破壊のルートは常に kinematic (骨アニメが動かす。物理には動かさせない)
    const bool isSkinRoot = world.GetComponent<SkinnedMeshComponent>(root) != nullptr;
    if (auto* rb = world.GetComponent<RigidbodyComponent>(root)) {
        rb->compoundColliders = true;
        if (isSkinRoot) {
            rb->isKinematic = true;
        }
    }
    // root 自身の Collider は外す — 複合の子形状と二重に当たらないようにする
    if (world.GetComponent<ColliderComponent>(root) != nullptr) {
        MYE_LOG_WARN("[fracture] %s: removing the root's own Collider (fracture pieces replace it "
                     "as a compound)",
                     world.GetName(root));
        world.RemoveComponent<ColliderComponent>(root);
    }

    // root 自身のアーキタイプはここまでで確定 (以後は子しか触らない) — ポインタを安全に読める
    AssetID outerMaterial = {};
    AssetID innerMaterial = {};
    if (const auto* mr = world.GetComponent<MeshRendererComponent>(root)) {
        outerMaterial = mr->material;
    }
    if (const auto* d = world.GetComponent<DestructibleComponent>(root)) {
        innerMaterial = d->innerMaterial;
    }
    if (innerMaterial.IsNull()) {
        innerMaterial = outerMaterial;
    }

    for (size_t i = 0; i < asset.pieces.size(); ++i) {
        const FracturePieceRef& piece = asset.pieces[i];

        char fragNameBuf[32];
        std::snprintf(fragNameBuf, sizeof(fragNameBuf), "Frag%d", static_cast<int>(i));
        const std::string fragName = MakeUniqueSiblingName(world, root, fragNameBuf, kNullEntity);
        GameObject frag(&world, world.CreateEntity(fragName));
        world.SetParent(frag.Id(), root);

        // 構造変更 (AddComponent) を先に済ませてからポインタを取り直す
        // (アーキタイプ移動で既存のコンポーネントポインタは死ぬ)
        frag.AddComponent<MeshRendererComponent>();
        frag.AddComponent<ColliderComponent>();
        frag.AddComponent<FracturePieceComponent>();

        if (auto* lt = frag.GetComponent<LocalTransform>()) {
            lt->position = piece.origin;
        }
        if (auto* mr = frag.GetComponent<MeshRendererComponent>()) {
            mr->mesh = piece.outerMesh;
            mr->material = outerMaterial;
        }
        if (auto* col = frag.GetComponent<ColliderComponent>()) {
            col->shape = collidershape::kConvex;
            col->isTrigger = false;
            col->meshAsset = piece.hull;
        }
        if (auto* fp = frag.GetComponent<FracturePieceComponent>()) {
            fp->root = root;
            fp->index = static_cast<int32_t>(i);
            fp->brokenBonds = 0;
            fp->damage = 0.0f;
            fp->releaseTicks = -1;
            fp->phase = 0;
        }
        // M80j: 骨が割り当たっている破片 (スキン破壊) は root 直下の子として骨に追従する
        // (PartFollowSystem の「部位は source の直子」規約、spec §10.5 Ragdolls と同じ形)
        if (!piece.boneName.empty()) {
            auto* pc = frag.AddComponent<PartComponent>();
            std::snprintf(pc->joint, sizeof(pc->joint), "%s", piece.boneName.c_str());
            pc->source = kNullEntity; // 最も近い SkinnedMesh (= root) へフォールバック
        }

        const std::string capName = MakeUniqueSiblingName(world, frag.Id(), "_cap", kNullEntity);
        GameObject cap(&world, world.CreateEntity(capName));
        world.SetParent(cap.Id(), frag.Id());
        cap.AddComponent<MeshRendererComponent>();
        if (auto* mr = cap.GetComponent<MeshRendererComponent>()) {
            mr->mesh = piece.capMesh;
            mr->material = innerMaterial;
        }
    }

    world.ApplyStructuralChanges();
    return static_cast<int>(asset.pieces.size());
}

int CountFracturePieceChildren(World& world, EntityID root)
{
    std::vector<EntityID> existing;
    CollectExistingPieces(world, root, &existing);
    return static_cast<int>(existing.size());
}

bool DestructiblePiecesMatchAsset(World& world, EntityID root, const FractureAssetHandle& asset,
                                  bool broken)
{
    std::vector<int32_t> indices;
    const ComponentTypeId req[] = { FracturePieceComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int fi = arch.FindTypeIndex(FracturePieceComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const auto* fp = static_cast<const FracturePieceComponent*>(arch.GetPtr(fi, row));
            if (fp->root == root) {
                indices.push_back(fp->index);
            }
        }
    });
    return FracturePieceIndicesMatchAsset(asset.pieces.size(), indices, broken);
}

} // namespace mye
