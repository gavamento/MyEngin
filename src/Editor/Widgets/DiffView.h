//====================================================================================
//                          DiffView.h
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          Hunk の列を描く読み取り専用の差分表示部品 (宣言)
//====================================================================================
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mye {

// 差分表示部品の入力 1 件。追跡 (ChangeTracking) には依存しない: 呼び手が自分の Hunk をこの形へ詰め替える。
// 文字列は改行込み (LF / CRLF / 末尾の改行なし)。行番号は 1 始まり
struct DiffHunkInput {
    int64_t oldStart = 1;
    int64_t oldCount = 0;
    int64_t newStart = 1;
    int64_t newCount = 0;
    std::string before;      // 変更のあった Before の行
    std::string after;       // 変更のあった After の行
    std::string contextPre;  // 前の文脈 (両側で同じ行)
    std::string contextPost; // 後ろの文脈
    bool eolOnly = false;    // 全行が改行の違いだけ
    bool oversize = false;   // 大きすぎて本文を持たない (beforeBytes / afterBytes だけ)
    int64_t beforeBytes = 0;
    int64_t afterBytes = 0;
    std::string badge;       // 見出しの右に添える短い文字列 (呼び手の状態表示。空 = 無し)
    uint32_t badgeColor = 0; // badge の色 (ImU32。0 = 副文字の色)
};

enum class DiffLayout : uint8_t { Inline, Split };
enum class DiffEol : uint8_t { None, Lf, CrLf };
enum class DiffLineKind : uint8_t { Context, Delete, Add, EolChange, Empty };
enum class DiffRowKind : uint8_t { Header, Line, Notice };

// 文字列の置き場 (DiffRows の pool_) の中の範囲
struct DiffTextRef {
    uint32_t off = 0;
    uint32_t len = 0;
};

// インライン (1 列) の 1 行
struct DiffInlineRow {
    DiffRowKind kind = DiffRowKind::Line;
    DiffLineKind line = DiffLineKind::Context;
    int32_t hunk = 0;
    int64_t oldNo = 0; // 0 = 無い
    int64_t newNo = 0;
    DiffTextRef text;
    DiffEol eol = DiffEol::None;
    DiffEol eolAfter = DiffEol::None; // EolChange のとき、新しい側の行末
    bool eolMark = false;             // 行末の種類が変わった行 (印を付ける)
};

// 左右分割の片側
struct DiffSplitSide {
    DiffLineKind kind = DiffLineKind::Empty;
    int64_t no = 0;
    DiffTextRef text;
    DiffEol eol = DiffEol::None;
    bool eolMark = false;
};

// 左右分割の 1 行 (左 = Before、右 = After)。削除だけ・追加だけの行は反対側が Empty
struct DiffSplitRow {
    DiffRowKind kind = DiffRowKind::Line;
    int32_t hunk = 0;
    DiffSplitSide left;
    DiffSplitSide right;
};

// 改行込みの文字列を 1 行ずつに分けたもの
struct DiffLineSlice {
    std::string_view body; // 行末を除く
    DiffEol eol = DiffEol::None;
};

// Hunk の列から、インラインと左右分割の行の列を組む (ImGui を呼ばない)。入力を受けたとき 1 回だけ走査し、描画では走査しない
class DiffRows {
public:
    // 1 行の本文を持つ上限 (バイト)。これを超える行は切って "..." を付ける (描画の費用の上限)
    static constexpr size_t kMaxLineBytes = 4096;
    // タブ幅 (桁)
    static constexpr int kTabWidth = 4;

    void Build(const std::vector<DiffHunkInput>& hunks);
    void Clear();

    const std::vector<DiffInlineRow>& InlineRows() const { return inline_; }
    const std::vector<DiffSplitRow>& SplitRows() const { return split_; }
    std::string_view Text(const DiffTextRef& ref) const { return std::string_view(pool_).substr(ref.off, ref.len); }

    size_t HunkCount() const { return headers_.size(); }
    // "@@ -a,b +c,d @@"
    const std::string& HunkHeader(size_t hunk) const { return headers_[hunk].header; }
    bool HunkEolOnly(size_t hunk) const { return headers_[hunk].eolOnly; }
    bool HunkOversize(size_t hunk) const { return headers_[hunk].oversize; }
    int64_t HunkBeforeBytes(size_t hunk) const { return headers_[hunk].beforeBytes; }
    int64_t HunkAfterBytes(size_t hunk) const { return headers_[hunk].afterBytes; }
    const std::string& HunkBadge(size_t hunk) const { return headers_[hunk].badge; }
    uint32_t HunkBadgeColor(size_t hunk) const { return headers_[hunk].badgeColor; }

    // 表示する桁の最大 (全角 = 2、タブ展開後)。横スクロールの幅の見積り
    int32_t MaxColumns() const { return maxColumns_; }
    // 表示する行番号の最大 (桁数の見積り)
    int64_t MaxLineNo() const { return maxLineNo_; }

    // 試験用に公開: 改行込みの文字列を行に分ける (区切りは LF のみ。CRLF の CR は行末の一部)
    static std::vector<DiffLineSlice> SplitLines(std::string_view text);

private:
    struct HunkInfo {
        std::string header;
        bool eolOnly = false;
        bool oversize = false;
        int64_t beforeBytes = 0;
        int64_t afterBytes = 0;
        std::string badge;
        uint32_t badgeColor = 0;
    };
    // タブを展開し、長すぎる行を切って pool_ に積む
    DiffTextRef Store(std::string_view body);

    std::vector<DiffInlineRow> inline_;
    std::vector<DiffSplitRow> split_;
    std::vector<HunkInfo> headers_;
    std::string pool_;
    int32_t maxColumns_ = 0;
    int64_t maxLineNo_ = 0;
};

// 差分の描画 (読み取り専用。SourceControl の mye::DiffView とは別物なので DiffViewer)。見えている行だけを描き (ImGuiListClipper)、毎フレーム本文を走査しない。
// 呼び手は SetHunks で入力を渡し、毎フレーム Draw を呼ぶ。Hunk の選択は呼び手が読む (sub-14 / sub-15 が使う)
class DiffViewer {
public:
    // 入力を作り直す (1 回走査)。選択は解除する
    void SetHunks(const std::vector<DiffHunkInput>& hunks);
    void Clear();

    void SetLayout(DiffLayout layout) { layout_ = layout; }
    DiffLayout Layout() const { return layout_; }

    // Hunk を選ぶ (-1 = 解除)。範囲外は解除
    void SelectHunk(int hunk);
    int SelectedHunk() const { return selected_; }

    // 描く。Hunk が 0 個のときは emptyMessage (呼び手が Tr したもの) を出す。戻り値 = このフレームで選択が変わったか。
    // size = (0, 0) で残りの領域いっぱい
    bool Draw(const char* id, const char* emptyMessage, float width = 0.0f, float height = 0.0f);

    // 直近の Draw が実際に描いた行数 (見えている範囲だけに限られることの試験用)
    int LastDrawnRows() const { return lastDrawnRows_; }
    const DiffRows& Rows() const { return rows_; }

private:
    DiffRows rows_;
    DiffLayout layout_ = DiffLayout::Inline;
    int selected_ = -1;
    int lastDrawnRows_ = 0;
};

} // namespace mye
