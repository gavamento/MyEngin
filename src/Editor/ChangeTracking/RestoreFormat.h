//====================================================================================
//                          RestoreFormat.h
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          復元のプレビューの文面の整形 (ImGui を呼ばない純関数、宣言)
//====================================================================================
#pragma once
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

#include "Editor/ChangeTracking/PathGuard.h"
#include "Editor/ChangeTracking/RestoreGate.h"
#include "Editor/ChangeTracking/RestorePlan.h"
#include "Editor/ChangeTracking/RestorePreview.h"

namespace mye {

// 文面は、呼んだ時点の言語 (Tr) で返す。

// 書き先の検査で拒否した理由
std::string PathIssueText(PathIssue issue);
// 復元できない・競合の理由 (None は空)
std::string RestoreIssueText(RestoreIssue issue);
// 照合の結果の 1 行 (一致 / 復元済み / 競合 / 復元できない)
std::string RestoreVerdictText(RestoreVerdict verdict);
// 止める理由 1 件。文書が理由のときはパスを添える
std::string RestoreBlockText(const RestoreBlock& block);
// 書くことになる操作 1 件 (対象のパスと、削除・作成・書き換え・名前を戻す の別)。Hunk を逆向きにして書く内容は hunkOnly で言い分ける
std::string RestoreOpText(const RestoreOp& op, bool hunkOnly);
// Hunk ごとの照合の結果。一致 / 位置がずれて一致 (何行ずれたか) / 復元済み / 2 か所以上 / 見つからない
std::string RestoreHunkOutcomeText(const RestoreHunkOutcome& outcome);
// 注記 (削除の確認・参照を確かめていない・stage 済み・作り直しの見込み・.meta・位置のずれ)。1 件 1 行
std::vector<std::string> RestoreNoteLines(const RestoreNotes& notes);
// 競合のときの 3 つの内容 (今・Before・After) のハッシュの 1 行。ハッシュは先頭 8 桁。無い内容は「なし」
std::string RestoreHashLine(const RestoreRevision& revision, const NowState& now);

// describe の changeReview / operationReview (集約: {state, counts, truncated}) の 1 行: 状態の文面 + 内訳 ("Approved 1 / Pending 2")。
// サービスが返さなければ空
std::string ReviewGroupText(const nlohmann::json& group);

// 大きいファイルの先頭の表示の文面 (バイナリ / 先頭だけの注記)
std::string BlobHeadBinaryText(int64_t totalBytes);
std::string BlobHeadTruncatedText(int64_t shownBytes, int64_t totalBytes);

// プレビューの画面に出す内容 (文字列と印だけ。ImGui を呼ばずに作るので、試験が画面の内容をそのまま確かめられる)
struct RestoreScreen {
    bool open = false;
    RestorePreview::Phase phase = RestorePreview::Phase::Closed;
    std::string target;                // 対象のパス (名前の変更は「旧 -> 新」)
    std::string scope;                 // ファイル全体 / 選んだ Hunk だけ
    std::string failure;               // Failed のとき (サービスの応答の理由)
    std::vector<std::string> blocks;   // 止める理由 (全部)
    bool hasPlan = false;
    RestoreVerdict verdict = RestoreVerdict::NotRestorable;
    std::string verdictText;
    std::vector<std::string> issues;   // 復元できない・競合の理由 (書き先の検査の理由を含む)
    std::vector<std::string> hunkLines; // Hunk ごとの照合の結果
    std::string hashLine;              // 競合のとき: 今・Before・After のハッシュ
    std::string superseded;            // 後の変更に置き換わっているときの注記
    std::vector<std::string> ops;      // 書くことになる操作 (Ready のときだけ)
    std::vector<std::string> notes;    // 注記
    // 実行を押せるか (照合に通った計画があり、止める理由が無い)。押せないときは、その理由の一言を applyNote に
    bool canApply = false;
    std::string applyNote;
};

// 開いているプレビュー・今のゲートの理由・注記から画面の内容を作る
RestoreScreen BuildRestoreScreen(const RestorePreview& preview, const std::vector<RestoreBlock>& blocks, const RestoreNotes& notes);

} // namespace mye
