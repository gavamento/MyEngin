//====================================================================================
//                          RestoreRunner.cpp
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          復元の実行 (ジャーナル → 適用 → 検証 → 巻き戻し → 起動時の回復)
//====================================================================================
#include "Editor/ChangeTracking/RestoreRunner.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>

#include "Editor/ChangeTracking/BlobHeadView.h"
#include "Editor/ChangeTracking/PathGuard.h"
#include "Editor/ChangeTracking/TrackClient.h"
#include "Editor/ChangeTracking/TrackProtocol.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Platform/Sha256.h"

namespace fs = std::filesystem;
using nlohmann::json;

namespace mye {

namespace {

// blob_get の 1 回の取得量 (サービスの上限 512 KiB)
constexpr uint64_t kFetchChunkBytes = 512u * 1024u;
constexpr int64_t kRecordRetryMs = 3000;

std::string PreName(size_t index)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "pre/%04zu.bin", index + 1);
    return buf;
}

std::string StageName(size_t opIndex)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "new/%04zu.bin", opIndex + 1);
    return buf;
}

bool ResponseOk(const json& response)
{
    return response.is_object() && response.contains("ok") && response["ok"].is_boolean() && response["ok"].get<bool>();
}

std::string ErrorCode(const json& response)
{
    if (response.is_object() && response.contains("error") && response["error"].is_object() && response["error"].contains("code")
        && response["error"]["code"].is_string()) {
        return response["error"]["code"].get<std::string>();
    }
    return std::string();
}

bool SamePlanOps(const std::vector<RestoreOp>& a, const std::vector<RestoreOp>& b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].kind != b[i].kind || a[i].path != b[i].path || a[i].toPath != b[i].toPath || a[i].source != b[i].source || a[i].hash != b[i].hash
            || a[i].size != b[i].size) {
            return false;
        }
    }
    return true;
}

bool ReadAll(RestoreFileIo& io, const std::wstring& abs, std::string& out)
{
    out.clear();
    uint64_t offset = 0;
    uint64_t total = 0;
    for (;;) {
        std::string chunk;
        const RestoreFileIo::ReadStatus status = io.Read(abs, offset, kRestoreReadChunkBytes, chunk, total);
        if (status != RestoreFileIo::ReadStatus::Ok) {
            return false;
        }
        out += chunk;
        offset += chunk.size();
        if (chunk.empty() || offset >= total) {
            break;
        }
    }
    return out.size() == total;
}

std::wstring ParentOf(const std::wstring& abs)
{
    return fs::path(abs).parent_path().wstring();
}

} // namespace

RestoreRunner::~RestoreRunner()
{
    // BeginBatch を呼んだままにしない (どの経路でも EndBatch を 1 回呼ぶ)
    if (writeSection_) {
        CloseWriteSection({});
    }
}

void RestoreRunner::Shutdown()
{
    ++generation_;
    if (writeSection_) {
        const std::vector<BatchChange> changes = ChangesFor(false);
        CloseWriteSection(changes);
    }
    // 進行中のジャーナルは書き換えない (applying / rolling_back のまま = 次の起動の回復の対象)
    phase_ = RestoreProgress::Phase::Idle;
    fetch_ = FetchState();
    saveJob_ = Job();
    verifyJob_ = Job();
    rollbackJob_ = Job();
    hooks_ = RestoreRunHooks();
}

void RestoreRunner::Configure(const std::wstring& projectRoot, TrackClient* client)
{
    root_ = projectRoot;
    client_ = client;
}

void RestoreRunner::SetIo(std::unique_ptr<RestoreFileIo> read, std::unique_ptr<RestoreWriteIo> write)
{
    readIo_ = std::move(read);
    writeIo_ = std::move(write);
}

RestoreWriteIo& RestoreRunner::Write()
{
    if (!writeIo_) {
        writeIo_ = MakeSystemRestoreWriteIo();
    }
    return *writeIo_;
}

RestoreFileIo& RestoreRunner::Read()
{
    if (!readIo_) {
        readIo_ = MakeSystemRestoreFileIo();
    }
    return *readIo_;
}

RestoreJournalStore RestoreRunner::Store()
{
    return RestoreJournalStore(root_, Write());
}

std::wstring RestoreRunner::AbsOf(const std::string& rel) const
{
    return JoinProjectPath(root_, rel);
}

// ---- 起動時の回復 ----

void RestoreRunner::RefreshRecoveries()
{
    recoveries_.clear();
    if (root_.empty()) {
        return;
    }
    for (RestoreJournalStore::Entry& entry : Store().Scan()) {
        if (entry.broken || NeedsRecovery(entry.manifest.state)) {
            recoveries_.push_back(std::move(entry));
        }
    }
}

void RestoreRunner::ScanRecovery()
{
    if (root_.empty() || Busy()) {
        return;
    }
    // prepared は何も書いていないので捨てる。古い committed などは保存期間を過ぎたら消す (自分のジャーナルの掃除だけ)
    Store().Prune(NowMs(), kRestoreJournalKeepMs);
    RefreshRecoveries();
}

