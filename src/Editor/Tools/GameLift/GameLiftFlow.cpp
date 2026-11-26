//====================================================================================
//                          GameLiftFlow.cpp
//  MyEngin/ 秋田蓮音                                                       11/27/2026
//                                          GameLift 窓の手順の状態機械の実装 (状態確認と Anywhere の準備・起動・参加・片付け)
//====================================================================================
#include "Editor/Tools/GameLift/GameLiftFlow.h"

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <filesystem>

#include <Windows.h>

#include "Editor/Tools/GameLift/AwsCliCommands.h"
#include "Engine/Core/Diagnostics/Log.h"

namespace fs = std::filesystem;

namespace mye {
namespace {

constexpr char kStatusActive[] = "ACTIVE";
constexpr char kStatusActivating[] = "ACTIVATING";
constexpr char kStatusDeleting[] = "DELETING";
constexpr int kErrorDetailLines = 3;
constexpr wchar_t kServerExeName[] = L"Server.exe";
constexpr wchar_t kRuntimeExeName[] = L"Runtime.exe";

std::wstring Widen(const std::string& ascii)
{
    return std::wstring(ascii.begin(), ascii.end());
}

bool FileExists(const fs::path& path)
{
    std::error_code ec;
    return fs::is_regular_file(path, ec);
}

// 秘密を含みうる文字列をゼロで埋める
void WipeString(std::string& text)
{
    if (!text.empty()) {
        SecureZeroMemory(text.data(), text.size());
    }
    text.clear();
}

std::string PlayerIdStamp()
{
    SYSTEMTIME t{};
    GetSystemTime(&t);
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%04u%02u%02u%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                  t.wSecond);
    return buffer;
}

// 入力を検査して AwsCall を組む。組めなければ nullopt (Issue が InvalidInput の失敗にする)
template <class Build>
std::optional<AwsCall> MakeCall(Build&& build)
{
    AwsCall call;
    if (!build(call)) {
        return std::nullopt;
    }
    return call;
}

int ActionIndex(FlowAction action)
{
    return static_cast<int>(action);
}

FlowAction ActionOf(FlowOp op)
{
    switch (op) {
    case FlowOp::StartServer: return FlowAction::StartServer;
    case FlowOp::Join: return FlowAction::Join;
    case FlowOp::Cleanup: return FlowAction::Cleanup;
    default: return FlowAction::Prepare;
    }
}

} // namespace

ServerLaunchSpec BuildAnywhereServerLaunch(const AnywhereServerLaunchInput& input)
{
    ServerLaunchSpec spec;
    spec.exePath = (fs::path(input.exeDir) / kServerExeName).wstring();
    spec.args = {
        L"--project", input.projectRoot,
        L"--hosting", L"gamelift",
        L"--gamelift-ws-url", Widen(input.wsUrl),
        L"--gamelift-fleet-id", Widen(input.fleetId),
        L"--gamelift-host-id", Widen(input.computeName),
        L"--port", std::to_wstring(input.port),
        L"--max-players", std::to_wstring(input.maxPlayers),
        L"--stop-event", input.stopEventName,
    };
    spec.extraEnv = { { kServerTokenEnvName, Widen(input.authToken) } };
    spec.workDir = input.projectRoot;
    spec.logPath = input.logPath;
    spec.stopEventName = input.stopEventName;
    return spec;
}

ClientLaunchSpec BuildAnywhereClientLaunch(const std::wstring& exeDir, const std::wstring& projectRoot,
                                           const std::string& host, int port, const std::string& playerSessionId)
{
    ClientLaunchSpec spec;
    spec.exePath = (fs::path(exeDir) / kRuntimeExeName).wstring();
    spec.args = {
        L"--project", projectRoot,
        L"--net-connect", Widen(host) + L":" + std::to_wstring(port),
        L"--player-session-id", Widen(playerSessionId),
    };
    spec.workDir = projectRoot;
    return spec;
}

std::wstring ProjectGameLogicPath(const std::wstring& projectRoot)
{
    return (fs::path(projectRoot) / L"cache" / L"GameLogic.dll").wstring();
}

std::wstring ServerLogPathFor(const std::wstring& projectRoot, const std::wstring& stamp)
{
    return (fs::path(projectRoot) / L"cache" / L"gamelift" / (L"server-" + stamp + L".log")).wstring();
}

GameLiftFlow::GameLiftFlow(IAwsCliRunner& runner, ILocalProcessHost& host, GameLiftSettings& settings,
                           FlowTimings timings)
    : runner_(runner), host_(host), settings_(settings), timings_(timings)
{
    playerIdPrefix_ = "mye-" + PlayerIdStamp() + "-";
}

std::string GameLiftFlow::EffectiveComputeName() const
{
    return settings_.computeName.empty() ? DefaultComputeName(environment_.computerName) : settings_.computeName;
}

// ---------------------------------------------------------------------------------
// 問い合わせ
// ---------------------------------------------------------------------------------

