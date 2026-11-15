//====================================================================================
//                          RestoreRunWindow.h
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          復元の実行・結果・起動時の回復・履歴の画面 (宣言)
//====================================================================================
#pragma once
#include <string>
#include <utility>
#include <vector>

#include "Editor/ChangeTracking/RestoreRunFormat.h"
#include "Editor/Windows/Project/AiChangesWindow.h"

namespace mye {

// AI Changes 窓とは別の画面: 復元の進行と結果 (モーダル)、起動時に見つけた回復待ちの画面、履歴。
// 進行は ChangeTrackingSession の RestoreRunner が窓の開閉と無関係に進めるので、この画面は EditorApp が毎フレーム呼ぶ。
// 回復の画面は閉じても Editor を普通に使える (勝手には何も戻さない)
class RestoreRunWindow {
public:
    // 毎フレーム
    void OnImGui(ChangeTrackingSession& session, const AiChangesHost& host);

    void OpenRecovery() { recoveryOpen_ = true; inspectedDirty_ = true; }
    void OpenHistory(ChangeTrackingSession& session);

    // ---- 回復の選択 (画面のボタンと試験が同じ口を通る) ----
    // 適用前の内容へ戻す (今の内容が書いた内容のファイルだけ)。実行の直前にゲートを判定し直す
    bool RestoreOriginal(ChangeTrackingSession& session, const AiChangesHost& host, const std::string& id);
    bool KeepCurrent(ChangeTrackingSession& session, const std::string& id);
    bool Dismiss(ChangeTrackingSession& session, const std::string& id);

    // 試験用
    const RestoreRunView& LastRunView() const { return runView_; }
    const std::vector<RecoveryEntryView>& LastRecoveryViews() const { return recoveryViews_; }
    bool RecoveryOpen() const { return recoveryOpen_; }
    bool HistoryOpen() const { return historyOpen_; }
    void CloseRecovery() { recoveryOpen_ = false; }

private:
    void DrawRun(ChangeTrackingSession& session);
    void DrawRecovery(ChangeTrackingSession& session, const AiChangesHost& host);
    void DrawHistory(ChangeTrackingSession& session);
    void Inspect(ChangeTrackingSession& session);

    RestoreRunView runView_;
    std::vector<RecoveryEntryView> recoveryViews_;
    std::vector<std::pair<std::string, std::vector<RecoveryFileView>>> inspected_;
    bool inspectedDirty_ = true;
    bool recoveryOpen_ = false;
    bool historyOpen_ = false;
    bool runPopupRequested_ = false;
    bool resultPopupRequested_ = false;
    std::string recoveryBlockedText_;
    std::vector<std::string> recoveryBlocks_;
};

} // namespace mye
