//====================================================================================
//                          AiChangesWindow.h
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          AI Changes 窓 (追跡した変更の一覧・詳細・差分、宣言)
//====================================================================================
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "Editor/ChangeTracking/BlobHeadView.h"
#include "Editor/ChangeTracking/ChangeFormat.h"
#include "Editor/ChangeTracking/ChangeModel.h"
#include "Editor/ChangeTracking/ChangeTrackingSession.h"
#include "Editor/ChangeTracking/RemovedEntityGhosts.h"
#include "Editor/ChangeTracking/RestoreFileIo.h"
#include "Editor/ChangeTracking/RestoreFormat.h"
#include "Editor/ChangeTracking/RestoreGate.h"
#include "Editor/ChangeTracking/RestorePlan.h"
#include "Editor/ChangeTracking/RestorePreview.h"
#include "Editor/ChangeTracking/ReviewManager.h"
#include "Editor/ChangeTracking/SceneRestorePlan.h"
#include "Editor/ChangeTracking/SceneRestoreRunner.h"
#include "Editor/ChangeTracking/SceneChangeOverlay.h"
#include "Editor/ChangeTracking/ScenePreviewSession.h"
#include "Editor/Widgets/DiffView.h"

namespace mye {

// 窓が EditorApp に頼むこと / EditorApp が窓に渡すこと。窓に EngineContext は渡さない
// (復元の実行は ChangeTrackingSession の RestoreRunner。窓は始めるだけで、ファイルを書く口は置かない)
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

    // ---- 復元のプレビュー (M104p)。どれも読み取りだけ ----
    // EditorApp::BuildGateInputs の結果。未設定なら全部偽 (止める理由なし)
    std::function<GateInputs()> gateInputs;
    // どの窓がどの文書を編集中か。教えられない窓は known = false にする (その窓の未保存が立っていれば安全側で止まる)
    std::function<EditedDocuments()> editedDocuments;
    // 回復待ちの復元があるか (起動時に見つけた、applying / rolling_back / recovery_required のジャーナル。未設定 = 無い)。
    // 進行中の復元 (RestoreRunner::Busy) は窓が自分で見るので、ここには入れない
    std::function<bool()> restoreJournalPending;
    // 復元の回復の画面・履歴を開く (RestoreRunWindow。未設定 = ボタンを出さない)
    std::function<void()> openRestoreRecovery;
    std::function<void()> openRestoreHistory;
    // 既存の Source Control の status モデルだけを読んで、stage 済みか答える (git を起動しない・Index に触らない)。未設定 = 分からない
    std::function<StagedState(const std::string& relPath)> stagedState;

    // ---- ライブのシーンの復元 (M104q) ----
    // 今のシーン・Undo・選択・C# のホスト。未設定 = 復元できない (World に触れない)。ImGui の 1 フレームの間だけ有効な値
    std::function<SceneRestoreEnv()> sceneRestoreEnv;
    // 「シーンの変更を復元...」を押した (SceneRestoreDialog を開く)。anchor = 選んだ構造の変更の行。未設定 = ボタンを出さない
    std::function<void(SceneRestoreScope scope, const TrackSceneChange& anchor)> openSceneRestore;

    // ---- Before / After の 3D 比較 (M104s) ----
    // 比較の窓を開く (ScenePreviewWindow)。args の revision・sceneChanges・projectRoot は AI Changes 窓が詰める。
    // 持ち主 (EditorApp) が C# の判定とアセットのパスの解決を足す。未設定 = ボタンを出さない
    std::function<bool(ScenePreviewOpenArgs args)> openScenePreview;
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
// Hunk の見出しに今のレビューの状態を添える (状態の文面と色)。hunks と input は同じ並び
void ApplyReviewBadges(std::vector<DiffHunkInput>& input, const std::vector<TrackHunk>& hunks);

// 復元を止める理由 (ゲート + 追跡に固有の条件)。窓のプレビューと、実行の直前の判定し直し、回復の巻き戻しが同じ口を通る。
// targetPaths = 復元が触るパス (相対)。journalPending は host.restoreJournalPending (進行中の復元は含まない)
std::vector<RestoreBlock> BuildRestoreBlocks(ChangeTrackingSession& session, const AiChangesHost& host, const std::vector<std::string>& targetPaths, bool blobMissing);

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

    // ---- レビュー (M104o)。窓のボタンと試験が同じ口を通る。送るのは ReviewManager で、画面の状態はサービスの応答のあとの取り直しで変わる ----

