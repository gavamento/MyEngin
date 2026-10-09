//====================================================================================
//                          RenderStatsSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                       10/09/2026
//                                          描画統計のビュー別集計と JSON ダンプの回帰テストの実装
//====================================================================================
#include "Engine/Engine/Rendering/RenderStatsSelfTest.h"

#include <string>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Diagnostics/Profiler.h"
#include "Engine/Engine/Rendering/RenderStatsDump.h"

namespace mye {
namespace {

// 全欄を足した 1 つの数 (欄の足し忘れを和の比較で拾うため、欄ごとに別の重みを掛ける)
long long Fingerprint(const prof::RenderStats& s)
{
    long long sum = 0;
    long long weight = 1;
    auto add = [&](int v) {
        sum += weight * v;
        weight += 2;
    };
    add(s.drawCalls);
    add(s.triangles);
    add(s.culled);
    add(s.shadowDrawCalls);
    add(s.shadowTriangles);
    for (int v : s.shadowCascadeDraws) {
        add(v);
    }
    for (int v : s.lodDraws) {
        add(v);
    }
    add(s.paletteEvaluated);
    add(s.paletteReused);
    add(s.occlusionPhase1Draws);
    add(s.occlusionPhase2Draws);
    add(s.occluded);
    return sum;
}

size_t CountOf(const std::string& text, const std::string& needle)
{
    size_t n = 0;
    for (size_t pos = text.find(needle); pos != std::string::npos; pos = text.find(needle, pos + needle.size())) {
        ++n;
    }
    return n;
}

} // namespace

bool RunRenderStatsSelfTest()
{
    MYE_LOG_INFO("==== Render stats self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  [PASS] %s", what);
        } else {
            MYE_LOG_ERROR("  [FAIL] %s", what);
            ++failCount;
        }
    };

    // ---- ビュー別の集計 ----
    prof::BeginFrame();
    prof::SetRenderStatsView(1);
    prof::AddDraw(12);
    prof::AddDraw(6);
    prof::AddCulled(4);
    prof::AddShadowDraw(30, 0);
    prof::AddShadowDraw(20, 2);
    prof::AddShadowDraw(10); // CSM 以外 (局所影アトラス)
    prof::SetRenderStatsView(2);
    prof::AddDraw(100);
    prof::AddShadowDraw(7, 1);
    prof::SetRenderStatsView(3);
    prof::RenderStats delta;
    delta.drawCalls = 3;
    delta.triangles = 9;
    delta.lodDraws[1] = 2;
    delta.paletteEvaluated = 5;
    delta.paletteReused = 6;
    delta.occlusionPhase1Draws = 7;
    delta.occlusionPhase2Draws = 8;
    delta.occluded = 9;
    prof::AddRenderStats(delta);
    prof::SetRenderStatsView(99); // 範囲外は 0 番へ
    prof::AddDraw(1);

    const prof::RenderStats total = prof::GetRenderStats();
    check(total.drawCalls == 2 + 1 + 3 + 1 && total.triangles == 12 + 6 + 100 + 9 + 1,
          "total draw calls / triangles are the sum over all draws");
    check(total.shadowDrawCalls == 4 && total.shadowTriangles == 30 + 20 + 10 + 7,
          "shadow draws stay out of the main draw fields");
    check(total.shadowCascadeDraws[0] == 1 && total.shadowCascadeDraws[1] == 1 && total.shadowCascadeDraws[2] == 1,
          "CSM cascade draws are counted per cascade (cascade < 0 is not a CSM draw)");
    check(total.culled == 4, "culled is accumulated");

    long long viewSum = 0;
    for (int v = 0; v < prof::kRenderStatsViewSlots; ++v) {
        viewSum += Fingerprint(prof::GetRenderStatsForView(static_cast<uint32_t>(v)));
    }
    check(viewSum == Fingerprint(total), "the per-view stats add up to the total in every field");
    check(prof::GetRenderStatsForView(1).drawCalls == 2 && prof::GetRenderStatsForView(2).drawCalls == 1
              && prof::GetRenderStatsForView(3).drawCalls == 3 && prof::GetRenderStatsForView(0).drawCalls == 1,
          "draws are filed under the current viewKey (out of range -> 0)");
    check(prof::GetRenderStatsForView(3).lodDraws[1] == 2 && prof::GetRenderStatsForView(3).occluded == 9,
          "AddRenderStats fills the fields later sub-milestones use");
    check(Fingerprint(prof::GetRenderStatsForView(99)) == 0, "an out-of-range view reads as empty");

    // ---- JSON ----
    RenderStatsDump dump;
    dump.frame = 60;
    dump.width = 960;
    dump.height = 540;
    dump.total = total;
    for (int v = 0; v < prof::kRenderStatsViewSlots; ++v) {
        dump.views[v] = prof::GetRenderStatsForView(static_cast<uint32_t>(v));
    }
    const std::string a = FormatRenderStatsJson(dump);
    dump.frameMs = 1.5f;
    dump.gbufferMs = 0.5f;
    dump.csmMs = 0.25f;
    const std::string b = FormatRenderStatsJson(dump);

    const size_t gpuA = a.find("\"gpuMs\"");
    const size_t gpuB = b.find("\"gpuMs\"");
    check(gpuA != std::string::npos && gpuA == gpuB && a.substr(0, gpuA) == b.substr(0, gpuB) && a != b,
          "GPU times live in their own section: everything before it is identical when only ms differ");
    check(CountOf(a, "\"viewKey\"") == 4, "every view that drew appears once in views[]");
    check(a.find("\"occlusionPhase2Draws\": 8") != std::string::npos
              && a.find("\"shadowCascadeDraws\": [1, 1, 1]") != std::string::npos
              && a.find("\"lodDraws\": [0, 2, 0, 0]") != std::string::npos,
          "the reserved fields and arrays are written in a fixed shape");

    prof::BeginFrame(); // 後続の表示へ持ち越さない
    check(Fingerprint(prof::GetRenderStats()) == 0, "BeginFrame clears the totals and the per-view stats");

    MYE_LOG_INFO("Render stats self test: %s (%d failures)", failCount == 0 ? "PASS" : "FAIL", failCount);
    return failCount == 0;
}

} // namespace mye