BlockReason GameLiftFlow::ValidateInputs(FlowAction action) const
{
    if (settings_.region.empty()) {
        return BlockReason::NoRegion;
    }
    if (!IsValidRegion(settings_.region)) {
        return BlockReason::InvalidRegion;
    }
    const bool needSuffix = action == FlowAction::Prepare || action == FlowAction::Join || action == FlowAction::Cleanup;
    const bool needCompute = action == FlowAction::Prepare || action == FlowAction::StartServer ||
                             (action == FlowAction::Cleanup && !settings_.anywhereFleetId.empty());
    if (needSuffix && !IsValidLocationSuffix(settings_.locationSuffix)) {
        return BlockReason::InvalidSuffix;
    }
    if (action == FlowAction::Prepare && !IsValidFleetName(settings_.anywhereFleetName)) {
        return BlockReason::InvalidFleetName;
    }
    if (needCompute && !IsValidComputeName(EffectiveComputeName())) {
        return BlockReason::InvalidComputeName;
    }
    if (action == FlowAction::Prepare && !IsValidIpv4(settings_.ipAddress)) {
        return BlockReason::InvalidIp;
    }
    if (action == FlowAction::StartServer && !IsValidPort(settings_.port)) {
        return BlockReason::InvalidPort;
    }
    if ((action == FlowAction::StartServer || action == FlowAction::Join) && !IsValidMaxPlayers(settings_.maxPlayers)) {
        return BlockReason::InvalidMaxPlayers;
    }
    return BlockReason::None;
}

ActionState GameLiftFlow::Query(FlowAction action) const
{
    const auto blocked = [](BlockReason reason) { return ActionState{ false, reason }; };

    // 停止は CLI を使わない。サーバが動いていれば、CLI が走っている間も押せる (片付けは自分で止めるので除く)
    if (action == FlowAction::StopServer) {
        if (op_ == FlowOp::Cleanup) {
            return blocked(BlockReason::Busy);
        }
        const bool running = server_.phase == ServerPhase::Starting || server_.phase == ServerPhase::Ready;
        return running ? ActionState{ true, BlockReason::None } : blocked(BlockReason::ServerNotRunning);
    }

    if (op_ != FlowOp::None) {
        return blocked(BlockReason::Busy);
    }
    if (!status_.checked) {
        return blocked(BlockReason::StatusUnknown);
    }
    if (!status_.cliFound || !status_.versionKnown) {
        return blocked(BlockReason::CliMissing);
    }
    if (status_.version.major < 2) {
        return blocked(BlockReason::CliTooOld);
    }
    if (status_.login != LoginState::LoggedIn) {
        return blocked(BlockReason::NotLoggedIn);
    }
    if (const BlockReason invalid = ValidateInputs(action); invalid != BlockReason::None) {
        return blocked(invalid);
    }

    const bool needsProject = action == FlowAction::StartServer || action == FlowAction::Join;
    if (needsProject && environment_.projectRoot.empty()) {
        return blocked(BlockReason::NoProject);
    }
    switch (action) {
    case FlowAction::StartServer:
        if (!FileExists(ProjectGameLogicPath(environment_.projectRoot))) {
            return blocked(BlockReason::NoGameLogic);
        }
        if (!FileExists(fs::path(environment_.exeDir) / kServerExeName)) {
            return blocked(BlockReason::NoServerExe);
        }
        if (settings_.anywhereFleetId.empty()) {
            return blocked(BlockReason::NotPrepared);
        }
        if (server_.Active()) {
            return blocked(BlockReason::ServerAlreadyRunning);
        }
        break;
    case FlowAction::Join:
        if (!FileExists(fs::path(environment_.exeDir) / kRuntimeExeName)) {
            return blocked(BlockReason::NoRuntimeExe);
        }
        if (settings_.anywhereFleetId.empty()) {
            return blocked(BlockReason::NotPrepared);
        }
        if (!server_.Active()) {
            return blocked(BlockReason::ServerNotRunning);
        }
        if (server_.phase != ServerPhase::Ready) {
            return blocked(BlockReason::ServerNotReady);
        }
        if (clientsLaunched_ >= settings_.maxPlayers) {
            return blocked(BlockReason::ClientLimit);
        }
        break;
    default:
        break;
    }
    return ActionState{ true, BlockReason::None };
}

StepState GameLiftFlow::Step(FlowAction action) const
{
    const int index = ActionIndex(action);
    switch (action) {
    case FlowAction::Prepare:
        if (op_ == FlowOp::Prepare) {
            return StepState::Running;
        }
        if (outcome_[index] == Outcome::Failed) {
            return StepState::Failed;
        }
        return settings_.anywhereFleetId.empty() ? StepState::NotDone : StepState::Done;
    case FlowAction::StartServer:
        if (op_ == FlowOp::StartServer || server_.phase == ServerPhase::Starting) {
            return StepState::Running;
        }
        if (server_.phase == ServerPhase::Ready || server_.phase == ServerPhase::Stopping) {
            return StepState::Done;
        }
        if (server_.phase == ServerPhase::Exited && (server_.exitCode != 0 || server_.forcedKill)) {
            return StepState::Failed;
        }
        return outcome_[index] == Outcome::Failed ? StepState::Failed : StepState::NotDone;
    case FlowAction::Join:
        if (op_ == FlowOp::Join) {
            return StepState::Running;
        }
        if (outcome_[index] == Outcome::Failed) {
            return StepState::Failed;
        }
        return (clientsLaunched_ > 0 && server_.Active()) ? StepState::Done : StepState::NotDone;
    case FlowAction::StopServer:
        if (server_.phase == ServerPhase::Stopping) {
            return StepState::Running;
        }
        return (server_.phase == ServerPhase::Exited && server_.exitCode == 0 && !server_.forcedKill)
                   ? StepState::Done
                   : StepState::NotDone;
    case FlowAction::Cleanup:
        if (op_ == FlowOp::Cleanup) {
            return StepState::Running;
        }
        if (outcome_[index] == Outcome::Failed) {
            return StepState::Failed;
        }
        return outcome_[index] == Outcome::Succeeded ? StepState::Done : StepState::NotDone;
    }
    return StepState::NotDone;
}

