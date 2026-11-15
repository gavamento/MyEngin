//====================================================================================
//                          RestorePlan.cpp
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          ファイル・Hunk の復元の計画 (競合の検証。書かない純関数)
//====================================================================================
#include "Editor/ChangeTracking/RestorePlan.h"

#include <algorithm>

#include "Engine/Platform/Sha256.h"

namespace mye {

namespace {

std::string StringField(const nlohmann::json& object, const char* key)
{
    return object.is_object() && object.contains(key) && object[key].is_string() ? object[key].get<std::string>() : std::string();
}

RestoreSide ParseSide(const nlohmann::json& value)
{
    RestoreSide side;
    if (!value.is_object()) {
        return side; // NoSnapshot
    }
    const std::string state = StringField(value, "blobState");
    if (state == "present") {
        side.state = RestoreBlob::Present;
    } else if (state == "missing") {
        side.state = RestoreBlob::Missing;
    } else if (state == "excluded") {
        side.state = RestoreBlob::Excluded;
    } else if (state == "evicted") {
        side.state = RestoreBlob::Evicted;
    } else {
        side.state = RestoreBlob::Unknown;
    }
    side.hash = StringField(value, "hash");
    if (value.contains("size") && value["size"].is_number_integer()) {
        side.size = value["size"].get<int64_t>();
    }
    return side;
}

RestorePlan NotRestorable(RestorePlan plan, RestoreIssue issue)
{
    plan.verdict = RestoreVerdict::NotRestorable;
    plan.issue = issue;
    plan.ops.clear();
    return plan;
}

RestorePlan Conflict(RestorePlan plan, RestoreIssue issue)
{
    plan.verdict = RestoreVerdict::Conflict;
    plan.issue = issue;
    plan.ops.clear();
    return plan;
}

RestorePlan AlreadyRestored(RestorePlan plan)
{
    plan.verdict = RestoreVerdict::AlreadyRestored;
    plan.issue = RestoreIssue::None;
    plan.ops.clear();
    return plan;
}

RestorePlan Ready(RestorePlan plan, std::vector<RestoreOp> ops)
{
    plan.verdict = RestoreVerdict::Ready;
    plan.issue = RestoreIssue::None;
    plan.ops = std::move(ops);
    return plan;
}

// 今のパスを読めているか。読めていない・ファイルでないときの理由 (None = 判定に使える)
RestoreIssue NowProblem(const NowState& now)
{
    switch (now.kind) {
    case NowKind::Unreadable: return RestoreIssue::NowUnreadable;
    case NowKind::TooLarge: return RestoreIssue::NowTooLarge;
    case NowKind::Directory:
    case NowKind::Link: return RestoreIssue::NowNotAFile;
    case NowKind::Missing:
    case NowKind::File: break;
    }
    return RestoreIssue::None;
}

// 内容の「ハッシュ」の比較。無い面は空 (absent)。今のパスに何も無いことも absent
struct Content {
    bool absent = true;
    std::string hash;
    bool Same(const Content& other) const { return absent == other.absent && (absent || hash == other.hash); }
};

Content NowContent(const NowState& now)
{
    Content content;
    if (now.kind == NowKind::File) {
        content.absent = false;
        content.hash = now.hash;
    }
    return content;
}

Content RecordedContent(const RestoreSide& side)
{
    Content content;
    if (side.state != RestoreBlob::NoSnapshot) {
        content.absent = false;
        content.hash = side.hash;
    }
    return content;
}

RestoreOp BeforeBlobOp(RestoreOp::Kind kind, const std::string& path, const RestoreSide& before)
{
    RestoreOp op;
    op.kind = kind;
    op.path = path;
    op.source = RestoreOp::Source::BeforeBlob;
    op.hash = before.hash;
    op.size = before.size;
    return op;
}

RestorePlan PlanFile(const RestoreInput& in, RestorePlan plan)
{
    const RestoreRevision& rev = in.revision;
    const Content after = RecordedContent(rev.after);
    const Content before = RecordedContent(rev.before);

    if (rev.changeType == "renamed") {
        // 新しい名前の内容が After と一致し、かつ古い名前に何も無いときだけ戻す
        const Content nowNew = NowContent(in.now);
        const Content nowOld = NowContent(in.nowOld);
        const bool newMatches = !nowNew.absent && !after.absent && nowNew.hash == after.hash;
        if (newMatches && nowOld.absent) {
            std::vector<RestoreOp> ops;
            RestoreOp rename;
            rename.kind = RestoreOp::Kind::Rename;
            rename.path = rev.path;
            rename.toPath = rev.oldPath;
            ops.push_back(rename);
            if (after.hash != before.hash) {
                ops.push_back(BeforeBlobOp(RestoreOp::Kind::Overwrite, rev.oldPath, rev.before)); // 名前を戻したうえで、内容も戻す
            }
            return Ready(std::move(plan), std::move(ops));
        }
        if (nowNew.absent && !nowOld.absent && !before.absent && nowOld.hash == before.hash) {
            return AlreadyRestored(std::move(plan));
        }
        return Conflict(std::move(plan), newMatches ? RestoreIssue::OldPathOccupied : RestoreIssue::FileChanged);
    }

    const Content now = NowContent(in.now);
    if (!after.absent && !before.absent && after.hash == before.hash && now.Same(after)) {
        return AlreadyRestored(std::move(plan)); // After と Before が同じ内容 (書くものが無い)
    }
    if (now.Same(after)) {
        if (before.absent) {
            RestoreOp op;
            op.kind = RestoreOp::Kind::Delete; // 追加された変更の取り消し
            op.path = rev.path;
            return Ready(std::move(plan), { op });
        }
        return Ready(std::move(plan), { BeforeBlobOp(after.absent ? RestoreOp::Kind::Create : RestoreOp::Kind::Overwrite, rev.path, rev.before) });
    }
    if (now.Same(before)) {
        return AlreadyRestored(std::move(plan));
    }
    return Conflict(std::move(plan), after.absent && !now.absent ? RestoreIssue::PathOccupied : RestoreIssue::FileChanged);
}

std::vector<HunkSpec> ToSpecs(const std::vector<TrackHunk>& hunks)
{
    std::vector<HunkSpec> specs;
    specs.reserve(hunks.size());
    for (const TrackHunk& h : hunks) {
        HunkSpec spec;
        spec.oldStart = h.oldStart;
        spec.oldCount = h.oldCount;
        spec.newStart = h.newStart;
        spec.newCount = h.newCount;
        spec.beforeText = h.beforeText;
        spec.afterText = h.afterText;
        spec.contextPre = h.contextPre;
        spec.contextPost = h.contextPost;
        specs.push_back(std::move(spec));
    }
    return specs;
}

RestorePlan PlanHunks(const RestoreInput& in, RestorePlan plan)
{
    if (in.now.kind == NowKind::Missing) {
        return Conflict(std::move(plan), RestoreIssue::FileChanged); // 戻す先のファイルが無い
    }
    const HunkApplyResult applied = UnapplyHunks(in.now.bytes, ToSpecs(in.hunks));
    for (size_t i = 0; i < in.hunks.size() && i < applied.finds.size(); ++i) {
        RestoreHunkOutcome outcome;
        outcome.hunkId = in.hunks[i].hunkId;
        outcome.ordinal = in.hunks[i].ordinal;
        outcome.find = applied.finds[i].find;
        outcome.shiftLines = applied.finds[i].shiftLines;
        outcome.bodyLine = applied.finds[i].bodyLine;
        plan.hunks.push_back(outcome);
    }
    if (!applied.Ok()) {
        if (applied.failure == HunkApplyFailure::TooManyLines) {
            return NotRestorable(std::move(plan), RestoreIssue::NowTooLarge);
        }
        if (applied.failure == HunkApplyFailure::Overlap) {
            return Conflict(std::move(plan), RestoreIssue::HunkOverlap);
        }
        const bool ambiguous = std::any_of(plan.hunks.begin(), plan.hunks.end(), [](const RestoreHunkOutcome& o) { return o.find == HunkFind::Ambiguous; });
        return Conflict(std::move(plan), ambiguous ? RestoreIssue::HunkAmbiguous : RestoreIssue::HunkNotFound);
    }
    if (!applied.changed) {
        return AlreadyRestored(std::move(plan));
    }
    RestoreOp op;
    op.kind = RestoreOp::Kind::Overwrite;
    op.path = in.revision.path;
    op.source = RestoreOp::Source::Bytes;
    op.bytes = applied.bytes;
    op.size = static_cast<int64_t>(op.bytes.size());
    op.hash = Sha256Hex(op.bytes);
    return Ready(std::move(plan), { std::move(op) });
}

} // namespace

RestoreRevision ParseRestoreRevision(const nlohmann::json& detail)
{
    RestoreRevision rev;
    if (!detail.is_object()) {
        return rev;
    }
    if (detail.contains("revisionId") && detail["revisionId"].is_number_integer()) {
        rev.revisionId = detail["revisionId"].get<int64_t>();
    }
    rev.path = StringField(detail, "path");
    rev.oldPath = StringField(detail, "oldPath");
    rev.changeType = StringField(detail, "changeType");
    rev.actor = StringField(detail, "actor");
    rev.changeState = StringField(detail, "changeState");
    rev.diffMode = StringField(detail, "diffMode");
    rev.completeness = StringField(detail, "completeness");
    rev.before = ParseSide(detail.contains("before") ? detail["before"] : nlohmann::json());
    rev.after = ParseSide(detail.contains("after") ? detail["after"] : nlohmann::json());
    return rev;
}

bool IsRestorableActor(const std::string& actor)
{
    return actor == "ai" || actor == "unknown";
}

std::vector<std::string> RestoreTargetPaths(const RestoreRevision& revision)
{
    std::vector<std::string> paths;
    if (!revision.path.empty()) {
        paths.push_back(revision.path);
    }
    if (revision.changeType == "renamed" && !revision.oldPath.empty()) {
        paths.push_back(revision.oldPath);
    }
    return paths;
}

RestorePlan BuildRestorePlan(const RestoreInput& in)
{
    const RestoreRevision& rev = in.revision;
    RestorePlan plan;
    plan.scope = in.scope;
    plan.pathIssue = in.pathIssue;

    if (in.pathIssue != PathIssue::None) {
        return NotRestorable(std::move(plan), RestoreIssue::PathRejected);
    }
    if (!IsRestorableActor(rev.actor)) {
        return NotRestorable(std::move(plan), RestoreIssue::ActorNotRestorable);
    }
    // Before: 追加された変更以外は、戻す先の内容 (Blob) が要る
    if (rev.changeType != "added") {
        if (rev.before.state == RestoreBlob::NoSnapshot || (rev.before.state == RestoreBlob::Present && rev.before.hash.empty())) {
            return NotRestorable(std::move(plan), RestoreIssue::NoBefore);
        }
        if (rev.before.state != RestoreBlob::Present) {
            return NotRestorable(std::move(plan), RestoreIssue::BeforeUnavailable);
        }
        if (rev.changeType == "renamed" && rev.oldPath.empty()) {
            return NotRestorable(std::move(plan), RestoreIssue::NoBefore);
        }
    } else if (rev.before.state != RestoreBlob::NoSnapshot) {
        return NotRestorable(std::move(plan), RestoreIssue::NoBefore); // 追加なのに Before がある (記録の食い違い)
    }
    // After: 削除された変更以外は、今の内容と比べるためのハッシュが要る (Blob が欠けていてもハッシュがあれば比べられる)
    if (rev.changeType != "deleted" && (rev.after.state == RestoreBlob::NoSnapshot || rev.after.hash.empty())) {
        return NotRestorable(std::move(plan), RestoreIssue::AfterUnknown);
    }
    if (in.scope == RestoreScope::Hunks) {
        if (rev.changeType != "modified") {
            return NotRestorable(std::move(plan), RestoreIssue::HunkNeedsModified);
        }
        if (rev.diffMode != "lines") {
            return NotRestorable(std::move(plan), RestoreIssue::HunkNeedsLines);
        }
        if (in.hunks.empty()) {
            return NotRestorable(std::move(plan), RestoreIssue::HunkNoneSelected);
        }
        if (std::any_of(in.hunks.begin(), in.hunks.end(), [](const TrackHunk& h) { return h.oversize; })) {
            return NotRestorable(std::move(plan), RestoreIssue::HunkOversize);
        }
    }
    if (const RestoreIssue problem = NowProblem(in.now); problem != RestoreIssue::None) {
        return NotRestorable(std::move(plan), problem);
    }
    if (rev.changeType == "renamed") {
        if (const RestoreIssue problem = NowProblem(in.nowOld); problem != RestoreIssue::None) {
            return NotRestorable(std::move(plan), problem);
        }
    }
    return in.scope == RestoreScope::Hunks ? PlanHunks(in, std::move(plan)) : PlanFile(in, std::move(plan));
}

std::vector<DiffHunkInput> ReverseHunkView(const std::vector<TrackHunk>& hunks)
{
    std::vector<DiffHunkInput> out;
    out.reserve(hunks.size());
    for (const TrackHunk& h : hunks) {
        DiffHunkInput in;
        // 戻すと After の側が Before の側になる。見出しの座標も入れ替える
        in.oldStart = h.newStart;
        in.oldCount = h.newCount;
        in.newStart = h.oldStart;
        in.newCount = h.oldCount;
        in.before = h.afterText;
        in.after = h.beforeText;
        in.contextPre = h.contextPre;
        in.contextPost = h.contextPost;
        in.eolOnly = h.eolOnly;
        in.oversize = h.oversize;
        in.beforeBytes = h.afterBytes;
        in.afterBytes = h.beforeBytes;
        out.push_back(std::move(in));
    }
    return out;
}

std::vector<DiffHunkInput> ConflictHunkView(const std::string& currentBytes, const std::vector<TrackHunk>& hunks, const std::vector<RestoreHunkOutcome>& outcomes)
{
    std::vector<DiffHunkInput> out;
    const std::vector<std::string_view> lines = SplitLinesKeepEol(currentBytes);
    for (const RestoreHunkOutcome& outcome : outcomes) {
        if (outcome.find != HunkFind::NotFound && outcome.find != HunkFind::Ambiguous) {
            continue;
        }
        const auto it = std::find_if(hunks.begin(), hunks.end(), [&](const TrackHunk& h) { return h.hunkId == outcome.hunkId; });
        if (it == hunks.end()) {
            continue;
        }
        // 記録した位置 (After の座標) にある、記録した After と同じ行数の今の内容
        const size_t start = static_cast<size_t>((std::min<int64_t>)((std::max<int64_t>)(it->newStart - 1, 0), static_cast<int64_t>(lines.size())));
        const size_t count = static_cast<size_t>((std::max<int64_t>)(it->newCount, 0));
        std::string nowText;
        size_t nowLines = 0;
        for (size_t i = start; i < lines.size() && nowLines < count; ++i, ++nowLines) {
            nowText.append(lines[i]);
        }
        DiffHunkInput in;
        in.oldStart = it->newStart;
        in.oldCount = static_cast<int64_t>(nowLines);
        in.newStart = it->newStart;
        in.newCount = it->newCount;
        in.before = std::move(nowText);  // 今
        in.after = it->afterText;        // 記録した After
        in.contextPre = it->contextPre;
        in.contextPost = it->contextPost;
        out.push_back(std::move(in));
    }
    return out;
}

RestoreNotes BuildRestoreNotes(const RestorePlan& plan, const std::string& activeScene, const std::function<StagedState(const std::string&)>& stagedOf)
{
    RestoreNotes notes;
    if (plan.verdict != RestoreVerdict::Ready) {
        return notes;
    }
    StageInputs stageInputs;
    stageInputs.activeScene = activeScene;
    std::vector<std::string> touched;
    auto isMeta = [](const std::string& path) { return IsMetaSidecar(path); };
    for (const RestoreOp& op : plan.ops) {
        StageChange change;
        change.path = op.kind == RestoreOp::Kind::Rename ? op.toPath : op.path;
        switch (op.kind) {
        case RestoreOp::Kind::Overwrite: change.kind = BatchChange::Kind::Modified; break;
        case RestoreOp::Kind::Delete:
            change.kind = BatchChange::Kind::Deleted;
            notes.deletesFile = true;
            notes.referencesUnchecked = true;
            break;
        case RestoreOp::Kind::Create: change.kind = BatchChange::Kind::Added; break;
        case RestoreOp::Kind::Rename:
            change.kind = BatchChange::Kind::Renamed;
            change.oldPath = op.path;
            touched.push_back(op.path);
            break;
        }
        if (isMeta(op.kind == RestoreOp::Kind::Rename ? op.toPath : op.path)) {
            notes.metaGuidUnchecked = true;
        }
        touched.push_back(change.path);
        stageInputs.changes.push_back(std::move(change));
    }
    notes.stage = Classify(stageInputs);
    for (const RestoreHunkOutcome& outcome : plan.hunks) {
        if (outcome.shiftLines != 0 && outcome.find == HunkFind::Match) {
            notes.shifted = true;
        }
    }
    std::sort(touched.begin(), touched.end());
    touched.erase(std::unique(touched.begin(), touched.end()), touched.end());
    for (const std::string& path : touched) {
        const StagedState state = stagedOf ? stagedOf(path) : StagedState::Unknown;
        if (state == StagedState::Staged) {
            notes.stagedPaths.push_back(path);
        } else if (state == StagedState::Unknown) {
            notes.stagedUnknown = true;
        }
    }
    return notes;
}

} // namespace mye
