/*----
 ProjectShaderProperties.h  プロジェクトシェーダー Properties DSL の型定義・パース・パック
 作成者: 秋田蓮音                                09/22/2026
----*/
#pragma once
#include <array>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace mye {

// --- Properties DSL の型 ---

// プロパティの種別
enum class PropType
{
    Float,   // スカラー (CB: 4 bytes)
    Range,   // スカラー + min/max 制約 (CB: 4 bytes)
    Color,   // float4 (CB: 16 bytes, 16 バイト境界)
    Vector,  // float4 (CB: 16 bytes, 16 バイト境界)
    Tex2D,   // テクスチャ (CB には含まない)
};

// プロパティの属性 (角括弧で宣言するもの)
struct PropAttr
{
    // [Range(min, max)] 属性 or Range型として宣言した場合
    bool  hasRange        = false;
    float rangeMin        = 0.0f;
    float rangeMax        = 1.0f;

    bool  isHDR           = false;
    bool  hideInInspector = false;
    bool  noScaleOffset   = false;

    // [Header(name)] → Inspector 上の区切り見出し (空 = 非 Header 行)
    std::string header;
};

// プロパティ 1 件のスキーマ
struct PropertySchema
{
    std::string  name;        // 例: _Intensity
    std::string  displayName; // 例: "Intensity"
    PropType     type         = PropType::Float;
    PropAttr     attr;

    // 既定値
    float                defaultFloat = 0.0f;
    std::array<float, 4> defaultVec4  = {};
    std::string          defaultTex;   // Tex2D 用 ("white"/"black" etc.)

    // PackProperties 後に設定される CB バイトオフセット (-1 = CB 対象外)
    int cbOffset = -1;
};

// パース結果
struct PropertyParseResult
{
    bool                        ok           = false;
    std::string                 errorMessage;
    std::vector<PropertySchema> properties;
    // CB 全体のバイトサイズ (16 バイト倍数に切り上げ済み)
    int cbSizeBytes = 0;
};

// HLSL ソーステキストから /*@MyEngineProperties ... @*/ ブロックを抽出してパース。
// ブロックが存在しない場合は空プロパティで ok=true を返す。
// 閉じ漏れ・未知の型は ok=false + errorMessage を返し、例外を投げない。
PropertyParseResult ParseProperties(std::string_view hlslSource);

// パース済みスキーマと名前→値辞書から定数バッファのバイト列を生成。
// values に無いプロパティは既定値を使う。Tex2D はスキップ。
// parsed.ok が false の場合は何もせず false を返す。
// PropValue: float = スカラー, array<float,4> = Color/Vector,
//            string = Tex2D のアセットパス/ビルトイン名 (CB 対象外)
using PropValue = std::variant<float, std::array<float, 4>, std::string>;
bool PackProperties(
    const PropertyParseResult&                         parsed,
    const std::unordered_map<std::string, PropValue>&  values,
    std::vector<uint8_t>&                              cbData);

} // namespace mye
