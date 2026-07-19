#include "Engine/Engine/TransformSystem.h"

#include <algorithm>

#include <DirectXMath.h>

#include "Engine/Core/Components.h"
#include "Engine/Core/World.h"

using namespace DirectX;

namespace mye {

void TransformSystem::Rebuild(World& world)
{
    sorted_.clear();

    struct Entry {
        uint32_t depth;
        EntityID id;
    };
    std::vector<Entry> entries;

    const ComponentTypeId req[] = { HierarchyComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int hi = arch.FindTypeIndex(HierarchyComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            // 深度 = 親チェーンの長さ。再構築時に計算し Hierarchy.depth へ書き戻す
            uint32_t depth = 0;
            EntityID cur = static_cast<HierarchyComponent*>(arch.GetPtr(hi, row))->parent;
            while (!cur.IsNull()) {
                ++depth;
                auto* ph = world.GetComponent<HierarchyComponent>(cur);
                if (!ph) {
                    break;
                }
                cur = ph->parent;
            }
            static_cast<HierarchyComponent*>(arch.GetPtr(hi, row))->depth = depth;
            entries.push_back({ depth, e });
        }
    });

    // (depth, index) の明示キーでソート — 決定論 (spec 11.2 規則 7)
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.depth != b.depth) {
            return a.depth < b.depth;
        }
        return a.id.index < b.id.index;
    });

    sorted_.reserve(entries.size());
    for (const Entry& e : entries) {
        sorted_.push_back(e.id);
    }
}

void TransformSystem::Update(World& world)
{
    if (world.HierarchyDirty()) {
        Rebuild(world);
        world.ClearHierarchyDirty();
    }

    // 深度昇順なので、親のワールド行列は常に子より先に更新済み
    for (const EntityID e : sorted_) {
        auto* lt = world.GetComponent<LocalTransform>(e);
        auto* wm = world.GetComponent<WorldMatrixComponent>(e);
        auto* h = world.GetComponent<HierarchyComponent>(e);
        if (!lt || !wm || !h) {
            continue; // 破棄直後など (次の dirty 再構築で除去される)
        }

        const XMVECTOR s = XMLoadFloat3(&lt->scale);
        const XMVECTOR r = XMLoadFloat4(&lt->rotation);
        const XMVECTOR t = XMLoadFloat3(&lt->position);
        XMMATRIX local = XMMatrixAffineTransformation(s, XMVectorZero(), r, t);

        if (!h->parent.IsNull()) {
            if (auto* pw = world.GetComponent<WorldMatrixComponent>(h->parent)) {
                local = XMMatrixMultiply(local, XMLoadFloat4x4(&pw->value));
            }
        }
        XMStoreFloat4x4(&wm->value, local);
    }
}

} // namespace mye
