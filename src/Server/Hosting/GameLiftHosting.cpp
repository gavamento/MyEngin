//====================================================================================
//                          GameLiftHosting.cpp
//  MyEngin/ 秋田蓮音                                                     10/02/2026
//                                          AWS GameLift 向けホスティングの実装
//====================================================================================
#include "Server/Hosting/GameLiftHosting.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <deque>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {
namespace {

// 締め切り / 予約期間の上書き値の許容範囲 (範囲外は無視して既定のまま進める)
constexpr uint32_t kMaxDeadlineTicks = 600;
constexpr uint32_t kMaxRejoinTimeoutTicks = 60 * 60 * 60; // 1 時間

uint64_t SteadyNowMs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

// "123" だけを受ける (符号・空白・余り文字は不可)
bool ParseTicks(const std::string& s, uint32_t maxValue, uint32_t& out)
{
    if (s.empty() || s.size() > 9) {
        return false;
    }
    uint32_t v = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') {
            return false;
        }
        v = v * 10 + static_cast<uint32_t>(c - '0');
    }
    if (v == 0 || v > maxValue) {
        return false;
    }
    out = v;
    return true;
}

} // namespace

// SDK のスレッドとメインスレッドが共有するもの。コールバックの lambda が shared_ptr で持つので、
// GameLiftHosting が先に消えても SDK のスレッドが読んで安全
struct GameLiftHosting::Shared {
    struct Item {
        bool terminate = false;
        GameLiftGameSession session;
    };
    std::mutex mutex;
    std::deque<Item> queue;
    std::atomic<uint64_t> lastPollMs{ 0 };
    uint32_t healthStaleMs = 30000;
    std::function<uint64_t()> nowMs;
};

GameLiftHosting::GameLiftHosting(std::unique_ptr<IGameLiftSdk> sdk, GameLiftHostingOptions options)
    : sdk_(std::move(sdk)), opt_(std::move(options)), shared_(std::make_shared<Shared>())
{
    shared_->healthStaleMs = opt_.healthStaleMs;
    shared_->nowMs = opt_.nowMs ? opt_.nowMs : std::function<uint64_t()>(&SteadyNowMs);
}

GameLiftHosting::~GameLiftHosting()
{
    Shutdown();
}

uint64_t GameLiftHosting::NowMs() const
{
    return shared_->nowMs();
}

bool GameLiftHosting::Fail(const char* what, const GameLiftResult& r)
{
    lastError_ = std::string(what) + " failed: " + (r.error.empty() ? "unknown error" : r.error);
    MYE_LOG_ERROR("[gamelift] %s", lastError_.c_str());
    return false;
}

bool GameLiftHosting::Init()
{
    shared_->lastPollMs.store(NowMs());
    const GameLiftResult r = sdk_->InitSdk(opt_.connection);
    if (!r.ok) {
        return Fail("InitSDK", r);
    }
    initialised_ = true;
    MYE_LOG_INFO("[gamelift] SDK initialised (fleet %s, host %s, process %s)", opt_.connection.fleetId.c_str(),
                 opt_.connection.hostId.c_str(), opt_.connection.processId.c_str());
    return true;
}

bool GameLiftHosting::NotifyReady(uint16_t port, const std::vector<std::wstring>& logPaths)
{
    if (!initialised_) {
        lastError_ = "NotifyReady before a successful Init";
        return false;
    }
    std::vector<std::string> paths;
    paths.reserve(logPaths.size());
    for (const std::wstring& p : logPaths) {
        paths.push_back(WideToUtf8(p));
    }
    // コールバックは SDK のスレッドで走る。触るのは shared だけ (キューへ積む / 時刻を読む)
    const std::shared_ptr<Shared> shared = shared_;
    GameLiftCallbacks cb;
    cb.onStartGameSession = [shared](GameLiftGameSession session) {
        std::lock_guard<std::mutex> lock(shared->mutex);
        Shared::Item item;
        item.session = std::move(session);
        shared->queue.push_back(std::move(item));
    };
    cb.onProcessTerminate = [shared]() {
        std::lock_guard<std::mutex> lock(shared->mutex);
        Shared::Item item;
        item.terminate = true;
        shared->queue.push_back(std::move(item));
    };
    cb.onHealthCheck = [shared]() {
        const uint64_t last = shared->lastPollMs.load();
        const uint64_t now = shared->nowMs();
        return now < last || now - last <= shared->healthStaleMs;
    };
    shared_->lastPollMs.store(NowMs());
    const GameLiftResult r = sdk_->ProcessReady(port, paths, cb);
    if (!r.ok) {
        return Fail("ProcessReady", r);
    }
    readySent_ = true;
    MYE_LOG_INFO("[gamelift] ProcessReady sent (UDP port %u, %zu log path(s)); waiting for a game session", port,
                 paths.size());
    return true;
}

