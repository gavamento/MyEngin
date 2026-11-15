//====================================================================================
//                          ChangeModel.h
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          追跡サービスから取った表示用の保持 (宣言)
//====================================================================================
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace mye {

class TrackClient;

// status の要約 (ipc-contract.md §9.4)
struct TrackStatusInfo {
    bool valid = false;
    std::string phase; // starting / baselining / watching / failed / stopped
    int64_t uptimeMs = 0;
    int64_t trackedFiles = 0;
    int64_t blobCount = 0;
    int64_t blobBytes = 0;
    int64_t scanDone = 0; // ベースラインの進捗
    int64_t scanTotal = 0;
    int64_t gapCount = 0; // 欠落区間の数
    int64_t watchCount = 0;
    bool watchFallback = false; // 監視をルートの再帰 1 本へ戻している
    bool dbDegraded = false;
    std::string dbReason;

    bool Baselining() const { return phase == "starting" || phase == "baselining"; }
};

// レビューの状態ごとの葉の数 (葉 = Hunk / 構造の変更 / Revision 自身)
struct TrackReviewCounts {
    int64_t pending = 0;
    int64_t approved = 0;
    int64_t rejected = 0;
    int64_t needsReview = 0;
    int64_t Total() const { return pending + approved + rejected + needsReview; }
    bool operator==(const TrackReviewCounts&) const = default;
};

struct TrackSessionRow {
    std::string sessionId;
    std::string provider;
    std::string source;
    std::string workspace;
    std::string status; // running / finished / interrupted
    int64_t startedAt = 0;
    int64_t endedAt = -1; // -1 = 終わっていない
    int64_t changeCount = 0; // そのセッションが候補に入っている変更の数
    // レビューの集約 (pending / approved / rejected / needs_review / partially_approved / partially_rejected。空 = サービスが返さなかった)
    std::string review;
    TrackReviewCounts reviewCounts;
    bool reviewTruncated = false; // 数えきれなかった (件数は読めた分だけ)
};

struct TrackChangeRow {
    int64_t revisionId = 0;
    int64_t changeId = 0;
    std::string path;
    std::string oldPath;
    std::string changeType; // added / modified / deleted / renamed
    std::string actor;
    std::string attribution;
    std::string completeness;
    std::string diffMode;
    // レビューの集約。空 = レビューの対象ではない (行為者が editor_user / restore)
    std::string review;
    std::string changeState; // active / superseded / ...
    int64_t observedAt = 0;
    int64_t hunkCount = 0;
    int64_t beforeSize = -1; // -1 = 無い
    int64_t afterSize = -1;
};

// hunks の 1 件 (本文つき)。oversize のときは本文が空で、大きさだけが入る
struct TrackHunk {
    int64_t hunkId = 0;
    int64_t ordinal = 0;
    int64_t oldStart = 0;
    int64_t oldCount = 0;
    int64_t newStart = 0;
    int64_t newCount = 0;
    std::string beforeText;
    std::string afterText;
    std::string contextPre;
    std::string contextPost;
    bool eolOnly = false;
    bool oversize = false;
    int64_t beforeBytes = 0;
    int64_t afterBytes = 0;
    std::string state;  // active / restored / conflicted
    std::string review; // 今のレビューの状態 (pending / approved / rejected / needs_review)
};

// scene_diff の 1 件 (構造の変更)。値は JSON の字句のまま。oversize のときは値が空で、大きさだけが入る
struct TrackSceneChange {
    int64_t sceneChangeId = 0;
    std::string documentId;
    std::string resolvedDocumentId; // 引き継ぎをたどった ID
    bool hasFileId = false;
    int64_t fileId = 0;
    std::string changeType; // EntityAdded ... PrefabPropagation / unparsed
    std::string component;
    std::string field;
    bool hasBefore = false;
    bool hasAfter = false;
    std::string beforeValue;
    std::string afterValue; // unparsed のときは理由の JSON 文字列
    bool oversize = false;
    int64_t beforeBytes = 0;
    int64_t afterBytes = 0;
    std::string review; // 今のレビューの状態 (pending / approved / rejected / needs_review)
};

struct TrackListFilter {
    std::string sessionId;
    std::string path;
    std::string changeType;
    // どれか 1 つに合えばよい (空 = 絞らない)。1 つならサービスへは文字列、複数なら配列で送る
    std::vector<std::string> attributions;
    std::string review;
    bool operator==(const TrackListFilter&) const = default;
};

// 表示のために取り直す部分。窓 (sub-10) が見ている部分だけをビット OR で伝える
enum class TrackPart : uint32_t { Status = 1, Sessions = 2, List = 4, Detail = 8 };
constexpr uint32_t TrackPartMask(TrackPart part) { return static_cast<uint32_t>(part); }
constexpr uint32_t kTrackAllParts = 15;

// 通知を受けたら「古い」印を付け、見ている部分だけを間引きつつ取り直す。通知のたびに全部は取り直さない
constexpr int64_t kTrackModelMinRefetchMs = 500;
// 一覧・セッション 1 ページの件数 (サービスの既定と同じ)
constexpr int kTrackModelPageSize = 50;
// 1 つの Revision の Hunk を取るページ数の上限 (サービスは 1 ページ約 768 KiB まで返す。取り終わらない暴走を止める)
constexpr int kTrackModelMaxHunkPages = 64;

// 追跡サービスから取った内容の保持 (表示用)。要求は TrackClient 経由で、応答はこのクラスが解釈する
class ChangeModel {
public:
    ChangeModel() = default;
    ChangeModel(const ChangeModel&) = delete;
    ChangeModel& operator=(const ChangeModel&) = delete;

    void Attach(TrackClient* client) { client_ = client; }

    // いま画面で見ている部分。見ていない部分は、古くなっても取りに行かない
    void SetInterest(uint32_t partMask) { interest_ = partMask; }

    // 接続が Ready になった / 切れた (全部を古い印にして、取り直しを間引きの対象外にする)
    void OnConnected();
    void OnDisconnected();

    // 通知 1 件 (TrackClient の通知ハンドラから)
    void OnNotification(const nlohmann::json& event);

    // 毎フレーム 1 回 (TrackClient::Poll の後)。必要な部分だけ要求を出す
    void Tick();

    // レビューを送ったあと (成功・失敗とも) 表示を実際の状態へ合わせる: 一覧・セッション・詳細を古い印にする (間引きはかかる)
    void Refresh();

    // 一覧のフィルタ。変えたら一覧を取り直す
    void SetFilter(const TrackListFilter& filter);
    const TrackListFilter& Filter() const { return filter_; }
    // 続きのページを足す (無ければ何もしない)
    void LoadMore();
    // 詳細を見る Revision。0 で解除
    void SelectRevision(int64_t revisionId);

    const TrackStatusInfo& Status() const { return status_; }
    const std::vector<TrackSessionRow>& Sessions() const { return sessions_; }
    const std::vector<TrackChangeRow>& Changes() const { return changes_; }
    bool HasMoreChanges() const { return !nextCursor_.empty(); }
    // 一覧の取得時点で、サービスがベースライン中 (一覧が不完全でありうる)
    bool ListIncomplete() const { return listBaselining_; }
    int64_t SelectedRevision() const { return selected_; }
    // describe の結果 (null = まだ無い)
    const nlohmann::json& Detail() const { return detail_; }
    // 選んだ Revision の Hunk (本文つき)。describe の後に全ページを取る。HunksComplete が偽の間は途中
    const std::vector<TrackHunk>& Hunks() const { return hunks_; }
    bool HunksComplete() const { return hunksComplete_; }
    // hunks の取得が失敗した (Hunk の本文だけ見えない。describe の内容は見える)
    bool HunksFailed() const { return hunksFailed_; }
    // Hunk の入れ替えのたびに増える。DiffView の入力を作り直す合図
    uint64_t HunksSerial() const { return hunksSerial_; }
    // 選んだ Revision の構造の変更 (scene_diff。describe に sceneSummary があるときだけ全ページを取る)
    const std::vector<TrackSceneChange>& SceneChanges() const { return sceneChanges_; }
    bool SceneChangesComplete() const { return sceneComplete_; }
    bool SceneChangesFailed() const { return sceneFailed_; }
    uint64_t SceneChangesSerial() const { return sceneSerial_; }

    // 試験用: その部分を取りに行った回数と、古い印
    int FetchCount(TrackPart part) const { return part_[Index(part)].fetches; }
    bool IsStale(TrackPart part) const { return part_[Index(part)].stale; }

    // 応答の解釈 (試験が直接叩く。純関数)
    static TrackStatusInfo ParseStatus(const nlohmann::json& result);
    static std::vector<TrackSessionRow> ParseSessions(const nlohmann::json& result);
    static std::vector<TrackChangeRow> ParseChanges(const nlohmann::json& result);
    static std::vector<TrackHunk> ParseHunks(const nlohmann::json& result);
    static std::vector<TrackSceneChange> ParseSceneChanges(const nlohmann::json& result);
    // 件数の JSON ({pending, approved, rejected, needsReview}) を読む。無ければ全部 0
    static TrackReviewCounts ParseReviewCounts(const nlohmann::json& counts);

private:
    struct PartState {
        bool stale = false;
        bool inFlight = false;
        bool force = false;      // 間引きを無視してすぐ取る
        int64_t lastFetchMs = 0;
        int fetches = 0;
    };
    static size_t Index(TrackPart part);
    void MarkStale(TrackPart part, bool force = false);
    void Fetch(TrackPart part, int64_t now);
    void FetchList(bool append);
    void FetchHunks(int64_t revisionId, std::vector<TrackHunk> collected, const std::string& cursor, int page);
    void FetchSceneChanges(int64_t revisionId, std::vector<TrackSceneChange> collected, const std::string& cursor, int page);
    void ResetSceneChanges();

    TrackClient* client_ = nullptr;
    uint32_t interest_ = 0;
    PartState part_[4];
    // 一覧を取り直した回数。追加ページの応答が、取り直しのあとに届いたら捨てる
    uint64_t listGeneration_ = 0;

    TrackStatusInfo status_;
    std::vector<TrackSessionRow> sessions_;
    std::vector<TrackChangeRow> changes_;
    std::string nextCursor_;
    bool listBaselining_ = false;
    TrackListFilter filter_;
    int64_t selected_ = 0;
    nlohmann::json detail_;
    std::vector<TrackHunk> hunks_;
    bool hunksComplete_ = false;
    bool hunksFailed_ = false;
    uint64_t hunksSerial_ = 0;
    std::vector<TrackSceneChange> sceneChanges_;
    bool sceneComplete_ = false;
    bool sceneFailed_ = false;
    uint64_t sceneSerial_ = 0;
};

} // namespace mye
