//====================================================================================
//                          ReviewManager.cpp
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          レビューの送信・まとめての対象の準備 (応答を待ってから表示を変える)
//====================================================================================
#include "Editor/ChangeTracking/ReviewManager.h"

#include <algorithm>
#include <set>

#include "Editor/ChangeTracking/TrackClient.h"
#include "Editor/ChangeTracking/TrackProtocol.h"

namespace mye {

namespace {

// UTF-8 の文字数で maxChars に切る (継続バイトの途中では切らない)
std::string ClipChars(const std::string& text, size_t maxChars)
{
    size_t chars = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) {
            if (chars == maxChars) {
                return text.substr(0, i);
            }
            ++chars;
        }
    }
    return text;
}

bool ResponseOk(const nlohmann::json& response)
{
    return response.is_object() && response.contains("ok") && response["ok"].is_boolean() && response["ok"].get<bool>();
}

std::string ErrorField(const nlohmann::json& response, const char* key)
{
    if (response.is_object() && response.contains("error") && response["error"].is_object() && response["error"].contains(key)
        && response["error"][key].is_string()) {
        return response["error"][key].get<std::string>();
    }
    return std::string();
}

} // namespace

const char* ReviewActionWire(ReviewAction action)
{
    switch (action) {
    case ReviewAction::Approve: return "approved";
    case ReviewAction::Reject: return "rejected";
    case ReviewAction::NeedsReview: return "needs_review";
    case ReviewAction::Reopen: return "reopened";
    }
    return "reopened";
}

ReviewTarget ReviewManager::ForRevision(int64_t revisionId)
{
    return ReviewTarget{ revisionId, "revision", std::string() };
}

ReviewTarget ReviewManager::ForHunk(int64_t revisionId, int64_t hunkId)
{
    return ReviewTarget{ revisionId, "hunk", std::to_string(hunkId) };
}

ReviewTarget ReviewManager::ForEntity(int64_t revisionId, int64_t fileId)
{
    return ReviewTarget{ revisionId, "entity", std::to_string(fileId) };
}

ReviewTarget ReviewManager::ForComponent(int64_t revisionId, int64_t fileId, const std::string& component)
{
    return ReviewTarget{ revisionId, "component", std::to_string(fileId) + "/" + component };
}

ReviewTarget ReviewManager::ForField(int64_t revisionId, int64_t fileId, const std::string& component, const std::string& field)
{
    return ReviewTarget{ revisionId, "field", std::to_string(fileId) + "/" + component + "/" + field };
}

ReviewTarget ReviewManager::ForSceneChange(int64_t revisionId, const TrackSceneChange& change)
{
    if (!change.hasFileId) {
        return ForRevision(revisionId); // 文書全体の変更は、Revision 全体の範囲だけが効く
    }
    if (!change.field.empty()) {
        return ForField(revisionId, change.fileId, change.component, change.field);
    }
    if (!change.component.empty()) {
        return ForComponent(revisionId, change.fileId, change.component);
    }
    return ForEntity(revisionId, change.fileId);
}

std::vector<int64_t> ReviewManager::RevisionsOf(const std::vector<TrackChangeRow>& rows, const std::vector<int64_t>& selected, int* skipped)
{
    std::vector<int64_t> out;
    int skippedCount = 0;
    std::set<int64_t> seen;
    for (const int64_t id : selected) {
        if (!seen.insert(id).second) {
            continue;
        }
        const auto it = std::find_if(rows.begin(), rows.end(), [id](const TrackChangeRow& row) { return row.revisionId == id; });
        if (it != rows.end() && IsReviewable(*it)) {
            out.push_back(id);
        } else {
            ++skippedCount;
        }
    }
    if (skipped != nullptr) {
        *skipped = skippedCount;
    }
    return out;
}

std::vector<std::vector<ReviewTarget>> ReviewManager::Split(std::vector<ReviewTarget> targets, size_t maxPerCall)
{
    std::vector<std::vector<ReviewTarget>> chunks;
    if (maxPerCall == 0) {
        return chunks;
    }
    for (size_t i = 0; i < targets.size(); i += maxPerCall) {
        const size_t end = (std::min)(targets.size(), i + maxPerCall);
        chunks.emplace_back(std::make_move_iterator(targets.begin() + static_cast<std::ptrdiff_t>(i)),
                            std::make_move_iterator(targets.begin() + static_cast<std::ptrdiff_t>(end)));
    }
    return chunks;
}

nlohmann::json ReviewManager::BuildArgs(const std::vector<ReviewTarget>& targets, ReviewAction action, const std::string& note)
{
    nlohmann::json list = nlohmann::json::array();
    for (const ReviewTarget& target : targets) {
        nlohmann::json scope = { { "kind", target.kind } };
        if (target.kind != "revision") {
            scope["ref"] = target.ref;
        }
        list.push_back({ { "revisionId", target.revisionId }, { "scope", std::move(scope) } });
    }
    nlohmann::json args = { { "targets", std::move(list) }, { "state", ReviewActionWire(action) } };
    const std::string clipped = ClipChars(note, kReviewNoteMaxChars);
    if (!clipped.empty()) {
        args["note"] = clipped;
    }
    return args;
}

void ReviewManager::ClearResult()
{
    if (status_ != Status::Sending) {
        status_ = Status::Idle;
        errorCode_.clear();
        errorDetail_.clear();
        recorded_ = 0;
        total_ = 0;
    }
}

void ReviewManager::Reset()
{
    ++sendGeneration_;
    ++collectGeneration_;
    status_ = Status::Idle;
    collect_ = Collect::None;
    errorCode_.clear();
    errorDetail_.clear();
    recorded_ = 0;
    total_ = 0;
    prepared_.clear();
    preparedSkipped_ = 0;
    preparedSession_.clear();
}

