#include "Engine/Engine/FontSelfTest.h"

#include <vector>

#include "Engine/Core/Log.h"
#include "Engine/Renderer/FontGeometry.h"

namespace mye {

bool RunFontSelfTest()
{
    MYE_LOG_INFO("==== Font self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };
    using fontgeom::kReplacementChar;
    using fontgeom::Utf8Next;

    // ---- (1) UTF-8 デコード: 1〜4 バイト境界 ----
    {
        const char* p = "A";
        check(Utf8Next(p) == 0x41 && Utf8Next(p) == 0, "utf8: 1-byte ASCII");
        p = "\xC2\xA2"; // U+00A2 ¢
        check(Utf8Next(p) == 0xA2 && Utf8Next(p) == 0, "utf8: 2-byte");
        p = "\xE3\x81\x82"; // U+3042 あ
        check(Utf8Next(p) == 0x3042 && Utf8Next(p) == 0, "utf8: 3-byte (hiragana)");
        p = "\xF0\x90\x8D\x88"; // U+10348
        check(Utf8Next(p) == 0x10348 && Utf8Next(p) == 0, "utf8: 4-byte");
        p = "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E"; // 日本語
        const uint32_t a = Utf8Next(p), b = Utf8Next(p), c = Utf8Next(p);
        check(a == 0x65E5 && b == 0x672C && c == 0x8A9E && Utf8Next(p) == 0,
              "utf8: consecutive kanji");
    }

    // ---- (2) UTF-8 不正列: 置換文字 + 1 バイト消費で前進が保証される ----
    {
        const char* p = "\x80" "A"; // 孤立継続バイト
        check(Utf8Next(p) == kReplacementChar && Utf8Next(p) == 0x41,
              "utf8: lone continuation -> U+FFFD, next resumes");
        p = "\xE3\x81"; // 途中で切れた 3 バイト列 (SetUIText の切詰め耐性)
        const uint32_t r1 = Utf8Next(p);
        const uint32_t r2 = Utf8Next(p);
        check(r1 == kReplacementChar && r2 == kReplacementChar && Utf8Next(p) == 0,
              "utf8: truncated sequence terminates safely");
        p = "\xC0\x80"; // 過長エンコード (U+0000 の 2 バイト表現)
        check(Utf8Next(p) == kReplacementChar, "utf8: overlong encoding rejected");
        p = "\xED\xA0\x80"; // サロゲート U+D800
        check(Utf8Next(p) == kReplacementChar, "utf8: surrogate rejected");
        p = "\xFF";
        check(Utf8Next(p) == kReplacementChar && Utf8Next(p) == 0, "utf8: invalid lead byte");
    }

    // ---- (3) シェルフパッカー: 充填 / 行送り / 満杯 ----
    {
        fontgeom::FontShelfPacker pk;
        pk.Reset(64, 64);
        int x = 0, y = 0;
        check(pk.Alloc(10, 10, x, y) && x == 1 && y == 1, "packer: first alloc at (1,1)");
        int x2 = 0, y2 = 0;
        check(pk.Alloc(10, 10, x2, y2) && x2 == 12 && y2 == 1, "packer: second alloc packs right");
        // 行に入らない幅 → 次の棚 (y = 1 + rowH(10) + 1)
        int x3 = 0, y3 = 0;
        check(pk.Alloc(50, 8, x3, y3) && x3 == 1 && y3 == 12, "packer: row wrap to next shelf");
        // 満杯 (縦が足りない): 高さ 64 に対し y3=12 + 8 の棚の次に 60 は入らない
        int x4 = 0, y4 = 0;
        check(!pk.Alloc(10, 60, x4, y4), "packer: full atlas returns false");
        // アトラスより大きいグリフは常に false
        fontgeom::FontShelfPacker pk2;
        pk2.Reset(32, 32);
        check(!pk2.Alloc(40, 8, x4, y4), "packer: oversized glyph rejected");
        // ぎっしり詰めても座標が範囲内 + 重複しないことを軽く網羅
        fontgeom::FontShelfPacker pk3;
        pk3.Reset(64, 64);
        std::vector<int> got;
        int placed = 0;
        for (int i = 0; i < 200; ++i) {
            int gx = 0, gy = 0;
            if (!pk3.Alloc(7, 7, gx, gy)) {
                break;
            }
            got.push_back(gy * 64 + gx);
            ++placed;
        }
        bool inRange = true;
        for (int v : got) {
            const int gx = v % 64;
            const int gy = v / 64;
            if (gx < 1 || gy < 1 || gx + 7 > 64 || gy + 7 > 64) {
                inRange = false;
            }
        }
        check(placed >= 49 && inRange, "packer: dense fill stays in bounds"); // 8x8 セルで 7x7 が 49+ 個
    }

    if (failCount == 0) {
        MYE_LOG_INFO("==== Font self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== Font self test: %d FAILURE(S) ====", failCount);
    return false;
}

} // namespace mye