FlowProgress GameLiftFlow::Progress() const
{
    FlowProgress progress;
    progress.op = op_;
    progress.cliRunning = calling_;
    progress.cliLabel = currentSecret_ ? std::wstring() : currentLabel_;
    progress.cliElapsedMs = calling_ ? runner_.ElapsedMs() : 0;
    progress.wait = waitKind_;
    progress.attempt = attempt_;
    progress.maxAttempts = maxAttempts_;
    return progress;
}

// ---------------------------------------------------------------------------------
// ボタン
// ---------------------------------------------------------------------------------

bool GameLiftFlow::Request(FlowAction action)
{
    if (!Query(action).enabled) {
        return false;
    }
    if (action == FlowAction::Prepare || action == FlowAction::Cleanup) {
        pending_ = action; // 作成・削除は確認ダイアログを経てから (Confirm)
        return true;
    }
    return Begin(action);
}

std::optional<ConfirmInfo> GameLiftFlow::PendingConfirm() const
{
    if (!pending_.has_value()) {
        return std::nullopt;
    }
    ConfirmInfo info;
    info.action = *pending_;
    info.region = settings_.region;
    info.locationName = LocationName(settings_.locationSuffix);
    info.fleetName = settings_.anywhereFleetName;
    info.computeName = EffectiveComputeName();
    info.ipAddress = settings_.ipAddress;
    info.fleetId = settings_.anywhereFleetId;
    return info;
}

bool GameLiftFlow::Confirm()
{
    if (!pending_.has_value()) {
        return false;
    }
    const FlowAction action = *pending_;
    pending_.reset();
    if (!Query(action).enabled) {
        return false; // 確認している間に状態が変わった
    }
    return Begin(action);
}

void GameLiftFlow::Decline()
{
    pending_.reset();
}

bool GameLiftFlow::Begin(FlowAction action)
{
    if (action == FlowAction::StopServer) {
        host_.RequestServerStop(timings_.serverStopGraceMs); // CLI を使わない。直近の CLI のエラー表示は消さない
        UpdateServerView(); // 次の Tick を待たずに「停止処理中」へ (その間に二重に押せないように)
        return true;
    }
    lastError_.reset();
    cancelledNotice_ = false;
    outcome_[ActionIndex(action)] = Outcome::None;

    attempt_ = maxAttempts_ = 0;
    switch (action) {
    case FlowAction::Prepare:
        op_ = FlowOp::Prepare;
        Issue(MakeCall([&](AwsCall& c) { return GameLiftListCustomLocationsCall(settings_.region, c); }),
              Stage::PrepListLocations);
        break;
    case FlowAction::StartServer:
        op_ = FlowOp::StartServer;
        Issue(MakeCall([&](AwsCall& c) {
                  return GameLiftDescribeComputeCall(settings_.region, settings_.anywhereFleetId, EffectiveComputeName(), c);
              }),
              Stage::StartDescribeCompute);
        break;
    case FlowAction::Join:
        op_ = FlowOp::Join;
        Issue(MakeCall([&](AwsCall& c) {
                  DescribeGameSessionsQuery query;
                  query.fleetId = settings_.anywhereFleetId;
                  query.locationSuffix = settings_.locationSuffix;
                  query.activeOnly = true;
                  return GameLiftDescribeGameSessionsCall(settings_.region, query, c);
              }),
              Stage::JoinListActive);
        break;
    case FlowAction::Cleanup:
        op_ = FlowOp::Cleanup;
        if (server_.Active()) {
            host_.RequestServerStop(timings_.serverStopGraceMs);
            waitKind_ = WaitKind::ServerStop;
            stage_ = Stage::CleanStopServer;
        } else {
            CleanupAfterServer();
        }
        break;
    case FlowAction::StopServer:
        break;
    }
    return true;
}

void GameLiftFlow::RefreshStatus()
{
    if (Busy()) {
        return;
    }
    status_ = FlowStatus();
    status_.checking = true;
    op_ = FlowOp::Check;
    attempt_ = maxAttempts_ = 0;
    std::wstring awsPath;
    if (!runner_.Locate(awsPath)) {
        FinishOp(Outcome::Succeeded); // 見つからないことが確認結果 (cliFound = false)
        return;
    }
    status_.cliFound = true;
    status_.awsPath = awsPath;
    Issue(AwsVersionCall(), Stage::CheckVersion);
}

