#include "Engine/Engine/TransformSystem.h"

#include <algorithm>
#include <cstring>

#include <DirectXMath.h>

#include "Engine/Core/Components.h"
#include "Engine/Core/JobSystem.h"
#include "Engine/Core/World.h"

using namespace DirectX;

namespace mye {

namespace {

// スキップ判定はこの 40 バイトをそのまま memcmp する (M51c)
static_assert(sizeof(LocalTransform) == 10 * sizeof(float),
              "LocalTransform は TRS 10 float の連続配置が前提 (ビット比較)");

// 1 エンティティのワールド行列を計算する。親 (より浅い深度) は更新済み前提。
// レンジ非依存 (自分の WorldMatrix のみ書く) なので並列でも直列と同一結果。
// 通常経路とスキップ経路 (UpdateCached) が唯一共有する数式 — ON/OFF ビット一致の根拠
void ComputeWorldFrom(World& world, const LocalTransform* lt, WorldMatrixComponent* wm,
                      const HierarchyComponent* h)
{
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

void ComputeWorld(World& world, EntityID e)
{
    auto* lt = world.GetComponent<LocalTransform>(e);
    auto* wm = world.GetComponent<WorldMatrixComponent>(e);
    auto* h = world.GetComponent<HierarchyComponent>(e);
    if (!lt || !wm || !h) {
        return; // 破棄直後など (次の dirty 再構築で除去される)
    }
    ComputeWorldFrom(world, lt, wm, h);
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
        HierarchyComponent* h;
    };
    std::vector<Entry> entries;

    uint32_t maxIndex = 0;
    const ComponentTypeId req[] = { HierarchyComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int hi = arch.FindTypeIndex(HierarchyComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            entries.push_back({ 0, e, static_cast<HierarchyComponent*>(arch.GetPtr(hi, row)) });
            maxIndex = std::max(maxIndex, e.index);
        }
    });

    // 深度 = 親チェーンの長さ。M51c で親チェーンのメモ化により O(N) (旧: エンティティ毎に
    // 根まで歩く O(N×深度))。depth は HierarchyComponent 内 = WorldHash 対象なので、
    // 旧ロジックとの厳密同値を保つ:
    //   親が null                                     → 0
    //   親が「生きていて Hierarchy を持つ」           → depth(親) + 1
    //   それ以外 (死亡 / 世代不一致 / Hierarchy なし) → 1 (親を 1 段数えた直後に打ち切り)
    const size_t tableSize = entries.empty() ? 0 : static_cast<size_t>(maxIndex) + 1;
    constexpr uint32_t kUnset = 0xFFFFFFFFu;
    std::vector<uint32_t> memo(tableSize, kUnset);
    std::vector<EntityID> parentOf(tableSize);
    std::vector<uint32_t> genOf(tableSize, 0);
    std::vector<uint8_t> inSet(tableSize, 0);
    for (const Entry& en : entries) {
        parentOf[en.id.index] = en.h->parent;
        genOf[en.id.index] = en.id.generation;
        inSet[en.id.index] = 1;
    }
    std::vector<uint32_t> chain; // 深度未確定のまま辿った index 列 (根側が末尾)
    for (Entry& en : entries) {
        if (memo[en.id.index] == kUnset) {
            chain.clear();
            EntityID cur = en.id;
            uint32_t base = 0; // chain 末尾の深度
            while (true) {
                chain.push_back(cur.index);
                const EntityID p = parentOf[cur.index];
                if (p.IsNull()) {
                    base = 0;
                    break;
                }
                const bool parentLive =
                    p.index < tableSize && inSet[p.index] != 0 && genOf[p.index] == p.generation;
                if (!parentLive) {
                    base = 1; // 親を 1 段数えて打ち切り (旧チェーン歩きの break と同値)
                    break;
                }
                if (memo[p.index] != kUnset) {
                    base = memo[p.index] + 1;
                    break;
                }
                cur = p;
            }
            for (size_t j = chain.size(); j-- > 0;) {
                memo[chain[j]] = base + static_cast<uint32_t>(chain.size() - 1 - j);
            }
        }
        en.depth = memo[en.id.index];
        en.h->depth = en.depth; // 再構築時に Hierarchy.depth へ書き戻す (従来どおり)
    }

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

    // M51c: 側テーブルを全無効化。構造変更はすべてここを通るため、再ペアレントによる
    // 親行列の変化やスロット再利用が「TRS 不変」に紛れて温存されることはない
    lastTrs_.resize(tableSize);
    state_.assign(tableSize, 0);
}

// M51c 比較スキップ経路 (World::SimCacheEnabled() 時のみ)。スキップ = WorldMatrix の
// 前回値温存で、同じ入力ビットに対する ComputeWorldFrom の再実行と完全にビット一致する
void TransformSystem::UpdateCached(World& world, EntityID e)
{
    auto* lt = world.GetComponent<LocalTransform>(e);
    auto* wm = world.GetComponent<WorldMatrixComponent>(e);
    auto* h = world.GetComponent<HierarchyComponent>(e);
    uint8_t& st = state_[e.index];
    if (!lt || !wm || !h) {
        st = kSlotChanged; // 破棄直後など。valid を落とし、子も保守的に再計算へ倒す
        return;
    }
    // 親の据え置き可否は「スロット有効かつ今 tick 不変 (== kSlotValid)」のときだけ。
    // 親は必ずより浅いレベルで処理済み (レベル間バリア) なので、これは今 tick の値。
    // テーブル外 / 無効 / 再計算済みはすべて「変化あり」扱い (保守的 = 正しさ側に倒す)
    const bool parentStable = h->parent.IsNull()
        || (h->parent.index < state_.size() && state_[h->parent.index] == kSlotValid);
    if (parentStable && (st & kSlotValid) != 0
        && std::memcmp(&lastTrs_[e.index], lt, sizeof(TrsBits)) == 0) {
        st = kSlotValid; // スキップ (前回値温存)。changed を落として子にも据え置きを許す
        return;
    }
    std::memcpy(&lastTrs_[e.index], lt, sizeof(TrsBits));
    ComputeWorldFrom(world, lt, wm, h);
    st = kSlotValid | kSlotChanged;
}

void TransformSystem::Update(World& world)
{
    if (world.HierarchyDirty()) {
        Rebuild(world);
        world.ClearHierarchyDirty();
    }

    const bool useCache = World::SimCacheEnabled();

    // 深度レベルを順に処理 (レベル間はバリア = 親が先に確定)。各レベル内は互いに独立なので
    // JobSystem で並列化する。GetComponentRaw は読み取り専用ルックアップで並行安全、各エンティティ
    // は自分の WorldMatrix (と M51c の自スロット) のみ書くため、直列と完全にビット一致する。
    for (const auto& [begin, end] : levels_) {
        const size_t count = end - begin;
        jobs::System().ParallelRanges(count, kTransformGrain, [&](size_t a, size_t b) {
            for (size_t k = a; k < b; ++k) {
                if (useCache) {
                    UpdateCached(world, sorted_[begin + k]);
                } else {
                    ComputeWorld(world, sorted_[begin + k]);
                }
            }
        });
    }

    if (useCache) {
        uint32_t computed = 0;
        for (const EntityID e : sorted_) {
            computed += (state_[e.index] & kSlotChanged) != 0 ? 1u : 0u;
        }
        stats_ = { computed, static_cast<uint32_t>(sorted_.size()) - computed };
    } else {
        stats_ = { static_cast<uint32_t>(sorted_.size()), 0 };
    }
}

} // namespace mye
