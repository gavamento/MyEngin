#include "Editor/SsrSelfTest.h"

#include <cmath>

#include "Engine/Core/Log.h"
#include "Engine/Renderer/RayTracing/RtMath.h"  // RtReflWeight (フェードの一致を照合する)
#include "Engine/Renderer/RayTracing/RtTypes.h" // kRtReflMaxRoughness
#include "Engine/Renderer/SsrPass.h"

namespace mye {
namespace {

// 光線 p = (px,py) + (dx,dy)*t を cellSize のセル 1 つぶん進める。
// 「増分が正であること」と「本当にセルの外へ出ること」の 2 つを同時に見る。
// minAdvance は「最低 0.5 px 進む」を t 単位に直したもの (シェーダと同じ導き方)
bool AdvanceLeavesCell(float px, float py, float dx, float dy, float cellSize)
{
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len <= 0.0f) {
        return false;
    }
    const float minAdvance = kSsrMinPixelStep / len;
    const float adv = SsrCellAdvance(px, py, dx, dy, cellSize, minAdvance);
    if (!(adv >= minAdvance)) {
        return false; // ★前進しない = 光線が同じ場所で反復を使い切る
    }
    const int cx = static_cast<int>(std::floor(px / cellSize));
    const int cy = static_cast<int>(std::floor(py / cellSize));
    const int nx = static_cast<int>(std::floor((px + dx * adv) / cellSize));
    const int ny = static_cast<int>(std::floor((py + dy * adv) / cellSize));
    return cx != nx || cy != ny;
}

} // namespace

