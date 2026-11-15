//====================================================================================
//                          RestoreFormat.cpp
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          復元のプレビューの文面の整形 (ImGui を呼ばない純関数)
//====================================================================================
#include "Editor/ChangeTracking/RestoreFormat.h"

#include <utility>

#include "Editor/ChangeTracking/ChangeFormat.h"
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

} // namespace

std::string PathIssueText(PathIssue issue)
{
    switch (issue) {
    case PathIssue::None: return std::string();
    case PathIssue::Empty: return Tr(StrId::AiChg_RsPathEmpty);
    case PathIssue::Absolute: return Tr(StrId::AiChg_RsPathAbsolute);
    case PathIssue::Traversal: return Tr(StrId::AiChg_RsPathTraversal);
    case PathIssue::Separator: return Tr(StrId::AiChg_RsPathSeparator);
    case PathIssue::InvalidChar: return Tr(StrId::AiChg_RsPathInvalidChar);
    case PathIssue::Stream: return Tr(StrId::AiChg_RsPathStream);
    case PathIssue::ReservedDevice: return Tr(StrId::AiChg_RsPathReserved);
    case PathIssue::TrailingDotOrSpace: return Tr(StrId::AiChg_RsPathTrailing);
    case PathIssue::ProtectedDirectory: return Tr(StrId::AiChg_RsPathProtected);
    case PathIssue::OutsideRoot: return Tr(StrId::AiChg_RsPathOutside);
    case PathIssue::Link: return Tr(StrId::AiChg_RsPathLink);
    case PathIssue::ParentNotDirectory: return Tr(StrId::AiChg_RsPathParentFile);
    }
    return std::string();
}

std::string RestoreIssueText(RestoreIssue issue)
{
    switch (issue) {
    case RestoreIssue::None: return std::string();
    case RestoreIssue::PathRejected: return Tr(StrId::AiChg_RsIssuePath);
    case RestoreIssue::ActorNotRestorable: return Tr(StrId::AiChg_RsIssueActor);
    case RestoreIssue::NoBefore: return Tr(StrId::AiChg_RsIssueNoBefore);
    case RestoreIssue::BeforeUnavailable: return Tr(StrId::AiChg_RsIssueBeforeGone);
    case RestoreIssue::AfterUnknown: return Tr(StrId::AiChg_RsIssueAfterUnknown);
    case RestoreIssue::HunkNeedsModified: return Tr(StrId::AiChg_RsIssueHunkModified);
    case RestoreIssue::HunkNeedsLines: return Tr(StrId::AiChg_RsIssueHunkLines);
    case RestoreIssue::HunkOversize: return Tr(StrId::AiChg_RsIssueHunkOversize);
    case RestoreIssue::HunkNoneSelected: return Tr(StrId::AiChg_RsIssueHunkNone);
    case RestoreIssue::NowUnreadable: return Tr(StrId::AiChg_RsIssueUnreadable);
    case RestoreIssue::NowTooLarge: return Tr(StrId::AiChg_RsIssueTooLarge);
    case RestoreIssue::NowNotAFile: return Tr(StrId::AiChg_RsIssueNotAFile);
    case RestoreIssue::FileChanged: return Tr(StrId::AiChg_RsIssueChanged);
    case RestoreIssue::PathOccupied: return Tr(StrId::AiChg_RsIssueOccupied);
    case RestoreIssue::OldPathOccupied: return Tr(StrId::AiChg_RsIssueOldOccupied);
    case RestoreIssue::HunkAmbiguous: return Tr(StrId::AiChg_RsIssueHunkAmbiguous);
    case RestoreIssue::HunkNotFound: return Tr(StrId::AiChg_RsIssueHunkNotFound);
    case RestoreIssue::HunkOverlap: return Tr(StrId::AiChg_RsIssueHunkOverlap);
    }
    return std::string();
}

std::string RestoreVerdictText(RestoreVerdict verdict)
{
    switch (verdict) {
    case RestoreVerdict::Ready: return Tr(StrId::AiChg_RsVerdictReady);
    case RestoreVerdict::AlreadyRestored: return Tr(StrId::AiChg_RsVerdictRestored);
    case RestoreVerdict::Conflict: return Tr(StrId::AiChg_RsVerdictConflict);
    case RestoreVerdict::NotRestorable: return Tr(StrId::AiChg_RsVerdictNotRestorable);
    }
    return std::string();
}

