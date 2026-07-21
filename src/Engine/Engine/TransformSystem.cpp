#include "Engine/Engine/TransformSystem.h"

#include <algorithm>

#include <DirectXMath.h>

#include "Engine/Core/Components.h"
#include "Engine/Core/JobSystem.h"
#include "Engine/Core/World.h"

using namespace DirectX;

namespace mye {

namespace {

// 1 エンティティのワールド行列を計算する。親 (より浅い深度) は更新済み前提。
// レンジ非依存 (自分の WorldMatrix のみ書く) なので並列でも直列と同一結果。
void ComputeWorld(World& world, EntityID e)
{
    auto* lt = world.GetComponent<LocalTransform>(e);
    auto* wm = world.GetComponent<WorldMatrixComponent>(e);
    auto* h = world.GetComponent<HierarchyComponent>(e);
    if (!lt || !wm || !h) {
        return; // 破棄直後など (次の dirty 再構築で除去される)
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

// レベル内を並列化する閾値 (これ未満は直列 = スレッド起動コスト回避)
constexpr size_t kTransformGrain = 256;

} // namespace

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

    // 深度レベル境界を算出 (entries は depth 昇順なので連続) — 並列化の分割単位
    levels_.clear();
    size_t i = 0;
    while (i < entries.size()) {
        const uint32_t d = entries[i].depth;
        size_t j = i + 1;
        while (j < entries.size() && entries[j].depth == d) {
            ++j;
        }
        levels_.emplace_back(i, j);
        i = j;
    }
}

void TransformSystem::Update(World& world)
{
    if (world.HierarchyDirty()) {
        Rebuild(world);
        world.ClearHierarchyDirty();
    }

    // 深度レベルを順に処理 (レベル間はバリア = 親が先に確定)。各レベル内は互いに独立なので
    // JobSystem で並列化する。GetComponentRaw は読み取り専用ルックアップで並行安全、各エンティティ
    // は自分の WorldMatrix のみ書くため、直列と完全にビット一致する。
    for (const auto& [begin, end] : levels_) {
        const size_t count = end - begin;
        jobs::System().ParallelRanges(count, kTransformGrain, [&](size_t a, size_t b) {
            for (size_t k = a; k < b; ++k) {
                ComputeWorld(world, sorted_[begin + k]);
            }
        });
    }
}

} // namespace mye
