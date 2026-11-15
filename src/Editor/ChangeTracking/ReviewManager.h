//====================================================================================
//                          ReviewManager.h
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          レビュー (承認・却下・要確認・再開) をサービスへ送る薄い層 (宣言)
//====================================================================================
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

#include "Editor/ChangeTracking/ChangeModel.h"

namespace mye {

class TrackClient;

// 1 回の review_set が受ける対象の上限 (サービス側と同じ。超える分は分けて送る)
constexpr size_t kReviewMaxTargetsPerCall = 1000;
// メモの上限 (文字数。サービス側と同じ)
constexpr size_t kReviewNoteMaxChars = 4000;
// セッション・操作の全体を集めるときに引く一覧のページの大きさと数の上限 (超えたら失敗にして、部分的なレビューをしない)
constexpr int kReviewCollectPageSize = 200;
constexpr int kReviewCollectMaxPages = 100;

// 記録する状態。reopened は「確認し直す (未確認へ戻す)」
enum class ReviewAction : uint8_t { Approve, Reject, NeedsReview, Reopen };
const char* ReviewActionWire(ReviewAction action);

// 1 つの対象。kind は revision / hunk / entity / component / field、ref は kind ごとの形 (ipc-contract.md §4.6)
struct ReviewTarget {
    int64_t revisionId = 0;
    std::string kind;
    std::string ref;
    bool operator==(const ReviewTarget&) const = default;
};

// 送信と、まとめての対象の準備。承認は「確認した」という記録だけで、何も実行しない。
// 画面の状態は先に変えない: サービスの応答を待ち、その後の取り直し (通知 / Refresh) で表示が変わる
class ReviewManager {
public:
    enum class Status : uint8_t { Idle, Sending, Done, Failed };
    enum class Collect : uint8_t { None, Collecting, Ready, Failed };

    void Attach(TrackClient* client, ChangeModel* model)
    {
        client_ = client;
        model_ = model;
    }

    // 対象を送る。1000 件ごとに分け、前の分の応答を待って次を送る。送れる状態でなければ false (Failed になり、理由が残る)。
    // 送信中は新しい送信を受けない (false。結果は変えない)
    bool Submit(std::vector<ReviewTarget> targets, ReviewAction action, const std::string& note);

    // まとめての操作の準備。選んだ Revision の ID (そのまま Ready)、またはセッションの全体 (list を最後まで引く)。
    // 集めるのは行為者が AI / 不明の Revision だけ (サービスが review を返さない行は数えない)
    void PrepareRevisions(std::vector<int64_t> revisionIds, int skipped);
    void PrepareSession(const std::string& sessionId);
    void CancelPrepared();
    // 準備した全体を revision の範囲で送る (Ready のときだけ)。Submit と同じ送り方
    bool CommitPrepared(ReviewAction action, const std::string& note);

    Status GetStatus() const { return status_; }
    bool Busy() const { return status_ == Status::Sending || collect_ == Collect::Collecting; }
    Collect GetCollect() const { return collect_; }
    const std::vector<int64_t>& PreparedRevisions() const { return prepared_; }
    int PreparedSkipped() const { return preparedSkipped_; }
    const std::string& PreparedSessionId() const { return preparedSession_; }

    // 直近の結果。失敗のとき code / detail は応答のまま (切断・時間切れは合成した値)
    const std::string& ErrorCode() const { return errorCode_; }
    const std::string& ErrorDetail() const { return errorDetail_; }
    // 何件の対象が記録済みか / 全部で何件か (分けて送った途中で失敗したとき、前の分は記録されている)
    size_t RecordedCount() const { return recorded_; }
    size_t TotalCount() const { return total_; }
    void ClearResult();
    // 接続の張り直しで、送信・準備の途中の状態を捨てる (古い応答は世代で捨てる)
    void Reset();

    // 組み立て (純関数)
    static std::vector<std::vector<ReviewTarget>> Split(std::vector<ReviewTarget> targets, size_t maxPerCall = kReviewMaxTargetsPerCall);
    static nlohmann::json BuildArgs(const std::vector<ReviewTarget>& targets, ReviewAction action, const std::string& note);
    static ReviewTarget ForRevision(int64_t revisionId);
    static ReviewTarget ForHunk(int64_t revisionId, int64_t hunkId);
    static ReviewTarget ForEntity(int64_t revisionId, int64_t fileId);
    // component が空のフィールド (コンポーネントに属さない) は "fileId//field"
    static ReviewTarget ForComponent(int64_t revisionId, int64_t fileId, const std::string& component);
    static ReviewTarget ForField(int64_t revisionId, int64_t fileId, const std::string& component, const std::string& field);
    // 構造の変更 1 行を指す一番狭い範囲 (フィールド → コンポーネント → エンティティ → Revision)
    static ReviewTarget ForSceneChange(int64_t revisionId, const TrackSceneChange& change);
    // 一覧の行がレビューの対象か (行為者が AI / 不明のものだけ。サービスが review を返す)
    static bool IsReviewable(const TrackChangeRow& row) { return !row.review.empty(); }
    // 一覧の行から Revision の範囲の対象を作る (レビューの対象でない行は数えず skipped へ)
    static std::vector<int64_t> RevisionsOf(const std::vector<TrackChangeRow>& rows, const std::vector<int64_t>& selected, int* skipped);

private:
    void SendChunk(std::vector<std::vector<ReviewTarget>> chunks, size_t index, ReviewAction action, std::string note, uint64_t generation);
    void Fail(const std::string& code, const std::string& detail);
    void CollectPage(const std::string& sessionId, const std::string& cursor, std::vector<int64_t> collected, int page, uint64_t generation);

    TrackClient* client_ = nullptr;
    ChangeModel* model_ = nullptr;
    Status status_ = Status::Idle;
    Collect collect_ = Collect::None;
    std::string errorCode_;
    std::string errorDetail_;
    size_t recorded_ = 0;
    size_t total_ = 0;
    std::vector<int64_t> prepared_;
    int preparedSkipped_ = 0;
    std::string preparedSession_;
    // 準備を取り消した・やり直したあとに届いた古い応答を捨てる
    uint64_t collectGeneration_ = 0;
    uint64_t sendGeneration_ = 0;
};

} // namespace mye