std::vector<RecoveryFileView> RestoreRunner::InspectRecovery(const std::string& id) const
{
    std::vector<RecoveryFileView> views;
    for (const RestoreJournalStore::Entry& entry : recoveries_) {
        if (entry.id != id || entry.broken) {
            continue;
        }
        for (const JournalFile& file : entry.manifest.files) {
            RecoveryFileView view;
            view.path = file.path;
            view.existed = file.existed;
            view.preHash = file.preHash;
            view.intendedHash = file.IntendedHash();
            const std::wstring abs = JoinProjectPath(root_, file.path);
            std::error_code ec;
            if (!fs::exists(fs::path(abs), ec)) {
                view.current = view.preHash.empty() ? RecoveryFileView::Current::Pre : (view.intendedHash.empty() ? RecoveryFileView::Current::Intended : RecoveryFileView::Current::Other);
            } else if (!fs::is_regular_file(fs::path(abs), ec) || !Sha256FileHex(abs, view.currentHash)) {
                view.current = RecoveryFileView::Current::Unreadable;
            } else if (view.currentHash == view.preHash) {
                view.current = RecoveryFileView::Current::Pre;
            } else if (view.currentHash == view.intendedHash) {
                view.current = RecoveryFileView::Current::Intended;
            } else {
                view.current = RecoveryFileView::Current::Other;
            }
            views.push_back(std::move(view));
        }
    }
    return views;
}

bool RestoreRunner::KeepCurrent(const std::string& id)
{
    for (RestoreJournalStore::Entry& entry : recoveries_) {
        if (entry.id == id && !entry.broken) {
            JournalManifest manifest = entry.manifest;
            manifest.state = JournalState::Recovered;
            manifest.resolution = "kept_current";
            manifest.updatedAt = NowMs();
            if (!Store().Save(manifest)) {
                return false;
            }
            SendRecord(manifest, "recovered", "kept_current");
            RefreshRecoveries();
            return true;
        }
    }
    return false;
}

bool RestoreRunner::Dismiss(const std::string& id)
{
    for (const RestoreJournalStore::Entry& entry : recoveries_) {
        if (entry.id == id && entry.broken) {
            const bool ok = Store().MarkDismissed(id);
            RefreshRecoveries();
            return ok;
        }
    }
    return false;
}

bool RestoreRunner::StartRecoveryRollback(const std::string& id, std::function<std::vector<RestoreBlock>(bool)> blocks)
{
    if (Busy() || client_ == nullptr) {
        return false;
    }
    const RestoreJournalStore::Entry* found = nullptr;
    for (const RestoreJournalStore::Entry& entry : recoveries_) {
        if (entry.id == id && !entry.broken) {
            found = &entry;
        }
    }
    AcknowledgeResult();
    result_ = RestoreRunResult();
    result_.restoreId = id;
    result_.fromRecovery = true;
    if (found == nullptr) {
        result_.outcome = RestoreOutcome::Blocked;
        result_.reason = RestoreStopReason::NotRecoverable;
        phase_ = RestoreProgress::Phase::Done;
        return false;
    }
    manifest_ = found->manifest;
    result_.totalOps = static_cast<int>(manifest_.ops.size());
    request_ = StartRequest();
    request_.blocks = std::move(blocks);
    recovery_ = true;
    ++generation_;
    std::vector<RestoreBlock> blockList;
    if (!Gate(blockList, false)) {
        result_.outcome = RestoreOutcome::Blocked;
        result_.reason = RestoreStopReason::Gate;
        result_.blocks = std::move(blockList);
        phase_ = RestoreProgress::Phase::Done;
        recovery_ = false;
        return false;
    }
    if (hooks_.beginBatch) {
        hooks_.beginBatch();
    }
    writeSection_ = true;
    BeginRollback(RestoreStopReason::None, "recovery");
    return true;
}

// ---- 実行の開始と終わり ----

void RestoreRunner::AcknowledgeResult()
{
    if (phase_ == RestoreProgress::Phase::Done) {
        phase_ = RestoreProgress::Phase::Idle;
    }
}

RestoreProgress RestoreRunner::Progress() const
{
    RestoreProgress p;
    p.phase = phase_;
    switch (phase_) {
    case RestoreProgress::Phase::Rechecking:
        p.canCancel = true;
        break;
    case RestoreProgress::Phase::Fetching:
        p.step = static_cast<int>(fetchPos_);
        p.total = static_cast<int>(fetchOps_.size());
        p.path = fetchPos_ < fetchOps_.size() ? ops_[fetchOps_[fetchPos_]].path : std::string();
        p.canCancel = true;
        break;
    case RestoreProgress::Phase::Saving:
        p.step = static_cast<int>(savePos_);
        p.total = static_cast<int>(touched_.size());
        p.path = savePos_ < touched_.size() ? touched_[savePos_].rel : std::string();
        p.canCancel = true;
        break;
    case RestoreProgress::Phase::Applying:
        p.step = static_cast<int>(opPos_);
        p.total = static_cast<int>(ops_.size());
        p.path = opPos_ < ops_.size() ? ops_[opPos_].path : std::string();
        p.canCancel = !applyStarted_;
        break;
    case RestoreProgress::Phase::Verifying:
        p.step = static_cast<int>(verifyPos_);
        p.total = static_cast<int>(touched_.size());
        p.path = verifyPos_ < touched_.size() ? touched_[verifyPos_].rel : std::string();
        break;
    case RestoreProgress::Phase::RollingBack:
        p.step = static_cast<int>(rollbackPos_);
        p.total = static_cast<int>(manifest_.files.size());
        p.path = rollbackPos_ < manifest_.files.size() ? manifest_.files[rollbackPos_].path : std::string();
        break;
    default: break;
    }
    return p;
}

