#include "Editor/HzbSelfTest.h"

#include <vector>

#include "Engine/Core/Log.h"
#include "Engine/Renderer/HzbPass.h"

namespace mye {
namespace {

// D3D11 がミップ列を自動生成するときの段数 (floor(log2(max(w,h))) + 1)。
// **HzbMipCount とは別の式で書いてある** — 同じ書き方で 2 回書いても検査にならないため
int D3DMipCount(int width, int height)
{
    int longest = (width > height) ? width : height;
    int count = 1;
    while (longest > 1) {
        longest >>= 1;
        ++count;
    }
    return count;
}

// floor 半減 (最小 1) を level 回繰り返した素朴版。HzbMipExtent のシフト実装の対照
int NaiveMipExtent(int base, int level)
{
    int v = base;
    for (int i = 0; i < level; ++i) {
        v = (v > 1) ? (v / 2) : 1;
    }
    return v;
}

// 出力 dstExtent テクセルの被覆区間が [0, srcExtent-1] を穴なく覆うか。
// 重なり (min なので無害) は許すが、**1 テクセルでも漏れたら false**
bool SpansCoverAll(int srcExtent, int dstExtent)
{
    std::vector<bool> hit(static_cast<size_t>(srcExtent), false);
    int prevBegin = 0;
    for (int i = 0; i < dstExtent; ++i) {
        int b = 0;
        int e = 0;
        HzbReduceSpan(i, srcExtent, dstExtent, b, e);
        if (b < 0 || e < b || e > srcExtent - 1) {
            return false;
        }
        if (b < prevBegin) {
            return false; // 区間の開始が単調でない = 読み位置が飛んでいる
        }
        prevBegin = b;
        for (int x = b; x <= e; ++x) {
            hit[static_cast<size_t>(x)] = true;
        }
    }
    for (bool h : hit) {
        if (!h) {
            return false;
        }
    }
    return true;
}

// ピラミッド全段について被覆を確かめる (段 0 = 深度からの素通しも含む)
bool PyramidCoversAll(int width, int height)
{
    const int mips = HzbMipCount(width, height);
    for (int level = 0; level < mips; ++level) {
        const int srcW = (level == 0) ? width : HzbMipExtent(width, level - 1);
        const int srcH = (level == 0) ? height : HzbMipExtent(height, level - 1);
        const int dstW = HzbMipExtent(width, level);
        const int dstH = HzbMipExtent(height, level);
        if (!SpansCoverAll(srcW, dstW) || !SpansCoverAll(srcH, dstH)) {
            return false;
        }
    }
    return true;
}

} // namespace

bool RunHzbSelfTest()
{
    MYE_LOG_INFO("==== HZB (min-Z pyramid) self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    // 検査に使う寸法。960x540 は決定的撮影の実寸、1920x1080 は既定ウィンドウ、
    // 残りは「奇数」「素数」「極端に細長い」= 素朴な 2x2 畳みが必ず取りこぼす形
    const int sizes[][2] = { { 960, 540 },  { 1920, 1080 }, { 1, 1 },   { 2, 1 },
                             { 15, 7 },     { 17, 3 },      { 31, 31 }, { 1023, 1 },
                             { 3, 1024 },   { 100, 100 },   { 640, 480 } };

    // ---- 段数: D3D の自動ミップ列と同じ長さか ----
    {
        bool allMatch = true;
        for (const auto& s : sizes) {
            allMatch = allMatch && (HzbMipCount(s[0], s[1]) == D3DMipCount(s[0], s[1]));
        }
        check(allMatch, "HzbMipCount matches the D3D mip chain length for every test size");
        check(HzbMipCount(960, 540) == 10, "960x540 has 10 mips (960 -> ... -> 1x1)");
        check(HzbMipCount(1, 1) == 1, "a 1x1 pyramid is a single level");
        check(HzbMipCount(0, 16) == 0 && HzbMipCount(-4, -4) == 0,
              "a degenerate size yields no levels (the pass bails out instead of allocating)");
    }

    // ---- 段の寸法: floor 半減 (最小 1) の繰り返しと厳密に同値か ----
    {
        bool allMatch = true;
        bool endsAtOne = true;
        for (const auto& s : sizes) {
            const int mips = HzbMipCount(s[0], s[1]);
            for (int level = 0; level < mips; ++level) {
                allMatch = allMatch && (HzbMipExtent(s[0], level) == NaiveMipExtent(s[0], level))
                    && (HzbMipExtent(s[1], level) == NaiveMipExtent(s[1], level));
            }
            endsAtOne = endsAtOne && (HzbMipExtent(s[0], mips - 1) == 1)
                && (HzbMipExtent(s[1], mips - 1) == 1);
        }
        check(allMatch, "HzbMipExtent equals repeated floor-halving (min 1) at every level");
        check(endsAtOne, "the last level of every pyramid is exactly 1x1");
        check(HzbMipExtent(540, 3) == 67, "540 halves to 67 by level 3 (floor at each step)");
        check(HzbMipExtent(1024, 64) == 1 && HzbMipExtent(1024, 31) == 1,
              "an absurd level clamps to 1 instead of shifting out of range");
    }

    // ---- 縮小の被覆区間 ----
    {
        int b = 0;
        int e = 0;
        // 素通しコピー (段 0): 1 テクセルに退化する。ここが崩れると深度がぼける
        bool passthrough = true;
        for (int i = 0; i < 16; ++i) {
            HzbReduceSpan(i, 16, 16, b, e);
            passthrough = passthrough && (b == i) && (e == i);
        }
        check(passthrough, "src == dst degenerates to a 1-texel passthrough (mip 0 needs no CS)");

        // 偶数辺: きっちり 2x2
        bool even = true;
        for (int i = 0; i < 8; ++i) {
            HzbReduceSpan(i, 16, 8, b, e);
            even = even && (b == 2 * i) && (e == 2 * i + 1);
        }
        check(even, "an even halving reads exactly two source texels per output");

        // ★奇数辺: 最後の 1 テクセルまで必ず誰かが読む (取りこぼしゼロ)
        HzbReduceSpan(6, 15, 7, b, e);
        check(e == 14, "the last output of a 15 -> 7 reduction reaches source texel 14");
        HzbReduceSpan(0, 15, 7, b, e);
        check(b == 0, "the first output of a 15 -> 7 reduction starts at source texel 0");

        // 範囲外 / 退化した引数は空区間ではなく (0,0) に潰す (シェーダ側は端数スレッドを
        // 早期 return で落とすので、CPU 鏡は「呼ばれない」ことの表明として 0 を返す)
        HzbReduceSpan(7, 15, 7, b, e);
        check(b == 0 && e == 0, "an out-of-range output index yields the degenerate span (0,0)");
        HzbReduceSpan(0, 0, 4, b, e);
        check(b == 0 && e == 0, "a zero-sized source yields the degenerate span (0,0)");
    }

    // ---- 全段の被覆 (これが 1 本落ちたら SSR の光線が壁を抜ける) ----
    {
        bool allCover = true;
        for (const auto& s : sizes) {
            allCover = allCover && PyramidCoversAll(s[0], s[1]);
        }
        check(allCover, "every mip of every test pyramid covers its source with no gaps");
    }

    // ---- ディスパッチの切り上げが全テクセルを覆うか ----
    {
        check(kHzbThreadGroupSize > 0, "the thread group size is positive");
        bool covered = true;
        for (const auto& s : sizes) {
            const int mips = HzbMipCount(s[0], s[1]);
            for (int level = 0; level < mips; ++level) {
                const int dstW = HzbMipExtent(s[0], level);
                const int dstH = HzbMipExtent(s[1], level);
                const int gx = (dstW + kHzbThreadGroupSize - 1) / kHzbThreadGroupSize;
                const int gy = (dstH + kHzbThreadGroupSize - 1) / kHzbThreadGroupSize;
                covered = covered && (gx * kHzbThreadGroupSize >= dstW)
                    && (gy * kHzbThreadGroupSize >= dstH) && (gx >= 1) && (gy >= 1);
            }
        }
        check(covered, "the thread group round-up dispatches at least one thread per texel");
    }

    MYE_LOG_INFO("==== HZB self test: %s ====", failCount == 0 ? "PASS" : "FAIL");
    return failCount == 0;
}

} // namespace mye