void GameLiftHosting::Poll(std::vector<HostingEvent>& out)
{
    shared_->lastPollMs.store(NowMs());
    std::deque<Shared::Item> items;
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        items.swap(shared_->queue);
    }
    for (Shared::Item& item : items) {
        if (item.terminate) {
            if (!terminateEmitted_) {
                terminateEmitted_ = true;
                MYE_LOG_INFO("[gamelift] OnProcessTerminate received");
                HostingEvent e;
                e.kind = HostingEventKind::Terminate;
                out.push_back(e);
            }
            continue;
        }
        const GameLiftGameSession& gs = item.session;
        if (sessionStarted_) {
            MYE_LOG_WARN("[gamelift] a second game session (%s) was offered while one is running; ignored",
                         gs.gameSessionId.c_str());
            continue;
        }
        // 人数: GameSession の最大人数とレーン数の小さい方を入場の上限にする。
        // レーン数は sim の構築時に決まっていて広げられない (大きい方は使い切れない)
        admissionLimit_ = opt_.laneCount;
        if (gs.maxPlayers > 0 && static_cast<uint32_t>(gs.maxPlayers) < admissionLimit_) {
            admissionLimit_ = static_cast<uint32_t>(gs.maxPlayers);
        } else if (gs.maxPlayers > static_cast<int>(opt_.laneCount)) {
            MYE_LOG_WARN("[gamelift] game session allows %d players but the server has %u lane(s) (--max-players); "
                         "extra players will be rejected",
                         gs.maxPlayers, opt_.laneCount);
        }
        HostingSessionRequest req;
        for (const auto& kv : gs.properties) {
            uint32_t v = 0;
            if (kv.first == kGameLiftPropDeadlineTicks) {
                if (ParseTicks(kv.second, kMaxDeadlineTicks, v)) {
                    req.deadlineTicks = v;
                } else {
                    MYE_LOG_WARN("[gamelift] game property %s='%s' is not 1..%u; ignored", kv.first.c_str(),
                                 kv.second.c_str(), kMaxDeadlineTicks);
                }
            } else if (kv.first == kGameLiftPropRejoinTimeoutTicks) {
                if (ParseTicks(kv.second, kMaxRejoinTimeoutTicks, v)) {
                    req.rejoinTimeoutTicks = v;
                } else {
                    MYE_LOG_WARN("[gamelift] game property %s='%s' is not 1..%u; ignored", kv.first.c_str(),
                                 kv.second.c_str(), kMaxRejoinTimeoutTicks);
                }
            }
        }
        const GameLiftResult r = sdk_->ActivateGameSession();
        if (!r.ok) {
            // 活性化できないセッションは使えない。プロセスを畳んで GameLift に別のプロセスを割り当てさせる
            Fail("ActivateGameSession", r);
            if (!terminateEmitted_) {
                terminateEmitted_ = true;
                HostingEvent e;
                e.kind = HostingEventKind::Terminate;
                out.push_back(e);
            }
            continue;
        }
        sessionStarted_ = true;
        MYE_LOG_INFO("[gamelift] game session %s activated (max players %d, admission limit %u, deadline %u, rejoin timeout %u)",
                     gs.gameSessionId.c_str(), gs.maxPlayers, admissionLimit_, req.deadlineTicks,
                     req.rejoinTimeoutTicks);
        HostingEvent e;
        e.kind = HostingEventKind::StartSession;
        e.session = req;
        out.push_back(e);
    }
}

bool GameLiftHosting::ValidatePlayer(const char* playerSessionId)
{
    if (playerSessionId == nullptr || playerSessionId[0] == '\0') {
        MYE_LOG_WARN("[gamelift] a client connected without a player session ID; rejected");
        return false;
    }
    const std::string id = playerSessionId;
    if (accepted_.count(id) != 0) {
        return true; // Hello の再送、または予約中の再接続。GameLift では承認済み (ACTIVE)
    }
    if (accepted_.size() >= admissionLimit_) {
        MYE_LOG_WARN("[gamelift] player session %s rejected: the game session is full (%u)", id.c_str(),
                     admissionLimit_);
        return false;
    }
    // ★同期呼び出し (SDK が WebSocket の応答を待つ)。失敗時は SDK 内で再試行するため長引きうる
    const GameLiftResult r = sdk_->AcceptPlayerSession(id);
    if (!r.ok) {
        MYE_LOG_WARN("[gamelift] AcceptPlayerSession(%s) failed: %s", id.c_str(), r.error.c_str());
        return false;
    }
    accepted_.insert(id);
    return true;
}

void GameLiftHosting::PlayerLeft(const char* playerSessionId)
{
    // 一時的な切断。player session は ACTIVE のまま残し、再接続を待つ (解放は PlayerReleased)
    MYE_LOG_INFO("[gamelift] player session %s disconnected (kept for reconnection)",
                 playerSessionId != nullptr ? playerSessionId : "");
}

void GameLiftHosting::PlayerReleased(const char* playerSessionId)
{
    if (playerSessionId == nullptr) {
        return;
    }
    const std::string id = playerSessionId;
    if (accepted_.erase(id) == 0) {
        return; // 承認していない ID (拒否された参加者など)
    }
    const GameLiftResult r = sdk_->RemovePlayerSession(id);
    if (!r.ok) {
        MYE_LOG_WARN("[gamelift] RemovePlayerSession(%s) failed: %s", id.c_str(), r.error.c_str());
    }
}

void GameLiftHosting::NotifySessionEnded()
{
    if (endingSent_ || !readySent_) {
        return;
    }
    endingSent_ = true;
    const GameLiftResult r = sdk_->ProcessEnding();
    if (!r.ok) {
        Fail("ProcessEnding", r);
    } else {
        MYE_LOG_INFO("[gamelift] ProcessEnding sent");
    }
}

void GameLiftHosting::Shutdown()
{
    if (destroyed_ || sdk_ == nullptr) {
        return;
    }
    NotifySessionEnded(); // 終了通知前に畳まれる経路 (起動直後の Terminate など) でも ProcessEnding を欠かさない
    destroyed_ = true;
    if (initialised_) {
        const GameLiftResult r = sdk_->Destroy();
        if (!r.ok) {
            Fail("Destroy", r);
        }
    }
}

} // namespace mye
