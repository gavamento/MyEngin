#include "Engine/Engine/Net/NetSelfTest.h"

#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include "Engine/Core/Log.h"
#include "Engine/Engine/Net/NetSession.h"

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
    check(sizeof(NetPacketHeader) == 48, "packet header is 48 bytes");
    check(sizeof(NetHandshakePayload) == 48, "handshake payload is 48 bytes");
    check(sizeof(InputSnapshot) == 64, "input snapshot is 64 bytes");
    check(kNetMaxPacket == 48 + 8 * 64, "max packet = header + 8 inputs");

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

    if (failCount == 0) {
        MYE_LOG_INFO("==== Net session self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== Net session self test: %d FAILED ====", failCount);
    return false;
}

} // namespace mye