bool RestoreRunner::Start(StartRequest request)
{
    if (Pending() || client_ == nullptr || root_.empty() || request.previewOps.empty()) {
        return false;
    }
    AcknowledgeResult();
    ++generation_;
    result_ = RestoreRunResult();
    request_ = std::move(request);
    recovery_ = false;
    ops_.clear();
    touched_.clear();
    manifest_ = JournalManifest();
    fetched_.clear();
    staged_.clear();
    fetchOps_.clear();
    fetchPos_ = 0;
    fetch_ = FetchState();
    hasResponse_ = false;
    savePos_ = 0;
    saveJob_ = Job();
    saveStage_ = 0;
    savedHashes_.clear();
    applyStarted_ = false;
    opPos_ = 0;
    verifyPos_ = 0;
    verifyJob_ = Job();
    rollbackPos_ = 0;
    rollbackJob_ = Job();
    rollbackStage_ = 0;
    unresolved_.clear();
    blobMissing_ = false;
    result_.totalOps = static_cast<int>(request_.previewOps.size());
    // 実行の直前の判定し直し: 今のファイルを読み直して計画を作り直す (パスの検査もここで 2 回目)
    recheck_.Open(request_.scope, request_.detail, request_.allHunks, request_.selectedHunkIds, root_, *client_, Read());
    phase_ = RestoreProgress::Phase::Rechecking;
    return true;
}

void RestoreRunner::Cancel()
{
    const bool early = phase_ == RestoreProgress::Phase::Rechecking || phase_ == RestoreProgress::Phase::Fetching || phase_ == RestoreProgress::Phase::Saving
        || (phase_ == RestoreProgress::Phase::Applying && !applyStarted_);
    if (early && !recovery_) {
        StopBeforeWrite(RestoreOutcome::Cancelled, RestoreStopReason::UserCancelled, std::string());
    }
}

// 書く前に止める。ジャーナルは捨てる (prepared は何も書いていない)。利用者のファイルは 1 バイトも書いていない
void RestoreRunner::StopBeforeWrite(RestoreOutcome outcome, RestoreStopReason reason, const std::string& detail)
{
    ++generation_;
    recheck_.Close();
    if (!manifest_.restoreId.empty()) {
        Store().Discard(manifest_.restoreId);
    }
    result_.outcome = outcome;
    result_.reason = reason;
    result_.detail = detail;
    result_.restoreId = manifest_.restoreId;
    phase_ = RestoreProgress::Phase::Done;
    fetch_ = FetchState();
    saveJob_ = Job();
    RefreshRecoveries();
}

void RestoreRunner::Finish(RestoreOutcome outcome)
{
    ++generation_;
    result_.outcome = outcome;
    result_.restoreId = manifest_.restoreId;
    result_.completedOps = manifest_.completedOps;
    result_.unresolved = unresolved_;
    phase_ = RestoreProgress::Phase::Done;
    RefreshRecoveries();
    result_.recordPending = !records_.empty() || recordInFlight_;
}

void RestoreRunner::CloseWriteSection(const std::vector<BatchChange>& changes)
{
    if (!writeSection_) {
        return;
    }
    writeSection_ = false; // EndBatch の中で例外が出ても、もう 1 回呼ばない
    if (hooks_.endBatch) {
        hooks_.endBatch(changes);
    }
}

std::vector<BatchChange> RestoreRunner::ChangesFor(bool committed) const
{
    std::vector<BatchChange> changes;
    auto exists = [&](const std::string& rel) {
        std::error_code ec;
        return fs::exists(fs::path(JoinProjectPath(root_, rel)), ec);
    };
    if (committed) {
        for (const RestoreOp& op : ops_) {
            BatchChange change;
            change.path = JoinProjectPath(root_, op.kind == RestoreOp::Kind::Rename ? op.toPath : op.path);
            switch (op.kind) {
            case RestoreOp::Kind::Overwrite: change.kind = BatchChange::Kind::Modified; break;
            case RestoreOp::Kind::Create: change.kind = BatchChange::Kind::Added; break;
            case RestoreOp::Kind::Delete: change.kind = BatchChange::Kind::Deleted; break;
            case RestoreOp::Kind::Rename:
                change.kind = BatchChange::Kind::Renamed;
                change.oldPath = JoinProjectPath(root_, op.path);
                break;
            }
            changes.push_back(std::move(change));
        }
        return changes;
    }
    // 失敗・巻き戻し: 触ったパスの今のディスクの姿をそのまま流す (戻っていれば同じ内容の読み直しになるだけ)
    for (const JournalFile& file : manifest_.files) {
        BatchChange change;
        change.path = JoinProjectPath(root_, file.path);
        change.kind = exists(file.path) ? BatchChange::Kind::Modified : BatchChange::Kind::Deleted;
        changes.push_back(std::move(change));
    }
    return changes;
}

bool RestoreRunner::Gate(std::vector<RestoreBlock>& blocksOut, bool blobMissing)
{
    blocksOut.clear();
    if (!request_.blocks) {
        return true;
    }
    for (RestoreBlock& block : request_.blocks(blobMissing)) {
        // 進行中・回復待ちは Start / StartRecoveryRollback が見る。回復の巻き戻しはサービスに繋がっていなくてもよい (ジャーナルだけで戻せる)
        if (block.kind == RestoreBlockKind::JournalPending) {
            continue;
        }
        if (recovery_ && (block.kind == RestoreBlockKind::TrackUnavailable || block.kind == RestoreBlockKind::BlobMissing)) {
            continue;
        }
        blocksOut.push_back(std::move(block));
    }
    return blocksOut.empty();
}

bool RestoreRunner::SaveManifest()
{
    manifest_.updatedAt = NowMs();
    return Store().Save(manifest_);
}

// ---- Poll ----