    // 一覧の行のクリック。ctrl = 複数選択へ足す・外す、shift = 前に選んだ行からの範囲、どちらも無ければ 1 件を詳細に出す
    void ClickChange(ChangeModel& model, int64_t revisionId, bool ctrl, bool shift);
    // 選んだ Revision 全体 / その中で選んだ Hunk / 選んだシーンの変更 (範囲は SetSceneScope で選ぶ)。送れたら true。
    // 詳細が無い・行為者が対象外 (editor_user / restore)・葉の種類が合わないときは何もしない (false)
    bool ReviewRevision(ChangeTrackingSession& session, ReviewAction action);
    bool ReviewSelectedHunk(ChangeTrackingSession& session, ReviewAction action);
    bool ReviewSelectedScene(ChangeTrackingSession& session, ReviewAction action);
    // まとめて (選んだ複数の行 / 左で選んだセッション全体)。対象を集め、確認 (件数つき) を 1 回出す。集める対象が無ければ false
    bool RequestBulkSelected(ChangeTrackingSession& session, ReviewAction action);
    bool RequestBulkSession(ChangeTrackingSession& session, ReviewAction action);
    // 確認の結果。ConfirmBulk は準備した全体を送る
    bool ConfirmBulk(ChangeTrackingSession& session);
    void CancelBulk(ChangeTrackingSession& session);
    // 一覧のレビューの絞り込み (集約の状態。空 = 絞らない)
    void SetReviewFilter(ChangeModel& model, const std::string& state);
    // 選んだ構造の変更 (sceneChangeId。0 で解除) と、その範囲 (0 = いちばん狭い範囲、増えるほど広い)
    void SelectSceneChange(int64_t sceneChangeId);
    void SetSceneScope(int scope) { sceneScope_ = scope; }
    // メモ (次の単独のレビューに付く)
    void SetReviewNote(const std::string& note);
    // 差分の Hunk を選ぶ (詳細の描画のあと。index は model.Hunks() の添字、範囲外で解除)
    void SelectHunkIndex(const ChangeModel& model, int index);
    // 選んだ Revision の変更を属する操作の全体でまとめてレビューする (操作の Revision を集め、件数つきの確認を 1 回出す)。
    // 詳細が無い・対象外の行為者・操作に属さないときは false
    bool RequestBulkOperation(ChangeTrackingSession& session, ReviewAction action);

    // ---- 復元のプレビュー (M104p)。窓のボタンと試験が同じ口を通る。**作業ツリーを書かない** ----

    // 選んだ Revision のファイル全体 / 選んだ Hunk の復元のプレビューを開く (必要な内容を集めて計画を作る)。
    // 詳細が無い・Hunk 単位で Hunk が選ばれていないときは false
    bool OpenRestore(ChangeTrackingSession& session, RestoreScope scope, const AiChangesHost& host);
    void CloseRestore();
    // いま復元を止めている理由 (全部)。毎フレーム判定し直す (確認を出している間に状態が変わる)。プレビューが開いていなくても使える
    std::vector<RestoreBlock> RestoreBlocks(ChangeTrackingSession& session, const AiChangesHost& host) const;
    // 注記 (削除の確認・stage 済み・作り直しの見込み)
    RestoreNotes RestoreNotesFor(const AiChangesHost& host) const;
    // 開いているプレビューの復元を実行する (「復元する」ボタンと試験が同じ口を通る)。ゲート・照合に通っていなければ何もしない (false)。
    // 実行は ChangeTrackingSession の RestoreRunner が毎フレーム進める。始められたらプレビューを閉じる
    bool ApplyRestore(ChangeTrackingSession& session, const AiChangesHost& host);
    // 試験用: 今のファイルの読み取り口を差し替える (既定は本物の読み取り専用の実装)
    void SetRestoreFileIo(std::unique_ptr<RestoreFileIo> io) { restoreIo_ = std::move(io); }
    const RestorePreview& Restore() const { return restore_; }
    // 直近のフレームで描いた画面の内容 (文字列と印)。試験が画面に出ているものを確かめる
    const RestoreScreen& LastRestoreScreen() const { return restoreScreen_; }
    const BlobHeadView& Heads() const { return head_; }

    // ---- 削除されたエンティティの Before の読み取り専用の表示 (M104r)。窓の操作と試験が同じ口を通る ----
    // 一覧の「エンティティの削除」の行を選んだ。Selection には入れない (実体が無い)。revisionId = いま詳細に出している変更。
    // 行が別のシーンの変更なら、表示はするが復元は出さない
    void ShowRemovedBefore(const TrackSceneChange& removedRow, int64_t revisionId, const SceneChangeOverlay& overlay);
    // Scene View のゴーストを選んだ (overlay の表の削除済みのエンティティ)。表に無ければ false
    bool ShowRemovedBeforeFromOverlay(const SceneChangeOverlay& overlay, int64_t fileId);
    void CloseRemovedBefore();
    // 「このエンティティを復元...」: 削除した変更を選び、シーンの変更を取り終えたら、その行でシーンの復元を開く (TickRestoreRequest が毎フレーム進める)。
    // 削除した変更が分からない・何も開いていないときは false
    bool RequestRestoreRemoved(ChangeTrackingSession& session);
    const RemovedBeforeView& RemovedBefore() const { return removedView_; }
    bool RestoreRequestPending() const { return restorePending_; }
    // 直近のフレームで Before の表に描いた文字列の行 (名前・親・コンポーネント・値・注記)。試験が画面に出ているものを確かめる
    const std::vector<std::string>& LastBeforeLines() const { return beforeLines_; }

