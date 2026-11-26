//====================================================================================
//                          GameLiftFlowSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                       11/27/2026
//                                          GameLift 窓の手順 (状態機械・Server.exe の子プロセス) の回帰テスト実装
//====================================================================================
#include "Editor/Tools/GameLift/GameLiftFlowSelfTest.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <Windows.h>

#include "Editor/App/EditorSettings.h"
#include "Editor/Tools/GameLift/GameLiftFlow.h"
#include "Editor/Tools/GameLift/GameLiftFlowTestDoubles.h"
#include "Editor/Tools/GameLift/GameLiftSettings.h"
#include "Editor/Tools/GameLift/ServerProcess.h"
#include "Editor/Windows/Project/GameLiftWindow.h"
#include "Engine/Core/Diagnostics/Log.h"

namespace fs = std::filesystem;

namespace mye {
namespace {

using flowtest::FakeAwsCli;
using flowtest::FakeAwsWorld;
using flowtest::FakeHost;
using flowtest::FakeReply;

struct Counter
{
    int failCount = 0;
    void Check(bool cond, const char* what)
    {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    }
};

std::string Narrow(const std::wstring& wide)
{
    std::string narrow;
    for (const wchar_t ch : wide) {
        narrow.push_back(static_cast<char>(ch));
    }
    return narrow;
}

void Touch(const fs::path& path)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream(path, std::ios::binary) << "x";
}

// 待ちの間隔と上限を短くした時間設定 (偽の時計を 5 ms ずつ進めて回す)
FlowTimings FastTimings()
{
    FlowTimings t;
    t.capacityRetryIntervalMs = 5;
    t.capacityRetryMax = 20;
    t.sessionPollIntervalMs = 5;
    t.sessionWaitMaxMs = 200;
    t.fleetGonePollIntervalMs = 5;
    t.fleetGoneWaitMaxMs = 500;
    t.serverStopGraceMs = 50;
    return t;
}

// 一時フォルダ (project / exe) と偽の世界・CLI・ホスト・手順をひとまとめにしたもの
struct Fixture
{
    fs::path root;
    fs::path project;
    fs::path exeDir;
    FakeAwsWorld world;
    FakeAwsCli cli;
    FakeHost host;
    GameLiftSettings settings;
    std::unique_ptr<GameLiftFlow> flow;
    uint64_t now = 1000;
    int settingsChanged = 0;

