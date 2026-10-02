//====================================================================================
//                          ServerLoop.cpp
//  MyEngin/ 秋田蓮音                                                     10/02/2026
//                                          専用サーバの実運用ループの実装
//====================================================================================
#include "Server/ServerLoop.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <vector>

#include <Windows.h>
#include <timeapi.h>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Util/Hash.h"
#include "Engine/Core/Util/Random.h"
#include "Engine/Platform/CrashHandler.h"
#include "Engine/Engine/Loop/HeadlessSim.h"
#include "Engine/Engine/Net/NetProtocol.h"
#include "Engine/Engine/Net/ServerSession.h"
#include "Engine/Engine/Replay/Replay.h"
#include "Engine/Engine/Replay/SimSnapshot.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Platform/Net/UdpSocket.h"
#include "Engine/Platform/PathUtil.h"
#include "Server/Hosting/IHostingProvider.h"

namespace mye {
namespace {

using SteadyClock = std::chrono::steady_clock;

constexpr int kMaxConfirmsPerIteration = 8;    // 1 周で追いつく tick 数の上限 (受信と送信を飢えさせない)
constexpr int kMaxDatagramsPerIteration = 512;

// 終了時に timeBeginPeriod を必ず戻す (スリープ精度を 1ms にする間だけ)
struct TimerResolutionScope {
    TimerResolutionScope() { active = (timeBeginPeriod(1) == TIMERR_NOERROR); }
    ~TimerResolutionScope()
    {
        if (active) {
            timeEndPeriod(1);
        }
    }
    bool active = false;
};

uint64_t NowMs(SteadyClock::time_point origin)
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - origin).count());
}

// 前回実行の残党パケットを弾くためのセッション ID (実時間由来で良い: ネットのハンドシェイクの値で sim には入らない)
uint64_t MakeSessionId(uint16_t port)
{
    const uint64_t t = static_cast<uint64_t>(std::chrono::system_clock::now().time_since_epoch().count());
    const uint64_t id = HashBytes(&t, sizeof(t), HashBytes(&port, sizeof(port)));
    return id == 0 ? 1 : id;
}

} // namespace

uint32_t PeerTable::KeyOf(const NetAddress& from, bool create)
{
    for (const Entry& e : entries_) {
        if (e.addr == from) {
            return e.key;
        }
    }
    if (!create) {
        return 0;
    }
    if (entries_.size() >= kMaxAddrs) {
        ++ignoredWhileFull_;
        if (!fullReported_) {
            fullReported_ = true;
            MYE_LOG_ERROR("[server] the peer address table is full (%u addresses): ignoring new Hello packets until "
                          "an address is released (first ignored: %s)",
                          kMaxAddrs, NetAddressToString(from).c_str());
        }
        return 0;
    }
    Entry e;
    e.addr = from;
    e.key = nextKey_++;
    entries_.push_back(e);
    return e.key;
}

bool PeerTable::AddressOf(uint32_t key, NetAddress& out) const
{
    for (const Entry& e : entries_) {
        if (e.key == key) {
            out = e.addr;
            return true;
        }
    }
    return false;
}
void InterpretHostingEvents(const std::vector<HostingEvent>& events, HostingDecision& d)
{
    for (const HostingEvent& e : events) {
        if (e.kind == HostingEventKind::StartSession) {
            d.startSession = true;
            if (e.session.deadlineTicks != 0) {
                d.session.deadlineTicks = e.session.deadlineTicks;
            }
            if (e.session.rejoinTimeoutTicks != 0) {
                d.session.rejoinTimeoutTicks = e.session.rejoinTimeoutTicks;
            }
        } else if (e.kind == HostingEventKind::Terminate) {
            d.terminate = true;
        }
        // HealthCheck: ループが回っていること自体が応答。何もしない
    }
}

bool CloseSession(ReplayRecorder& recorder, const std::wstring& recordPath, IHostingProvider& hosting)
{
    bool ok = true;
    if (recorder.IsActive()) {
        const uint64_t recorded = recorder.TickCount();
        if (recorder.Finish()) {
            MYE_LOG_INFO("[server] recorded %llu tick(s) to %s", static_cast<unsigned long long>(recorded),
                         WideToUtf8(recordPath).c_str());
        } else {
            ok = false;
        }
    }
    // 記録を閉じてから外へ知らせる (知らせた直後にプロセスが片付けられても .rep は完成している)
    hosting.NotifySessionEnded();
    return ok;
}

