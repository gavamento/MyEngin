//====================================================================================
//                          ChangeFormat.cpp
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          AI Changes 窓の文面の整形 (ImGui を呼ばない純関数)
//====================================================================================
#include "Editor/ChangeTracking/ChangeFormat.h"

#include <cstdio>
#include <ctime>
#include <utility>

#include "Engine/Core/Localization/Localization.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {

namespace {

struct Entry {
    const char* key;
    StrId id;
};

// 表を引く。見つからなければ空 (呼び手が値そのものへ落とす)
template <size_t N>
std::string Lookup(const Entry (&table)[N], const std::string& key)
{
    for (const Entry& e : table) {
        if (key == e.key) {
            return Tr(e.id);
        }
    }
    return std::string();
}

const Entry kKinds[] = {
    { "added", StrId::AiChg_KindAdded },
    { "modified", StrId::AiChg_KindModified },
    { "deleted", StrId::AiChg_KindDeleted },
    { "renamed", StrId::AiChg_KindRenamed },
};
const Entry kAttributions[] = {
    { "verified", StrId::AiChg_AttrVerified },
    { "claimed", StrId::AiChg_AttrClaimed },
    { "inferred", StrId::AiChg_AttrInferred },
    { "unknown", StrId::AiChg_AttrUnknown },
};
const Entry kAttributionNotes[] = {
    { "verified", StrId::AiChg_AttrVerifiedNote },
    { "claimed", StrId::AiChg_AttrClaimedNote },
    { "inferred", StrId::AiChg_AttrInferredNote },
    { "unknown", StrId::AiChg_AttrUnknownNote },
};
const Entry kActors[] = {
    { "ai", StrId::AiChg_ActorAi },
    { "editor_user", StrId::AiChg_ActorEditor },
    { "restore", StrId::AiChg_ActorRestore },
    { "unknown", StrId::AiChg_ActorUnknown },
};
const Entry kCompleteness[] = {
    { "complete", StrId::AiChg_CompleteLabel },
    { "coalesced", StrId::AiChg_CoalescedLabel },
    { "gap", StrId::AiChg_GapLabel },
    { "no_before", StrId::AiChg_NoBeforeLabel },
};
const Entry kSessionStatuses[] = {
    { "running", StrId::AiChg_SessionRunning },
    { "finished", StrId::AiChg_SessionFinished },
    { "interrupted", StrId::AiChg_SessionInterrupted },
};
const Entry kDiffModes[] = {
    { "lines", StrId::AiChg_ModeLines },
    { "whole", StrId::AiChg_ModeWhole },
    { "binary", StrId::AiChg_ModeBinary },
    { "excluded", StrId::AiChg_ModeExcluded },
    { "unavailable", StrId::AiChg_ModeUnavailable },
};
const Entry kDiffModeNotes[] = {
    { "whole", StrId::AiChg_ModeWholeNote },
    { "binary", StrId::AiChg_ModeBinaryNote },
    { "excluded", StrId::AiChg_ModeExcludedNote },
    { "unavailable", StrId::AiChg_ModeUnavailableNote },
};
const Entry kEvidence[] = {
    { "observed", StrId::AiChg_EvObserved },
    { "op_window", StrId::AiChg_EvOpWindow },
    { "before_match", StrId::AiChg_EvBeforeMatch },
    { "declared_edit_match", StrId::AiChg_EvDeclaredEditMatch },
    { "report_hash_match", StrId::AiChg_EvReportHashMatch },
    { "report_only", StrId::AiChg_EvReportOnly },
    { "worktree", StrId::AiChg_EvWorktree },
    { "time_near", StrId::AiChg_EvTimeNear },
    { "editor_saved", StrId::AiChg_EvEditorSaved },
    { "restore_expected", StrId::AiChg_EvRestoreExpected },
    { "overlap", StrId::AiChg_EvOverlap },
    { "preexisting", StrId::AiChg_EvPreexisting },
};
const Entry kSources[] = {
    { "startup_gap", StrId::AiChg_SrcStartupGap },
    { "overflow_gap", StrId::AiChg_SrcOverflowGap },
    { "verify_gap", StrId::AiChg_SrcVerifyGap },
};

std::string OrKey(std::string text, const std::string& key)
{
    return text.empty() ? key : text;
}

} // namespace

ChangeSection ClassifyChange(const std::string& attribution, bool mixUncertain)
{
    if (mixUncertain) {
        return ChangeSection::Main;
    }
    return attribution == "unknown" || attribution == "inferred" ? ChangeSection::Uncertain : ChangeSection::Main;
}

