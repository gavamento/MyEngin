//====================================================================================
//                          SceneRestoreWindow.cpp
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          ライブのシーンの復元のプレビューと結果の画面
//====================================================================================
#include "Editor/Windows/Project/SceneRestoreWindow.h"

#include <filesystem>

#include "imgui.h"

#include "Engine/Core/Localization/Localization.h"
#include "Engine/Platform/PathUtil.h"
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

ImVec4 VerdictColor(SceneRestoreVerdict verdict)
{
    switch (verdict) {
    case SceneRestoreVerdict::Ready: return themeColor::Success;
    case SceneRestoreVerdict::AlreadyRestored: return themeColor::TextDim;
    case SceneRestoreVerdict::Conflict: return themeColor::Error;
    case SceneRestoreVerdict::NotRestorable: return themeColor::Warning;
    }
    return themeColor::Text;
}

} // namespace

bool SceneRestoreDialog::Open(ChangeTrackingSession& session, const AiChangesHost& host, SceneRestoreScope scope, const TrackSceneChange& anchor)
{
    const ChangeModel& model = session.Model();
    const nlohmann::json& detail = model.Detail();
    if (model.SelectedRevision() == 0 || !detail.is_object() || !detail.contains("revisionId") || !detail["revisionId"].is_number_integer()
        || detail["revisionId"].get<int64_t>() != model.SelectedRevision() || !model.SceneChangesComplete()) {
        return false;
    }
    SceneRestoreOpenArgs args;
    args.revisionId = model.SelectedRevision();
    args.scope = scope;
    args.actor = detail.contains("actor") && detail["actor"].is_string() ? detail["actor"].get<std::string>() : std::string();
    args.openDocumentId = session.Overlay().DocumentId();
    args.scenePath = host.editedDocuments ? host.editedDocuments().openScene : std::string();
    args.sceneName = args.scenePath.empty() ? WideToUtf8(std::filesystem::path(session.Overlay().DocumentPath()).filename().wstring()) : args.scenePath;
    args.rows = SelectSceneRows(model.SceneChanges(), anchor, scope);
    const std::string& doc = anchor.resolvedDocumentId.empty() ? anchor.documentId : anchor.resolvedDocumentId;
    args.notOpen = args.openDocumentId.empty() || doc != args.openDocumentId;
    OpenWith(session, host, std::move(args));
    return true;
}

void SceneRestoreDialog::OpenWith(ChangeTrackingSession& session, const AiChangesHost& host, SceneRestoreOpenArgs args)
{
    revisionId_ = args.revisionId;
    scope_ = args.scope;
    actor_ = std::move(args.actor);
    openDocumentId_ = std::move(args.openDocumentId);
    scenePath_ = std::move(args.scenePath);
    sceneName_ = std::move(args.sceneName);
    rows_ = std::move(args.rows);
    notOpen_ = args.notOpen;
    plan_ = SceneRestorePlan();
    const SceneRestoreEnv env = host.sceneRestoreEnv ? host.sceneRestoreEnv() : SceneRestoreEnv();
    if (!notOpen_ && env.scene != nullptr) {
        SceneRestoreInput input;
        input.rows = rows_;
        input.actor = actor_;
        input.openDocumentId = openDocumentId_;
        LiveSceneReadView view(env);
        plan_ = BuildSceneRestorePlan(input, view);
    }
    open_ = true;
    openRequested_ = true;
    screen_ = BuildScreen(session, host);
}

void SceneRestoreDialog::Close()
{
    open_ = false;
    openRequested_ = false;
    rows_.clear();
    plan_ = SceneRestorePlan();
    screen_ = SceneRestoreScreen();
}

