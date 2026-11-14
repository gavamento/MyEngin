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

struct TrackSessionRow {
    std::string sessionId;
    std::string provider;
    std::string source;
    std::string workspace;
    std::string status; // running / finished / interrupted
    int64_t startedAt = 0;
    int64_t endedAt = -1; // -1 = 終わっていない
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
    std::string review;
    int64_t observedAt = 0;
    int64_t hunkCount = 0;
    int64_t beforeSize = -1; // -1 = 無い
    int64_t afterSize = -1;
};

struct TrackListFilter {
    std::string sessionId;
    std::string path;
    std::string changeType;
    std::string attribution;
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

    // 試験用: その部分を取りに行った回数と、古い印
    int FetchCount(TrackPart part) const { return part_[Index(part)].fetches; }
    bool IsStale(TrackPart part) const { return part_[Index(part)].stale; }

    // 応答の解釈 (試験が直接叩く。純関数)
    static TrackStatusInfo ParseStatus(const nlohmann::json& result);
    static std::vector<TrackSessionRow> ParseSessions(const nlohmann::json& result);
    static std::vector<TrackChangeRow> ParseChanges(const nlohmann::json& result);

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
};

} // namespace mye