ChangePartition PartitionChanges(const std::vector<TrackChangeRow>& rows, bool mixUncertain)
{
    ChangePartition out;
    for (size_t i = 0; i < rows.size(); ++i) {
        (ClassifyChange(rows[i].attribution, mixUncertain) == ChangeSection::Main ? out.main : out.uncertain).push_back(i);
    }
    return out;
}

std::string KindLabel(const std::string& changeType)
{
    return OrKey(Lookup(kKinds, changeType), changeType);
}

std::string AttributionLabel(const std::string& attribution)
{
    return OrKey(Lookup(kAttributions, attribution), attribution);
}

std::string AttributionNote(const std::string& attribution)
{
    return Lookup(kAttributionNotes, attribution);
}

std::string ActorLabel(const std::string& actor)
{
    return OrKey(Lookup(kActors, actor), actor);
}

std::string CompletenessLabel(const std::string& completeness)
{
    return OrKey(Lookup(kCompleteness, completeness), completeness);
}

std::string CompletenessNote(const std::string& completeness, const std::string& evidenceSource)
{
    if (completeness == "coalesced") {
        return Tr(StrId::AiChg_CoalescedNote);
    }
    if (completeness == "gap") {
        return Tr(evidenceSource == "verify_gap" ? StrId::AiChg_GapVerifyNote : StrId::AiChg_GapNote);
    }
    if (completeness == "no_before") {
        return Tr(StrId::AiChg_NoBeforeNote);
    }
    return std::string();
}

std::string SessionStatusLabel(const std::string& status)
{
    return OrKey(Lookup(kSessionStatuses, status), status);
}

std::string DiffModeLabel(const std::string& diffMode)
{
    return OrKey(Lookup(kDiffModes, diffMode), diffMode);
}

std::string DiffModeNote(const std::string& diffMode)
{
    return Lookup(kDiffModeNotes, diffMode);
}

const std::vector<std::string>& EvidenceKinds()
{
    static const std::vector<std::string> kinds = [] {
        std::vector<std::string> out;
        for (const Entry& e : kEvidence) {
            out.push_back(e.key);
        }
        return out;
    }();
    return kinds;
}

std::string EvidenceSource(const nlohmann::json& detail)
{
    if (detail.is_object() && detail.contains("source") && detail["source"].is_string()) {
        return detail["source"].get<std::string>();
    }
    return std::string();
}

std::string EvidenceText(const nlohmann::json& evidence)
{
    const std::string kind = evidence.is_object() && evidence.contains("kind") && evidence["kind"].is_string()
                                 ? evidence["kind"].get<std::string>()
                                 : std::string();
    std::string text = Lookup(kEvidence, kind);
    if (text.empty()) {
        text = std::string(Tr(StrId::AiChg_EvOther)) + ": " + kind;
    }
    // observed は、どうやって見つけたか (起動時の再走査・あふれの再走査・検証の走査) を添える
    if (kind == "observed" && evidence.contains("detail")) {
        const std::string how = Lookup(kSources, EvidenceSource(evidence["detail"]));
        if (!how.empty()) {
            text += " ";
            text += how;
        }
    }
    return text;
}

std::string FormatTimeMs(int64_t utcMs)
{
    if (utcMs <= 0) {
        return std::string();
    }
    const std::time_t seconds = static_cast<std::time_t>(utcMs / 1000);
    std::tm local{};
    if (localtime_s(&local, &seconds) != 0) {
        return std::string();
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                  local.tm_hour, local.tm_min, local.tm_sec);
    return buf;
}

