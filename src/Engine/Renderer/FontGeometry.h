#pragma once
// フォント基盤の純関数群 (M34)。D3D 非依存 — FontSelfTest がヘッドレスで検証する。
// FontAtlas (GPU アトラス) と VfxGeometry (TextMesh) が共有する。
// テキストは描画専用 (kComponentNoHash) なので sim/リプレイの決定論契約とは無関係。
#include <cstdint>
#include <unordered_map>

namespace mye {

// 焼成済みグリフ 1 個の計測 (ベイク px 基準)。UV はアトラス正規化座標。
// xoff/yoff は「ペンのベースライン原点」からビットマップ左上までのオフセット
// (stb_truetype の GetCodepointBitmapBox 準拠。yoff は上方向が負)。
struct FontGlyphInfo {
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0; // アトラス UV
    float w = 0, h = 0;                   // ビットマップ寸法 (ベイク px)
    float xoff = 0, yoff = 0;             // ベースライン原点 → ビットマップ左上
    float advance = 0;                    // 送り幅 (ベイク px)
    bool valid = false;                   // false = 焼成失敗 (フォールバック '?' を使う)
};

// codepoint → グリフ計測。FontAtlas が所有し、消費側 (UIRenderer/VfxGeometry) は参照で受ける
using FontGlyphMap = std::unordered_map<uint32_t, FontGlyphInfo>;

namespace fontgeom {

inline constexpr uint32_t kReplacementChar = 0xFFFD; // 不正 UTF-8 列の置換文字

// UTF-8 の次のコードポイントを返し p を進める。不正な列は 1 バイトだけ進めて
// kReplacementChar を返す (途中で切れた列にも安全 = SetUIText の 255B 切詰め耐性)。
// 終端 ('\0') では 0 を返し p は進めない。
inline uint32_t Utf8Next(const char*& p)
{
    const unsigned char c0 = static_cast<unsigned char>(*p);
    if (c0 == 0) {
        return 0;
    }
    if (c0 < 0x80) {
        ++p;
        return c0;
    }
    int len = 0;
    uint32_t cp = 0;
    if ((c0 & 0xE0) == 0xC0) {
        len = 2;
        cp = c0 & 0x1F;
    } else if ((c0 & 0xF0) == 0xE0) {
        len = 3;
        cp = c0 & 0x0F;
    } else if ((c0 & 0xF8) == 0xF0) {
        len = 4;
        cp = c0 & 0x07;
    } else {
        ++p; // 孤立継続バイト等
        return kReplacementChar;
    }
    for (int i = 1; i < len; ++i) {
        const unsigned char ci = static_cast<unsigned char>(p[i]);
        if ((ci & 0xC0) != 0x80) {
            ++p; // 途中で切れた列: 先頭 1 バイトだけ消費
            return kReplacementChar;
        }
        cp = (cp << 6) | (ci & 0x3F);
    }
    p += len;
    // 過長エンコード / サロゲート / 範囲外は置換
    if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000)
        || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
        return kReplacementChar;
    }
    return cp;
}

// シェルフ (棚) パッカー: 左→右に詰め、行が溢れたら次の棚へ。グリフ間 1px パディング。
// Alloc が false を返したらアトラスが満杯 = 呼び出し側が成長 (Reset して全再焼成) する。
struct FontShelfPacker {
    int width = 0;
    int height = 0;
    int cursorX = 1; // 1px 境界パディング
    int cursorY = 1;
    int rowH = 0;

    void Reset(int w, int h)
    {
        width = w;
        height = h;
        cursorX = 1;
        cursorY = 1;
        rowH = 0;
    }
    bool Alloc(int w, int h, int& outX, int& outY)
    {
        if (w + 2 > width || h + 2 > height) {
            return false; // 1 グリフがアトラスより大きい
        }
        if (cursorX + w + 1 > width) { // 行送り
            cursorX = 1;
            cursorY += rowH + 1;
            rowH = 0;
        }
        if (cursorY + h + 1 > height) {
            return false; // 満杯
        }
        outX = cursorX;
        outY = cursorY;
        cursorX += w + 1;
        if (h > rowH) {
            rowH = h;
        }
        return true;
    }
};

} // namespace fontgeom
} // namespace mye
