//====================================================================================
//                          SceneRestoreWindow.h
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          ライブのシーンの復元のプレビューと結果の画面 (宣言)
//====================================================================================
#pragma once
#include <string>
#include <vector>

#include "Editor/ChangeTracking/SceneRestoreFormat.h"
#include "Editor/Windows/Project/AiChangesWindow.h"

namespace mye {

// 今開いているシーンの変更を項目ごとに戻す画面 (プレビュー → 確認 → 結果)。AI Changes 窓とは別の持ち主 (EditorApp)。
// ファイルは書かない: World を Undo の 1 エントリで書き換え、シーンは未保存になる。画面のボタンと試験が同じ口を通る
// プレビューを開くのに要る入力 (モデルから集めたもの。試験とスクショのプローブはモデルを通さずに渡す)
struct SceneRestoreOpenArgs {
    int64_t revisionId = 0;
    std::string actor;
    std::string openDocumentId;
    std::string sceneName;
    std::string scenePath; // プロジェクトからの相対 (履歴に載せる)
    SceneRestoreScope scope = SceneRestoreScope::Field;
    std::vector<TrackSceneChange> rows;
    bool notOpen = false;
};

class SceneRestoreDialog {
public:
    // プレビューを開く。範囲の行を選び、ライブのシーンと照合して計画を作る (World は書かない)。
    // 詳細が無い・シーンの変更を取り終えていないときは false
    bool Open(ChangeTrackingSession& session, const AiChangesHost& host, SceneRestoreScope scope, const TrackSceneChange& anchor);
    // 入力を直接渡して開く (Open の後半。ライブのシーンと照合して計画を作る)
    void OpenWith(ChangeTrackingSession& session, const AiChangesHost& host, SceneRestoreOpenArgs args);
    void Close();
    bool IsOpen() const { return open_; }

    // 毎フレーム。プレビューと結果のモーダルを描く
    void OnImGui(ChangeTrackingSession& session, const AiChangesHost& host);

    // 今、復元を止めている理由 (全部。毎フレーム判定し直す)
    std::vector<RestoreBlock> Blocks(ChangeTrackingSession& session, const AiChangesHost& host) const;
    // 画面の内容を今のゲートで作り直す (プレビューを描く毎フレームと試験が通る口。計画は作り直さない)
    void RefreshScreen(ChangeTrackingSession& session, const AiChangesHost& host) { screen_ = BuildScreen(session, host); }

    // プレビューの復元を実行する (「復元する」ボタンと試験が同じ口)。止める理由・照合が通っていなければ書かない
    bool Apply(ChangeTrackingSession& session, const AiChangesHost& host);

    // 結果を見終えた
    void AcknowledgeResult() { hasResult_ = false; resultPopupRequested_ = false; }

    // 試験用
    const SceneRestorePlan& Plan() const { return plan_; }
    const SceneRestoreScreen& LastScreen() const { return screen_; }
    bool HasResult() const { return hasResult_; }
    const SceneRestoreResult& Result() const { return result_; }
    const SceneRestoreResultView& LastResultView() const { return resultView_; }
    size_t RowCount() const { return rows_.size(); }
    bool NotOpen() const { return notOpen_; }

private:
    SceneRestoreScreen BuildScreen(ChangeTrackingSession& session, const AiChangesHost& host) const;
    void DrawPreview(ChangeTrackingSession& session, const AiChangesHost& host);
    void DrawResult(ChangeTrackingSession& session);

    bool open_ = false;
    bool openRequested_ = false;
    SceneRestoreScope scope_ = SceneRestoreScope::Field;
    int64_t revisionId_ = 0;
    std::string actor_;
    std::string openDocumentId_;
    std::string sceneName_;
    std::string scenePath_; // プロジェクトからの相対 (履歴に載せる)
    std::vector<TrackSceneChange> rows_;
    SceneRestorePlan plan_;
    bool notOpen_ = false;
    SceneRestoreScreen screen_;

    bool hasResult_ = false;
    bool resultPopupRequested_ = false;
    SceneRestoreResult result_;
    SceneRestoreResultView resultView_;
};

} // namespace mye
