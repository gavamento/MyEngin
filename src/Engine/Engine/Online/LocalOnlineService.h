//====================================================================================
//                          LocalOnlineService.h
//  MyEngin/ 秋田蓮音                                                     10/14/2026
//                                          同じ PC の別プロセス同士を繋ぐ、オンラインの開発用の代役
//====================================================================================
#pragma once
#include <deque>
#include <string>
#include <vector>

#include "Engine/Engine/Online/IOnlineService.h"
#include "Engine/Platform/Net/UdpSocket.h"

namespace mye {

// EOS を使わずに「ロビーを作る → 参加コードで入る → P2P でゲームのパケットを運ぶ」の流れを
// 実プロセスで確かめるための実装 (online.backend = "local")。製品には使わない。
//   ・ロビーの場所は参加コードのハッシュから決まる 127.0.0.1 のポート (ホストがそこへ bind する)
//   ・ユーザー ID は "local-<自分のポート>"。P2P の宛先はその番号から引く
//   ・ボイスは無い (送信の on / off とミュートの状態だけ持つ)
// ★プロセス間の生存確認はしない。ホストが落ちたことはゲームのセッション (ClientSession のタイムアウト) が知らせる
class LocalOnlineService final : public IOnlineService {
public:
    bool Init(const OnlineSettings& settings) override;
    void Tick() override;
    void Shutdown() override;

    OnlineState State() const override { return state_; }
    const char* LocalId() const override { return localId_.c_str(); }
    const char* LastError() const override { return lastError_.c_str(); }
    const char* BackendName() const override { return "local"; }

    void CreateLobby(const char* joinCode, uint32_t maxMembers) override;
    void JoinLobbyByCode(const char* joinCode) override;
    void LeaveLobby() override;
    const OnlineLobbyView& Lobby() const override { return lobby_; }

    bool SendPacket(const char* peerId, const void* data, size_t size) override;
    bool ReceivePacket(char (&outPeerId)[kOnlineIdChars], void* buf, size_t capacity, size_t& outSize) override;

    bool VoiceAvailable() const override { return false; }
    void SetVoiceSending(bool enabled) override { voiceSending_ = enabled; }
    bool VoiceSending() const override { return voiceSending_; }
    void SetMemberMuted(const char* memberId, bool muted) override;

    // 参加コード → ロビーのポート (selftest 用に公開)
    static uint16_t LobbyPortForCode(const char* joinCode);

private:
    struct Incoming {
        std::string from;
        std::vector<uint8_t> bytes;
    };
    void OpenSocket(uint16_t port);
    void HandleControl(const NetAddress& from, const uint8_t* data, size_t size);
    void BroadcastMembers();
    void SendControl(const NetAddress& to, const std::vector<uint8_t>& frame);
    void Fail(const std::string& reason);
    void ResetLobby();
    bool AddressOfId(const char* id, NetAddress& out) const;
    std::string IdOf(const NetAddress& a) const;

    OnlineState state_ = OnlineState::Disabled;
    OnlineLobbyView lobby_ = {};
    UdpSocket socket_;
    std::string localId_;
    std::string displayName_;
    std::string lastError_;
    NetAddress hostAddr_ = {};   // 参加側: ロビー (ホスト) の宛先
    uint32_t maxMembers_ = kOnlineMaxMembers;
    uint64_t joinStartMs_ = 0;
    uint64_t lastJoinSendMs_ = 0;
    std::deque<Incoming> inbox_;
    std::vector<uint8_t> recvBuf_ = std::vector<uint8_t>(2048);
    bool voiceSending_ = true;
};

} // namespace mye
