//====================================================================================
//                          SceneRestoreFormat.h
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          ライブのシーンの復元の文面の整形 (ImGui を呼ばない純関数、宣言)
//====================================================================================
#pragma once
#include <string>
#include <vector>

#include "Editor/ChangeTracking/RestoreGate.h"
#include "Editor/ChangeTracking/SceneRestorePlan.h"
#include "Editor/ChangeTracking/SceneRestoreRunner.h"

namespace mye {

// 文面は、呼んだ時点の言語 (Tr) で返す。

std::string SceneRestoreScopeText(SceneRestoreScope scope);
// 理由 1 件。detail は fileId など (理由の文面の {id} に入る)
std::string SceneRestoreIssueText(SceneRestoreIssue issue, const std::string& detail);
// 戻すと行う操作 1 件 (どのエンティティのどの値を何から何へ)
std::string SceneRestoreOpText(const SceneRestoreOp& op);

// プレビューの画面に出す内容 (文字列と印だけ。試験が画面に出るものをそのまま確かめられる)
struct SceneRestoreScreen {
    bool open = false;
    std::string target;                 // シーンの名前
    std::string scope;                  // 戻す範囲
    std::string rowCount;               // 範囲の項目数
    bool notOpen = false;               // 今開いているシーンの変更ではない
    std::string notOpenText;
    std::vector<std::string> blocks;    // 止める理由 (全部)
    bool hasPlan = false;
    SceneRestoreVerdict verdict = SceneRestoreVerdict::NotRestorable;
    std::string verdictText;
    std::vector<std::string> issues;    // 戻せない・競合した項目の理由
    std::vector<std::string> ops;       // 戻すと行う操作 (Ready のときだけ)
    std::vector<std::string> notes;     // 注記 (Undo で取り消せる・未保存になる・削除する)
    bool canApply = false;
    std::string applyNote;
};

struct SceneRestoreScreenInput {
    std::string sceneName;
    SceneRestoreScope scope = SceneRestoreScope::Field;
    size_t rowCount = 0;
    bool notOpen = false;
    const SceneRestorePlan* plan = nullptr;
    std::vector<RestoreBlock> blocks;
};

SceneRestoreScreen BuildSceneRestoreScreen(const SceneRestoreScreenInput& input);

// 実行の結果の画面
struct SceneRestoreResultView {
    SceneRestoreOutcome outcome = SceneRestoreOutcome::NoScene;
    bool success = false;
    std::string headline;
    std::vector<std::string> reasons;  // 止めた理由 (ゲートの全部、競合した項目)
    std::string recordNote;            // 履歴をサービスへ送れていないとき
};

SceneRestoreResultView BuildSceneRestoreResultView(const SceneRestoreResult& result, bool recordPending);

// 履歴の行に添える「シーンの復元」の印 (restore_list の targets[].sceneChangeIds)。シーンの復元でなければ空
std::string SceneRestoreHistoryMark(const nlohmann::json& target);

} // namespace mye
