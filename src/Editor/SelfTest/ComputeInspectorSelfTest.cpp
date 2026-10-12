//====================================================================================
//                          ComputeInspectorSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                       10/12/2026
//                                          Compute 欄の文字列 (状態・実行先・版・補足) の回帰テスト
//====================================================================================
#include "Editor/SelfTest/ComputeInspectorSelfTest.h"

#include <set>
#include <string>

#include "Editor/Windows/Scene/ComputeInspectorSection.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Localization/Localization.h"

namespace mye {
namespace {

using namespace compute;

int g_fails = 0;

void Check(bool cond, const char* msg)
{
    if (cond) {
        MYE_LOG_INFO("  PASS: %s", msg);
    } else {
        MYE_LOG_ERROR("  FAIL: %s", msg);
        ++g_fails;
    }
}

template <typename Fn>
bool AllDistinctNonEmpty(Fn&& textOf, int count)
{
    std::set<std::string> seen;
    for (int i = 0; i < count; ++i) {
        const std::string s = textOf(i);
        if (s.empty() || s == "?" || !seen.insert(s).second) {
            return false;
        }
    }
    return true;
}

void TestLanguage(Lang lang, const char* label)
{
    SetLanguage(lang);
    const std::string tag = label;
    Check(AllDistinctNonEmpty([](int i) { return std::string(ComputeRowStateText(static_cast<RowState>(i))); }, 5),
          (tag + ": 状態の 5 種類が別々の文字列になる").c_str());
    // 実行先の "-" と版の "-" は別の欄なので、欄ごとに重複を見る
    Check(AllDistinctNonEmpty([](int i) { return std::string(ComputeRowTargetText(static_cast<RowTarget>(i))); }, 6),
          (tag + ": 実行先の 6 種類が別々の文字列になる").c_str());
    Check(AllDistinctNonEmpty([](int i) { return std::string(ComputeRowVersionText(static_cast<RowVersion>(i), CallerKind::Cpp)); }, 4),
          (tag + ": 版の 4 種類が別々の文字列になる").c_str());
    Check(std::string(ComputeRowVersionText(RowVersion::Mismatch, CallerKind::Cpp))
              != std::string(ComputeRowVersionText(RowVersion::Mismatch, CallerKind::CSharp)),
          (tag + ": 版ずれは C++ (再ビルド) と C# (再コンパイル待ち) で案内が違う").c_str());

    ComputeInspectorRow notDeclared;
    notDeclared.state = RowState::FailedNotDeclared;
    notDeclared.target = RowTarget::Failed;
    notDeclared.lastFail = FailReason::NotDeclared;
    const std::string cppHint = ComputeRowDetail(notDeclared, CallerKind::Cpp);
    const std::string csHint = ComputeRowDetail(notDeclared, CallerKind::CSharp);
    Check(cppHint.find("COMPUTE_KERNELS") != std::string::npos && csHint.find("ComputeKernels") != std::string::npos
              && cppHint.find("not-declared") != std::string::npos,
          (tag + ": 宣言なしの補足は言語ごとの直し方と失敗理由を含む").c_str());

    ComputeInspectorRow lint;
    lint.state = RowState::LintViolation;
    lint.violation = "Foo.kernel.hlsl(12): error KL005 [swizzle]: x";
    Check(ComputeRowDetail(lint, CallerKind::Cpp).find("KL005") != std::string::npos, (tag + ": 検査違反の補足は規則と行を含む").c_str());

    ComputeInspectorRow ok;
    ok.state = RowState::Ran;
    Check(ComputeRowDetail(ok, CallerKind::Cpp).empty(), (tag + ": 正常な行は補足なし").c_str());
}

} // namespace

bool RunComputeInspectorSelfTest()
{
    MYE_LOG_INFO("==== Compute inspector strings (M91h) self test ====");
    g_fails = 0;
    const Lang original = CurrentLanguage();
    TestLanguage(Lang::En, "En");
    TestLanguage(Lang::Ja, "Ja");
    SetLanguage(original);
    MYE_LOG_INFO("==== Compute inspector strings self test: %s ====", g_fails == 0 ? "PASS" : "FAIL");
    return g_fails == 0;
}

} // namespace mye
