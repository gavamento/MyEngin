//====================================================================================
//                          HunkRestore.h
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          Hunk の逆適用 (今のファイルの中で探して Before の本文へ戻す。純関数、宣言)
//====================================================================================
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mye {

// 戻す 1 つの Hunk。サービスの diff.rs の Hunk と同じ座標・同じ本文 (改行込み)
struct HunkSpec {
    int64_t oldStart = 1;
    int64_t oldCount = 0;
    int64_t newStart = 1;
    int64_t newCount = 0;
    std::string beforeText;
    std::string afterText;
    std::string contextPre;
    std::string contextPost;
};

enum class HunkFind : uint8_t {
    Match,     // After の並び (前の文脈 + After の本文 + 後ろの文脈) が見つかった
    Restored,  // Before の並びが見つかった (既に戻っている)
    Ambiguous, // After の並びが 2 か所以上にある
    NotFound,  // どちらの並びも無い (位置ずれを許さない Hunk を含む)
};

struct HunkFindResult {
    HunkFind find = HunkFind::NotFound;
    // 見つけた位置と、記録した位置との差 (行数。+ = 記録より後ろ)。Match / Restored のときだけ意味がある
    int64_t shiftLines = 0;
    // 見つけた本文の先頭の行 (0 始まり。本文が 0 行なら挿入する位置)
    int64_t bodyLine = 0;
};

struct HunkApplyOptions {
    // 記録した位置以外も探す (文脈が 0 行の Hunk には効かせない)
    bool allowShift = true;
    // Before の並びが見つかったら Restored にする
    bool detectRestored = true;
    // 記録した位置で、文脈の長さがサービスの unapply_hunks と同じ (前: 先頭までに限り最大 3 行、後ろ: 末尾までに限り最大 3 行) ことを要求する。
    // 固定表 (tests\track\fixture\hunk_apply_cases.json) の strict の列と比べるための設定で、本番では使わない
    bool exactContextLength = false;
};

enum class HunkApplyFailure : uint8_t {
    None,
    Conflict,     // どれかの Hunk が Ambiguous / NotFound
    Overlap,      // 戻す本文の範囲が互いに重なる
    TooManyLines, // 行数が上限を超える (判定しない)
};

struct HunkApplyResult {
    HunkApplyFailure failure = HunkApplyFailure::None;
    // 入力した Hunk と同じ並びの結果
    std::vector<HunkFindResult> finds;
    // 戻す Hunk が 1 つでもある (全部 Restored なら偽)
    bool changed = false;
    // 戻した結果のバイト列 (failure が None のときだけ。changed が偽なら入力のまま)
    std::string bytes;

    bool Ok() const { return failure == HunkApplyFailure::None; }
};

// 行数の上限 (サービスの diff.rs の max_lines と同じ値)
constexpr size_t kHunkRestoreMaxLines = 2000000;
// 文脈の行数 (diff.rs の CONTEXT_LINES)
constexpr int64_t kHunkContextLines = 3;

// 改行込みで行に分ける。区切りは LF だけ (CRLF の CR は行の末尾に残る)。末尾の改行が無い最後の行もそのまま 1 行
std::vector<std::string_view> SplitLinesKeepEol(std::string_view text);

// hunks を current に逆向きに当てる (After → Before)。**全部の Hunk を、current の元の内容の中で探してから**一度に組み立てる:
// 後ろの Hunk から当てても前から当てても同じバイト列になり、前の Hunk の行数の増減が後ろの探索に影響しない。
// 1 つでも Ambiguous / NotFound / 範囲の重なりがあれば、何も当てない (bytes は空)
HunkApplyResult UnapplyHunks(std::string_view current, const std::vector<HunkSpec>& hunks, const HunkApplyOptions& options = HunkApplyOptions());

} // namespace mye