void ReviewManager::Fail(const std::string& code, const std::string& detail)
{
    status_ = Status::Failed;
    errorCode_ = code;
    errorDetail_ = detail;
    // 記録されたかどうか分からない (時間切れ) こともある。表示を実際の状態へ合わせるため取り直す
    if (model_ != nullptr) {
        model_->Refresh();
    }
}

bool ReviewManager::Submit(std::vector<ReviewTarget> targets, ReviewAction action, const std::string& note)
{
    if (status_ == Status::Sending) {
        return false;
    }
    errorCode_.clear();
    errorDetail_.clear();
    recorded_ = 0;
    total_ = targets.size();
    if (targets.empty()) {
        status_ = Status::Idle;
        return false;
    }
    if (client_ == nullptr || !client_->CanRequest()) {
        // 切断中・無効のときは I/O を呼ばない
        Fail(trackerror::kNotConnected, std::string());
        return false;
    }
    status_ = Status::Sending;
    ++sendGeneration_;
    SendChunk(Split(std::move(targets)), 0, action, note, sendGeneration_);
    return true;
}

void ReviewManager::SendChunk(std::vector<std::vector<ReviewTarget>> chunks, size_t index, ReviewAction action, std::string note,
                              uint64_t generation)
{
    if (index >= chunks.size()) {
        status_ = Status::Done;
        if (model_ != nullptr) {
            model_->Refresh();
        }
        return;
    }
    const nlohmann::json args = BuildArgs(chunks[index], action, note);
    const size_t chunkSize = chunks[index].size();
    client_->Request(trackop::kReviewSet, args,
                     [this, chunks = std::move(chunks), index, action, note = std::move(note), generation,
                      chunkSize](const nlohmann::json& response) mutable {
                         if (generation != sendGeneration_) {
                             return; // 取り消した・つなぎ直したあとに届いた古い応答
                         }
                         if (!ResponseOk(response)) {
                             Fail(ErrorField(response, "code"), ErrorField(response, "detail"));
                             return;
                         }
                         recorded_ += chunkSize;
                         SendChunk(std::move(chunks), index + 1, action, std::move(note), generation);
                     });
}

void ReviewManager::PrepareRevisions(std::vector<int64_t> revisionIds, int skipped)
{
    ++collectGeneration_;
    prepared_ = std::move(revisionIds);
    preparedSkipped_ = skipped;
    preparedSession_.clear();
    collect_ = Collect::Ready;
}

void ReviewManager::CancelPrepared()
{
    ++collectGeneration_;
    prepared_.clear();
    preparedSkipped_ = 0;
    preparedSession_.clear();
    collect_ = Collect::None;
}

void ReviewManager::PrepareSession(const std::string& sessionId)
{
    ++collectGeneration_;
    prepared_.clear();
    preparedSkipped_ = 0;
    preparedSession_ = sessionId;
    errorCode_.clear();
    errorDetail_.clear();
    if (client_ == nullptr || !client_->CanRequest()) {
        collect_ = Collect::Failed;
        errorCode_ = trackerror::kNotConnected;
        return;
    }
    collect_ = Collect::Collecting;
    CollectPage(sessionId, std::string(), {}, 0, collectGeneration_);
}

void ReviewManager::CollectPage(const std::string& sessionId, const std::string& cursor, std::vector<int64_t> collected, int page,
                                uint64_t generation)
{
    nlohmann::json args = { { "sessionId", sessionId }, { "limit", kReviewCollectPageSize } };
    if (!cursor.empty()) {
        args["cursor"] = cursor;
    }
    client_->Request(trackop::kList, args,
                     [this, sessionId, page, generation, collected = std::move(collected)](const nlohmann::json& response) mutable {
                         if (generation != collectGeneration_) {
                             return;
                         }
                         if (!ResponseOk(response) || !response.contains("result") || !response["result"].is_object()) {
                             collect_ = Collect::Failed;
                             errorCode_ = ErrorField(response, "code");
                             errorDetail_ = ErrorField(response, "detail");
                             return;
                         }
                         const nlohmann::json& result = response["result"];
                         for (const TrackChangeRow& row : ChangeModel::ParseChanges(result)) {
                             if (IsReviewable(row)) {
                                 collected.push_back(row.revisionId);
                             } else {
                                 ++preparedSkipped_;
                             }
                         }
                         const std::string next = result.contains("nextCursor") && result["nextCursor"].is_string()
                                                      ? result["nextCursor"].get<std::string>()
                                                      : std::string();
                         if (!next.empty()) {
                             if (page + 1 >= kReviewCollectMaxPages || !client_->CanRequest()) {
                                 // 集めきれないまま一部だけをレビューしない
                                 collect_ = Collect::Failed;
                                 errorCode_ = trackerror::kTimeout;
                                 errorDetail_ = "too many changes to collect";
                                 return;
                             }
                             CollectPage(sessionId, next, std::move(collected), page + 1, generation);
                             return;
                         }
                         prepared_ = std::move(collected);
                         collect_ = Collect::Ready;
                     });
}

bool ReviewManager::CommitPrepared(ReviewAction action, const std::string& note)
{
    if (collect_ != Collect::Ready || prepared_.empty()) {
        return false;
    }
    std::vector<ReviewTarget> targets;
    targets.reserve(prepared_.size());
    for (const int64_t id : prepared_) {
        targets.push_back(ForRevision(id));
    }
    const bool sent = Submit(std::move(targets), action, note);
    if (sent) {
        CancelPrepared();
    }
    return sent;
}

} // namespace mye