std::string FormatBytes(int64_t bytes)
{
    if (bytes < 0) {
        return std::string();
    }
    char buf[32];
    if (bytes < 1024) {
        std::snprintf(buf, sizeof(buf), "%lld B", static_cast<long long>(bytes));
    } else if (bytes < 1024 * 1024) {
        std::snprintf(buf, sizeof(buf), "%.1f KiB", static_cast<double>(bytes) / 1024.0);
    } else if (bytes < 1024LL * 1024 * 1024) {
        std::snprintf(buf, sizeof(buf), "%.1f MiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else {
        std::snprintf(buf, sizeof(buf), "%.2f GiB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    }
    return buf;
}

ChangeRowText FormatChangeRow(const TrackChangeRow& row)
{
    ChangeRowText out;
    out.path = row.changeType == "renamed" && !row.oldPath.empty() ? row.oldPath + " -> " + row.path : row.path;
    out.kind = KindLabel(row.changeType);
    out.attribution = AttributionLabel(row.attribution);
    out.completeness = row.completeness.empty() || row.completeness == "complete" ? std::string() : CompletenessLabel(row.completeness);
    out.superseded = row.changeState == "superseded" ? std::string(Tr(StrId::AiChg_Superseded)) : std::string();
    out.time = FormatTimeMs(row.observedAt);
    out.size = row.afterSize >= 0 ? FormatBytes(row.afterSize) : std::string();
    return out;
}

SessionRowText FormatSessionRow(const TrackSessionRow& row)
{
    SessionRowText out;
    out.title = row.provider.empty() ? row.sessionId.substr(0, 8) : row.provider;
    out.status = SessionStatusLabel(row.status);
    out.time = FormatTimeMs(row.startedAt);
    out.count = std::to_string(row.changeCount) + " " + Tr(StrId::AiChg_SessionChanges);
    return out;
}

std::wstring ProjectFilePath(const std::wstring& projectRoot, const std::string& relPath)
{
    constexpr wchar_t kSep = 0x5C; // バックスラッシュ
    if (projectRoot.empty() || relPath.empty()) {
        return std::wstring();
    }
    std::wstring rel = Utf8ToWide(relPath);
    for (wchar_t& c : rel) {
        if (c == L'/') {
            c = kSep;
        }
    }
    // 絶対パス (ドライブ・UNC・先頭の区切り) と ".." の要素は、プロジェクトの外を指しうるので受けない
    if (rel[0] == kSep || rel.find(L':') != std::wstring::npos) {
        return std::wstring();
    }
    size_t start = 0;
    while (start <= rel.size()) {
        const size_t end = rel.find(kSep, start);
        const std::wstring part = rel.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        if (part == L"..") {
            return std::wstring();
        }
        if (end == std::wstring::npos) {
            break;
        }
        start = end + 1;
    }
    std::wstring root = projectRoot;
    while (!root.empty() && (root.back() == kSep || root.back() == L'/')) {
        root.pop_back();
    }
    return root + kSep + rel;
}

std::string StateBannerText(TrackState state, bool allowed, const std::string& dbReason)
{
    switch (state) {
    case TrackState::Disabled: return Tr(allowed ? StrId::AiChg_StateDisabled : StrId::AiChg_StateNonInteractive);
    case TrackState::NoProject: return Tr(StrId::AiChg_StateNoProject);
    case TrackState::Connecting: return Tr(StrId::AiChg_StateConnecting);
    case TrackState::Ready: return Tr(StrId::AiChg_StateReady);
    case TrackState::NoService: return Tr(StrId::AiChg_StateNoService);
    case TrackState::ProtoMismatch: return Tr(StrId::AiChg_StateProtoMismatch);
    case TrackState::WrongProject: return Tr(StrId::AiChg_StateWrongProject);
    case TrackState::DbDegraded: {
        std::string text = Tr(StrId::AiChg_StateDbDegraded);
        if (!dbReason.empty()) {
            text += ": " + dbReason;
        }
        return text;
    }
    case TrackState::Disconnected: return Tr(StrId::AiChg_StateDisconnected);
    }
    return std::string();
}

std::vector<std::string> StatusLines(const TrackStatusInfo& s)
{
    std::vector<std::string> lines;
    if (!s.valid) {
        return lines;
    }
    std::string baseline = std::string(Tr(StrId::AiChg_StatBaseline)) + ": ";
    if (s.Baselining() || s.scanDone < s.scanTotal) {
        baseline += std::to_string(s.scanDone) + " / " + std::to_string(s.scanTotal) + " " + Tr(StrId::AiChg_StatFiles);
    } else {
        baseline += Tr(StrId::AiChg_StatBaselineDone);
    }
    lines.push_back(std::move(baseline));
    lines.push_back(std::string(Tr(StrId::AiChg_StatTracked)) + ": " + std::to_string(s.trackedFiles));
    lines.push_back(std::string(Tr(StrId::AiChg_StatBlobs)) + ": " + FormatBytes(s.blobBytes) + " (" + std::to_string(s.blobCount) + " "
                    + Tr(StrId::AiChg_StatBlobCount) + ")");
    lines.push_back(std::string(Tr(StrId::AiChg_StatGaps)) + ": " + std::to_string(s.gapCount));
    if (s.watchFallback) {
        lines.push_back(Tr(StrId::AiChg_StatWatchFallback));
    }
    return lines;
}

} // namespace mye
