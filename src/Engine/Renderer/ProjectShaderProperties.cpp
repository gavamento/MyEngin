/*----
 ProjectShaderProperties.cpp  プロジェクトシェーダー Properties DSL のパース・パック実装
 作成者: 秋田蓮音                                09/22/2026
----*/
#include "Engine/Renderer/ProjectShaderProperties.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <sstream>

namespace mye {
namespace {

// ---------------------------------------------------------------------------
// 文字列ユーティリティ
// ---------------------------------------------------------------------------

// 先頭/末尾のホワイトスペースを除去した view を返す
std::string_view Trim(std::string_view s)
{
    size_t l = 0;
    size_t r = s.size();
    while (l < r && std::isspace(static_cast<unsigned char>(s[l])))
        ++l;
    while (r > l && std::isspace(static_cast<unsigned char>(s[r - 1])))
        --r;
    return s.substr(l, r - l);
}

// s が prefix で始まるか
bool StartsWith(std::string_view s, std::string_view prefix)
{
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

// ---------------------------------------------------------------------------
// 数値パース
// ---------------------------------------------------------------------------

// s の先頭から float を読み、消費分だけ s を進める。失敗時は false
bool ParseFloat(std::string_view& s, float& out)
{
    s = Trim(s);
    if (s.empty()) return false;
    // std::string_view は null 終端保証なし → std::string を経由
    std::string tmp(s);
    char* end = nullptr;
    float val = std::strtof(tmp.c_str(), &end);
    if (end == tmp.c_str()) return false;
    size_t consumed = static_cast<size_t>(end - tmp.c_str());
    out = val;
    s = s.substr(consumed);
    return true;
}

// ---------------------------------------------------------------------------
// ブロック抽出
// ---------------------------------------------------------------------------

static constexpr std::string_view kBlockOpen  = "/*@MyEngineProperties";
static constexpr std::string_view kBlockClose = "@*/";

// ブロック抽出結果
enum class BlockStatus
{
    None,          // /*@MyEngineProperties が見つからない (プロパティ無し = 正常)
    Found,         // ブロックを抽出できた
    UnclosedBlock, // /*@MyEngineProperties はあるが @*/ が無い (エラー)
};

BlockStatus ExtractBlock(std::string_view src, std::string_view& out)
{
    auto openPos = src.find(kBlockOpen);
    if (openPos == std::string_view::npos)
        return BlockStatus::None;

    auto closePos = src.find(kBlockClose, openPos + kBlockOpen.size());
    if (closePos == std::string_view::npos)
        return BlockStatus::UnclosedBlock;

    size_t bodyStart = openPos + kBlockOpen.size();
    out = src.substr(bodyStart, closePos - bodyStart);
    return BlockStatus::Found;
}

// ---------------------------------------------------------------------------
// 属性パース: "[Attr1][Attr2]..." を読んで attr に追記し、s を残余まで進める
// 閉じ ] が無い場合は false
// ---------------------------------------------------------------------------
bool ParseAttributes(std::string_view& s, PropAttr& attr)
{
    while (true)
    {
        s = Trim(s);
        if (!StartsWith(s, "[")) break;

        auto closeB = s.find(']');
        if (closeB == std::string_view::npos)
            return false; // 閉じ ] なし

        std::string_view inner = Trim(s.substr(1, closeB - 1));
        s = s.substr(closeB + 1);

        if (StartsWith(inner, "Range("))
        {
            // [Range(min, max)]
            std::string_view args = inner.substr(6); // "min, max)"
            auto paren = args.rfind(')');
            if (paren == std::string_view::npos) return false;
            args = args.substr(0, paren);
            float mn = 0.0f, mx = 1.0f;
            std::string_view sv = args;
            if (!ParseFloat(sv, mn)) return false;
            sv = Trim(sv);
            if (!StartsWith(sv, ",")) return false;
            sv = sv.substr(1);
            if (!ParseFloat(sv, mx)) return false;
            attr.hasRange = true;
            attr.rangeMin = mn;
            attr.rangeMax = mx;
        }
        else if (StartsWith(inner, "Header("))
        {
            // [Header(ラベル名)]
            std::string_view txt = inner.substr(7);
            auto paren = txt.rfind(')');
            if (paren == std::string_view::npos) return false;
            attr.header = std::string(Trim(txt.substr(0, paren)));
        }
        else if (inner == "HDR")
        {
            attr.isHDR = true;
        }
        else if (inner == "HideInInspector")
        {
            attr.hideInInspector = true;
        }
        else if (inner == "NoScaleOffset")
        {
            attr.noScaleOffset = true;
        }
        // 未知の属性は将来の拡張に備えてスキップ (警告なし)
    }
    return true;
}

// ---------------------------------------------------------------------------
// デフォルト値パース
// ---------------------------------------------------------------------------

// Float/Range 用: "0.5"
bool ParseDefaultFloat(std::string_view s, float& out)
{
    s = Trim(s);
    return ParseFloat(s, out);
}

// Color/Vector 用: "(1, 1, 1, 1)"
bool ParseDefaultVec4(std::string_view s, std::array<float, 4>& out)
{
    s = Trim(s);
    if (!StartsWith(s, "(")) return false;
    s = s.substr(1);
    for (int i = 0; i < 4; ++i)
    {
        float v = 0.0f;
        if (!ParseFloat(s, v)) return false;
        out[static_cast<size_t>(i)] = v;
        s = Trim(s);
        if (i < 3)
        {
            if (!StartsWith(s, ",")) return false;
            s = s.substr(1);
        }
    }
    s = Trim(s);
    return StartsWith(s, ")");
}

// Tex2D 用: '"white" {}' のうち " ... " 部分を読む
bool ParseDefaultTex(std::string_view s, std::string& out)
{
    s = Trim(s);
    if (!StartsWith(s, "\"")) return false;
    s = s.substr(1);
    auto q = s.find('"');
    if (q == std::string_view::npos) return false;
    out = std::string(s.substr(0, q));
    return true;
}

// ---------------------------------------------------------------------------
// 1 行のプロパティパース
// "[attrs] _Name ("Display", Type) = default"
// 存在しない場合や形式が違う場合は false
// ---------------------------------------------------------------------------
bool ParsePropertyLine(std::string_view line, PropertySchema& out)
{
    std::string_view s = Trim(line);
    if (s.empty()) return false;

    // 行内の属性を読む
    PropAttr attr;
    if (!ParseAttributes(s, attr)) return false;
    s = Trim(s);

    // プロパティ名 (_xxx)
    if (s.empty() || s[0] != '_') return false;
    size_t nameEnd = 0;
    while (nameEnd < s.size() &&
           (std::isalnum(static_cast<unsigned char>(s[nameEnd])) || s[nameEnd] == '_'))
    {
        ++nameEnd;
    }
    std::string name = std::string(s.substr(0, nameEnd));
    s = Trim(s.substr(nameEnd));

    // "("
    if (!StartsWith(s, "(")) return false;
    s = s.substr(1);

    // 表示名 ("...")"
    s = Trim(s);
    if (!StartsWith(s, "\"")) return false;
    s = s.substr(1);
    auto q = s.find('"');
    if (q == std::string_view::npos) return false;
    std::string displayName = std::string(s.substr(0, q));
    s = s.substr(q + 1);

    // ","
    s = Trim(s);
    if (!StartsWith(s, ",")) return false;
    s = s.substr(1);
    s = Trim(s);

    // 型宣言
    PropType type;
    if (StartsWith(s, "Range("))
    {
        // 型として Range(a,b) を使う
        type = PropType::Range;
        std::string_view rangeStr = s.substr(6);
        auto paren = rangeStr.find(')');
        if (paren == std::string_view::npos) return false;
        std::string_view args = rangeStr.substr(0, paren);
        s = s.substr(6 + paren + 1);
        float mn = 0.0f, mx = 1.0f;
        std::string_view sv = args;
        if (!ParseFloat(sv, mn)) return false;
        sv = Trim(sv);
        if (!StartsWith(sv, ",")) return false;
        sv = sv.substr(1);
        if (!ParseFloat(sv, mx)) return false;
        attr.hasRange = true;
        attr.rangeMin = mn;
        attr.rangeMax = mx;
    }
    else if (StartsWith(s, "Float"))
    {
        type = PropType::Float;
        s = s.substr(5);
    }
    else if (StartsWith(s, "Color"))
    {
        type = PropType::Color;
        s = s.substr(5);
    }
    else if (StartsWith(s, "Vector"))
    {
        type = PropType::Vector;
        s = s.substr(6);
    }
    else if (StartsWith(s, "2D"))
    {
        type = PropType::Tex2D;
        s = s.substr(2);
    }
    else
    {
        // 未知の型 → 行全体をエラー扱い
        return false;
    }

    // ")"
    s = Trim(s);
    if (!StartsWith(s, ")")) return false;
    s = s.substr(1);

    // "="
    s = Trim(s);
    if (!StartsWith(s, "=")) return false;
    s = s.substr(1);
    s = Trim(s);

    // デフォルト値
    float                defFloat = 0.0f;
    std::array<float, 4> defVec   = {};
    std::string          defTex;

    switch (type)
    {
    case PropType::Float:
    case PropType::Range:
        if (!ParseDefaultFloat(s, defFloat)) return false;
        break;
    case PropType::Color:
    case PropType::Vector:
        if (!ParseDefaultVec4(s, defVec)) return false;
        break;
    case PropType::Tex2D:
        if (!ParseDefaultTex(s, defTex)) return false;
        break;
    }

    out.name        = std::move(name);
    out.displayName = std::move(displayName);
    out.type        = type;
    out.attr        = attr;
    out.defaultFloat = defFloat;
    out.defaultVec4  = defVec;
    out.defaultTex   = std::move(defTex);
    return true;
}

// ---------------------------------------------------------------------------
// CB オフセット割り当て (HLSL 既定パックルール)
//   float/Range : 4 bytes、16 バイト境界を跨がないように充填
//   Color/Vector: 16 bytes、常に 16 バイト境界から開始
//   Tex2D       : CB 対象外 (cbOffset = -1)
// ---------------------------------------------------------------------------
static int AlignUp(int offset, int align)
{
    return (offset + align - 1) & ~(align - 1);
}

void AssignCbOffsets(std::vector<PropertySchema>& props, int& totalBytes)
{
    int offset = 0;
    for (auto& p : props)
    {
        // Header-only 行 (name が空) は CB に含まない
        if (p.name.empty())
        {
            p.cbOffset = -1;
            continue;
        }

        switch (p.type)
        {
        case PropType::Float:
        case PropType::Range:
        {
            // 残り余地が 4 バイト未満なら次の 16 バイトレジスタへ進める
            int rem = 16 - (offset % 16);
            if (rem < 4) offset = AlignUp(offset, 16);
            p.cbOffset = offset;
            offset += 4;
            break;
        }
        case PropType::Color:
        case PropType::Vector:
            // 常に 16 バイト境界から開始
            offset = AlignUp(offset, 16);
            p.cbOffset = offset;
            offset += 16;
            break;
        case PropType::Tex2D:
            p.cbOffset = -1; // CB 対象外
            break;
        }
    }
    // 定数バッファは 16 バイトの倍数に切り上げ
    totalBytes = (offset > 0) ? AlignUp(offset, 16) : 0;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// 公開 API
// ---------------------------------------------------------------------------

PropertyParseResult ParseProperties(std::string_view hlslSource)
{
    PropertyParseResult result;

    // ブロック抽出
    std::string_view block;
    BlockStatus status = ExtractBlock(hlslSource, block);
    switch (status)
    {
    case BlockStatus::None:
        // ブロック無し = プロパティ無し (正常)
        result.ok = true;
        return result;
    case BlockStatus::UnclosedBlock:
        result.ok = false;
        result.errorMessage = "Properties ブロックが閉じていません (@*/ が見つかりません)";
        return result;
    case BlockStatus::Found:
        break;
    }

    // ブロック内を行に分割してパース
    std::string blockStr(block);
    std::istringstream ss(blockStr);
    std::string rawLine;
    while (std::getline(ss, rawLine))
    {
        std::string_view sv = Trim(std::string_view(rawLine));
        if (sv.empty()) continue;

        // "[Header(xxx)]" だけの行か判定
        if (StartsWith(sv, "["))
        {
            PropAttr tempAttr;
            std::string_view tmp = sv;
            if (ParseAttributes(tmp, tempAttr))
            {
                tmp = Trim(tmp);
                if (tmp.empty())
                {
                    // Header-only 行: 名前空の PropertySchema として記録
                    if (!tempAttr.header.empty())
                    {
                        PropertySchema hdr;
                        hdr.attr     = tempAttr;
                        hdr.cbOffset = -1;
                        result.properties.push_back(hdr);
                    }
                    continue;
                }
            }
        }

        // 通常のプロパティ行
        PropertySchema prop;
        if (!ParsePropertyLine(sv, prop))
        {
            // パース失敗 = ブロック全体を無効化
            result.ok = false;
            result.errorMessage = "プロパティ行のパース失敗: " + std::string(sv);
            result.properties.clear();
            return result;
        }
        result.properties.push_back(prop);
    }

    // CB オフセットを割り当て
    AssignCbOffsets(result.properties, result.cbSizeBytes);
    result.ok = true;
    return result;
}

bool PackProperties(
    const PropertyParseResult&                         parsed,
    const std::unordered_map<std::string, PropValue>&  values,
    std::vector<uint8_t>&                              cbData)
{
    if (!parsed.ok) return false;

    if (parsed.cbSizeBytes == 0)
    {
        cbData.clear();
        return true;
    }

    // ゼロ初期化してから各プロパティを書き込む
    cbData.assign(static_cast<size_t>(parsed.cbSizeBytes), 0);

    for (const auto& p : parsed.properties)
    {
        if (p.cbOffset < 0) continue;

        auto it = values.find(p.name);

        switch (p.type)
        {
        case PropType::Float:
        case PropType::Range:
        {
            float val = p.defaultFloat;
            if (it != values.end() && std::holds_alternative<float>(it->second))
                val = std::get<float>(it->second);
            std::memcpy(cbData.data() + p.cbOffset, &val, sizeof(float));
            break;
        }
        case PropType::Color:
        case PropType::Vector:
        {
            auto val = p.defaultVec4;
            if (it != values.end() &&
                std::holds_alternative<std::array<float, 4>>(it->second))
            {
                val = std::get<std::array<float, 4>>(it->second);
            }
            std::memcpy(cbData.data() + p.cbOffset, val.data(), 4 * sizeof(float));
            break;
        }
        case PropType::Tex2D:
            break; // CB 対象外
        }
    }

    return true;
}

} // namespace mye