void RestoreRunner::Poll()
{
    PumpRecords();
    if (!Busy()) {
        return;
    }
    // 例外でも BeginBatch の対になる EndBatch を通す。書き始めていれば巻き戻す
    auto onFailure = [this](const char* what) {
        MYE_LOG_ERROR("[restore] exception: %s", what);
        if (phase_ == RestoreProgress::Phase::RollingBack) {
            for (size_t i = rollbackPos_; i < manifest_.files.size(); ++i) {
                unresolved_.push_back(manifest_.files[i].path);
            }
            rollbackPos_ = manifest_.files.size();
            rollbackJob_ = Job();
            try {
                TickRollback();
            } catch (...) {
                // 後始末の中でもう一度例外が出ても、EndBatch は通して終わる (残ったファイルは回復の対象)
                CloseWriteSection({});
                Finish(RestoreOutcome::RecoveryRequired);
            }
        } else if (writeSection_) {
            BeginRollback(RestoreStopReason::Exception, what);
        } else {
            StopBeforeWrite(RestoreOutcome::Blocked, RestoreStopReason::Exception, what);
        }
    };
    try {
        switch (phase_) {
        case RestoreProgress::Phase::Rechecking: TickRecheck(); break;
        case RestoreProgress::Phase::Fetching: TickFetch(); break;
        case RestoreProgress::Phase::Saving: TickSave(); break;
        case RestoreProgress::Phase::Applying: TickApply(); break;
        case RestoreProgress::Phase::Verifying: TickVerify(); break;
        case RestoreProgress::Phase::RollingBack: TickRollback(); break;
        default: break;
        }
    } catch (const std::exception& e) {
        onFailure(e.what());
    } catch (...) {
        onFailure("unknown exception");
    }
}

// ---- 判定し直し ----

void RestoreRunner::TickRecheck()
{
    recheck_.Tick(Read());
    if (recheck_.GetPhase() == RestorePreview::Phase::Preparing) {
        return;
    }
    if (recheck_.GetPhase() == RestorePreview::Phase::Failed) {
        StopBeforeWrite(RestoreOutcome::Blocked, RestoreStopReason::PreviewFailed, recheck_.ErrorCode());
        return;
    }
    blobMissing_ = recheck_.BlobMissing();
    std::vector<RestoreBlock> blocks;
    if (!Gate(blocks, blobMissing_)) {
        result_.blocks = std::move(blocks);
        StopBeforeWrite(RestoreOutcome::Blocked, RestoreStopReason::Gate, std::string());
        return;
    }
    if (!recheck_.HasPlan()) {
        StopBeforeWrite(RestoreOutcome::Blocked, RestoreStopReason::PreviewFailed, "not connected");
        return;
    }
    const RestorePlan& plan = recheck_.Plan();
    if (plan.verdict != RestoreVerdict::Ready || !SamePlanOps(plan.ops, request_.previewOps)) {
        StopBeforeWrite(RestoreOutcome::Blocked, RestoreStopReason::PlanChanged, std::string());
        return;
    }
    ops_ = plan.ops;
    const RestoreRevision& revision = recheck_.Revision();
    const std::string pathKey = RestorePathKey(revision.path);
    const std::string oldKey = RestorePathKey(revision.oldPath);
    for (const std::string& rel : TouchedPaths(ops_)) {
        const std::string key = RestorePathKey(rel);
        const NowState* now = key == pathKey ? &recheck_.Now() : (key == oldKey ? &recheck_.NowOld() : nullptr);
        if (now == nullptr || (now->kind != NowKind::File && now->kind != NowKind::Missing)) {
            StopBeforeWrite(RestoreOutcome::Blocked, RestoreStopReason::PlanChanged, rel);
            return;
        }
        Touched touched;
        touched.rel = rel;
        touched.abs = AbsOf(rel);
        touched.existed = now->kind == NowKind::File;
        touched.hash = touched.existed ? now->hash : std::string();
        touched.size = touched.existed ? static_cast<uint64_t>((std::max<int64_t>)(now->size, 0)) : 0;
        touched.large = now->bytesOmitted;
        touched_.push_back(std::move(touched));
    }
    // 計画の操作の出どころが BeforeBlob のものは、Blob を取る
    fetched_.assign(ops_.size(), std::string());
    staged_.assign(ops_.size(), false);
    for (size_t i = 0; i < ops_.size(); ++i) {
        if (ops_[i].source == RestoreOp::Source::BeforeBlob) {
            fetchOps_.push_back(i);
        }
    }
    // ジャーナルを作る (prepared)。ここから先、止めたらジャーナルを捨てる
    manifest_ = JournalManifest();
    manifest_.restoreId = NewRestoreId();
    manifest_.state = JournalState::Prepared;
    manifest_.revisionId = revision.revisionId;
    manifest_.scope = request_.scope == RestoreScope::File ? "file" : "hunks";
    manifest_.hunkIds = request_.selectedHunkIds;
    manifest_.startedAt = NowMs();
    manifest_.ops = ToJournalOps(ops_);
    result_.restoreId = manifest_.restoreId;
    manifest_.updatedAt = manifest_.startedAt;
    if (!Store().Begin(manifest_)) {
        StopBeforeWrite(RestoreOutcome::Blocked, RestoreStopReason::JournalFailed, std::string());
        return;
    }
    phase_ = RestoreProgress::Phase::Fetching;
}

// ---- Before の内容を取る ----

