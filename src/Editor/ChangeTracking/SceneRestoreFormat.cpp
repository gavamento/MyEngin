//====================================================================================
//                          SceneRestoreFormat.cpp
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          ライブのシーンの復元の文面の整形 (ImGui を呼ばない純関数)
//====================================================================================
#include "Editor/ChangeTracking/SceneRestoreFormat.h"

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

constexpr size_t kValueChars = 40;
// 画面に並べる理由の行の上限 (数千行の Revision で窓を埋めない)
constexpr size_t kMaxIssueLines = 30;

std::string ValueText(const nlohmann::json& value)
{
    return value.is_discarded() || value.is_null() ? std::string("-") : ClipValue(value.dump(), kValueChars);
}

std::string EntityText(const SceneRestoreOp& op)
{
    return (op.entityName.empty() ? std::string("entity") : op.entityName) + " #" + std::to_string(op.fileId);
}

std::string RowLabel(const SceneRestoreRowResult& row)
{
    std::string label = row.fileId != 0 ? "#" + std::to_string(row.fileId) : std::string();
    if (!row.component.empty()) {
        label += (label.empty() ? "" : " ") + row.component + (row.field.empty() ? "" : "." + row.field);
    } else if (!row.field.empty()) {
        label += (label.empty() ? "" : " ") + row.field;
    }
    return label.empty() ? std::to_string(row.sceneChangeId) : label;
}

} // namespace

std::string SceneRestoreScopeText(SceneRestoreScope scope)
{
    switch (scope) {
    case SceneRestoreScope::Field: return Tr(StrId::AiChg_SrScopeField);
    case SceneRestoreScope::Component: return Tr(StrId::AiChg_SrScopeComponent);
    case SceneRestoreScope::Entity: return Tr(StrId::AiChg_SrScopeEntity);
    case SceneRestoreScope::Revision: return Tr(StrId::AiChg_SrScopeRevision);
    }
    return std::string();
}

std::string SceneRestoreIssueText(SceneRestoreIssue issue, const std::string& detail)
{
    switch (issue) {
    case SceneRestoreIssue::None: return std::string();
    case SceneRestoreIssue::ActorNotRestorable: return Tr(StrId::AiChg_SrIssueActor);
    case SceneRestoreIssue::NotOpenDocument: return Tr(StrId::AiChg_SrIssueNotOpen);
    case SceneRestoreIssue::NoRows: return Tr(StrId::AiChg_SrIssueNoRows);
    case SceneRestoreIssue::TooManyRows: return Tr(StrId::AiChg_SrIssueTooMany);
    case SceneRestoreIssue::RowUnsupported: return Tr(StrId::AiChg_SrIssueUnsupported);
    case SceneRestoreIssue::PropagatedRow: return Tr(StrId::AiChg_SrIssuePropagated);
    case SceneRestoreIssue::OverrideKeyOnly: return Tr(StrId::AiChg_SrIssueOverrideKey);
    case SceneRestoreIssue::ValueOversize: return Tr(StrId::AiChg_SrIssueOversize);
    case SceneRestoreIssue::NoBefore: return Tr(StrId::AiChg_SrIssueNoBefore);
    case SceneRestoreIssue::ValueUnreadable: return Tr(StrId::AiChg_SrIssueUnreadable);
    case SceneRestoreIssue::UnknownComponent: return Tr(StrId::AiChg_SrIssueUnknownComp);
    case SceneRestoreIssue::UnknownField: return Tr(StrId::AiChg_SrIssueUnknownField);
    case SceneRestoreIssue::ProtectedComponent: return Tr(StrId::AiChg_SrIssueProtected);
    case SceneRestoreIssue::EntityMissing: return Tr(StrId::AiChg_SrIssueEntityMissing);
    case SceneRestoreIssue::ComponentMissing: return Tr(StrId::AiChg_SrIssueCompMissing);
    case SceneRestoreIssue::ValueChanged: return Tr(StrId::AiChg_SrIssueValueChanged);
    case SceneRestoreIssue::ComponentChanged: return Tr(StrId::AiChg_SrIssueCompChanged);
    case SceneRestoreIssue::ComponentExists: return Tr(StrId::AiChg_SrIssueCompExists);
    case SceneRestoreIssue::EntityChanged: return Tr(StrId::AiChg_SrIssueEntityChanged);
    case SceneRestoreIssue::ExtraDescendant: return Fill(Tr(StrId::AiChg_SrIssueExtraChild), { { "id", detail } });
    case SceneRestoreIssue::EntityExists: return Tr(StrId::AiChg_SrIssueEntityExists);
    case SceneRestoreIssue::ParentMissing: return Fill(Tr(StrId::AiChg_SrIssueParentMissing), { { "id", detail } });
    case SceneRestoreIssue::ReferenceMissing: return Fill(Tr(StrId::AiChg_SrIssueRefMissing), { { "id", detail } });
    case SceneRestoreIssue::ParentCycle: return Fill(Tr(StrId::AiChg_SrIssueCycle), { { "id", detail } });
    }
    return std::string();
}