    explicit Fixture(const char* name, bool gameLogic = true, bool serverExe = true, bool runtimeExe = true,
                     bool withProject = true)
    {
        root = fs::temp_directory_path() / (L"mye_gl_flow_selftest_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
                                            std::wstring(name, name + std::strlen(name)));
        std::error_code ec;
        fs::remove_all(root, ec);
        project = root / L"project";
        exeDir = root / L"exe";
        fs::create_directories(project, ec);
        fs::create_directories(exeDir, ec);
        if (gameLogic) {
            Touch(project / L"cache" / L"GameLogic.dll");
        }
        if (serverExe) {
            Touch(exeDir / L"Server.exe");
        }
        if (runtimeExe) {
            Touch(exeDir / L"Runtime.exe");
        }
        cli.respond = [this](const std::vector<std::wstring>& args) { return world(args); };
        settings.region = "ap-northeast-1";
        flow = std::make_unique<GameLiftFlow>(cli, host, settings, FastTimings());
        FlowEnvironment env;
        env.projectRoot = withProject ? project.wstring() : std::wstring();
        env.exeDir = exeDir.wstring();
        env.computerName = L"MY_PC";
        flow->SetEnvironment(env);
        flow->SetSettingsChangedHandler([this]() { ++settingsChanged; });
    }
    ~Fixture()
    {
        flow.reset();
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    // 操作が終わるまで偽の時計を進めて Tick する (Server.exe の停止待ちなども進む)
    void Pump(int maxTicks = 3000)
    {
        for (int i = 0; i < maxTicks && (flow->Busy() || i == 0); ++i) {
            now += 5;
            flow->Tick(now);
        }
    }
    void TickN(int count)
    {
        for (int i = 0; i < count; ++i) {
            now += 5;
            flow->Tick(now);
        }
    }
    void Refresh()
    {
        flow->RefreshStatus();
        Pump();
    }
    bool Run(FlowAction action, bool confirm = true)
    {
        if (!flow->Request(action)) {
            return false;
        }
        if (flow->PendingConfirm().has_value()) {
            if (!confirm) {
                flow->Decline();
                return true;
            }
            if (!flow->Confirm()) {
                return false;
            }
        }
        Pump();
        return true;
    }
    std::vector<std::string> Ops() const
    {
        std::vector<std::string> ops;
        for (const AwsCall& call : cli.calls) {
            ops.push_back(Narrow(call.label));
        }
        return ops;
    }
    bool AnyArgContains(const std::string& needle) const
    {
        for (const AwsCall& call : cli.calls) {
            for (const std::wstring& arg : call.args) {
                if (Narrow(arg).find(needle) != std::string::npos) {
                    return true;
                }
            }
        }
        return false;
    }
    bool HasArgPair(size_t callIndex, const wchar_t* name, const wchar_t* value) const
    {
        const std::vector<std::wstring>& args = cli.calls[callIndex].args;
        for (size_t i = 0; i + 1 < args.size(); ++i) {
            if (args[i] == name && args[i + 1] == value) {
                return true;
            }
        }
        return false;
    }
    // 準備まで済ませる (フリート ID が設定へ入る)
    void PrepareAll()
    {
        Refresh();
        Run(FlowAction::Prepare);
    }
    // サーバを起動して待機中にする
    void StartServerReady()
    {
        Run(FlowAction::StartServer);
        host.MakeReady({ "[gamelift] ProcessReady sent (UDP port 7777, 0 log path(s)); waiting for a game session" });
        TickN(1);
    }
};

using Ops = std::vector<std::string>;

// ---------------------------------------------------------------------------------
// ボタンの有効 / 無効と理由
// ---------------------------------------------------------------------------------

void TestAvailability(Counter& c)
{
    {
        Fixture f("avail_unknown");
        c.Check(f.flow->Query(FlowAction::Prepare).reason == BlockReason::StatusUnknown && !f.flow->Query(FlowAction::Prepare).enabled,
                "before the first check every CLI button is disabled (StatusUnknown)");
    }
    {
        Fixture f("avail_nocli");
        f.cli.exeFound = false;
        f.Refresh();
        c.Check(f.flow->Status().checked && !f.flow->Status().cliFound && f.cli.calls.empty(), "no aws.exe: checked, not found, no process started");
        c.Check(f.flow->Query(FlowAction::Prepare).reason == BlockReason::CliMissing && f.flow->Query(FlowAction::StartServer).reason == BlockReason::CliMissing &&
                    f.flow->Query(FlowAction::Join).reason == BlockReason::CliMissing && f.flow->Query(FlowAction::Cleanup).reason == BlockReason::CliMissing,
                "no CLI: Prepare / Start / Join / Cleanup are disabled with CliMissing");
        c.Check(!f.flow->Request(FlowAction::Prepare) && !f.flow->PendingConfirm().has_value(), "a disabled button does nothing (no confirmation either)");
    }
    {
        Fixture f("avail_v1");
        f.world.v1 = true;
        f.Refresh();
        c.Check(f.flow->Status().versionKnown && f.flow->Status().version.major == 1 && f.flow->Query(FlowAction::Prepare).reason == BlockReason::CliTooOld,
                "AWS CLI v1: disabled with CliTooOld");
        c.Check(f.Ops() == Ops{ "--version" }, "v1: stops after --version (no region / identity calls)");
    }
    {
        Fixture f("avail_login");
        f.world.loggedIn = false;
        f.Refresh();
        c.Check(f.flow->Status().login == LoginState::NotLoggedIn && f.flow->Query(FlowAction::Prepare).reason == BlockReason::NotLoggedIn,
                "expired credentials: NotLoggedIn, buttons disabled");
        f.world.loggedIn = true;
        f.Refresh();
        c.Check(f.flow->Status().login == LoginState::LoggedIn && f.flow->Status().identity.account == "123456789012" && f.flow->Query(FlowAction::Prepare).enabled,
                "Recheck after signing in: account is read and Prepare is enabled");
    }
    {
        Fixture f("avail_noregion");
        f.settings.region.clear();
        f.world.configRegion.clear();
        f.Refresh();
        c.Check(f.flow->Query(FlowAction::Prepare).reason == BlockReason::NoRegion, "no region (settings empty, aws configure has none): NoRegion");
        f.settings.region = "ap-northeast-1 ";
        c.Check(f.flow->Query(FlowAction::Prepare).reason == BlockReason::InvalidRegion, "a malformed region is rejected (InvalidRegion)");
    }
    {
        Fixture f("avail_regionfrom");
        f.settings.region.clear();
        f.world.configRegion = "us-west-2";
        f.Refresh();
        c.Check(f.settings.region == "us-west-2" && f.settingsChanged >= 1, "an empty region is filled from aws configure get region (and saved)");
        c.Check(f.Ops() == Ops{ "--version", "configure get region", "sts get-caller-identity" }, "the check runs --version, configure get region, sts get-caller-identity");
    }
    {
        Fixture f("avail_noproject", true, true, true, false);
        f.settings.anywhereFleetId = "fleet-1";
        f.Refresh();
        c.Check(f.flow->Query(FlowAction::StartServer).reason == BlockReason::NoProject && f.flow->Query(FlowAction::Join).reason == BlockReason::NoProject,
                "no project: Start / Join are disabled with NoProject");
        c.Check(f.flow->Query(FlowAction::Prepare).enabled && f.flow->Query(FlowAction::Cleanup).enabled, "no project: Prepare / Cleanup (AWS only) stay enabled");
    }
    {
        Fixture f("avail_nodll", false);
        f.settings.anywhereFleetId = "fleet-1";
        f.Refresh();
        c.Check(f.flow->Query(FlowAction::StartServer).reason == BlockReason::NoGameLogic, "no GameLogic.dll: Start is disabled with NoGameLogic");
    }
    {
        Fixture f("avail_noserver", true, false);
        f.settings.anywhereFleetId = "fleet-1";
        f.Refresh();
        c.Check(f.flow->Query(FlowAction::StartServer).reason == BlockReason::NoServerExe, "no Server.exe next to the editor: NoServerExe");
    }
    {
        Fixture f("avail_noruntime", true, true, false);
        f.settings.anywhereFleetId = "fleet-1";
        f.Refresh();
        f.host.MakeReady();
        f.TickN(1);
        c.Check(f.flow->Query(FlowAction::Join).reason == BlockReason::NoRuntimeExe, "no Runtime.exe next to the editor: Join is disabled with NoRuntimeExe");
    }
    {
        Fixture f("avail_flow");
        f.Refresh();
        c.Check(f.flow->Query(FlowAction::StartServer).reason == BlockReason::NotPrepared, "Start before Prepare: NotPrepared");
        c.Check(f.flow->Query(FlowAction::StopServer).reason == BlockReason::ServerNotRunning && f.flow->Query(FlowAction::Join).reason == BlockReason::NotPrepared,
                "Stop without a server: ServerNotRunning; Join before Prepare: NotPrepared");
        f.settings.anywhereFleetId = "fleet-1";
        c.Check(f.flow->Query(FlowAction::StartServer).enabled && f.flow->Query(FlowAction::Join).reason == BlockReason::ServerNotRunning,
                "prepared: Start is enabled; Join needs a server");
        std::string error;
        ServerLaunchSpec spec;
        f.host.StartServer(spec, error);
        f.TickN(1);
        c.Check(f.flow->Query(FlowAction::Join).reason == BlockReason::ServerNotReady && f.flow->Query(FlowAction::StartServer).reason == BlockReason::ServerAlreadyRunning &&
                    f.flow->Query(FlowAction::StopServer).enabled,
                "server starting: Join waits for ProcessReady, Start is refused, Stop is allowed");
        f.host.MakeReady();
        f.TickN(1);
        c.Check(f.flow->Query(FlowAction::Join).enabled, "server ready: Join is enabled");
        f.settings.anywhereFleetName = "bad name";
        c.Check(f.flow->Query(FlowAction::Prepare).reason == BlockReason::InvalidFleetName, "a fleet name with a space: InvalidFleetName");
        f.settings.anywhereFleetName = "mye-anywhere";
        f.settings.locationSuffix = "custom-x";
        c.Check(f.flow->Query(FlowAction::Prepare).reason == BlockReason::InvalidSuffix, "'custom-x' as the suffix: InvalidSuffix");
        f.settings.locationSuffix = "my_loc";
        c.Check(f.flow->Query(FlowAction::Prepare).reason == BlockReason::InvalidSuffix, "'_' in the suffix: InvalidSuffix");
        f.settings.locationSuffix = "mye-dev";
        f.settings.computeName = "my_pc";
        c.Check(f.flow->Query(FlowAction::Prepare).reason == BlockReason::InvalidComputeName, "'my_pc' as the compute name: InvalidComputeName");
        f.settings.computeName.clear();
        f.settings.ipAddress = "1.2.3";
        c.Check(f.flow->Query(FlowAction::Prepare).reason == BlockReason::InvalidIp, "a malformed IP: InvalidIp");
        f.settings.ipAddress = "127.0.0.1";
        f.settings.port = 0;
        c.Check(f.flow->Query(FlowAction::StartServer).reason == BlockReason::ServerAlreadyRunning || f.flow->Query(FlowAction::StartServer).reason == BlockReason::InvalidPort,
                "port 0 is refused for Start (InvalidPort, or the running server comes first)");
        f.settings.port = 7777;
        f.settings.maxPlayers = 9;
        c.Check(f.flow->Query(FlowAction::Join).reason == BlockReason::InvalidMaxPlayers, "max players 9: InvalidMaxPlayers");
    }
}

// ---------------------------------------------------------------------------------
// 準備
// ---------------------------------------------------------------------------------

void TestPrepare(Counter& c)
{
    {
        Fixture f("prep_all");
        f.Refresh();
        f.cli.calls.clear();
        // 確認ダイアログで「やめる」→ CLI は 1 本も出ない
        c.Check(f.Run(FlowAction::Prepare, false) && f.cli.calls.empty() && !f.flow->Busy() && !f.flow->PendingConfirm().has_value(),
                "Prepare: declining the confirmation runs no CLI");
        // 確認待ちの間も CLI は出ない
        c.Check(f.flow->Request(FlowAction::Prepare) && f.flow->PendingConfirm().has_value() && f.cli.calls.empty() && !f.flow->Busy(),
                "Prepare: Request alone only asks for confirmation");
        const std::optional<ConfirmInfo> info = f.flow->PendingConfirm();
        c.Check(info.has_value() && info->action == FlowAction::Prepare && info->region == "ap-northeast-1" && info->locationName == "custom-mye-dev" &&
                    info->fleetName == "mye-anywhere" && info->computeName == "MY-PC" && info->ipAddress == "127.0.0.1",
                "the confirmation names the region, location (custom- once), fleet and compute (PC name tidied: '_' -> '-')");
        f.flow->Decline();

        // 何も無い → location → fleet → compute の順に 3 本の作成
        c.Check(f.Run(FlowAction::Prepare) && !f.flow->LastError().has_value(), "Prepare (nothing exists) completes");
        c.Check(f.Ops() == Ops{ "gamelift list-locations", "gamelift create-location", "gamelift create-fleet", "gamelift describe-compute", "gamelift register-compute" },
                "Prepare (nothing exists): list-locations, create-location, create-fleet, describe-compute, register-compute");
        c.Check(f.world.createLocationCalls == 1 && f.world.createFleetCalls == 1 && f.world.registerComputeCalls == 1, "three creations, one each");
        c.Check(!f.AnyArgContains("custom-custom") && f.HasArgPair(0, L"--filters", L"CUSTOM") && f.HasArgPair(1, L"--location-name", L"custom-mye-dev") &&
                    f.HasArgPair(2, L"--locations", L"Location=custom-mye-dev") && f.HasArgPair(2, L"--compute-type", L"ANYWHERE") &&
                    f.HasArgPair(4, L"--location", L"custom-mye-dev") && f.HasArgPair(4, L"--ip-address", L"127.0.0.1") && f.HasArgPair(4, L"--compute-name", L"MY-PC"),
                "'custom-' is added exactly once; the fleet / compute use the location name, IP and tidied compute name");
        c.Check(f.settings.anywhereFleetId == f.world.fleetId && !f.settings.anywhereFleetId.empty() && f.settingsChanged >= 1,
                "the fleet ID is saved into the settings");
        c.Check(f.flow->Step(FlowAction::Prepare) == StepState::Done, "step state: Prepare is done");

        // 全部ある → 作成 0 本
        f.cli.calls.clear();
        c.Check(f.Run(FlowAction::Prepare), "Prepare (everything exists) completes");
        c.Check(f.Ops() == Ops{ "gamelift list-locations", "gamelift describe-fleet-attributes", "gamelift describe-compute" } &&
                    f.world.createLocationCalls == 1 && f.world.createFleetCalls == 1 && f.world.registerComputeCalls == 1,
                "Prepare (everything exists): reads only, nothing is created again");
    }
    {
        // 保存済みの ID が AWS 側で消えていた → 作り直して ID を差し替える
        Fixture f("prep_stale");
        f.Refresh();
        f.world.locations = { "custom-mye-dev" };
        f.settings.anywhereFleetId = "fleet-stale";
        f.Run(FlowAction::Prepare);
        c.Check(f.world.createFleetCalls == 1 && f.settings.anywhereFleetId == f.world.fleetId && f.world.createLocationCalls == 0,
                "Prepare: a saved fleet ID that no longer exists is replaced by a new fleet (the location is reused)");
    }
    {
        // 一部だけある (location と fleet はあり compute が無い) → compute だけ作る
        Fixture f("prep_partial");
        f.Refresh();
        f.world.locations = { "custom-mye-dev" };
        f.world.fleetId = "fleet-1111aaaa-2222-3333-4444-555566667777";
        f.settings.anywhereFleetId = f.world.fleetId;
        f.Run(FlowAction::Prepare);
        c.Check(f.world.createLocationCalls == 0 && f.world.createFleetCalls == 0 && f.world.registerComputeCalls == 1, "Prepare (partial): only the missing compute is registered");
    }
    {
        // 入力が不正なら Request が拒否され、CLI は出ない
        Fixture f("prep_reject");
        f.Refresh();
        f.cli.calls.clear();
        f.settings.locationSuffix = "custom-x";
        c.Check(!f.Run(FlowAction::Prepare) && f.cli.calls.empty(), "Prepare with a 'custom-x' suffix is rejected without any CLI");
        f.settings.locationSuffix = "mye-dev";
        f.settings.computeName = "my_pc";
        c.Check(!f.Run(FlowAction::Prepare) && f.cli.calls.empty(), "Prepare with a 'my_pc' compute name is rejected without any CLI");
    }
    {
        // 資格情報切れ: 失敗が分類され、状態が NotLoggedIn に戻る
        Fixture f("prep_expired");
        f.Refresh();
        f.world.loggedIn = false;
        f.Run(FlowAction::Prepare);
        c.Check(f.flow->LastError().has_value() && f.flow->LastError()->failure == FlowFailure::Aws && f.flow->LastError()->awsKind == AwsErrorKind::Credentials &&
                    f.flow->Step(FlowAction::Prepare) == StepState::Failed,
                "credentials expiring mid-way: the failure is classified (Credentials) and the step is Failed");
        c.Check(f.flow->Status().login == LoginState::NotLoggedIn && f.flow->Query(FlowAction::Prepare).reason == BlockReason::NotLoggedIn,
                "after that the window asks to sign in again (NotLoggedIn)");
    }
}

// ---------------------------------------------------------------------------------
// サーバ起動 (argv と環境) / 停止 / 設定の往復
// ---------------------------------------------------------------------------------

void TestStartServer(Counter& c)
{
    Fixture f("start");
    f.PrepareAll();
    f.cli.calls.clear();
    f.cli.respond = [&f](const std::vector<std::wstring>& args) {
        FakeReply r = f.world(args);
        if (args.size() > 1 && args[1] == L"get-compute-auth-token") {
            r.polls = 4; // 実行中の間の表示を見られるように長めに
        }
        return r;
    };

    c.Check(f.flow->Request(FlowAction::StartServer) && f.flow->Busy(), "Start: no confirmation, the first CLI starts at once");
    bool sawSecretCall = false;
    bool labelHidden = false;
    for (int i = 0; i < 100 && f.flow->Busy(); ++i) {
        f.TickN(1);
        const FlowProgress progress = f.flow->Progress();
        if (progress.cliRunning && !f.cli.calls.empty() && f.cli.calls.back().secret) {
            sawSecretCall = true;
            labelHidden = progress.cliLabel.empty();
        }
    }
    c.Check(sawSecretCall && labelHidden, "the get-compute-auth-token call is shown without its name (secret)");
    c.Check(f.Ops() == Ops{ "gamelift describe-compute", "gamelift get-compute-auth-token" } && f.host.startCount == 1,
            "Start: describe-compute (SDK endpoint) -> get-compute-auth-token -> Server.exe launched once");

    const ServerLaunchSpec& spec = f.host.lastServerSpec;
    const std::wstring proj = f.project.wstring();
    const std::wstring name = spec.stopEventName;
    const std::vector<std::wstring> expected = {
        L"--project", proj, L"--hosting", L"gamelift", L"--gamelift-ws-url", L"wss://ap-northeast-1.api.amazongamelift.com",
        L"--gamelift-fleet-id", L"fleet-1111aaaa-2222-3333-4444-555566667777", L"--gamelift-host-id", L"MY-PC", L"--port", L"7777",
        L"--max-players", L"4", L"--stop-event", name,
    };
    c.Check(spec.exePath == (f.exeDir / L"Server.exe").wstring() && spec.args == expected && !name.empty() && name.rfind(L"mye_gamelift_stop_", 0) == 0,
            "Server.exe argv is exactly the spec (project / gamelift / ws-url / fleet / host-id / port / max-players / stop-event)");
    bool tokenInArgs = false;
    for (const std::wstring& arg : spec.args) {
        tokenInArgs = tokenInArgs || Narrow(arg).find("SECRET-TOKEN") != std::string::npos;
    }
    c.Check(!tokenInArgs && !f.AnyArgContains("SECRET-TOKEN"), "the token is in no argv (neither Server.exe's nor any aws call's)");
    c.Check(spec.extraEnv.size() == 1 && spec.extraEnv[0].name == kServerTokenEnvName && spec.extraEnv[0].value == L"SECRET-TOKEN-0123456789",
            "the token goes into the child's environment as MYE_GAMELIFT_AUTH_TOKEN");
    c.Check(spec.workDir == proj && spec.logPath.rfind((f.project / L"cache" / L"gamelift").wstring(), 0) == 0 &&
                spec.logPath.find(L"server-") != std::wstring::npos && spec.logPath.size() > 4 && spec.logPath.substr(spec.logPath.size() - 4) == L".log",
            "the log goes to <project>\\cache\\gamelift\\server-<stamp>.log and the working directory is the project");
    c.Check(f.cli.clearOutputCount >= 1, "the secret CLI output is cleared once used");

    // ProcessReady の行で「待機中」
    c.Check(f.flow->Server().phase == ServerPhase::Starting && f.flow->Step(FlowAction::StartServer) == StepState::Running, "before ProcessReady: starting");
    f.host.MakeReady({ "[gamelift] ProcessReady sent (UDP port 7777, 0 log path(s)); waiting for a game session" });
    f.TickN(1);
    c.Check(f.flow->Server().phase == ServerPhase::Ready && f.flow->Step(FlowAction::StartServer) == StepState::Done &&
                f.flow->Server().logTail.size() == 1,
            "after the ProcessReady line: ready (waiting for a game session)");

    // 設定の往復: ID は戻り、トークンはファイルに無い
    {
        const fs::path settingsDir = f.root / L"settings";
        std::error_code ec;
        fs::create_directories(settingsDir, ec);
        EditorSettings out;
        out.Load(settingsDir.wstring());
        out.gameLift = f.settings;
        out.gameLift.computeName = "my-pc";
        out.Save();
        std::ifstream in(settingsDir / L"editor_settings.json", std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        c.Check(text.find("SECRET-TOKEN") == std::string::npos && text.find("AuthToken") == std::string::npos && text.find("authToken") == std::string::npos &&
                    text.find("\"anywhereFleetId\"") != std::string::npos,
                "editor_settings.json holds the fleet ID and no token");
        EditorSettings back;
        back.Load(settingsDir.wstring());
        c.Check(back.gameLift.anywhereFleetId == f.settings.anywhereFleetId && back.gameLift.computeName == "my-pc" && back.gameLift.region == "ap-northeast-1" &&
                    back.gameLift.locationSuffix == "mye-dev" && back.gameLift.port == 7777 && back.gameLift.maxPlayers == 4,
                "saving then loading brings the IDs and inputs back");
        // 壊れた値は既定へ戻り、既存のキーは保つ
        {
            std::ofstream broken(settingsDir / L"editor_settings.json", std::ios::binary);
            broken << R"({"snapScale":0.25,"gameLift":{"region":"BAD REGION","locationSuffix":"custom-x","computeName":"a.b","ipAddress":"x","port":"7777","maxPlayers":9,"anywhereFleetId":"-bad"}})";
        }
        EditorSettings fixedUp;
        fixedUp.Load(settingsDir.wstring());
        c.Check(fixedUp.snapScale == 0.25f && fixedUp.gameLift.region.empty() && fixedUp.gameLift.locationSuffix == "mye-dev" && fixedUp.gameLift.computeName.empty() &&
                    fixedUp.gameLift.ipAddress == "127.0.0.1" && fixedUp.gameLift.port == 7777 && fixedUp.gameLift.maxPlayers == 4 && fixedUp.gameLift.anywhereFleetId.empty(),
                "broken values fall back to the defaults (the other keys are kept)");
    }

    // 停止: stop イベントを立てる手順が 1 回呼ばれ、止まると Exited
    c.Check(f.flow->Request(FlowAction::StopServer) && f.host.stopRequests == 1 && f.flow->Query(FlowAction::StopServer).enabled == false,
            "Stop: the stop request (stop event -> grace -> terminate) is issued once and Stop is then disabled");
    f.TickN(5);
    c.Check(f.flow->Server().phase == ServerPhase::Exited && f.flow->Step(FlowAction::StopServer) == StepState::Done &&
                f.flow->Query(FlowAction::StartServer).enabled,
            "after the server exits: stopped, and Start is available again");

    // 自分で終わった (異常終了) → exit code とログ末尾が出て「停止」に戻る
    f.Run(FlowAction::StartServer);
    f.host.MakeExited(3, false, { "line a", "line b" });
    f.TickN(1);
    c.Check(f.flow->Server().phase == ServerPhase::Exited && f.flow->Server().exitCode == 3 && f.flow->Server().logTail.size() == 2 &&
                f.flow->Step(FlowAction::StartServer) == StepState::Failed && f.flow->ClientsLaunched() == 0,
            "a server that exits by itself shows its exit code and log tail, and counts as failed");

    // エディタ終了: 停止手順を踏む
    f.Run(FlowAction::StartServer);
    f.flow->Shutdown();
    c.Check(f.host.blockingStops == 1 && f.flow->Server().phase == ServerPhase::Exited, "Shutdown runs the stop procedure for a running server");

    // 起動失敗 / トークン応答の不正
    {
        Fixture g("start_fail");
        g.PrepareAll();
        g.host.serverStartOk = false;
        g.Run(FlowAction::StartServer);
        c.Check(g.flow->LastError().has_value() && g.flow->LastError()->failure == FlowFailure::ServerStartFailed && g.flow->Step(FlowAction::StartServer) == StepState::Failed,
                "a Server.exe that cannot start is reported (ServerStartFailed)");
    }
}

// ---------------------------------------------------------------------------------
// セッションを作って参加
// ---------------------------------------------------------------------------------

void TestJoin(Counter& c)
{
    {
        Fixture f("join");
        f.PrepareAll();
        f.StartServerReady();
        f.cli.calls.clear();
        f.world.capacityFailuresLeft = 2;
        f.world.activatingDescribesLeft = 2;

        c.Check(f.flow->Query(FlowAction::Join).enabled, "Join is enabled once the server is ready");
        c.Check(f.flow->Request(FlowAction::Join), "Join starts");
        bool sawRetryWait = false;
        bool sawActiveWait = false;
        for (int i = 0; i < 500 && f.flow->Busy(); ++i) {
            f.TickN(1);
            const FlowProgress progress = f.flow->Progress();
            sawRetryWait = sawRetryWait || progress.wait == WaitKind::CapacityRetry;
            sawActiveWait = sawActiveWait || progress.wait == WaitKind::SessionActive;
        }
        c.Check(!f.flow->LastError().has_value() && !f.flow->Busy(), "Join completes without an error");
        c.Check(f.Ops() == Ops{ "gamelift describe-game-sessions", "gamelift create-game-session", "gamelift create-game-session", "gamelift create-game-session",
                                "gamelift describe-game-sessions", "gamelift describe-game-sessions", "gamelift describe-game-sessions", "gamelift create-player-session" },
                "Join: look for an ACTIVE session, create (2 capacity failures retried), wait ACTIVATING -> ACTIVE, then the player session");
        c.Check(sawRetryWait && sawActiveWait, "the retry and the ACTIVE wait are visible as progress (kind / attempt)");
        c.Check(f.HasArgPair(1, L"--location", L"custom-mye-dev") && f.HasArgPair(1, L"--maximum-player-session-count", L"4") && f.world.createGameSessionCalls == 3,
                "create-game-session carries the location and the max players");
        c.Check(f.host.clients.size() == 1, "the client is started in the same flow");
        const std::vector<std::wstring> expected = { L"--project", f.project.wstring(), L"--net-connect", L"127.0.0.1:7777", L"--player-session-id", L"psess-1" };
        c.Check(f.host.clients[0].exePath == (f.exeDir / L"Runtime.exe").wstring() && f.host.clients[0].args == expected && f.host.clients[0].workDir == f.project.wstring(),
                "the client argv is Runtime.exe --project <p> --net-connect IP:PORT --player-session-id <id> (working directory = project)");
        c.Check(f.flow->ClientsLaunched() == 1 && f.flow->Step(FlowAction::Join) == StepState::Done, "one client launched; Join is done");

        // 2 人目: ACTIVE なセッションがあるので新しく作らずに player session を足す
        f.cli.calls.clear();
        f.Run(FlowAction::Join);
        c.Check(f.Ops() == Ops{ "gamelift describe-game-sessions", "gamelift create-player-session" } && f.world.createGameSessionCalls == 3 && f.host.clients.size() == 2 &&
                    f.host.clients[1].args.back() == L"psess-2",
                "a second Join reuses the ACTIVE session (no create-game-session) and starts another client");
        // 最大人数 (4) まで
        f.Run(FlowAction::Join);
        f.Run(FlowAction::Join);
        c.Check(f.flow->ClientsLaunched() == 4 && f.flow->Query(FlowAction::Join).reason == BlockReason::ClientLimit && !f.flow->Request(FlowAction::Join),
                "Join stops at the max players (ClientLimit)");
        // サーバが止まったら数え直す
        f.flow->Request(FlowAction::StopServer);
        f.TickN(5);
        c.Check(f.flow->ClientsLaunched() == 0, "the count restarts when the server stops");
    }
    {
        // 容量不足が続く → 上限で諦める
        Fixture f("join_capacity");
        f.PrepareAll();
        f.StartServerReady();
        f.world.capacityFailuresLeft = 1000;
        f.Run(FlowAction::Join);
        c.Check(f.flow->LastError().has_value() && f.flow->LastError()->failure == FlowFailure::CapacityRetriesExhausted && f.world.createGameSessionCalls == 21 &&
                    f.host.clients.empty() && f.flow->Step(FlowAction::Join) == StepState::Failed,
                "capacity failures beyond the retry limit (20) end in CapacityRetriesExhausted without a client");
    }
    {
        // ACTIVE にならない → 待ちの上限
        Fixture f("join_timeout");
        f.PrepareAll();
        f.StartServerReady();
        f.world.activatingDescribesLeft = 100000;
        f.Run(FlowAction::Join);
        c.Check(f.flow->LastError().has_value() && f.flow->LastError()->failure == FlowFailure::SessionWaitTimedOut && f.host.clients.empty(),
                "a session that never becomes ACTIVE ends in SessionWaitTimedOut (the 60 s limit, shortened here)");
    }
    {
        // クライアントを起動できない
        Fixture f("join_clientfail");
        f.PrepareAll();
        f.StartServerReady();
        f.host.clientStartOk = false;
        f.Run(FlowAction::Join);
        c.Check(f.flow->LastError().has_value() && f.flow->LastError()->failure == FlowFailure::ClientStartFailed && f.flow->ClientsLaunched() == 0,
                "a client that cannot start is reported (ClientStartFailed)");
    }
}

// ---------------------------------------------------------------------------------
// 片付け
// ---------------------------------------------------------------------------------

void TestCleanup(Counter& c)
{
    {
        Fixture f("clean");
        f.PrepareAll();
        f.StartServerReady();
        f.host.stopDelayPolls = 3;
        f.world.deletingDescribesLeft = 2;
        f.cli.calls.clear();

        c.Check(f.flow->Request(FlowAction::Cleanup) && f.flow->PendingConfirm().has_value() && f.flow->PendingConfirm()->action == FlowAction::Cleanup &&
                    f.flow->PendingConfirm()->fleetId == f.settings.anywhereFleetId && f.cli.calls.empty() && f.host.stopRequests == 0,
                "Cleanup: Request only asks for confirmation (nothing is stopped or deleted yet)");
        f.flow->Decline();
        c.Check(f.cli.calls.empty() && f.host.stopRequests == 0 && !f.flow->Busy(), "Cleanup: declining runs no CLI and does not stop the server");

        f.flow->Request(FlowAction::Cleanup);
        c.Check(f.flow->Confirm() && f.host.stopRequests == 1 && f.flow->Progress().wait == WaitKind::ServerStop && f.cli.calls.empty(),
                "Cleanup: a running server is stopped first (no CLI while it stops)");
        bool sawFleetWait = false;
        for (int i = 0; i < 500 && f.flow->Busy(); ++i) {
            f.TickN(1);
            sawFleetWait = sawFleetWait || f.flow->Progress().wait == WaitKind::FleetGone;
        }
        c.Check(!f.flow->LastError().has_value() && !f.flow->Busy() && f.flow->Step(FlowAction::Cleanup) == StepState::Done, "Cleanup completes");
        c.Check(f.Ops() == Ops{ "gamelift deregister-compute", "gamelift delete-fleet", "gamelift describe-fleet-attributes", "gamelift describe-fleet-attributes",
                                "gamelift describe-fleet-attributes", "gamelift delete-location" },
                "Cleanup: deregister-compute -> delete-fleet -> describe (still there x2, then gone) -> delete-location");
        c.Check(sawFleetWait, "the wait for the fleet to disappear is visible as progress");
        c.Check(f.settings.anywhereFleetId.empty() && f.world.locations.empty() && !f.world.computeRegistered, "the saved fleet ID is cleared and the AWS side is empty");
        c.Check(f.flow->Query(FlowAction::StartServer).reason == BlockReason::NotPrepared, "after Cleanup, Start asks for Prepare again");
    }
    {
        // フリート削除が終わらない → 待ちの上限
        Fixture f("clean_timeout");
        f.PrepareAll();
        f.world.deletingDescribesLeft = 100000;
        f.Run(FlowAction::Cleanup);
        c.Check(f.flow->LastError().has_value() && f.flow->LastError()->failure == FlowFailure::FleetDeleteTimedOut && !f.settings.anywhereFleetId.empty(),
                "a fleet that does not disappear ends in FleetDeleteTimedOut and keeps the saved ID");
    }
    {
        // 何も無い状態の片付けも通る (NotFound は済み扱い)
        Fixture f("clean_empty");
        f.Refresh();
        f.cli.calls.clear();
        f.Run(FlowAction::Cleanup);
        c.Check(!f.flow->LastError().has_value() && f.Ops() == Ops{ "gamelift delete-location" }, "Cleanup with nothing saved only tries delete-location, and a missing location is not an error");
    }
}

// ---------------------------------------------------------------------------------
// 取消 / 実行中は他を始めない
// ---------------------------------------------------------------------------------

void TestCancelAndBusy(Counter& c)
{
    Fixture f("cancel");
    f.Refresh();
    f.cli.calls.clear();
    f.cli.respond = [&f](const std::vector<std::wstring>& args) {
        FakeReply r = f.world(args);
        r.polls = 100000; // 終わらない CLI
        return r;
    };
    f.flow->Request(FlowAction::Prepare);
    f.flow->Confirm();
    f.TickN(3);
    c.Check(f.flow->Busy() && f.flow->Progress().cliRunning && f.flow->Progress().cliLabel == L"gamelift list-locations", "a long CLI keeps the step running (label shown)");
    c.Check(!f.flow->Query(FlowAction::Cleanup).enabled && f.flow->Query(FlowAction::Cleanup).reason == BlockReason::Busy && !f.flow->Request(FlowAction::Cleanup),
            "while a CLI runs, other steps are disabled with Busy (one CLI per window)");
    f.flow->RefreshStatus();
    c.Check(f.cli.calls.size() == 1, "Recheck while busy does not start a second CLI");
    f.flow->Cancel();
    f.TickN(2);
    c.Check(!f.flow->Busy() && f.flow->WasCancelled() && !f.flow->LastError().has_value() && f.cli.calls.size() == 1, "Cancel ends the step (cancelled, not an error) without further CLI");
    c.Check(f.world.createLocationCalls == 0 && f.flow->Query(FlowAction::Prepare).enabled, "after Cancel nothing was created and the buttons are back");
    f.flow->Cancel(); // 何も動いていないときの取消は無害
    c.Check(!f.flow->Busy(), "Cancel when idle is harmless");
}

// ---------------------------------------------------------------------------------
// 純関数: 起動の組み立てと既定値
// ---------------------------------------------------------------------------------

void TestPureFunctions(Counter& c)
{
    AnywhereServerLaunchInput in;
    in.exeDir = L"C:\\edit or\\bin";
    in.projectRoot = L"C:\\my game";
    in.wsUrl = "wss://x.example/";
    in.fleetId = "fleet-1";
    in.computeName = "PC-01";
    in.port = 7778;
    in.maxPlayers = 2;
    in.stopEventName = L"evt";
    in.logPath = L"C:\\my game\\cache\\gamelift\\server-1.log";
    in.authToken = "tok";
    const ServerLaunchSpec spec = BuildAnywhereServerLaunch(in);
    c.Check(spec.exePath == L"C:\\edit or\\bin\\Server.exe" && spec.args.size() == 16 && spec.args[1] == L"C:\\my game" && spec.args[3] == L"gamelift" &&
                spec.args[11] == L"7778" && spec.args[13] == L"2" && spec.args[15] == L"evt" && spec.extraEnv.size() == 1 && spec.extraEnv[0].value == L"tok" &&
                spec.stopEventName == L"evt" && spec.logPath == in.logPath,
            "BuildAnywhereServerLaunch: argv / environment / log / stop event");
    ServerLaunchSpec wiped = spec;
    wiped.WipeSecrets();
    c.Check(wiped.extraEnv.empty(), "WipeSecrets clears the environment values");
    const ClientLaunchSpec client = BuildAnywhereClientLaunch(L"C:\\bin", L"C:\\proj", "10.0.0.5", 7780, "psess-9");
    c.Check(client.exePath == L"C:\\bin\\Runtime.exe" && client.args == std::vector<std::wstring>{ L"--project", L"C:\\proj", L"--net-connect", L"10.0.0.5:7780", L"--player-session-id", L"psess-9" } &&
                client.workDir == L"C:\\proj",
            "BuildAnywhereClientLaunch: argv and working directory");
    c.Check(ProjectGameLogicPath(L"C:\\proj") == L"C:\\proj\\cache\\GameLogic.dll" &&
                ServerLogPathFor(L"C:\\proj", L"20261127-010203") == L"C:\\proj\\cache\\gamelift\\server-20261127-010203.log",
            "GameLogic.dll and server log paths");

    c.Check(DefaultComputeName(L"my_pc") == "my-pc" && DefaultComputeName(L"a.b") == "a-b" && DefaultComputeName(L"DESKTOP-AB12") == "DESKTOP-AB12" &&
                DefaultComputeName(L"PC_01.x y") == "PC-01-x-y",
            "DefaultComputeName: '_' '.' and other characters become '-'");
    c.Check(DefaultComputeName(L"") == "mye-dev-pc" && DefaultComputeName(L"---") == "mye-dev-pc" && DefaultComputeName(L"日本語") == "mye-dev-pc" &&
                DefaultComputeName(L"_pc") == "pc",
            "DefaultComputeName: empty / no usable character -> mye-dev-pc; a leading symbol is dropped");
    c.Check(IsValidComputeName(DefaultComputeName(L"my_pc")) && IsValidComputeName(DefaultComputeName(std::wstring(200, L'x'))) &&
                DefaultComputeName(std::wstring(200, L'x')).size() == 64,
            "DefaultComputeName always passes the compute-name check (64 characters at most)");

    // 入力欄のバッファ同期: 空の欄にリージョンが外から入っても、欄は新しい値になり、古い (空の) 文字で上書きされない
    char buf[16] = {};
    std::string region;
    c.Check(!SyncTextBuffer(buf, sizeof(buf), region), "SyncTextBuffer: equal (both empty) changes nothing");
    region = "ap-northeast-1";
    c.Check(SyncTextBuffer(buf, sizeof(buf), region) && region == buf, "SyncTextBuffer: an empty field takes the region set from outside (the caller then drops the ImGui edit state)");
    c.Check(!SyncTextBuffer(buf, sizeof(buf), region) && region == "ap-northeast-1", "SyncTextBuffer: once equal, nothing is rewritten (the setting is never overwritten by the field)");
    c.Check(SyncTextBuffer(buf, sizeof(buf), std::string(40, 'x')) && std::string(buf).size() == sizeof(buf) - 1, "SyncTextBuffer: a long value is truncated, not overflowed");

    GameLiftSettings s;
    s.region = "BAD";
    s.locationSuffix = "custom-x";
    s.anywhereFleetName = "a b";
    s.computeName = "a.b";
    s.ipAddress = "1.2";
    s.port = 70000;
    s.maxPlayers = 0;
    s.anywhereFleetId = "-x";
    SanitizeGameLiftSettings(s);
    const GameLiftSettings defaults;
    c.Check(s.region.empty() && s.locationSuffix == defaults.locationSuffix && s.anywhereFleetName == defaults.anywhereFleetName && s.computeName.empty() &&
                s.ipAddress == defaults.ipAddress && s.port == defaults.port && s.maxPlayers == defaults.maxPlayers && s.anywhereFleetId.empty(),
            "SanitizeGameLiftSettings resets every invalid field to its default");
}

// ---------------------------------------------------------------------------------
// Server.exe の子プロセス (偽サーバ = cmd.exe / powershell.exe)
// ---------------------------------------------------------------------------------

// 述語が真になるまで Poll する (limitMs で打ち切り)
template <class Pred>
bool PollUntil(ServerProcess& process, Pred pred, uint64_t limitMs)
{
    const uint64_t begin = GetTickCount64();
    while (GetTickCount64() - begin < limitMs) {
        process.Poll();
        if (pred(process.View())) {
            return true;
        }
        Sleep(5);
    }
    process.Poll();
    return pred(process.View());
}

bool TailContains(const ServerView& view, const char* needle)
{
    for (const std::string& line : view.logTail) {
        if (line.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void TestServerProcess(Counter& c)
{
    const fs::path dir = fs::temp_directory_path() / (L"mye_gl_server_selftest_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const std::wstring eventPrefix = L"mye_gl_selftest_stop_" + std::to_wstring(GetCurrentProcessId()) + L"_";
    std::string error;

    // 1. 環境へのトークンの到達・親の不変・ログのファイル化・ProcessReady の検出・stop イベントが立つ・猶予切れの強制終了
    {
        ServerLaunchSpec spec;
        spec.exePath = L"cmd.exe";
        spec.args = { L"/d", L"/s", L"/c", L"echo token=%MYE_GAMELIFT_AUTH_TOKEN%& echo [gamelift] ProcessReady sent (UDP port 7777)& ping -n 30 127.0.0.1 >nul" };
        spec.extraEnv = { { kServerTokenEnvName, L"tok-selftest-123" } };
        spec.workDir = dir.wstring();
        spec.logPath = (dir / L"logs" / L"a.log").wstring();
        spec.stopEventName = eventPrefix + L"1";
        ServerProcess process;
        c.Check(process.Start(spec, error), "ServerProcess: a child starts (log folder is created)");
        c.Check(process.View().phase == ServerPhase::Starting || process.View().phase == ServerPhase::Ready, "just after Start: starting");
        c.Check(PollUntil(process, [](const ServerView& v) { return v.phase == ServerPhase::Ready; }, 15000), "the ProcessReady line in the log file makes the phase Ready");
        const ServerView ready = process.View();
        c.Check(TailContains(ready, "token=tok-selftest-123"), "the token reached the child's environment");
        c.Check(GetEnvironmentVariableW(kServerTokenEnvName, nullptr, 0) == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND, "the editor's own environment was not changed");
        c.Check(fs::exists(dir / L"logs" / L"a.log") && ready.logPath == spec.logPath, "stdout / stderr go to the log file");
        c.Check(!process.Start(spec, error) && process.View().phase == ServerPhase::Ready, "a second Start while running is refused (the first child is kept)");

        // stop イベント: 子が自分で開く名前付きイベントが立つ。cmd はイベントを見ないので猶予切れで強制終了になる
        const HANDLE observer = OpenEventW(SYNCHRONIZE, FALSE, spec.stopEventName.c_str());
        c.Check(observer != nullptr && WaitForSingleObject(observer, 0) == WAIT_TIMEOUT, "the stop event exists (created before the child starts) and is not signalled yet");
        process.RequestStop(400);
        c.Check(observer != nullptr && WaitForSingleObject(observer, 0) == WAIT_OBJECT_0, "RequestStop signals the stop event");
        c.Check(process.View().phase == ServerPhase::Stopping, "after RequestStop: stopping");
        if (observer != nullptr) {
            CloseHandle(observer);
        }
        c.Check(PollUntil(process, [](const ServerView& v) { return v.phase == ServerPhase::Exited; }, 15000), "a child that ignores the event is terminated after the grace period");
        c.Check(process.View().forcedKill, "the forced termination is reported (forcedKill)");
    }

    // 2. 猶予内に自分で終わる停止 (powershell が stop イベントを待って exit 7)
    {
        ServerLaunchSpec spec;
        const std::wstring eventName = eventPrefix + L"2";
        spec.exePath = L"powershell.exe";
        spec.args = { L"-NoProfile", L"-NonInteractive", L"-Command",
                      L"[Console]::Out.WriteLine('[gamelift] ProcessReady sent'); $e = [Threading.EventWaitHandle]::OpenExisting('" + eventName +
                          L"'); [void]$e.WaitOne(); exit 7" };
        spec.workDir = dir.wstring();
        spec.logPath = (dir / L"b.log").wstring();
        spec.stopEventName = eventName;
        ServerProcess process;
        c.Check(process.Start(spec, error), "ServerProcess: a PowerShell child that waits for the stop event starts");
        c.Check(PollUntil(process, [](const ServerView& v) { return v.phase == ServerPhase::Ready; }, 60000), "the PowerShell child reaches ProcessReady");
        process.RequestStop(30000);
        c.Check(PollUntil(process, [](const ServerView& v) { return v.phase == ServerPhase::Exited; }, 60000), "the child ends after the stop event");
        c.Check(process.View().exitCode == 7 && !process.View().forcedKill,
                "the child exited by itself on the event (exit code 7, no forced termination)");
    }

    // 3. 自然終了: exit code とログの末尾 (Ready にならない)
    {
        ServerLaunchSpec spec;
        spec.exePath = L"cmd.exe";
        spec.args = { L"/d", L"/s", L"/c", L"echo line1& echo line2& exit /b 4" };
        spec.workDir = dir.wstring();
        spec.logPath = (dir / L"c.log").wstring();
        ServerProcess process;
        c.Check(process.Start(spec, error), "ServerProcess: a short child starts");
        c.Check(PollUntil(process, [](const ServerView& v) { return v.phase == ServerPhase::Exited; }, 15000), "a child that ends by itself becomes Exited");
        const ServerView view = process.View();
        c.Check(view.exitCode == 4 && !view.forcedKill && TailContains(view, "line1") && TailContains(view, "line2"), "its exit code and the log tail are kept");
        c.Check(process.Start(spec, error) && PollUntil(process, [](const ServerView& v) { return v.phase == ServerPhase::Exited; }, 15000),
                "after it ended, the same ServerProcess starts again");
    }

    // 4. 起動失敗 / 停止イベント名の衝突 / ブロッキングの停止
    {
        ServerLaunchSpec spec;
        spec.exePath = (dir / L"no_such_server.exe").wstring();
        spec.workDir = dir.wstring();
        spec.logPath = (dir / L"d.log").wstring();
        ServerProcess process;
        c.Check(!process.Start(spec, error) && !error.empty() && process.View().phase == ServerPhase::NotStarted, "a missing exe fails to start with a reason (NotStarted)");

        ServerLaunchSpec slow;
        slow.exePath = L"cmd.exe";
        slow.args = { L"/d", L"/s", L"/c", L"ping -n 30 127.0.0.1 >nul" };
        slow.workDir = dir.wstring();
        slow.logPath = (dir / L"e.log").wstring();
        slow.stopEventName = eventPrefix + L"4";
        ServerProcess blocking;
        c.Check(blocking.Start(slow, error), "ServerProcess: a slow child starts");
        const uint64_t begin = GetTickCount64();
        blocking.StopBlocking(300);
        c.Check(blocking.View().phase == ServerPhase::Exited && blocking.View().forcedKill && GetTickCount64() - begin < 5000,
                "StopBlocking waits for the grace period, then terminates, and returns promptly");

        ServerLaunchSpec noEvent = slow;
        noEvent.stopEventName.clear();
        noEvent.logPath = (dir / L"f.log").wstring();
        ServerProcess direct;
        c.Check(direct.Start(noEvent, error), "ServerProcess: a child without a stop event starts");
        direct.RequestStop(60000);
        c.Check(PollUntil(direct, [](const ServerView& v) { return v.phase == ServerPhase::Exited; }, 15000) && direct.View().forcedKill,
                "without a stop event, RequestStop terminates at once (there is no gentler way)");
    }

    fs::remove_all(dir, ec);
}

} // namespace

bool RunGameLiftFlowSelfTest()
{
    Counter c;
    TestAvailability(c);
    TestPrepare(c);
    TestStartServer(c);
    TestJoin(c);
    TestCleanup(c);
    TestCancelAndBusy(c);
    TestPureFunctions(c);
    if (c.failCount != 0) {
        MYE_LOG_ERROR("gameliftflow: %d check(s) failed", c.failCount);
    }
    return c.failCount == 0;
}

bool RunGameLiftServerSelfTest()
{
    Counter c;
    TestServerProcess(c);
    if (c.failCount != 0) {
        MYE_LOG_ERROR("gameliftserver: %d check(s) failed", c.failCount);
    }
    return c.failCount == 0;
}

} // namespace mye