bool RunSsrSelfTest()
{
    MYE_LOG_INFO("==== SSR (screen-space reflections) self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    // ---- セル前進: 必ず進み、必ずセルの外へ出る ----
    {
        // 斜め・軸平行・負方向・セル境界のちょうど上 (= 素朴な実装が 0 を返す形) を混ぜる。
        // 段が上がるほどセルは 2 倍になるので cellSize は 1,2,4,...512 まで見る
        const float dirs[][2] = { { 1.0f, 0.7f },   { -1.0f, 0.3f }, { 0.05f, -1.0f },
                                  { 1.0f, 0.0f },   { 0.0f, 1.0f },  { -1.0f, 0.0f },
                                  { 0.0f, -1.0f },  { 1.0f, 1.0f },  { -0.6f, -0.6f },
                                  { 1e-9f, 1.0f } };
        const float starts[][2] = { { 100.3f, 50.7f }, { 0.0f, 0.0f },  { 64.0f, 64.0f },
                                    { 63.999f, 8.0f }, { 8.0f, 128.0f } };
        bool allLeave = true;
        bool allForward = true;
        for (int level = 0; level <= 9; ++level) {
            const float cell = static_cast<float>(1 << level);
            for (const auto& s : starts) {
                for (const auto& d : dirs) {
                    // 画面全体を渡る長さの光線に正規化して minAdvance を作る
                    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1]);
                    const float minAdvance = kSsrMinPixelStep / (len * 960.0f);
                    const float adv =
                        SsrCellAdvance(s[0], s[1], d[0] * 960.0f, d[1] * 960.0f, cell, minAdvance);
                    allForward = allForward && (adv >= minAdvance) && (adv > 0.0f);
                    allLeave = allLeave
                        && AdvanceLeavesCell(s[0], s[1], d[0] * 960.0f, d[1] * 960.0f, cell);
                }
            }
        }
        check(allForward, "SsrCellAdvance never returns a non-positive step (the ray always moves)");
        check(allLeave, "SsrCellAdvance lands outside the cell it started in, at every mip level");

        // 軸に平行な成分は候補から外している。ここが inf のままだと min が壊れる
        const float onlyX = SsrCellAdvance(10.0f, 10.0f, 1.0f, 0.0f, 8.0f, 0.001f);
        check(onlyX > 0.0f && onlyX < 7.0f,
              "an axis-aligned ray ignores the parallel axis instead of picking up an infinity");

        // ★セル境界のちょうど上から負方向へ出る = 素朴な実装が 0 を返す形
        const float onEdge = SsrCellAdvance(64.0f, 32.0f, -1.0f, 0.0f, 32.0f, 0.001f);
        check(onEdge >= 0.001f,
              "starting exactly on a cell edge and travelling backwards still advances");

        // 極端に短い光線 (minAdvance が支配する) でも下限を割らない
        const float tiny = SsrCellAdvance(5.5f, 5.5f, 1e-7f, 1e-7f, 1.0f, 0.25f);
        check(tiny >= 0.25f, "a degenerate direction falls back to the minimum advance");
    }

    // ---- roughness フェード ----
    {
        const float maxR = 0.6f;
        check(SsrReflWeight(0.0f, maxR) == 1.0f, "a mirror surface takes the reflection 100%");
        // ★ここが厳密に 0 でないと「粗い面には 1 ビットも足さない」が崩れる
        check(SsrReflWeight(maxR, maxR) == 0.0f,
              "the cutoff roughness yields exactly 0 (no bits are added to rough surfaces)");
        check(SsrReflWeight(1.0f, maxR) == 0.0f, "roughness beyond the cutoff yields exactly 0");
        check(SsrReflWeight(0.4f, maxR) == 1.0f,
              "the weight is still 1 at the fade start (0.6 * 2/3 = 0.4)");
        // 単調減少
        bool monotonic = true;
        float prev = SsrReflWeight(0.0f, maxR);
        for (int i = 1; i <= 200; ++i) {
            const float w = SsrReflWeight(static_cast<float>(i) / 200.0f, maxR);
            monotonic = monotonic && (w <= prev + 1e-6f);
            prev = w;
        }
        check(monotonic, "the roughness fade is monotonically decreasing");
        // ★既定値では RT 反射 (M46h) のフェードと完全一致 = 同じ面で段差が出ない
        bool sameAsRt = true;
        for (int i = 0; i <= 100; ++i) {
            const float r = static_cast<float>(i) / 100.0f;
            sameAsRt = sameAsRt && std::fabs(SsrReflWeight(r, kRtReflMaxRoughness)
                                             - RtReflWeight(r))
                    < 1e-6f;
        }
        check(sameAsRt,
              "at the default cutoff the SSR fade matches RtReflWeight exactly (no seam vs RT)");
        // maxRoughness が 0 (スライダ下限) でも NaN を出さない
        check(SsrReflWeight(0.5f, 0.0f) == 0.0f && SsrReflWeight(0.0f, 0.0f) == 0.0f,
              "a zero cutoff disables the effect instead of producing NaN");
    }

    // ---- 画面端フェード ----
    {
        check(SsrEdgeFade(0.5f, 0.5f, kSsrEdgeFade) == 1.0f, "the screen centre is not faded");
        check(SsrEdgeFade(0.0f, 0.5f, kSsrEdgeFade) == 0.0f
                  && SsrEdgeFade(0.5f, 1.0f, kSsrEdgeFade) == 0.0f,
              "the screen border fades to exactly 0");
        check(SsrEdgeFade(-0.2f, 0.5f, kSsrEdgeFade) == 0.0f
                  && SsrEdgeFade(0.5f, 1.4f, kSsrEdgeFade) == 0.0f,
              "a hit outside the screen contributes nothing");
        bool monotonic = true;
        float prev = 0.0f;
        for (int i = 0; i <= 100; ++i) {
            const float u = static_cast<float>(i) / 200.0f; // 0 .. 0.5
            const float f = SsrEdgeFade(u, 0.5f, kSsrEdgeFade);
            monotonic = monotonic && (f >= prev - 1e-6f);
            prev = f;
        }
        check(monotonic, "the edge fade rises monotonically from the border to the centre");
        check(SsrEdgeFade(0.5f, 0.5f, 0.0f) == 1.0f && SsrEdgeFade(1.5f, 0.5f, 0.0f) == 0.0f,
              "a zero fade width degenerates to a plain inside/outside test");
    }

    // ---- 共有定数の健全性 (HLSL 側とは規則 9 が照合する) ----
    {
        check(kSsrMaxSteps > 0 && kSsrMaxSteps <= 256,
              "the step budget is positive and small enough for a pixel shader loop");
        check(kSsrMinPixelStep > 0.0f, "the minimum advance is strictly positive");
        check(kSsrFadeStartRatio > 0.0f && kSsrFadeStartRatio < 1.0f,
              "the fade starts strictly before the cutoff");
        check(kSsrMaxDistance > 0.0f && kSsrThickness > 0.0f,
              "the ray length and the surface thickness are positive");
    }

    MYE_LOG_INFO("==== SSR self test: %s ====", failCount == 0 ? "PASS" : "FAIL");
    return failCount == 0;
}

} // namespace mye
