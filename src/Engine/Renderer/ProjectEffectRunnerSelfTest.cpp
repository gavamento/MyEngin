/*----
 ProjectEffectRunnerSelfTest.cpp  ProjectEffectRunner の回帰テスト (M78b)
 作成者: 秋田蓮音                                09/22/2026
----*/
#include "Engine/Renderer/ProjectEffectRunnerSelfTest.h"

#include <algorithm>
#include <string>
#include <vector>

#include "Engine/Core/Log.h"
#include "Engine/Renderer/ProjectEffectRunner.h"

namespace mye {
namespace {

int g_failCount = 0;

// テスト判定マクロ
#define RUN_CHECK(cond)                                                               \
    do {                                                                              \
        if (cond) {                                                                   \
            MYE_LOG_INFO("  PASS: %s", #cond);                                       \
        } else {                                                                      \
            MYE_LOG_ERROR("  FAIL: %s (%s:%d)", #cond, __FILE__, __LINE__);           \
            ++g_failCount;                                                            \
        }                                                                             \
    } while (0)

// ---------------------------------------------------------------------------
// テスト 1: 空スタックで HasPasses が false を返す
// ---------------------------------------------------------------------------
void TestEmptyStack()
{
    MYE_LOG_INFO("[selftest] ProjectEffectRunner: 空スタック");

    ProjectEffectRunner runner;

    // パスが 0 件のとき HasPasses は false
    RUN_CHECK(!runner.HasPasses(PostInsertionPoint::BeforeTonemap));
    RUN_CHECK(!runner.HasPasses(PostInsertionPoint::AfterTonemap));

    // パスを追加して ClearPasses 後も false
    ProjectPostPassDesc desc;
    desc.shaderName = "TestShader.post";
    desc.insertion  = PostInsertionPoint::BeforeTonemap;
    runner.AddPass(desc);
    RUN_CHECK(runner.HasPasses(PostInsertionPoint::BeforeTonemap));

    runner.ClearPasses();
    RUN_CHECK(!runner.HasPasses(PostInsertionPoint::BeforeTonemap));
}

// ---------------------------------------------------------------------------
// テスト 2: 挿入点フィルタリング
// ---------------------------------------------------------------------------
void TestInsertionFilter()
{
    MYE_LOG_INFO("[selftest] ProjectEffectRunner: 挿入点フィルタリング");

    ProjectEffectRunner runner;

    // BeforeTonemap パスのみ登録
    {
        ProjectPostPassDesc d;
        d.shaderName = "A.post";
        d.insertion  = PostInsertionPoint::BeforeTonemap;
        runner.AddPass(d);
    }

    // BeforeTonemap は有効、AfterTonemap は無効
    RUN_CHECK(runner.HasPasses(PostInsertionPoint::BeforeTonemap));
    RUN_CHECK(!runner.HasPasses(PostInsertionPoint::AfterTonemap));

    // AfterTonemap パスを追加
    {
        ProjectPostPassDesc d;
        d.shaderName = "B.post";
        d.insertion  = PostInsertionPoint::AfterTonemap;
        runner.AddPass(d);
    }

    RUN_CHECK(runner.HasPasses(PostInsertionPoint::BeforeTonemap));
    RUN_CHECK(runner.HasPasses(PostInsertionPoint::AfterTonemap));
}

// ---------------------------------------------------------------------------
// テスト 3: enabled=false のパスは HasPasses から除外される
// ---------------------------------------------------------------------------
void TestDisabledPass()
{
    MYE_LOG_INFO("[selftest] ProjectEffectRunner: disabled パスの除外");

    ProjectEffectRunner runner;

    ProjectPostPassDesc desc;
    desc.shaderName = "C.post";
    desc.insertion  = PostInsertionPoint::BeforeTonemap;
    desc.enabled    = false;
    runner.AddPass(desc);

    // enabled=false のパスは HasPasses で見えない
    RUN_CHECK(!runner.HasPasses(PostInsertionPoint::BeforeTonemap));

    // enabled=true のパスを追加すると見える
    desc.enabled = true;
    desc.shaderName = "D.post";
    runner.AddPass(desc);
    RUN_CHECK(runner.HasPasses(PostInsertionPoint::BeforeTonemap));
}

// ---------------------------------------------------------------------------
// テスト 4: Priority 昇順安定ソート — CollectSortedPasses で実装本体を観測
// ---------------------------------------------------------------------------
void TestPrioritySort()
{
    MYE_LOG_INFO("[selftest] ProjectEffectRunner: Priority 昇順安定ソート (実装本体)");

    // Z(200) → A(100) → M(100) の順で登録する
    // 期待実行順: A(100) → M(100) → Z(200) (昇順安定ソート)
    ProjectEffectRunner runner;

    ProjectPostPassDesc dZ;
    dZ.shaderName = "Z.post";
    dZ.priority   = 200;
    dZ.insertion  = PostInsertionPoint::BeforeTonemap;

    ProjectPostPassDesc dA;
    dA.shaderName = "A.post";
    dA.priority   = 100;
    dA.insertion  = PostInsertionPoint::BeforeTonemap;

    ProjectPostPassDesc dM;
    dM.shaderName = "M.post";
    dM.priority   = 100; // A と同値 → 登録順 (A→M) を維持する安定ソートを確認
    dM.insertion  = PostInsertionPoint::BeforeTonemap;

    runner.AddPass(dZ);
    runner.AddPass(dA);
    runner.AddPass(dM);

    // CollectSortedPasses で RunPasses が使うソート結果を直接観測する
    auto sorted = runner.CollectSortedPasses(PostInsertionPoint::BeforeTonemap);

    RUN_CHECK(sorted.size() == 3);
    if (sorted.size() == 3)
    {
        RUN_CHECK(sorted[0]->shaderName == "A.post"); // priority 100 (先に登録)
        RUN_CHECK(sorted[1]->shaderName == "M.post"); // priority 100 (後に登録)
        RUN_CHECK(sorted[2]->shaderName == "Z.post"); // priority 200
    }

    // AfterTonemap には 1 件も入っていない (挿入点フィルタも CollectSortedPasses で確認)
    auto sortedAfter = runner.CollectSortedPasses(PostInsertionPoint::AfterTonemap);
    RUN_CHECK(sortedAfter.empty());

    // 同値が逆順 (M→A) で登録された場合も安定ソートで元の順を維持
    ProjectEffectRunner runner2;
    ProjectPostPassDesc dM2 = dM; dM2.shaderName = "M2.post";
    ProjectPostPassDesc dA2 = dA; dA2.shaderName = "A2.post";
    runner2.AddPass(dM2); // 先に登録
    runner2.AddPass(dA2); // 後に登録

    auto sorted2 = runner2.CollectSortedPasses(PostInsertionPoint::BeforeTonemap);
    RUN_CHECK(sorted2.size() == 2);
    if (sorted2.size() == 2)
    {
        RUN_CHECK(sorted2[0]->shaderName == "M2.post"); // 先に登録 → 安定ソートで先頭維持
        RUN_CHECK(sorted2[1]->shaderName == "A2.post");
    }
}

// ---------------------------------------------------------------------------
// テスト 5: ハード上限 (8 件超は切り捨て)
// ---------------------------------------------------------------------------
void TestHardLimit()
{
    MYE_LOG_INFO("[selftest] ProjectEffectRunner: ハード上限 %d",
                 ProjectEffectRunner::kMaxPostPasses);

    ProjectEffectRunner runner;

    const int over = ProjectEffectRunner::kMaxPostPasses + 2;
    std::vector<ProjectPostPassDesc> descs;
    descs.reserve(static_cast<size_t>(over));
    for (int i = 0; i < over; ++i)
    {
        ProjectPostPassDesc d;
        d.shaderName = "Shader" + std::to_string(i) + ".post";
        d.insertion  = PostInsertionPoint::BeforeTonemap;
        d.enabled    = true;
        descs.push_back(std::move(d));
    }

    runner.SetPasses(descs);

    auto sorted = runner.CollectSortedPasses(PostInsertionPoint::BeforeTonemap);
    RUN_CHECK(sorted.size() == static_cast<size_t>(ProjectEffectRunner::kMaxPostPasses));

    const std::string overflowName = "Shader" + std::to_string(over - 1) + ".post";
    const bool hasOverflow = std::any_of(sorted.begin(), sorted.end(),
        [&](const ProjectPostPassDesc* p) { return p->shaderName == overflowName; });
    RUN_CHECK(!hasOverflow);
}

// ---------------------------------------------------------------------------
// テスト 6: AfterTonemap の挿入点定数値
// ---------------------------------------------------------------------------
void TestInsertionPointConstants()
{
    MYE_LOG_INFO("[selftest] ProjectEffectRunner: 挿入点定数値");

    // spec §4.1 に基づく定数値 (0/1) を確認する
    // (シリアライズ・比較の安定性に関わるため)
    // volatile で定数畳み込みを防ぎ C4127 を回避する
    volatile auto vBefore = static_cast<int32_t>(PostInsertionPoint::BeforeTonemap);
    volatile auto vAfter  = static_cast<int32_t>(PostInsertionPoint::AfterTonemap);
    RUN_CHECK(vBefore == 0);
    RUN_CHECK(vAfter  == 1);
}

} // namespace

// ---------------------------------------------------------------------------
// エントリポイント
// ---------------------------------------------------------------------------
bool RunProjectEffectRunnerSelfTest()
{
    g_failCount = 0;
    MYE_LOG_INFO("=== ProjectEffectRunner SelfTest (M78b) ===");

    TestEmptyStack();
    TestInsertionFilter();
    TestDisabledPass();
    TestPrioritySort();
    TestHardLimit();
    TestInsertionPointConstants();

    if (g_failCount == 0)
    {
        MYE_LOG_INFO("=== ProjectEffectRunner SelfTest: ALL PASS ===");
    }
    else
    {
        MYE_LOG_ERROR("=== ProjectEffectRunner SelfTest: %d FAIL(s) ===", g_failCount);
    }
    return g_failCount == 0;
}

} // namespace mye
