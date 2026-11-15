//====================================================================================
//                          RestoreRunWindow.cpp
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          復元の実行・結果・起動時の回復・履歴の画面
//====================================================================================
#include "Editor/Windows/Project/RestoreRunWindow.h"

#include "imgui.h"

#include "Editor/ChangeTracking/RestoreFormat.h"
#include "Engine/Core/Localization/Localization.h"
#include "Engine/Renderer/ImGui/ImGuiTheme.h"

namespace mye {

namespace {

void BulletLines(const std::vector<std::string>& lines)
{
    for (const std::string& line : lines) {
        ImGui::Bullet();
        ImGui::TextWrapped("%s", line.c_str());
    }
}

// "表示名###識別子" の表示名だけ (タイトルバーの無い窓に見出しとして出す)
std::string VisibleTitle(const char* title)
{
    std::string text = title;
    const size_t id = text.find("###");
    return id == std::string::npos ? text : text.substr(0, id);
}

ImVec4 OutcomeColor(RestoreOutcome outcome)
{
    switch (outcome) {
    case RestoreOutcome::Committed: return themeColor::Success;
    case RestoreOutcome::RolledBack:
    case RestoreOutcome::Blocked:
    case RestoreOutcome::Cancelled: return themeColor::Warning;
    case RestoreOutcome::RecoveryRequired: return themeColor::Error;
    case RestoreOutcome::None: break;
    }
    return themeColor::Text;
}

} // namespace

void RestoreRunWindow::OnImGui(ChangeTrackingSession& session, const AiChangesHost& host)
{
    runView_ = BuildRestoreRunView(session.Restore());
    DrawRun(session);
    if (recoveryOpen_) {
        DrawRecovery(session, host);
    }
    if (historyOpen_) {
        DrawHistory(session);
    }
}

// ---- 進行と結果 (モーダル。書き込み中は他の操作を受けない) ----

void RestoreRunWindow::DrawRun(ChangeTrackingSession& session)
{
    RestoreRunner& runner = session.Restore();
    if (runView_.mode == RestoreRunView::Mode::Progress) {
        const char* title = Tr(StrId::AiChg_RrTitle);
        if (!ImGui::IsPopupOpen(title)) {
            ImGui::OpenPopup(title);
        }
        if (ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
            ImGui::TextUnformatted(VisibleTitle(title).c_str());
            ImGui::Separator();
            ImGui::TextUnformatted(runView_.phase.c_str());
            ImGui::ProgressBar(runView_.fraction, ImVec2(ImGui::GetFontSize() * 24.0f, 0.0f));
            ImGui::TextDisabled("%s", runView_.cancelHint.c_str());
            ImGui::BeginDisabled(!runView_.canCancel);
            if (ImGui::Button(Tr(StrId::AiChg_RrCancel))) {
                runner.Cancel();
            }
            ImGui::EndDisabled();
            ImGui::EndPopup();
        }
        return;
    }
    if (runView_.mode == RestoreRunView::Mode::Result) {
        const char* title = Tr(StrId::AiChg_RrResultTitle);
        if (!ImGui::IsPopupOpen(title)) {
            ImGui::OpenPopup(title);
        }
        ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 36.0f, 0.0f), ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::PushStyleColor(ImGuiCol_Text, OutcomeColor(runView_.outcome));
            ImGui::TextWrapped("%s", runView_.headline.c_str());
            ImGui::PopStyleColor();
            BulletLines(runView_.reasons);
            if (!runView_.unresolved.empty()) {
                ImGui::TextUnformatted(Tr(StrId::AiChg_RrUnresolved));
                BulletLines(runView_.unresolved);
            }
            if (!runView_.journal.empty()) {
                ImGui::TextDisabled("%s", runView_.journal.c_str());
            }
            if (!runView_.recordNote.empty()) {
                ImGui::TextWrapped("%s", runView_.recordNote.c_str());
            }
            ImGui::Separator();
            bool close = false;
            if (runView_.needsRecovery) {
                if (ImGui::Button(Tr(StrId::AiChg_RcOpen))) {
                    OpenRecovery();
                    close = true;
                }
                ImGui::SameLine();
            }
            if (ImGui::Button(Tr(StrId::Common_Close))) {
                close = true;
            }
            if (close) {
                runner.AcknowledgeResult();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
}

// ---- 起動時の回復 ----

void RestoreRunWindow::Inspect(ChangeTrackingSession& session)
{
    inspected_.clear();
    for (const RestoreJournalStore::Entry& entry : session.Restore().Recoveries()) {
        if (!entry.broken) {
            inspected_.emplace_back(entry.id, session.Restore().InspectRecovery(entry.id));
        }
    }
    inspectedDirty_ = false;
}

bool RestoreRunWindow::RestoreOriginal(ChangeTrackingSession& session, const AiChangesHost& host, const std::string& id)
{
    std::vector<std::string> paths;
    for (const RestoreJournalStore::Entry& entry : session.Restore().Recoveries()) {
        if (entry.id == id) {
            for (const JournalFile& file : entry.manifest.files) {
                paths.push_back(file.path);
            }
        }
    }
    const bool started = session.Restore().StartRecoveryRollback(id, [&session, hostCopy = host, paths](bool blobMissing) {
        return BuildRestoreBlocks(session, hostCopy, paths, blobMissing);
    });
    inspectedDirty_ = true;
    return started;
}

bool RestoreRunWindow::KeepCurrent(ChangeTrackingSession& session, const std::string& id)
{
    const bool ok = session.Restore().KeepCurrent(id);
    inspectedDirty_ = true;
    return ok;
}

bool RestoreRunWindow::Dismiss(ChangeTrackingSession& session, const std::string& id)
{
    const bool ok = session.Restore().Dismiss(id);
    inspectedDirty_ = true;
    return ok;
}

void RestoreRunWindow::DrawRecovery(ChangeTrackingSession& session, const AiChangesHost& host)
{
    RestoreRunner& runner = session.Restore();
    if (!runner.RecoveryPending()) {
        recoveryOpen_ = false;
        return;
    }
    if (inspectedDirty_) {
        Inspect(session);
    }
    recoveryViews_ = BuildRecoveryViews(runner, inspected_);
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 56.0f, ImGui::GetFontSize() * 34.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(Tr(StrId::AiChg_RcTitle), &recoveryOpen_)) {
        ImGui::End();
        return;
    }
    ImGui::TextWrapped("%s", Tr(StrId::AiChg_RcIntro));
    if (!recoveryBlocks_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, themeColor::Warning);
        ImGui::TextUnformatted(Tr(StrId::AiChg_RcBlocked));
        ImGui::PopStyleColor();
        BulletLines(recoveryBlocks_);
    }
    std::string chosen;
    enum class Choice : uint8_t { None, Restore, Keep, Ignore } choice = Choice::None;
    for (const RecoveryEntryView& entry : recoveryViews_) {
        ImGui::PushID(entry.id.c_str());
        ImGui::Separator();
        ImGui::TextUnformatted(entry.header.c_str());
        if (entry.broken) {
            ImGui::TextWrapped("%s", entry.brokenText.c_str());
            if (ImGui::Button(Tr(StrId::AiChg_RcBtnIgnore))) {
                choice = Choice::Ignore;
                chosen = entry.id;
            }
            ImGui::PopID();
            continue;
        }
        if (ImGui::BeginTable("##files", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn(Tr(StrId::AiChg_RcColFile));
            ImGui::TableSetupColumn(Tr(StrId::AiChg_RcColPre));
            ImGui::TableSetupColumn(Tr(StrId::AiChg_RcColIntended));
            ImGui::TableSetupColumn(Tr(StrId::AiChg_RcColNow));
            ImGui::TableHeadersRow();
            for (const RecoveryFileText& file : entry.files) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(file.path.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(file.before.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(file.intended.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%s  %s", file.now.c_str(), file.verdict.c_str());
            }
            ImGui::EndTable();
        }
        if (ImGui::Button(Tr(StrId::AiChg_RcBtnRestore))) {
            choice = Choice::Restore;
            chosen = entry.id;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", Tr(StrId::AiChg_RcBtnRestoreTip));
        }
        ImGui::SameLine();
        if (ImGui::Button(Tr(StrId::AiChg_RcBtnKeep))) {
            choice = Choice::Keep;
            chosen = entry.id;
        }
        ImGui::PopID();
    }
    ImGui::End();
    switch (choice) {
    case Choice::Restore:
        recoveryBlocks_.clear();
        if (!RestoreOriginal(session, host, chosen) && runner.HasResult()) {
            for (const RestoreBlock& block : runner.Result().blocks) {
                recoveryBlocks_.push_back(RestoreBlockText(block));
            }
            runner.AcknowledgeResult(); // 画面に理由を出したので、結果のモーダルは出さない
        }
        break;
    case Choice::Keep: KeepCurrent(session, chosen); break;
    case Choice::Ignore: Dismiss(session, chosen); break;
    case Choice::None: break;
    }
}

// ---- 履歴 ----

void RestoreRunWindow::OpenHistory(ChangeTrackingSession& session)
{
    historyOpen_ = true;
    session.Restore().RequestHistory();
}

void RestoreRunWindow::DrawHistory(ChangeTrackingSession& session)
{
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 44.0f, ImGui::GetFontSize() * 18.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(Tr(StrId::AiChg_RhTitle), &historyOpen_)) {
        ImGui::End();
        return;
    }
    RestoreRunner& runner = session.Restore();
    if (!session.Client().CanRequest()) {
        ImGui::TextDisabled("%s", Tr(StrId::AiChg_RhNeedService));
    }
    const std::vector<RestoreHistoryRow> rows = BuildHistoryRows(runner.History());
    if (rows.empty()) {
        ImGui::TextDisabled("%s", Tr(StrId::AiChg_RhEmpty));
    } else if (ImGui::BeginTable("##history", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn(Tr(StrId::AiChg_RhColTime));
        ImGui::TableSetupColumn(Tr(StrId::AiChg_RhColState));
        ImGui::TableSetupColumn(Tr(StrId::AiChg_RhColTarget));
        ImGui::TableHeadersRow();
        for (const RestoreHistoryRow& row : rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(row.time.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(row.state.c_str());
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", row.target.c_str());
            if (!row.error.empty() && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", row.error.c_str());
            }
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

} // namespace mye