void GameLiftFlow::Cancel()
{
    if (op_ == FlowOp::None) {
        return;
    }
    if (calling_) {
        runner_.Cancel(); // 結果の確定は Tick (Cancelled)
        return;
    }
    FinishOp(Outcome::Cancelled);
}

void GameLiftFlow::Shutdown()
{
    pending_.reset();
    if (calling_) {
        runner_.Cancel();
        calling_ = false;
    }
    op_ = FlowOp::None;
    stage_ = Stage::Idle;
    waitKind_ = WaitKind::None;
    host_.StopServerBlocking(timings_.serverStopGraceMs);
    UpdateServerView();
}

// ---------------------------------------------------------------------------------
// 進行
// ---------------------------------------------------------------------------------

void GameLiftFlow::Tick(uint64_t nowMs)
{
    now_ = nowMs;
    host_.PollServer();
    UpdateServerView();

    if (calling_) {
        const CliRunState state = runner_.Poll();
        if (state != CliRunState::Running) {
            calling_ = false;
            OnCallFinished(state);
        }
        return;
    }
    if (op_ == FlowOp::None || waitKind_ == WaitKind::None) {
        return;
    }
    if (waitKind_ == WaitKind::ServerStop) {
        if (!server_.Active()) {
            waitKind_ = WaitKind::None;
            CleanupAfterServer();
        }
        return;
    }
    if (now_ >= waitUntil_) {
        waitKind_ = WaitKind::None;
        OnTimer();
    }
}

void GameLiftFlow::UpdateServerView()
{
    const ServerPhase before = server_.phase;
    server_ = host_.ServerStatus();
    if (before != ServerPhase::Exited && server_.phase == ServerPhase::Exited) {
        clientsLaunched_ = 0; // 次に起動するサーバは新しいセッション
    }
}

void GameLiftFlow::ChangedSettings()
{
    if (onSettingsChanged_) {
        onSettingsChanged_();
    }
}

bool GameLiftFlow::Issue(const std::optional<AwsCall>& call, Stage next)
{
    if (!call.has_value()) {
        FailWith(FlowFailure::InvalidInput, "an input value is not valid");
        return false;
    }
    std::string error;
    if (!runner_.Start(*call, timings_.cliTimeoutMs, error)) {
        FailWith(FlowFailure::CliStartFailed, error);
        return false;
    }
    calling_ = true;
    stage_ = next;
    currentLabel_ = call->label;
    currentSecret_ = call->secret;
    return true;
}

void GameLiftFlow::WaitFor(uint32_t ms, WaitKind kind, Stage next)
{
    waitKind_ = kind;
    waitUntil_ = now_ + ms;
    stage_ = next;
}

void GameLiftFlow::FinishOp(Outcome outcome)
{
    if (op_ == FlowOp::Check) {
        status_.checking = false;
        status_.checked = true;
    } else if (op_ != FlowOp::None) {
        outcome_[ActionIndex(ActionOf(op_))] = outcome;
        cancelledNotice_ = outcome == Outcome::Cancelled;
    }
    op_ = FlowOp::None;
    stage_ = Stage::Idle;
    waitKind_ = WaitKind::None;
    calling_ = false;
    attempt_ = maxAttempts_ = 0;
}

void GameLiftFlow::FailWith(FlowFailure failure, const std::string& detail, const CallResult* result)
{
    FlowError error;
    error.failure = failure;
    error.action = ActionOf(op_);
    error.detail = detail;
    if (result != nullptr) {
        error.awsKind = result->aws.kind;
        error.code = result->aws.code;
        if (result->aws.kind == AwsErrorKind::Credentials) {
            status_.login = LoginState::NotLoggedIn;
        }
    }
    MYE_LOG_WARN("[gamelift] operation failed (%d): %s", static_cast<int>(failure), detail.c_str());
    lastError_ = std::move(error);
    FinishOp(Outcome::Failed);
}

void GameLiftFlow::OnCallFinished(CliRunState state)
{
    const bool secret = currentSecret_;
    CallResult result;
    result.exitCode = runner_.ExitCode();
    result.out = runner_.StdoutText();
    result.err = runner_.StderrText();
    result.aws = ClassifyAwsFailure(static_cast<int>(result.exitCode), result.err);
    if (secret) {
        runner_.ClearOutput();
    }

    if (state == CliRunState::Cancelled) {
        FinishOp(Outcome::Cancelled);
    } else if (state == CliRunState::TimedOut) {
        FailWith(FlowFailure::CliTimedOut, std::string());
    } else {
        switch (op_) {
        case FlowOp::Check: HandleCheck(result); break;
        case FlowOp::Prepare: HandlePrepare(result); break;
        case FlowOp::StartServer: HandleStartServer(result); break;
        case FlowOp::Join: HandleJoin(result); break;
        case FlowOp::Cleanup: HandleCleanup(result); break;
        case FlowOp::None: break;
        }
    }
    if (secret) {
        WipeString(result.out);
        WipeString(result.err);
    }
}

void GameLiftFlow::OnTimer()
{
    switch (stage_) {
    case Stage::JoinCreateRetryWait: IssueCreateGameSession(); break;
    case Stage::JoinSessionPollWait: IssueDescribeSession(); break;
    case Stage::CleanFleetPollWait: IssueDescribeFleetGone(); break;
    default: break;
    }
}