std::string RestoreBlockText(const RestoreBlock& block)
{
    std::string reason;
    switch (block.kind) {
    case RestoreBlockKind::Gate: reason = Tr(GateBlockerText(block.gate)); break;
    case RestoreBlockKind::TrackUnavailable: reason = Tr(StrId::AiChg_RsBlockTrack); break;
    case RestoreBlockKind::JournalPending: reason = Tr(StrId::AiChg_RsBlockJournal); break;
    case RestoreBlockKind::BlobMissing: reason = Tr(StrId::AiChg_RsBlockBlob); break;
    }
    if (!block.path.empty()) {
        reason += " (" + block.path + ")";
    }
    return reason;
}

std::string RestoreOpText(const RestoreOp& op, bool hunkOnly)
{
    switch (op.kind) {
    case RestoreOp::Kind::Overwrite:
        return Fill(Tr(hunkOnly ? StrId::AiChg_RsOpOverwriteHunks : StrId::AiChg_RsOpOverwrite),
                    { { "path", op.path }, { "size", op.size >= 0 ? FormatBytes(op.size) : std::string("?") } });
    case RestoreOp::Kind::Delete: return Fill(Tr(StrId::AiChg_RsOpDelete), { { "path", op.path } });
    case RestoreOp::Kind::Create: return Fill(Tr(StrId::AiChg_RsOpCreate), { { "path", op.path }, { "size", op.size >= 0 ? FormatBytes(op.size) : std::string("?") } });
    case RestoreOp::Kind::Rename: return Fill(Tr(StrId::AiChg_RsOpRename), { { "from", op.path }, { "to", op.toPath } });
    }
    return std::string();
}

std::string RestoreHunkOutcomeText(const RestoreHunkOutcome& outcome)
{
    const std::string id = std::to_string(outcome.hunkId);
    const int64_t lines = outcome.shiftLines < 0 ? -outcome.shiftLines : outcome.shiftLines;
    switch (outcome.find) {
    case HunkFind::Match:
        if (outcome.shiftLines == 0) {
            return Fill(Tr(StrId::AiChg_RsHunkMatch), { { "id", id } });
        }
        return Fill(Tr(outcome.shiftLines > 0 ? StrId::AiChg_RsHunkShiftedDown : StrId::AiChg_RsHunkShiftedUp), { { "id", id }, { "lines", std::to_string(lines) } });
    case HunkFind::Restored: return Fill(Tr(StrId::AiChg_RsHunkRestored), { { "id", id } });
    case HunkFind::Ambiguous: return Fill(Tr(StrId::AiChg_RsHunkAmbiguous), { { "id", id } });
    case HunkFind::NotFound: return Fill(Tr(StrId::AiChg_RsHunkNotFound), { { "id", id } });
    }
    return std::string();
}

std::vector<std::string> RestoreNoteLines(const RestoreNotes& notes)
{
    std::vector<std::string> lines;
    if (notes.deletesFile) {
        lines.push_back(Tr(StrId::AiChg_RsNoteDelete));
    }
    if (notes.referencesUnchecked) {
        lines.push_back(Tr(StrId::AiChg_RsNoteReferences));
    }
    if (notes.shifted) {
        lines.push_back(Tr(StrId::AiChg_RsNoteShifted));
    }
    for (const std::string& path : notes.stagedPaths) {
        lines.push_back(Fill(Tr(StrId::AiChg_RsNoteStaged), { { "path", path } }));
    }
    if (notes.stagedUnknown) {
        lines.push_back(Tr(StrId::AiChg_RsNoteStagedUnknown));
    }
    if (notes.metaGuidUnchecked) {
        lines.push_back(Tr(StrId::AiChg_RsNoteMeta));
    }
    if (notes.stage == ApplyStage::B) {
        lines.push_back(Tr(StrId::AiChg_RsNoteStageB));
    } else if (notes.stage == ApplyStage::C) {
        lines.push_back(Tr(StrId::AiChg_RsNoteStageC));
    }
    return lines;
}