void RestoreRunner::TickFetch()
{
    if (fetchPos_ >= fetchOps_.size()) {
        phase_ = RestoreProgress::Phase::Saving;
        return;
    }
    const size_t opIndex = fetchOps_[fetchPos_];
    const RestoreOp& op = ops_[opIndex];
    if (fetch_.hashing) {
        if (fetch_.hashJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            return;
        }
        const std::string hash = fetch_.hashJob.get();
        if (hash != op.hash) {
            StopBeforeWrite(RestoreOutcome::Blocked, RestoreStopReason::BlobMismatch, op.path);
            return;
        }
        ++fetchPos_;
        fetch_ = FetchState();
        return;
    }
    if (!hasResponse_) {
        if (!fetch_.inFlight) {
            fetch_.op = opIndex;
            fetch_.inFlight = true;
            const uint64_t generation = generation_;
            client_->Request(trackop::kBlobGet, { { "hash", op.hash }, { "offset", fetch_.offset }, { "length", kFetchChunkBytes } },
                             [this, generation](const json& response) {
                                 if (generation != generation_) {
                                     return; // 止めたあとに届いた古い応答
                                 }
                                 lastResponse_ = response;
                                 hasResponse_ = true;
                             });
        }
        return;
    }
    hasResponse_ = false;
    fetch_.inFlight = false;
    const json response = std::move(lastResponse_);
    if (!ResponseOk(response) || !response.contains("result") || !response["result"].is_object() || !response["result"].contains("data")
        || !response["result"]["data"].is_string()) {
        StopBeforeWrite(RestoreOutcome::Blocked, RestoreStopReason::FetchFailed, ErrorCode(response));
        return;
    }
    const json& result = response["result"];
    bool decoded = false;
    const std::string data = BlobHeadView::Base64Decode(result["data"].get<std::string>(), &decoded);
    const int64_t total = result.contains("size") && result["size"].is_number_integer() ? result["size"].get<int64_t>() : -1;
    if (!decoded || total < 0 || (op.size >= 0 && total != op.size)) {
        StopBeforeWrite(RestoreOutcome::Blocked, RestoreStopReason::BlobMismatch, op.path);
        return;
    }
    if (fetch_.offset == 0) {
        fetch_.large = static_cast<uint64_t>(total) > kRestoreReadMaxBytes;
        staged_[opIndex] = fetch_.large;
        if (fetch_.large) {
            // 大きい内容はメモリに全部載せず、ジャーナルの中へ塊ごとに積む
            if (!Write().MakeDirs(Store().DirOf(manifest_.restoreId) + L"\\new")) {
                StopBeforeWrite(RestoreOutcome::Blocked, RestoreStopReason::JournalFailed, std::string());
                return;
            }
        }
    }
    if (data.empty() && fetch_.offset < static_cast<uint64_t>(total)) {
        StopBeforeWrite(RestoreOutcome::Blocked, RestoreStopReason::FetchFailed, op.path); // 進まない
        return;
    }
    if (fetch_.large) {
        if (!Write().Append(Store().FileIn(manifest_.restoreId, StageName(opIndex)), fetch_.offset, data)) {
            StopBeforeWrite(RestoreOutcome::Blocked, RestoreStopReason::JournalFailed, std::string());
            return;
        }
    } else {
        fetched_[opIndex] += data;
    }
    fetch_.offset += data.size();
    if (fetch_.offset < static_cast<uint64_t>(total)) {
        return;
    }
    if (fetch_.large) {
        // 取りきった: ハッシュ (別のスレッド) が記録と合うことを確かめる
        fetch_.hashing = true;
        fetch_.hashJob = std::async(std::launch::async, [abs = Store().FileIn(manifest_.restoreId, StageName(opIndex))]() {
            std::string hex;
            return Sha256FileHex(abs, hex) ? hex : std::string();
        });
        return;
    }
    if (Sha256Hex(fetched_[opIndex]) != op.hash) {
        StopBeforeWrite(RestoreOutcome::Blocked, RestoreStopReason::BlobMismatch, op.path); // 途中で化けた内容は書かない
        return;
    }
    ++fetchPos_;
    fetch_ = FetchState();
}

// ---- 適用前の内容をジャーナルへ ----

RestoreRunner::HashStatus RestoreRunner::HashNow(const std::string& rel, Job& job, std::string& out)
{
    if (job.hash.valid()) {
        if (job.hash.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            return HashStatus::Pending;
        }
        out = job.hash.get();
        return out.empty() ? HashStatus::Error : HashStatus::Done;
    }
    const std::wstring abs = AbsOf(rel);
    const uint32_t attrs = Read().Attributes(abs);
    if (attrs == kRestoreAttrAbsent) {
        out.clear();
        return HashStatus::Done;
    }
    if ((attrs & (kRestoreAttrReparse | kRestoreAttrDirectory)) != 0) {
        return HashStatus::Error;
    }
    std::string probe;
    uint64_t total = 0;
    if (Read().Read(abs, 0, 0, probe, total) != RestoreFileIo::ReadStatus::Ok) {
        return HashStatus::Error;
    }
    if (total > kRestoreReadMaxBytes) {
        job.hash = std::async(std::launch::async, [abs]() {
            std::string hex;
            return Sha256FileHex(abs, hex) ? hex : std::string();
        });
        return HashStatus::Pending;
    }
    std::string bytes;
    if (!ReadAll(Read(), abs, bytes)) {
        return HashStatus::Error;
    }
    out = Sha256Hex(bytes);
    return out.empty() ? HashStatus::Error : HashStatus::Done;
}

