//====================================================================================
//                          GameLiftFlowTestDoubles.h
//  MyEngin/ 秋田蓮音                                                       11/27/2026
//                                          GameLift 手順の試験用の偽物 (偽 aws CLI の世界・偽ホスト。自己テストとスクショ検証が使う)
//====================================================================================
#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "Editor/Tools/GameLift/GameLiftHost.h"

namespace mye {
namespace flowtest {

// 偽 CLI が返す 1 回分の応答。polls = 完了するまでに Poll が Running を返す回数
struct FakeReply
{
    uint32_t exitCode = 0;
    std::string out;
    std::string err;
    int polls = 1;
};

// プロセスを起こさない IAwsCliRunner。args に対する応答は respond が決め、呼ばれた argv を全部記録する
class FakeAwsCli : public IAwsCliRunner
{
public:
    bool exeFound = true;
    std::function<FakeReply(const std::vector<std::wstring>&)> respond;
    std::vector<AwsCall> calls;
    uint32_t lastTimeoutMs = 0;
    int clearOutputCount = 0;

    bool Locate(std::wstring& awsPath) override
    {
        awsPath = exeFound ? L"C:\\fake\\aws.exe" : L"";
        return exeFound;
    }
    bool Start(const AwsCall& call, uint32_t timeoutMs, std::string& error) override
    {
        if (running_) {
            error = "a call is already running";
            return false;
        }
        calls.push_back(call);
        lastTimeoutMs = timeoutMs;
        reply_ = respond ? respond(call.args) : FakeReply();
        remaining_ = reply_.polls;
        running_ = true;
        state_ = CliRunState::Running;
        return true;
    }
    CliRunState Poll() override
    {
        if (running_) {
            if (remaining_-- > 0) {
                return CliRunState::Running;
            }
            running_ = false;
            state_ = CliRunState::Finished;
        }
        return state_;
    }
    void Cancel() override
    {
        if (running_) {
            running_ = false;
            state_ = CliRunState::Cancelled;
        }
    }
    uint32_t ExitCode() const override { return reply_.exitCode; }
    uint64_t ElapsedMs() const override { return 0; }
    std::string StdoutText() const override { return reply_.out; }
    std::string StderrText() const override { return reply_.err; }
    void ClearOutput() override
    {
        ++clearOutputCount;
        reply_.out.clear();
        reply_.err.clear();
    }
    bool Running() const { return running_; }

private:
    FakeReply reply_;
    bool running_ = false;
    int remaining_ = 0;
    CliRunState state_ = CliRunState::Idle;
};

// プロセスを起こさない ILocalProcessHost。Server.exe の状態は test が動かす (MakeReady など)
class FakeHost : public ILocalProcessHost
{
public:
    bool serverStartOk = true;
    bool clientStartOk = true;
    int stopDelayPolls = 2; // 停止要求から Exited になるまでの PollServer の回数
    int startCount = 0;
    int stopRequests = 0;
    int blockingStops = 0;
    ServerLaunchSpec lastServerSpec; // 起動時点の写し (本物は起動後に秘密を消す)
    std::vector<ClientLaunchSpec> clients;

    bool StartServer(const ServerLaunchSpec& spec, std::string& error) override
    {
        if (!serverStartOk) {
            error = "fake start failure";
            return false;
        }
        ++startCount;
        lastServerSpec = spec;
        view_ = ServerView();
        view_.phase = ServerPhase::Starting;
        view_.logPath = spec.logPath;
        return true;
    }
    void PollServer() override
    {
        if (view_.phase == ServerPhase::Stopping && --stopPollsLeft_ <= 0) {
            view_.phase = ServerPhase::Exited;
            view_.exitCode = 0;
        }
    }
    ServerView ServerStatus() const override { return view_; }
    void RequestServerStop(uint32_t) override
    {
        if (view_.Active() && view_.phase != ServerPhase::Stopping) {
            ++stopRequests;
            view_.phase = ServerPhase::Stopping;
            stopPollsLeft_ = stopDelayPolls;
        }
    }
    void StopServerBlocking(uint32_t) override
    {
        ++blockingStops;
        if (view_.Active()) {
            view_.phase = ServerPhase::Exited;
        }
    }
    bool LaunchClient(const ClientLaunchSpec& spec, std::string& error) override
    {
        if (!clientStartOk) {
            error = "fake client failure";
            return false;
        }
        clients.push_back(spec);
        return true;
    }

