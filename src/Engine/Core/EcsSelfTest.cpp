#include "Engine/Core/EcsSelfTest.h"

#include <algorithm>
#include <cstring>
#include <span>
#include <vector>

#include "Engine/Core/Components.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"

namespace mye {
namespace {

int g_failCount = 0;

#define TEST_CHECK(cond)                                                     \
    do {                                                                     \
        if (cond) {                                                          \
            MYE_LOG_INFO("  PASS: %s", #cond);                               \
        } else {                                                             \
            MYE_LOG_ERROR("  FAIL: %s (%s:%d)", #cond, __FILE__, __LINE__);  \
            ++g_failCount;                                                   \
        }                                                                    \
    } while (0)

void TestLifetimeAndGenerations()
{
    MYE_LOG_INFO("[selftest] lifetime & generational handles");
    World w;

    const EntityID e = w.CreateEntity("A");
    TEST_CHECK(w.IsAlive(e));
    TEST_CHECK(strcmp(w.GetName(e), "A") == 0);
    TEST_CHECK(w.GetComponent<LocalTransform>(e) != nullptr);

    // Destroy は tick 末まで遅延 (spec 4.3)
    w.DestroyEntity(e);
    TEST_CHECK(w.IsAlive(e));
    w.ApplyStructuralChanges();
    TEST_CHECK(!w.IsAlive(e));

    // スロット再利用で generation が進み、古いハンドルは無効のまま
    const EntityID e2 = w.CreateEntity("B");
    TEST_CHECK(e2.index == e.index);
    TEST_CHECK(e2.generation != e.generation);
    TEST_CHECK(!w.IsAlive(e));
    TEST_CHECK(w.IsAlive(e2));
}

void TestArchetypeMovePreservesData()
{
    MYE_LOG_INFO("[selftest] archetype move preserves data");
    World w;

    const EntityID e = w.CreateEntity("Mover");
    auto* t = w.GetComponent<LocalTransform>(e);
    t->position = { 1.0f, 2.0f, 3.0f };

    // AddComponent → アーキタイプ移動。既存データが保持されること
    auto* mr = w.AddComponent<MeshRendererComponent>(e);
    TEST_CHECK(mr != nullptr);
    mr->mesh = AssetID{ 42 };
    t = w.GetComponent<LocalTransform>(e); // 移動後のポインタを取り直す
    TEST_CHECK(t != nullptr && t->position.x == 1.0f && t->position.z == 3.0f);
    TEST_CHECK(w.GetComponent<MeshRendererComponent>(e)->mesh.value == 42);

    w.RemoveComponent<MeshRendererComponent>(e);
    TEST_CHECK(w.GetComponent<MeshRendererComponent>(e) == nullptr);
    t = w.GetComponent<LocalTransform>(e);
    TEST_CHECK(t != nullptr && t->position.y == 2.0f);

    // 基本コンポーネントは除去できない
    w.RemoveComponentRaw(e, LocalTransform::sTypeId);
    TEST_CHECK(w.GetComponent<LocalTransform>(e) != nullptr);
}

void TestDeferredCommandsDuringIteration()
{
    MYE_LOG_INFO("[selftest] deferred commands during iteration");
    World w;

    const EntityID a = w.CreateEntity("A");
    const EntityID b = w.CreateEntity("B");

    const ComponentTypeId req[] = { NameComponent::sTypeId };
    w.ForEachArchetype(req, [&](Archetype& arch) {
        (void)arch;
        // イテレーション中の Add は scratch 経由で tick 末適用
        auto* cam = w.AddComponent<CameraComponent>(a);
        if (cam) {
            cam->fovYDeg = 123.0f;
        }
        w.DestroyEntity(b);
    });

    // 適用前: まだ見えない / まだ生きている
    TEST_CHECK(w.GetComponent<CameraComponent>(a) == nullptr);
    TEST_CHECK(w.IsAlive(b));

    w.ApplyStructuralChanges();
    auto* cam = w.GetComponent<CameraComponent>(a);
    TEST_CHECK(cam != nullptr && cam->fovYDeg == 123.0f); // scratch の初期値が実体に反映
    TEST_CHECK(!w.IsAlive(b));
}

void TestHierarchyAndSubtreeDestroy()
{
    MYE_LOG_INFO("[selftest] hierarchy & subtree destroy");
    World w;

    const EntityID root = w.CreateEntity("Root");
    const EntityID c1 = w.CreateEntity("Child1");
    const EntityID c2 = w.CreateEntity("Child2");
    const EntityID gc = w.CreateEntity("GrandChild");

    w.SetParent(c1, root);
    w.SetParent(c2, root);
    w.SetParent(gc, c1);
    w.ApplyStructuralChanges(); // SetParent は遅延適用 (spec 4.4)

    TEST_CHECK(w.GetParent(c1) == root);
    TEST_CHECK(w.GetParent(gc) == c1);
    auto* rh = w.GetComponent<HierarchyComponent>(root);
    TEST_CHECK(rh->firstChild == c1);
    auto* c1h = w.GetComponent<HierarchyComponent>(c1);
    TEST_CHECK(c1h->nextSibling == c2); // 適用順 = 兄弟順

    // 循環は拒否される
    w.SetParent(root, gc);
    w.ApplyStructuralChanges();
    TEST_CHECK(w.GetParent(root).IsNull());

    // 親の破棄で子孫も破棄
    w.DestroyEntity(root);
    w.ApplyStructuralChanges();
    TEST_CHECK(!w.IsAlive(root));
    TEST_CHECK(!w.IsAlive(c1));
    TEST_CHECK(!w.IsAlive(c2));
    TEST_CHECK(!w.IsAlive(gc));
    TEST_CHECK(w.AliveCount() == 0);

    // 再ペアレント: 旧親の子リストから外れる
    const EntityID p1 = w.CreateEntity("P1");
    const EntityID p2 = w.CreateEntity("P2");
    const EntityID ch = w.CreateEntity("C");
    w.SetParent(ch, p1);
    w.ApplyStructuralChanges();
    w.SetParent(ch, p2);
    w.ApplyStructuralChanges();
    TEST_CHECK(w.GetParent(ch) == p2);
    TEST_CHECK(w.GetComponent<HierarchyComponent>(p1)->firstChild.IsNull());
    TEST_CHECK(w.GetComponent<HierarchyComponent>(p2)->firstChild == ch);
}

void TestDeterministicRng()
{
    MYE_LOG_INFO("[selftest] deterministic RNG");
    Pcg32 a, b;
    a.Seed(12345);
    b.Seed(12345);
    bool same = true;
    for (int i = 0; i < 1000; ++i) {
        if (a.NextU32() != b.NextU32()) {
            same = false;
            break;
        }
    }
    TEST_CHECK(same);

    // 状態の保存/復元 (リプレイで使用)
    const uint64_t st = a.State();
    const uint64_t inc = a.Inc();
    const uint32_t next = a.NextU32();
    Pcg32 c;
    c.Restore(st, inc);
    TEST_CHECK(c.NextU32() == next);
}

void TestQueryCacheTransparency()
{
    MYE_LOG_INFO("[selftest] query cache transparency (M51a)");
    const bool savedFlag = World::SimCacheEnabled();
    World w;

    // 型構成の異なるエンティティ群 (base のみ / base+MR)
    w.CreateEntity("A");
    const EntityID b = w.CreateEntity("B");
    w.AddComponent<MeshRendererComponent>(b);
    const EntityID c = w.CreateEntity("C");
    w.AddComponent<MeshRendererComponent>(c);

    const ComponentTypeId mrType = MeshRendererComponent::sTypeId;
    auto enumerate = [&](std::span<const ComponentTypeId> req) {
        std::vector<EntityID> out;
        w.ForEachArchetype(req, [&](Archetype& arch) {
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                out.push_back(arch.EntityAt(row));
            }
        });
        return out;
    };

    // キャッシュ経路 (初回充填 + 2 回目ヒット) と線形経路が同一順・同一集合であること
    World::SetSimCacheEnabled(true);
    const auto cachedFill = enumerate({ &mrType, 1 });
    const auto cachedHit = enumerate({ &mrType, 1 });
    World::SetSimCacheEnabled(false);
    const auto linear = enumerate({ &mrType, 1 });
    TEST_CHECK(cachedFill.size() == 2);
    TEST_CHECK(cachedFill == linear);
    TEST_CHECK(cachedHit == linear);

    // キャッシュ充填「後」に生成された新アーキタイプ (base+MR+FileId) を追記マッチで拾うこと
    World::SetSimCacheEnabled(true);
    (void)enumerate({ &mrType, 1 }); // 充填 (Clear はしていないので既存エントリが残っている)
    const EntityID d = w.CreateEntity("D");
    w.AddComponent<MeshRendererComponent>(d);
    w.AddComponent<FileIdComponent>(d); // 新しい型組み合わせ → GetOrCreateArchetype の追記点
    const auto afterCached = enumerate({ &mrType, 1 });
    World::SetSimCacheEnabled(false);
    const auto afterLinear = enumerate({ &mrType, 1 });
    TEST_CHECK(afterCached.size() == 3);
    TEST_CHECK(afterCached == afterLinear);

    // Clear でキャッシュも破棄されること (index が張り替わっても誤マッチしない)
    World::SetSimCacheEnabled(true);
    w.Clear();
    TEST_CHECK(enumerate({ &mrType, 1 }).empty());
    const EntityID e2 = w.CreateEntity("E");
    w.AddComponent<MeshRendererComponent>(e2);
    TEST_CHECK(enumerate({ &mrType, 1 }).size() == 1);

    World::SetSimCacheEnabled(savedFlag);
}

void TestFindTypeIndexMatchesLinear()
{
    MYE_LOG_INFO("[selftest] FindTypeIndex binary search matches linear (M51a)");
    World w;
    const EntityID e = w.CreateEntity("T");
    w.AddComponent<MeshRendererComponent>(e);
    w.AddComponent<FileIdComponent>(e);

    bool sortedOk = true;
    bool foundOk = true;
    bool absentOk = true;
    for (const auto& archPtr : w.Archetypes()) {
        const auto types = archPtr->Types();
        sortedOk &= std::is_sorted(types.begin(), types.end());
        for (size_t i = 0; i < types.size(); ++i) {
            foundOk &= (archPtr->FindTypeIndex(types[i]) == static_cast<int>(i));
        }
        // 含まない型は -1 (線形参照): 登録済み型のうち types に無いものを総当たり
        for (ComponentTypeId t = 0; t < 96; ++t) {
            const bool has = std::find(types.begin(), types.end(), t) != types.end();
            if (!has) {
                absentOk &= (archPtr->FindTypeIndex(t) < 0);
            }
        }
    }
    TEST_CHECK(sortedOk);
    TEST_CHECK(foundOk);
    TEST_CHECK(absentOk);
}

} // namespace

bool RunEcsSelfTest()
{
    g_failCount = 0;
    MYE_LOG_INFO("==== ECS self test ====");
    TestLifetimeAndGenerations();
    TestArchetypeMovePreservesData();
    TestDeferredCommandsDuringIteration();
    TestHierarchyAndSubtreeDestroy();
    TestDeterministicRng();
    TestQueryCacheTransparency();
    TestFindTypeIndexMatchesLinear();
    if (g_failCount == 0) {
        MYE_LOG_INFO("==== ECS self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== ECS self test: %d FAILURE(S) ====", g_failCount);
    return false;
}

} // namespace mye
