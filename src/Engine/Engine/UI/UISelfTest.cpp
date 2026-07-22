#include "Engine/Engine/UI/UISelfTest.h"

#include <cmath>

#include "Engine/Core/Log.h"
#include "Engine/Engine/UI/UIGeometry.h"
#include "Engine/Engine/UI/UINav.h"
#include "Engine/Engine/UI/UIRenderer.h"

namespace mye {

bool RunUISelfTest()
{
    MYE_LOG_INFO("==== UI self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    // ResolveAnchor: 9-grid が正しい画面基準点にマップされるか (x/y は基準点からのオフセット)
    constexpr int W = 1000;
    constexpr int H = 800;
    constexpr float ox = 10.0f;
    constexpr float oy = 5.0f;
    struct Case {
        int anchor;
        float baseX;
        float baseY;
        const char* name;
    };
    const Case cases[9] = {
        { 0, 0.0f, 0.0f, "top-left" },      { 1, 500.0f, 0.0f, "top-center" },
        { 2, 1000.0f, 0.0f, "top-right" },  { 3, 0.0f, 400.0f, "mid-left" },
        { 4, 500.0f, 400.0f, "center" },    { 5, 1000.0f, 400.0f, "mid-right" },
        { 6, 0.0f, 800.0f, "bottom-left" }, { 7, 500.0f, 800.0f, "bottom-center" },
        { 8, 1000.0f, 800.0f, "bottom-right" },
    };
    for (const Case& c : cases) {
        float rx = 0, ry = 0;
        UIRenderer::ResolveAnchor(c.anchor, ox, oy, 100.0f, 40.0f, W, H, rx, ry);
        const bool ok = std::fabs(rx - (c.baseX + ox)) < 1e-4f
            && std::fabs(ry - (c.baseY + oy)) < 1e-4f;
        check(ok, c.name);
    }

    // ---- BuildFillQuad (M35): 境界値 0 / 0.5 / 1、水平と垂直 ----
    {
        using uigeom::BuildFillQuad;
        auto approx = [](float a, float b) { return std::fabs(a - b) < 1e-4f; };
        const auto full = BuildFillQuad(10, 20, 100, 40, 1, 1.0f);
        check(approx(full.w, 100) && approx(full.u1, 1.0f), "fill: horizontal 1.0 = full");
        const auto half = BuildFillQuad(10, 20, 100, 40, 1, 0.5f);
        check(approx(half.x, 10) && approx(half.w, 50) && approx(half.u1, 0.5f),
              "fill: horizontal 0.5 clips right half");
        const auto zero = BuildFillQuad(10, 20, 100, 40, 1, 0.0f);
        check(approx(zero.w, 0), "fill: horizontal 0 = empty");
        const auto vhalf = BuildFillQuad(10, 20, 100, 40, 2, 0.5f);
        check(approx(vhalf.y, 40) && approx(vhalf.h, 20) && approx(vhalf.v0, 0.5f) && approx(vhalf.v1, 1.0f),
              "fill: vertical 0.5 fills bottom-up");
        const auto over = BuildFillQuad(0, 0, 100, 40, 1, 1.5f);
        check(approx(over.w, 100), "fill: amount clamped to 1");
    }

    // ---- Build9Slice (M35): 9 矩形 / UV / 退化 / 過大 border ----
    {
        using uigeom::Build9Slice;
        auto approx = [](float a, float b) { return std::fabs(a - b) < 1e-4f; };
        uigeom::UIQuad q[9];
        // 64x64 テクスチャ・border 8px・矩形 200x100 → フル 9 枚
        const int n = Build9Slice(0, 0, 200, 100, 8, 8, 8, 8, 64, 64, q);
        check(n == 9, "9slice: full border -> 9 quads");
        // 左上隅は原寸 8x8、UV は 0..8/64
        check(approx(q[0].w, 8) && approx(q[0].h, 8) && approx(q[0].u1, 8.0f / 64.0f),
              "9slice: corner keeps native size + uv");
        // 中央は伸縮 (200-16 x 100-16)
        check(approx(q[4].w, 184) && approx(q[4].h, 84), "9slice: center stretches");
        // 幅の合計 = 全体幅
        check(approx(q[0].w + q[1].w + q[2].w, 200), "9slice: column widths sum to rect");
        // border 0 → 中央 1 枚だけ
        const int n1 = Build9Slice(0, 0, 200, 100, 0, 0, 0, 0, 64, 64, q);
        check(n1 == 1 && approx(q[0].w, 200), "9slice: zero border -> single quad");
        // 過大 border (l+r > w) は比率縮小で総和が矩形に収まる
        const int n2 = Build9Slice(0, 0, 10, 100, 8, 0, 8, 0, 64, 64, q);
        float wsum = 0;
        for (int i = 0; i < n2; ++i) {
            wsum += q[i].w;
        }
        check(n2 >= 2 && wsum <= 10.0f + 1e-3f, "9slice: oversized border shrinks to fit");
    }

    // ---- UINav::FindNext (M35): 方向選択 / 半平面除外 / タイブレーク ----
    {
        using namespace uinav;
        // 十字配置: 中央(0) 上(1) 下(2) 左(3) 右(4)
        const NavRect r[5] = {
            { 100, 100, 20, 20, 0 },
            { 100, 40, 20, 20, 1 },
            { 100, 160, 20, 20, 2 },
            { 40, 100, 20, 20, 3 },
            { 160, 100, 20, 20, 4 },
        };
        check(FindNext(r, 5, r[0], kNavUp) == 1, "nav: up picks upper");
        check(FindNext(r, 5, r[0], kNavDown) == 2, "nav: down picks lower");
        check(FindNext(r, 5, r[0], kNavLeft) == 3, "nav: left picks left");
        check(FindNext(r, 5, r[0], kNavRight) == 4, "nav: right picks right");
        // 上端からさらに上 → 候補なし = 現在維持
        check(FindNext(r, 5, r[1], kNavUp) == 1, "nav: no candidate keeps current");
        // 同点タイブレーク: 等距離の 2 候補は index 昇順
        const NavRect tie[3] = {
            { 100, 100, 20, 20, 5 },
            { 60, 40, 20, 20, 2 },  // 左上 (等距離)
            { 140, 40, 20, 20, 1 }, // 右上 (等距離)
        };
        check(FindNext(tie, 3, tie[0], kNavUp) == 1, "nav: tie-break by index");
        // 直交ずれの重み: 真上の遠い候補 vs 斜めの近い候補
        const NavRect wt[3] = {
            { 100, 100, 20, 20, 0 },
            { 100, 20, 20, 20, 1 },  // 真上 80px (score 80)
            { 130, 70, 20, 20, 2 },  // 斜め (axial 30 + ortho 30*2 = 90)
        };
        check(FindNext(wt, 3, wt[0], kNavUp) == 1, "nav: orthogonal drift is penalized");
    }

    if (failCount == 0) {
        MYE_LOG_INFO("==== UI self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== UI self test: %d FAILURE(S) ====", failCount);
    return false;
}

} // namespace mye
