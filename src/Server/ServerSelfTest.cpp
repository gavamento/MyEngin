//====================================================================================
//                          ServerSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                     10/02/2026
//                                          GameLift ホスティング / Terminate 時の .rep 幕引きの自己テスト
//====================================================================================
#include "Server/ServerSelfTest.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <Windows.h>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Engine/Demo/ShowcaseScenes.h"
#include "Engine/Engine/Loop/HeadlessSim.h"
#include "Engine/Engine/Loop/SimInit.h"
#include "Engine/Engine/Navigation/NavDeterminismSelfTest.h"
#include "Engine/Engine/Navigation/NavAgentSelfTest.h"
#include "Engine/Engine/Navigation/NavSurfaceSelfTest.h"
#include "Engine/Engine/Net/ServerSession.h"
#include "Engine/Engine/Replay/Replay.h"
#include "Engine/Platform/PathUtil.h"
#include "Server/Hosting/GameLiftHosting.h"
#include "Server/Hosting/LocalHosting.h"
#include "Server/ServerLoop.h"

namespace mye {
namespace {

using namespace std::chrono_literals;

// SDK の呼び出しを記録し、コールバックを保持するだけの偽物。コールバックを呼ぶのはテスト側 (別スレッド)
class FakeGameLiftSdk final : public IGameLiftSdk {
public:
    // ---- テストが設定する ----
    bool failInit = false;
    bool failActivate = false;
    std::set<std::string> rejectIds;
    std::function<void()> onEnding; // ProcessEnding を受けた瞬間に走る (その時点の外部状態を観測する)
    // ---- 記録 (メインスレッドの呼び出しだけが書く) ----
    std::vector<std::string> calls;
    GameLiftConnection connection;
    uint16_t readyPort = 0;
    std::vector<std::string> readyLogPaths;
    std::vector<std::string> accepted;
    std::vector<std::string> removed;
    int activateCalls = 0;
    int endingCalls = 0;
    int destroyCalls = 0;

    GameLiftResult InitSdk(const GameLiftConnection& c) override
    {
        calls.push_back("InitSdk");
        connection = c;
        return failInit ? GameLiftResult{ false, "ValidationException: bad auth token" } : GameLiftResult{};
    }
    GameLiftResult ProcessReady(uint16_t port, const std::vector<std::string>& logPaths,
                                const GameLiftCallbacks& callbacks) override
    {
        calls.push_back("ProcessReady");
        readyPort = port;
        readyLogPaths = logPaths;
        std::lock_guard<std::mutex> lock(cbMutex_);
        cb_ = callbacks;
        cbSet_ = true;
        return {};
    }
    GameLiftResult ActivateGameSession() override
    {
        calls.push_back("ActivateGameSession");
        ++activateCalls;
        return failActivate ? GameLiftResult{ false, "InvalidRequestException: not activatable" } : GameLiftResult{};
    }
    GameLiftResult AcceptPlayerSession(const std::string& id) override
    {
        calls.push_back("AcceptPlayerSession");
        if (rejectIds.count(id) != 0) {
            return { false, "NotFoundException: player session" };
        }
        accepted.push_back(id);
        return {};
    }
    GameLiftResult RemovePlayerSession(const std::string& id) override
    {
        calls.push_back("RemovePlayerSession");
        removed.push_back(id);
        return {};
    }
    GameLiftResult ProcessEnding() override
    {
        calls.push_back("ProcessEnding");
        ++endingCalls;
        if (onEnding) {
            onEnding();
        }
        return {};
    }
    GameLiftResult Destroy() override
    {
        calls.push_back("Destroy");
        ++destroyCalls;
        return {};
    }

