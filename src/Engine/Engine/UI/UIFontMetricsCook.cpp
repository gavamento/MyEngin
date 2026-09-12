//====================================================================================
//                          UIFontMetricsCook.cpp
//  MyEngine/ 秋田蓮音                                                      09/13/2026
//                                          TTF からフォント計測表を作る cook の実装
//====================================================================================
#include "Engine/Engine/UI/UIFontMetricsCook.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

#include "Engine/Core/Log.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/FontFiles.h"

#include "stb/stb_truetype.h"

namespace mye {
namespace uitext {

bool CookFontMetricsFromTtf(const std::vector<uint8_t>& fileBytes, const std::string& fontName,
                            FontMetrics& out, std::string* error)
{
    out = FontMetrics{};
    const auto fail = [&](const char* what) {
        if (error) {
            *error = what;
        }
        return false;
    };
    if (fileBytes.size() < 12) {
        return fail("font file is too small");
    }
    stbtt_fontinfo info{};
    // ttc は先頭フォント (FontAtlas::InitTtf と同じ)
    const int offset = stbtt_GetFontOffsetForIndex(fileBytes.data(), 0);
    if (offset < 0 || !stbtt_InitFont(&info, fileBytes.data(), offset)) {
        return fail("stb_truetype could not open the font (CFF-based .otf is not supported)");
    }
    int asc = 0, desc = 0, gap = 0;
    stbtt_GetFontVMetrics(&info, &asc, &desc, &gap);
    // 行高 (フォント単位)。FontAtlas の baseLineHPx_ = (asc - desc + gap) × scale と同じ分子
    const int64_t lineUnits = static_cast<int64_t>(asc) - desc + gap;
    if (lineUnits <= 0) {
        return fail("font has a non-positive line height");
    }
    // 情報欄だけは px 換算 (ハッシュに入らないので浮動小数の機種差は表の同一性に影響しない)
    const float scale = stbtt_ScaleForPixelHeight(&info, static_cast<float>(kCookBasePx));
    const int32_t lineH256 =
        static_cast<int32_t>(std::lround(static_cast<double>(lineUnits) * scale * 256.0));

    std::vector<GlyphAdvance> glyphs;
    for (uint32_t cp = 0x20; cp <= kMaxCodepoint; ++cp) {
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            continue; // サロゲートは Utf8Next が置換文字にするので文字として現れない
        }
        const int gi = stbtt_FindGlyphIndex(&info, static_cast<int>(cp));
        if (gi == 0) {
            continue; // フォントに無い = 描画は '?' (表の AdvanceOf も '?' へ倒す)
        }
        int adv = 0, lsb = 0;
        stbtt_GetGlyphHMetrics(&info, gi, &adv, &lsb);
        // ★切り上げ: 表の和が実グリフの和を下回らない = 箱に合わせた文字が折り返らない
        const int64_t a = std::max<int64_t>(adv, 0);
        int64_t ratio = (a * kAdvPerLine + lineUnits - 1) / lineUnits;
        if (ratio > kAdvanceMax) {
            ratio = kAdvanceMax;
        }
        glyphs.push_back({ cp, static_cast<uint16_t>(ratio) });
    }
    std::string buildError;
    if (!FontMetrics::Build(fontName, fileBytes.size(), kCookBasePx, lineH256, glyphs, out,
                            &buildError)) {
        if (error) {
            *error = buildError;
        }
        return false;
    }
    return true;
}

FontMetricsCookResult CookProjectFontMetrics(const std::wstring& assetsRoot)
{
    FontMetricsCookResult r;
    const std::vector<std::wstring> fonts = fontfiles::ListProjectFontFiles(assetsRoot);
    if (fonts.empty()) {
        r.noFont = true;
        r.error = "no .ttf / .ttc in assets\\fonts";
        return r;
    }
    r.fontPath = fonts.front();
    r.outPath = FontMetricsPathFor(r.fontPath);

    std::vector<uint8_t> bytes;
    {
        std::ifstream f(std::filesystem::path(r.fontPath), std::ios::binary);
        if (!f) {
            r.error = "could not read " + WideToUtf8(r.fontPath);
            return r;
        }
        bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    const std::string fontName = WideToUtf8(std::filesystem::path(r.fontPath).filename().wstring());
    FontMetrics m;
    if (!CookFontMetricsFromTtf(bytes, fontName, m, &r.error)) {
        return r;
    }
    const std::string text = SerializeFontMetricsJson(m);
    r.glyphs = m.GlyphCount();
    r.hash = m.Hash();

    // 同じ内容なら書かない (Source Control に偽の変更を出さない)
    {
        std::ifstream f(std::filesystem::path(r.outPath), std::ios::binary);
        if (f) {
            std::stringstream ss;
            ss << f.rdbuf();
            if (ss.str() == text) {
                r.ok = true;
                r.unchanged = true;
                return r;
            }
        }
    }
    std::ofstream f(std::filesystem::path(r.outPath), std::ios::binary | std::ios::trunc);
    if (!f) {
        r.error = "could not write " + WideToUtf8(r.outPath);
        return r;
    }
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!f) {
        r.error = "write failed: " + WideToUtf8(r.outPath);
        return r;
    }
    r.ok = true;
    return r;
}

int RunFontMetricsCookCli(const std::wstring& assetsRoot)
{
    const FontMetricsCookResult r = CookProjectFontMetrics(assetsRoot);
    if (r.noFont) {
        std::printf("[fontmetrics] %s (%s) - nothing to cook; text is measured with fixed metrics\n",
                    r.error.c_str(), WideToUtf8(assetsRoot).c_str());
        return 2;
    }
    std::printf("[fontmetrics] font = %s\n", WideToUtf8(r.fontPath).c_str());
    if (!r.ok) {
        std::printf("[fontmetrics] FAILED: %s\n", r.error.c_str());
        return 1;
    }
    std::printf("[fontmetrics] %s %s (%u glyphs, hash 0x%016llx)\n",
                r.unchanged ? "up to date:" : "wrote", WideToUtf8(r.outPath).c_str(), r.glyphs,
                static_cast<unsigned long long>(r.hash));
    return 0;
}

} // namespace uitext
} // namespace mye