std::vector<RestoreBlock> SceneRestoreDialog::Blocks(ChangeTrackingSession& session, const AiChangesHost& host) const
{
    RestoreGateInput in;
    in.target = RestoreGateTarget::LiveScene;
    if (host.gateInputs) {
        in.gate = host.gateInputs();
    }
    if (host.editedDocuments) {
        in.documents = host.editedDocuments();
    }
    in.targetPaths = { scenePath_ };
    in.trackConnected = session.Client().CanRequest();
    in.journalPending = (host.restoreJournalPending && host.restoreJournalPending()) || session.Restore().Pending();
    in.externalChangePending = host.externalChangePending;
    std::vector<RestoreBlock> blocks = EvaluateRestoreGate(in);
    const SceneRestoreEnv env = host.sceneRestoreEnv ? host.sceneRestoreEnv() : SceneRestoreEnv();
    if (env.scene == nullptr || env.undo == nullptr || env.selection == nullptr) {
        RestoreBlock block;
        block.kind = RestoreBlockKind::NoScene;
        blocks.push_back(block);
    }
    return blocks;
}

SceneRestoreScreen SceneRestoreDialog::BuildScreen(ChangeTrackingSession& session, const AiChangesHost& host) const
{
    SceneRestoreScreenInput input;
    input.sceneName = sceneName_;
    input.scope = scope_;
    input.rowCount = rows_.size();
    input.notOpen = notOpen_;
    input.plan = notOpen_ ? nullptr : &plan_;
    input.blocks = Blocks(session, host);
    return BuildSceneRestoreScreen(input);
}

bool SceneRestoreDialog::Apply(ChangeTrackingSession& session, const AiChangesHost& host)
{
    if (!open_ || notOpen_ || plan_.verdict != SceneRestoreVerdict::Ready || plan_.ops.empty() || !Blocks(session, host).empty()) {
        return false;
    }
    SceneRestoreRequest request;
    request.env = host.sceneRestoreEnv ? host.sceneRestoreEnv() : SceneRestoreEnv();
    request.revisionId = revisionId_;
    request.actor = actor_;
    request.openDocumentId = openDocumentId_;
    request.scenePath = scenePath_;
    request.rows = rows_;
    request.previewOps = plan_.ops;
    request.undoLabel = Tr(StrId::AiChg_SrUndoLabel);
    // 実行の直前に、同じゲートでもう一度判定する
    request.blocks = [this, &session, &host]() { return Blocks(session, host); };
    result_ = ApplySceneRestore(request);
    // 記録は、書いた (または書いて戻した) ときだけサービスへ送る。止めた・競合のときは何も書いていないので送らない
    if (result_.outcome == SceneRestoreOutcome::Committed) {
        session.Restore().QueueRecord(BuildSceneRestoreRecord(result_.restoreId, "committed", revisionId_, RestoredSceneChangeIds(result_.plan), scenePath_, std::string()));
    } else if (result_.outcome == SceneRestoreOutcome::VerifyFailed) {
        session.Restore().QueueRecord(BuildSceneRestoreRecord(result_.restoreId, "rolled_back", revisionId_, RestoredSceneChangeIds(result_.plan), scenePath_, "verify_failed " + result_.detail));
    }
    resultView_ = BuildSceneRestoreResultView(result_, session.Restore().PendingRecordCount() > 0);
    hasResult_ = true;
    resultPopupRequested_ = true;
    Close();
    return true;
}

void SceneRestoreDialog::OnImGui(ChangeTrackingSession& session, const AiChangesHost& host)
{
    if (open_) {
        DrawPreview(session, host);
    }
    if (hasResult_) {
        DrawResult(session);
    }
}

