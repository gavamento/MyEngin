//====================================================================================
//                          RestorePreview.cpp
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          復元のプレビューの準備 (内容を集めて計画を作る。書かない)
//====================================================================================
#include "Editor/ChangeTracking/RestorePreview.h"

#include <algorithm>

#include "Editor/ChangeTracking/TrackClient.h"
#include "Editor/ChangeTracking/TrackProtocol.h"
#include "Engine/Platform/Sha256.h"

namespace mye {

namespace {

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

void RestorePreview::Close()
{
    ++generation_;
    phase_ = Phase::Closed;
    revision_ = RestoreRevision();
    hunks_.clear();
    allHunks_.clear();
    targetPaths_.clear();
    now_ = NowState();
    nowOld_ = NowState();
    pathIssue_ = PathIssue::None;
    connected_ = false;
    hasPlan_ = false;
    blobMissing_ = false;
    blobPending_ = false;
    readsStarted_ = false;
    plan_ = RestorePlan();
    jobs_.clear();
    errorCode_.clear();
    errorDetail_.clear();
    changeView_.clear();
    conflictView_.clear();
    ++viewSerial_;
}

void RestorePreview::FailWith(const std::string& code, const std::string& detail)
{
    phase_ = Phase::Failed;
    errorCode_ = code;
    errorDetail_ = detail;
    hasPlan_ = false;
    jobs_.clear();
    blobPending_ = false;
}

void RestorePreview::Open(RestoreScope scope, const nlohmann::json& detail, const std::vector<TrackHunk>& allHunks, const std::vector<int64_t>& selectedHunkIds,
                          const std::wstring& projectRoot, TrackClient& client, RestoreFileIo& io)
{
    Close();
    scope_ = scope;
    revision_ = ParseRestoreRevision(detail);
    allHunks_ = allHunks;
    projectRoot_ = projectRoot;
    targetPaths_ = RestoreTargetPaths(revision_);
    if (scope == RestoreScope::Hunks) {
        for (const TrackHunk& hunk : allHunks) {
            if (std::find(selectedHunkIds.begin(), selectedHunkIds.end(), hunk.hunkId) != selectedHunkIds.end()) {
                hunks_.push_back(hunk);
            }
        }
    } else {
        hunks_ = allHunks;
    }
    phase_ = Phase::Preparing;

    // 追跡に接続できていなければ、内容を取れないので I/O を 1 回も呼ばない (ゲートが理由を出す)
    connected_ = client.CanRequest();
    if (!connected_) {
        phase_ = Phase::Ready;
        return;
    }
    // 書き先の検査 (1 回目)。拒否されたら、ファイルを読まずに計画 (戻せない) を作る
    for (const std::string& rel : targetPaths_) {
        const PathIssue issue = CheckRestorePath(projectRoot_, rel, io);
        if (issue != PathIssue::None) {
            pathIssue_ = issue;
            break;
        }
    }
    if (pathIssue_ != PathIssue::None) {
        Finish();
        return;
    }
    blobMissing_ = revision_.before.state == RestoreBlob::Missing || revision_.before.state == RestoreBlob::Evicted;
    // 戻す先の内容 (Before の Blob) が本当に取れるか。describe の印は古いことがあるので、1 バイトだけ引いて確かめる
    const bool needsBlob = scope == RestoreScope::File && revision_.changeType != "added" && revision_.before.state == RestoreBlob::Present && !revision_.before.hash.empty();
    if (needsBlob) {
        blobPending_ = true;
        const uint64_t generation = generation_;
        const int64_t expectedSize = revision_.before.size;
        client.Request(trackop::kBlobGet, { { "hash", revision_.before.hash }, { "offset", 0 }, { "length", 1 } },
                       [this, generation, expectedSize](const nlohmann::json& response) {
                           if (generation != generation_) {
                               return; // 閉じた・開き直したあとに届いた古い応答
                           }
                           blobPending_ = false;
                           if (!ResponseOk(response)) {
                               const std::string code = ErrorField(response, "code");
                               if (code == "blob_missing") {
                                   blobMissing_ = true;
                               } else {
                                   FailWith(code, ErrorField(response, "detail"));
                                   return;
                               }
                           } else if (expectedSize >= 0 && response.contains("result") && response["result"].is_object() && response["result"].contains("size")
                                      && response["result"]["size"].is_number_integer() && response["result"]["size"].get<int64_t>() != expectedSize) {
                               blobMissing_ = true; // 記録した大きさと合わない Blob は使わない
                           }
                           if (readsStarted_ && jobs_.empty()) {
                               Finish();
                           }
                       });
        if (phase_ == Phase::Failed) {
            return;
        }
    }
    StartReads(io);
    readsStarted_ = true;
    if (jobs_.empty() && !blobPending_ && phase_ == Phase::Preparing) {
        Finish();
    }
}

void RestorePreview::StartReads(RestoreFileIo& io)
{
    auto classify = [&](const std::string& rel, bool oldPath) {
        ReadJob job;
        job.oldPath = oldPath;
        job.abs = JoinProjectPath(projectRoot_, rel);
        NowState& target = TargetOf(job);
        const uint32_t attrs = io.Attributes(job.abs);
        if (attrs == kRestoreAttrAbsent) {
            target.kind = NowKind::Missing;
            job.done = true;
        } else if ((attrs & kRestoreAttrReparse) != 0) {
            target.kind = NowKind::Link;
            job.done = true;
        } else if ((attrs & kRestoreAttrDirectory) != 0) {
            target.kind = NowKind::Directory;
            job.done = true;
        }
        if (!job.done) {
            jobs_.push_back(std::move(job));
        }
    };
    classify(revision_.path, false);
    if (revision_.changeType == "renamed" && !revision_.oldPath.empty()) {
        classify(revision_.oldPath, true);
    }
}

void RestorePreview::AdvanceRead(ReadJob& job, RestoreFileIo& io)
{
    NowState& target = TargetOf(job);
    std::string chunk;
    uint64_t total = 0;
    switch (io.Read(job.abs, job.offset, kRestoreReadChunkBytes, chunk, total)) {
    case RestoreFileIo::ReadStatus::Absent:
        target = NowState();
        target.kind = NowKind::Missing;
        job.done = true;
        return;
    case RestoreFileIo::ReadStatus::Error:
        target = NowState();
        target.kind = NowKind::Unreadable;
        job.done = true;
        return;
    case RestoreFileIo::ReadStatus::Ok: break;
    }
    if (total > kRestoreReadMaxBytes) {
        target = NowState();
        target.kind = NowKind::TooLarge;
        target.size = static_cast<int64_t>(total);
        job.done = true;
        return;
    }
    target.bytes += chunk;
    job.offset += chunk.size();
    if (chunk.empty() || job.offset >= total) {
        target.hash = Sha256Hex(target.bytes);
        target.size = static_cast<int64_t>(target.bytes.size());
        target.kind = target.hash.empty() ? NowKind::Unreadable : NowKind::File;
        job.done = true;
    }
}

void RestorePreview::Tick(RestoreFileIo& io)
{
    if (phase_ != Phase::Preparing || jobs_.empty()) {
        return;
    }
    // 1 フレームに読むのは 1 つの塊だけ
    for (ReadJob& job : jobs_) {
        if (!job.done) {
            AdvanceRead(job, io);
            break;
        }
    }
    if (std::all_of(jobs_.begin(), jobs_.end(), [](const ReadJob& job) { return job.done; })) {
        jobs_.clear();
        if (!blobPending_) {
            Finish();
        }
    }
}

void RestorePreview::Finish()
{
    // 記録した Before の Blob が取れなかったら、戻す先の内容が無い
    if (blobMissing_ && revision_.before.state == RestoreBlob::Present) {
        revision_.before.state = RestoreBlob::Missing;
    }
    RestoreInput input;
    input.scope = scope_;
    input.revision = revision_;
    input.hunks = scope_ == RestoreScope::Hunks ? hunks_ : std::vector<TrackHunk>();
    input.now = now_;
    input.nowOld = nowOld_;
    input.pathIssue = pathIssue_;
    plan_ = BuildRestorePlan(input);
    hasPlan_ = true;
    changeView_ = ReverseHunkView(hunks_);
    conflictView_.clear();
    if (scope_ == RestoreScope::Hunks && plan_.verdict == RestoreVerdict::Conflict) {
        conflictView_ = ConflictHunkView(now_.bytes, hunks_, plan_.hunks);
    }
    ++viewSerial_;
    phase_ = Phase::Ready;
}

} // namespace mye
