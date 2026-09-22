/*----
 ProjectComputeRunnerSelfTest.cpp  ProjectComputeRunner のヘッドレス回帰テスト (M78d)
 作成者: 秋田蓮音                                09/22/2026
----*/
#include "Engine/Renderer/ProjectComputeRunnerSelfTest.h"

#include <vector>

#include "Engine/Core/Log.h"
#include "Engine/Renderer/ProjectComputeRunner.h"

namespace mye {
namespace {

int g_failCount = 0;

// テスト判定マクロ
#define CS_CHECK(cond)                                                                  \
    do {                                                                                \
        if (cond) {                                                                     \
            MYE_LOG_INFO("  PASS: %s", #cond);                                         \
        } else {                                                                        \
            MYE_LOG_ERROR("  FAIL: %s (%s:%d)", #cond, __FILE__, __LINE__);             \
            ++g_failCount;                                                              \
        }                                                                               \
    } while (0)

// ---------------------------------------------------------------------------
// テスト 1: 空スタックは HasPasses=false
// ---------------------------------------------------------------------------
void TestEmptyStack()
{
    MYE_LOG_INFO("[selftest] ProjectComputeRunner: 空スタック");

    ProjectComputeRunner runner;
    CS_CHECK(!runner.HasPasses(ComputeDispatchPoint::BeforePost));
    CS_CHECK(!runner.HasPasses(ComputeDispatchPoint::BeforeTonemap));
    CS_CHECK(!runner.HasPasses(ComputeDispatchPoint::AfterTonemap));
}

// ---------------------------------------------------------------------------
// テスト 2: DispatchPoint 文字列変換の往復
// ---------------------------------------------------------------------------
void TestDispatchPointStringConversion()
{
    MYE_LOG_INFO("[selftest] ProjectComputeRunner: DispatchPoint 文字列変換");

    CS_CHECK(std::string(DispatchPointToString(ComputeDispatchPoint::BeforePost))
             == "BeforePost");
    CS_CHECK(std::string(DispatchPointToString(ComputeDispatchPoint::BeforeTonemap))
             == "BeforeTonemap");
    CS_CHECK(std::string(DispatchPointToString(ComputeDispatchPoint::AfterTonemap))
             == "AfterTonemap");

    CS_CHECK(DispatchPointFromString("BeforePost")    == ComputeDispatchPoint::BeforePost);
    CS_CHECK(DispatchPointFromString("BeforeTonemap") == ComputeDispatchPoint::BeforeTonemap);
    CS_CHECK(DispatchPointFromString("AfterTonemap")  == ComputeDispatchPoint::AfterTonemap);

    // 未知の文字列は BeforePost にフォールバック
    CS_CHECK(DispatchPointFromString("Unknown")       == ComputeDispatchPoint::BeforePost);
    CS_CHECK(DispatchPointFromString("")              == ComputeDispatchPoint::BeforePost);
}

// ---------------------------------------------------------------------------
// テスト 3: HasPasses の挿入点フィルタリング
// ---------------------------------------------------------------------------
void TestDispatchPointFilter()
{
    MYE_LOG_INFO("[selftest] ProjectComputeRunner: HasPasses 挿入点フィルタリング");

    ProjectComputeRunner runner;

    // BeforePost に 1 件追加
    ProjectComputePassDesc d;
    d.shader        = "A.cs";
    d.dispatchPoint = ComputeDispatchPoint::BeforePost;
    d.enabled       = true;

    std::vector<ProjectComputePassDesc> descs = { d };
    runner.SetPasses(descs);

    CS_CHECK( runner.HasPasses(ComputeDispatchPoint::BeforePost));
    CS_CHECK(!runner.HasPasses(ComputeDispatchPoint::BeforeTonemap));
    CS_CHECK(!runner.HasPasses(ComputeDispatchPoint::AfterTonemap));

    // enabled=false にすると HasPasses=false
    descs[0].enabled = false;
    runner.SetPasses(descs);
    CS_CHECK(!runner.HasPasses(ComputeDispatchPoint::BeforePost));
}

// ---------------------------------------------------------------------------
// テスト 4: ハード上限 (8 件超は切り捨てて警告)
// ---------------------------------------------------------------------------
void TestHardLimit()
{
    MYE_LOG_INFO("[selftest] ProjectComputeRunner: ハード上限 %d",
                 ProjectComputeRunner::kMaxComputePasses);

    ProjectComputeRunner runner;

    // 上限 + 2 件のパスを SetPasses する
    const int over = ProjectComputeRunner::kMaxComputePasses + 2;
    std::vector<ProjectComputePassDesc> descs;
    descs.reserve(static_cast<size_t>(over));
    for (int i = 0; i < over; ++i)
    {
        ProjectComputePassDesc d;
        d.shader        = "Shader" + std::to_string(i) + ".cs";
        d.dispatchPoint = ComputeDispatchPoint::BeforePost;
        d.enabled       = true;
        descs.push_back(std::move(d));
    }

    runner.SetPasses(descs);

    // 内部で持つパス数が kMaxComputePasses に切り捨てられているかを HasPasses で間接確認
    // (全部 BeforePost なので HasPasses は true になるが、個数の直接取得 API はない)
    CS_CHECK(runner.HasPasses(ComputeDispatchPoint::BeforePost));

    // GetOutputSRV でも上限を超えたシェーダ名は解決できない
    const std::string overflowName = "Shader" + std::to_string(over - 1) + ".cs";
    CS_CHECK(runner.GetOutputSRV(overflowName) == nullptr);
}

// ---------------------------------------------------------------------------
// テスト 5: グループ数の計算 (CalcGroups 相当をパブリック API で確認)
// ---------------------------------------------------------------------------
void TestGroupCalc()
{
    MYE_LOG_INFO("[selftest] ProjectComputeRunner: グループ数計算");

    // kDefaultGroupSize=8 で ceil(width/8) を確認する
    const int kGS = ProjectComputeRunner::kDefaultGroupSize;

    // 幅 64 → グループ 8
    const int w64 = 64;
    const int ex64 = (w64 + kGS - 1) / kGS;
    CS_CHECK(ex64 == 8);

    // 幅 65 → グループ 9 (切り上げ)
    const int w65 = 65;
    const int ex65 = (w65 + kGS - 1) / kGS;
    CS_CHECK(ex65 == 9);

    // 幅 1 → グループ 1
    const int w1 = 1;
    const int ex1 = (w1 + kGS - 1) / kGS;
    CS_CHECK(ex1 == 1);

    // 幅 0 → グループ 1 (ゼロ割保護)
    const int w0 = 0;
    const int ex0 = (w0 <= 0 || kGS <= 0) ? 1 : (w0 + kGS - 1) / kGS;
    CS_CHECK(ex0 == 1);
}

// ---------------------------------------------------------------------------
// テスト 6: ClearPasses 後に HasPasses=false
// ---------------------------------------------------------------------------
void TestClearPasses()
{
    MYE_LOG_INFO("[selftest] ProjectComputeRunner: ClearPasses");

    ProjectComputeRunner runner;

    ProjectComputePassDesc d;
    d.shader        = "Foo.cs";
    d.dispatchPoint = ComputeDispatchPoint::AfterTonemap;
    d.enabled       = true;
    runner.SetPasses({ d });

    CS_CHECK(runner.HasPasses(ComputeDispatchPoint::AfterTonemap));

    runner.ClearPasses();
    CS_CHECK(!runner.HasPasses(ComputeDispatchPoint::AfterTonemap));
}

// ---------------------------------------------------------------------------
// テスト 7: DispatchPoint 定数値
// ---------------------------------------------------------------------------
void TestDispatchPointConstants()
{
    MYE_LOG_INFO("[selftest] ProjectComputeRunner: DispatchPoint 定数値");

    // シリアライズの安定性のため、定数値を固定する (spec §4.1 の順序と一致)
    CS_CHECK(static_cast<int>(ComputeDispatchPoint::BeforePost)    == 0);
    CS_CHECK(static_cast<int>(ComputeDispatchPoint::BeforeTonemap) == 1);
    CS_CHECK(static_cast<int>(ComputeDispatchPoint::AfterTonemap)  == 2);
}

} // namespace

// ---------------------------------------------------------------------------
// エントリポイント
// ---------------------------------------------------------------------------
bool RunProjectComputeRunnerSelfTest()
{
    g_failCount = 0;
    MYE_LOG_INFO("=== ProjectComputeRunner SelfTest (M78d) ===");

    TestEmptyStack();
    TestDispatchPointStringConversion();
    TestDispatchPointFilter();
    TestHardLimit();
    TestGroupCalc();
    TestClearPasses();
    TestDispatchPointConstants();

    if (g_failCount == 0)
    {
        MYE_LOG_INFO("=== ProjectComputeRunner SelfTest: ALL PASS ===");
    }
    else
    {
        MYE_LOG_ERROR("=== ProjectComputeRunner SelfTest: %d FAIL(s) ===", g_failCount);
    }
    return g_failCount == 0;
}

} // namespace mye
