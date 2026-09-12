//====================================================================================
//                          UITextMetrics.cpp
//  MyEngine/ 秋田蓮音                                                      09/13/2026
//                                          フォント計測表の読み書きと文字列計測の実装
//====================================================================================
#include "Engine/Engine/UI/UITextMetrics.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/FontFiles.h"
#include "Engine/Renderer/FontGeometry.h"

namespace mye {
namespace uitext {
namespace {

constexpr uint16_t kAbsent = 0xFFFF;

// 直列化の区切り。同値が kUniformMin 文字以上続けば「長さ 1 の配列」の区間に畳み、
// それ以外は kChunkMax 文字ごとに 1 行 (差分を行単位で読めるように)
constexpr size_t kUniformMin = 8;
constexpr size_t kChunkMax = 32;

// 表のハッシュの種。形式が変わったら文字列ごと変える (同じ組でも別物として弾く)
constexpr const char* kHashDomain = "mye.uitext.fontmetrics.v1";

void SetError(std::string* error, const std::string& what)
{
    if (error) {
        *error = what;
    }
}

// JSON の整数欄を読む (無い / 整数でない / 範囲外は false)
bool ReadInt(const nlohmann::json& obj, const char* key, int64_t lo, int64_t hi, int64_t& out)
{
    if (!obj.contains(key) || !obj[key].is_number_integer()) {
        return false;
    }
    // 巨大な符号なし値は int64 へ入らないので先に弾く
    if (obj[key].is_number_unsigned()
        && obj[key].get<uint64_t>() > static_cast<uint64_t>(INT64_MAX)) {
        return false;
    }
    const int64_t v = obj[key].get<int64_t>();
    if (v < lo || v > hi) {
        return false;
    }
    out = v;
    return true;
}

// 配列要素版 (ranges の [start, end, [adv...]] 用)
bool ElemInt(const nlohmann::json& v, int64_t lo, int64_t hi, int64_t& out)
{
    if (!v.is_number_integer()) {
        return false;
    }
    if (v.is_number_unsigned() && v.get<uint64_t>() > static_cast<uint64_t>(INT64_MAX)) {
        return false;
    }
    const int64_t x = v.get<int64_t>();
    if (x < lo || x > hi) {
        return false;
    }
    out = x;
    return true;
}

std::string JsonString(const std::string& s)
{
    // 不正な UTF-8 で例外を投げさせない (ファイル名由来の文字列しか来ないが、直列化は落とさない)
    return nlohmann::json(s).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

FontMetrics g_activeMetrics; // 起動時に 1 回だけ書く (SetActiveFontMetrics)

} // namespace

bool FontMetrics::Has(uint32_t cp) const
{
    return cp <= kMaxCodepoint && !advances_.empty() && advances_[cp] != kAbsent;
}

uint16_t FontMetrics::AdvanceOf(uint32_t cp) const
{
    if (advances_.empty()) {
        return kFixedAdvance;
    }
    if (cp <= kMaxCodepoint && advances_[cp] != kAbsent) {
        return advances_[cp];
    }
    // 表にあるフォントで描けない文字は描画側 (UIRenderer::PushTextLine) が '?' で描く
    const uint16_t q = advances_[static_cast<uint32_t>('?')];
    return (q != kAbsent) ? q : kFixedAdvance;
}

std::vector<GlyphAdvance> FontMetrics::Glyphs() const
{
    std::vector<GlyphAdvance> out;
    if (advances_.empty()) {
        return out;
    }
    out.reserve(glyphCount_);
    for (uint32_t cp = 0; cp <= kMaxCodepoint; ++cp) {
        if (advances_[cp] != kAbsent) {
            out.push_back({ cp, advances_[cp] });
        }
    }
    return out;
}

bool FontMetrics::Build(const std::string& fontName, uint64_t fontBytes, int32_t basePx,
                        int32_t lineH256, const std::vector<GlyphAdvance>& glyphs, FontMetrics& out,
                        std::string* error)
{
    out = FontMetrics{};
    std::vector<uint16_t> table(static_cast<size_t>(kMaxCodepoint) + 1, kAbsent);
    // 畳む順 = codepoint 昇順 (入力の並びをそのまま使えるのは狭義昇順を検査しているから)
    std::vector<uint8_t> bytes;
    bytes.reserve(glyphs.size() * 6);
    uint32_t prev = 0;
    for (size_t i = 0; i < glyphs.size(); ++i) {
        const GlyphAdvance& g = glyphs[i];
        if (g.codepoint > kMaxCodepoint) {
            SetError(error, "codepoint outside the BMP");
            return false;
        }
        if (i > 0 && g.codepoint <= prev) {
            SetError(error, "codepoints must be strictly ascending");
            return false;
        }
        if (g.advance > kAdvanceMax) {
            SetError(error, "advance out of range");
            return false;
        }
        table[g.codepoint] = g.advance;
        prev = g.codepoint;
        // エンディアンを明示して畳む (x64 限定だが、線に載る値なので暗黙に頼らない)
        bytes.push_back(static_cast<uint8_t>(g.codepoint & 0xFF));
        bytes.push_back(static_cast<uint8_t>((g.codepoint >> 8) & 0xFF));
        bytes.push_back(static_cast<uint8_t>((g.codepoint >> 16) & 0xFF));
        bytes.push_back(static_cast<uint8_t>((g.codepoint >> 24) & 0xFF));
        bytes.push_back(static_cast<uint8_t>(g.advance & 0xFF));
        bytes.push_back(static_cast<uint8_t>((g.advance >> 8) & 0xFF));
    }
    if (glyphs.empty()) {
        // 0 文字の表は「表なし」と区別がつかない (ハッシュ 0 と衝突する) ので作らせない
        SetError(error, "no glyphs");
        return false;
    }
    uint64_t h = HashBytes(bytes.data(), bytes.size(), HashStr(kHashDomain));
    if (h == 0) {
        h = 1; // 0 は「表なし」の予約
    }
    out.advances_ = std::move(table);
    out.fontName_ = fontName;
    out.fontBytes_ = fontBytes;
    out.basePx_ = basePx;
    out.lineH256_ = lineH256;
    out.glyphCount_ = static_cast<uint32_t>(glyphs.size());
    out.hash_ = h;
    return true;
}

bool ParseFontMetricsJson(const std::string& jsonText, FontMetrics& out, std::string* error)
{
    out = FontMetrics{};
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(jsonText);
    } catch (const nlohmann::json::exception& ex) {
        SetError(error, std::string("parse error: ") + ex.what());
        return false;
    }
    if (!root.is_object()) {
        SetError(error, "root is not an object");
        return false;
    }
    int64_t format = 0;
    if (!ReadInt(root, "format", 0, INT32_MAX, format) || format != kFormatVersion) {
        SetError(error, "unsupported format (expected 1)");
        return false;
    }
    int64_t advPerLine = 0;
    if (!ReadInt(root, "advPerLine", 0, INT32_MAX, advPerLine) || advPerLine != kAdvPerLine) {
        SetError(error, "advPerLine must be 1000");
        return false;
    }
    if (!root.contains("font") || !root["font"].is_string()) {
        SetError(error, "font must be a string");
        return false;
    }
    int64_t fontBytes = 0;
    int64_t basePx = 0;
    int64_t lineH256 = 0;
    if (!ReadInt(root, "fontBytes", 0, INT64_MAX, fontBytes)
        || !ReadInt(root, "basePx", 0, INT32_MAX, basePx)
        || !ReadInt(root, "lineH256", 0, INT32_MAX, lineH256)) {
        SetError(error, "fontBytes / basePx / lineH256 must be non-negative integers");
        return false;
    }
    if (!root.contains("ranges") || !root["ranges"].is_array()) {
        SetError(error, "ranges must be an array");
        return false;
    }
    std::vector<GlyphAdvance> glyphs;
    int64_t lastEnd = -1;
    for (const nlohmann::json& r : root["ranges"]) {
        int64_t start = 0;
        int64_t end = 0;
        if (!r.is_array() || r.size() != 3 || !ElemInt(r[0], 0, kMaxCodepoint, start)
            || !ElemInt(r[1], 0, kMaxCodepoint, end) || !r[2].is_array()) {
            SetError(error, "each range must be [start, end, [advances]] within the BMP");
            return false;
        }
        // ★区間は昇順・非重複に限る。重なりを「後勝ち」で読むと、表の見た目と中身が食い違う
        if (start > end || start <= lastEnd) {
            SetError(error, "ranges must be ascending and must not overlap");
            return false;
        }
        const size_t count = static_cast<size_t>(end - start + 1);
        const nlohmann::json& advs = r[2];
        if (advs.size() != 1 && advs.size() != count) {
            SetError(error, "advance array must have 1 or (end - start + 1) entries");
            return false;
        }
        for (size_t i = 0; i < count; ++i) {
            int64_t adv = 0;
            if (!ElemInt(advs[advs.size() == 1 ? 0 : i], 0, kAdvanceMax, adv)) {
                SetError(error, "advance must be an integer in [0, 65534]");
                return false;
            }
            glyphs.push_back({ static_cast<uint32_t>(start + static_cast<int64_t>(i)),
                               static_cast<uint16_t>(adv) });
        }
        lastEnd = end;
    }
    return FontMetrics::Build(root["font"].get<std::string>(), static_cast<uint64_t>(fontBytes),
                              static_cast<int32_t>(basePx), static_cast<int32_t>(lineH256), glyphs,
                              out, error);
}

std::string SerializeFontMetricsJson(const FontMetrics& metrics)
{
    const std::vector<GlyphAdvance> g = metrics.Glyphs();
    std::ostringstream os;
    os << "{\n";
    os << "  \"format\": " << kFormatVersion << ",\n";
    os << "  \"font\": " << JsonString(metrics.FontName()) << ",\n";
    os << "  \"fontBytes\": " << metrics.FontBytes() << ",\n";
    os << "  \"basePx\": " << metrics.BasePx() << ",\n";
    os << "  \"lineH256\": " << metrics.LineH256() << ",\n";
    os << "  \"advPerLine\": " << kAdvPerLine << ",\n";
    os << "  \"ranges\": [";

    bool first = true;
    auto emit = [&](size_t from, size_t to, bool uniform) { // [from, to) の文字
        os << (first ? "\n    [" : ",\n    [") << g[from].codepoint << ", " << g[to - 1].codepoint
           << ", [";
        if (uniform) {
            os << g[from].advance;
        } else {
            for (size_t i = from; i < to; ++i) {
                os << (i == from ? "" : ", ") << g[i].advance;
            }
        }
        os << "]]";
        first = false;
    };
    // 位置 e から始まる同値の並びが kUniformMin 文字以上あるか (連続区間 [.., end) の中で)
    auto uniformAt = [&](size_t e, size_t end) {
        size_t r = e + 1;
        while (r < end && g[r].advance == g[e].advance && r - e < kUniformMin) {
            ++r;
        }
        return r - e >= kUniformMin;
    };

    size_t i = 0;
    while (i < g.size()) {
        // codepoint が連続する区間 [i, j)
        size_t j = i + 1;
        while (j < g.size() && g[j].codepoint == g[j - 1].codepoint + 1) {
            ++j;
        }
        size_t k = i;
        while (k < j) {
            if (uniformAt(k, j)) {
                size_t r = k + 1;
                while (r < j && g[r].advance == g[k].advance) {
                    ++r;
                }
                emit(k, r, true);
                k = r;
                continue;
            }
            // 明示配列: 最大 kChunkMax 文字。途中で同値の長い並びが始まるならその手前で切る
            size_t e = k + 1;
            while (e < j && e - k < kChunkMax && !uniformAt(e, j)) {
                ++e;
            }
            emit(k, e, false);
            k = e;
        }
        i = j;
    }
    os << (first ? "]\n" : "\n  ]\n");
    os << "}\n";
    return os.str();
}

std::wstring FontMetricsPathFor(const std::wstring& fontPath)
{
    std::filesystem::path p(fontPath);
    p.replace_extension(L".fontmetrics.json");
    return p.wstring();
}

FontMetrics LoadProjectFontMetrics(const std::wstring& assetsRoot)
{
    const std::vector<std::wstring> fonts = fontfiles::ListProjectFontFiles(assetsRoot);
    if (fonts.empty()) {
        return {}; // システムフォント / 内蔵 8x8 で描く = 表は作れない。固定メトリクス
    }
    const std::filesystem::path fontPath(fonts.front());
    const std::string fontFile = WideToUtf8(fontPath.filename().wstring());
    const std::wstring tablePath = FontMetricsPathFor(fonts.front());
    const std::string tableFile = WideToUtf8(std::filesystem::path(tablePath).filename().wstring());

    std::ifstream f(std::filesystem::path(tablePath), std::ios::binary);
    if (!f) {
        MYE_LOG_WARN("[ui] font metrics: %s has no %s - text is measured with fixed metrics "
                     "(0.8 line per glyph). Cook it with Editor.exe --cook-font-metrics "
                     "(or Project Settings > UI) and commit the file",
                     fontFile.c_str(), tableFile.c_str());
        return {};
    }
    std::stringstream ss;
    ss << f.rdbuf();
    FontMetrics m;
    std::string error;
    if (!ParseFontMetricsJson(ss.str(), m, &error)) {
        MYE_LOG_WARN("[ui] font metrics: %s is broken (%s) - using fixed metrics", tableFile.c_str(),
                     error.c_str());
        return {};
    }
    // 表は読む (コミットされた内容がそのまま sim の入力 = 決定論は保たれる)。食い違いは見た目の問題
    std::error_code ec;
    const uint64_t actualBytes = static_cast<uint64_t>(std::filesystem::file_size(fontPath, ec));
    if (m.FontName() != fontFile || (!ec && m.FontBytes() != actualBytes)) {
        MYE_LOG_WARN("[ui] font metrics: %s was cooked from %s (%llu bytes) but the atlas font is "
                     "%s (%llu bytes) - layout boxes may not match the glyphs; re-cook it",
                     tableFile.c_str(), m.FontName().c_str(),
                     static_cast<unsigned long long>(m.FontBytes()), fontFile.c_str(),
                     static_cast<unsigned long long>(actualBytes));
    }
    return m;
}

void SetActiveFontMetrics(FontMetrics metrics)
{
    g_activeMetrics = std::move(metrics);
}

const FontMetrics& ActiveFontMetrics()
{
    return g_activeMetrics;
}

TextSize Measure(const char* utf8, float fontScale, bool wrap, float maxW, const FontMetrics& metrics)
{
    TextSize out;
    if (!utf8) {
        return out;
    }
    const float lineScale = kLineH * fontScale;
    const float denom = static_cast<float>(kAdvPerLine);
    // 整数の送り幅の和 → キャンバス単位。(和 × 行高) / 1000 の順で割る — 8x8 相当の値
    // (800 × n × 行高) は途中が整数値のまま進むので、textlayout の float 積算と同じ値になる
    const auto toWidth = [&](uint64_t adv) {
        return static_cast<float>(adv) * lineScale / denom;
    };
    const bool doWrap = wrap && maxW > 0.0f;

    uint64_t cur = 0;
    uint64_t widest = 0;
    int32_t lines = 0;
    bool lineHasGlyph = false;
    const char* lineBegin = utf8;
    const char* p = utf8;
    const auto closeLine = [&]() {
        ++lines;
        widest = std::max(widest, cur);
        cur = 0;
        lineHasGlyph = false;
    };
    for (;;) {
        const char* cpStart = p;
        const uint32_t cp = fontgeom::Utf8Next(p);
        if (cp == 0) {
            // 末尾の空行は数えない (textlayout と同じ。制御文字だけの行は数える)
            if (lineBegin != cpStart) {
                closeLine();
            }
            break;
        }
        if (cp == '\n') {
            closeLine(); // 空行も 1 行 ("a\n\nb" の中間行)
            lineBegin = p;
            continue;
        }
        if (cp < 0x20) {
            continue; // 制御文字は幅 0
        }
        const uint64_t adv = metrics.AdvanceOf(cp);
        if (doWrap && lineHasGlyph && toWidth(cur + adv) > maxW) {
            closeLine(); // この文字の直前で折る
            lineBegin = cpStart;
        }
        cur += adv;
        lineHasGlyph = true;
    }
    out.lines = lines;
    out.w = toWidth(widest);
    out.h = static_cast<float>(lines) * lineScale;
    return out;
}

} // namespace uitext
} // namespace mye
