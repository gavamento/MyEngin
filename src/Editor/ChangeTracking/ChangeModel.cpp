//====================================================================================
//                          ChangeModel.cpp
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          追跡サービスから取った表示用の保持 (通知で古い印・間引き)
//====================================================================================
#include "Editor/ChangeTracking/ChangeModel.h"

#include "Editor/ChangeTracking/TrackClient.h"
#include "Editor/ChangeTracking/TrackProtocol.h"

namespace mye {

namespace {

std::string Str(const nlohmann::json& j, const char* key)
{
    return j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : std::string();
}

int64_t Int(const nlohmann::json& j, const char* key, int64_t fallback)
{
    return j.contains(key) && j[key].is_number_integer() ? j[key].get<int64_t>() : fallback;
}

bool Bool(const nlohmann::json& j, const char* key)
{
    return j.contains(key) && j[key].is_boolean() && j[key].get<bool>();
}

const nlohmann::json& Child(const nlohmann::json& j, const char* key)
{
    static const nlohmann::json kNull;
    return j.is_object() && j.contains(key) ? j[key] : kNull;
}

bool IsOk(const nlohmann::json& response)
{
    return response.is_object() && Bool(response, "ok") && response.contains("result") && response["result"].is_object();
}

} // namespace

size_t ChangeModel::Index(TrackPart part)
{
    switch (part) {
    case TrackPart::Status: return 0;
    case TrackPart::Sessions: return 1;
    case TrackPart::List: return 2;
    case TrackPart::Detail: return 3;
    }
    return 0;
}

TrackStatusInfo ChangeModel::ParseStatus(const nlohmann::json& result)
{
    TrackStatusInfo s;
    if (!result.is_object()) {
        return s;
    }
    s.valid = true;
    s.phase = Str(result, "phase");
    s.uptimeMs = Int(result, "uptimeMs", 0);
    s.trackedFiles = Int(result, "trackedFiles", 0);
    const nlohmann::json& baseline = Child(result, "baseline");
    s.scanDone = Int(baseline, "scanDone", 0);
    s.scanTotal = Int(baseline, "scanTotal", 0);
    const nlohmann::json& blobs = Child(result, "blobs"); // DB を読めないときは null
    s.blobCount = Int(blobs, "count", 0);
    s.blobBytes = Int(blobs, "bytes", 0);
    s.gapCount = Int(Child(result, "gaps"), "count", 0);
    const nlohmann::json& watches = Child(result, "watches");
    s.watchCount = Int(watches, "count", 0);
    s.watchFallback = Bool(watches, "fallback");
    const nlohmann::json& db = Child(result, "db");
    const std::string dbState = Str(db, "state");
    s.dbDegraded = !dbState.empty() && dbState != "recording";
    s.dbReason = Str(db, "reason");
    return s;
}

std::vector<TrackSessionRow> ChangeModel::ParseSessions(const nlohmann::json& result)
{
    std::vector<TrackSessionRow> rows;
    const nlohmann::json& items = Child(result, "items");
    if (!items.is_array()) {
        return rows;
    }
    for (const nlohmann::json& item : items) {
        if (!item.is_object()) {
            continue;
        }
        TrackSessionRow row;
        row.sessionId = Str(item, "sessionId");
        row.provider = Str(item, "provider");
        row.source = Str(item, "source");
        row.workspace = Str(item, "workspace");
        row.status = Str(item, "status");
        row.startedAt = Int(item, "startedAt", 0);
        row.endedAt = Int(item, "endedAt", -1);
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<TrackChangeRow> ChangeModel::ParseChanges(const nlohmann::json& result)
{
    std::vector<TrackChangeRow> rows;
    const nlohmann::json& items = Child(result, "items");
    if (!items.is_array()) {
        return rows;
    }
    for (const nlohmann::json& item : items) {
        if (!item.is_object()) {
            continue;
        }
        TrackChangeRow row;
        row.revisionId = Int(item, "revisionId", 0);
        row.changeId = Int(item, "changeId", 0);
        row.path = Str(item, "path");
        row.oldPath = Str(item, "oldPath");
        row.changeType = Str(item, "changeType");
        row.actor = Str(item, "actor");
        row.attribution = Str(item, "attribution");
        row.completeness = Str(item, "completeness");
        row.diffMode = Str(item, "diffMode");
        row.review = Str(item, "review");
        row.observedAt = Int(item, "observedAt", 0);
        row.hunkCount = Int(item, "hunkCount", 0);
        row.beforeSize = Int(item, "beforeSize", -1);
        row.afterSize = Int(item, "afterSize", -1);
        rows.push_back(std::move(row));
    }
    return rows;
}

void ChangeModel::MarkStale(TrackPart part, bool force)
{
    PartState& state = part_[Index(part)];
    state.stale = true;
    state.force = state.force || force;
}

void ChangeModel::OnConnected()
{
    for (PartState& state : part_) {
        state.stale = true;
        state.force = true;
        state.inFlight = false;
    }
}

void ChangeModel::OnDisconnected()
{
    // 取り直し中の要求は TrackClient が失敗で返す。つながり直したら OnConnected が全部を古くする
    for (PartState& state : part_) {
        state.inFlight = false;
    }
}

void ChangeModel::OnNotification(const nlohmann::json& event)
{
    const std::string name = Str(event, "event");
    if (name == trackevent::kChangeRecorded) {
        MarkStale(TrackPart::List);
        MarkStale(TrackPart::Status); // 追跡中のファイル数・Blob の量が変わる
    } else if (name == trackevent::kAttributionUpdated) {
        MarkStale(TrackPart::List);
        if (selected_ != 0 && Int(event, "revisionId", -1) == selected_) {
            MarkStale(TrackPart::Detail);
        }
    } else if (name == trackevent::kCoverageGap) {
        MarkStale(TrackPart::Status);
    } else if (name == trackevent::kSessionChanged) {
        MarkStale(TrackPart::Sessions);
    } else if (name == trackevent::kResync) {
        // 通知を取りこぼした: 何が変わったか分からないので、全部をすぐ取り直す
        for (const TrackPart part : { TrackPart::Status, TrackPart::Sessions, TrackPart::List, TrackPart::Detail }) {
            MarkStale(part, true);
        }
    }
    // shutting_down は何もしない (接続が切れれば OnDisconnected)
}

void ChangeModel::SetFilter(const TrackListFilter& filter)
{
    if (filter == filter_) {
        return;
    }
    filter_ = filter;
    // 古いフィルタで飛んでいる要求の応答 (追加ページを含む) は捨てる
    ++listGeneration_;
    MarkStale(TrackPart::List, true);
}

void ChangeModel::SelectRevision(int64_t revisionId)
{
    if (revisionId == selected_) {
        return;
    }
    selected_ = revisionId;
    detail_ = nlohmann::json();
    if (revisionId != 0) {
        MarkStale(TrackPart::Detail, true);
    } else {
        part_[Index(TrackPart::Detail)].stale = false;
    }
}

void ChangeModel::LoadMore()
{
    if (!nextCursor_.empty() && !part_[Index(TrackPart::List)].inFlight) {
        FetchList(true);
    }
}

void ChangeModel::Tick()
{
    if (client_ == nullptr || !client_->CanRequest()) {
        return;
    }
    const int64_t now = client_->NowMs();
    for (const TrackPart part : { TrackPart::Status, TrackPart::Sessions, TrackPart::List, TrackPart::Detail }) {
        PartState& state = part_[Index(part)];
        if ((interest_ & TrackPartMask(part)) == 0 || !state.stale || state.inFlight) {
            continue;
        }
        if (part == TrackPart::Detail && selected_ == 0) {
            continue;
        }
        if (state.force || now - state.lastFetchMs >= kTrackModelMinRefetchMs || state.fetches == 0) {
            Fetch(part, now);
        }
    }
}

void ChangeModel::Fetch(TrackPart part, int64_t now)
{
    PartState& state = part_[Index(part)];
    state.stale = false;
    state.force = false;
    state.inFlight = true;
    state.lastFetchMs = now;
    ++state.fetches;

    switch (part) {
    case TrackPart::Status:
        client_->Request(trackop::kStatus, nlohmann::json::object(), [this](const nlohmann::json& response) {
            part_[Index(TrackPart::Status)].inFlight = false;
            if (IsOk(response)) {
                status_ = ParseStatus(response["result"]);
                // サービスが DB へ書けないなら、窓の状態もそう見せる (接続は保つ)
                client_->SetDbDegraded(status_.dbDegraded);
            }
        });
        break;
    case TrackPart::Sessions:
        client_->Request(trackop::kSessions, { { "limit", kTrackModelPageSize } }, [this](const nlohmann::json& response) {
            part_[Index(TrackPart::Sessions)].inFlight = false;
            if (IsOk(response)) {
                sessions_ = ParseSessions(response["result"]);
            }
        });
        break;
    case TrackPart::List:
        FetchList(false);
        break;
    case TrackPart::Detail: {
        const int64_t asked = selected_;
        client_->Request(trackop::kDescribe, { { "revisionId", asked } }, [this, asked](const nlohmann::json& response) {
            part_[Index(TrackPart::Detail)].inFlight = false;
            // 選び直したあとに届いた古い応答は捨てる
            if (asked == selected_ && IsOk(response)) {
                detail_ = response["result"];
            }
        });
        break;
    }
    }
}

void ChangeModel::FetchList(bool append)
{
    PartState& state = part_[Index(TrackPart::List)];
    state.inFlight = true;
    if (!append) {
        ++listGeneration_;
    }
    const uint64_t generation = listGeneration_;
    nlohmann::json args = { { "limit", kTrackModelPageSize } };
    if (!filter_.sessionId.empty()) {
        args["sessionId"] = filter_.sessionId;
    }
    if (!filter_.path.empty()) {
        args["path"] = filter_.path;
    }
    if (!filter_.changeType.empty()) {
        args["changeType"] = filter_.changeType;
    }
    if (!filter_.attribution.empty()) {
        args["attribution"] = filter_.attribution;
    }
    if (!filter_.review.empty()) {
        args["review"] = filter_.review;
    }
    if (append) {
        args["cursor"] = nextCursor_;
    }
    client_->Request(trackop::kList, args, [this, append, generation](const nlohmann::json& response) {
        part_[Index(TrackPart::List)].inFlight = false;
        if (generation != listGeneration_ || !IsOk(response)) {
            return; // 取り直しのあとに届いた古いページ、または失敗
        }
        const nlohmann::json& result = response["result"];
        std::vector<TrackChangeRow> rows = ParseChanges(result);
        if (append) {
            changes_.insert(changes_.end(), rows.begin(), rows.end());
        } else {
            changes_ = std::move(rows);
        }
        nextCursor_ = Str(result, "nextCursor");
        listBaselining_ = Bool(Child(result, "coverage"), "baselining");
    });
}

} // namespace mye
