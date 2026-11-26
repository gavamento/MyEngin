//====================================================================================
//                          GameLiftWindow.cpp
//  MyEngin/ 秋田蓮音                                                       11/27/2026
//                                          GameLift 窓の実装 (状態確認・Anywhere の手順・確認ダイアログの描画)
//====================================================================================
#include "Editor/Windows/Project/GameLiftWindow.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <Windows.h>

#include "Engine/Core/Localization/Localization.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/ImGui/ImGuiTheme.h" // themeColor (意味色)

#include "imgui.h"
#include "imgui_internal.h" // ClearActiveID (外から変わった欄の編集状態を捨てる)

namespace mye {
namespace {

constexpr float kWindowWidth = 640.0f;
constexpr float kWindowHeight = 720.0f;
constexpr float kStateColumnX = 84.0f; // 手順の行: 状態の次の列 (ボタンの左端)
constexpr float kInputWidth = 220.0f;
constexpr float kFieldWidth = 150.0f;
constexpr float kLogHeight = 120.0f;
constexpr float kBottomHeight = 92.0f;  // 下段 (実行中の CLI と取消・直近のエラー) の固定の高さ。本体は残りでスクロールする
constexpr char kConfirmPrepareId[] = "###GLConfirmPrepare";
constexpr char kConfirmCleanupId[] = "###GLConfirmCleanup";

void CopyToBuffer(char* buffer, size_t size, const std::string& text)
{
    std::snprintf(buffer, size, "%s", text.c_str());
}

const char* ReasonText(BlockReason reason)
{
    switch (reason) {
    case BlockReason::Busy: return Tr(StrId::GameLift_Why_Busy);
    case BlockReason::StatusUnknown: return Tr(StrId::GameLift_Why_StatusUnknown);
    case BlockReason::CliMissing: return Tr(StrId::GameLift_Why_CliMissing);
    case BlockReason::CliTooOld: return Tr(StrId::GameLift_Why_CliTooOld);
    case BlockReason::NotLoggedIn: return Tr(StrId::GameLift_Why_NotLoggedIn);
    case BlockReason::NoRegion: return Tr(StrId::GameLift_Why_NoRegion);
    case BlockReason::InvalidRegion: return Tr(StrId::GameLift_Why_InvalidRegion);
    case BlockReason::InvalidSuffix: return Tr(StrId::GameLift_Why_InvalidSuffix);
    case BlockReason::InvalidFleetName: return Tr(StrId::GameLift_Why_InvalidFleetName);
    case BlockReason::InvalidComputeName: return Tr(StrId::GameLift_Why_InvalidComputeName);
    case BlockReason::InvalidIp: return Tr(StrId::GameLift_Why_InvalidIp);
    case BlockReason::InvalidPort: return Tr(StrId::GameLift_Why_InvalidPort);
    case BlockReason::InvalidMaxPlayers: return Tr(StrId::GameLift_Why_InvalidMaxPlayers);
    case BlockReason::NoProject: return Tr(StrId::GameLift_Why_NoProject);
    case BlockReason::NoGameLogic: return Tr(StrId::GameLift_Why_NoGameLogic);
    case BlockReason::NoServerExe: return Tr(StrId::GameLift_Why_NoServerExe);
    case BlockReason::NoRuntimeExe: return Tr(StrId::GameLift_Why_NoRuntimeExe);
    case BlockReason::NotPrepared: return Tr(StrId::GameLift_Why_NotPrepared);
    case BlockReason::ServerAlreadyRunning: return Tr(StrId::GameLift_Why_ServerRunning);
    case BlockReason::ServerNotRunning: return Tr(StrId::GameLift_Why_ServerNotRunning);
    case BlockReason::ServerNotReady: return Tr(StrId::GameLift_Why_ServerNotReady);
    case BlockReason::ClientLimit: return Tr(StrId::GameLift_Why_ClientLimit);
    case BlockReason::None: break;
    }
    return "";
}

const char* AwsKindText(AwsErrorKind kind)
{
    switch (kind) {
    case AwsErrorKind::Credentials: return Tr(StrId::GameLift_Err_Credentials);
    case AwsErrorKind::NotFound: return Tr(StrId::GameLift_Err_NotFound);
    case AwsErrorKind::CapacityExceeded: return Tr(StrId::GameLift_Err_Capacity);
    case AwsErrorKind::Conflict: return Tr(StrId::GameLift_Err_Conflict);
    case AwsErrorKind::InvalidRequest: return Tr(StrId::GameLift_Err_Invalid);
    case AwsErrorKind::NoRegion: return Tr(StrId::GameLift_Err_NoRegion);
    default: return Tr(StrId::GameLift_Err_Other);
    }
}

const char* FailureText(const FlowError& error)
{
    switch (error.failure) {
    case FlowFailure::Aws: return AwsKindText(error.awsKind);
    case FlowFailure::CliStartFailed: return Tr(StrId::GameLift_Err_CliStart);
    case FlowFailure::CliTimedOut: return Tr(StrId::GameLift_Err_CliTimeout);
    case FlowFailure::InvalidInput: return Tr(StrId::GameLift_Err_InvalidInput);
    case FlowFailure::BadResponse: return Tr(StrId::GameLift_Err_BadResponse);
    case FlowFailure::NoSdkEndpoint: return Tr(StrId::GameLift_Err_NoSdkEndpoint);
    case FlowFailure::ServerStartFailed: return Tr(StrId::GameLift_Err_ServerStart);
    case FlowFailure::ClientStartFailed: return Tr(StrId::GameLift_Err_ClientStart);
    case FlowFailure::CapacityRetriesExhausted: return Tr(StrId::GameLift_Err_CapacityGiveUp);
    case FlowFailure::SessionNotActive: return Tr(StrId::GameLift_Err_SessionNotActive);
    case FlowFailure::SessionWaitTimedOut: return Tr(StrId::GameLift_Err_SessionTimeout);
    case FlowFailure::FleetDeleteTimedOut: return Tr(StrId::GameLift_Err_FleetTimeout);
    case FlowFailure::None: break;
    }
    return "";
}

ImVec4 StepColor(StepState state)
{
    switch (state) {
    case StepState::Running: return themeColor::Accent;
    case StepState::Done: return themeColor::Success;
    case StepState::Failed: return themeColor::Error;
    case StepState::NotDone: break;
    }
    return themeColor::TextDim;
}

const char* StepWord(StepState state)
{
    switch (state) {
    case StepState::Running: return Tr(StrId::GameLift_StateRunning);
    case StepState::Done: return Tr(StrId::GameLift_StateDone);
    case StepState::Failed: return Tr(StrId::GameLift_StateFailed);
    case StepState::NotDone: break;
    }
    return Tr(StrId::GameLift_StateNotDone);
}

void DimText(const char* text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

void ColoredWrapped(const ImVec4& color, const char* text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

} // namespace

bool SyncTextBuffer(char* buffer, size_t size, const std::string& value)
{
    if (value == buffer) {
        return false;
    }
    std::snprintf(buffer, size, "%s", value.c_str());
    return true;
}

namespace {

// 外から設定が変わった欄は、編集中でも新しい値を見せる (ImGui は active な欄の内部状態を優先するため捨てる)
void SyncField(char* buffer, size_t size, const std::string& value)
{
    if (SyncTextBuffer(buffer, size, value)) {
        ImGui::ClearActiveID();
    }
}

} // namespace

void GameLiftWindow::UseBackends(IAwsCliRunner& runner, ILocalProcessHost& host)
{
    runner_ = &runner;
    host_ = &host;
}

void GameLiftWindow::Bind(GameLiftSettings& settings, std::function<void()> saveSettings)
{
    settings_ = &settings;
    saveSettings_ = std::move(saveSettings);
    flow_ = std::make_unique<GameLiftFlow>(*runner_, *host_, settings);
    // 手順が設定 (リージョン・フリート ID) を書き換えたら、入力欄へ写して保存する
    flow_->SetSettingsChangedHandler([this]() {
        SyncBuffersFromSettings();
        if (saveSettings_) {
            saveSettings_();
        }
    });
    SyncBuffersFromSettings();
}

void GameLiftWindow::SyncBuffersFromSettings()
{
    if (settings_ == nullptr) {
        return;
    }
    CopyToBuffer(region_, sizeof(region_), settings_->region);
    CopyToBuffer(suffix_, sizeof(suffix_), settings_->locationSuffix);
    CopyToBuffer(fleetName_, sizeof(fleetName_), settings_->anywhereFleetName);
    CopyToBuffer(computeName_, sizeof(computeName_), settings_->computeName);
    CopyToBuffer(ipAddress_, sizeof(ipAddress_), settings_->ipAddress);
}

void GameLiftWindow::Pump(const GameLiftWindowEnv& env)
{
    if (!flow_) {
        return;
    }
    if (computerName_.empty()) {
        wchar_t name[MAX_COMPUTERNAME_LENGTH + 1] = {};
        DWORD length = MAX_COMPUTERNAME_LENGTH + 1;
        if (GetComputerNameW(name, &length)) {
            computerName_.assign(name, length);
        }
    }
    FlowEnvironment environment;
    environment.projectRoot = env.projectRoot;
    environment.exeDir = env.exeDir;
    environment.computerName = computerName_;
    flow_->SetEnvironment(environment);
    flow_->Tick(GetTickCount64());
}

void GameLiftWindow::Shutdown()
{
    if (flow_) {
        flow_->Shutdown(); // Server.exe を停止手順で止める (イベント → 猶予 → 強制終了)
        flow_.reset();
    }
}

void GameLiftWindow::OnImGui(const GameLiftWindowEnv& /*env*/)
{
    if (!open || !flow_) {
        wasOpen_ = false;
        return;
    }
    if (!wasOpen_) {
        ImGui::SetNextWindowFocus();
        // 開いた最初に 1 回確かめる (窓を開くまで aws には触らない)
        if (!flow_->Status().checked && !flow_->Busy()) {
            flow_->RefreshStatus();
        }
    }
    wasOpen_ = true;
    ImGui::SetNextWindowSize(ImVec2(kWindowWidth, kWindowHeight), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(Tr(StrId::Win_GameLift), &open)) {
        ImGui::End();
        return;
    }
    DrawStatus();
    ImGui::Separator();
    // 下段は実行中・取消の通知・エラーがあるときだけ出す (無いときは本体が高さを使う)
    const bool bottomVisible = flow_->Busy() || flow_->WasCancelled() || flow_->LastError().has_value();
    const float bottomHeight = bottomVisible ? kBottomHeight : 0.0f;
    if (ImGui::BeginTabBar("###GLTabs")) {
        if (ImGui::BeginTabItem(Tr(StrId::GameLift_TabAnywhere))) {
            if (ImGui::BeginChild("###GLBody", ImVec2(0.0f, -bottomHeight))) {
                DrawAnywhereTab();
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(Tr(StrId::GameLift_TabEc2))) {
            if (ImGui::BeginChild("###GLBody", ImVec2(0.0f, -bottomHeight))) {
                ImGui::TextUnformatted(Tr(StrId::GameLift_Ec2Soon));
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    if (bottomHeight > 0.0f) {
        if (ImGui::BeginChild("###GLBottom", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders)) {
            DrawBottom();
        }
        ImGui::EndChild();
    }
    DrawConfirmPopup();
    ImGui::End();
}

// 上段: aws CLI の場所と版 / アカウント / リージョン / 再確認 / aws login を開く
void GameLiftWindow::DrawStatus()
{
    const FlowStatus& status = flow_->Status();
    bool cliUsable = false;
    if (status.checking) {
        ImGui::TextUnformatted(Tr(StrId::GameLift_Checking));
    } else if (!status.checked) {
        ImGui::TextUnformatted(Tr(StrId::GameLift_NotChecked));
    } else if (!status.cliFound) {
        ColoredWrapped(themeColor::Error, Tr(StrId::GameLift_CliNotFound));
    } else if (!status.versionKnown) {
        ColoredWrapped(themeColor::Error, Tr(StrId::GameLift_CliVersionUnreadable));
        if (!status.detail.empty()) {
            DimText(status.detail.c_str());
        }
    } else {
        ImGui::Text(Tr(StrId::GameLift_CliLine), WideToUtf8(status.awsPath).c_str(), status.version.text.c_str());
        if (status.version.major < 2) {
            ImGui::PushStyleColor(ImGuiCol_Text, themeColor::Error);
            ImGui::Text(Tr(StrId::GameLift_CliTooOld), status.version.text.c_str());
            ImGui::PopStyleColor();
        } else {
            cliUsable = true;
        }
    }

    if (cliUsable && !status.checking) {
        switch (status.login) {
        case LoginState::LoggedIn:
            ImGui::PushStyleColor(ImGuiCol_Text, themeColor::Success);
            ImGui::Text(Tr(StrId::GameLift_AccountLine), status.identity.account.c_str(), status.identity.arn.c_str());
            ImGui::PopStyleColor();
            break;
        case LoginState::NotLoggedIn:
            ColoredWrapped(themeColor::Warning, Tr(StrId::GameLift_NotLoggedIn));
            break;
        case LoginState::Error:
            ColoredWrapped(themeColor::Error, Tr(StrId::GameLift_LoginError));
            if (!status.detail.empty()) {
                DimText(status.detail.c_str());
            }
            break;
        case LoginState::Unknown:
            break;
        }
    }

    SyncField(region_, sizeof(region_), settings_->region);
    ImGui::SetNextItemWidth(kInputWidth * 0.6f);
    if (ImGui::InputText(Tr(StrId::GameLift_Region), region_, sizeof(region_))) {
        settings_->region = region_;
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && saveSettings_) {
        saveSettings_();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(flow_->Busy());
    if (ImGui::Button(Tr(StrId::GameLift_Recheck))) {
        flow_->RefreshStatus();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!cliUsable);
    if (ImGui::Button(Tr(StrId::GameLift_OpenLogin))) {
        loginOpenError_.clear();
        OpenAwsLoginConsole(status.awsPath, loginOpenError_);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", Tr(StrId::GameLift_OpenLoginTip));
    }
    if (!loginOpenError_.empty()) {
        ColoredWrapped(themeColor::Error, (std::string(Tr(StrId::GameLift_OpenLoginFailed)) + " " + loginOpenError_).c_str());
    }
}

void GameLiftWindow::DrawStepRow(FlowAction action, const char* buttonLabel)
{
    const ActionState state = flow_->Query(action);
    const StepState step = flow_->Step(action);
    ImGui::PushStyleColor(ImGuiCol_Text, StepColor(step));
    ImGui::Text("[%s]", StepWord(step));
    ImGui::PopStyleColor();
    ImGui::SameLine(kStateColumnX);
    ImGui::BeginDisabled(!state.enabled);
    if (ImGui::Button(buttonLabel)) {
        flow_->Request(action);
    }
    ImGui::EndDisabled();
    // 理由は「別の手順の実行中」以外を出す (実行中は全ボタンが無効になるので、毎行に出すと騒がしい)
    if (!state.enabled && state.reason != BlockReason::Busy) {
        ImGui::SameLine();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextUnformatted(ReasonText(state.reason));
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
    }
}

void GameLiftWindow::DrawAnywhereTab()
{
    bool edited = false;
    const auto track = [&edited]() { edited = edited || ImGui::IsItemDeactivatedAfterEdit(); };
    const float secondColumn = ImGui::GetContentRegionAvail().x * 0.5f;

    SyncField(suffix_, sizeof(suffix_), settings_->locationSuffix);
    SyncField(fleetName_, sizeof(fleetName_), settings_->anywhereFleetName);
    SyncField(computeName_, sizeof(computeName_), settings_->computeName);
    SyncField(ipAddress_, sizeof(ipAddress_), settings_->ipAddress);
    // 入力欄は 2 列 x 3 行 (縦を食わないため)
    ImGui::TextDisabled("custom-");
    ImGui::SameLine(0.0f, 2.0f);
    ImGui::SetNextItemWidth(kFieldWidth - 52.0f);
    if (ImGui::InputText(Tr(StrId::GameLift_FieldSuffix), suffix_, sizeof(suffix_))) {
        settings_->locationSuffix = suffix_;
    }
    track();
    ImGui::SameLine(secondColumn);
    ImGui::SetNextItemWidth(kFieldWidth);
    if (ImGui::InputText(Tr(StrId::GameLift_FieldFleetName), fleetName_, sizeof(fleetName_))) {
        settings_->anywhereFleetName = fleetName_;
    }
    track();

    ImGui::SetNextItemWidth(kFieldWidth);
    if (ImGui::InputTextWithHint(Tr(StrId::GameLift_FieldCompute), flow_->EffectiveComputeName().c_str(), computeName_,
                                 sizeof(computeName_))) {
        settings_->computeName = computeName_;
    }
    track();
    ImGui::SameLine(secondColumn);
    ImGui::SetNextItemWidth(kFieldWidth);
    if (ImGui::InputText(Tr(StrId::GameLift_FieldIp), ipAddress_, sizeof(ipAddress_))) {
        settings_->ipAddress = ipAddress_;
    }
    track();

    ImGui::SetNextItemWidth(kFieldWidth * 0.6f);
    ImGui::InputInt(Tr(StrId::GameLift_FieldPort), &settings_->port, 0, 0);
    track();
    ImGui::SameLine(secondColumn);
    ImGui::SetNextItemWidth(kFieldWidth * 0.6f);
    ImGui::InputInt(Tr(StrId::GameLift_FieldMaxPlayers), &settings_->maxPlayers, 0, 0);
    track();
    if (edited && saveSettings_) {
        saveSettings_();
    }

    ImGui::Separator();
    DrawStepRow(FlowAction::Prepare, Tr(StrId::GameLift_BtnPrepare));
    DrawStepRow(FlowAction::StartServer, Tr(StrId::GameLift_BtnStart));
    DrawStepRow(FlowAction::Join, Tr(StrId::GameLift_BtnJoin));
    DrawStepRow(FlowAction::StopServer, Tr(StrId::GameLift_BtnStop));
    DrawStepRow(FlowAction::Cleanup, Tr(StrId::GameLift_BtnCleanup));
    ImGui::Separator();
    DrawServerBlock();
}

void GameLiftWindow::DrawServerBlock()
{
    const ServerView& server = flow_->Server();
    char phaseText[160] = {};
    switch (server.phase) {
    case ServerPhase::NotStarted: std::snprintf(phaseText, sizeof(phaseText), "%s", Tr(StrId::GameLift_SrvStopped)); break;
    case ServerPhase::Starting: std::snprintf(phaseText, sizeof(phaseText), "%s", Tr(StrId::GameLift_SrvStarting)); break;
    case ServerPhase::Ready: std::snprintf(phaseText, sizeof(phaseText), "%s", Tr(StrId::GameLift_SrvReady)); break;
    case ServerPhase::Stopping: std::snprintf(phaseText, sizeof(phaseText), "%s", Tr(StrId::GameLift_SrvStopping)); break;
    case ServerPhase::Exited:
        std::snprintf(phaseText, sizeof(phaseText), Tr(StrId::GameLift_SrvExited), static_cast<int>(server.exitCode));
        break;
    }
    const ImVec4 phaseColor = server.phase == ServerPhase::Ready ? themeColor::Success
                              : (server.phase == ServerPhase::Exited && (server.exitCode != 0 || server.forcedKill))
                                  ? themeColor::Error
                                  : themeColor::Text;
    ImGui::PushStyleColor(ImGuiCol_Text, phaseColor);
    ImGui::Text(Tr(StrId::GameLift_ServerLine), phaseText);
    ImGui::PopStyleColor();
    if (server.phase == ServerPhase::Exited && server.forcedKill) {
        ColoredWrapped(themeColor::Warning, Tr(StrId::GameLift_SrvForced));
    }
    if (server.Active()) {
        ImGui::Text(Tr(StrId::GameLift_ClientsLine), flow_->ClientsLaunched(), settings_->maxPlayers);
    }
    if (!server.logPath.empty()) {
        ImGui::TextDisabled(Tr(StrId::GameLift_LogFile), WideToUtf8(server.logPath).c_str());
    }
    if (!server.logTail.empty()) {
        ImGui::TextUnformatted(Tr(StrId::GameLift_LogHeader));
        if (ImGui::BeginChild("###GLLog", ImVec2(0.0f, kLogHeight), ImGuiChildFlags_Borders)) {
            for (const std::string& line : server.logTail) {
                ImGui::TextUnformatted(line.c_str());
            }
        }
        ImGui::EndChild();
    }
}

// 下段: 実行中の CLI と取消 / 待ちの進捗 / 直近のエラー
void GameLiftWindow::DrawBottom()
{
    const FlowProgress progress = flow_->Progress();
    if (progress.op != FlowOp::None) {
        if (progress.cliRunning) {
            ImGui::Text(Tr(StrId::GameLift_Running), WideToUtf8(progress.cliLabel).c_str(),
                        static_cast<int>(progress.cliElapsedMs / 1000));
        } else {
            switch (progress.wait) {
            case WaitKind::CapacityRetry: ImGui::Text(Tr(StrId::GameLift_WaitCapacity), progress.attempt, progress.maxAttempts); break;
            case WaitKind::SessionActive: ImGui::Text(Tr(StrId::GameLift_WaitSession), progress.attempt, progress.maxAttempts); break;
            case WaitKind::FleetGone: ImGui::Text(Tr(StrId::GameLift_WaitFleet), progress.attempt, progress.maxAttempts); break;
            case WaitKind::ServerStop: ImGui::TextUnformatted(Tr(StrId::GameLift_WaitServerStop)); break;
            case WaitKind::None: ImGui::TextUnformatted(Tr(StrId::GameLift_Checking)); break;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(Tr(StrId::GameLift_Cancel))) {
            flow_->Cancel();
        }
        if (progress.op != FlowOp::Check) {
            DimText(Tr(StrId::GameLift_CancelNote));
        }
    } else if (flow_->WasCancelled()) {
        ColoredWrapped(themeColor::Warning, Tr(StrId::GameLift_Cancelled));
    }

    if (flow_->LastError().has_value()) {
        const FlowError& error = *flow_->LastError();
        ColoredWrapped(themeColor::Error, Tr(StrId::GameLift_LastError));
        ColoredWrapped(themeColor::Error, FailureText(error));
        std::string detail = error.code;
        if (!error.detail.empty()) {
            detail += (detail.empty() ? "" : "\n") + error.detail;
        }
        if (!detail.empty()) {
            DimText(detail.c_str());
        }
    }
}

// 作成・削除の確認 (モーダル)。既定のフォーカスは「やめる」
void GameLiftWindow::DrawConfirmPopup()
{
    const std::optional<ConfirmInfo> pending = flow_->PendingConfirm();
    if (pending.has_value()) {
        confirmIsPrepare_ = pending->action == FlowAction::Prepare;
    }
    const bool isPrepare = confirmIsPrepare_; // 閉じる途中のフレームでも開いたときと同じ ID で Begin する
    const char* popupId = isPrepare ? kConfirmPrepareId : kConfirmCleanupId;
    if (pending.has_value() && !ImGui::IsPopupOpen(popupId)) {
        ImGui::OpenPopup(popupId);
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    const char* title = isPrepare ? Tr(StrId::GameLift_ConfirmPrepareTitle) : Tr(StrId::GameLift_ConfirmCleanupTitle);
    if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    if (!pending.has_value()) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34.0f);
    if (isPrepare) {
        ImGui::Text(Tr(StrId::GameLift_ConfirmPrepareIntro), pending->region.c_str());
        ImGui::Indent();
        ImGui::Text(Tr(StrId::GameLift_ConfirmLocation), pending->locationName.c_str());
        ImGui::Text(Tr(StrId::GameLift_ConfirmFleet), pending->fleetName.c_str());
        ImGui::Text(Tr(StrId::GameLift_ConfirmCompute), pending->computeName.c_str(), pending->ipAddress.c_str());
        ImGui::Unindent();
        ImGui::Spacing();
        DimText(Tr(StrId::GameLift_ConfirmPrepareNote));
    } else {
        ImGui::Text(Tr(StrId::GameLift_ConfirmCleanupIntro), pending->region.c_str());
        ImGui::Indent();
        ImGui::Text(Tr(StrId::GameLift_ConfirmCompute), pending->computeName.c_str(), pending->ipAddress.c_str());
        ImGui::Text(Tr(StrId::GameLift_ConfirmFleet),
                    pending->fleetId.empty() ? Tr(StrId::GameLift_ConfirmFleetNone) : pending->fleetId.c_str());
        ImGui::Text(Tr(StrId::GameLift_ConfirmLocation), pending->locationName.c_str());
        ImGui::Unindent();
        ImGui::Spacing();
        DimText(Tr(StrId::GameLift_ConfirmCleanupNote));
    }
    ImGui::PopTextWrapPos();
    ImGui::Separator();

    const char* runLabel = isPrepare ? Tr(StrId::GameLift_ConfirmCreate) : Tr(StrId::GameLift_ConfirmDelete);
    if (ImGui::Button(runLabel)) {
        flow_->Confirm();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    const bool abort = ImGui::Button(Tr(StrId::GameLift_ConfirmAbort));
    ImGui::SetItemDefaultFocus(); // 既定は「やめる」(Enter で誤って作成・削除しない)
    if (abort || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        flow_->Decline();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace mye
