//====================================================================================
//                          UITextMetrics.h
//  MyEngine/ 秋田蓮音                                                      09/13/2026
//                                          sim が読むフォント計測表（送り幅の表と文字列の計測）
//====================================================================================
#pragma once
// フォント計測表 (M75d)。Layout Group / ContentSizeFitter / InputField のキャレットは
// **sim の中で**テキスト幅を要する (HitTest が矩形を解くため) が、実グリフの送り幅は
// その機械の TTF と stb_truetype の浮動小数の計算に依存するので sim へ入れられない。
// そこでプロジェクトフォントの送り幅を**整数の表**にしてアセットとしてコミットし、sim はそれだけを読む。
//
//   assets\fonts\<フォント名の stem>.fontmetrics.json
//   {"format":1, "font":"X.ttf", "fontBytes":N, "basePx":32, "lineH256":N, "advPerLine":1000,
//    "ranges":[[start, end, [adv, ...]], ...]}
//
// * adv は「行高を 1000 とした送り幅」の整数 (uint16)。cook 時に**切り上げ**て作る =
//   表で測った幅は実グリフで描いた幅を下回らない (箱に合わせた文字が最後の 1 文字で折り返らない)。
//   JSON に float を書かないのはパーサ実装差を持ち込まないため。
// * ranges の adv 配列は長さ end-start+1 (1 文字ずつ) か 長さ 1 (区間全体が同じ値) のどちらか。
//   CJK の全角ブロックは 1 行に畳まれる。
// * 表の選択は**描画のフォントと同じ規則** (fontfiles::ListProjectFontFiles の先頭) の stem。
//   エンジンリポジトリへは倒さない — FontAtlas もエンジンリポジトリのフォントを見ないので、
//   倒すと「絵のフォント」と「表のフォント」がずれる。
// * 表が無い / 文字が表に無いときは下の固定メトリクス。
//
// ★読むのは EngineLoop の起動時 1 回だけ (SetActiveFontMetrics)。途中で差し替えると
//   タイムトラベルの再シムや .rep の検証が割れる (基準解像度 = M75c と同じ扱い)。.rep には載せず、
//   ネットは NetIdentity.fontMetricsHash で入口照合する。
// ★描画 (UIRenderer の折返し) は今も実グリフ幅で組む。この表は sim 側の「箱の寸法」専用。
#include <cstdint>
#include <string>
#include <vector>

namespace mye {
namespace uitext {

// fontScale=1 の行高 (キャンバス単位)。FontAtlas::kUILineH と同値 (UIRenderer.cpp の static_assert)。
// ここで FontAtlas.h を include しないのは、この表を D3D 非依存の純データに保つため
inline constexpr float kLineH = 10.0f;

// 送り幅の分母 (行高 = 1000)
inline constexpr uint32_t kAdvPerLine = 1000;

// 固定メトリクス: **全文字 0.8 行**。内蔵 8x8 フォント (advance 8 px / 行高 10 px。ASCII 外は
// '?' で描く) と厳密に同じ = `--font-embedded` で撮るエンジンリポジトリの golden では、
// 表なしでも箱と文字が 1 画素もずれない。プロポーショナルな TTF に対しては広め (ASCII は
// 実幅の 2 倍近い) に倒れるが、狭く見積もって勝手に折り返すよりは安全
inline constexpr uint16_t kFixedAdvance = 800;

inline constexpr uint32_t kMaxCodepoint = 0xFFFF;  // 表は BMP のみ (それ以外は固定メトリクス)
inline constexpr uint16_t kAdvanceMax = 0xFFFE;    // 0xFFFF は「表に無い」の番兵
inline constexpr int32_t kFormatVersion = 1;

// cook が作る 1 文字分
struct GlyphAdvance {
    uint32_t codepoint = 0;
    uint16_t advance = 0; // 行高 = kAdvPerLine
};

// 計測表。空 (= 表なし) が既定値
class FontMetrics {
public:
    // 表が無い (固定メトリクスで測る)
    bool Empty() const { return advances_.empty(); }
    // cp が表に載っているか (BMP 外と表なしは常に false)
    bool Has(uint32_t cp) const;
    // 計測に使う送り幅。表 → 表の '?' (描画側も無い文字は '?' で描く) → 固定メトリクスの順
    uint16_t AdvanceOf(uint32_t cp) const;