void RestoreRunner::TickSave()
{
    if (savePos_ >= touched_.size()) {
        // 全部保存し終えた。manifest に軌跡を入れる (まだ prepared。applying にするのは適用の直前)
        std::vector<std::pair<std::string, std::string>> initial;
        for (size_t i = 0; i < touched_.size(); ++i) {
            initial.emplace_back(touched_[i].rel, touched_[i].hash);
        }
        manifest_.files = BuildJournalFiles(ops_, initial);
        for (size_t i = 0; i < manifest_.files.size(); ++i) {
            manifest_.files[i].preSize = static_cast<int64_t>(touched_[i].size);
            manifest_.files[i].preFile = touched_[i].existed ? PreName(i) : std::string();
        }
        if (!SaveManifest()) {
            StopBeforeWrite(RestoreOutcome::Blocked, RestoreStopReason::JournalFailed, std::string());
            return;
        }
        phase_ = RestoreProgress::Phase::Applying;
        applyStarted_ = false;
        return;
    }
    Touched& t = touched_[savePos_];
    const std::wstring preAbs = Store().FileIn(manifest_.restoreId, PreName(savePos_));
    if (!t.existed) {
        // 今そのパスに何も無いこと (プレビューのあとで作られていたら止める)
        if (Read().Attributes(t.abs) != kRestoreAttrAbsent) {
            StopBeforeWrite(RestoreOutcome::Blocked, RestoreStopReason::ContentChanged, t.rel);
            return;
        }
        ++savePos_;
        return;
    }
    if (!t.large) {
        std::string bytes;
        if (!ReadAll(Read(), t.abs, bytes) || Sha256Hex(bytes) != t.hash) {
            StopBeforeWrite(RestoreOutcome::Blocked, RestoreStopReason::ContentChanged, t.rel);
            return;
        }
        std::string back;
        if (!Write().Write(preAbs, bytes) || !ReadAll(Read(), preAbs, back) || Sha256Hex(back) != t.hash) {
            StopBeforeWrite(RestoreOutcome::Blocked, RestoreStopReason::JournalFailed, t.rel);
            return;
        }
        ++savePos_;
        return;
    }
    // 大きいファイル: 複写 → 複写のハッシュ、をそれぞれ別のスレッドで。フレームを止めない
    if (saveStage_ == 0) {
        saveJob_.done = std::async(std::launch::async, [w = &Write(), from = t.abs, to = preAbs]() { return w->Copy(from, to); });
        saveStage_ = 1;
        return;
    }
    if (saveStage_ == 1) {
        if (saveJob_.done.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            return;
        }
        if (!saveJob_.done.get()) {
            StopBeforeWrite(RestoreOutcome::Blocked, RestoreStopReason::JournalFailed, t.rel);
            return;
        }
        saveJob_.hash = std::async(std::launch::async, [preAbs]() {
            std::string hex;
            return Sha256FileHex(preAbs, hex) ? hex : std::string();
        });
        saveStage_ = 2;
        return;
    }
    if (saveJob_.hash.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return;
    }
    if (saveJob_.hash.get() != t.hash) {
        StopBeforeWrite(RestoreOutcome::Blocked, RestoreStopReason::ContentChanged, t.rel); // 複写の間に変わった
        return;
    }
    saveStage_ = 0;
    ++savePos_;
}

// ---- サービスへの記録 ----

void RestoreRunner::SendExpect()
{
    if (client_ == nullptr || !client_->CanRequest()) {
        return; // 知らせられない: サービスから見ると、この書き込みは「誰のものか分からない変更」になる
    }
    json paths = json::array();
    json hashes = json::array();
    for (const JournalFile& file : manifest_.files) {
        paths.push_back(file.path);
        hashes.push_back(file.IntendedHash().empty() ? json() : json(file.IntendedHash()));
    }
    client_->Request(trackop::kRestoreExpect, { { "restoreId", manifest_.restoreId }, { "paths", paths }, { "hashes", hashes } }, [](const json&) {});
}

void RestoreRunner::SendRecord(const JournalManifest& manifest, const char* state, const std::string& error)
{
    json paths = json::array();
    json pre = json::object();
    json post = json::object();
    for (const JournalFile& file : manifest.files) {
        paths.push_back(file.path);
        pre[file.path] = file.existed ? json{ { "hash", file.preHash }, { "size", file.preSize } } : json();
        post[file.path] = file.IntendedHash().empty() ? json() : json(file.IntendedHash());
    }
    json args = { { "restoreId", manifest.restoreId },
                  { "state", state },
                  { "targets", json::array({ { { "revisionId", manifest.revisionId }, { "scope", manifest.scope }, { "hunkIds", manifest.hunkIds }, { "paths", paths } } }) },
                  { "preHashes", pre } };
    if (std::string(state) == "committed") {
        args["postHashes"] = post;
    }
    if (!error.empty()) {
        args["error"] = error;
    }
    records_.push_back(std::move(args));
    PumpRecords();
}

// 1 件ずつ、順番に送る。つながっていない・時間切れのときは残して、つながったら送り直す
void RestoreRunner::PumpRecords()
{
    if (client_ == nullptr || recordInFlight_ || records_.empty() || !client_->CanRequest()) {
        return;
    }
    if (NowMs() < recordRetryAtMs_) {
        return;
    }
    recordInFlight_ = true;
    const json args = records_.front();
    client_->Request(trackop::kRestoreRecord, args, [this](const json& response) {
        recordInFlight_ = false;
        if (ResponseOk(response)) {
            if (!records_.empty()) {
                records_.erase(records_.begin());
            }
            return;
        }
        const std::string code = ErrorCode(response);
        if (code == "bad_args" || code == "forbidden" || code == "unknown_op") {
            if (!records_.empty()) {
                records_.erase(records_.begin()); // 送り直しても通らない
            }
            return;
        }
        recordRetryAtMs_ = NowMs() + kRecordRetryMs;
    });
}