    // ---- Before / After の 3D 比較 (M104s)。窓のボタンと試験が同じ口を通る。ファイルもライブのシーンも書かない ----
    // 選んだ Revision がシーンの文書で、詳細とシーンの変更を取り終えているとき、比較の窓を開く。開けなければ false
    bool OpenScenePreview(ChangeTrackingSession& session, const AiChangesHost& host);

    // 試験用
    const std::set<int64_t>& MultiSelection() const { return multi_; }
    bool BulkConfirmPending() const { return bulkPending_; }
    ReviewAction BulkAction() const { return bulkAction_; }
    const std::string& ReviewFilter() const { return reviewFilter_; }
    int64_t SelectedHunkId() const { return selectedHunkId_; }
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
    void DrawSessions(ChangeTrackingSession& session);
    void DrawChanges(ChangeTrackingSession& session);
    void DrawDetail(ChangeTrackingSession& session, const AiChangesHost& host);
    void DrawSceneBand(ChangeTrackingSession& session, const AiChangesHost& host);
    void DrawColorSettings(SceneChangeOverlay& overlay);
    void DrawSceneChanges(ChangeTrackingSession& session, const AiChangesHost& host);
    void ApplyFilter(ChangeModel& model);
    void DrawReviewStatus(ChangeTrackingSession& session);
    void DrawReviewPanel(ChangeTrackingSession& session);
    void DrawHunkReview(ChangeTrackingSession& session);
    void DrawSceneReview(ChangeTrackingSession& session);
    void DrawSceneRestoreButton(ChangeTrackingSession& session, const AiChangesHost& host);
    void DrawRemovedBefore(ChangeTrackingSession& session, const AiChangesHost& host);
    void DrawPreviewButton(ChangeTrackingSession& session, const AiChangesHost& host);
    void TickRestoreRequest(ChangeTrackingSession& session, const AiChangesHost& host);
    void DrawBulkPopup(ChangeTrackingSession& session);
    void DrawAggregates(ChangeTrackingSession& session);
    void DrawRestoreButtons(ChangeTrackingSession& session, const AiChangesHost& host, const std::string& actor);
    void DrawRestorePopup(ChangeTrackingSession& session, const AiChangesHost& host);
    void DrawBlobHeads(ChangeTrackingSession& session);
    RestoreFileIo& RestoreIo();
    std::optional<ReviewAction> DrawReviewButtons(const char* id, bool busy);
    bool SubmitReview(ChangeTrackingSession& session, std::vector<ReviewTarget> targets, ReviewAction action);

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
    // レビュー
    std::set<int64_t> multi_;     // 複数選択の Revision (単独で詳細に出している行は含めない)
    int64_t anchor_ = 0;          // shift の範囲の起点
    char reviewNote_[512] = {};
    bool noteSent_ = false;       // メモを付けて送った (Done で消す。失敗では残す)
    std::string reviewFilter_;
    int64_t selectedHunkId_ = 0;  // 詳細を取り直しても選んでいる Hunk を保つ
    int64_t shownRevision_ = 0;
    int64_t selectedSceneId_ = 0;
    int sceneScope_ = 0;
    bool bulkPending_ = false;    // まとめての確認を出す
    bool bulkOpenRequested_ = false;
    ReviewAction bulkAction_ = ReviewAction::Approve;
    // 復元のプレビュー
    RestorePreview restore_;
    // プレビューを開いた時点の入力 (実行の直前の判定し直しが同じ入力で計画を作り直す)
    nlohmann::json restoreDetail_;
    std::vector<TrackHunk> restoreAllHunks_;
    std::vector<int64_t> restoreHunkIds_;
    RestoreScreen restoreScreen_;
    std::unique_ptr<RestoreFileIo> restoreIo_;
    DiffViewer restoreDiff_;
    DiffViewer restoreConflictDiff_;
    uint64_t shownRestoreSerial_ = ~0ull;
    bool restoreOpenRequested_ = false;
    BlobHeadView head_;
    // 削除されたエンティティの Before (読み取り専用)
    RemovedBeforeView removedView_;
    bool removedNotOpen_ = false; // その削除は、今開いていないシーンの変更
    bool restorePending_ = false;
    int64_t restoreRevisionId_ = 0;
    int64_t restoreFileId_ = 0;
    bool restoreRowMissing_ = false;
    std::vector<std::string> beforeLines_;
    std::vector<int> items_; // 一覧の仮想行。-1 = 主の節の見出し、-2 = 不明 / 推定の節の見出し、-3 = Editor / 復元の節の見出し、0 以上 = Changes の添字
};

} // namespace mye
