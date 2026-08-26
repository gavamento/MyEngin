#include "Engine/Engine/Net/NetSelfTest.h"

#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include "Engine/Core/Components.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/Net/NetRollback.h"
#include "Engine/Engine/Net/NetSession.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene.h"

namespace mye {
namespace {

NetIdentity MakeIdentity()
{
    NetIdentity id;
    id.apiVersion = 12;
    id.repVersion = 4;
    id.snapshotVersion = 3;
    id.playerCount = 2;
    id.inputDelay = 3;
    id.configBits = kNetCfgSynthInput | kNetCfgJobs;
    id.startWorldHash = 0xABCDEF0123456789ull;
    return id;
}

NetConfig MakeConfig(NetRole role, uint32_t loss)
{
    NetConfig c;
    c.role = role;
    c.port = 0; // ★ホストも任意ポートで開く (CI でのポート衝突を原理的に無くす)
    c.playerCount = 2;
    c.inputDelay = 3;
    c.lossPercent = loss;
    c.connectTimeoutMs = 8000;
    c.stallTimeoutMs = 8000;
    return c;
}

// 2 つのセッションを 1 プロセス内で協調させて進める。
// pred が真になるまで両方の Poll を回す (真になるのは通常 1 周目)
template <typename Pred>
bool DriveUntil(NetSession& a, NetSession& b, Pred pred)
{
    for (int i = 0; i < 40000; ++i) { // 上限 = 約 8 秒 (200us x 40000)
        a.Poll();
        b.Poll();
        if (pred()) {
            return true;
        }
        if (a.State() == NetState::Failed || b.State() == NetState::Failed) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return false;
}

// ループバックで繋ぐ。ホストを先に開いて、実際に割り当たったポートへ参加させる
bool Connect(NetSession& host, NetSession& join, uint32_t loss, const NetIdentity& hostId,
             const NetIdentity& joinId)
{
    if (!host.Start(MakeConfig(NetRole::Host, loss), hostId, 0)) {
        return false;
    }
    NetConfig jc = MakeConfig(NetRole::Join, loss);
    jc.joinTarget = L"127.0.0.1:" + std::to_wstring(host.LocalPort());
    if (!join.Start(jc, joinId, 0)) {
        return false;
    }
    // 参加側の Join 送出は WaitUntilReady が回す。pump でホスト側の受信を進める
    return join.WaitUntilReady([&host] {
        host.Poll();
        return true;
    });
}

// delay tick 分の先出し (ticks 0..delay-1)。EngineLoop の priming と同じ規則:
// 「tick T のレーン値は T だけで決まる」ので両側が同じ値を作る
void Prime(NetSession& s, uint32_t delay)
{
    for (uint32_t t = 0; t < delay; ++t) {
        s.SubmitLocalInput(t, SynthLaneInput(t, s.LocalPlayerIndex()));
    }
}

} // namespace

bool RunNetSelfTest()
{
    MYE_LOG_INFO("==== Net session (UDP lockstep) self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
        return cond;
    };

    // ---- 1. 線上のレイアウト ----
    // proto v2 (M52i) でヘッダへ確定 (tick, hash) の 16 バイトが増えた
    check(sizeof(NetPacketHeader) == 64, "packet header is 64 bytes");
    check(sizeof(NetHandshakePayload) == 48, "handshake payload is 48 bytes");
    check(sizeof(InputSnapshot) == 64, "input snapshot is 64 bytes");
    check(kNetMaxPacket == 64 + 8 * 64, "max packet = header + 8 inputs");
    check(kNetProtoVersion == 2, "protocol version is 2 (M52i hash piggyback)");

    // ---- 2. 指紋の照合はフィールドごとに理由を返す ----
    {
        const NetIdentity base = MakeIdentity();
        check(CompareNetIdentity(base, base) == NetReject::None, "identical identities match");
        struct Case {
            NetReject want;
            NetIdentity id;
        };
        NetIdentity a = base; a.proto += 1;
        NetIdentity b = base; b.apiVersion += 1;
        NetIdentity c = base; c.repVersion += 1;
        NetIdentity d = base; d.snapshotVersion += 1;
        NetIdentity e = base; e.playerCount += 1;
        NetIdentity f = base; f.inputDelay += 1;
        NetIdentity g = base; g.configBits ^= kNetCfgSynthInput;
        NetIdentity h = base; h.startWorldHash ^= 1ull;
        const Case cases[] = {
            { NetReject::Proto, a },           { NetReject::ApiVersion, b },
            { NetReject::RepVersion, c },      { NetReject::SnapshotVersion, d },
            { NetReject::PlayerCount, e },     { NetReject::InputDelay, f },
            { NetReject::ConfigBits, g },      { NetReject::WorldHash, h },
        };
        bool ok = true;
        for (const Case& cs : cases) {
            ok = ok && CompareNetIdentity(base, cs.id) == cs.want;
        }
        check(ok, "each identity field reports its own reject reason");
    }

    // ---- 3. ループバックのハンドシェイク ----
    {
        NetSession host;
        NetSession join;
        const NetIdentity id = MakeIdentity();
        const bool ok = Connect(host, join, 0, id, id);
        check(ok, "loopback handshake completes");
        check(host.Running() && join.Running(), "both sides are running");
        check(host.LocalPlayerIndex() == 0 && join.LocalPlayerIndex() == 1,
              "host takes lane 0 and the joiner takes lane 1");
        host.Close();
        join.Close();
    }

    // ---- 4. 指紋が違えば接続しない ----
    {
        NetSession host;
        NetSession join;
        NetIdentity hostId = MakeIdentity();
        NetIdentity joinId = MakeIdentity();
        joinId.startWorldHash ^= 0x1000ull; // 別のシーンから始めた想定
        const bool ok = Connect(host, join, 0, hostId, joinId);
        check(!ok, "a mismatched peer is refused");
        check(join.State() == NetState::Failed, "the joiner ends up in the failed state");
        check(join.RejectReason() == NetReject::WorldHash,
              "the reject reason names the starting world hash");
        check(!host.Running(), "the host stays unconnected");
        host.Close();
        join.Close();
    }

    // ---- 5-6. 入力交換 (ロス無し / ロス 30%) ----
    // 見たいのは「両側が tick ごとに**同じ kMaxPlayers 本**を持つこと」。
    // これが .rep のバイト一致の必要条件そのもの (net_verify.bat が実プロセスで見る)。
    // ロス有りは冗長送信 + キープアライブが効いているかの試験でもある
    struct Trial {
        uint32_t loss;
        uint32_t ticks;
        const char* label;
    };
    const Trial trials[] = { { 0, 120, "input exchange matches on both sides (no loss)" },
                             { 30, 120, "input exchange matches on both sides (30% loss)" } };
    for (const Trial& tr : trials) {
        NetSession host;
        NetSession join;
        const NetIdentity id = MakeIdentity();
        if (!Connect(host, join, tr.loss, id, id)) {
            check(false, tr.label);
            continue;
        }
        const uint32_t delay = host.InputDelay();
        Prime(host, delay);
        Prime(join, delay);
        bool ok = true;
        bool zeroLanes = true;
        for (uint32_t t = 0; t < tr.ticks && ok; ++t) {
            const uint64_t tick = t;
            ok = DriveUntil(host, join,
                            [&] { return host.HasInputs(tick) && join.HasInputs(tick); });
            if (!ok) {
                MYE_LOG_ERROR("  [net selftest] stalled at tick %u (loss %u%%)", t, tr.loss);
                break;
            }
            const InputSnapshot* ha = host.InputsFor(tick);
            const InputSnapshot* ja = join.InputsFor(tick);
            ok = std::memcmp(ha, ja, sizeof(InputSnapshot) * kMaxPlayers) == 0;
            // レーン 0/1 は合成入力そのもの、2 以降は恒常ゼロ
            const InputSnapshot zero = {};
            for (uint32_t p = 0; p < kMaxPlayers; ++p) {
                const InputSnapshot want = (p < 2) ? SynthLaneInput(tick, p) : zero;
                if (std::memcmp(&ha[p], &want, sizeof(InputSnapshot)) != 0) {
                    zeroLanes = false;
                }
            }
            host.SubmitLocalInput(tick + delay, SynthLaneInput(tick + delay, 0));
            join.SubmitLocalInput(tick + delay, SynthLaneInput(tick + delay, 1));
            host.OnTickConsumed(tick);
            join.OnTickConsumed(tick);
        }
        check(ok, tr.label);
        check(zeroLanes, tr.loss == 0
                  ? "lane values are the sender's synth input; unused lanes stay zero (no loss)"
                  : "lane values are the sender's synth input; unused lanes stay zero (30% loss)");
        if (tr.loss > 0) {
            check(host.PacketsDropped() + join.PacketsDropped() > 0,
                  "--net-loss actually dropped packets");
        }
        host.Finish();
        join.Finish();
        host.Close();
        join.Close();
    }

    // ---- 7. 予測値の取り出し (M52i) ----
    // 「未着 tick の予測 = 直近の確定値の繰り返し」がリング越しに成立するか。
    // ★ここは**実際に受信したレーン**で見る (自分で書き込んだ値では、リングの
    //   stamp 管理が壊れていても素通りしてしまう)
    {
        NetSession host;
        NetSession join;
        const NetIdentity id = MakeIdentity();
        if (!Connect(host, join, 0, id, id)) {
            check(false, "prediction: loopback session");
        } else {
            const uint32_t delay = host.InputDelay();
            Prime(host, delay);
            Prime(join, delay);
            // host のレーン 0 を tick 0..9 まで確定させ、join 側へ届くのを待つ
            for (uint32_t t = delay; t < 10; ++t) {
                host.SubmitLocalInput(t, SynthLaneInput(t, 0));
            }
            const bool got = DriveUntil(host, join, [&] { return join.LaneInput(9, 0) != nullptr; });
            check(got, "prediction: the peer lane arrives through the ring");
            check(join.LaneInput(10, 0) == nullptr,
                  "prediction: a tick the peer has not sent yet is absent");
            const InputSnapshot* guess = join.PredictLane(10, 0);
            const InputSnapshot want = SynthLaneInput(9, 0);
            check(guess != nullptr && std::memcmp(guess, &want, sizeof(InputSnapshot)) == 0,
                  "prediction: an unknown tick repeats the newest confirmed value");
            check(join.PredictLane(0, 0) == nullptr,
                  "prediction: there is nothing to repeat before the first tick");

            // ---- 8. 確定ハッシュのピギーバック (desync 検出の土台) ----
            host.SetLocalConfirmed(7, 0xFEEDFACE12345678ull);
            host.SubmitLocalInput(10, SynthLaneInput(10, 0)); // 次のパケットに載る
            uint64_t pt = 0;
            uint64_t ph = 0;
            const bool carried = DriveUntil(host, join, [&] { return join.PeerConfirmed(pt, ph); });
            check(carried && pt == 7 && ph == 0xFEEDFACE12345678ull,
                  "desync: the confirmed (tick, hash) rides along with the input packets");
            uint64_t bt = 0;
            uint64_t bh = 0;
            check(!host.PeerConfirmed(bt, bh),
                  "desync: a peer that never confirmed anything reports nothing");
        }
        host.Close();
        join.Close();
    }

    // ---- 9. ロールバックのリング (M52i) ----
    // **本物のワールドを撮って戻す**ところまで見る。器だけのテストにすると
    // 「撮れているのに戻していない」型の非対称が丸ごと素通りする (M52d と同じ理由)
    {
        Scene scene;
        World& w = scene.GetWorld();
        GameObject a = scene.CreateGameObjectTracked("Alpha");
        w.ApplyStructuralChanges();
        uint64_t tickIndex = 100;
        InputSnapshot prevLanes[kMaxPlayers] = {};
        SimRefs refs;
        refs.scene = &scene;
        refs.prevTickInput = prevLanes;
        refs.tickIndex = &tickIndex;

        NetRollback rb;
        check(rb.Begin(refs, 100), "rollback: the ring starts with a snapshot");
        check(rb.Active() && rb.ConfirmedTick() == 100,
              "rollback: the confirmed frontier starts at the first tick");
        check(rb.SnapshotBefore(100) != nullptr, "rollback: the state before tick 100 is kept");
        check(rb.SnapshotBefore(101) == nullptr, "rollback: nothing is kept for a future tick");
        const uint64_t hash100 = HashWorld(w, {nullptr, &scene.Time(), &scene.Persist()});

        // tick 100 を「予測入力で走った」ことにして世界を動かす
        InputSnapshot lanes[kMaxPlayers] = {};
        lanes[0] = SynthLaneInput(100, 0);
        lanes[1] = SynthLaneInput(100, 1);
        if (auto* t = w.GetComponent<LocalTransform>(a.Id())) {
            t->position.x = 5.0f;
        }
        tickIndex = 101;
        const uint64_t hash101 = HashWorld(w, {nullptr, &scene.Time(), &scene.Persist()});
        rb.OnTickEnd(refs, 100, lanes, 2, hash101, true, true);
        const NetSpecTick* e = rb.Entry(100);
        check(e != nullptr && e->predicted && e->hashAfter == hash101 && e->simulated,
              "rollback: the speculative tick is recorded with its hash");
        check(rb.SnapshotBefore(101) != nullptr, "rollback: the state before the next tick is kept");
        check(rb.InputsMatch(100, lanes, 2), "rollback: matching inputs compare equal");
        InputSnapshot other[kMaxPlayers] = {};
        other[0] = lanes[0];
        other[1] = SynthLaneInput(999, 1);
        check(!rb.InputsMatch(100, other, 2), "rollback: a different peer lane compares unequal");
        rb.MarkConfirmed(100);
        check(rb.Entry(100) != nullptr && !rb.Entry(100)->predicted,
              "rollback: a correct prediction clears the speculative flag");

        // 巻き戻す: tick 100 が走る前へ戻すと世界がビット同値へ返る
        const std::vector<std::byte>* blob = rb.SnapshotBefore(100);
        check(blob != nullptr && RestoreSimSnapshot(refs, blob->data(), blob->size()),
              "rollback: the snapshot restores");
        check(tickIndex == 100, "rollback: restoring rewinds the tick index");
        check(HashWorld(w, {nullptr, &scene.Time(), &scene.Persist()}) == hash100,
              "rollback: the restored world hashes to the pre-tick state");

        // 確定ハッシュのリング
        rb.NoteCommitted(100, hash101);
        uint64_t out = 0;
        check(rb.CommittedHash(100, out) && out == hash101,
              "rollback: the committed hash comes back");
        check(!rb.CommittedHash(101, out), "rollback: an uncommitted tick has no hash");
        rb.NoteCommitted(102, 0);
        check(!rb.CommittedHash(102, out), "rollback: hash 0 keeps meaning no value");

        // リングの追い出し: 投機上限より深く進めたら古い tick は消える
        tickIndex = 100;
        for (uint64_t t = 100; t < 100 + kNetSpecRing + 2; ++t) {
            tickIndex = t + 1;
            rb.OnTickEnd(refs, t, lanes, 2, 0x1234ull + t, false, true);
        }
        check(rb.Entry(100) == nullptr && rb.SnapshotBefore(100) == nullptr,
              "rollback: entries older than the ring are gone");
        check(rb.Entry(100 + kNetSpecRing + 1) != nullptr,
              "rollback: the newest entry is still there");
        check(rb.SnapshotBytes() > 0, "rollback: the ring accounts for its snapshot bytes");
    }

    if (failCount == 0) {
        MYE_LOG_INFO("==== Net session self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== Net session self test: %d FAILED ====", failCount);
    return false;
}

} // namespace mye
