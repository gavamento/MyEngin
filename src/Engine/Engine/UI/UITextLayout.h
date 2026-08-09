#pragma once
// テキスト行レイアウトの純関数群 (M51e)。D3D 非依存 — UISelfTest が合成グリフマップで検証。
// UIRenderer::PushText から「行分割 (改行 + 折返し) と整列オフセット」を抽出したもの。
// 折返しは文字単位 (日本語優先 — 単語境界は扱わない)。計測は FontGlyphInfo::advance × k
// (k = FontAtlas::GlyphScale(fontScale)) で、描画側と同じ式なのでズレない。
#include <vector>

#include "Engine/Renderer/FontGeometry.h"

namespace mye {
namespace textlayout {

// 1 行 = 元文字列のバイト範囲 [begin,end) + 計測幅 px。範囲に改行バイトは含まない
struct Line {
    const char* begin = nullptr;
    const char* end = nullptr;
    float width = 0.0f;
};

// s を行に分割する。'\n' で常に改行、wrap かつ maxW>0 なら幅超過の文字の直前で折る
// (行頭 1 文字は必ず載せる = 無限ループしない)。制御文字 (<0x20) は幅 0。
// グリフ未焼成は '?' の幅で代用 (描画側と同じ規則)。末尾の空行は出力しない。
inline void LayoutText(const FontGlyphMap& glyphs, const char* s, float k, bool wrap, float maxW,
                       std::vector<Line>& out)
{
    out.clear();
    if (!s) {
        return;
    }
    const auto itFallback = glyphs.find(static_cast<uint32_t>('?'));
    const FontGlyphInfo* fallback = (itFallback != glyphs.end()) ? &itFallback->second : nullptr;
    const bool doWrap = wrap && maxW > 0.0f;
    Line cur;
    cur.begin = s;
    const char* p = s;
    bool lineHasGlyph = false;
    for (;;) {
        const char* cpStart = p;
        const uint32_t cp = fontgeom::Utf8Next(p);
        if (cp == 0) {
            cur.end = cpStart;
            if (cur.begin != cur.end) {
                out.push_back(cur);
            }
            return;
        }
        if (cp == '\n') {
            cur.end = cpStart;
            out.push_back(cur); // 空行も出す ("a\n\nb" の中間行 = 行送り)
            cur = {};
            cur.begin = p;
            cur.width = 0.0f;
            lineHasGlyph = false;
            continue;
        }
        if (cp < 0x20) {
            continue; // 制御文字: 幅 0 のまま範囲にだけ残る (描画側もスキップする)
        }
        const FontGlyphInfo* g = nullptr;
        const auto it = glyphs.find(cp);
        if (it != glyphs.end()) {
            g = &it->second;
        } else {
            g = fallback;
        }
        const float adv = g ? g->advance * k : 0.0f;
        if (doWrap && lineHasGlyph && cur.width + adv > maxW) {
            cur.end = cpStart; // この文字の直前で折る
            out.push_back(cur);
            cur = {};
            cur.begin = cpStart;
            cur.width = 0.0f;
            lineHasGlyph = false;
        }
        cur.width += adv;
        lineHasGlyph = true;
    }
}

// 9-grid align (0..8) の列 → 行の水平オフセット (0=左 1=中 2=右)
inline float AlignX(int align, float lineW, float boxW)
{
    const int col = align % 3;
    return (col == 0) ? 0.0f : (col == 1) ? (boxW - lineW) * 0.5f : boxW - lineW;
}

// 9-grid align (0..8) の行 → ブロック全体の垂直オフセット (0=上 1=中 2=下)
inline float AlignY(int align, float totalH, float boxH)
{
    const int row = (align / 3) % 3;
    return (row == 0) ? 0.0f : (row == 1) ? (boxH - totalH) * 0.5f : boxH - totalH;
}

} // namespace textlayout
} // namespace mye