    // test が Server.exe の様子を動かす
    void MakeReady(const std::vector<std::string>& tail = {})
    {
        view_.phase = ServerPhase::Ready;
        view_.logTail = tail;
    }
    void MakeExited(uint32_t exitCode, bool forced, const std::vector<std::string>& tail)
    {
        view_.phase = ServerPhase::Exited;
        view_.exitCode = exitCode;
        view_.forcedKill = forced;
        view_.logTail = tail;
    }
    ServerView& View() { return view_; }

private:
    ServerView view_;
    int stopPollsLeft_ = 0;
};

// aws の応答を模した「世界」。状態を持ち、argv に応じて公式の応答例の形の JSON か型付きエラーを返す。
// 作成系の呼び出しは数える (冪等さの検査用)
struct FakeAwsWorld
{
    // 資格情報・CLI
    bool v1 = false;
    bool loggedIn = true;
    std::string configRegion = "ap-northeast-1"; // 空 = `aws configure get region` が失敗
    // AWS 側の状態
    std::vector<std::string> locations;
    std::string fleetId;               // 空 = フリート無し
    bool fleetDeleting = false;
    int deletingDescribesLeft = 0;     // delete-fleet の後、describe が「まだある (DELETING)」を返す回数
    bool computeRegistered = false;
    std::string sessionId;             // 空 = ゲームセッション無し
    std::string sessionStatus;         // "ACTIVATING" / "ACTIVE"
    int capacityFailuresLeft = 0;      // create-game-session が FleetCapacityExceeded を返す回数
    int activatingDescribesLeft = 0;   // 作成直後の describe が ACTIVATING を返す回数
    int playerSessions = 0;
    // 作成系の呼び出し回数
    int createLocationCalls = 0;
    int createFleetCalls = 0;
    int registerComputeCalls = 0;
    int createGameSessionCalls = 0;

    static std::string ArgValue(const std::vector<std::wstring>& args, const wchar_t* name)
    {
        for (size_t i = 0; i + 1 < args.size(); ++i) {
            if (args[i] == name) {
                return std::string(args[i + 1].begin(), args[i + 1].end());
            }
        }
        return std::string();
    }

    static std::string TypedError(const char* code, const char* op, const char* message)
    {
        return std::string("\nAn error occurred (") + code + ") when calling the " + op + " operation: " + message + "\n";
    }

    FakeReply Fail(const char* code, const char* op, const char* message) const
    {
        FakeReply r;
        r.exitCode = 254;
        r.err = TypedError(code, op, message);
        return r;
    }

