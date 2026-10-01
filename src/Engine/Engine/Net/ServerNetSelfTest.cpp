//====================================================================================
//                          ServerNetSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                     10/02/2026
//                                          入力確定型サーバの 1 プロセス内検証 (偽トランスポート) の実装
//====================================================================================
#include "Engine/Engine/Net/ServerNetSelfTest.h"

#include <algorithm>
#include <chrono>
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
#include "Engine/Engine/Loop/HeadlessSim.h"
#include "Engine/Engine/Net/ClientSession.h"
#include "Engine/Engine/Net/ClientSimRunner.h"
#include "Engine/Engine/Net/NetProtocol.h"
#include "Engine/Engine/Net/NetRollback.h"
#include "Engine/Engine/Net/ServerSession.h"
#include "Engine/Engine/Replay/CrashRing.h"
#include "Engine/Engine/Replay/SimSnapshot.h"
#include "Engine/Engine/Scene/GameObject.h"
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
    return true;
}

bool HasEventProbe()
{
    return ComponentRegistry::Get().FindByName("NetEventProbe") != kInvalidComponentType;
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
};

struct ScenarioResult {
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
    if (!RestoreSimSnapshot(sim.Refs(), sims.initialBlob.data(), sims.initialBlob.size())) {
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