// ---------------------------------------------------------------------------------
// 状態確認 (aws --version → リージョン → sts get-caller-identity)
// ---------------------------------------------------------------------------------

void GameLiftFlow::HandleCheck(const CallResult& r)
{
    const auto issueIdentity = [&]() {
        const std::string region = IsValidRegion(settings_.region) ? settings_.region : std::string();
        Issue(MakeCall([&](AwsCall& c) { return AwsStsGetCallerIdentityCall(region, c); }), Stage::CheckIdentity);
    };

    switch (stage_) {
    case Stage::CheckVersion: {
        // v2 は stdout、v1 は stderr に版を出す
        const std::string text = r.out + "\n" + r.err;
        status_.versionKnown = r.exitCode == 0 && ParseAwsCliVersion(text, status_.version);
        if (!status_.versionKnown) {
            status_.detail = FirstErrorLines(r.err, kErrorDetailLines);
            FinishOp(Outcome::Succeeded);
            return;
        }
        if (status_.version.major < 2) {
            FinishOp(Outcome::Succeeded);
            return;
        }
        if (settings_.region.empty()) {
            Issue(AwsConfigureGetRegionCall(), Stage::CheckRegion);
        } else {
            issueIdentity();
        }
        return;
    }
    case Stage::CheckRegion: {
        std::string region;
        if (r.exitCode == 0 && ParseSingleLineText(r.out, region) && IsValidRegion(region)) {
            settings_.region = region;
            ChangedSettings();
        }
        issueIdentity();
        return;
    }
    case Stage::CheckIdentity:
        if (r.aws.kind == AwsErrorKind::None) {
            std::string error;
            if (ParseCallerIdentity(r.out, status_.identity, error)) {
                status_.login = LoginState::LoggedIn;
            } else {
                status_.login = LoginState::Error;
                status_.detail = error;
            }
        } else if (r.aws.kind == AwsErrorKind::Credentials) {
            status_.login = LoginState::NotLoggedIn;
        } else {
            status_.login = LoginState::Error;
            status_.detail = FirstErrorLines(r.err, kErrorDetailLines);
        }
        FinishOp(Outcome::Succeeded);
        return;
    default:
        FinishOp(Outcome::Failed);
        return;
    }
}

// ---------------------------------------------------------------------------------
// 準備: location → fleet → compute の順に、無いものだけ作る
// ---------------------------------------------------------------------------------

void GameLiftFlow::PrepareNextFleet()
{
    if (settings_.anywhereFleetId.empty()) {
        Issue(MakeCall([&](AwsCall& c) {
                  return GameLiftCreateAnywhereFleetCall(settings_.region, settings_.anywhereFleetName,
                                                         settings_.locationSuffix, c);
              }),
              Stage::PrepCreateFleet);
    } else {
        Issue(MakeCall([&](AwsCall& c) {
                  return GameLiftDescribeFleetAttributesCall(settings_.region, settings_.anywhereFleetId, c);
              }),
              Stage::PrepDescribeFleet);
    }
}

void GameLiftFlow::PrepareNextCompute()
{
    Issue(MakeCall([&](AwsCall& c) {
              return GameLiftDescribeComputeCall(settings_.region, settings_.anywhereFleetId, EffectiveComputeName(), c);
          }),
          Stage::PrepDescribeCompute);
}

