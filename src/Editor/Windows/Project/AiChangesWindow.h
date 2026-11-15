//====================================================================================
//                          AiChangesWindow.h
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          AI Changes 窓 (追跡した変更の一覧・詳細・差分、宣言)
//====================================================================================
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "Editor/ChangeTracking/ChangeFormat.h"
#include "Editor/ChangeTracking/ChangeModel.h"
#include "Editor/ChangeTracking/ChangeTrackingSession.h"
#include "Editor/ChangeTracking/SceneChangeOverlay.h"
#include "Editor/Widgets/DiffView.h"

namespace mye {

// 窓が EditorApp に頼むこと / EditorApp が窓に渡すこと。窓に EngineContext は渡さない
// (承認・復元の操作は置かない。後続のサブが足す)
struct AiChangesHost {
    // 自動実行の起動 (--selftest / --screenshot / リプレイ) でないか。偽なら切り替えを出さない (出しても効かない)
    bool allowTracking = false;
    // 設定の現在値 (EditorSettings::changeTrackingEnabled)
    bool enabled = false;
    // 設定を保存して ChangeTrackingSession::Start をやり直す。無効にしてもサービスは止めない (接続を閉じるだけ)
    std::function<void(bool)> setEnabled;
    // プロジェクトの絶対パス (対象ファイルの場所を作る)
    std::wstring projectRoot;
    // 外部エディタで開く (line = 1 始まり)。EditorSettings::externalEditorCmd の経路
    std::function<void(const std::wstring& absPath, int line)> openFile;
    // エクスプローラーで場所を開く
    std::function<void(const std::wstring& absPath)> showInExplorer;
    // 開いているシーンのエンティティを選ぶ (Hierarchy と Inspector が追従する)。ライブのシーンに居なければ false
    std::function<bool(uint64_t fileId)> selectEntity;
    // 開いているシーンへの外部の変更が保留中 (ライブのシーンとディスクが食い違っている)。EditorApp が ReloadHub から読んで渡す
    bool externalChangePending = false;
};

// シーンの変更の行をクリックした結果
enum class SceneChangeActivation : uint8_t {
    Selected,      // 開いているシーンのエンティティを選んだ
    NoEntity,      // 文書全体の変更 (エンティティを指さない)
    NotOpen,       // その文書は今開いているシーンではない (選ばない)
    EntityMissing, // ライブのシーンに居ない (削除された・未保存。選ばない)
};

// TrackHunk → DiffView の入力 (追跡に依存しない形へ詰め替える)
std::vector<DiffHunkInput> ToDiffHunks(const std::vector<TrackHunk>& hunks);

class AiChangesWindow {
public:
    bool open = false; // 既定は閉じておく (Window メニューから開く)

    // 毎フレーム呼ぶ。閉じている間は何も描かず、モデルが見る部分を 0 にする
    void OnImGui(ChangeTrackingSession& session, const AiChangesHost& host);

    // 窓が見ている部分をモデルへ伝える。閉じている間は 0 (一覧を取りに行かない)。ImGui を呼ばない
    void SyncInterest(ChangeModel& model, TrackState state) const;

    // 一覧の左の項目
    enum class Scope : uint8_t { All, Session, Uncertain };

    // 左の項目を選ぶ (一覧のフィルタを作り直す)。窓の操作と試験が同じ口を通る
    void SelectScope(ChangeModel& model, Scope scope, const std::string& sessionId);

    // シーンの変更の行をクリックしたときの処理 (窓の操作と試験が同じ口を通る)。結果は同じフレームの下に文面で出す
    SceneChangeActivation ActivateSceneChange(const TrackSceneChange& change, const SceneChangeOverlay& overlay, const AiChangesHost& host);

    // 試験用
    SceneChangeActivation LastActivation() const { return lastActivation_; }
    bool HasActivation() const { return hasActivation_; }
    Scope CurrentScope() const { return scope_; }
    const std::string& CurrentSessionId() const { return sessionId_; }
    bool MixUncertain() const { return mixUncertain_; }
    bool ShowEditorChanges() const { return showEditor_; }
    // 直近に描いた一覧の仮想行の数 (見出しを含む)
    size_t DrawnItemCount() const { return items_.size(); }
    void SetShowEditorChanges(bool show) { showEditor_ = show; }
    const DiffViewer& GetDiffViewer() const { return diff_; }

private:
    void DrawHeader(ChangeTrackingSession& session, const AiChangesHost& host);
    void DrawSessions(ChangeModel& model);
    void DrawChanges(ChangeModel& model);
    void DrawDetail(ChangeTrackingSession& session, const AiChangesHost& host);
    void DrawSceneBand(ChangeTrackingSession& session, const AiChangesHost& host);
    void DrawColorSettings(SceneChangeOverlay& overlay);
    void DrawSceneChanges(ChangeTrackingSession& session, const AiChangesHost& host);
    void ApplyFilter(ChangeModel& model);

    Scope scope_ = Scope::All;
    std::string sessionId_;
    bool mixUncertain_ = false;
    bool showEditor_ = false; // Editor の保存・復元による変更の節を出すか (既定は出さない)
    bool confirmRequested_ = false;
    char pathFilter_[128] = {};
    std::string kindFilter_;
    std::string attributionFilter_;
    DiffViewer diff_;
    uint64_t shownHunksSerial_ = ~0ull;
    SceneChangeActivation lastActivation_ = SceneChangeActivation::Selected;
    bool hasActivation_ = false;
    std::vector<int> items_; // 一覧の仮想行。-1 = 主の節の見出し、-2 = 不明 / 推定の節の見出し、-3 = Editor / 復元の節の見出し、0 以上 = Changes の添字
};

} // namespace mye