    FakeReply operator()(const std::vector<std::wstring>& args)
    {
        FakeReply r;
        if (args.empty()) {
            return r;
        }
        if (args[0] == L"--version") {
            if (v1) {
                r.err = "aws-cli/1.27.1 Python/3.8.10 Windows/10 botocore/1.29.1\r\n";
            } else {
                r.out = "aws-cli/2.37.8 Python/3.13.9 Windows/11 exe/AMD64\r\n";
            }
            return r;
        }
        if (args[0] == L"configure") {
            if (configRegion.empty()) {
                r.exitCode = 1;
            } else {
                r.out = configRegion + "\r\n";
            }
            return r;
        }
        if (args[0] == L"sts") {
            if (!loggedIn) {
                return Fail("ExpiredToken", "GetCallerIdentity", "The security token included in the request is expired");
            }
            r.out = R"({"UserId":"AIDAEXAMPLE","Account":"123456789012","Arn":"arn:aws:iam::123456789012:user/dev"})";
            return r;
        }
        if (!loggedIn) {
            return Fail("ExpiredTokenException", "GameLift", "The security token included in the request is expired");
        }
        const std::string op(args[1].begin(), args[1].end());
        const std::string fleetArg = ArgValue(args, L"--fleet-id");
        if (op == "list-locations") {
            r.out = R"({"Locations":[)";
            for (size_t i = 0; i < locations.size(); ++i) {
                r.out += std::string(i ? "," : "") + R"({"LocationName":")" + locations[i] + R"("})";
            }
            r.out += "]}";
        } else if (op == "create-location") {
            ++createLocationCalls;
            const std::string name = ArgValue(args, L"--location-name");
            if (std::find(locations.begin(), locations.end(), name) != locations.end()) {
                return Fail("ConflictException", "CreateLocation", "exists");
            }
            locations.push_back(name);
            r.out = R"({"Location":{"LocationName":")" + name + R"("}})";
        } else if (op == "delete-location") {
            const std::string name = ArgValue(args, L"--location-name");
            const auto it = std::find(locations.begin(), locations.end(), name);
            if (it == locations.end()) {
                return Fail("NotFoundException", "DeleteLocation", "no such location");
            }
            locations.erase(it);
        } else if (op == "create-fleet") {
            ++createFleetCalls;
            fleetId = "fleet-1111aaaa-2222-3333-4444-555566667777";
            r.out = R"({"FleetAttributes":{"FleetId":")" + fleetId + R"(","Name":")" + ArgValue(args, L"--name") +
                    R"(","Status":"NEW","ComputeType":"ANYWHERE"}})";
        } else if (op == "describe-fleet-attributes") {
            const std::string wanted = ArgValue(args, L"--fleet-ids");
            if (fleetId.empty() || wanted != fleetId) {
                r.out = R"({"FleetAttributes":[]})";
            } else if (fleetDeleting) {
                if (deletingDescribesLeft-- > 0) {
                    r.out = R"({"FleetAttributes":[{"FleetId":")" + fleetId + R"(","Status":"DELETING"}]})";
                } else {
                    fleetId.clear();
                    fleetDeleting = false;
                    r.out = R"({"FleetAttributes":[]})";
                }
            } else {
                r.out = R"({"FleetAttributes":[{"FleetId":")" + fleetId + R"(","Name":"mye-anywhere","Status":"ACTIVE"}]})";
            }
        } else if (op == "delete-fleet") {
            if (fleetId.empty() || fleetArg != fleetId) {
                return Fail("NotFoundException", "DeleteFleet", "no such fleet");
            }
            fleetDeleting = true;
        } else if (op == "register-compute") {
            ++registerComputeCalls;
            computeRegistered = true;
            r.out = ComputeJson(ArgValue(args, L"--compute-name"));
        } else if (op == "describe-compute") {
            if (!computeRegistered) {
                return Fail("NotFoundException", "DescribeCompute", "compute not found");
            }
            r.out = ComputeJson(ArgValue(args, L"--compute-name"));
        } else if (op == "deregister-compute") {
            if (!computeRegistered) {
                return Fail("NotFoundException", "DeregisterCompute", "compute not found");
            }
            computeRegistered = false;
        } else if (op == "get-compute-auth-token") {
            r.out = R"({"FleetId":")" + fleetArg + R"(","ComputeName":")" + ArgValue(args, L"--compute-name") +
                    R"(","AuthToken":"SECRET-TOKEN-0123456789","ExpirationTimeStamp":"2026-11-27T10:15:00+09:00"})";
        } else if (op == "create-game-session") {
            ++createGameSessionCalls;
            if (capacityFailuresLeft > 0) {
                --capacityFailuresLeft;
                return Fail("FleetCapacityExceededException", "CreateGameSession", "No active and available server processes");
            }
            sessionId = "arn:aws:gamelift:ap-northeast-1::gamesession/" + fleetArg + "/custom-mye-dev/gsess-1";
            sessionStatus = "ACTIVATING";
            r.out = R"({"GameSession":)" + SessionJson() + "}";
        } else if (op == "describe-game-sessions") {
            if (sessionId.empty()) {
                r.out = R"({"GameSessions":[]})";
            } else if (!ArgValue(args, L"--game-session-id").empty()) {
                if (sessionStatus == "ACTIVATING" && activatingDescribesLeft-- <= 0) {
                    sessionStatus = "ACTIVE";
                }
                r.out = R"({"GameSessions":[)" + SessionJson() + "]}";
            } else if (!ArgValue(args, L"--status-filter").empty() && sessionStatus != "ACTIVE") {
                r.out = R"({"GameSessions":[]})";
            } else {
                r.out = R"({"GameSessions":[)" + SessionJson() + "]}";
            }
        } else if (op == "create-player-session") {
            ++playerSessions;
            r.out = R"({"PlayerSession":{"PlayerSessionId":"psess-)" + std::to_string(playerSessions) +
                    R"(","PlayerId":")" + ArgValue(args, L"--player-id") + R"(","GameSessionId":")" + sessionId +
                    R"(","FleetId":")" + fleetId + R"(","Status":"RESERVED","IpAddress":"127.0.0.1","Port":7777}})";
        } else {
            return Fail("InvalidRequestException", op.c_str(), "the fake world does not know this operation");
        }
        return r;
    }

    std::string ComputeJson(const std::string& name) const
    {
        return R"({"Compute":{"FleetId":")" + fleetId + R"(","ComputeName":")" + name +
               R"(","IpAddress":"127.0.0.1","ComputeStatus":"Active","Location":"custom-mye-dev","Type":"ANYWHERE","GameLiftServiceSdkEndpoint":"wss://ap-northeast-1.api.amazongamelift.com"}})";
    }

    std::string SessionJson() const
    {
        return R"({"GameSessionId":")" + sessionId + R"(","FleetId":")" + fleetId + R"(","Status":")" + sessionStatus +
               R"(","Location":"custom-mye-dev","IpAddress":"127.0.0.1","Port":7777,"MaximumPlayerSessionCount":4})";
    }
};

} // namespace flowtest
} // namespace mye
