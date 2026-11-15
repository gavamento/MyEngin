//====================================================================================
//                          RestoreRunFormat.h
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          復元の実行・結果・回復・履歴の文面 (ImGui を呼ばない純関数、宣言)
//====================================================================================
#pragma once
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

#include "Editor/ChangeTracking/RestoreRunner.h"

namespace mye {

// 文面は、呼んだ時点の言語 (Tr) で返す。画面の内容を文字列と印だけで作るので、試験が画面に出るものをそのまま確かめられる

// 進行 (実行中) または結果 (終わったあと) の画面
struct RestoreRunView {
    enum class Mode : uint8_t { None, Progress, Result };
    Mode mode = Mode::None;
    // Progress
    std::string phase;
    float fraction = 0.0f; // 0〜1。段階の中の進み具合
    bool canCancel = false;
    std::string cancelHint;
    // Result
    RestoreOutcome outcome = RestoreOutcome::None;
    std::string headline;
    std::vector<std::string> reasons;    // 止めた・失敗した理由 (止める理由の全部を含む)
    std::vector<std::string> unresolved; // 触らずに残したファイル
    std::string journal;                 // ジャーナルの識別子の 1 行
    std::string recordNote;              // 履歴をサービスへ送れていないとき
    bool needsRecovery = false;
};

RestoreRunView BuildRestoreRunView(const RestoreRunner& runner);

// 回復の画面の 1 ジャーナル分
struct RecoveryFileText {
    std::string path;
    std::string before;   // 復元の前 (ハッシュの先頭 8 桁、無ければ「ファイルなし」)
    std::string intended; // 復元が書こうとした内容
    std::string now;      // 今 (ハッシュ)
    std::string verdict;  // 今の内容がどれに当たるか
    RecoveryFileView::Current current = RecoveryFileView::Current::Other;
};

struct RecoveryEntryView {
    std::string id;
    std::string header;
    bool broken = false;
    std::string brokenText;
    std::vector<RecoveryFileText> files;
    bool canRestore = false; // 今の内容が「書こうとした内容」のファイルがある (戻せるものがある)
};

std::vector<RecoveryEntryView> BuildRecoveryViews(const RestoreRunner& runner, const std::vector<std::pair<std::string, std::vector<RecoveryFileView>>>& inspected);

// 復元の状態 (restore_operations.state) の文面
std::string RestoreStateText(const std::string& state);

// 履歴の 1 行 (restore_list の items[i])
struct RestoreHistoryRow {
    std::string time;
    std::string state;
    std::string target;
    std::string error;
};

std::vector<RestoreHistoryRow> BuildHistoryRows(const std::vector<nlohmann::json>& items);

} // namespace mye
