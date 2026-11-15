//====================================================================================
//                          RestoreRunFormat.cpp
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          復元の実行・結果・回復・履歴の文面 (ImGui を呼ばない純関数)
//====================================================================================
#include "Editor/ChangeTracking/RestoreRunFormat.h"

#include <algorithm>
#include <utility>

#include "Editor/ChangeTracking/ChangeFormat.h"
#include "Editor/ChangeTracking/RestoreFormat.h"
#include "Engine/Core/Localization/Localization.h"

namespace mye {

namespace {

// {name} を値に置き換える (Tr の結果を書式文字列として使わない)
std::string Fill(std::string text, const std::vector<std::pair<std::string, std::string>>& values)
{
    for (const auto& [name, value] : values) {
        const std::string token = "{" + name + "}";
        for (size_t pos = text.find(token); pos != std::string::npos; pos = text.find(token, pos + value.size())) {
            text.replace(pos, token.size(), value);
        }
    }
    return text;
}

std::string ShortHash(const std::string& hash)
{
    return hash.size() > 8 ? hash.substr(0, 8) : hash;
}

std::string HashOrNone(const std::string& hash)
{
    return hash.empty() ? std::string(Tr(StrId::AiChg_RcAbsent)) : ShortHash(hash);
}

std::string ReasonText(const RestoreRunResult& result)
{
    const std::string detail = result.detail;
    auto with = [&](StrId id) { return Fill(Tr(id), { { "detail", detail } }); };
    switch (result.reason) {
    case RestoreStopReason::None: return std::string();
    case RestoreStopReason::Gate: return Tr(StrId::AiChg_RrWhyGate);
    case RestoreStopReason::PlanChanged: return Tr(StrId::AiChg_RrWhyPlan);
    case RestoreStopReason::PreviewFailed: return with(StrId::AiChg_RrWhyPreview);
    case RestoreStopReason::FetchFailed: return with(StrId::AiChg_RrWhyFetch);
    case RestoreStopReason::BlobMismatch: return with(StrId::AiChg_RrWhyBlob);
    case RestoreStopReason::JournalFailed: return Tr(StrId::AiChg_RrWhyJournal);
    case RestoreStopReason::ContentChanged: return with(StrId::AiChg_RrWhyContent);
    case RestoreStopReason::PathChanged: return with(StrId::AiChg_RrWhyPath);
    case RestoreStopReason::WriteFailed: return with(StrId::AiChg_RrWhyWrite);
    case RestoreStopReason::VerifyFailed: return with(StrId::AiChg_RrWhyVerify);
    case RestoreStopReason::Exception: return with(StrId::AiChg_RrWhyException);
    case RestoreStopReason::UserCancelled: return std::string();
    case RestoreStopReason::NotRecoverable: return Tr(StrId::AiChg_RrWhyNotRecoverable);
    }
    return std::string();
}

} // namespace

RestoreRunView BuildRestoreRunView(const RestoreRunner& runner)
{
    RestoreRunView view;
    if (runner.HasResult()) {
        const RestoreRunResult& result = runner.Result();
        view.mode = RestoreRunView::Mode::Result;
        view.outcome = result.outcome;
        switch (result.outcome) {
        case RestoreOutcome::Committed:
            view.headline = Fill(Tr(StrId::AiChg_RrDoneCommitted), { { "ops", std::to_string(result.completedOps) } });
            break;
        case RestoreOutcome::Blocked: view.headline = Tr(StrId::AiChg_RrDoneBlocked); break;
        case RestoreOutcome::Cancelled: view.headline = Tr(StrId::AiChg_RrDoneCancelled); break;
        case RestoreOutcome::RolledBack: view.headline = Tr(StrId::AiChg_RrDoneRolledBack); break;
        case RestoreOutcome::RecoveryRequired:
            view.headline = Tr(StrId::AiChg_RrDoneRecovery);
            view.needsRecovery = true;
            break;
        case RestoreOutcome::None: break;
        }
        if (const std::string reason = ReasonText(result); !reason.empty()) {
            view.reasons.push_back(reason);
        }
        for (const RestoreBlock& block : result.blocks) {
            view.reasons.push_back(RestoreBlockText(block));
        }
        for (const std::string& path : result.unresolved) {
            view.unresolved.push_back(path);
        }
        if (!result.restoreId.empty() && result.outcome != RestoreOutcome::Blocked && result.outcome != RestoreOutcome::Cancelled) {
            view.journal = Fill(Tr(StrId::AiChg_RrJournalId), { { "id", result.restoreId } });
        }
        if (runner.PendingRecordCount() > 0) {
            view.recordNote = Tr(StrId::AiChg_RrRecordPending);
        }
        return view;
    }
    if (!runner.Busy()) {
        return view;
    }
    const RestoreProgress progress = runner.Progress();
    view.mode = RestoreRunView::Mode::Progress;
    view.canCancel = progress.canCancel;
    view.cancelHint = Tr(progress.canCancel ? StrId::AiChg_RrCancelHint : StrId::AiChg_RrNoCancel);
    StrId text = StrId::AiChg_RrPhaseRecheck;
    switch (progress.phase) {
    case RestoreProgress::Phase::Fetching: text = StrId::AiChg_RrPhaseFetch; break;
    case RestoreProgress::Phase::Saving: text = StrId::AiChg_RrPhaseSave; break;
    case RestoreProgress::Phase::Applying: text = StrId::AiChg_RrPhaseApply; break;
    case RestoreProgress::Phase::Verifying: text = StrId::AiChg_RrPhaseVerify; break;
    case RestoreProgress::Phase::RollingBack: text = StrId::AiChg_RrPhaseRollback; break;
    default: break;
    }
    view.phase = Fill(Tr(text), { { "step", std::to_string(progress.step) }, { "total", std::to_string(progress.total) } });
    view.fraction = progress.total > 0 ? (std::min)(1.0f, static_cast<float>(progress.step) / static_cast<float>(progress.total)) : 0.0f;
    return view;
}

std::vector<RecoveryEntryView> BuildRecoveryViews(const RestoreRunner& runner, const std::vector<std::pair<std::string, std::vector<RecoveryFileView>>>& inspected)
{
    std::vector<RecoveryEntryView> views;
    for (const RestoreJournalStore::Entry& entry : runner.Recoveries()) {
        RecoveryEntryView view;
        view.id = entry.id;
        view.broken = entry.broken;
        if (entry.broken) {
            view.header = entry.id;
            view.brokenText = Fill(Tr(StrId::AiChg_RcBroken), { { "id", entry.id }, { "reason", entry.brokenReason } });
            views.push_back(std::move(view));
            continue;
        }
        view.header = Fill(Tr(StrId::AiChg_RcEntry), { { "id", entry.id }, { "state", RestoreStateText(JournalStateName(entry.manifest.state)) } });
        const auto found = std::find_if(inspected.begin(), inspected.end(), [&](const auto& p) { return p.first == entry.id; });
        if (found != inspected.end()) {
            for (const RecoveryFileView& file : found->second) {
                RecoveryFileText text;
                text.path = file.path;
                text.before = HashOrNone(file.preHash);
                text.intended = HashOrNone(file.intendedHash);
                text.current = file.current;
                text.now = file.current == RecoveryFileView::Current::Unreadable ? std::string("?") : HashOrNone(file.currentHash);
                switch (file.current) {
                case RecoveryFileView::Current::Pre: text.verdict = Tr(StrId::AiChg_RcNowPre); break;
                case RecoveryFileView::Current::Intended:
                    text.verdict = Tr(StrId::AiChg_RcNowIntended);
                    view.canRestore = true;
                    break;
                case RecoveryFileView::Current::Other: text.verdict = Tr(StrId::AiChg_RcNowOther); break;
                case RecoveryFileView::Current::Unreadable: text.verdict = Tr(StrId::AiChg_RcNowUnreadable); break;
                }
                view.files.push_back(std::move(text));
            }
        }
        views.push_back(std::move(view));
    }
    return views;
}

std::string RestoreStateText(const std::string& state)
{
    if (state == "prepared") {
        return Tr(StrId::AiChg_RhStatePrepared);
    }
    if (state == "applying") {
        return Tr(StrId::AiChg_RhStateApplying);
    }
    if (state == "committed") {
        return Tr(StrId::AiChg_RhStateCommitted);
    }
    if (state == "rolling_back") {
        return Tr(StrId::AiChg_RhStateRollingBack);
    }
    if (state == "rolled_back") {
        return Tr(StrId::AiChg_RhStateRolledBack);
    }
    if (state == "recovery_required") {
        return Tr(StrId::AiChg_RhStateRecoveryRequired);
    }
    if (state == "recovered") {
        return Tr(StrId::AiChg_RhStateRecovered);
    }
    return state;
}

std::vector<RestoreHistoryRow> BuildHistoryRows(const std::vector<nlohmann::json>& items)
{
    std::vector<RestoreHistoryRow> rows;
    for (const nlohmann::json& item : items) {
        if (!item.is_object()) {
            continue;
        }
        RestoreHistoryRow row;
        row.time = item.contains("startedAt") && item["startedAt"].is_number_integer() ? FormatTimeMs(item["startedAt"].get<int64_t>()) : std::string();
        row.state = RestoreStateText(item.value("state", std::string()));
        std::string target;
        if (item.contains("targets") && item["targets"].is_array()) {
            for (const nlohmann::json& t : item["targets"]) {
                if (!t.is_object() || !t.contains("paths") || !t["paths"].is_array()) {
                    continue;
                }
                for (const nlohmann::json& p : t["paths"]) {
                    if (p.is_string()) {
                        target += (target.empty() ? "" : ", ") + p.get<std::string>();
                    }
                }
            }
        }
        row.target = target;
        row.error = item.contains("error") && item["error"].is_string() ? item["error"].get<std::string>() : std::string();
        rows.push_back(std::move(row));
    }
    return rows;
}

} // namespace mye