void GameLiftFlow::HandlePrepare(const CallResult& r)
{
    std::string parseError;
    switch (stage_) {
    case Stage::PrepListLocations: {
        if (r.aws.kind != AwsErrorKind::None) {
            return FailWith(FlowFailure::Aws, FirstErrorLines(r.err, kErrorDetailLines), &r);
        }
        std::vector<AwsLocation> locations;
        if (!ParseListLocations(r.out, locations, parseError)) {
            return FailWith(FlowFailure::BadResponse, parseError);
        }
        const std::string wanted = LocationName(settings_.locationSuffix);
        const bool exists = std::any_of(locations.begin(), locations.end(),
                                        [&](const AwsLocation& l) { return l.name == wanted; });
        if (exists) {
            PrepareNextFleet();
        } else {
            Issue(MakeCall([&](AwsCall& c) {
                      return GameLiftCreateLocationCall(settings_.region, settings_.locationSuffix, c);
                  }),
                  Stage::PrepCreateLocation);
        }
        return;
    }
    case Stage::PrepCreateLocation:
        if (r.aws.kind != AwsErrorKind::None && r.aws.kind != AwsErrorKind::Conflict) {
            return FailWith(FlowFailure::Aws, FirstErrorLines(r.err, kErrorDetailLines), &r); // 既にあるのは成功扱い
        }
        PrepareNextFleet();
        return;
    case Stage::PrepDescribeFleet: {
        bool exists = false;
        if (r.aws.kind == AwsErrorKind::None) {
            std::vector<AwsFleetAttributes> fleets;
            if (!ParseDescribeFleetAttributes(r.out, fleets, parseError)) {
                return FailWith(FlowFailure::BadResponse, parseError);
            }
            exists = !fleets.empty() && fleets.front().status != kStatusDeleting;
        } else if (r.aws.kind != AwsErrorKind::NotFound) {
            return FailWith(FlowFailure::Aws, FirstErrorLines(r.err, kErrorDetailLines), &r);
        }
        if (exists) {
            PrepareNextCompute();
        } else {
            settings_.anywhereFleetId.clear(); // 保存済みの ID は消えていた
            ChangedSettings();
            PrepareNextFleet();
        }
        return;
    }
    case Stage::PrepCreateFleet: {
        if (r.aws.kind != AwsErrorKind::None) {
            return FailWith(FlowFailure::Aws, FirstErrorLines(r.err, kErrorDetailLines), &r);
        }
        AwsFleetAttributes fleet;
        if (!ParseCreateFleet(r.out, fleet, parseError) || !IsValidResourceId(fleet.fleetId)) {
            return FailWith(FlowFailure::BadResponse, parseError);
        }
        settings_.anywhereFleetId = fleet.fleetId;
        ChangedSettings();
        PrepareNextCompute();
        return;
    }
    case Stage::PrepDescribeCompute:
        if (r.aws.kind == AwsErrorKind::None) {
            return FinishOp(Outcome::Succeeded); // コンピュートも登録済み
        }
        if (r.aws.kind != AwsErrorKind::NotFound) {
            return FailWith(FlowFailure::Aws, FirstErrorLines(r.err, kErrorDetailLines), &r);
        }
        Issue(MakeCall([&](AwsCall& c) {
                  return GameLiftRegisterComputeCall(settings_.region, settings_.anywhereFleetId, EffectiveComputeName(),
                                                     settings_.ipAddress, settings_.locationSuffix, c);
              }),
              Stage::PrepRegisterCompute);
        return;
    case Stage::PrepRegisterCompute:
        if (r.aws.kind != AwsErrorKind::None && r.aws.kind != AwsErrorKind::Conflict) {
            return FailWith(FlowFailure::Aws, FirstErrorLines(r.err, kErrorDetailLines), &r);
        }
        FinishOp(Outcome::Succeeded);
        return;
    default:
        FinishOp(Outcome::Failed);
        return;
    }
}

// ---------------------------------------------------------------------------------
// サーバ起動: describe-compute (SDK エンドポイント) → get-compute-auth-token (秘密) → Server.exe
// ---------------------------------------------------------------------------------

void GameLiftFlow::HandleStartServer(const CallResult& r)
{
    switch (stage_) {
    case Stage::StartDescribeCompute: {
        if (r.aws.kind != AwsErrorKind::None) {
            return FailWith(FlowFailure::Aws, FirstErrorLines(r.err, kErrorDetailLines), &r);
        }
        AwsCompute compute;
        std::string parseError;
        if (!ParseCompute(r.out, compute, parseError)) {
            return FailWith(FlowFailure::BadResponse, parseError);
        }
        if (compute.sdkEndpoint.empty() || !IsValidResourceId(compute.sdkEndpoint)) {
            return FailWith(FlowFailure::NoSdkEndpoint, compute.status);
        }
        sdkEndpoint_ = compute.sdkEndpoint;
        Issue(MakeCall([&](AwsCall& c) {
                  return GameLiftGetComputeAuthTokenCall(settings_.region, settings_.anywhereFleetId,
                                                         EffectiveComputeName(), c);
              }),
              Stage::StartGetToken);
        return;
    }
    case Stage::StartGetToken:
        LaunchServerWithToken(r);
        return;
    default:
        FinishOp(Outcome::Failed);
        return;
    }
}