std::string SceneRestoreOpText(const SceneRestoreOp& op)
{
    const std::string entity = EntityText(op);
    const std::string id = std::to_string(op.fileId);
    switch (op.kind) {
    case SceneRestoreOp::Kind::SetField:
        return Fill(Tr(StrId::AiChg_SrOpSetField),
                    { { "entity", entity }, { "component", op.component }, { "field", op.field }, { "from", ValueText(op.current) }, { "to", ValueText(op.value) } });
    case SceneRestoreOp::Kind::SetEntityKey:
        return Fill(Tr(StrId::AiChg_SrOpSetKey), { { "entity", entity }, { "field", op.field }, { "from", ValueText(op.current) }, { "to", ValueText(op.value) } });
    case SceneRestoreOp::Kind::SetHierarchy:
        return Fill(Tr(StrId::AiChg_SrOpSetHierarchy), { { "entity", entity }, { "field", op.field }, { "from", ValueText(op.current) }, { "to", ValueText(op.value) } });
    case SceneRestoreOp::Kind::AddComponent: return Fill(Tr(StrId::AiChg_SrOpAddComponent), { { "entity", entity }, { "component", op.component } });
    case SceneRestoreOp::Kind::RemoveComponent: return Fill(Tr(StrId::AiChg_SrOpRemoveComponent), { { "entity", entity }, { "component", op.component } });
    case SceneRestoreOp::Kind::CreateEntities:
        return Fill(Tr(StrId::AiChg_SrOpCreate), { { "entity", op.entityName.empty() ? std::string("entity") : op.entityName }, { "id", id }, { "n", std::to_string(op.value.size()) } });
    case SceneRestoreOp::Kind::DestroyEntities:
        return Fill(Tr(StrId::AiChg_SrOpDestroy), { { "entity", op.entityName.empty() ? std::string("entity") : op.entityName }, { "id", id }, { "n", std::to_string(op.subtree.size()) } });
    }
    return std::string();
}

