//====================================================================================
//                          OnlineSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                     10/14/2026
//                                          オンライン層 (参加コード・中継の分割・local 実装) の自己テスト
//====================================================================================
#include "Engine/Engine/Online/OnlineSelfTest.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include <Windows.h>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Engine/Online/LocalOnlineService.h"
#include "Engine/Engine/Online/OnlineSession.h"

namespace mye {

namespace {

std::vector<uint8_t> Pattern(size_t size, uint8_t salt)
{
    std::vector<uint8_t> v(size);
    for (size_t i = 0; i < size; ++i) {
        v[i] = static_cast<uint8_t>(i * 31u + salt);
    }
    return v;
}

// 分割して全断片を順に押し込み、組み上がったものを返す。組み上がらなければ空 + ok = false
std::vector<uint8_t> RoundTrip(const std::vector<uint8_t>& in, uint8_t seq, bool& ok, size_t& fragCount,
                               size_t& maxFrag)
{
    std::vector<std::vector<uint8_t>> frags;
    OnlineSession::Fragment(in.data(), in.size(), seq, frags);
    fragCount = frags.size();
    maxFrag = 0;
    OnlineSession::Reassembler r;
    std::vector<uint8_t> whole;
    ok = false;
    for (size_t i = 0; i < frags.size(); ++i) {
        maxFrag = (std::max)(maxFrag, frags[i].size());
        const bool done = r.Push(frags[i].data(), frags[i].size(), whole);
        if (done) {
            ok = (i + 1 == frags.size()); // 最後の断片でだけ組み上がること
        }
    }
    return ok ? whole : std::vector<uint8_t>();
}

} // namespace

bool RunOnlineSelfTest()
{
    MYE_LOG_INFO("==== Online (join code / relay fragments / local backend) self test ====");
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

    // ---- 参加コード ----
    {
        const std::string a = OnlineSession::MakeJoinCode(1);
        const std::string b = OnlineSession::MakeJoinCode(2);
        bool alphabetOk = a.size() == kOnlineJoinCodeChars && b.size() == kOnlineJoinCodeChars;
        for (const char c : a + b) {
            alphabetOk = alphabetOk && std::strchr("ABCDEFGHJKLMNPQRSTUVWXYZ23456789", c) != nullptr;
        }
        check(alphabetOk, "join code: 6 chars from the 32-char alphabet (no I / O / 0 / 1)");
        check(a == OnlineSession::MakeJoinCode(1) && a != b, "join code: same entropy -> same code, different -> different");
    }

    // ---- 中継の分割と組み立て ----
    {
        const size_t payload = kOnlineMaxPacket - 3; // 断片 1 つに載る量 (頭 3 バイト)
        const size_t sizes[] = { 0, 1, payload, payload + 1, 3000, payload * OnlineSession::kMaxFragments };
        const size_t expectFrags[] = { 1, 1, 1, 2, 3, OnlineSession::kMaxFragments };
        bool allOk = true;
        for (size_t i = 0; i < std::size(sizes); ++i) {
            const std::vector<uint8_t> in = Pattern(sizes[i], static_cast<uint8_t>(i));
            bool ok = false;
            size_t frags = 0;
            size_t maxFrag = 0;
            const std::vector<uint8_t> out = RoundTrip(in, static_cast<uint8_t>(i), ok, frags, maxFrag);
            const bool thisOk = ok && out == in && frags == expectFrags[i] && maxFrag <= kOnlineMaxPacket;
            if (!thisOk) {
                MYE_LOG_ERROR("    size %zu: ok=%d frags=%zu (expected %zu) maxFrag=%zu", sizes[i], ok ? 1 : 0, frags,
                              expectFrags[i], maxFrag);
            }
            allOk = allOk && thisOk;
        }
        check(allOk, "fragments: 0 B .. 4 fragments round-trip, every fragment fits one P2P packet");

        std::vector<std::vector<uint8_t>> tooBig;
        OnlineSession::Fragment(Pattern(payload * OnlineSession::kMaxFragments + 1, 9).data(),
                                payload * OnlineSession::kMaxFragments + 1, 0, tooBig);
        check(tooBig.empty(), "fragments: a packet larger than 4 fragments is refused (logged), not truncated");

        // 断片が欠けたパケットは捨て、次のパケットは組み上がる。分割なしのパケットは組み立て中を乱さない
        const std::vector<uint8_t> first = Pattern(2500, 1);
        const std::vector<uint8_t> second = Pattern(2500, 2);
        const std::vector<uint8_t> small = Pattern(10, 3);
        std::vector<std::vector<uint8_t>> f1;
        std::vector<std::vector<uint8_t>> f2;
        std::vector<std::vector<uint8_t>> fs;
        OnlineSession::Fragment(first.data(), first.size(), 7, f1);
        OnlineSession::Fragment(second.data(), second.size(), 8, f2);
        OnlineSession::Fragment(small.data(), small.size(), 9, fs);
        OnlineSession::Reassembler r;
        std::vector<uint8_t> whole;
        bool lostOk = !r.Push(f1[0].data(), f1[0].size(), whole); // first の 1 つ目だけ届き、残りは欠落
        lostOk = lostOk && !r.Push(f2[0].data(), f2[0].size(), whole);
        lostOk = lostOk && r.Push(fs[0].data(), fs[0].size(), whole) && whole == small; // 割り込み
        lostOk = lostOk && !r.Push(f2[1].data(), f2[1].size(), whole);
        lostOk = lostOk && r.Push(f2[2].data(), f2[2].size(), whole) && whole == second;
        check(lostOk, "fragments: a packet with a lost fragment is dropped; an unsplit packet does not disturb assembly");
    }

    // ---- local 実装のロビー (1 プロセス内のループバック UDP) ----
    {
        // 並行実行される selftest 同士でロビーのポートがぶつからないよう、コードはプロセス ID から作る
        const std::string code = OnlineSession::MakeJoinCode(0x5E1F7E57ull ^ GetCurrentProcessId());
        OnlineSettings settings;
        settings.displayName = "host";
        LocalOnlineService host;
        LocalOnlineService guest;
        bool ok = host.Init(settings);
        settings.displayName = "guest";
        ok = guest.Init(settings) && ok;
        check(ok && host.State() == OnlineState::Ready && guest.State() == OnlineState::Ready,
              "local: both instances log in");
        host.CreateLobby(code.c_str(), kOnlineMaxMembers);
        guest.JoinLobbyByCode(code.c_str());
        // done は 1 周に 1 回だけ評価する (受信のように、評価すると状態が進む条件を渡すため)
        const auto pump = [&](auto&& done) {
            for (int i = 0; i < 200; ++i) { // 最大 2 秒
                host.Tick();
                guest.Tick();
                if (done()) {
                    return true;
                }
                Sleep(10);
            }
            return false;
        };
        const bool joined = pump([&] {
            return guest.Lobby().state == LobbyState::InLobby && host.Lobby().memberCount == 2;
        });
        check(joined && host.Lobby().isOwner && !guest.Lobby().isOwner
                  && std::strcmp(guest.Lobby().ownerId, host.LocalId()) == 0 && guest.Lobby().memberCount == 2,
              "local: the guest joins by code, both see 2 members and the same owner");

        const char ping[] = "ping";
        const char pong[] = "pong";
        guest.SendPacket(guest.Lobby().ownerId, ping, sizeof(ping));
        char from[kOnlineIdChars] = {};
        char buf[64] = {};
        size_t size = 0;
        const bool gotPing = pump([&] { return host.ReceivePacket(from, buf, sizeof(buf), size); });
        const bool pingOk = gotPing && size == sizeof(ping) && std::strcmp(buf, ping) == 0
            && std::strcmp(from, guest.LocalId()) == 0;
        host.SendPacket(guest.LocalId(), pong, sizeof(pong));
        const bool gotPong = pump([&] { return guest.ReceivePacket(from, buf, sizeof(buf), size); });
        check(pingOk && gotPong && std::strcmp(buf, pong) == 0 && std::strcmp(from, host.LocalId()) == 0,
              "local: P2P data flows both ways and carries the sender id");

        guest.LeaveLobby();
        const bool left = pump([&] { return host.Lobby().memberCount == 1; });
        check(left && guest.Lobby().state == LobbyState::None, "local: leaving shrinks the host's member list");

        LocalOnlineService late;
        settings.displayName = "late";
        late.Init(settings);
        host.LeaveLobby(); // ホストが閉じたロビーには入れない
        late.JoinLobbyByCode(code.c_str());
        for (int i = 0; i < 600 && late.Lobby().state == LobbyState::Joining; ++i) { // タイムアウト 5 秒
            late.Tick();
            Sleep(10);
        }
        check(late.Lobby().state == LobbyState::Failed && late.LastError()[0] != '\0',
              "local: joining a closed lobby fails with a reason after the timeout");
        host.Shutdown();
        guest.Shutdown();
        late.Shutdown();
    }

    MYE_LOG_INFO("==== Online self test: %s (%d failure(s)) ====", failCount == 0 ? "PASS" : "FAIL", failCount);
    return failCount == 0;
}

} // namespace mye
