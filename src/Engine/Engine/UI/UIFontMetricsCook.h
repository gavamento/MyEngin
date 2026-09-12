//====================================================================================
//                          UIFontMetricsCook.h
//  MyEngine/ 秋田蓮音                                                      09/13/2026
//                                          TTF からフォント計測表を作る cook（CLI とエディタの共通口）
//====================================================================================
#pragma once
// フォント計測表の cook (M75d)。`Editor.exe --cook-font-metrics [--project DIR]` と
// Project Settings > UI のボタンが呼ぶ。stb_truetype を使うので計測表本体 (UITextMetrics) とは
// ファイルを分けている — sim が include するのは UITextMetrics.h だけ。
#include <cstdint>
#include <string>
#include <vector>

#include "Engine/Engine/UI/UITextMetrics.h"

namespace mye {
namespace uitext {

// 情報欄 basePx / lineH256 の基準 (FontAtlas のベイク基準 kBasePx と同じ 32 px)
inline constexpr int32_t kCookBasePx = 32;

// TTF / TTC (先頭フォント) のバイト列から表を作る。0x20..0xFFFF (サロゲート除く) のうち
// `stbtt_FindGlyphIndex != 0` の文字だけを載せる = FontAtlas が焼ける文字と同じ集合。
// 送り幅は **フォント単位の整数だけで** `ceil(advance × 1000 / (ascent - descent + lineGap))`
// (ピクセル倍率は比で消える) = どの機械で cook しても同じ表になる
bool CookFontMetricsFromTtf(const std::vector<uint8_t>& fileBytes, const std::string& fontName,
                            FontMetrics& out, std::string* error = nullptr);

struct FontMetricsCookResult {
    bool ok = false;
    bool noFont = false;    // assets\fonts に .ttf / .ttc が無い
    bool unchanged = false; // 既存ファイルと同じ内容だったので書かなかった
    std::wstring fontPath;
    std::wstring outPath;
    uint32_t glyphs = 0;
    uint64_t hash = 0;
    std::string error;
};

// プロジェクトの描画フォント (fontfiles::ListProjectFontFiles の先頭) を cook して
// 隣へ <stem>.fontmetrics.json を書く。内容が同じなら書かない (更新日時も動かさない)
FontMetricsCookResult CookProjectFontMetrics(const std::wstring& assetsRoot);

// --cook-font-metrics の本体。結果を標準出力へ 1〜2 行。
// exit 0 = 書いた / 最新 / 1 = 失敗 / 2 = フォントが無い
int RunFontMetricsCookCli(const std::wstring& assetsRoot);

} // namespace uitext
} // namespace mye
