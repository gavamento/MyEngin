/*----
 ProjectShaderProperties.h  プロジェクトシェーダー Properties DSL の型定義・パース・パック
 作成者: 秋田蓮音                                09/22/2026
----*/
#pragma once
#include <array>
#include <filesystem>
#include <functional>
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

// M79: サーフェスマテリアル用。パース順オフセット (AssignCbOffsets) ではなく、
// 作者の cbuffer を実際にコンパイルした D3DReflect のオフセットで詰める
// (作者の MyEnginePerMaterial 宣言順が Properties ブロックと一致しなくてよい契約のため)。
// PackProperties (パース順) の意味は変えず、別関数として追加する。
struct ReflectedVarSlot
{
    uint32_t offset = 0;
    uint32_t size   = 0;
};
// reflectionVars: MyEnginePerMaterial の変数名 → cbuffer 内オフセット/サイズ (D3DReflect 由来)。
// cbSizeBytes   : 確保する CB 全体のバイト数 (D3DReflect の cbuffer サイズ。0 ならバッファ無し)。
// スキーマにあって reflectionVars に無い名前・サイズが合わない名前は missingOut に積む
// (ログはここでは出さない — 呼び出し側が MYE_LOG_WARN する)
bool PackPropertiesReflected(
    const PropertyParseResult&                              parsed,
    const std::unordered_map<std::string, PropValue>&       values,
    const std::unordered_map<std::string, ReflectedVarSlot>& reflectionVars,
    uint32_t                                                 cbSizeBytes,
    std::vector<uint8_t>&                                    cbData,
    std::vector<std::string>*                                missingOut = nullptr);

// M79 sub-04: マテリアル Inspector の ".mat.json" 内 "properties" ⇄ PropValue map の変換
// (JSON テキスト直渡し。ImGui に依存しないので AssetOpsSelfTest からヘッドレスに呼べる)。
//
// schema が非 null なら型ごとに復号する: Float/Range は常に float、Color/Vector は 4 要素配列、
// Tex2D は JSON 数値なら Material.texture/normalMap と同じ decimal 文字列 (std::to_string) へ、
// JSON 文字列ならそのまま保持する (組込み名 / 16 進 GUID / 相対パスを区別せず素通しする —
// 読み側の ResolveSurfaceTexProperty が同じ規則で解決する)。
// schema に無いキー / schema が null の場合は型を推測して復号する (旧 fxstack と同じ規則)。
void DecodeMaterialProperties(std::string_view propertiesJsonText, const PropertyParseResult* schema,
                              std::unordered_map<std::string, PropValue>& out);

// PropValue map → "properties" オブジェクトの JSON テキスト。
// **罠**: 文字列値が 1 桁以上の ASCII 数字だけで構成されるときは Tex2D の数値 GUID とみなし
// JSON 数値で書く (読み側は文字列を 16 進として読むため、10 進の GUID を文字列のまま書くと
// 次回ロードで別の値に化ける)。数字以外を含む文字列 (組込み名・16 進 GUID・相対パス) はそのまま
std::string EncodeMaterialProperties(const std::unordered_map<std::string, PropValue>& props);

// M79 sub-04 round 2: マテリアル Inspector でシェーダをコンボで切り替えるときの契約そのもの。
// スキーマ (= 型) が変わっても Properties の値は破棄しない (spec §4.1「シェーダを戻したとき
// 値が残る」契約)。表示時の型不一致は DrawPropertiesEditor の std::get_if が既定値へ安全に
// 逃がすので、ここでは名前を差し替えるだけでよい。InspectorWindow はこの関数を経由して
// シェーダ名を変えること — properties を直接 clear() する近道を作らないためのガード
void ApplyMaterialShaderSelection(std::string& shaderName, const std::string& newShaderName);

// M79d-fix (review-1 #5): shader 名 → Properties スキーマのキャッシュ。マテリアル Inspector
// (matSchemaCache_) と fxstack Inspector (FxStackEditState::schemaCache) の両方が使う。
// shader 名だけをキーにした素朴な map だと、ホットリロードで HLSL に Properties を足しても
// エディタ再起動まで古いスキーマを返し続ける (キャッシュを捨てる箇所が無かった)。
// ここではファイルの更新時刻を鍵にして、変わっていれば fetch() で取り直す
class PropertySchemaCache
{
public:
    // resolvedPath の更新時刻を前回取得時と比較し、変わっていれば fetch() で再取得して
    // キャッシュを差し替える。パスの更新時刻が取得できない (未検出・削除済み等) 場合は
    // 「取得できない」という状態自体をキャッシュせず、都度 fetch() し直す
    // (一度だけ失敗して以後ずっと空スキーマに固定される事故を避ける)。
    // ImGui に依存しないので SelfTest からヘッドレスに呼べる
    const PropertyParseResult& GetOrFetch(
        const std::string& shaderName, const std::filesystem::path& resolvedPath,
        const std::function<PropertyParseResult()>& fetch);

private:
    struct Entry
    {
        std::filesystem::file_time_type mtime{};
        bool                             mtimeValid = false;
        PropertyParseResult              schema;
    };
    std::unordered_map<std::string, Entry> entries_;
};

} // namespace mye
