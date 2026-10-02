//====================================================================================
//                          ServerNetSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                     10/02/2026
//                                          入力確定型サーバの 1 プロセス内検証 (偽トランスポート) の実装
//====================================================================================
#include "Engine/Engine/Net/ServerNetSelfTest.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/ComponentRegistry.h"
#include "Engine/Core/Util/Hash.h"
#include "Engine/Core/Util/Random.h"
#include "Engine/Engine/Demo/ShowcaseScenes.h"
#include "Engine/Engine/Loop/GameFlow.h"
#include "Engine/Engine/Loop/HeadlessSim.h"
#include "Engine/Engine/Loop/TickRunner.h"
#include "Engine/Engine/Net/ClientSession.h"
#include "Engine/Engine/Net/ClientSimRunner.h"
#include "Engine/Engine/Net/NetProtocol.h"
#include "Engine/Engine/Net/NetRollback.h"
#include "Engine/Engine/Net/NetRuntime.h"
#include "Engine/Engine/Net/NetSession.h"
#include "Engine/Engine/Net/ServerSession.h"
#include "Engine/Engine/Replay/CrashRing.h"
#include "Engine/Engine/Replay/Replay.h"
#include "Engine/Engine/Replay/SimSnapshot.h"
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/Scene/SaveGame.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Engine/Script/EngineApiTable.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {
namespace {

int g_fail = 0;

// 各試験の所要時間 (ログ用。試験の判定には使わない)
class StepTimer {
public:
    explicit StepTimer(const char* name) : name_(name), t0_(std::chrono::steady_clock::now()) {}
    ~StepTimer()
    {
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0_).count();
        MYE_LOG_INFO("[server-net selftest] %s: %.0f ms", name_, ms);
    }

private:
    const char* name_;
    std::chrono::steady_clock::time_point t0_;
};

void Check(bool ok, const char* fmt, ...)
{
    if (ok) {
        return;
    }
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    MYE_LOG_ERROR("[server-net selftest] FAIL: %s", buf);
    ++g_fail;
}

// ---------------------------------------------------------------------------------------------
// 偽トランスポート: seed 付き Pcg32 で遅延 (固定 + ジッタ)・ロス・並べ替え・重複を作る。
// 乱数の引き方は Send の呼び出し順だけで決まるので、同じシナリオは毎回同じ配達順になる
// ---------------------------------------------------------------------------------------------

struct LinkSpec {
    uint32_t baseMs = 0;
    uint32_t jitterMs = 0;
    uint32_t lossPct = 0;
    uint32_t reorderPct = 0; // この確率で 20〜60ms 余計に遅らせる (後から送ったものに追い越される)
    uint32_t dupPct = 0;
    bool down = false;       // 切断中 (全部捨てる)
};

class FakeNet {
public:
    explicit FakeNet(uint64_t seed) { rng_.Seed(seed); }

    // from / to は 0 = サーバ、1.. = クライアント。クライアント k のリンクは両方向で link(k)
    LinkSpec& Link(int client) { return links_[client]; }

    void Send(int from, int to, const void* data, size_t size, uint64_t nowMs)
    {
        const LinkSpec& spec = links_[from == 0 ? to : from];
        if (spec.down) {
            ++dropped_;
            return;
        }
        if (rng_.RangeU32(100) < spec.lossPct) {
            ++dropped_;
            return;
        }
        uint32_t delay = spec.baseMs + (spec.jitterMs != 0 ? rng_.RangeU32(spec.jitterMs + 1) : 0);
        if (spec.reorderPct != 0 && rng_.RangeU32(100) < spec.reorderPct) {
            delay += 20 + rng_.RangeU32(41);
        }
        Push(from, to, data, size, nowMs + delay);
        if (spec.dupPct != 0 && rng_.RangeU32(100) < spec.dupPct) {
            Push(from, to, data, size, nowMs + delay + rng_.RangeU32(30));
        }
    }

    template <class F>
    void Deliver(uint64_t nowMs, F&& f)
    {
        std::vector<Packet> ready;
        for (size_t i = 0; i < inflight_.size();) {
            if (inflight_[i].at <= nowMs) {
                ready.push_back(std::move(inflight_[i]));
                inflight_.erase(inflight_.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
        std::sort(ready.begin(), ready.end(), [](const Packet& a, const Packet& b) {
            return a.at != b.at ? a.at < b.at : a.seq < b.seq;
        });
        for (const Packet& p : ready) {
            f(p.from, p.to, p.data.data(), p.data.size());
        }
    }

    uint64_t Dropped() const { return dropped_; }

private:
    struct Packet {
        uint64_t at = 0;
        uint64_t seq = 0;
        int from = 0;
        int to = 0;
        std::vector<uint8_t> data;
    };
    void Push(int from, int to, const void* data, size_t size, uint64_t at)
    {
        Packet p;
        p.at = at;
        p.seq = seq_++;
        p.from = from;
        p.to = to;
        p.data.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
        inflight_.push_back(std::move(p));
    }

    Pcg32 rng_;
    LinkSpec links_[kMaxPlayers + 1];
    std::vector<Packet> inflight_;
    uint64_t seq_ = 0;
    uint64_t dropped_ = 0;
};

// ---------------------------------------------------------------------------------------------
// sim 無しのプロトコル単体検証 (ServerSession / ClientSession を偽のスナップショットで回す)
// ---------------------------------------------------------------------------------------------

SimProvenance FakeProvenance()
{
    SimProvenance p = {};
    p.engineVersion = 0xE1E1E1E1ull;
    p.protocolVersion = kNetProtoVersion;
    p.apiVersion = 23;
    p.schemaVersion = 7;
    p.replayVersion = 9;
    p.gameVersion = 0x6A3E6A3Eull;
    p.contentHash = 0xC0117E27ull;
    return p;
}

SessionConfig FakeSessionConfig(uint32_t playerCount)
{
    SessionConfig c = {};
    c.role = static_cast<uint32_t>(SessionRole::Server);
    c.playerCount = playerCount;
    c.tickRate = 60;
    c.inputDelay = 3;
    c.deadlineTicks = 2;
    c.rejoinTimeoutTicks = 60;
    return c;
}

// サーバ 1 + クライアント数本を 1 つの偽ネットでつなぐ最小の駆動役。sim は持たず、
// スナップショットは疑似乱数で埋めた blob、tick 末ハッシュは tick 番号から作る
struct MiniWorld {
    FakeNet net;
    ServerSession srv;
    std::vector<std::unique_ptr<ClientSession>> cl;
    std::vector<std::byte> blob;
    uint64_t now = 0;
    bool autoApply = true;
    uint64_t maxChunksInOnePump = 0;
    uint32_t lastTickRan = 0;
    std::vector<SystemEvent> events; // 確定した tick に載ったシステムイベント (確定順)

    explicit MiniWorld(uint64_t seed) : net(seed) {}

    bool Start(const ServerSessionConfig& cfg, size_t blobBytes, const std::function<bool(const char*)>& validate = {})
    {
        Pcg32 fill;
        fill.Seed(7);
        blob.resize(blobBytes);
        for (std::byte& b : blob) {
            b = static_cast<std::byte>(fill.NextU32() & 0xFFu);
        }
        ServerHooks hooks;
        hooks.send = [this](uint32_t peer, const void* d, size_t n) { net.Send(0, static_cast<int>(peer), d, n, now); };
        hooks.validatePlayer = validate;
        hooks.captureSnapshot = [this](std::vector<std::byte>& out, uint64_t& worldHash) {
            out = blob;
            worldHash = 0xFEEDFACEull;
            return true;
        };
        return srv.Init(cfg, hooks);
    }

    ClientSession& AddClient(const ClientSessionConfig& cfg)
    {
        const int idx = static_cast<int>(cl.size()) + 1;
        cl.push_back(std::make_unique<ClientSession>());
        cl.back()->Start(cfg, [this, idx](const void* d, size_t n) { net.Send(idx, 0, d, n, now); }, now);
        return *cl.back();
    }

    void Step()
    {
        net.Deliver(now, [this](int from, int to, const void* d, size_t n) {
            if (to == 0) {
                srv.OnPacket(static_cast<uint32_t>(from), d, n, now);
            } else if (static_cast<size_t>(to) <= cl.size()) {
                cl[static_cast<size_t>(to) - 1]->OnPacket(d, n, now);
            }
        });
        NetConfirmedTick ct;
        for (int i = 0; i < 8 && srv.TryConfirm(now, ct); ++i) {
            srv.OnTickRan(ct.tick, 0x1000ull + ct.tick);
            for (uint32_t e = 0; e < ct.sys.eventCount; ++e) {
                events.push_back(ct.sys.events[e]);
            }
        }
        const uint64_t before = srv.Stats().chunksSent;
        srv.Pump(now);
        maxChunksInOnePump = (std::max)(maxChunksInOnePump, srv.Stats().chunksSent - before);
        for (auto& c : cl) {
            c->Poll(now);
            if (autoApply && c->State() == ClientState::SnapshotReady) {
                c->OnSnapshotApplied(true, now);
            }
        }
        ++now;
    }

    void Run(uint64_t untilMs)
    {
        while (now < untilMs) {
            Step();
        }
    }
};

ClientSessionConfig FakeClientConfig(const char* sid)
{
    ClientSessionConfig c;
    c.provenance = FakeProvenance();
    c.playerSessionId = sid;
    return c;
}

ServerSessionConfig FakeServerConfig(uint32_t playerCount)
{
    ServerSessionConfig c;
    c.session = FakeSessionConfig(playerCount);
    c.provenance = FakeProvenance();
    c.sessionId = 77;
    return c;
}

void TestTickRecordRoundTrip()
{
    Pcg32 rng;
    rng.Seed(11);
    for (int round = 0; round < 50; ++round) {
        NetConfirmedTick t;
        t.tick = 1000 + round;
        for (uint32_t p = 0; p < kMaxPlayers; ++p) {
            if (rng.RangeU32(3) != 0) {
                InputSnapshot in = {};
                in.keys[rng.RangeU32(32)] = static_cast<uint8_t>(1 + rng.RangeU32(255));
                in.mouseDeltaX = static_cast<int32_t>(rng.RangeU32(100)) - 50;
                in.padLX = static_cast<int16_t>(rng.RangeU32(60000)) - 30000;
                t.inputs[p] = in;
            }
        }
        t.sys.eventCount = rng.RangeU32(kMaxSystemEventsPerTick + 1);
        for (uint32_t i = 0; i < t.sys.eventCount; ++i) {
            t.sys.events[i].eventSeq = 100 + i;
            t.sys.events[i].playerId = 5 + rng.RangeU32(4);
            t.sys.events[i].kind = static_cast<uint8_t>(1 + rng.RangeU32(4));
            t.sys.events[i].lane = static_cast<uint8_t>(rng.RangeU32(4));
        }
        std::vector<uint8_t> bytes;
        NetWriteTickRecord(t, bytes);
        Check(bytes.size() == NetTickRecordBytes(t), "tick record size matches NetTickRecordBytes (round %d)", round);
        NetConfirmedTick back;
        const size_t used = NetReadTickRecord(bytes.data(), bytes.size(), t.tick, back);
        Check(used == bytes.size(), "tick record reads back all its bytes (round %d)", round);
        Check(NetConfirmedTickEqual(t, back, kMaxPlayers), "tick record round trip is byte-identical (round %d)", round);
        // 切り詰めた入力は壊れたものとして拒否される
        Check(NetReadTickRecord(bytes.data(), bytes.size() - 1, t.tick, back) == 0
                  || bytes.size() == sizeof(NetTickRecordHeader),
              "a truncated tick record is rejected (round %d)", round);
    }
}

void TestRejectPaths()
{
    // ---- 出自 (gameVersion) の食い違い: 拒否 / --allow-game-mismatch で通る ----
    {
        MiniWorld w(1);
        w.Start(FakeServerConfig(4), 3000);
        ClientSessionConfig cc = FakeClientConfig("p-a");
        cc.provenance.gameVersion ^= 1;
        ClientSession& c = w.AddClient(cc);
        w.Run(200);
        Check(c.State() == ClientState::Failed && c.RejectReason() == NetReject::GameVersion,
              "a different GameLogic.dll is rejected with GameVersion (state %d, reason %d)",
              static_cast<int>(c.State()), static_cast<int>(c.RejectReason()));
        Check(w.srv.Stats().rejects >= 1, "the server counted the reject");
    }
    {
        MiniWorld w(2);
        ServerSessionConfig sc = FakeServerConfig(4);
        sc.session.configBits = kCfgAllowGameMismatch;
        w.Start(sc, 3000);
        ClientSessionConfig cc = FakeClientConfig("p-a");
        cc.provenance.gameVersion ^= 1;
        cc.allowGameMismatch = true;
        cc.configBits = kCfgAllowGameMismatch;
        ClientSession& c = w.AddClient(cc);
        w.Run(400);
        Check(c.Running(), "--allow-game-mismatch lets a different GameLogic.dll in (state %d)",
              static_cast<int>(c.State()));
    }
    // ---- そのほかの項目: 起動オプション / UI 基準解像度 / フォント表 ----
    {
        MiniWorld w(3);
        w.Start(FakeServerConfig(4), 3000);
        ClientSessionConfig a = FakeClientConfig("p-a");
        a.configBits = kCfgSynthInput;
        ClientSessionConfig b = FakeClientConfig("p-b");
        b.referenceW = 1280;
        b.referenceH = 720;
        ClientSessionConfig c = FakeClientConfig("p-c");
        c.fontMetricsHash = 5;
        ClientSession& ca = w.AddClient(a);
        ClientSession& cb = w.AddClient(b);
        ClientSession& cc = w.AddClient(c);
        w.Run(300);
        Check(ca.State() == ClientState::Failed && ca.RejectReason() == NetReject::ConfigBits,
              "different launch options are rejected (ConfigBits)");
        Check(cb.State() == ClientState::Failed && cb.RejectReason() == NetReject::ReferenceSize,
              "a different UI reference size is rejected (ReferenceSize)");
        Check(cc.State() == ClientState::Failed && cc.RejectReason() == NetReject::FontMetrics,
              "a different font metrics table is rejected (FontMetrics)");
    }
    // ---- 満員 / ホスティングの検証 / 再接続の主張 ----
    {
        MiniWorld w(4);
        w.Start(FakeServerConfig(1), 3000, [](const char* sid) { return std::strcmp(sid, "banned") != 0; });
        ClientSession& a = w.AddClient(FakeClientConfig("p-a"));
        ClientSession& banned = w.AddClient(FakeClientConfig("banned"));
        w.Run(300);
        Check(banned.State() == ClientState::Failed && banned.RejectReason() == NetReject::PlayerRejected,
              "a player session the hosting refuses is rejected (PlayerRejected)");
        ClientSession& b = w.AddClient(FakeClientConfig("p-b"));
        w.Run(600);
        Check(a.Running(), "the first player got the only lane");
        Check(b.State() == ClientState::Failed && b.RejectReason() == NetReject::ServerFull,
              "a full server rejects the next player (ServerFull)");
        ClientSessionConfig bad = FakeClientConfig("not-the-owner");
        bad.playerId = a.PlayerId();
        ClientSession& thief = w.AddClient(bad);
        w.Run(900);
        Check(thief.State() == ClientState::Failed && thief.RejectReason() == NetReject::UnknownPlayer,
              "a rejoin with the wrong player session ID is rejected (UnknownPlayer)");
        Check(a.Running(), "the owner is unaffected by the rejected thief");
    }
}

// スナップショットを 400KB のロス・並べ替え・重複のある線で送る。欠落分だけ再送して完成し、
// 1 回の Pump が送るチャンクは上限を超えない
// 正常終了 (Bye) -> Reserved -> 予約期間を過ぎて Release
void TestGracefulLeave()
{
    MiniWorld w(8);
    ServerSessionConfig sc = FakeServerConfig(4);
    sc.session.rejoinTimeoutTicks = 60;
    w.Start(sc, 2000);
    ClientSession& a = w.AddClient(FakeClientConfig("p-a"));
    w.Run(300);
    Check(a.Running(), "graceful leave: the client is running");
    const uint32_t lane = a.Lane();
    Check(w.srv.Lanes().lanes[lane].state == static_cast<uint32_t>(LaneState::Connected),
          "graceful leave: the lane is connected");
    a.Close(w.now);
    w.Run(400);
    Check(w.srv.Lanes().lanes[lane].state == static_cast<uint32_t>(LaneState::Reserved)
              && w.srv.Stats().leaves == 1,
          "graceful leave: Bye makes the lane Reserved (state %u, leaves %llu)", w.srv.Lanes().lanes[lane].state,
          static_cast<unsigned long long>(w.srv.Stats().leaves));
    w.Run(2000);
    Check(w.srv.Lanes().lanes[lane].state == static_cast<uint32_t>(LaneState::Empty)
              && w.srv.Lanes().lanes[lane].playerId == 0 && w.srv.Stats().releases == 1,
          "graceful leave: the reservation expires into Empty (releases %llu)",
          static_cast<unsigned long long>(w.srv.Stats().releases));
    // 空いたレーンは次の Join が最小の空きレーンとして取る
    ClientSession& b = w.AddClient(FakeClientConfig("p-b"));
    w.Run(w.now + 400);
    Check(b.Running() && b.Lane() == lane, "graceful leave: the next player takes the freed lane (%u vs %u)",
          b.Lane(), lane);
}

// ログリングの cursor 以降に出た ERROR の数 (cursor は logging::TotalWritten() で取る)
size_t CountErrorLogsSince(uint64_t cursor)
{
    LogEntry buf[64];
    size_t errors = 0;
    for (size_t n = logging::ReadSince(cursor, buf, 64); n != 0; n = logging::ReadSince(cursor, buf, 64)) {
        for (size_t i = 0; i < n; ++i) {
            errors += buf[i].level == LogLevel::Error ? 1u : 0u;
        }
    }
    return errors;
}

// V11: 旧 peer が Gone (切断を知った) になった同じ受信周に、同じ player の再接続 Hello が届く。
// Leave は旧 peer の脱落で 1 本だけ積まれ、Hello が 2 本目を積まない (2 本目は適用時に「not connected」の
// ERROR になり、無効なイベントが確定入力 = .rep に残る)
void TestReconnectLeavesOnce()
{
    MiniWorld w(9);
    ServerSessionConfig sc = FakeServerConfig(4);
    w.Start(sc, 2000);
    ClientSession& a = w.AddClient(FakeClientConfig("p-a"));
    w.Run(300);
    Check(a.Running(), "reconnect: the first client is running");
    const uint64_t playerId = a.PlayerId();
    const uint32_t lane = a.Lane();
    w.events.clear();
    const uint64_t errorCursor = logging::TotalWritten();
    w.srv.DropPeer(1); // トランスポートが切断を知った (Leave が積まれる)
    ClientSessionConfig again = FakeClientConfig("p-a");
    again.playerId = playerId;
    ClientSession& b = w.AddClient(again); // Hello は次の Step の受信で、TryConfirm の前に届く
    w.Run(w.now + 600);
    uint32_t leaves = 0;
    uint32_t rejoins = 0;
    bool leaveFirst = false;
    for (const SystemEvent& ev : w.events) {
        if (ev.playerId != playerId) {
            continue;
        }
        if (ev.kind == static_cast<uint8_t>(SystemEventKind::Leave)) {
            ++leaves;
            leaveFirst = rejoins == 0;
        } else if (ev.kind == static_cast<uint8_t>(SystemEventKind::Rejoin)) {
            ++rejoins;
        }
    }
    Check(b.Running() && b.Lane() == lane && b.PlayerId() == playerId,
          "reconnect: the same player gets the same lane back (running %d, lane %u vs %u)", b.Running() ? 1 : 0, b.Lane(),
          lane);
    Check(leaves == 1 && rejoins == 1 && leaveFirst,
          "reconnect: exactly one Leave then one Rejoin were confirmed (leaves %u, rejoins %u)", leaves, rejoins);
    Check(w.srv.Stats().leaves == 1, "reconnect: the server counted one leave (%llu)",
          static_cast<unsigned long long>(w.srv.Stats().leaves));
    Check(!w.srv.HasPeer(1) && w.srv.HasPeer(2), "reconnect: the session keeps only the new peer (HasPeer old %d, new %d)",
          w.srv.HasPeer(1) ? 1 : 0, w.srv.HasPeer(2) ? 1 : 0);
    const size_t errors = CountErrorLogsSince(errorCursor);
    Check(errors == 0, "reconnect: no ERROR was logged while the events were applied (%zu)", errors);
}
void TestSnapshotTransfer()
{
    MiniWorld w(5);
    ServerSessionConfig sc = FakeServerConfig(4);
    sc.maxChunksPerPump = 16;
    w.Start(sc, 400 * 1024); // 400 チャンク: ack のウィンドウ (256) が滑らないと完成しない
    LinkSpec& l = w.net.Link(1);
    l.baseMs = 30;
    l.jitterMs = 10;
    l.lossPct = 30;
    l.reorderPct = 15;
    l.dupPct = 10;
    w.autoApply = false;
    ClientSession& c = w.AddClient(FakeClientConfig("p-a"));
    uint64_t readyAt = 0;
    while (w.now < 20000 && c.State() != ClientState::SnapshotReady && c.State() != ClientState::Failed) {
        w.Step();
    }
    readyAt = w.now;
    Check(c.State() == ClientState::SnapshotReady, "the snapshot completes over a 30%% loss link (state %d at %llu ms)",
          static_cast<int>(c.State()), static_cast<unsigned long long>(readyAt));
    Check(c.SnapshotBlob() == w.blob, "the received snapshot is byte-identical");
    Check(w.srv.Stats().chunkResends > 0, "lost chunks were re-sent (resends %llu)",
          static_cast<unsigned long long>(w.srv.Stats().chunkResends));
    Check(w.maxChunksInOnePump <= 16, "one Pump never sends more than the chunk cap (max %llu)",
          static_cast<unsigned long long>(w.maxChunksInOnePump));
    c.OnSnapshotApplied(true, w.now);
    Check(c.Running(), "applying the snapshot starts the client");
    // 復元に失敗したら再要求し、3 回続けて失敗したら諦める
    MiniWorld w2(6);
    w2.Start(FakeServerConfig(4), 5000);
    w2.autoApply = false;
    ClientSession& d = w2.AddClient(FakeClientConfig("p-b"));
    int attempts = 0;
    while (w2.now < 20000 && d.State() != ClientState::Failed) {
        w2.Step();
        if (d.State() == ClientState::SnapshotReady) {
            d.OnSnapshotApplied(false, w2.now);
            ++attempts;
        }
    }
    Check(d.State() == ClientState::Failed && attempts == 3,
          "a snapshot that never restores to the meta hash fails after 3 attempts (attempts %d)", attempts);
}

// ---------------------------------------------------------------------------------------------
// sim ありの検証
// ---------------------------------------------------------------------------------------------

constexpr int kClients = 3;

struct Sims {
    HeadlessSim server;
    HeadlessSim client[kClients];
    std::vector<std::byte> initialBlob;
};

bool InitSim(HeadlessSim& sim)
{
    HeadlessSimSetup s;
    s.config.title = L"MyEngine ServerNetSelfTest";
    s.config.localPlayers = static_cast<int>(kMaxPlayers);
    s.scene.showcase = FindShowcase(L"--local-demo", /*editor=*/true);
    s.systemInput = true;
    if (s.scene.showcase == nullptr || !sim.Init(s)) {
        return false;
    }
    // ABI v23 の probe (GameLogic.dll の NetEventProbe)。DLL が無い環境では付けず、probe の照合を飛ばす。
    // 全 sim に同じ順で付けるので、初期スナップショットとクライアントへの転送に乗る
    const ComponentTypeId probe = ComponentRegistry::Get().FindByName("NetEventProbe");
    if (probe != kInvalidComponentType) {
        Scene* scene = sim.Refs().scene;
        scene->GetWorld().AddComponentRaw(scene->CreateGameObject("NetEventProbeHost").Id(), probe);
    }
    // LoadGame / LoadPersist の境界を見る probe (GameLogic.dll の SaveLoadProbe)。同じく全 sim に同じ順で付ける
    const ComponentTypeId saveProbe = ComponentRegistry::Get().FindByName("SaveLoadProbe");
    if (saveProbe != kInvalidComponentType) {
        Scene* scene = sim.Refs().scene;
        scene->GetWorld().AddComponentRaw(scene->CreateGameObject("SaveLoadProbeHost").Id(), saveProbe);
    }
    // v13 の Net* を毎 tick sim へ書く probe (GameLogic.dll の NetInfoProbe)。ライブとオフライン再生で同じ値を読むことの検証用
    const ComponentTypeId infoProbe = ComponentRegistry::Get().FindByName("NetInfoProbe");
    if (infoProbe != kInvalidComponentType) {
        Scene* scene = sim.Refs().scene;
        scene->GetWorld().AddComponentRaw(scene->CreateGameObject("NetInfoProbeHost").Id(), infoProbe);
    }
    return true;
}

bool HasEventProbe()
{
    return ComponentRegistry::Get().FindByName("NetEventProbe") != kInvalidComponentType;
}

bool HasSaveLoadProbe()
{
    return ComponentRegistry::Get().FindByName("SaveLoadProbe") != kInvalidComponentType;
}

// SaveLoadProbe のフィールドを ABI 経由で読み書きする。4 バイトのフィールドは下位に入る (リトルエンディアン)
bool ReadSaveProbe(HeadlessSim& sim, const char* field, uint64_t& out)
{
    Scene* scene = sim.Refs().scene;
    const GameObject host = scene->Find("SaveLoadProbeHost");
    if (!host) {
        return false;
    }
    ScriptApiContext apiCtx;
    apiCtx.scene = scene;
    MyeEngineApi api = {};
    BuildEngineApi(api, &apiCtx);
    uint64_t buf = 0;
    const MyeEntityId id = { host.Id().index, host.Id().generation };
    if (api.GetComponentField(&apiCtx, id, HashStr("SaveLoadProbe"), HashStr(field), &buf, sizeof(buf), nullptr) <= 0) {
        return false;
    }
    out = buf;
    return true;
}

bool WriteSaveProbe(HeadlessSim& sim, const char* field, const void* value, int32_t size)
{
    Scene* scene = sim.Refs().scene;
    const GameObject host = scene->Find("SaveLoadProbeHost");
    if (!host) {
        return false;
    }
    ScriptApiContext apiCtx;
    apiCtx.scene = scene;
    MyeEngineApi api = {};
    BuildEngineApi(api, &apiCtx);
    const MyeEntityId id = { host.Id().index, host.Id().generation };
    return api.SetComponentField(&apiCtx, id, HashStr("SaveLoadProbe"), HashStr(field), value, size) == 1;
}

// NetInfoProbe のフィールドを ABI 経由で読む (無ければ false)
bool ReadNetInfoProbe(HeadlessSim& sim, const char* field, uint64_t& out)
{
    Scene* scene = sim.Refs().scene;
    const GameObject host = scene->Find("NetInfoProbeHost");
    if (!host) {
        return false;
    }
    ScriptApiContext apiCtx;
    apiCtx.scene = scene;
    MyeEngineApi api = {};
    BuildEngineApi(api, &apiCtx);
    uint64_t buf = 0;
    const MyeEntityId id = { host.Id().index, host.Id().generation };
    if (api.GetComponentField(&apiCtx, id, HashStr("NetInfoProbe"), HashStr(field), &buf, sizeof(buf), nullptr) <= 0) {
        return false;
    }
    out = buf;
    return true;
}

// probe のフィールドを ABI (GetComponentField) 経由で読む。無ければ false
bool ReadProbe(HeadlessSim& sim, const char* field, uint64_t& out)
{
    Scene* scene = sim.Refs().scene;
    const GameObject host = scene->Find("NetEventProbeHost");
    if (!host) {
        return false;
    }
    ScriptApiContext apiCtx;
    apiCtx.scene = scene;
    MyeEngineApi api = {};
    BuildEngineApi(api, &apiCtx);
    uint64_t buf = 0; // 4 バイトのフィールドは下位に入る (リトルエンディアン)
    const MyeEntityId id = { host.Id().index, host.Id().generation };
    if (api.GetComponentField(&apiCtx, id, HashStr("NetEventProbe"), HashStr(field), &buf, sizeof(buf), nullptr) <= 0) {
        return false;
    }
    out = buf;
    return true;
}

SessionConfig SimSessionConfig(uint32_t deadlineTicks, uint32_t rejoinTimeoutTicks)
{
    SessionConfig c = DefaultServerSessionConfig(kMaxPlayers, 3);
    c.deadlineTicks = deadlineTicks;
    c.rejoinTimeoutTicks = rejoinTimeoutTicks;
    return c;
}

struct Action {
    enum Kind { Start, Kill, Restart, PokeSim, SkipSeq, SetLink } kind;
    uint64_t atMs;
    int client; // 1..kClients
    uint32_t arg;
};

struct ScenarioDef {
    const char* name = "";
    uint64_t seed = 1;
    uint64_t endMs = 10000;
    uint64_t serverStartMs = 0;
    uint32_t deadlineTicks = 2;
    uint32_t rejoinTimeoutTicks = 150;
    uint32_t peerTimeoutMs = 1000;
    uint32_t quietTicks = 0;      // この tick 未満の自レーン入力はゼロ
    uint32_t maxSpeculation = kNetMaxSpeculationClient;
    // SaveLoadProbe: この tick に LoadPersist / LoadGame を積ませる (0 = 積まない)。slot のセーブは試験が置く
    uint64_t persistLoadTick = 0;
    uint64_t gameLoadTick = 0;
    int32_t saveSlot = 0;
    bool traceMargin = false;
    LinkSpec link[kClients + 1];
    std::vector<Action> actions;
};

struct ClientOutcome {
    std::map<uint64_t, uint64_t> committed; // tick -> 確定ハッシュ
    uint64_t resyncs = 0, eventGaps = 0, snapshots = 0, desyncs = 0, stalls = 0;
    uint64_t rollbacks = 0, maxDepth = 0, resimTicks = 0, predictedTicks = 0, catchUpTicks = 0;
    double rttMs = 0.0, marginMs = 0.0;
    std::vector<std::pair<uint32_t, uint64_t>> lanes; // セッションごとの (レーン, playerId)
    std::wstring bundleDir;
    uint64_t lastSnapshotTick = 0;
    ClientState finalState = ClientState::Idle;
    uint64_t finalTick = 0;
    bool running = false;            // 終了時にセッションが稼働中 (EngineLoop が netInfo.connected へ写す値)
    uint32_t sessionPlayerCount = 0; // セッションの人数 (同 netInfo.playerCount)
    // (now ms, 到着余裕 ms) を 50ms ごと。traceMargin のシナリオで、標本が有効なときだけ
    std::vector<std::pair<uint64_t, double>> marginTrace;
};

struct ScenarioResult {
    // サーバが最初の tick を回す前の sim 状態。probe の設定を書いたシナリオだけ入る (空 = 共通の初期スナップショット)
    std::vector<std::byte> startBlob;
    std::vector<uint64_t> serverHash;
    std::vector<NetConfirmedTick> log;
    ServerStats stats;
    SessionLanes mirrorLanes = {};
    SessionLanes simLanes = {};
    ClientOutcome client[kClients + 1];
    uint64_t packetsDropped = 0;
};

struct ClientNode {
    std::unique_ptr<ClientSession> session;
    std::unique_ptr<ClientSimRunner> runner;
    std::unique_ptr<CrashRing> crash;
    bool alive = false;
};

void Harvest(ClientNode& n, ClientOutcome& out)
{
    if (!n.session || !n.runner) {
        return;
    }
    const ClientSimRunnerStats& rs = n.runner->Stats();
    out.resyncs += n.session->Stats().resyncs;
    out.eventGaps += n.session->Stats().eventGaps;
    out.snapshots += rs.snapshotsApplied;
    out.desyncs += rs.desyncs;
    out.stalls += rs.stalls;
    out.rollbacks += n.runner->Rollback().RollbackCount();
    out.maxDepth = (std::max)(out.maxDepth, n.runner->Rollback().MaxRollbackDepth());
    out.resimTicks += rs.ticksResimulated;
    out.predictedTicks += n.runner->Rollback().PredictedTicks();
    out.catchUpTicks += rs.catchUpTicks;
    out.rttMs = n.session->RttMs();
    out.marginMs = n.session->MarginMs();
    if (n.session->PlayerId() != 0) {
        out.lanes.emplace_back(n.session->Lane(), n.session->PlayerId());
        out.lastSnapshotTick = n.session->SnapshotMetaOf().tick;
    }
    if (!n.runner->LastBundleDir().empty()) {
        out.bundleDir = n.runner->LastBundleDir();
    }
    out.finalState = n.session->State();
    out.finalTick = n.runner->TickIndex();
    out.running = n.session->Running();
    out.sessionPlayerCount = n.session->PlayerCount();
}

std::wstring CrashRoot()
{
    return GetExecutableDir() + L"\\cache\\server_net_selftest";
}

ScenarioResult RunScenario(Sims& sims, const ScenarioDef& def)
{
    ScenarioResult res;
    HeadlessSim& serverSim = sims.server;
    serverSim.Activate();
    if (!RestoreSimSnapshot(serverSim.Refs(), sims.initialBlob.data(), sims.initialBlob.size())) {
        Check(false, "[%s] could not reset the server sim to its initial snapshot", def.name);
        return res;
    }
    for (HeadlessSim& c : sims.client) {
        c.SetPokeTick(-1);
    }
    if (def.persistLoadTick != 0 || def.gameLoadTick != 0) {
        // probe の要求 tick を最初のスナップショットへ含める (クライアントは参加スナップショットで受け取る)
        const bool wrote = WriteSaveProbe(serverSim, "loadPersistTick", &def.persistLoadTick, sizeof(def.persistLoadTick))
            && WriteSaveProbe(serverSim, "loadGameTick", &def.gameLoadTick, sizeof(def.gameLoadTick))
            && WriteSaveProbe(serverSim, "slot", &def.saveSlot, sizeof(def.saveSlot));
        if (!wrote || !CaptureSimSnapshot(serverSim.Refs(), res.startBlob)) {
            Check(false, "[%s] could not set up the SaveLoadProbe", def.name);
            return res;
        }
    }

    FakeNet net(def.seed);
    for (int k = 1; k <= kClients; ++k) {
        net.Link(k) = def.link[k];
    }
    uint64_t now = 0;

    ServerSessionConfig sc;
    sc.session = SimSessionConfig(def.deadlineTicks, def.rejoinTimeoutTicks);
    sc.provenance = serverSim.Provenance();
    sc.sessionId = 0x5E55100ull + def.seed;
    sc.startMs = def.serverStartMs;
    sc.peerTimeoutMs = def.peerTimeoutMs;
    ServerHooks hooks;
    hooks.send = [&](uint32_t peer, const void* d, size_t n) { net.Send(0, static_cast<int>(peer), d, n, now); };
    hooks.captureSnapshot = [&](std::vector<std::byte>& blob, uint64_t& worldHash) {
        serverSim.Activate();
        if (!CaptureSimSnapshot(serverSim.Refs(), blob)) {
            return false;
        }
        worldHash = serverSim.WorldHash();
        return true;
    };
    ServerSession srv;
    if (!srv.Init(sc, hooks)) {
        Check(false, "[%s] ServerSession::Init failed", def.name);
        return res;
    }

    ClientNode nodes[kClients + 1];
    const auto startClient = [&](int k, uint64_t playerId) {
        ClientNode& n = nodes[k];
        Harvest(n, res.client[k]);
        n.session = std::make_unique<ClientSession>();
        n.runner = std::make_unique<ClientSimRunner>();
        n.crash = std::make_unique<CrashRing>();
        SessionConfig crashSession = sc.session;
        crashSession.role = static_cast<uint32_t>(SessionRole::Client);
        CrashRingConfig cc;
        cc.playerCount = kMaxPlayers;
        cc.session = crashSession;
        n.crash->Configure(cc);
        n.crash->SetEnabled(true);

        ClientSessionConfig cfg;
        cfg.provenance = serverSim.Provenance();
        cfg.playerSessionId = "selftest-player-" + std::to_string(k);
        cfg.playerId = playerId;
        n.session->Start(cfg, [&net, &now, k](const void* d, size_t sz) { net.Send(k, 0, d, sz, now); }, now);

        HeadlessSim& sim = sims.client[k - 1];
        ClientSession* session = n.session.get();
        ClientSimHooks ch;
        ch.runTick = [&sim](const InputSnapshot* lanes, uint32_t, const SystemInputTick& sys, bool resim) {
            return sim.RunTick(lanes, &sys, resim);
        };
        ch.worldHash = [&sim]() { return sim.WorldHash(); };
        const uint32_t quiet = def.quietTicks;
        ch.liveInput = [session, quiet](uint64_t tick) {
            return tick < quiet ? InputSnapshot{} : SynthLaneInput(tick, session->Lane());
        };
        ClientOutcome* out = &res.client[k];
        ch.onCommitted = [out](uint64_t tick, const NetConfirmedTick&, uint64_t hash) { out->committed[tick] = hash; };
        ClientSimRunnerConfig rc;
        rc.maxSpeculation = def.maxSpeculation;
        rc.crashRing = n.crash.get();
        rc.crashRoot = CrashRoot();
        n.runner->Attach(session, sim.Refs(), ch, rc);
        n.alive = true;
    };

    size_t nextAction = 0;
    std::vector<Action> actions = def.actions;
    std::stable_sort(actions.begin(), actions.end(), [](const Action& a, const Action& b) { return a.atMs < b.atMs; });

    for (now = 0; now < def.endMs; ++now) {
        // 1. 受信 (到着順 = 配達時刻順)
        net.Deliver(now, [&](int from, int to, const void* d, size_t n) {
            if (to == 0) {
                srv.OnPacket(static_cast<uint32_t>(from), d, n, now);
            } else if (nodes[to].alive) {
                nodes[to].session->OnPacket(d, n, now);
            }
        });
        // 2. サーバ: 確定 → sim → 記録。(Hosting イベントはこの試験には無い)
        NetConfirmedTick ct;
        for (int i = 0; i < 8 && srv.TryConfirm(now, ct); ++i) {
            const uint64_t hash = serverSim.RunTick(ct.inputs, &ct.sys, false);
            res.log.push_back(ct);
            res.serverHash.push_back(hash);
            srv.OnTickRan(ct.tick, hash);
        }
        srv.Pump(now);
        // 3. クライアント
        for (int k = 1; k <= kClients; ++k) {
            if (nodes[k].alive) {
                sims.client[k - 1].Activate();
                nodes[k].runner->Update(now);
            }
        }
        if (def.traceMargin && now % 50 == 0) {
            for (int k = 1; k <= kClients; ++k) {
                if (nodes[k].alive && nodes[k].session->Running() && nodes[k].session->MarginValid()) {
                    res.client[k].marginTrace.emplace_back(now, nodes[k].session->MarginMs());
                }
            }
        }
        // 4. シナリオの操作
        while (nextAction < actions.size() && actions[nextAction].atMs <= now) {
            const Action& a = actions[nextAction++];
            switch (a.kind) {
            case Action::Start:
                startClient(a.client, 0);
                break;
            case Action::Kill:
                Harvest(nodes[a.client], res.client[a.client]);
                nodes[a.client].alive = false;
                nodes[a.client].session.reset();
                nodes[a.client].runner.reset();
                break;
            case Action::Restart: {
                // 落ちたクライアントの再起動 = 新しいセッションが前回の playerId を名乗る
                const ClientOutcome& o = res.client[a.client];
                const uint64_t pid = o.lanes.empty() ? 0 : o.lanes.back().second;
                startClient(a.client, pid);
                break;
            }
            case Action::PokeSim:
                if (nodes[a.client].alive) {
                    sims.client[a.client - 1].SetPokeTick(
                        static_cast<int64_t>(nodes[a.client].runner->TickIndex() + a.arg));
                }
                break;
            case Action::SkipSeq:
                srv.TestSkipEventSeq(a.arg);
                break;
            case Action::SetLink:
                net.Link(a.client).baseMs = a.arg; // 遅延の急変 (スパイク)
                break;
            }
        }
    }
    for (int k = 1; k <= kClients; ++k) {
        if (nodes[k].alive) {
            Harvest(nodes[k], res.client[k]);
        }
    }
    res.stats = srv.Stats();
    res.mirrorLanes = srv.Lanes();
    res.simLanes = serverSim.Refs().scene->Lanes();
    res.packetsDropped = net.Dropped();
    return res;
}

// 全クライアントの確定 tick のハッシュがサーバのハッシュ列と一致するか。from[k] 未満の tick は見ない
// (desync 試験で、再同期前の壊れた確定ハッシュを除く)。一致しない tick の数を返す
uint64_t CountChainMismatches(const ScenarioResult& r, const char* name, const uint64_t* from = nullptr)
{
    uint64_t bad = 0;
    for (int k = 1; k <= kClients; ++k) {
        uint64_t first = ~0ull;
        for (const auto& kv : r.client[k].committed) {
            if (from != nullptr && kv.first < from[k]) {
                continue;
            }
            if (kv.first >= r.serverHash.size() || r.serverHash[kv.first] != kv.second) {
                if (first == ~0ull) {
                    first = kv.first;
                }
                ++bad;
            }
        }
        if (first != ~0ull) {
            MYE_LOG_ERROR("[server-net selftest] %s: client %d first diverges from the server at tick %llu",
                          name, k, static_cast<unsigned long long>(first));
        }
    }
    return bad;
}

void LogScenario(const char* name, const ScenarioDef& def, const ScenarioResult& r)
{
    MYE_LOG_INFO("[server-net selftest] %s: %zu server ticks over %llu ms, dropped %llu packets; server: joins %llu "
                 "rejoins %llu leaves %llu releases %llu late-subst %llu late-drop %llu resyncs-served %llu "
                 "chunks %llu (re-sent %llu) client-desync-reports %llu",
                 name, r.log.size(), static_cast<unsigned long long>(def.endMs),
                 static_cast<unsigned long long>(r.packetsDropped),
                 static_cast<unsigned long long>(r.stats.joins), static_cast<unsigned long long>(r.stats.rejoins),
                 static_cast<unsigned long long>(r.stats.leaves), static_cast<unsigned long long>(r.stats.releases),
                 static_cast<unsigned long long>(r.stats.lateSubstitutions),
                 static_cast<unsigned long long>(r.stats.lateInputsDropped),
                 static_cast<unsigned long long>(r.stats.resyncsServed),
                 static_cast<unsigned long long>(r.stats.chunksSent),
                 static_cast<unsigned long long>(r.stats.chunkResends),
                 static_cast<unsigned long long>(r.stats.clientDesyncReports));
    for (int k = 1; k <= kClients; ++k) {
        const ClientOutcome& c = r.client[k];
        if (c.committed.empty() && c.lanes.empty()) {
            continue;
        }
        MYE_LOG_INFO("[server-net selftest]   client %d: committed %zu (last tick %llu), rollbacks %llu (max depth "
                     "%llu, resim %llu), predicted %llu, catch-up %llu, stalls %llu, resyncs %llu, gaps %llu, "
                     "snapshots %llu, desyncs %llu, rtt %.0f ms, margin %.0f ms",
                     k, c.committed.size(),
                     static_cast<unsigned long long>(c.committed.empty() ? 0 : c.committed.rbegin()->first),
                     static_cast<unsigned long long>(c.rollbacks), static_cast<unsigned long long>(c.maxDepth),
                     static_cast<unsigned long long>(c.resimTicks), static_cast<unsigned long long>(c.predictedTicks),
                     static_cast<unsigned long long>(c.catchUpTicks), static_cast<unsigned long long>(c.stalls),
                     static_cast<unsigned long long>(c.resyncs), static_cast<unsigned long long>(c.eventGaps),
                     static_cast<unsigned long long>(c.snapshots), static_cast<unsigned long long>(c.desyncs),
                     c.rttMs, c.marginMs);
    }
}

// サーバの確定入力列を別の経路 (初期スナップショットから回し直す) で再生してハッシュ列を比べる。
// 「ハッシュ列は確定入力列だけの関数」の直接の証明 (サーバの .rep を別の sim で再生するのと同じ)
void CheckReplayOfLog(Sims& sims, const ScenarioResult& r, const char* name)
{
    HeadlessSim& sim = sims.server;
    sim.Activate();
    const std::vector<std::byte>& start = r.startBlob.empty() ? sims.initialBlob : r.startBlob;
    if (!RestoreSimSnapshot(sim.Refs(), start.data(), start.size())) {
        Check(false, "[%s] could not reset the sim for the log replay", name);
        return;
    }
    uint64_t bad = 0;
    for (size_t i = 0; i < r.log.size(); ++i) {
        const uint64_t h = sim.RunTick(r.log[i].inputs, &r.log[i].sys, false);
        if (h != r.serverHash[i]) {
            if (bad == 0) {
                MYE_LOG_ERROR("[server-net selftest] %s: replaying the confirmed log diverges at tick %zu", name, i);
            }
            ++bad;
        }
    }
    Check(bad == 0, "%s: replaying the server's confirmed input log reproduces its hash chain (%llu bad ticks)", name,
          static_cast<unsigned long long>(bad));
}

// ABI v23 (A2): probe が読んだ参加・離脱の数が、確定入力列の中のイベント数と一致する。
// CheckReplayOfLog の直後 (server sim が確定列を最後まで回した状態) で呼ぶ。
// 全クライアントが同じ tick に同じイベントを読んだことは、probe の値がワールドハッシュに載っている
// ことと、CountChainMismatches (各クライアントの確定ハッシュ列 == サーバ) の両方で示される
void CheckEventProbe(Sims& sims, const ScenarioResult& r, const char* name)
{
    if (!HasEventProbe()) {
        MYE_LOG_WARN("[server-net selftest] %s: NetEventProbe is not registered (GameLogic.dll not loaded) - ABI v23 "
                     "probe checks skipped", name);
        return;
    }
    uint64_t want[5] = {}; // Join / Leave / Rejoin / Release, 最後の eventSeq
    uint64_t lastEventTick = 0;
    for (size_t i = 0; i < r.log.size(); ++i) {
        const SystemInputTick& s = r.log[i].sys;
        for (uint32_t e = 0; e < s.eventCount; ++e) {
            const uint8_t kind = s.events[e].kind;
            if (kind >= 1 && kind <= 4) {
                ++want[kind - 1];
            }
            want[4] = s.events[e].eventSeq;
            lastEventTick = i;
        }
    }
    HeadlessSim& sim = sims.server;
    sim.Activate();
    uint64_t got[7] = {};
    const char* fields[7] = { "joinCount", "leaveCount", "rejoinCount", "releaseCount", "lastEventSeq",
                              "lastEventTick", "laneMask" };
    bool readable = true;
    for (int i = 0; i < 7; ++i) {
        readable = ReadProbe(sim, fields[i], got[i]) && readable;
    }
    Check(readable, "%s: the NetEventProbe fields are readable", name);
    if (!readable) {
        return;
    }
    Check(got[0] == want[0] && got[1] == want[1] && got[2] == want[2] && got[3] == want[3] && got[4] == want[4],
          "%s: the probe counted join %llu/%llu leave %llu/%llu rejoin %llu/%llu release %llu/%llu lastSeq %llu/%llu "
          "(read/in the confirmed input)", name, (unsigned long long)got[0], (unsigned long long)want[0],
          (unsigned long long)got[1], (unsigned long long)want[1], (unsigned long long)got[2],
          (unsigned long long)want[2], (unsigned long long)got[3], (unsigned long long)want[3],
          (unsigned long long)got[4], (unsigned long long)want[4]);
    Check(want[0] > 0 && want[1] > 0 && want[2] > 0 && want[3] > 0,
          "%s: the scenario exercises all four event kinds (join/leave/rejoin/release)", name);
    // probe は Update (tick の中) で読むので、イベントを適用した tick そのものを記録している
    Check(got[5] == lastEventTick, "%s: the probe read the last event on tick %llu (applied on tick %llu)", name,
          (unsigned long long)got[5], (unsigned long long)lastEventTick);
    // 最終レーン状態から導いたマスクと probe の最後の観測が一致 (最終 tick の Update で読んだ値)
    uint32_t mask = 0;
    for (uint32_t l = 0; l < kMaxPlayers; ++l) {
        if (sim.Refs().scene->Lanes().lanes[l].state == static_cast<uint32_t>(LaneState::Connected)) {
            mask |= 1u << l;
        }
    }
    Check(got[6] == mask, "%s: the probe's lane mask %llx equals the final SessionLanes' %x", name,
          (unsigned long long)got[6], mask);
    // 負の対照: probe の値はワールドハッシュに載っている (載っていなければ chain 一致は何も示さない)
    const uint64_t before = sim.WorldHash();
    ScriptApiContext apiCtx;
    apiCtx.scene = sim.Refs().scene;
    MyeEngineApi api = {};
    BuildEngineApi(api, &apiCtx);
    const GameObject host = apiCtx.scene->Find("NetEventProbeHost");
    const MyeEntityId id = { host.Id().index, host.Id().generation };
    const int32_t bumped = static_cast<int32_t>(got[0]) + 1;
    const bool wrote = api.SetComponentField(&apiCtx, id, HashStr("NetEventProbe"), HashStr("joinCount"), &bumped,
                                             sizeof(bumped)) == 1;
    const uint64_t after = sim.WorldHash();
    Check(wrote && after != before, "%s (negative control): changing the probe's joinCount changes the world hash", name);
}

LinkSpec Link(uint32_t base, uint32_t jitter, uint32_t loss, uint32_t reorder, uint32_t dup)
{
    LinkSpec s;
    s.baseMs = base;
    s.jitterMs = jitter;
    s.lossPct = loss;
    s.reorderPct = reorder;
    s.dupPct = dup;
    return s;
}

// N1: 締め切り超過・途中参加・切断→再接続・予約のタイムアウト→Release・ロス 20%・並べ替え
void TestLifecycle(Sims& sims)
{
    ScenarioDef d;
    d.name = "N1 lifecycle (20% loss, reorder, dup)";
    d.seed = 101;
    d.endMs = 16000;
    d.deadlineTicks = 3;
    d.rejoinTimeoutTicks = 150;
    d.peerTimeoutMs = 1000;
    for (int k = 1; k <= kClients; ++k) {
        d.link[k] = Link(25, 15, 20, 10, 5);
    }
    d.actions = {
        { Action::Start, 0, 1, 0 },
        { Action::Start, 10, 2, 0 },
        { Action::Start, 3000, 3, 0 },   // 途中参加 (tick 180 付近)
        { Action::Kill, 5000, 2, 0 },    // クライアント 2 が突然消える
        { Action::Restart, 7500, 2, 0 }, // 予約が生きているうちに同じ playerId で戻る
        { Action::Kill, 9500, 1, 0 },    // クライアント 1 は二度と戻らない -> Reserved -> Release
    };
    ScenarioResult r = RunScenario(sims, d);
    LogScenario(d.name, d, r);
    Check(CountChainMismatches(r, d.name) == 0, "%s: every client's committed hash chain equals the server's", d.name);
    CheckReplayOfLog(sims, r, d.name);
    CheckEventProbe(sims, r, d.name);
    Check(r.stats.joins == 3 && r.stats.rejoins == 1, "%s: joins %llu (want 3) / rejoins %llu (want 1)", d.name,
          static_cast<unsigned long long>(r.stats.joins), static_cast<unsigned long long>(r.stats.rejoins));
    Check(r.stats.leaves >= 2 && r.stats.releases == 1, "%s: leaves %llu (want >= 2) / releases %llu (want 1)", d.name,
          static_cast<unsigned long long>(r.stats.leaves), static_cast<unsigned long long>(r.stats.releases));
    Check(std::memcmp(r.mirrorLanes.lanes, r.simLanes.lanes, sizeof(r.mirrorLanes.lanes)) == 0
              && r.mirrorLanes.lastEventSeq == r.simLanes.lastEventSeq,
          "%s: the server's lane mirror equals the sim's SessionLanes", d.name);

    // 同じ playerId・同じレーンで戻ったか
    const ClientOutcome& c2 = r.client[2];
    Check(c2.lanes.size() == 2 && c2.lanes[0] == c2.lanes[1],
          "%s: client 2 rejoined with the same lane and playerId (%zu sessions)", d.name, c2.lanes.size());
    const ClientOutcome& c1 = r.client[1];
    const ClientOutcome& c3 = r.client[3];
    Check(!c1.lanes.empty() && !c3.lanes.empty() && !c2.lanes.empty(), "%s: every client got a lane", d.name);
    if (!c1.lanes.empty() && !c3.lanes.empty() && !c2.lanes.empty()) {
        Check(c1.lanes[0].first != c2.lanes[0].first && c2.lanes[0].first != c3.lanes[0].first
                  && c1.lanes[0].first != c3.lanes[0].first,
              "%s: the three players hold different lanes", d.name);
        // 最終状態: 1 は Release で Empty、2 / 3 は Connected
        const uint32_t l1 = c1.lanes[0].first, l2 = c2.lanes[0].first, l3 = c3.lanes[0].first;
        Check(r.simLanes.lanes[l1].state == static_cast<uint32_t>(LaneState::Empty),
              "%s: the player who never came back was released (lane %u state %u)", d.name, l1,
              r.simLanes.lanes[l1].state);
        Check(r.simLanes.lanes[l2].state == static_cast<uint32_t>(LaneState::Connected)
                  && r.simLanes.lanes[l2].playerId == c2.lanes[0].second,
              "%s: the rejoined player is connected on its old lane", d.name);
        Check(r.simLanes.lanes[l3].state == static_cast<uint32_t>(LaneState::Connected),
              "%s: the late joiner is connected", d.name);
    }
    // 途中参加は tick > 0 から
    Check(!c3.committed.empty() && c3.committed.begin()->first > 100,
          "%s: the late joiner started at tick %llu (> 100)", d.name,
          static_cast<unsigned long long>(c3.committed.empty() ? 0 : c3.committed.begin()->first));
    // 再接続後の確定 tick が増えている (切断前だけで終わっていない)
    Check(!c2.committed.empty() && c2.committed.rbegin()->first > r.log.size() * 3 / 4,
          "%s: the reconnected client keeps committing ticks to the end", d.name);
    // 20% ロスでも rollback が働いた (予測が当たらない局面があった)
    Check(c1.rollbacks + c2.rollbacks + c3.rollbacks > 0, "%s: rollbacks happened under loss", d.name);
}

// N1: 締め切り超過。遅い 1 人の入力は代替入力 (前 tick の確定入力から消費型を落としたもの) で確定される
void TestDeadlineOverrun(Sims& sims)
{
    ScenarioDef d;
    d.name = "N1 deadline overrun";
    d.seed = 202;
    d.endMs = 8000;
    d.deadlineTicks = 2;
    d.link[1] = Link(20, 5, 0, 0, 0);
    d.link[2] = Link(20, 5, 0, 0, 0);
    d.link[3] = Link(20, 5, 0, 0, 0);
    d.actions = {
        { Action::Start, 0, 1, 0 },
        { Action::Start, 5, 2, 0 },
        { Action::Start, 10, 3, 0 },
        { Action::SetLink, 3000, 3, 250 }, // クライアント 3 の片道遅延が 250ms に跳ねる: 入力が締め切りに間に合わない
        { Action::SetLink, 5500, 3, 20 },  // 回復
    };
    ScenarioResult r = RunScenario(sims, d);
    LogScenario(d.name, d, r);
    Check(CountChainMismatches(r, d.name) == 0, "%s: every client's committed hash chain equals the server's", d.name);
    CheckReplayOfLog(sims, r, d.name);
    Check(r.stats.lateSubstitutions > 0, "%s: the slow player's inputs were substituted (%llu)", d.name,
          static_cast<unsigned long long>(r.stats.lateSubstitutions));
    // 代替入力 = 前 tick の確定入力から消費型フィールドを落とした値 (SubstituteLateInput)。
    // 遅い人のレーンで、本人の入力 (SynthLaneInput) と違う確定入力は全部この規則どおりのはず
    if (!r.client[3].lanes.empty()) {
        const uint32_t lane = r.client[3].lanes[0].first;
        uint64_t substituted = 0;
        uint64_t violations = 0;
        for (size_t t = 1; t < r.log.size(); ++t) {
            const InputSnapshot real = SynthLaneInput(t, lane);
            if (std::memcmp(&r.log[t].inputs[lane], &real, sizeof(real)) == 0) {
                continue;
            }
            ++substituted;
            const InputSnapshot expect = SubstituteLateInput(r.log[t - 1].inputs[lane]);
            if (std::memcmp(&r.log[t].inputs[lane], &expect, sizeof(expect)) != 0) {
                ++violations;
            }
        }
        Check(substituted > 0 && violations == 0,
              "%s: every non-real input of the slow lane is SubstituteLateInput(previous) (%llu substituted, %llu violations)",
              d.name, static_cast<unsigned long long>(substituted), static_cast<unsigned long long>(violations));
    }
    Check(r.client[3].rollbacks > 0, "%s: the slow client rolled back when the substituted inputs arrived", d.name);
    Check(!r.client[3].committed.empty() && r.client[3].committed.rbegin()->first > r.log.size() * 3 / 4,
          "%s: the slow client recovers and keeps committing after the spike", d.name);
    Check(r.stats.lateInputsDropped > 0, "%s: the late inputs were dropped, not applied afterwards (%llu)", d.name,
          static_cast<unsigned long long>(r.stats.lateInputsDropped));
}

// N2: 到着順・遅延パターンだけ変えて 3 通り流す。確定入力列が同じならハッシュ列も同じ
void TestOrderIndependence(Sims& sims)
{
    ScenarioResult runs[3];
    const char* names[3] = { "N2 pattern A (10/15/20ms)", "N2 pattern B (30/40/50ms, jitter, reorder)",
                             "N2 pattern C (60/75/90ms, jitter, reorder, dup)" };
    const LinkSpec patterns[3][kClients + 1] = {
        { {}, Link(10, 0, 0, 0, 0), Link(15, 0, 0, 0, 0), Link(20, 0, 0, 0, 0) },
        { {}, Link(30, 5, 0, 10, 5), Link(40, 5, 0, 10, 5), Link(50, 5, 0, 10, 5) },
        { {}, Link(60, 8, 0, 15, 10), Link(75, 8, 0, 15, 10), Link(90, 8, 0, 15, 10) },
    };
    for (int i = 0; i < 3; ++i) {
        ScenarioDef d;
        d.name = names[i];
        d.seed = 300 + static_cast<uint64_t>(i);
        d.endMs = 9000;
        d.serverStartMs = 600;       // Hello が全員ぶん届いてから始める (参加イベントの順序をそろえる)
        d.deadlineTicks = 600;       // 締め切りは実質無し: 待たされるだけで代替入力は入らない
        d.quietTicks = 150;          // 参加直後の稼働タイミングの差が入力列に出ないよう、最初はゼロ入力
        for (int k = 1; k <= kClients; ++k) {
            d.link[k] = patterns[i][k];
        }
        d.actions = { { Action::Start, 0, 1, 0 }, { Action::Start, 1, 2, 0 }, { Action::Start, 2, 3, 0 } };
        runs[i] = RunScenario(sims, d);
        LogScenario(d.name, d, runs[i]);
        Check(CountChainMismatches(runs[i], d.name) == 0, "%s: every client's committed hash chain equals the server's",
              d.name);
        CheckReplayOfLog(sims, runs[i], d.name);
        Check(runs[i].stats.lateSubstitutions == 0, "%s: nothing was substituted (%llu)", d.name,
              static_cast<unsigned long long>(runs[i].stats.lateSubstitutions));
    }
    for (int i = 1; i < 3; ++i) {
        const size_t n = (std::min)(runs[0].log.size(), runs[i].log.size());
        size_t common = 0;
        while (common < n && NetConfirmedTickEqual(runs[0].log[common], runs[i].log[common], kMaxPlayers)) {
            ++common;
        }
        Check(n >= 400 && common == n, "N2: pattern %c has the same confirmed input log as pattern A (%zu of %zu ticks equal)",
              'A' + i, common, n);
        size_t sameHash = 0;
        while (sameHash < common && runs[0].serverHash[sameHash] == runs[i].serverHash[sameHash]) {
            ++sameHash;
        }
        Check(sameHash == common, "N2: pattern %c has the same hash chain as pattern A (%zu of %zu ticks equal)",
              'A' + i, sameHash, common);
    }
}

// N3: eventSeq の欠番 -> 再同期要求 -> 以降一致
void TestEventGap(Sims& sims)
{
    ScenarioDef d;
    d.name = "N3 eventSeq gap";
    d.seed = 404;
    d.endMs = 9000;
    for (int k = 1; k <= kClients; ++k) {
        d.link[k] = Link(25, 10, 5, 5, 0);
    }
    d.actions = {
        { Action::Start, 0, 1, 0 },
        { Action::Start, 5, 2, 0 },
        { Action::SkipSeq, 4000, 0, 3 }, // 次のイベントの eventSeq が 3 飛ぶ
        { Action::Start, 4200, 3, 0 },   // その参加イベントで全員が欠番を見る
    };
    ScenarioResult r = RunScenario(sims, d);
    LogScenario(d.name, d, r);
    Check(CountChainMismatches(r, d.name) == 0, "%s: every committed hash equals the server's", d.name);
    CheckReplayOfLog(sims, r, d.name);
    for (int k = 1; k <= 2; ++k) {
        Check(r.client[k].eventGaps >= 1 && r.client[k].resyncs >= 1,
              "%s: client %d saw the gap (%llu) and asked for a resync (%llu)", d.name, k,
              static_cast<unsigned long long>(r.client[k].eventGaps),
              static_cast<unsigned long long>(r.client[k].resyncs));
        Check(r.client[k].snapshots >= 2, "%s: client %d received a fresh snapshot (%llu snapshots)", d.name, k,
              static_cast<unsigned long long>(r.client[k].snapshots));
        Check(!r.client[k].committed.empty() && r.client[k].committed.rbegin()->first > r.log.size() * 3 / 4,
              "%s: client %d keeps committing after the resync", d.name, k);
    }
    Check(r.stats.resyncsServed >= 2, "%s: the server served the resyncs (%llu)", d.name,
          static_cast<unsigned long long>(r.stats.resyncsServed));
}

// N3: desync を注入 -> 診断バンドルを出して再同期 -> 以降一致
void TestDesync(Sims& sims)
{
    ScenarioDef d;
    d.name = "N3 desync";
    d.seed = 505;
    d.endMs = 9000;
    for (int k = 1; k <= kClients; ++k) {
        d.link[k] = Link(25, 8, 0, 0, 0);
    }
    d.actions = {
        { Action::Start, 0, 1, 0 },
        { Action::Start, 5, 2, 0 },
        { Action::Start, 10, 3, 0 },
        { Action::PokeSim, 4000, 2, 40 }, // クライアント 2 の sim だけ、40 tick 先で 1 フィールドを壊す
    };
    ScenarioResult r = RunScenario(sims, d);
    LogScenario(d.name, d, r);
    const ClientOutcome& c2 = r.client[2];
    Check(c2.desyncs >= 1, "%s: the poked client detected the desync (%llu)", d.name,
          static_cast<unsigned long long>(c2.desyncs));
    Check(r.client[1].desyncs == 0 && r.client[3].desyncs == 0, "%s: the other clients saw no desync", d.name);
    Check(!c2.bundleDir.empty() && std::filesystem::exists(std::filesystem::path(c2.bundleDir) / L"desync.txt")
              && std::filesystem::exists(std::filesystem::path(c2.bundleDir) / L"local.rep"),
          "%s: a desync bundle (desync.txt + local.rep) was written", d.name);
    Check(c2.resyncs >= 1 && c2.snapshots >= 2, "%s: the poked client resynced from a fresh snapshot (%llu / %llu)",
          d.name, static_cast<unsigned long long>(c2.resyncs), static_cast<unsigned long long>(c2.snapshots));
    // 壊れた確定ハッシュが実際に記録された (試験が空振りでない) ことと、再同期後は一致することを確かめる
    uint64_t wrong = 0;
    for (const auto& kv : c2.committed) {
        if (kv.first < r.serverHash.size() && r.serverHash[kv.first] != kv.second) {
            ++wrong;
        }
    }
    Check(wrong > 0, "%s: the poke really changed the client's committed hashes (%llu ticks)", d.name,
          static_cast<unsigned long long>(wrong));
    uint64_t from[kClients + 1] = {};
    from[2] = c2.lastSnapshotTick;
    Check(CountChainMismatches(r, d.name, from) == 0,
          "%s: all hashes match the server again from the resync snapshot (tick %llu) on", d.name,
          static_cast<unsigned long long>(c2.lastSnapshotTick));
    Check(!c2.committed.empty() && c2.committed.rbegin()->first > r.log.size() * 3 / 4,
          "%s: the client keeps committing after the resync", d.name);
    CheckReplayOfLog(sims, r, d.name);
    Check(r.stats.clientDesyncReports >= 1, "%s: the server noticed the client's wrong checkpoint (%llu)", d.name,
          static_cast<unsigned long long>(r.stats.clientDesyncReports));
}

// N4: 予測上限 12 で RTT 150ms 相当は stall しない。RTT 300ms 相当は stall するが sim は一致
void TestSpeculationLimit(Sims& sims)
{
    ScenarioDef d;
    d.name = "N4 RTT 150ms, speculation 12";
    d.seed = 606;
    d.endMs = 6500;
    d.deadlineTicks = 4;
    for (int k = 1; k <= kClients; ++k) {
        d.link[k] = Link(75, 0, 0, 0, 0);
    }
    d.actions = { { Action::Start, 0, 1, 0 }, { Action::Start, 5, 2, 0 }, { Action::Start, 10, 3, 0 } };
    ScenarioResult a = RunScenario(sims, d);
    LogScenario(d.name, d, a);
    Check(CountChainMismatches(a, d.name) == 0, "%s: hash chains match", d.name);
    uint64_t stalls150 = 0;
    for (int k = 1; k <= kClients; ++k) {
        stalls150 += a.client[k].stalls;
        Check(a.client[k].committed.size() > 250, "%s: client %d made progress (%zu ticks)", d.name, k,
              a.client[k].committed.size());
    }
    Check(stalls150 == 0, "%s: no client stalled (%llu stalls)", d.name, static_cast<unsigned long long>(stalls150));

    ScenarioDef e = d;
    e.name = "N4 RTT 300ms, speculation 12";
    e.seed = 607;
    for (int k = 1; k <= kClients; ++k) {
        e.link[k] = Link(150, 0, 0, 0, 0);
    }
    ScenarioResult b = RunScenario(sims, e);
    LogScenario(e.name, e, b);
    Check(CountChainMismatches(b, e.name) == 0, "%s: hash chains match", e.name);
    uint64_t stalls300 = 0;
    for (int k = 1; k <= kClients; ++k) {
        stalls300 += b.client[k].stalls;
    }
    Check(stalls300 > 0, "%s: clients stall at the limit (%llu stalls)", e.name,
          static_cast<unsigned long long>(stalls300));
    CheckReplayOfLog(sims, b, e.name);
}

// 到着余裕の標本 (50ms ごと) を [fromMs, toMs) の 1 秒窓で平均し、全部の窓が目標 ± 1 tick に入るか。
// 窓の数を返す (0 = 標本が無い = 検査していない)。外れた窓があれば最初の 1 つを name 付きで報告する
int CheckMarginWindows(const ClientOutcome& c, uint64_t fromMs, uint64_t toMs, const char* name, int client)
{
    constexpr double kTarget = 16.0; // ClientSessionConfig::targetMarginMs の既定
    constexpr double kTickMs = 1000.0 / 60.0;
    int windows = 0;
    std::string means;
    for (uint64_t w = fromMs; w + 1000 <= toMs; w += 1000) {
        double sum = 0.0;
        int n = 0;
        for (const auto& s : c.marginTrace) {
            if (s.first >= w && s.first < w + 1000) {
                sum += s.second;
                ++n;
            }
        }
        if (n == 0) {
            Check(false, "%s: client %d has no margin sample in [%llu, %llu) ms", name, client,
                  static_cast<unsigned long long>(w), static_cast<unsigned long long>(w + 1000));
            continue;
        }
        ++windows;
        const double mean = sum / n;
        means += (means.empty() ? "" : " ") + std::to_string(static_cast<int>(std::lround(mean)));
        Check(std::fabs(mean - kTarget) <= kTickMs,
              "%s: client %d's arrival margin averages %.1f ms in [%llu, %llu) ms (target %.0f +- %.1f)", name, client,
              mean, static_cast<unsigned long long>(w), static_cast<unsigned long long>(w + 1000), kTarget, kTickMs);
    }
    MYE_LOG_INFO("[server-net selftest] %s: client %d arrival margin, 1 s window means in [%llu, %llu) ms: %s ms", name,
                 client, static_cast<unsigned long long>(fromMs), static_cast<unsigned long long>(toMs), means.c_str());
    return windows;
}

// V7 (spec 4.1.6): 到着余裕が目標 (1 tick) に収束する。追いつきと速度係数が同じ基準 (到着余裕) から導かれるので、
// 遅延が一定ならロス 0 でも参加の 10 秒後以降は目標 +- 1 tick に入り、遅延が変わっても 10 秒後に戻る。
// 以前は 2 つが別の基準 (到着余裕 / 確定フロンティア) を追って打ち消し合い、約 90ms に張り付いた
void TestTimeSync(Sims& sims)
{
    {
        ScenarioDef d;
        d.name = "V7 time sync (1 client, 25ms one-way, then 75ms)";
        d.seed = 701;
        d.endMs = 40000;
        d.traceMargin = true;
        d.link[1] = Link(25, 0, 0, 0, 0);
        d.actions = {
            { Action::Start, 0, 1, 0 },
            { Action::SetLink, 20000, 1, 75 }, // 片道 25 -> 75ms (往復 50 -> 150ms)
        };
        ScenarioResult r = RunScenario(sims, d);
        LogScenario(d.name, d, r);
        Check(CountChainMismatches(r, d.name) == 0, "%s: the client's committed hash chain equals the server's", d.name);
        const int before = CheckMarginWindows(r.client[1], 10000, 20000, d.name, 1);
        const int after = CheckMarginWindows(r.client[1], 30000, 40000, d.name, 1);
        Check(before == 10 && after == 10, "%s: 10 + 10 one-second windows were checked (%d + %d)", d.name, before, after);
        const double lateRate = r.stats.laneWaitedTicks[0] > 0
            ? static_cast<double>(r.stats.laneLateSubst[0]) / static_cast<double>(r.stats.laneWaitedTicks[0])
            : 1.0;
        MYE_LOG_INFO("[server-net selftest] %s: late-subst %llu of %llu waited tick(s) (%.2f%%), catch-up %llu tick(s)",
                     d.name, static_cast<unsigned long long>(r.stats.laneLateSubst[0]),
                     static_cast<unsigned long long>(r.stats.laneWaitedTicks[0]), lateRate * 100.0,
                     static_cast<unsigned long long>(r.client[1].catchUpTicks));
    }
    {
        // 参加の時点で遅れが大きい (往復 180ms = Hello → Welcome → スナップショット受信に半秒かかる。往復 300ms は予測上限 12 tick では取り戻せない = N4) クライアントも、
        // 追いつき (到着余裕から導く) で埋めて目標へ収束する
        ScenarioDef d;
        d.name = "V7 time sync (1 client, 90ms one-way: joins half a second behind)";
        d.seed = 703;
        d.endMs = 30000;
        d.traceMargin = true;
        d.link[1] = Link(90, 0, 0, 0, 0);
        d.actions = { { Action::Start, 0, 1, 0 } };
        ScenarioResult r = RunScenario(sims, d);
        LogScenario(d.name, d, r);
        Check(CountChainMismatches(r, d.name) == 0, "%s: the client's committed hash chain equals the server's", d.name);
        Check(CheckMarginWindows(r.client[1], 10000, 30000, d.name, 1) == 20, "%s: 20 one-second windows were checked",
              d.name);
        Check(r.client[1].catchUpTicks > 0, "%s: the client caught up after joining behind (%llu catch-up ticks)", d.name,
              static_cast<unsigned long long>(r.client[1].catchUpTicks));
    }
    {
        ScenarioDef d;
        d.name = "V7 time sync (3 clients, 30ms +- 10 jitter)";
        d.seed = 702;
        d.endMs = 30000;
        d.traceMargin = true;
        for (int k = 1; k <= kClients; ++k) {
            d.link[k] = Link(30, 10, 0, 0, 0);
        }
        d.actions = { { Action::Start, 0, 1, 0 }, { Action::Start, 40, 2, 0 }, { Action::Start, 80, 3, 0 } };
        ScenarioResult r = RunScenario(sims, d);
        LogScenario(d.name, d, r);
        Check(CountChainMismatches(r, d.name) == 0, "%s: every client's committed hash chain equals the server's", d.name);
        for (int k = 1; k <= kClients; ++k) {
            Check(CheckMarginWindows(r.client[k], 10000, 30000, d.name, k) == 20,
                  "%s: client %d: 20 one-second windows were checked", d.name, k);
        }
    }
}
// システムイベントをまたぐロールバック: 予測 (イベント無し) で走った区間に Join / Leave が確定で届いたら、
// 直前のスナップショットへ戻して tick ごとの記録値で再シムすると、最初から確定入力で走った sim と
// ビット同一の結果になる
void TestRollbackAcrossSystemEvents(Sims& sims)
{
    HeadlessSim& sim = sims.server;
    const auto reset = [&]() {
        sim.Activate();
        return RestoreSimSnapshot(sim.Refs(), sims.initialBlob.data(), sims.initialBlob.size());
    };
    // 実際の確定列: tick 5 に Join(playerId 1)、tick 9 に Join(2)、tick 12 に Leave(1)。入力は SynthLaneInput
    constexpr uint64_t kTicks = 16;
    NetConfirmedTick actual[kTicks];
    uint64_t seq = 0;
    for (uint64_t t = 0; t < kTicks; ++t) {
        actual[t].tick = t;
        for (uint32_t p = 0; p < kMaxPlayers; ++p) {
            actual[t].inputs[p] = SynthLaneInput(t, p);
        }
    }
    const auto addEvent = [&](uint64_t tick, SystemEventKind kind, uint64_t playerId, uint8_t lane) {
        SystemInputTick& s = actual[tick].sys;
        s.events[s.eventCount].eventSeq = ++seq;
        s.events[s.eventCount].playerId = playerId;
        s.events[s.eventCount].kind = static_cast<uint8_t>(kind);
        s.events[s.eventCount].lane = lane;
        ++s.eventCount;
    };
    addEvent(5, SystemEventKind::Join, 1, 0);
    addEvent(9, SystemEventKind::Join, 2, 0);
    addEvent(12, SystemEventKind::Leave, 1, 0);

    // 基準: 最初から確定入力で走る
    Check(reset(), "rollback test: reset the sim");
    uint64_t truth[kTicks] = {};
    for (uint64_t t = 0; t < kTicks; ++t) {
        truth[t] = sim.RunTick(actual[t].inputs, &actual[t].sys, false);
    }
    const SessionLanes truthLanes = sim.Refs().scene->Lanes();

    // 予測で走る (システム入力 = イベント無し) -> 確定が届いて食い違い -> 巻き戻して再シム
    Check(reset(), "rollback test: reset the sim again");
    NetRollback rb;
    Check(rb.Begin(sim.Refs(), 0, kNetMaxSpeculationClient) && rb.MaxSpeculation() == 12,
          "rollback test: the ring starts with the client limit of 12");
    for (uint64_t t = 0; t < kTicks; ++t) {
        SystemInputTick none = {};
        const uint64_t h = sim.RunTick(actual[t].inputs, &none, false); // レーン入力は当たった、イベントだけ未着
        rb.OnTickEnd(sim.Refs(), t, actual[t].inputs, kMaxPlayers, h, true, true, &none);
    }
    Check(sim.Refs().scene->Lanes().lanes[0].state != static_cast<uint32_t>(LaneState::Connected),
          "rollback test: the predicted run has not seen the join");
    Check(!rb.SystemMatch(5, actual[5].sys) && rb.SystemMatch(4, actual[4].sys),
          "rollback test: a missed system event is a mismatch, an identical empty one is not");
    // tick 5 の直前へ戻して、tick ごとに記録値を差し替えて再シム (EngineLoop::NetResimFrom と同じ手順)
    const std::vector<std::byte>* blob = rb.SnapshotBefore(5);
    Check(blob != nullptr && RestoreSimSnapshot(sim.Refs(), blob->data(), blob->size()),
          "rollback test: the snapshot before tick 5 restores");
    uint64_t resim[kTicks] = {};
    for (uint64_t t = 5; t < kTicks; ++t) {
        resim[t] = sim.RunTick(actual[t].inputs, &actual[t].sys, true);
    }
    uint64_t bad = 0;
    for (uint64_t t = 5; t < kTicks; ++t) {
        bad += (resim[t] != truth[t]) ? 1 : 0;
    }
    Check(bad == 0, "rollback test: re-simulating across Join / Leave events reproduces the confirmed run (%llu bad ticks)",
          static_cast<unsigned long long>(bad));
    Check(std::memcmp(sim.Refs().scene->Lanes().lanes, truthLanes.lanes, sizeof(truthLanes.lanes)) == 0
              && sim.Refs().scene->Lanes().lastEventSeq == truthLanes.lastEventSeq,
          "rollback test: the SessionLanes after the re-simulation equal the confirmed run's");
    // 差し替えを忘れた再シム (全 tick でイベント無し) は食い違う = この試験が差し替えの要否を見ている
    blob = rb.SnapshotBefore(5);
    Check(blob != nullptr && RestoreSimSnapshot(sim.Refs(), blob->data(), blob->size()),
          "rollback test: restore once more for the negative control");
    uint64_t wrongHash = 0;
    for (uint64_t t = 5; t < kTicks; ++t) {
        SystemInputTick none = {};
        wrongHash = sim.RunTick(actual[t].inputs, &none, true);
    }
    Check(wrongHash != truth[kTicks - 1], "rollback test (negative control): a re-simulation that keeps the predicted "
                                          "system input does NOT reproduce the confirmed run");
}

// サーバの確定入力列 r.log を .rep (role = Server) として書く。from > 0 なら from の直前の状態を開始
// スナップショットにして from 以降だけを記録する (参加途中のクライアントが録る .rep と同じ形)
// startMetaTickOffset != 0: ヘッダの startMeta.tick だけをずらす (埋め込みスナップショットの tick と食い違う壊れた .rep を作る)
bool WriteRepFromLog(Sims& sims, const ScenarioResult& r, const std::wstring& path, size_t from,
                     uint64_t startMetaTickOffset = 0)
{
    HeadlessSim& sim = sims.server;
    sim.Activate();
    const std::vector<std::byte>& start = r.startBlob.empty() ? sims.initialBlob : r.startBlob;
    if (!RestoreSimSnapshot(sim.Refs(), start.data(), start.size())) {
        return false;
    }
    from = (std::min)(from, r.log.size());
    for (size_t i = 0; i < from; ++i) {
        sim.RunTick(r.log[i].inputs, &r.log[i].sys, false);
    }
    std::vector<std::byte> blob;
    if (!CaptureSimSnapshot(sim.Refs(), blob)) {
        return false;
    }
    World& world = sim.Refs().scene->GetWorld();
    const SessionConfig session = SimSessionConfig(3, 150);
    SimProvenance prov = sim.Provenance();
    SnapshotMeta meta = {};
    meta.tick = sim.TickIndex() + startMetaTickOffset;
    meta.lastEventSeq = sim.Refs().scene->Lanes().lastEventSeq;
    meta.config = session;
    meta.blobHash = HashBytes(blob.data(), blob.size());
    meta.worldHash = sim.WorldHash();
    prov.initialSnapshotHash = meta.blobHash;
    meta.provenance = prov;
    ReplayRecorder rec;
    rec.Start(path, world.Rng().State(), world.Rng().Inc(), world.AliveCount(), kMaxPlayers, blob.data(), blob.size(),
              session, prov, meta);
    if (!rec.IsActive()) {
        return false;
    }
    for (size_t i = from; i < r.log.size(); ++i) {
        rec.RecordTick(r.log[i].inputs, kMaxPlayers, r.serverHash[i], &r.log[i].sys);
    }
    return rec.Finish();
}

struct OfflineVerify {
    bool ready = false;
    HeadlessVerifyResult result;
    TickGates gates;
    // 再生が終わった sim の NetInfoProbe (probe が無い環境では probeRead = false)
    bool probeRead = false;
    uint64_t probeConnectedTicks = 0;
    uint64_t probePlayerCount = 0;
};

// Server.exe --replay-verify と同じ経路 (systemInput = false の HeadlessSim で VerifyReplay)
// dumpPath 非空: dumpTick の tick 末のフィールド単位ダンプを書く (--hash-dump-tick / --hash-dump)
OfflineVerify VerifyRepOffline(const std::wstring& path, const std::wstring& dumpPath = L"", int64_t dumpTick = 0)
{
    OfflineVerify o;
    HeadlessSimSetup s;
    s.config.title = L"MyEngine ServerNetSelfTest verify";
    s.config.localPlayers = static_cast<int>(kMaxPlayers);
    s.config.replayVerifyPath = path;
    s.config.hashDumpPath = dumpPath;
    s.config.hashDumpTick = dumpTick;
    s.scene.showcase = FindShowcase(L"--local-demo", /*editor=*/true);
    HeadlessSim sim;
    if (s.scene.showcase == nullptr || !sim.Init(s)) {
        return o;
    }
    o.ready = true;
    o.gates = sim.Gates();
    o.result = sim.VerifyReplay();
    o.probeRead = ReadNetInfoProbe(sim, "connectedTicks", o.probeConnectedTicks)
        && ReadNetInfoProbe(sim, "playerCountSeen", o.probePlayerCount);
    return o;
}

// V3 (D14): サーバ構成の sim が v13 の Net* へ返す値。ゲームが tick の中で読みうる NetIsConnected / NetPlayerCount は
// クライアント (EngineLoop が connected = 稼働中、playerCount = セッションの人数 を書く) と同じ値になる
void CheckSessionNetInfo(HeadlessSim& sim, const char* who, uint32_t wantPlayers)
{
    ScriptApiContext apiCtx;
    apiCtx.net = &sim.NetInfo();
    MyeEngineApi api = {};
    BuildEngineApi(api, &apiCtx);
    Check(api.NetIsConnected(&apiCtx) == 1, "%s: NetIsConnected = 1 in a session sim (D14)", who);
    Check(api.NetPlayerCount(&apiCtx) == wantPlayers, "%s: NetPlayerCount = %u (the session's player count), got %u", who,
          wantPlayers, api.NetPlayerCount(&apiCtx));
    Check(api.NetLocalPlayer(&apiCtx) == 0, "%s: NetLocalPlayer = 0 (D14)", who);
    Check(sim.NetInfo().active && sim.NetInfo().role == static_cast<int>(NetRole::Server),
          "%s: the session sim's NetRuntimeInfo is active with role Server", who);
}

// V1 / V2 / V3 / V4: サーバの sim がネット中と同じ決定論の境界で回る。
// 実在のセーブファイルを置き、tick の中で LoadPersist / LoadGame を積む。境界が立っていれば、サーバもクライアントも
// セーブを sim へ読み込まない (ネット中は no-op) ので、ハッシュ列が一致し、サーバ .rep のオフライン再生 (Verifying) も一致する。
// 立っていなければ、サーバだけがセーブを読んで割れる (負の対照は SELF_EVAL の手順で境界を外して確認する)
void TestSaveLoadBoundary(Sims& sims)
{
    if (!HasSaveLoadProbe()) {
        MYE_LOG_WARN("[server-net selftest] SaveLoadProbe is not registered (GameLogic.dll not loaded) - the save "
                     "boundary checks are skipped");
        return;
    }
    constexpr int32_t kSlot = 7;
    constexpr int32_t kSavedValue = 1234;
    HeadlessSim& server = sims.server;

    // ゲート系フラグ (V2): セッション構成の sim はすべて同じ境界。再生専用の sim (systemInput = false) とは違う
    Check(IsNetSessionGates(server.Gates()), "server sim: the live tick gates are the net-session gates");
    for (int k = 0; k < kClients; ++k) {
        Check(IsNetSessionGates(sims.client[k].Gates()), "client sim %d: the live tick gates are the net-session gates", k + 1);
    }
    Check(NetLockstepBoundary(static_cast<int>(NetRole::Server)) && NetLockstepBoundary(static_cast<int>(NetRole::Client))
              && NetLockstepBoundary(static_cast<int>(NetRole::Host)) && !NetLockstepBoundary(0),
          "NetLockstepBoundary: every net role sets the boundary, no role does not");

    // 実在のセーブ (scenePath 無し = LoadGame もシーンを動かさない)
    const std::wstring savePath = SaveGameFile::PathForSlot(server.SaveDir(), kSlot);
    std::error_code ec;
    std::filesystem::create_directories(server.SaveDir(), ec);
    PersistStore saved;
    int32_t savedValue = kSavedValue;
    saved.Set(HashStr("save_probe.value"), &savedValue, sizeof(savedValue));
    Check(SaveGameFile::Write(savePath, L"", saved), "wrote a real save file for slot %d", kSlot);

    ScenarioDef d;
    d.name = "V1 LoadPersist / LoadGame in a session";
    d.seed = 707;
    d.endMs = 9000;
    d.persistLoadTick = 180;
    d.gameLoadTick = 360;
    d.saveSlot = kSlot;
    for (int k = 1; k <= kClients; ++k) {
        d.link[k] = Link(25, 10, 5, 5, 0);
    }
    d.actions = { { Action::Start, 0, 1, 0 }, { Action::Start, 5, 2, 0 }, { Action::Start, 10, 3, 0 } };
    ScenarioResult r = RunScenario(sims, d);
    LogScenario(d.name, d, r);
    Check(r.log.size() > 450, "%s: the scenario runs past both load ticks (%zu ticks)", d.name, r.log.size());
    Check(CountChainMismatches(r, d.name) == 0, "%s: every client's committed hash chain equals the server's", d.name);
    CheckReplayOfLog(sims, r, d.name);

    // 要求は積まれたが、セーブは sim へ入っていない (サーバ・クライアントとも)
    const auto checkProbe = [&](HeadlessSim& sim, const char* who) {
        uint64_t requests = 0, value = 0;
        sim.Activate();
        Check(ReadSaveProbe(sim, "requests", requests) && ReadSaveProbe(sim, "persistValue", value),
              "%s: the SaveLoadProbe fields are readable", who);
        Check(requests == 2, "%s: the script requested LoadPersist and LoadGame (%llu requests)", who,
              static_cast<unsigned long long>(requests));
        Check(static_cast<int32_t>(value) == -1 && sim.Refs().scene->Persist().Find(HashStr("save_probe.value")) == nullptr,
              "%s: the save file was NOT loaded into the sim during the session (persistValue %d)", who,
              static_cast<int32_t>(value));
    };
    checkProbe(server, "server sim");
    for (int k = 1; k <= kClients; ++k) {
        if (!r.client[k].committed.empty() && r.client[k].committed.rbegin()->first > 361) {
            checkProbe(sims.client[k - 1], k == 1 ? "client sim 1" : (k == 2 ? "client sim 2" : "client sim 3"));
        }
    }

    // サーバの確定ログの .rep を、Server.exe --replay-verify と同じ経路 (Verifying) で再生して一致 (V1)
    const std::filesystem::path dir(CrashRoot());
    std::filesystem::create_directories(dir, ec);
    const std::wstring fullRep = (dir / L"save_boundary_full.rep").wstring();
    Check(WriteRepFromLog(sims, r, fullRep, 0), "%s: wrote the server's confirmed log as a .rep", d.name);
    const OfflineVerify full = VerifyRepOffline(fullRep);
    Check(full.ready && full.result.ran && full.result.passed && full.result.verifiedTicks == r.log.size(),
          "%s: the offline replay of the server .rep matches every tick (verified %llu of %zu, reason '%s')", d.name,
          static_cast<unsigned long long>(full.result.verifiedTicks), r.log.size(), full.result.failReason.c_str());
    // V12: ゲームが v13 の Net* を毎 tick 読んで sim へ書いても (誤用だが)、サーバ .rep のオフライン再生は
    // ライブと同じ値を読む (再生専用の sim でも NetIsConnected = 1 / NetPlayerCount = 人数)。
    // 上の全 tick 一致 (ライブのサーバのハッシュ列と同じ) がその証拠で、ここでは probe が実際に値を読んだことも見る
    // (読んでいなければ一致は何も示さない)
    if (full.probeRead) {
        Check(full.probeConnectedTicks == r.log.size() && full.probePlayerCount == kMaxPlayers,
              "%s: the offline replay's NetInfoProbe read NetIsConnected = 1 on every tick and NetPlayerCount = %u (connected "
              "ticks %llu of %zu, player count %llu)", d.name, kMaxPlayers,
              static_cast<unsigned long long>(full.probeConnectedTicks), r.log.size(),
              static_cast<unsigned long long>(full.probePlayerCount));
    } else {
        MYE_LOG_WARN("[server-net selftest] %s: NetInfoProbe is not registered (GameLogic.dll not loaded) - V12 check skipped",
                     d.name);
    }
    // 再生専用の sim (systemInput = false) はセッションのゲートではない = IsNetSessionGates は両者を区別できる
    Check(full.ready && !IsNetSessionGates(full.gates) && full.gates.hasPlayer,
          "%s (negative control): the replay-only sim's gates are not the net-session gates", d.name);

    // V3: 参加中のクライアントが書く値 (稼働中 / セッションの人数) とサーバの Net* が同じ
    CheckSessionNetInfo(server, "server sim", kMaxPlayers);
    ScriptApiContext serverCtx;
    serverCtx.net = &server.NetInfo();
    MyeEngineApi serverApi = {};
    BuildEngineApi(serverApi, &serverCtx);
    for (int k = 1; k <= kClients; ++k) {
        const ClientOutcome& c = r.client[k];
        Check(c.running && c.sessionPlayerCount != 0, "%s: client %d is running with a session (%u lanes)", d.name, k,
              c.sessionPlayerCount);
        Check((serverApi.NetIsConnected(&serverCtx) == 1) == c.running
                  && serverApi.NetPlayerCount(&serverCtx) == c.sessionPlayerCount,
              "%s: the server's NetIsConnected / NetPlayerCount equal client %d's (%u lanes)", d.name, k,
              c.sessionPlayerCount);
    }
    for (int k = 0; k < kClients; ++k) {
        CheckSessionNetInfo(sims.client[k], k == 0 ? "client sim 1" : (k == 1 ? "client sim 2" : "client sim 3"),
                            kMaxPlayers);
    }

    // V4: 0 tick の照合は FAIL。tick が 0 本の .rep と、開始 tick が .rep の範囲外の .rep (参加途中のクライアントの .rep の形)
    const std::wstring emptyRep = (dir / L"save_boundary_empty.rep").wstring();
    Check(WriteRepFromLog(sims, r, emptyRep, r.log.size()), "%s: wrote a .rep with no tick records", d.name);
    const OfflineVerify empty = VerifyRepOffline(emptyRep);
    Check(empty.ready && empty.result.ran && !empty.result.passed && empty.result.verifiedTicks == 0
              && empty.result.failReason.find("no tick records") != std::string::npos,
          "%s: a .rep with no ticks FAILS (passed %d, verified %llu, reason '%s')", d.name, empty.result.passed ? 1 : 0,
          static_cast<unsigned long long>(empty.result.verifiedTicks), empty.result.failReason.c_str());
    // 開始 tick がスナップショットの tick と食い違う .rep (ヘッダが壊れている) は、範囲外として FAIL
    const std::wstring lateRep = (dir / L"save_boundary_late.rep").wstring();
    const size_t clientFrom = r.log.size() / 2;
    Check(WriteRepFromLog(sims, r, lateRep, clientFrom, /*startMetaTickOffset=*/1000), "%s: wrote a .rep whose header start "
          "tick (%zu) differs from its snapshot's", d.name, clientFrom + 1000);
    const OfflineVerify late = VerifyRepOffline(lateRep);
    Check(late.ready && late.result.ran && !late.result.passed && late.result.verifiedTicks == 0
              && late.result.failReason.find("outside the .rep's tick range") != std::string::npos,
          "%s: a .rep whose start tick is outside its record range FAILS (passed %d, verified %llu, reason '%s')", d.name,
          late.result.passed ? 1 : 0, static_cast<unsigned long long>(late.result.verifiedTicks),
          late.result.failReason.c_str());

    // V6: 参加途中のクライアントが録る .rep (開始 tick = 参加 tick ≠ 0) は単独で再生でき、全 tick 一致する。
    // ReplayPlayer が startMeta.tick を基点に引く。サーバ .rep (tick 0 から) の同じ tick のダンプとも一致する
    const std::wstring clientRep = (dir / L"save_boundary_client.rep").wstring();
    Check(WriteRepFromLog(sims, r, clientRep, clientFrom), "%s: wrote a client-shaped .rep that starts at tick %zu", d.name,
          clientFrom);
    const int64_t dumpTick = static_cast<int64_t>(clientFrom + 10);
    const std::wstring clientDump = (dir / L"save_boundary_client.dump").wstring();
    const std::wstring serverDump = (dir / L"save_boundary_server.dump").wstring();
    const OfflineVerify client = VerifyRepOffline(clientRep, clientDump, dumpTick);
    Check(client.ready && client.result.ran && client.result.passed && client.result.verifiedTicks == r.log.size() - clientFrom,
          "%s: a .rep that starts at tick %zu replays alone and matches every tick (verified %llu of %zu, reason '%s')", d.name,
          clientFrom, static_cast<unsigned long long>(client.result.verifiedTicks), r.log.size() - clientFrom,
          client.result.failReason.c_str());
    const OfflineVerify serverFull = VerifyRepOffline(fullRep, serverDump, dumpTick);
    HashDump dc;
    HashDump ds;
    const bool dumpsRead = ReadHashDump(clientDump, dc) && ReadHashDump(serverDump, ds);
    Check(serverFull.result.passed && dumpsRead && dc.tick == static_cast<uint64_t>(dumpTick) && !dc.lines.empty()
              && DiffHashDumps(dc, ds, 0).Same(),
          "%s: the client .rep's dump at tick %lld exists and equals the server .rep's dump at the same tick (%zu lines)",
          d.name, static_cast<long long>(dumpTick), dc.lines.size());
    // 判定関数そのもの
    {
        ReplayPlayer none;
        std::string reason;
        Check(!JudgeReplayVerification(none, 0, reason) && !reason.empty(),
              "JudgeReplayVerification: a player with no records is not a pass ('%s')", reason.c_str());
        ReplayPlayer mismatch;
        mismatch.failed = true;
        mismatch.firstMismatchTick = 9;
        Check(!JudgeReplayVerification(mismatch, 0, reason) && reason.find("tick 9") != std::string::npos,
              "JudgeReplayVerification: a hash mismatch is not a pass ('%s')", reason.c_str());
    }

    std::filesystem::remove(savePath, ec);
    std::filesystem::remove(fullRep, ec);
    std::filesystem::remove(emptyRep, ec);
    std::filesystem::remove(lateRep, ec);
    std::filesystem::remove(clientRep, ec);
    std::filesystem::remove(clientDump, ec);
    std::filesystem::remove(serverDump, ec);
}

} // namespace

bool RunServerNetSelfTest()
{
    g_fail = 0;
    MYE_LOG_INFO("==== Server/client net self test: start ====");

    // ---- sim 無しのプロトコル単体 ----
    {
        StepTimer t("protocol unit tests");
        TestTickRecordRoundTrip();
        TestRejectPaths();
        TestGracefulLeave();
        TestReconnectLeavesOnce();
        TestSnapshotTransfer();
    }

    // ---- sim あり: サーバ 1 + クライアント 3 を 1 プロセスに立てる ----
    auto sims = std::make_unique<Sims>();
    StepTimer simTimer("sims (init + scenarios)");
    if (!InitSim(sims->server)) {
        Check(false, "could not start the server sim");
    } else {
        bool ok = true;
        for (int k = 0; k < kClients; ++k) {
            ok = ok && InitSim(sims->client[k]);
        }
        Check(ok, "could not start the client sims");
        sims->server.Activate();
        Check(CaptureSimSnapshot(sims->server.Refs(), sims->initialBlob), "capture the initial snapshot");
        if (ok && !sims->initialBlob.empty()) {
            const auto timed = [&](const char* name, void (*fn)(Sims&)) {
                StepTimer t(name);
                fn(*sims);
            };
            timed("rollback across system events", &TestRollbackAcrossSystemEvents);
            timed("N1 lifecycle", &TestLifecycle);
            timed("N1 deadline overrun", &TestDeadlineOverrun);
            timed("N2 order independence", &TestOrderIndependence);
            timed("N3 eventSeq gap", &TestEventGap);
            timed("N3 desync", &TestDesync);
            timed("N4 speculation limit", &TestSpeculationLimit);
            timed("V7 time sync", &TestTimeSync);
            timed("V1-V4 save/load boundary, net info, empty replay", &TestSaveLoadBoundary);
        }
    }

    if (g_fail == 0) {
        MYE_LOG_INFO("==== Server/client net self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== Server/client net self test: %d FAILED ====", g_fail);
    return false;
}

} // namespace mye