int RunServerLoop(HeadlessSim& sim, IHostingProvider& hosting, const ServerLoopConfig& cfgIn)
{
    ServerLoopConfig cfg = cfgIn;
    if (cfg.session.playerCount < 1 || cfg.session.playerCount > kMaxPlayers
        || cfg.session.playerCount != sim.PlayerCount()) {
        MYE_LOG_ERROR("[server] session player count %u does not match the sim's lane count %u",
                      cfg.session.playerCount, sim.PlayerCount());
        return kServerExitFailed;
    }

    UdpSocket socket;
    if (!socket.Open(cfg.port)) {
        MYE_LOG_ERROR("[server] could not open UDP port %u", cfg.port);
        return kServerExitFailed;
    }
    TimerResolutionScope timerScope;
    const SteadyClock::time_point origin = SteadyClock::now();

    // ---- ホスティング: 準備完了を知らせ、セッション開始の指示を待つ ----
    // 回収してほしいログ = 記録する .rep (GameLift がプロセス終了時に集める)
    std::vector<std::wstring> logPaths;
    if (!cfg.replayRecordPath.empty()) {
        logPaths.push_back(std::filesystem::absolute(cfg.replayRecordPath).wstring());
    }
    if (!hosting.NotifyReady(socket.LocalPort(), logPaths)) {
        MYE_LOG_ERROR("[server] hosting '%s' could not be marked ready", hosting.Name());
        return kServerExitFailed;
    }
    std::vector<HostingEvent> events;
    HostingDecision decision;
    while (!decision.startSession && !decision.terminate) {
        events.clear();
        hosting.Poll(events);
        InterpretHostingEvents(events, decision);
        if (cfg.timeoutSec > 0 && NowMs(origin) > static_cast<uint64_t>(cfg.timeoutSec) * 1000) {
            MYE_LOG_ERROR("[server] timed out (%u s) before the hosting started a session", cfg.timeoutSec);
            ReplayRecorder none;
            CloseSession(none, cfg.replayRecordPath, hosting);
            return kServerExitTimeout;
        }
        if (!decision.startSession && !decision.terminate) {
            Sleep(5);
        }
    }
    if (decision.terminate && !decision.startSession) {
        MYE_LOG_INFO("[server] terminated by the hosting before a session started");
        ReplayRecorder none;
        CloseSession(none, cfg.replayRecordPath, hosting);
        return kServerExitOk;
    }
    if (decision.session.deadlineTicks != 0) {
        cfg.session.deadlineTicks = decision.session.deadlineTicks;
    }
    if (decision.session.rejoinTimeoutTicks != 0) {
        cfg.session.rejoinTimeoutTicks = decision.session.rejoinTimeoutTicks;
    }

    // ---- セッション開始: .rep の開始点 (tick 0 のスナップショット) と ServerSession ----
    sim.Activate();
    World& world = sim.Refs().scene->GetWorld();
    cfg.session.seed = world.Rng().State(); // 記録開始時点の RNG (rngState と同じ値)
    ReplayRecorder recorder;
    if (!cfg.replayRecordPath.empty()) {
        std::vector<std::byte> startBlob;
        if (!CaptureSimSnapshot(sim.Refs(), startBlob)) {
            MYE_LOG_ERROR("[server] could not capture the start snapshot for the .rep");
            return kServerExitFailed;
        }
        SimProvenance prov = sim.Provenance();
        SnapshotMeta start = {};
        start.tick = sim.TickIndex();
        start.lastEventSeq = sim.Refs().scene->Lanes().lastEventSeq;
        start.config = cfg.session;
        start.blobHash = HashBytes(startBlob.data(), startBlob.size());
        start.worldHash = sim.WorldHash();
        prov.initialSnapshotHash = start.blobHash;
        start.provenance = prov;
        // 逐次書き出し: 異常終了しても flush 済みの tick までは .rep が読める (Load がファイル長から復元する)
        recorder.Start(cfg.replayRecordPath, world.Rng().State(), world.Rng().Inc(), world.AliveCount(),
                       cfg.session.playerCount, startBlob.data(), startBlob.size(), cfg.session, prov, start,
                       kServerReplayFlushTicks);
        if (!recorder.IsActive()) {
            MYE_LOG_ERROR("[server] could not open %s for recording", WideToUtf8(cfg.replayRecordPath).c_str());
            return kServerExitFailed;
        }
    }

    PeerTable peers;
    Pcg32 lossRng;
    lossRng.Seed(static_cast<uint64_t>(socket.LocalPort()) * 0x9E3779B9ull + 1, 3);
    uint64_t packetsDropped = 0;

    ServerSessionConfig sc;
    sc.session = cfg.session;
    sc.provenance = sim.Provenance();
    sc.sessionId = MakeSessionId(socket.LocalPort());
    sc.startTick = sim.TickIndex();
    sc.startMs = NowMs(origin);
    ServerHooks hooks;
    hooks.send = [&](uint32_t peer, const void* data, size_t size) {
        NetAddress to;
        if (!peers.AddressOf(peer, to)) {
            return;
        }
        if (cfg.lossPercent > 0 && (lossRng.NextU32() % 100u) < cfg.lossPercent) {
            ++packetsDropped;
            return; // 故意の欠落 (検証用)。再送と冗長送信で埋まる
        }
        socket.Send(to, data, size);
    };
    hooks.validatePlayer = [&](const char* id) { return hosting.ValidatePlayer(id); };
    hooks.playerLeft = [&](const char* id) { hosting.PlayerLeft(id); };
    hooks.playerReleased = [&](const char* id) { hosting.PlayerReleased(id); };
    hooks.captureSnapshot = [&](std::vector<std::byte>& blob, uint64_t& worldHash) {
        sim.Activate();
        if (!CaptureSimSnapshot(sim.Refs(), blob)) {
            return false;
        }
        worldHash = sim.WorldHash();
        return true;
    };
    ServerSession session;
    if (!session.Init(sc, hooks)) {
        CloseSession(recorder, cfg.replayRecordPath, hosting);
        return kServerExitFailed;
    }
    MYE_LOG_INFO("[server] session %016llx started: %u lane(s), input delay %u, deadline %u tick(s), rejoin timeout "
                 "%u tick(s), UDP port %u, hosting '%s'%s",
                 static_cast<unsigned long long>(sc.sessionId), cfg.session.playerCount, cfg.session.inputDelay,
                 cfg.session.deadlineTicks, cfg.session.rejoinTimeoutTicks, socket.LocalPort(), hosting.Name(),
                 recorder.IsActive() ? ", recording" : "");

    // ---- 主ループ ----
    int exitCode = kServerExitOk;
    uint64_t ticks = 0;
    double tickMsSum = 0.0;
    double tickMsMax = 0.0;
    uint32_t peakLive = 0;
    uint64_t emptySinceMs = 0;
    bool emptySinceValid = false;
    uint64_t lastStatsMs = NowMs(origin);
    uint64_t statsTicks = 0;
    double statsTickMsSum = 0.0;
    std::vector<uint8_t> buf(2048);
    // 開始と同じ Poll で Terminate が来ていたら、セッションを始めてすぐ閉じる (落とさない)
    bool stop = decision.terminate;
    if (stop) {
        MYE_LOG_INFO("[server] terminate requested by the hosting");
    }
    while (!stop) {
        const uint64_t now = NowMs(origin);
        bool busy = false;

        // 1. 受信。Hello 以外の知らない宛先は無視する (表を膨らませない)
        NetAddress from;
        for (int i = 0; i < kMaxDatagramsPerIteration; ++i) {
            const int n = socket.Recv(buf.data(), buf.size(), from);
            if (n <= 0) {
                break;
            }
            busy = true;
            NetPacketHeader h;
            if (!NetParseHeader(buf.data(), static_cast<size_t>(n), h)) {
                continue;
            }
            const uint32_t key = peers.KeyOf(from, /*create=*/h.type == static_cast<uint16_t>(NetMsg::Hello));
            if (key != 0) {
                session.OnPacket(key, buf.data(), static_cast<size_t>(n), now);
            }
        }

        // 2. ホスティングの出来事
        events.clear();
        hosting.Poll(events);
        HostingDecision loopDecision;
        InterpretHostingEvents(events, loopDecision);
        if (loopDecision.terminate) {
            MYE_LOG_INFO("[server] terminate requested by the hosting");
            stop = true;
        }

        // 3. 締め切り判定と確定 → RunTick → .rep へ記録 (送信より先)
        NetConfirmedTick ct;
        for (int i = 0; i < kMaxConfirmsPerIteration && !stop && session.TryConfirm(now, ct); ++i) {
            busy = true;
            const SteadyClock::time_point t0 = SteadyClock::now();
            const uint64_t hash = sim.RunTick(ct.inputs, &ct.sys, /*resim=*/false);
            const double ms = std::chrono::duration<double, std::milli>(SteadyClock::now() - t0).count();
            session.OnTickRan(ct.tick, hash);
            if (recorder.IsActive()) {
                recorder.RecordTick(ct.inputs, cfg.session.playerCount, hash, &ct.sys);
            }
            ++ticks;
            ++statsTicks;
            tickMsSum += ms;
            statsTickMsSum += ms;
            tickMsMax = (std::max)(tickMsMax, ms);
            if (cfg.crashTestKind != 0 && cfg.crashAtTick > 0 && ticks == static_cast<uint64_t>(cfg.crashAtTick)) {
                MYE_LOG_WARN("[server] --crash-test: crashing on purpose after %llu tick(s)",
                             static_cast<unsigned long long>(ticks));
                TriggerTestCrash(static_cast<CrashTestKind>(cfg.crashTestKind));
            }
            if (cfg.tickLimit > 0 && ticks >= static_cast<uint64_t>(cfg.tickLimit)) {
                MYE_LOG_INFO("[server] --replay-ticks %lld reached", static_cast<long long>(cfg.tickLimit));
                stop = true;
            }
        }

        // 4. 送信 (Welcome / チャンク / Confirmed / keepalive) と peer のタイムアウト検出
        session.Pump(now);
        peakLive = (std::max)(peakLive, session.LivePeerCount());
        // セッションが手放した peer (Gone の片付け後、Hello を断った相手) の宛先を回収する。
        // Reject などの返信は OnPacket / TryConfirm の中で送り終えている
        peers.Sweep([&](uint32_t key) { return session.HasPeer(key); });

        // 5. 終了条件
        if (cfg.exitWhenEmpty) {
            const ServerStats& st = session.Stats();
            const bool everJoined = st.joins + st.rejoins > 0;
            if (everJoined && session.ActivePeerCount() == 0) {
                if (!emptySinceValid) {
                    emptySinceValid = true;
                    emptySinceMs = now;
                } else if (now - emptySinceMs >= cfg.emptyGraceMs) {
                    MYE_LOG_INFO("[server] every client has left - exiting");
                    stop = true;
                }
            } else {
                emptySinceValid = false;
            }
        }
        if (cfg.timeoutSec > 0 && now > static_cast<uint64_t>(cfg.timeoutSec) * 1000) {
            MYE_LOG_ERROR("[server] timed out after %u s - stopping", cfg.timeoutSec);
            exitCode = kServerExitTimeout;
            stop = true;
        }

        // 6. 統計 (sim の外。実時間なので run ごとに違って良い)
        if (cfg.statsIntervalSec > 0 && now - lastStatsMs >= static_cast<uint64_t>(cfg.statsIntervalSec) * 1000) {
            const ServerStats& st = session.Stats();
            MYE_LOG_INFO("[server] tick %llu: %u live peer(s), tick time avg %.3f ms over the last %llu tick(s), "
                         "late-subst %llu, late-drop %llu, resyncs %llu, packets in/out %llu/%llu (dropped %llu)",
                         static_cast<unsigned long long>(session.NextTick()), session.LivePeerCount(),
                         statsTicks > 0 ? statsTickMsSum / static_cast<double>(statsTicks) : 0.0,
                         static_cast<unsigned long long>(statsTicks),
                         static_cast<unsigned long long>(st.lateSubstitutions),
                         static_cast<unsigned long long>(st.lateInputsDropped),
                         static_cast<unsigned long long>(st.resyncsServed),
                         static_cast<unsigned long long>(st.packetsIn), static_cast<unsigned long long>(st.packetsOut),
                         static_cast<unsigned long long>(packetsDropped));
            lastStatsMs = now;
            statsTicks = 0;
            statsTickMsSum = 0.0;
        }

        // 7. 次の周まで眠る (仕事があった周は眠らない: 追いつき中に tick を遅らせない)
        if (!busy && !stop) {
            Sleep(1);
        }
    }

    // ---- 後始末: 記録を閉じてから外へ知らせる (CloseSession)。統計ログは .rep を閉じた後に出す ----
    const bool recordingClosed = CloseSession(recorder, cfg.replayRecordPath, hosting);
    if (!recordingClosed) {
        exitCode = kServerExitFailed;
    }
    const ServerStats& st = session.Stats();
    MYE_LOG_INFO("[server] tick time: avg %.3f ms, max %.3f ms over %llu tick(s) (peak %u live client(s), %u lane(s))",
                 ticks > 0 ? tickMsSum / static_cast<double>(ticks) : 0.0, tickMsMax,
                 static_cast<unsigned long long>(ticks), peakLive, cfg.session.playerCount);
    MYE_LOG_INFO("[server] session summary: joins %llu, rejoins %llu, leaves %llu, releases %llu, rejects %llu, "
                 "late-subst %llu, late-drop %llu, snapshots %llu, resyncs %llu (chunks %llu, re-sent %llu), "
                 "client desync reports %llu, packets in/out %llu/%llu (dropped %llu)",
                 static_cast<unsigned long long>(st.joins), static_cast<unsigned long long>(st.rejoins),
                 static_cast<unsigned long long>(st.leaves), static_cast<unsigned long long>(st.releases),
                 static_cast<unsigned long long>(st.rejects), static_cast<unsigned long long>(st.lateSubstitutions),
                 static_cast<unsigned long long>(st.lateInputsDropped), static_cast<unsigned long long>(st.snapshotsSent),
                 static_cast<unsigned long long>(st.resyncsServed), static_cast<unsigned long long>(st.chunksSent),
                 static_cast<unsigned long long>(st.chunkResends),
                 static_cast<unsigned long long>(st.clientDesyncReports),
                 static_cast<unsigned long long>(st.packetsIn), static_cast<unsigned long long>(st.packetsOut),
                 static_cast<unsigned long long>(packetsDropped));
    // レーンごとの代替入力の割合 (確定を待たれた tick のうち、締め切りまでに入力が着かなかった割合)。
    // server_verify のケース A がこの値と forced resyncs を合否に使う
    for (uint32_t lane = 0; lane < cfg.session.playerCount; ++lane) {
        const uint64_t waited = st.laneWaitedTicks[lane];
        if (waited == 0) {
            continue;
        }
        MYE_LOG_INFO("[server] lane %u: waited on %llu tick(s), late-subst %llu (%.2f%%)", lane,
                     static_cast<unsigned long long>(waited), static_cast<unsigned long long>(st.laneLateSubst[lane]),
                     100.0 * static_cast<double>(st.laneLateSubst[lane]) / static_cast<double>(waited));
    }
    MYE_LOG_INFO("[server] forced resyncs: %llu", static_cast<unsigned long long>(st.forcedResyncs));
    // 目標値 (4 クライアントでの平均 tick 時間)。超えても止めず報告だけする (測定は Release の server_verify)
    constexpr double kTickBudgetMs = 4.0;
    if (ticks > 0 && peakLive >= kMaxPlayers && tickMsSum / static_cast<double>(ticks) > kTickBudgetMs) {
        MYE_LOG_WARN("[server] tick time avg %.3f ms exceeds the %.1f ms budget with %u clients",
                     tickMsSum / static_cast<double>(ticks), kTickBudgetMs, peakLive);
    }
    return exitCode;
}

} // namespace mye