SceneRestoreScreen BuildSceneRestoreScreen(const SceneRestoreScreenInput& input)
{
    SceneRestoreScreen screen;
    screen.open = true;
    screen.target = input.sceneName;
    screen.scope = SceneRestoreScopeText(input.scope);
    screen.rowCount = Fill(Tr(StrId::AiChg_SrRows), { { "n", std::to_string(input.rowCount) } });
    screen.notOpen = input.notOpen;
    for (const RestoreBlock& block : input.blocks) {
        screen.blocks.push_back(RestoreBlockText(block));
    }
    screen.applyNote = Tr(StrId::AiChg_SrApplyNote);
    if (input.notOpen) {
        screen.notOpenText = Tr(StrId::AiChg_SrNotOpen);
        return screen;
    }
    if (input.plan == nullptr) {
        return screen;
    }
    const SceneRestorePlan& plan = *input.plan;
    screen.hasPlan = true;
    screen.verdict = plan.verdict;
    screen.verdictText = RestoreVerdictText(static_cast<RestoreVerdict>(plan.verdict)); // 4 つの値は同じ並びで同じ意味
    size_t shown = 0;
    size_t hidden = 0;
    for (const SceneRestoreRowResult& row : plan.rows) {
        if (row.derived || (row.verdict != SceneRestoreVerdict::Conflict && row.verdict != SceneRestoreVerdict::NotRestorable)) {
            continue;
        }
        if (shown >= kMaxIssueLines) {
            ++hidden;
            continue;
        }
        screen.issues.push_back(RowLabel(row) + ": " + SceneRestoreIssueText(row.issue, row.detail));
        ++shown;
    }
    if (hidden > 0) {
        screen.issues.push_back("... +" + std::to_string(hidden));
    }
    if (screen.issues.empty() && plan.issue != SceneRestoreIssue::None) {
        screen.issues.push_back(SceneRestoreIssueText(plan.issue, std::string())); // 行に当たらない理由 (範囲が空・行為者)
    }
    bool deletes = false;
    if (plan.verdict == SceneRestoreVerdict::Ready) {
        for (const SceneRestoreOp& op : plan.ops) {
            screen.ops.push_back(SceneRestoreOpText(op));
            deletes = deletes || op.kind == SceneRestoreOp::Kind::DestroyEntities;
        }
        screen.notes.push_back(Tr(StrId::AiChg_SrNoteUndo));
        screen.notes.push_back(Tr(StrId::AiChg_SrNoteUnsaved));
        if (deletes) {
            screen.notes.push_back(Tr(StrId::AiChg_SrNoteDelete));
        }
    }
    screen.canApply = plan.verdict == SceneRestoreVerdict::Ready && input.blocks.empty();
    return screen;
}

SceneRestoreResultView BuildSceneRestoreResultView(const SceneRestoreResult& result, bool recordPending)
{
    SceneRestoreResultView view;
    view.outcome = result.outcome;
    switch (result.outcome) {
    case SceneRestoreOutcome::Committed:
        view.success = true;
        view.headline = Fill(Tr(StrId::AiChg_SrDone), { { "n", std::to_string(RestoredSceneChangeIds(result.plan).size()) } });
        break;
    case SceneRestoreOutcome::AlreadyDone: view.headline = Tr(StrId::AiChg_SrAlready); break;
    case SceneRestoreOutcome::Blocked:
        view.headline = Tr(StrId::AiChg_SrBlocked);
        for (const RestoreBlock& block : result.blocks) {
            view.reasons.push_back(RestoreBlockText(block));
        }
        break;
    case SceneRestoreOutcome::NotReady:
        view.headline = Tr(StrId::AiChg_SrNotReady);
        for (const SceneRestoreRowResult& row : result.plan.rows) {
            if (!row.derived && (row.verdict == SceneRestoreVerdict::Conflict || row.verdict == SceneRestoreVerdict::NotRestorable) && view.reasons.size() < kMaxIssueLines) {
                view.reasons.push_back(RowLabel(row) + ": " + SceneRestoreIssueText(row.issue, row.detail));
            }
        }
        break;
    case SceneRestoreOutcome::PlanChanged: view.headline = Tr(StrId::AiChg_SrPlanChanged); break;
    case SceneRestoreOutcome::VerifyFailed:
        view.headline = Tr(StrId::AiChg_SrVerifyFailed);
        if (!result.detail.empty()) {
            view.reasons.push_back("#" + result.detail);
        }
        break;
    case SceneRestoreOutcome::NoScene: view.headline = Tr(StrId::AiChg_SrNoScene); break;
    }
    if (recordPending) {
        view.recordNote = Tr(StrId::AiChg_SrRecordPending);
    }
    return view;
}

std::string SceneRestoreHistoryMark(const nlohmann::json& target)
{
    if (!target.is_object() || !target.contains("scope") || !target["scope"].is_string() || target["scope"].get<std::string>() != "scene") {
        return std::string();
    }
    const size_t count = target.contains("sceneChangeIds") && target["sceneChangeIds"].is_array() ? target["sceneChangeIds"].size() : 0;
    return Fill(Tr(StrId::AiChg_SrHistoryScene), { { "n", std::to_string(count) } });
}

} // namespace mye