void SceneRestoreDialog::DrawPreview(ChangeTrackingSession& session, const AiChangesHost& host)
{
    const char* title = Tr(StrId::AiChg_SrTitle);
    if (openRequested_ || !ImGui::IsPopupOpen(title)) {
        ImGui::OpenPopup(title);
        openRequested_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 52.0f, ImGui::GetFontSize() * 36.0f), ImGuiCond_Appearing);
    bool keepOpen = true;
    if (!ImGui::BeginPopupModal(title, &keepOpen)) {
        return;
    }
    // ゲートは毎フレーム判定し直す (確認の画面を出している間に、再生・外部の変更の状態が変わる)
    RefreshScreen(session, host);
    const SceneRestoreScreen& screen = screen_;
    // 本文はスクロールでき、ボタンは常に下に見える (操作の一覧が長くても「復元する」を探さずに済む)
    const float footerHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetTextLineHeightWithSpacing() * 2.0f + ImGui::GetStyle().ItemSpacing.y * 3.0f;
    if (ImGui::BeginChild("##scenerestorebody", ImVec2(0.0f, -footerHeight))) {
        ImGui::TextUnformatted(Tr(StrId::AiChg_RsTarget));
        ImGui::SameLine();
        ImGui::TextUnformatted(screen.target.c_str());
        ImGui::TextDisabled("%s", screen.scope.c_str());
        ImGui::TextDisabled("%s", screen.rowCount.c_str());
        ImGui::Separator();
        if (screen.notOpen) {
            ImGui::PushStyleColor(ImGuiCol_Text, themeColor::Warning);
            ImGui::TextWrapped("%s", screen.notOpenText.c_str());
            ImGui::PopStyleColor();
        } else {
            if (screen.blocks.empty()) {
                ImGui::TextDisabled("%s", Tr(StrId::AiChg_RsNoBlocks));
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, themeColor::Warning);
                ImGui::TextUnformatted(Tr(StrId::AiChg_RsBlocks));
                ImGui::PopStyleColor();
                BulletLines(screen.blocks);
            }
            if (screen.hasPlan) {
                ImGui::Separator();
                ImGui::TextUnformatted(Tr(StrId::AiChg_RsResult));
                ImGui::PushStyleColor(ImGuiCol_Text, VerdictColor(screen.verdict));
                ImGui::TextWrapped("%s", screen.verdictText.c_str());
                ImGui::PopStyleColor();
                BulletLines(screen.issues);
                if (!screen.ops.empty()) {
                    ImGui::Separator();
                    ImGui::TextUnformatted(Tr(StrId::AiChg_RsWillWrite));
                    BulletLines(screen.ops);
                }
                if (!screen.notes.empty()) {
                    ImGui::Separator();
                    ImGui::TextUnformatted(Tr(StrId::AiChg_RsNotes));
                    BulletLines(screen.notes);
                }
            }
        }
    }
    ImGui::EndChild();
    ImGui::Separator();
    // 照合に通って、止める理由が無いときだけ押せる。押すと実行の直前にもう一度判定する
    ImGui::BeginDisabled(!screen.canApply);
    const bool apply = ImGui::Button(Tr(StrId::AiChg_RsApply));
    ImGui::EndDisabled();
    ImGui::SameLine();
    const bool close = ImGui::Button(Tr(StrId::Common_Close));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", screen.applyNote.c_str());
    ImGui::PopStyleColor();
    if (apply && Apply(session, host)) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    if (close || !keepOpen) {
        ImGui::CloseCurrentPopup();
        Close();
    }
    ImGui::EndPopup();
}

void SceneRestoreDialog::DrawResult(ChangeTrackingSession& session)
{
    const char* title = Tr(StrId::AiChg_SrResultTitle);
    if (resultPopupRequested_ || !ImGui::IsPopupOpen(title)) {
        ImGui::OpenPopup(title);
        resultPopupRequested_ = false;
    }
    resultView_.recordNote = session.Restore().PendingRecordCount() > 0 ? Tr(StrId::AiChg_SrRecordPending) : std::string();
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 36.0f, 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, resultView_.success ? themeColor::Success : themeColor::Warning);
    ImGui::TextWrapped("%s", resultView_.headline.c_str());
    ImGui::PopStyleColor();
    BulletLines(resultView_.reasons);
    if (!resultView_.recordNote.empty()) {
        ImGui::TextWrapped("%s", resultView_.recordNote.c_str());
    }
    ImGui::Separator();
    if (ImGui::Button(Tr(StrId::Common_Close))) {
        ImGui::CloseCurrentPopup();
        AcknowledgeResult();
    }
    ImGui::EndPopup();
}

} // namespace mye