std::string RestoreHashLine(const RestoreRevision& revision, const NowState& now)
{
    const std::string none = Tr(StrId::AiChg_RsNone);
    auto show = [&](const std::string& hash, bool present) { return present ? ShortHash(hash) : none; };
    return Fill(Tr(StrId::AiChg_RsHashLine),
                { { "now", show(now.hash, now.kind == NowKind::File) },
                  { "before", show(revision.before.hash, revision.before.state != RestoreBlob::NoSnapshot && !revision.before.hash.empty()) },
                  { "after", show(revision.after.hash, revision.after.state != RestoreBlob::NoSnapshot && !revision.after.hash.empty()) } });
}

std::string ReviewGroupText(const nlohmann::json& group)
{
    if (!group.is_object()) {
        return std::string();
    }
    const std::string state = group.contains("state") && group["state"].is_string() ? group["state"].get<std::string>() : std::string();
    std::string text = ReviewStateLabel(state);
    const std::string counts = ReviewCountsText(ChangeModel::ParseReviewCounts(group.contains("counts") ? group["counts"] : nlohmann::json()));
    if (!counts.empty()) {
        text += " (" + counts + ")";
    }
    if (group.contains("truncated") && group["truncated"].is_boolean() && group["truncated"].get<bool>()) {
        text += " " + std::string(Tr(StrId::AiChg_RvTruncated));
    }
    return text;
}

std::string BlobHeadBinaryText(int64_t totalBytes)
{
    return Fill(Tr(StrId::AiChg_HeadBinary), { { "bytes", FormatBytes(totalBytes) } });
}

std::string BlobHeadTruncatedText(int64_t shownBytes, int64_t totalBytes)
{
    return Fill(Tr(StrId::AiChg_HeadTruncated), { { "bytes", FormatBytes(shownBytes) }, { "total", FormatBytes(totalBytes) } });
}

RestoreScreen BuildRestoreScreen(const RestorePreview& preview, const std::vector<RestoreBlock>& blocks, const RestoreNotes& notes)
{
    RestoreScreen screen;
    screen.open = preview.IsOpen();
    screen.phase = preview.GetPhase();
    if (!screen.open) {
        return screen;
    }
    const RestoreRevision& rev = preview.Revision();
    screen.target = rev.changeType == "renamed" && !rev.oldPath.empty() ? rev.oldPath + " -> " + rev.path : rev.path;
    screen.scope = Tr(preview.Scope() == RestoreScope::File ? StrId::AiChg_RsScopeFile : StrId::AiChg_RsScopeHunks);
    for (const RestoreBlock& block : blocks) {
        screen.blocks.push_back(RestoreBlockText(block));
    }
    if (preview.GetPhase() == RestorePreview::Phase::Failed) {
        screen.failure = preview.ErrorCode().empty() ? preview.ErrorDetail() : preview.ErrorCode() + (preview.ErrorDetail().empty() ? "" : ": " + preview.ErrorDetail());
    }
    screen.applyNote = Tr(StrId::AiChg_RsNoWrite);
    if (preview.GetPhase() != RestorePreview::Phase::Ready || !preview.HasPlan()) {
        return screen;
    }
    const RestorePlan& plan = preview.Plan();
    screen.hasPlan = true;
    screen.verdict = plan.verdict;
    screen.verdictText = RestoreVerdictText(plan.verdict);
    if (plan.issue != RestoreIssue::None) {
        std::string issue = RestoreIssueText(plan.issue);
        if (plan.issue == RestoreIssue::PathRejected) {
            issue += " " + PathIssueText(plan.pathIssue);
        }
        screen.issues.push_back(issue);
    }
    if (rev.changeState == "superseded") {
        screen.superseded = Tr(StrId::AiChg_RsSuperseded);
    }
    for (const RestoreHunkOutcome& outcome : plan.hunks) {
        screen.hunkLines.push_back(RestoreHunkOutcomeText(outcome));
    }
    if (plan.verdict == RestoreVerdict::Conflict) {
        screen.hashLine = RestoreHashLine(rev, preview.Now());
    }
    for (const RestoreOp& op : plan.ops) {
        screen.ops.push_back(RestoreOpText(op, plan.scope == RestoreScope::Hunks));
    }
    screen.notes = RestoreNoteLines(notes);
    // 押せるのは、照合に通った (Ready) 計画があり、止める理由が 1 つも無いときだけ
    screen.canApply = plan.verdict == RestoreVerdict::Ready && !plan.ops.empty() && blocks.empty();
    screen.applyNote = Tr(screen.canApply ? StrId::AiChg_RsWillApply : StrId::AiChg_RsNoWrite);
    return screen;
}

} // namespace mye