    // テストのスレッドから呼ぶ (SDK のスレッドの代役)
    bool HasCallbacks()
    {
        std::lock_guard<std::mutex> lock(cbMutex_);
        return cbSet_;
    }
    GameLiftCallbacks Callbacks()
    {
        std::lock_guard<std::mutex> lock(cbMutex_);
        return cb_;
    }
    // コールバックを別スレッドで呼んで join する (SDK の内部スレッドと同じ条件)
    void StartSessionFromThread(const GameLiftGameSession& gs)
    {
        const GameLiftCallbacks c = Callbacks();
        std::thread([c, gs]() { c.onStartGameSession(gs); }).join();
    }
    void TerminateFromThread()
    {
        const GameLiftCallbacks c = Callbacks();
        std::thread([c]() { c.onProcessTerminate(); }).join();
    }
    bool HealthFromThread()
    {
        const GameLiftCallbacks c = Callbacks();
        bool healthy = false;
        std::thread([c, &healthy]() { healthy = c.onHealthCheck(); }).join();
        return healthy;
    }

private:
    std::mutex cbMutex_;
    GameLiftCallbacks cb_;
    bool cbSet_ = false;
};

// 偽 SDK を差し込んだホスティングを作る。sdk は呼び出し側が所有し続ける (記録を読むため)
struct HostingFixture {
    FakeGameLiftSdk* sdk = nullptr;
    std::unique_ptr<GameLiftHosting> hosting;
    std::atomic<uint64_t> nowMs{ 1000 };
};

std::unique_ptr<HostingFixture> MakeFixture(uint32_t laneCount, uint32_t healthStaleMs = 30000)
{
    auto fx = std::make_unique<HostingFixture>();
    auto sdk = std::make_unique<FakeGameLiftSdk>();
    fx->sdk = sdk.get();
    GameLiftHostingOptions opt;
    opt.connection.webSocketUrl = "wss://example.invalid/anywhere";
    opt.connection.authToken = "token";
    opt.connection.fleetId = "fleet-1";
    opt.connection.hostId = "compute-1";
    opt.connection.processId = "proc-1";
    opt.laneCount = laneCount;
    opt.healthStaleMs = healthStaleMs;
    HostingFixture* raw = fx.get();
    opt.nowMs = [raw]() { return raw->nowMs.load(); };
    fx->hosting = std::make_unique<GameLiftHosting>(std::move(sdk), std::move(opt));
    return fx;
}

GameLiftGameSession MakeGameSession(int maxPlayers, std::initializer_list<std::pair<const char*, const char*>> props = {})
{
    GameLiftGameSession gs;
    gs.gameSessionId = "gsess-1";
    gs.maxPlayers = maxPlayers;
    for (const auto& kv : props) {
        gs.properties.emplace_back(kv.first, kv.second);
    }
    return gs;
}

// 起動して ProcessReady まで済ませる
bool ReadyUp(HostingFixture& fx)
{
    return fx.hosting->Init() && fx.hosting->NotifyReady(7777, { L"C:\\logs\\a.rep" });
}

size_t CountCalls(const FakeGameLiftSdk& sdk, const char* name)
{
    size_t n = 0;
    for (const std::string& c : sdk.calls) {
        n += (c == name) ? 1 : 0;
    }
    return n;
}

// calls の中で a が b より前に出ること (どちらも存在する)
bool CallsInOrder(const FakeGameLiftSdk& sdk, const char* a, const char* b)
{
    size_t ia = sdk.calls.size();
    size_t ib = sdk.calls.size();
    for (size_t i = 0; i < sdk.calls.size(); ++i) {
        if (sdk.calls[i] == a && ia == sdk.calls.size()) {
            ia = i;
        }
        if (sdk.calls[i] == b) {
            ib = i;
        }
    }
    return ia < sdk.calls.size() && ib < sdk.calls.size() && ia < ib;
}

// .rep のヘッダの tickCount を読む (ファイルが無い / 短いときは UINT64_MAX)
uint64_t ReadRepHeaderTickCount(const std::wstring& path)
{
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    uint64_t v = ~0ull;
    if (f) {
        f.seekg(static_cast<std::streamoff>(offsetof(MyeReplayHeader, tickCount)));
        f.read(reinterpret_cast<char*>(&v), sizeof(v));
        if (!f) {
            v = ~0ull;
        }
    }
    return v;
}

using CheckFn = std::function<bool(bool, const char*)>;

// ---- G3: SDK のコールバックは Poll (tick 境界) でだけ処理される ----
// V10: 宛先の表 (PeerTable) は、セッションが手放した peer の宛先を回収する。再接続はそのたびに新しい
// エフェメラルポートから来るので、回収が無いと上限 (1024) に達して以後の Hello を受けられなくなる
void TestPeerTable(const CheckFn& check)
{
    const auto addr = [](uint32_t i) {
        NetAddress a;
        a.ipv4 = 0x0100007Fu + (i << 8);
        a.port = static_cast<uint16_t>(5000u + (i % 1000u));
        return a;
    };
    PeerTable table;
    bool allRegistered = true;
    for (uint32_t i = 0; i < PeerTable::kMaxAddrs; ++i) {
        allRegistered = allRegistered && table.KeyOf(addr(i), true) == i + 1;
    }
    check(allRegistered && table.Size() == PeerTable::kMaxAddrs, "V10: the table takes kMaxAddrs addresses with keys 1..N");
    check(table.KeyOf(addr(7), false) == 8, "V10: a known address keeps its key");
    check(table.KeyOf(addr(PeerTable::kMaxAddrs + 5), true) == 0 && table.HellosIgnoredWhileFull() == 1,
          "V10: a full table ignores a new Hello (the ERROR log names it once) and counts it");
    check(table.KeyOf(addr(PeerTable::kMaxAddrs + 6), false) == 0, "V10: an unknown non-Hello sender never registers");
    // 偶数のキーだけ残す = 半分を回収。空いたぶん新しい Hello を受けられ、キーは再利用しない
    const size_t freed = table.Sweep([](uint32_t key) { return key % 2 == 0; });
    NetAddress gone;
    check(freed == PeerTable::kMaxAddrs / 2 && table.Size() == PeerTable::kMaxAddrs / 2
              && !table.AddressOf(1, gone) && table.AddressOf(2, gone) && gone == addr(1),
          "V10: Sweep frees exactly the addresses the session no longer holds");
    const uint32_t fresh = table.KeyOf(addr(PeerTable::kMaxAddrs + 5), true);
    check(fresh == PeerTable::kMaxAddrs + 1, "V10: after the sweep the same Hello is accepted with a never-used key");
    // 再接続の嵐: 毎回新しい宛先から Hello が来て、前の宛先は Gone で回収される。上限を何倍も超えて続けられる
    table.Sweep([](uint32_t) { return false; });
    bool storm = true;
    uint32_t lastKey = fresh;
    for (uint32_t i = 0; i < 5 * PeerTable::kMaxAddrs; ++i) {
        const uint32_t key = table.KeyOf(addr(100000 + i), true);
        storm = storm && key > lastKey;
        lastKey = key;
        table.Sweep([key](uint32_t k) { return k == key; });
        storm = storm && table.Size() == 1;
    }
    check(storm, "V10: 5x the cap of reconnects, each from a new address, are all accepted");
}
void TestGameLiftHosting(const CheckFn& check)
{
    // InitSDK の失敗は 1 行で説明される。SDK の後始末 (ProcessEnding / Destroy) は呼ばない
    {
        auto fx = MakeFixture(4);
        fx->sdk->failInit = true;
        const bool ok = fx->hosting->Init();
        const std::string& err = fx->hosting->LastError();
        check(!ok && err.find("InitSDK failed") != std::string::npos && err.find("bad auth token") != std::string::npos
                  && err.find('\n') == std::string::npos,
              "G3: an InitSDK failure is explained in one line");
        fx->hosting->Shutdown();
        check(fx->sdk->calls.size() == 1 && fx->sdk->calls[0] == "InitSdk",
              "G3: after a failed InitSDK nothing else is called (no ProcessEnding / Destroy)");
    }

    // 通常の流れ: コールバックは別スレッドから来るが、Poll までは何も起きない
    {
        auto fx = MakeFixture(4);
        FakeGameLiftSdk& sdk = *fx->sdk;
        check(ReadyUp(*fx), "G3: Init + ProcessReady succeed");
        check(sdk.connection.fleetId == "fleet-1" && sdk.connection.hostId == "compute-1"
                  && sdk.connection.processId == "proc-1" && sdk.connection.authToken == "token"
                  && sdk.connection.webSocketUrl == "wss://example.invalid/anywhere",
              "G3: the connection info reaches InitSDK unchanged");
        check(sdk.readyPort == 7777 && sdk.readyLogPaths.size() == 1 && sdk.readyLogPaths[0] == "C:\\logs\\a.rep",
              "G3: ProcessReady carries the UDP port and the log paths (UTF-8)");
        std::vector<HostingEvent> ev;
        fx->hosting->Poll(ev);
        check(ev.empty(), "G3: nothing happens before a callback");

        sdk.StartSessionFromThread(MakeGameSession(2, { { kGameLiftPropDeadlineTicks, "5" },
                                                        { kGameLiftPropRejoinTimeoutTicks, "900" } }));
        check(sdk.activateCalls == 0, "G3: OnStartGameSession (SDK thread) only queues - ActivateGameSession is not called yet");
        fx->hosting->Poll(ev);
        check(ev.size() == 1 && ev[0].kind == HostingEventKind::StartSession && ev[0].session.deadlineTicks == 5
                  && ev[0].session.rejoinTimeoutTicks == 900 && sdk.activateCalls == 1,
              "G3: Poll turns it into StartSession (with the game-property overrides) and activates once");

        // 走行中に 2 つ目のセッションが来ても無視する
        sdk.StartSessionFromThread(MakeGameSession(4));
        ev.clear();
        fx->hosting->Poll(ev);
        check(ev.empty() && sdk.activateCalls == 1, "G3: a second game session while one is running is ignored");

        sdk.TerminateFromThread();
        check(sdk.endingCalls == 0, "G3: OnProcessTerminate (SDK thread) only queues - ProcessEnding is not called yet");
        fx->hosting->Poll(ev);
        check(ev.size() == 1 && ev[0].kind == HostingEventKind::Terminate && sdk.endingCalls == 0,
              "G3: Poll turns it into Terminate");
        // 重ねて Terminate が来ても 1 回だけ
        sdk.TerminateFromThread();
        ev.clear();
        fx->hosting->Poll(ev);
        check(ev.empty(), "G3: repeated terminate notices are folded into one");

        fx->hosting->NotifySessionEnded();
        fx->hosting->NotifySessionEnded();
        fx->hosting->Shutdown();
        fx->hosting->Shutdown();
        check(sdk.endingCalls == 1 && sdk.destroyCalls == 1 && CallsInOrder(sdk, "ProcessEnding", "Destroy")
                  && CallsInOrder(sdk, "ActivateGameSession", "ProcessEnding"),
              "G3: ProcessEnding once, then Destroy once, however often they are requested");
    }

    // 人数: GameSession の最大人数とレーン数の小さい方。承認は同期、再送は SDK を呼ばない
    {
        auto fx = MakeFixture(4);
        FakeGameLiftSdk& sdk = *fx->sdk;
        sdk.rejectIds.insert("bad");
        ReadyUp(*fx);
        sdk.StartSessionFromThread(MakeGameSession(2));
        std::vector<HostingEvent> ev;
        fx->hosting->Poll(ev);
        check(fx->hosting->ValidatePlayer("p1") && fx->hosting->ValidatePlayer("p2"),
              "G3: players within the game session's limit are accepted");
        check(!fx->hosting->ValidatePlayer("p3") && sdk.accepted.size() == 2,
              "G3: the 3rd player is rejected without asking GameLift (game session max 2 < 4 lanes)");
        check(fx->hosting->ValidatePlayer("p1") && sdk.accepted.size() == 2,
              "G3: a repeated Hello / reconnection with an accepted ID does not call AcceptPlayerSession again");
        fx->hosting->PlayerLeft("p1");
        check(sdk.removed.empty() && fx->hosting->ValidatePlayer("p1"),
              "G3: a mere disconnect keeps the player session (the lane is reserved for reconnection)");
        fx->hosting->PlayerReleased("p1");
        check(sdk.removed.size() == 1 && sdk.removed[0] == "p1",
              "G3: releasing the reservation calls RemovePlayerSession");
        check(fx->hosting->ValidatePlayer("p3") && sdk.accepted.size() == 3,
              "G3: a released slot can be taken by a new player session");
        check(!fx->hosting->ValidatePlayer("") && !fx->hosting->ValidatePlayer(nullptr),
              "G3: an empty player session ID is rejected");
        fx->hosting->PlayerReleased("never-accepted");
        check(sdk.removed.size() == 1, "G3: releasing an ID that was never accepted does nothing");
        fx->hosting->Shutdown();
    }
    {
        auto fx = MakeFixture(2);
        FakeGameLiftSdk& sdk = *fx->sdk;
        sdk.rejectIds.insert("bad");
        ReadyUp(*fx);
        sdk.StartSessionFromThread(MakeGameSession(8));
        std::vector<HostingEvent> ev;
        fx->hosting->Poll(ev);
        check(!fx->hosting->ValidatePlayer("bad") && !fx->hosting->ValidatePlayer("bad"),
              "G3: when GameLift refuses the player session the Hello is rejected (and asked again next time)");
        check(CountCalls(sdk, "AcceptPlayerSession") == 2, "G3: a refused ID is not cached as accepted");
        check(fx->hosting->ValidatePlayer("a") && fx->hosting->ValidatePlayer("b") && !fx->hosting->ValidatePlayer("c"),
              "G3: a game session larger than the lane count is capped at the lane count (2)");
        fx->hosting->Shutdown();
    }

    // ゲームプロパティの不正値は無視して既定のまま (範囲外 / 数字以外 / 0)
    {
        auto fx = MakeFixture(4);
        ReadyUp(*fx);
        fx->sdk->StartSessionFromThread(MakeGameSession(4, { { kGameLiftPropDeadlineTicks, "abc" },
                                                             { kGameLiftPropRejoinTimeoutTicks, "0" } }));
        std::vector<HostingEvent> ev;
        fx->hosting->Poll(ev);
        check(ev.size() == 1 && ev[0].session.deadlineTicks == 0 && ev[0].session.rejoinTimeoutTicks == 0,
              "G3: invalid game-property overrides are ignored");
        fx->hosting->Shutdown();
    }

    // ActivateGameSession の失敗は Terminate に倒す (使えないセッションを抱えて待たない)
    {
        auto fx = MakeFixture(4);
        fx->sdk->failActivate = true;
        ReadyUp(*fx);
        fx->sdk->StartSessionFromThread(MakeGameSession(4));
        std::vector<HostingEvent> ev;
        fx->hosting->Poll(ev);
        check(ev.size() == 1 && ev[0].kind == HostingEventKind::Terminate,
              "G3: a failed ActivateGameSession becomes Terminate");
        fx->hosting->Shutdown();
        check(fx->sdk->endingCalls == 1, "G3: ... and the process still reports ProcessEnding");
    }

    // ヘルスチェック: メインスレッドを待たず、Poll の最終周回時刻で即答する
    {
        auto fx = MakeFixture(4, /*healthStaleMs=*/1000);
        ReadyUp(*fx);
        check(fx->sdk->HealthFromThread(), "G3: healthy right after the main loop polled");
        fx->nowMs.fetch_add(5000);
        check(!fx->sdk->HealthFromThread(), "G3: unhealthy when the main loop has not polled for longer than the limit");
        std::vector<HostingEvent> ev;
        fx->hosting->Poll(ev);
        check(fx->sdk->HealthFromThread(), "G3: healthy again once the main loop polls");
        fx->hosting->Shutdown();
    }

    // セッション開始前の Terminate でも ProcessEnding を欠かさない
    {
        auto fx = MakeFixture(4);
        ReadyUp(*fx);
        fx->sdk->TerminateFromThread();
        std::vector<HostingEvent> ev;
        fx->hosting->Poll(ev);
        const bool gotTerminate = ev.size() == 1 && ev[0].kind == HostingEventKind::Terminate;
        fx->hosting->Shutdown();
        check(gotTerminate && fx->sdk->endingCalls == 1 && fx->sdk->destroyCalls == 1,
              "G3: a Terminate before any session still ends with ProcessEnding + Destroy");
    }
}

// ---- 共通の幕引き関数 (InterpretHostingEvents / CloseSession) ----
void TestSessionFunnel(const CheckFn& check, const std::filesystem::path& tempDir)
{
    {
        std::vector<HostingEvent> events(3);
        events[0].kind = HostingEventKind::HealthCheck;
        events[1].kind = HostingEventKind::StartSession;
        events[1].session.deadlineTicks = 7;
        events[2].kind = HostingEventKind::Terminate;
        HostingDecision d;
        InterpretHostingEvents(events, d);
        check(d.startSession && d.terminate && d.session.deadlineTicks == 7 && d.session.rejoinTimeoutTicks == 0,
              "R4: StartSession and Terminate in one Poll are both kept (a terminate is not dropped)");
    }

    // CloseSession: .rep を閉じてから、ホスティングへ終了を知らせる (順序まで見る)
    const std::wstring repPath = (tempDir / L"mye_server_selftest_close.rep").wstring();
    auto fx = MakeFixture(2);
    ReadyUp(*fx);
    uint64_t tickCountSeenAtEnding = ~0ull;
    fx->sdk->onEnding = [&]() { tickCountSeenAtEnding = ReadRepHeaderTickCount(repPath); };
    ReplayRecorder rec;
    SessionConfig cfg = {};
    cfg.role = static_cast<uint32_t>(SessionRole::Server);
    cfg.playerCount = 2;
    rec.Start(repPath, 1, 2, 3, 2, nullptr, 0, cfg, SimProvenance{}, SnapshotMeta{}, kServerReplayFlushTicks);
    InputSnapshot in[kMaxPlayers] = {};
    for (uint32_t t = 0; t < 25; ++t) {
        rec.RecordTick(in, 2, 0xC000 + t, nullptr);
    }
    check(ReadRepHeaderTickCount(repPath) == 0, "R4: while recording, the header still says tickCount 0");
    const bool closed = CloseSession(rec, repPath, *fx->hosting);
    check(closed && !rec.IsActive() && fx->sdk->endingCalls == 1 && tickCountSeenAtEnding == 25,
          "R4: CloseSession finishes the .rep (tickCount written back) before ProcessEnding is sent");
    ReplayPlayer p;
    check(p.Load(repPath) && p.TickCount() == 25 && !p.RecoveredUnfinished(), "R4: the closed .rep loads with all 25 ticks");
    fx->hosting->Shutdown();
    std::error_code ec;
    std::filesystem::remove(repPath, ec);
}

// ---- 実ループ: SDK のスレッドから Terminate が来たら、.rep を閉じて ProcessEnding を送って終わる ----
void TestLoopTerminate(const CheckFn& check, const std::filesystem::path& tempDir)
{
    const std::wstring repPath = (tempDir / L"mye_server_selftest_loop.rep").wstring();
    std::error_code ec;
    std::filesystem::remove(repPath, ec);

    HeadlessSimSetup setup;
    setup.config.title = L"MyEngine Server (selftest)";
    setup.config.localPlayers = 2;
    setup.systemInput = true;
    setup.scene.showcase = PickShowcase(nullptr, FindShowcase(L"--local-demo", /*editor=*/true));
    HeadlessSim sim;
    if (!check(sim.Init(setup), "G3: the headless sim starts for the loop test (run from the repository root)")) {
        return;
    }

    auto fx = MakeFixture(2);
    FakeGameLiftSdk& sdk = *fx->sdk;
    uint64_t tickCountSeenAtEnding = ~0ull;
    sdk.onEnding = [&]() { tickCountSeenAtEnding = ReadRepHeaderTickCount(repPath); };
    if (!check(fx->hosting->Init(), "G3: hosting initialises")) {
        return;
    }

    ServerLoopConfig lc;
    lc.port = 0; // 空きポート
    lc.session = DefaultServerSessionConfig(2, static_cast<uint32_t>(setup.config.netInputDelay));
    FillSessionConfigFromProject(lc.session, setup.config);
    lc.replayRecordPath = repPath;
    lc.timeoutSec = 60; // 保険
    lc.statsIntervalSec = 0;

    // SDK のスレッドの代役: ProcessReady を待ち、セッション開始 → しばらく後に Terminate
    std::thread sdkThread([&]() {
        for (int i = 0; i < 4000 && !sdk.HasCallbacks(); ++i) {
            std::this_thread::sleep_for(5ms);
        }
        const GameLiftCallbacks cb = sdk.Callbacks();
        if (!cb.onStartGameSession) {
            return;
        }
        std::this_thread::sleep_for(150ms);
        cb.onStartGameSession(MakeGameSession(2, { { kGameLiftPropDeadlineTicks, "4" } }));
        std::this_thread::sleep_for(1000ms);
        cb.onProcessTerminate();
    });
    const int rc = RunServerLoop(sim, *fx->hosting, lc);
    sdkThread.join();

    check(rc == kServerExitOk, "G3: the loop exits normally on Terminate");
    check(sdk.activateCalls == 1 && CallsInOrder(sdk, "ProcessReady", "ActivateGameSession"),
          "G3: the game session was activated after ProcessReady");
    check(sdk.endingCalls == 1 && tickCountSeenAtEnding != ~0ull && tickCountSeenAtEnding > 10,
          "R4: at the moment ProcessEnding was sent the .rep was already closed (header tickCount > 10)");
    check(sdk.readyLogPaths.size() == 1
              && std::filesystem::path(sdk.readyLogPaths[0]) == std::filesystem::absolute(repPath),
          "G3: the .rep path is offered to GameLift as a log to collect");
    ReplayPlayer p;
    if (check(p.Load(repPath), "R4: the .rep written by the loop loads")) {
        check(!p.RecoveredUnfinished() && p.TickCount() > 10 && p.PlayerCount() == 2 && p.HasSystemInput()
                  && p.Header().session.deadlineTicks == 4,
              "R4: it is a closed recording (not recovered), with the game-property deadline in its SessionConfig");
    }
    fx->hosting->Shutdown();
    check(sdk.destroyCalls == 1 && sdk.endingCalls == 1, "G3: Shutdown destroys the SDK once and does not repeat ProcessEnding");
    std::filesystem::remove(repPath, ec);
}

// ---- 実プロセス: Ctrl+Break (LocalHosting のコンソールハンドラ) で .rep が閉じて exit 0 ----
void TestRealConsoleTerminate(const CheckFn& check, const std::filesystem::path& tempDir)
{
    DWORD dummyPid = 0;
    if (GetConsoleProcessList(&dummyPid, 1) == 0) {
        MYE_LOG_WARN("  SKIP: no console is attached, so Ctrl+Break cannot be delivered to a child process");
        return;
    }
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    const std::wstring repPath = (tempDir / L"mye_server_selftest_ctrl.rep").wstring();
    const std::wstring logPath = (tempDir / L"mye_server_selftest_ctrl.log").wstring();
    std::error_code ec;
    std::filesystem::remove(repPath, ec);

    // ポートは PID から散らす (並走する検証とぶつかりにくくする)
    const unsigned port = 40000 + (GetCurrentProcessId() % 20000);
    std::wstring cmd = L"\"" + std::wstring(exePath) + L"\" --local-demo --max-players 2 --port " + std::to_wstring(port)
        + L" --server-timeout 90 --replay-record \"" + repPath + L"\"";

    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE logHandle = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = logHandle;
    si.hStdError = logHandle;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi = {};
    // 新しいプロセスグループ = Ctrl+Break だけをこの子に届けられる (Ctrl+C は子で無効になるため Break を使う)。
    // どちらも SetConsoleCtrlHandler のハンドラへ同じ経路で入る
    const BOOL created = CreateProcessW(exePath, cmd.data(), nullptr, nullptr, TRUE, CREATE_NEW_PROCESS_GROUP, nullptr,
                                        nullptr, &si, &pi);
    if (logHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(logHandle);
    }
    if (!check(created != FALSE, "R4: a real Server.exe child process starts")) {
        return;
    }

    // 子が tick を回し始めた (.rep が育ち始めた) のを見てから Ctrl+Break
    uint64_t lastSize = 0;
    bool growing = false;
    for (int i = 0; i < 400 && !growing; ++i) { // 最大 40 秒
        std::this_thread::sleep_for(100ms);
        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
            break;
        }
        std::error_code sec;
        const uint64_t size = std::filesystem::exists(repPath, sec) ? std::filesystem::file_size(repPath, sec) : 0;
        growing = size > 0 && lastSize > 0 && size > lastSize;
        lastSize = size;
    }
    bool sent = false;
    if (growing) {
        sent = GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pi.dwProcessId) != FALSE;
    }
    DWORD exitCode = 9999;
    const DWORD wait = WaitForSingleObject(pi.hProcess, 30000);
    if (wait == WAIT_OBJECT_0) {
        GetExitCodeProcess(pi.hProcess, &exitCode);
    } else {
        TerminateProcess(pi.hProcess, 99);
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    check(growing && sent, "R4: the child server was running (the .rep grew) and Ctrl+Break was delivered");
    check(wait == WAIT_OBJECT_0 && exitCode == 0, "R4: the server exits with code 0 after Ctrl+Break");
    ReplayPlayer p;
    if (check(p.Load(repPath), "R4: the .rep left by the Ctrl+Break'd server loads")) {
        check(!p.RecoveredUnfinished() && p.TickCount() > 0,
              "R4: it was closed properly (header tickCount written back, not recovered from the file length)");
    }
    std::filesystem::remove(repPath, ec);
    std::filesystem::remove(logPath, ec);
}

} // namespace

bool RunServerSelfTest()
{
    MYE_LOG_INFO("==== Server (GameLift hosting / .rep closing) self test ====");
    int failCount = 0;
    const CheckFn check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
        return cond;
    };
    std::error_code ec;
    const std::filesystem::path tempDir = std::filesystem::temp_directory_path(ec);

    TestPeerTable(check);
    TestGameLiftHosting(check);
    TestSessionFunnel(check, tempDir);
    TestLoopTerminate(check, tempDir);
    TestRealConsoleTerminate(check, tempDir);
    // Server.exe でも Recast 系のハッシュが Editor と一致することを確かめる (M82a)
    check(RunNavDeterminismSelfTest(), "NavMesh: Recast のビット一致と状態の復元");
    check(RunNavSurfaceSelfTest(), "NavMesh: Surface のベイク・.mnav・読み込み");
    check(RunNavAgentSelfTest(), "NavMesh: Agent・dtCrowd・SimSnapshot の Nav 節");

    if (failCount == 0) {
        MYE_LOG_INFO("Server self test: ALL PASS");
    } else {
        MYE_LOG_ERROR("Server self test: %d FAILED", failCount);
    }
    return failCount == 0;
}

} // namespace mye