void GameLiftFlow::LaunchServerWithToken(const CallResult& r)
{
    if (r.aws.kind != AwsErrorKind::None) {
        // stderr に秘密は出ないが、念のためこの経路では内容を detail にしない (分類と型付きコードだけ)
        return FailWith(FlowFailure::Aws, std::string(), &r);
    }
    AwsComputeAuthToken token;
    std::string parseError;
    if (!ParseComputeAuthToken(r.out, token, parseError)) {
        token.Wipe();
        return FailWith(FlowFailure::BadResponse, "the auth token response could not be read");
    }

    AnywhereServerLaunchInput input;
    input.exeDir = environment_.exeDir;
    input.projectRoot = environment_.projectRoot;
    input.wsUrl = sdkEndpoint_;
    input.fleetId = settings_.anywhereFleetId;
    input.computeName = EffectiveComputeName();
    input.port = settings_.port;
    input.maxPlayers = settings_.maxPlayers;
    input.stopEventName = L"mye_gamelift_stop_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
                          std::to_wstring(++stopEventSeq_);
    SYSTEMTIME t{};
    GetLocalTime(&t);
    wchar_t stamp[32] = {};
    swprintf_s(stamp, L"%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    input.logPath = ServerLogPathFor(environment_.projectRoot, stamp);
    input.authToken = token.authToken;
    token.Wipe();

    ServerLaunchSpec spec = BuildAnywhereServerLaunch(input);
    WipeString(input.authToken);
    std::string error;
    const bool started = host_.StartServer(spec, error);
    spec.WipeSecrets();
    if (!started) {
        return FailWith(FlowFailure::ServerStartFailed, error);
    }
    clientsLaunched_ = 0;
    UpdateServerView(); // 同じフレームの描画で「起動」がまだ押せる状態に見えないように
    FinishOp(Outcome::Succeeded);
}

// ---------------------------------------------------------------------------------
// セッションを作って参加
// ---------------------------------------------------------------------------------

void GameLiftFlow::IssueCreateGameSession()
{
    Issue(MakeCall([&](AwsCall& c) {
              return GameLiftCreateGameSessionCall(settings_.region, settings_.anywhereFleetId, settings_.locationSuffix,
                                                   settings_.maxPlayers, c);
          }),
          Stage::JoinCreateSession);
}

void GameLiftFlow::IssueDescribeSession()
{
    Issue(MakeCall([&](AwsCall& c) {
              DescribeGameSessionsQuery query;
              query.fleetId = settings_.anywhereFleetId;
              query.locationSuffix = settings_.locationSuffix;
              query.gameSessionId = gameSessionId_;
              return GameLiftDescribeGameSessionsCall(settings_.region, query, c);
          }),
          Stage::JoinDescribeSession);
}

void GameLiftFlow::IssueCreatePlayerSession()
{
    const std::string playerId = playerIdPrefix_ + std::to_string(++playerSeq_);
    Issue(MakeCall([&](AwsCall& c) {
              return GameLiftCreatePlayerSessionCall(settings_.region, gameSessionId_, playerId, c);
          }),
          Stage::JoinCreatePlayer);
}

void GameLiftFlow::HandleJoin(const CallResult& r)
{
    std::string parseError;
    switch (stage_) {
    case Stage::JoinListActive: {
        if (r.aws.kind != AwsErrorKind::None) {
            return FailWith(FlowFailure::Aws, FirstErrorLines(r.err, kErrorDetailLines), &r);
        }
        std::vector<AwsGameSession> sessions;
        if (!ParseDescribeGameSessions(r.out, sessions, parseError)) {
            return FailWith(FlowFailure::BadResponse, parseError);
        }
        const auto active = std::find_if(sessions.begin(), sessions.end(),
                                         [](const AwsGameSession& s) { return s.status == kStatusActive; });
        if (active != sessions.end() && IsValidResourceId(active->gameSessionId)) {
            gameSessionId_ = active->gameSessionId; // 既存の ACTIVE なセッションへ足す
            IssueCreatePlayerSession();
        } else {
            attempt_ = 0;
            maxAttempts_ = timings_.capacityRetryMax;
            IssueCreateGameSession();
        }
        return;
    }
    case Stage::JoinCreateSession: {
        if (r.aws.kind == AwsErrorKind::CapacityExceeded) {
            // ProcessReady の直後はまだ割り当て先として見えないことがある。間隔をあけて再試行する
            if (attempt_ >= timings_.capacityRetryMax) {
                return FailWith(FlowFailure::CapacityRetriesExhausted, FirstErrorLines(r.err, kErrorDetailLines), &r);
            }
            ++attempt_;
            WaitFor(timings_.capacityRetryIntervalMs, WaitKind::CapacityRetry, Stage::JoinCreateRetryWait);
            return;
        }
        if (r.aws.kind != AwsErrorKind::None) {
            return FailWith(FlowFailure::Aws, FirstErrorLines(r.err, kErrorDetailLines), &r);
        }
        AwsGameSession session;
        if (!ParseCreateGameSession(r.out, session, parseError) || !IsValidResourceId(session.gameSessionId)) {
            return FailWith(FlowFailure::BadResponse, parseError);
        }
        gameSessionId_ = session.gameSessionId;
        attempt_ = 0;
        maxAttempts_ = std::max<int>(1, static_cast<int>(timings_.sessionWaitMaxMs /
                                                         std::max<uint32_t>(1, timings_.sessionPollIntervalMs)));
        if (session.status == kStatusActive) {
            IssueCreatePlayerSession();
        } else {
            IssueDescribeSession(); // ACTIVATING → ACTIVE まで見る
        }
        return;
    }
    case Stage::JoinDescribeSession: {
        if (r.aws.kind != AwsErrorKind::None) {
            return FailWith(FlowFailure::Aws, FirstErrorLines(r.err, kErrorDetailLines), &r);
        }
        std::vector<AwsGameSession> sessions;
        if (!ParseDescribeGameSessions(r.out, sessions, parseError)) {
            return FailWith(FlowFailure::BadResponse, parseError);
        }
        if (sessions.empty()) {
            return FailWith(FlowFailure::SessionNotActive, "the game session was not found");
        }
        const AwsGameSession& session = sessions.front();
        if (session.status == kStatusActive) {
            IssueCreatePlayerSession();
        } else if (session.status == kStatusActivating) {
            if (attempt_ >= maxAttempts_) {
                return FailWith(FlowFailure::SessionWaitTimedOut, session.status);
            }
            ++attempt_;
            WaitFor(timings_.sessionPollIntervalMs, WaitKind::SessionActive, Stage::JoinSessionPollWait);
        } else {
            return FailWith(FlowFailure::SessionNotActive, session.status + " " + session.statusReason);
        }
        return;
    }
    case Stage::JoinCreatePlayer: {
        if (r.aws.kind != AwsErrorKind::None) {
            return FailWith(FlowFailure::Aws, FirstErrorLines(r.err, kErrorDetailLines), &r);
        }
        AwsPlayerSession session;
        if (!ParseCreatePlayerSession(r.out, session, parseError)) {
            return FailWith(FlowFailure::BadResponse, parseError);
        }
        LaunchClientFor(session);
        return;
    }
    default:
        FinishOp(Outcome::Failed);
        return;
    }
}

// player session は作成から 60 秒以内に Accept されないと TIMEDOUT になる。作った流れで直ちに起動する
void GameLiftFlow::LaunchClientFor(const AwsPlayerSession& session)
{
    const std::string host = session.ipAddress.empty() ? settings_.ipAddress : session.ipAddress;
    const int port = session.port > 0 ? session.port : settings_.port;
    if (!IsValidResourceId(host) || !IsValidPort(port) || !IsValidResourceId(session.playerSessionId)) {
        return FailWith(FlowFailure::BadResponse, "the player session response has an unusable address");
    }
    const ClientLaunchSpec spec =
        BuildAnywhereClientLaunch(environment_.exeDir, environment_.projectRoot, host, port, session.playerSessionId);
    std::string error;
    if (!host_.LaunchClient(spec, error)) {
        return FailWith(FlowFailure::ClientStartFailed, error);
    }
    ++clientsLaunched_;
    FinishOp(Outcome::Succeeded);
}

// ---------------------------------------------------------------------------------
// 片付け: (サーバ停止) → deregister → delete-fleet → 消えるまで → delete-location
// ---------------------------------------------------------------------------------

void GameLiftFlow::CleanupAfterServer()
{
    if (settings_.anywhereFleetId.empty()) {
        CleanupDeleteLocation();
        return;
    }
    Issue(MakeCall([&](AwsCall& c) {
              return GameLiftDeregisterComputeCall(settings_.region, settings_.anywhereFleetId, EffectiveComputeName(), c);
          }),
          Stage::CleanDeregister);
}

void GameLiftFlow::CleanupDeleteLocation()
{
    Issue(MakeCall([&](AwsCall& c) {
              return GameLiftDeleteLocationCall(settings_.region, settings_.locationSuffix, c);
          }),
          Stage::CleanDeleteLocation);
}

void GameLiftFlow::IssueDescribeFleetGone()
{
    Issue(MakeCall([&](AwsCall& c) {
              return GameLiftDescribeFleetAttributesCall(settings_.region, settings_.anywhereFleetId, c);
          }),
          Stage::CleanDescribeFleet);
}

void GameLiftFlow::HandleCleanup(const CallResult& r)
{
    // 片付けは「無いものは済んでいる」として進める (NotFound は成功扱い)
    const auto failUnlessNotFound = [&]() {
        if (r.aws.kind != AwsErrorKind::None && r.aws.kind != AwsErrorKind::NotFound) {
            FailWith(FlowFailure::Aws, FirstErrorLines(r.err, kErrorDetailLines), &r);
            return true;
        }
        return false;
    };
    switch (stage_) {
    case Stage::CleanDeregister:
        if (failUnlessNotFound()) {
            return;
        }
        Issue(MakeCall([&](AwsCall& c) { return GameLiftDeleteFleetCall(settings_.region, settings_.anywhereFleetId, c); }),
              Stage::CleanDeleteFleet);
        return;
    case Stage::CleanDeleteFleet:
        if (failUnlessNotFound()) {
            return;
        }
        attempt_ = 0;
        maxAttempts_ = std::max<int>(1, static_cast<int>(timings_.fleetGoneWaitMaxMs /
                                                         std::max<uint32_t>(1, timings_.fleetGonePollIntervalMs)));
        IssueDescribeFleetGone();
        return;
    case Stage::CleanDescribeFleet: {
        bool gone = false;
        if (r.aws.kind == AwsErrorKind::NotFound) {
            gone = true;
        } else if (r.aws.kind != AwsErrorKind::None) {
            return FailWith(FlowFailure::Aws, FirstErrorLines(r.err, kErrorDetailLines), &r);
        } else {
            std::vector<AwsFleetAttributes> fleets;
            std::string parseError;
            if (!ParseDescribeFleetAttributes(r.out, fleets, parseError)) {
                return FailWith(FlowFailure::BadResponse, parseError);
            }
            gone = fleets.empty();
        }
        if (gone) {
            settings_.anywhereFleetId.clear();
            ChangedSettings();
            CleanupDeleteLocation();
            return;
        }
        if (attempt_ >= maxAttempts_) {
            return FailWith(FlowFailure::FleetDeleteTimedOut, std::string());
        }
        ++attempt_;
        WaitFor(timings_.fleetGonePollIntervalMs, WaitKind::FleetGone, Stage::CleanFleetPollWait);
        return;
    }
    case Stage::CleanDeleteLocation:
        if (failUnlessNotFound()) {
            return;
        }
        FinishOp(Outcome::Succeeded);
        return;
    default:
        FinishOp(Outcome::Failed);
        return;
    }
}

} // namespace mye