void RestoreRunner::RequestHistory()
{
    if (client_ == nullptr || !client_->CanRequest() || historyPending_) {
        return;
    }
    historyPending_ = true;
    client_->Request(trackop::kRestoreList, { { "limit", 50 } }, [this](const json& response) {
        historyPending_ = false;
        if (ResponseOk(response) && response["result"].is_object() && response["result"].contains("items") && response["result"]["items"].is_array()) {
            history_.assign(response["result"]["items"].begin(), response["result"]["items"].end());
        }
    });
}

// ---- 適用 ----

void RestoreRunner::BeginApply()
{
    std::vector<RestoreBlock> blocks;
    if (!Gate(blocks, blobMissing_)) {
        result_.blocks = std::move(blocks);
        StopBeforeWrite(RestoreOutcome::Blocked, RestoreStopReason::Gate, std::string());
        return;
    }
    for (const Touched& t : touched_) {
        if (CheckRestorePath(root_, t.rel, Read()) != PathIssue::None) {
            StopBeforeWrite(RestoreOutcome::Blocked, RestoreStopReason::PathChanged, t.rel);
            return;
        }
        // 保存したあとで変わっていないか (小さいファイルはハッシュ、大きいファイルは大きさ)
        std::string probe;
        uint64_t total = 0;
        const bool present = Read().Attributes(t.abs) != kRestoreAttrAbsent;
        bool same = present == t.existed;
        if (same && present) {
            if (Read().Read(t.abs, 0, 0, probe, total) != RestoreFileIo::ReadStatus::Ok || total != t.size) {
                same = false;
            } else if (!t.large) {
                std::string bytes;
                same = ReadAll(Read(), t.abs, bytes) && Sha256Hex(bytes) == t.hash;
            }
        }
        if (!same) {
            StopBeforeWrite(RestoreOutcome::Blocked, RestoreStopReason::ContentChanged, t.rel);
            return;
        }
    }
    // 適用前の内容を全部保存し終えたので、ここで applying にする
    manifest_.state = JournalState::Applying;
    if (!SaveManifest()) {
        StopBeforeWrite(RestoreOutcome::Blocked, RestoreStopReason::JournalFailed, std::string());
        return;
    }
    SendExpect(); // 書く前に送る (サービスは、観測が先でも後でも同じ内容を restore の変更にする)
    SendRecord(manifest_, "applying", std::string());
    if (hooks_.beginBatch) {
        hooks_.beginBatch();
    }
    writeSection_ = true;
    applyStarted_ = true;
    opPos_ = 0;
}

// 操作の前に、書き先の今の内容が軌跡の「この操作の前」の状態であること (小さいファイルだけ。他者が割り込んでいたら書かない)
bool RestoreRunner::PathStateIs(const std::string& rel, const std::string& expectedHash, bool large)
{
    const std::wstring abs = AbsOf(rel);
    if (Read().Attributes(abs) == kRestoreAttrAbsent) {
        return expectedHash.empty();
    }
    if (expectedHash.empty()) {
        return false;
    }
    if (large) {
        return true; // 大きいファイルはここで全体を読まない (保存のときのハッシュと大きさで確かめ済み)
    }
    std::string bytes;
    return ReadAll(Read(), abs, bytes) && Sha256Hex(bytes) == expectedHash;
}

void RestoreRunner::TickApply()
{
    if (!applyStarted_) {
        BeginApply();
        return;
    }
    if (opPos_ >= ops_.size()) {
        phase_ = RestoreProgress::Phase::Verifying;
        verifyPos_ = 0;
        return;
    }
    const RestoreOp& op = ops_[opPos_];
    // 書く直前のパスの検査 (確認のあとでジャンクションに差し替えられていないか)
    if (CheckRestorePath(root_, op.path, Read()) != PathIssue::None || (op.kind == RestoreOp::Kind::Rename && CheckRestorePath(root_, op.toPath, Read()) != PathIssue::None)) {
        BeginRollback(RestoreStopReason::PathChanged, op.path);
        return;
    }
    const auto fileIndex = [&](const std::string& rel) -> const JournalFile* {
        const std::string key = RestorePathKey(rel);
        for (const JournalFile& f : manifest_.files) {
            if (RestorePathKey(f.path) == key) {
                return &f;
            }
        }
        return nullptr;
    };
    const JournalFile* source = fileIndex(op.path);
    const bool sourceLarge = [&] {
        for (const Touched& t : touched_) {
            if (RestorePathKey(t.rel) == RestorePathKey(op.path)) {
                return t.large;
            }
        }
        return false;
    }();
    if (source == nullptr || !PathStateIs(op.path, source->states[opPos_], sourceLarge)) {
        BeginRollback(RestoreStopReason::ContentChanged, op.path);
        return;
    }
    bool ok = false;
    const std::wstring abs = AbsOf(op.path);
    switch (op.kind) {
    case RestoreOp::Kind::Overwrite:
    case RestoreOp::Kind::Create:
        if (op.kind == RestoreOp::Kind::Create && !Write().MakeDirs(ParentOf(abs))) {
            break;
        }
        if (op.source == RestoreOp::Source::Bytes) {
            ok = Write().Write(abs, op.bytes);
        } else if (staged_[opPos_]) {
            ok = Write().Rename(Store().FileIn(manifest_.restoreId, StageName(opPos_)), abs, true); // 取り終えた大きい内容を所定の位置へ (同じボリュームの名前の付け替え)
        } else if (Sha256Hex(fetched_[opPos_]) == op.hash) {
            ok = Write().Write(abs, fetched_[opPos_]); // 書く直前にもう一度、記録のハッシュと合う内容だけを書く
        }
        break;
    case RestoreOp::Kind::Delete:
        ok = Write().Remove(abs);
        break;
    case RestoreOp::Kind::Rename: {
        const std::wstring to = AbsOf(op.toPath);
        ok = Write().MakeDirs(ParentOf(to)) && Write().Rename(abs, to, false);
        break;
    }
    }
    if (!ok) {
        BeginRollback(RestoreStopReason::WriteFailed, op.path);
        return;
    }
    ++opPos_;
    manifest_.completedOps = static_cast<int>(opPos_);
    if (!SaveManifest()) {
        BeginRollback(RestoreStopReason::JournalFailed, op.path);
    }
}