    // sim が読む値 (文字と送り幅の組) だけを畳んだ FNV。0 = 表なし。
    // ファイルのバイト列を畳まないのは、core.autocrlf で改行が変わっただけの 2 台を弾かないため
    uint64_t Hash() const { return hash_; }

    const std::string& FontName() const { return fontName_; }
    uint64_t FontBytes() const { return fontBytes_; }
    int32_t BasePx() const { return basePx_; }
    int32_t LineH256() const { return lineH256_; }
    uint32_t GlyphCount() const { return glyphCount_; }

    // 表に載っている文字を codepoint 昇順で返す (直列化とセルフテスト用)
    std::vector<GlyphAdvance> Glyphs() const;

    // glyphs (codepoint 狭義昇順・BMP 内・advance <= kAdvanceMax) から作る。
    // 規則違反は false (out は空のまま)。fontBytes / basePx / lineH256 は情報欄 (ハッシュに入らない)
    static bool Build(const std::string& fontName, uint64_t fontBytes, int32_t basePx,
                      int32_t lineH256, const std::vector<GlyphAdvance>& glyphs, FontMetrics& out,
                      std::string* error = nullptr);

private:
    std::vector<uint16_t> advances_; // kMaxCodepoint+1 要素 (0xFFFF = 無い) か空
    std::string fontName_;
    uint64_t fontBytes_ = 0;
    int32_t basePx_ = 0;
    int32_t lineH256_ = 0;
    uint32_t glyphCount_ = 0;
    uint64_t hash_ = 0;
};

// .fontmetrics.json の本文を読む (ファイル I/O 抜きの本体 = セルフテストの口)。
// 規則違反は false + error (out は空)
bool ParseFontMetricsJson(const std::string& jsonText, FontMetrics& out, std::string* error = nullptr);

// 直列化。**同じ表からは常に同じバイト列** (改行 LF、1 区間 1 行、明示配列は 32 文字で区切る) =
// 作り直しても git の差分が出ない
std::string SerializeFontMetricsJson(const FontMetrics& metrics);

// <フォントファイル>.ttf → 同じフォルダの <stem>.fontmetrics.json
std::wstring FontMetricsPathFor(const std::wstring& fontPath);

// プロジェクトの計測表を探して読む。assets\fonts にフォントが無い / 表が無い / 壊れている → 空。
// フォントがあるのに表が無い・表のフォント名やサイズが食い違う・壊れている場合は WARN 1 行
FontMetrics LoadProjectFontMetrics(const std::wstring& assetsRoot);

// 実効の表 (起動時に 1 回だけ書く静的な値)。未設定 = 空 = 固定メトリクス
void SetActiveFontMetrics(FontMetrics metrics);
const FontMetrics& ActiveFontMetrics();

// 文字列の計測結果 (キャンバス単位)
struct TextSize {
    float w = 0.0f;     // 最も長い行の幅
    float h = 0.0f;     // lines × 行高
    int32_t lines = 0;  // 行数 (空文字列は 0)
};

// textlayout::LayoutText と**同じ行分割規則**で測る: '\n' で常に改行 / wrap && maxW > 0 なら
// 幅を超える文字の直前で折る (行頭 1 文字は必ず載せる) / 制御文字は幅 0 / 末尾の空行は数えない。
// 送り幅は行ごとに**整数で**積んでから 1 回だけ float へ落とす (加算順に依らず決定的)。
// sim レーンから呼ばれるので scalar のみ
TextSize Measure(const char* utf8, float fontScale, bool wrap, float maxW, const FontMetrics& metrics);

} // namespace uitext
} // namespace mye
