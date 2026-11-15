//====================================================================================
//                          TrackClient.cpp
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          追跡サービスの pipe クライアントの実装
//====================================================================================
#include "Editor/ChangeTracking/TrackClient.h"

#include <algorithm>
#include <vector>

#include "Editor/ChangeTracking/TrackProjectId.h"
#include "Editor/ChangeTracking/TrackProtocol.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {

namespace {

// 状態の変化は 1 行ずつ。再接続のたびに WARN を出さない (WARN はトーストに集まる)
const char* kLogTag = "[track]";

std::string DumpJson(const nlohmann::json& j)
{
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

std::string ErrorCodeOf(const nlohmann::json& response)
{
    if (response.is_object() && response.contains("error") && response["error"].is_object()
        && response["error"].contains("code") && response["error"]["code"].is_string()) {
        return response["error"]["code"].get<std::string>();
    }
    return {};
}

} // namespace

const char* TrackStateName(TrackState state)
{
    switch (state) {
    case TrackState::Disabled: return "Disabled";
    case TrackState::NoProject: return "NoProject";
    case TrackState::Connecting: return "Connecting";
    case TrackState::Ready: return "Ready";
    case TrackState::NoService: return "NoService";
    case TrackState::ProtoMismatch: return "ProtoMismatch";
    case TrackState::WrongProject: return "WrongProject";
    case TrackState::DbDegraded: return "DbDegraded";
    case TrackState::Disconnected: return "Disconnected";
    }
    return "?";
}

int64_t TrackOpTimeoutMs(const std::string& op)
{
    // 一覧・詳細の取得は重くなりうるので 30 秒。それ以外 (hello・subscribe・記録) は 5 秒
    if (op == trackop::kStatus || op == trackop::kList || op == trackop::kDescribe || op == trackop::kSessions
        || op == trackop::kHunks || op == "scene_diff" || op == trackop::kBlobGet) {
        return kTrackReadTimeoutMs;
    }
    return kTrackRecordTimeoutMs;
}

nlohmann::json TrackClient::MakeFailure(const char* code, const std::string& detail)
{
    return nlohmann::json{ { "ok", false }, { "error", { { "code", code }, { "detail", detail } } } };
}

TrackClient::~TrackClient()
{
    Stop();
}

void TrackClient::Start(std::unique_ptr<TrackIo> io, const std::wstring& projectRoot, uint32_t pid)
{
    Stop();
    io_ = std::move(io);
    projectRoot_ = projectRoot;
    pid_ = pid;
    projectId_ = ProjectId(projectRoot);
    pipeName_ = TrackPipeName(projectId_);
    everReady_ = false;
    launchRequested_ = false;
    badTokenRetried_ = false;
    busyCount_ = 0;
    nextAttemptMs_ = 0;
    reason_.clear();
    if (pipeName_.empty()) {
        // SHA-256 を使えない環境では pipe 名を作れない。I/O はせずに無効のままにする
        reason_ = "projectId を計算できない";
        SetState(TrackState::Disabled);
        return;
    }
    SetState(TrackState::Connecting);
}

void TrackClient::SetIdle(TrackState state)
{
    Stop();
    SetState(state);
}

void TrackClient::Stop()
{
    if (io_) {
        CloseConnection(trackerror::kDisconnected);
        io_->ClosePipe();
    }
    io_.reset();
    helloOk_ = false;
}

void TrackClient::SetState(TrackState state)
{
    if (state == state_) {
        return;
    }
    MYE_LOG_INFO("%s %s -> %s%s%s", kLogTag, TrackStateName(state_), TrackStateName(state),
                 reason_.empty() ? "" : " : ", reason_.c_str());
    state_ = state;
    if (onState_) {
        onState_(state);
    }
}

void TrackClient::SetDbDegraded(bool degraded)
{
    if (!helloOk_) {
        return;
    }
    if (degraded && state_ == TrackState::Ready) {
        reason_ = "db_degraded";
        SetState(TrackState::DbDegraded);
    } else if (!degraded && state_ == TrackState::DbDegraded) {
        reason_.clear();
        SetState(TrackState::Ready);
    }
}

void TrackClient::Poll()
{
    if (!io_) {
        return;
    }
    switch (state_) {
    case TrackState::Disabled:
    case TrackState::NoProject:
    case TrackState::NoService:
    case TrackState::ProtoMismatch:
    case TrackState::WrongProject:
        return; // I/O をしない
    default:
        break;
    }
    const int64_t now = io_->NowMs();
    stats_.lastPollLines = 0;
    if (!connected_) {
        if (now >= nextAttemptMs_) {
            TryConnect(now);
        }
        return;
    }

    // pipe から読む。溜まりすぎていれば読まない (サービスの書き込みが止まるだけで、記録は止まらない)
    for (int i = 0; i < kTrackMaxPumpsPerPoll && !peerClosed_ && rx_.size() - rxPos_ < kTrackRxHighWater; ++i) {
        const size_t before = rx_.size();
        if (io_->Pump(rx_) == TrackIo::PumpResult::Closed) {
            peerClosed_ = true;
        }
        if (rx_.size() == before) {
            break;
        }
    }

    // 行に分けて配る。行数と時間に上限を持ち、残りは次のフレームへ
    const auto started = std::chrono::steady_clock::now();
    while (stats_.lastPollLines < static_cast<uint32_t>(kTrackMaxLinesPerPoll)) {
        const size_t newline = rx_.find('\n', rxPos_);
        if (newline == std::string::npos) {
            if (rx_.size() - rxPos_ > static_cast<size_t>(kTrackMaxLineBytes)) {
                Disconnect("line_too_long");
                return;
            }
            break;
        }
        if (newline - rxPos_ > static_cast<size_t>(kTrackMaxLineBytes)) {
            Disconnect("line_too_long");
            return;
        }
        const std::string line = rx_.substr(rxPos_, newline - rxPos_);
        rxPos_ = newline + 1;
        ++stats_.lastPollLines;
        DispatchLine(line);
        if (!connected_) {
            return; // 配った先 (hello の失敗など) が接続を閉じた
        }
        if (std::chrono::steady_clock::now() - started > kTrackPollBudget) {
            break;
        }
    }
    // 処理済みの先頭を捨てる (毎行 erase せず、まとめて)
    if (rxPos_ > 0 && (rxPos_ >= rx_.size() || rxPos_ > rx_.size() / 2)) {
        rx_.erase(0, rxPos_);
        rxPos_ = 0;
    }

    ExpireTimedOut(now);
    if (connected_ && peerClosed_ && !HasCompleteLine()) {
        Disconnect("closed_by_service");
    }
}

bool TrackClient::HasCompleteLine() const
{
    return rx_.find('\n', rxPos_) != std::string::npos;
}

void TrackClient::TryConnect(int64_t now)
{
    switch (io_->OpenPipe(pipeName_)) {
    case TrackIo::OpenResult::NotFound: {
        busyCount_ = 0;
        if (!launchRequested_) {
            // 起動は 1 回だけ頼む。以後は pipe が現れるのを見に行くだけ (起動に失敗し続けるサービスを呼び続けない)
            launchRequested_ = true;
            const TrackIo::LaunchResult launched = io_->LaunchService(projectRoot_);
            if (launched == TrackIo::LaunchResult::NoExe) {
                reason_ = "mye_track.exe が見つからない";
                SetState(TrackState::NoService);
                return;
            }
            if (launched == TrackIo::LaunchResult::Launched) {
                launchedAtMs_ = now;
            }
        }
        const bool inGrace = launchedAtMs_ != 0 && now - launchedAtMs_ < kTrackLaunchGraceMs;
        nextAttemptMs_ = now + (inGrace ? kTrackAfterLaunchRetryMs : kTrackReconnectIntervalMs);
        return;
    }
    case TrackIo::OpenResult::Busy:
        // 待たずに次のフレームで開き直す。続くなら間隔をあける
        ++busyCount_;
        nextAttemptMs_ = busyCount_ < kTrackBusyRetryLimit ? now : now + kTrackReconnectIntervalMs;
        return;
    case TrackIo::OpenResult::Failed:
        nextAttemptMs_ = now + kTrackReconnectIntervalMs;
        return;
    case TrackIo::OpenResult::Opened:
        break;
    }
    busyCount_ = 0;

    // トークンは接続のたびに読み直す (サービスが再起動すると変わる)
    std::string token;
    if (!io_->ReadServiceToken(projectRoot_, token)) {
        io_->ClosePipe();
        nextAttemptMs_ = now + kTrackAfterLaunchRetryMs; // service.json はサービスが pipe の後に書く
        return;
    }
    connected_ = true;
    helloOk_ = false;
    peerClosed_ = false;
    rx_.clear();
    rxPos_ = 0;

    const uint64_t id = nextId_++;
    helloId_ = id;
    const nlohmann::json hello = {
        { "id", id },
        { "op", trackop::kHello },
        { "args", { { "proto", kTrackProtoVersion }, { "role", "editor" }, { "token", token },
                    { "projectRoot", WideToUtf8(projectRoot_) }, { "pid", pid_ } } },
    };
    if (!io_->Write(DumpJson(hello) + "\n")) {
        Disconnect("write_failed");
        return;
    }
    pending_[id] = Pending{ [this](const nlohmann::json& response) { OnHello(response); },
                            now + TrackOpTimeoutMs(trackop::kHello), trackop::kHello };
}

void TrackClient::OnHello(const nlohmann::json& response)
{
    helloId_ = 0;
    if (response.value("ok", false) && response.contains("result") && response["result"].is_object()) {
        const std::string theirs = response["result"].value("projectId", std::string());
        if (theirs != projectId_) {
            FinalClose(TrackState::WrongProject, "projectId が違う: " + theirs);
            return;
        }
        helloOk_ = true;
        everReady_ = true;
        launchRequested_ = false;
        badTokenRetried_ = false;
        launchedAtMs_ = 0;
        reason_.clear();
        ++stats_.connects;
        SetState(TrackState::Ready);
        return;
    }
    const std::string code = ErrorCodeOf(response);
    if (code == trackerror::kProtoMismatch) {
        FinalClose(TrackState::ProtoMismatch, "proto_mismatch");
    } else if (code == trackerror::kWrongProject) {
        FinalClose(TrackState::WrongProject, "wrong_project");
    } else if (code == trackerror::kBadToken) {
        if (!badTokenRetried_) {
            // サービスが再起動してトークンが変わった。service.json を読み直して 1 回だけやり直す
            badTokenRetried_ = true;
            CloseConnection(trackerror::kDisconnected);
            nextAttemptMs_ = 0;
        } else {
            badTokenRetried_ = false;
            Disconnect("bad_token");
        }
    } else if (code == trackerror::kDbDegraded) {
        CloseConnection(trackerror::kDisconnected);
        reason_ = "db_degraded";
        SetState(TrackState::DbDegraded);
        nextAttemptMs_ = io_->NowMs() + kTrackReconnectIntervalMs;
    } else {
        Disconnect(code.empty() ? "hello_failed" : code);
    }
}

void TrackClient::CloseConnection(const char* failCode)
{
    if (io_ && connected_) {
        io_->ClosePipe();
    }
    connected_ = false;
    helloOk_ = false;
    peerClosed_ = false;
    rx_.clear();
    rxPos_ = 0;
    // 保留中の要求は全部失敗で返す。コールバックが Request を呼んでもよいよう、先に手元へ移す
    std::map<uint64_t, Pending> failed;
    failed.swap(pending_);
    const uint64_t hello = helloId_;
    helloId_ = 0;
    for (auto& [id, pending] : failed) {
        if (id != hello && pending.fn) {
            pending.fn(MakeFailure(failCode, "接続が切れた"));
        }
    }
}

void TrackClient::Disconnect(const std::string& reason)
{
    const bool wasUp = everReady_ || state_ == TrackState::Ready || state_ == TrackState::DbDegraded;
    CloseConnection(trackerror::kDisconnected);
    reason_ = reason;
    nextAttemptMs_ = (io_ ? io_->NowMs() : 0) + kTrackReconnectIntervalMs;
    SetState(wasUp ? TrackState::Disconnected : TrackState::Connecting);
}

void TrackClient::FinalClose(TrackState state, const std::string& reason)
{
    CloseConnection(trackerror::kDisconnected);
    reason_ = reason;
    SetState(state);
}

uint64_t TrackClient::Request(const char* op, const nlohmann::json& args, ResponseFn onDone)
{
    if (!io_ || !helloOk_) {
        if (onDone) {
            onDone(MakeFailure(trackerror::kNotConnected, "サービスに繋がっていない"));
        }
        return 0;
    }
    const uint64_t id = nextId_++;
    const nlohmann::json request = { { "id", id }, { "op", op }, { "args", args.is_null() ? nlohmann::json::object() : args } };
    if (!io_->Write(DumpJson(request) + "\n")) {
        Disconnect("write_failed");
        if (onDone) {
            onDone(MakeFailure(trackerror::kDisconnected, "送れなかった"));
        }
        return 0;
    }
    pending_[id] = Pending{ std::move(onDone), io_->NowMs() + TrackOpTimeoutMs(op), op };
    return id;
}

void TrackClient::DispatchLine(const std::string& rawLine)
{
    std::string line = rawLine;
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    if (line.find_first_not_of(" \t") == std::string::npos) {
        return; // 空行は無視する
    }
    ++stats_.lines;
    const nlohmann::json message = nlohmann::json::parse(line, nullptr, false);
    if (!message.is_object()) {
        ++stats_.badLines;
        return;
    }
    if (message.contains("id") && message["id"].is_number_unsigned()) {
        const uint64_t id = message["id"].get<uint64_t>();
        const auto found = pending_.find(id);
        if (found == pending_.end()) {
            ++stats_.lateResponses; // 時間切れで失敗を返したあとの応答
            return;
        }
        ResponseFn fn = std::move(found->second.fn);
        pending_.erase(found);
        if (fn) {
            fn(message);
        }
        return;
    }
    if (message.contains("event") && message["event"].is_string()) {
        if (message["event"].get<std::string>() == trackevent::kShuttingDown) {
            // サービスが自分で (または利用者が) 止まる。そのあとの切断で起動し直さない
            // (止めた意図を覆さない。居なくなった pipe を見に行くだけは続ける)
            launchRequested_ = true;
        }
        if (onNotify_) {
            onNotify_(message);
        }
        return;
    }
    ++stats_.badLines;
}

void TrackClient::ExpireTimedOut(int64_t now)
{
    std::vector<uint64_t> expired;
    for (const auto& [id, pending] : pending_) {
        if (pending.deadlineMs <= now) {
            expired.push_back(id);
        }
    }
    for (const uint64_t id : expired) {
        const auto found = pending_.find(id);
        if (found == pending_.end()) {
            continue; // 先のコールバックが片付けた
        }
        ResponseFn fn = std::move(found->second.fn);
        const std::string op = found->second.op;
        pending_.erase(found);
        ++stats_.timeouts;
        if (fn) {
            fn(MakeFailure(trackerror::kTimeout, op + " が時間内に返らない"));
        }
        if (!connected_) {
            return; // hello の時間切れで切れた
        }
    }
}

} // namespace mye