void RestoreRunner::TickVerify()
{
    if (verifyPos_ >= touched_.size()) {
        manifest_.state = JournalState::Committed;
        manifest_.completedOps = static_cast<int>(ops_.size());
        SaveManifest();
        const std::vector<BatchChange> changes = ChangesFor(true);
        CloseWriteSection(changes);
        SendRecord(manifest_, "committed", std::string());
        Finish(RestoreOutcome::Committed);
        return;
    }
    std::string hash;
    const HashStatus status = HashNow(touched_[verifyPos_].rel, verifyJob_, hash);
    if (status == HashStatus::Pending) {
        return;
    }
    const JournalFile& file = manifest_.files[verifyPos_];
    if (status == HashStatus::Error || hash != file.IntendedHash()) {
        BeginRollback(RestoreStopReason::VerifyFailed, file.path);
        return;
    }
    ++verifyPos_;
}

// ---- 巻き戻し ----

void RestoreRunner::BeginRollback(RestoreStopReason reason, const std::string& detail)
{
    result_.reason = reason;
    result_.detail = detail;
    manifest_.state = JournalState::RollingBack;
    manifest_.error = detail;
    SaveManifest(); // 失敗しても進める (巻き戻しの判定はジャーナルの軌跡だけで決まる)
    SendRecord(manifest_, "rolling_back", detail);
    phase_ = RestoreProgress::Phase::RollingBack;
    rollbackPos_ = 0;
    rollbackStage_ = 0;
    rollbackJob_ = Job();
    unresolved_.clear();
}

void RestoreRunner::TickRollback()
{
    if (rollbackPos_ >= manifest_.files.size()) {
        const bool clean = unresolved_.empty();
        if (clean) {
            manifest_.state = recovery_ ? JournalState::Recovered : JournalState::RolledBack;
            manifest_.resolution = recovery_ ? "restored_pre" : std::string();
        } else {
            manifest_.state = JournalState::RecoveryRequired;
        }
        SaveManifest();
        const std::vector<BatchChange> changes = ChangesFor(false);
        CloseWriteSection(changes);
        SendRecord(manifest_, clean ? "rolled_back" : "recovery_required", manifest_.error);
        recovery_ = false;
        Finish(clean ? RestoreOutcome::RolledBack : RestoreOutcome::RecoveryRequired);
        return;
    }
    const JournalFile& file = manifest_.files[rollbackPos_];
    const std::wstring abs = AbsOf(file.path);
    auto unresolved = [&]() {
        unresolved_.push_back(file.path);
        rollbackStage_ = 0;
        rollbackJob_ = Job();
        ++rollbackPos_;
    };
    if (rollbackStage_ == 0) {
        std::string current;
        const HashStatus status = HashNow(file.path, rollbackJob_, current);
        if (status == HashStatus::Pending) {
            return;
        }
        if (status == HashStatus::Error) {
            unresolved(); // 読めない
            return;
        }
        const int where = FindInTrajectory(file, current);
        if (where < 0) {
            unresolved(); // 他者が書いた内容: 触らない
            return;
        }
        if (where == 0) {
            ++rollbackPos_; // 適用前のまま
            return;
        }
        if (CheckRestorePath(root_, file.path, Read()) != PathIssue::None) {
            unresolved();
            return;
        }
        // 自分が書いた内容と一致した: 適用前へ戻す
        if (!file.existed) {
            if (!Write().Remove(abs)) {
                unresolved();
                return;
            }
            ++rollbackPos_;
            return;
        }
        const std::wstring preAbs = Store().FileIn(manifest_.restoreId, file.preFile);
        if (static_cast<uint64_t>((std::max<int64_t>)(file.preSize, 0)) > kRestoreReadMaxBytes) {
            rollbackJob_.done = std::async(std::launch::async, [w = &Write(), preAbs, abs]() {
                const std::wstring tmp = abs + L".mye-restore.tmp";
                return w->Copy(preAbs, tmp) && w->Rename(tmp, abs, true);
            });
            rollbackStage_ = 1;
            return;
        }
        std::string bytes;
        if (!ReadAll(Read(), preAbs, bytes) || Sha256Hex(bytes) != file.preHash || !Write().MakeDirs(ParentOf(abs)) || !Write().Write(abs, bytes)) {
            unresolved();
            return;
        }
        rollbackStage_ = 2; // 書いた: 読み直して確かめる
        return;
    }
    if (rollbackStage_ == 1) {
        if (rollbackJob_.done.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            return;
        }
        if (!rollbackJob_.done.get()) {
            unresolved();
            return;
        }
        rollbackStage_ = 2;
        return;
    }
    // stage 2: 戻したあとの内容が適用前と一致すること
    std::string back;
    const HashStatus status = HashNow(file.path, rollbackJob_, back);
    if (status == HashStatus::Pending) {
        return;
    }
    if (status == HashStatus::Error || back != file.preHash) {
        unresolved();
        return;
    }
    rollbackStage_ = 0;
    rollbackJob_ = Job();
    ++rollbackPos_;
}

} // namespace mye
