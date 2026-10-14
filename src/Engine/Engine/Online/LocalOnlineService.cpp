//====================================================================================
//                          LocalOnlineService.cpp
//  MyEngin/ 秋田蓮音                                                     10/14/2026
//                                          同じ PC の別プロセス同士を繋ぐ、オンラインの開発用の代役
//====================================================================================
#include "Engine/Engine/Online/LocalOnlineService.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include <Windows.h>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Util/Hash.h"

namespace mye {

namespace {

// ロビーのポートの範囲。参加コードのハッシュでこの中の 1 つを選ぶ (衝突したら作成が失敗する)
constexpr uint16_t kLobbyPortBase = 47100;
constexpr uint16_t kLobbyPortRange = 800;
constexpr uint64_t kJoinRetryMs = 250;
constexpr uint64_t kJoinTimeoutMs = 5000;

// 制御フレームの種類 (先頭 1 バイト)
enum Kind : uint8_t {
    kJoinRequest = 1, // 参加側 → ホスト: 表示名
    kMemberList = 2,  // ホスト → 全員: メンバー表
    kLeave = 3,       // 参加側 → ホスト
    kData = 4,        // P2P のゲームパケット
    kFull = 5,        // ホスト → 参加側: 満員
    kClosed = 6,      // ホスト → 全員: ロビーを閉じた
};

constexpr size_t kMemberRecord = kOnlineIdChars + kOnlineNameChars;

uint64_t NowMs()
{
    return GetTickCount64();
}

void CopyText(char* dst, size_t cap, const std::string& src)
{
    const size_t n = (std::min)(src.size(), cap - 1);
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

// NetAddress はネットワークバイト順で持つ (UdpSocket.h)。winsock を引き込まずに詰め替える
uint16_t SwapPort(uint16_t v)
{
    return static_cast<uint16_t>((v >> 8) | (v << 8));
}

NetAddress Loopback(uint16_t port)
{
    NetAddress a;
    a.ipv4 = 0x0100007Fu; // 127.0.0.1 (メモリ上のバイト列が 7F 00 00 01)
    a.port = SwapPort(port);
    return a;
}

} // namespace

uint16_t LocalOnlineService::LobbyPortForCode(const char* joinCode)
{
    const uint64_t h = HashBytes(joinCode, std::strlen(joinCode));
    return static_cast<uint16_t>(kLobbyPortBase + (h % kLobbyPortRange));
}

bool LocalOnlineService::Init(const OnlineSettings& settings)
{
    displayName_ = settings.displayName;
    if (displayName_.empty()) {
        const char* user = std::getenv("USERNAME");
        displayName_ = user != nullptr ? user : "player";
    }
    OpenSocket(0);
    if (!socket_.IsOpen()) {
        Fail("could not open a loopback UDP socket");
        state_ = OnlineState::Failed;
        return false;
    }
    state_ = OnlineState::Ready;
    MYE_LOG_INFO("[online] local backend ready as %s (\"%s\")", localId_.c_str(), displayName_.c_str());
    return true;
}

void LocalOnlineService::OpenSocket(uint16_t port)
{
    socket_.Close();
    if (!socket_.Open(port, true)) {
        localId_.clear();
        return;
    }
    localId_ = "local-" + std::to_string(socket_.LocalPort());
}

void LocalOnlineService::Shutdown()
{
    LeaveLobby();
    socket_.Close();
    state_ = OnlineState::Disabled;
}

std::string LocalOnlineService::IdOf(const NetAddress& a) const
{
    return "local-" + std::to_string(SwapPort(a.port));
}

bool LocalOnlineService::AddressOfId(const char* id, NetAddress& out) const
{
    if (std::strncmp(id, "local-", 6) != 0) {
        return false;
    }
    const int port = std::atoi(id + 6);
    if (port <= 0 || port > 65535) {
        return false;
    }
    out = Loopback(static_cast<uint16_t>(port));
    return true;
}

void LocalOnlineService::Fail(const std::string& reason)
{
    lastError_ = reason;
    MYE_LOG_WARN("[online] %s", reason.c_str());
}

void LocalOnlineService::ResetLobby()
{
    lobby_ = OnlineLobbyView{};
    hostAddr_ = NetAddress{};
    inbox_.clear();
}

void LocalOnlineService::CreateLobby(const char* joinCode, uint32_t maxMembers)
{
    if (state_ != OnlineState::Ready || lobby_.state != LobbyState::None) {
        return;
    }
    lastError_.clear();
    // ホストはロビーのポートに付け替える (参加側はコードからこのポートを引く)
    const uint16_t port = LobbyPortForCode(joinCode);
    OpenSocket(port);
    if (!socket_.IsOpen()) {
        OpenSocket(0); // ログイン状態は保つ
        lobby_.state = LobbyState::Failed;
        Fail("the lobby port for this join code is in use (try another code)");
        return;
    }
    maxMembers_ = (std::min)(maxMembers, kOnlineMaxMembers);
    lobby_.state = LobbyState::InLobby;
    lobby_.isOwner = true;
    CopyText(lobby_.joinCode, sizeof(lobby_.joinCode), joinCode);
    CopyText(lobby_.ownerId, sizeof(lobby_.ownerId), localId_);
    lobby_.memberCount = 1;
    OnlineMember& self = lobby_.members[0];
    self = OnlineMember{};
    CopyText(self.id, sizeof(self.id), localId_);
    CopyText(self.name, sizeof(self.name), displayName_);
    self.isOwner = true;
    self.isLocal = true;
    MYE_LOG_INFO("[online] local lobby %s created on 127.0.0.1:%u", joinCode, static_cast<unsigned>(port));
}

void LocalOnlineService::JoinLobbyByCode(const char* joinCode)
{
    if (state_ != OnlineState::Ready || lobby_.state != LobbyState::None) {
        return;
    }
    lastError_.clear();
    hostAddr_ = Loopback(LobbyPortForCode(joinCode));
    CopyText(lobby_.joinCode, sizeof(lobby_.joinCode), joinCode);
    lobby_.state = LobbyState::Joining;
    joinStartMs_ = NowMs();
    lastJoinSendMs_ = 0;
}

void LocalOnlineService::LeaveLobby()
{
    if (lobby_.state == LobbyState::InLobby) {
        if (lobby_.isOwner) {
            for (uint32_t i = 0; i < lobby_.memberCount; ++i) {
                NetAddress a;
                if (!lobby_.members[i].isLocal && AddressOfId(lobby_.members[i].id, a)) {
                    SendControl(a, { kClosed });
                }
            }
            OpenSocket(0); // ロビーのポートを空ける
        } else {
            SendControl(hostAddr_, { kLeave });
        }
    }
    ResetLobby();
}

void LocalOnlineService::SendControl(const NetAddress& to, const std::vector<uint8_t>& frame)
{
    socket_.Send(to, frame.data(), frame.size());
}

void LocalOnlineService::BroadcastMembers()
{
    std::vector<uint8_t> frame(2 + lobby_.memberCount * kMemberRecord, 0);
    frame[0] = kMemberList;
    frame[1] = static_cast<uint8_t>(lobby_.memberCount);
    for (uint32_t i = 0; i < lobby_.memberCount; ++i) {
        uint8_t* rec = frame.data() + 2 + i * kMemberRecord;
        std::memcpy(rec, lobby_.members[i].id, kOnlineIdChars);
        std::memcpy(rec + kOnlineIdChars, lobby_.members[i].name, kOnlineNameChars);
    }
    for (uint32_t i = 0; i < lobby_.memberCount; ++i) {
        NetAddress a;
        if (!lobby_.members[i].isLocal && AddressOfId(lobby_.members[i].id, a)) {
            SendControl(a, frame);
        }
    }
}

void LocalOnlineService::HandleControl(const NetAddress& from, const uint8_t* data, size_t size)
{
    const std::string fromId = IdOf(from);
    switch (data[0]) {
    case kJoinRequest: {
        if (!lobby_.isOwner || lobby_.state != LobbyState::InLobby) {
            return;
        }
        for (uint32_t i = 0; i < lobby_.memberCount; ++i) {
            if (fromId == lobby_.members[i].id) {
                BroadcastMembers(); // 再送された参加要求: 表をもう一度送るだけ
                return;
            }
        }
        if (lobby_.memberCount >= maxMembers_) {
            SendControl(from, { kFull });
            return;
        }
        OnlineMember& m = lobby_.members[lobby_.memberCount++];
        m = OnlineMember{};
        CopyText(m.id, sizeof(m.id), fromId);
        CopyText(m.name, sizeof(m.name),
                 std::string(reinterpret_cast<const char*>(data + 1), size - 1).c_str());
        MYE_LOG_INFO("[online] %s (\"%s\") joined the local lobby", m.id, m.name);
        BroadcastMembers();
        return;
    }
    case kLeave: {
        if (!lobby_.isOwner) {
            return;
        }
        for (uint32_t i = 0; i < lobby_.memberCount; ++i) {
            if (fromId == lobby_.members[i].id) {
                for (uint32_t j = i + 1; j < lobby_.memberCount; ++j) {
                    lobby_.members[j - 1] = lobby_.members[j];
                }
                --lobby_.memberCount;
                MYE_LOG_INFO("[online] %s left the local lobby", fromId.c_str());
                BroadcastMembers();
                return;
            }
        }
        return;
    }
    case kMemberList: {
        if (lobby_.isOwner || from != hostAddr_ || size < 2) {
            return;
        }
        const uint32_t count = (std::min)(static_cast<uint32_t>(data[1]), kOnlineMaxMembers);
        if (size < 2 + count * kMemberRecord) {
            return;
        }
        const OnlineLobbyView old = lobby_;
        lobby_.memberCount = count;
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t* rec = data + 2 + i * kMemberRecord;
            OnlineMember& m = lobby_.members[i];
            m = OnlineMember{};
            std::memcpy(m.id, rec, kOnlineIdChars - 1);
            std::memcpy(m.name, rec + kOnlineIdChars, kOnlineNameChars - 1);
            m.isOwner = (i == 0);
            m.isLocal = (localId_ == m.id);
            for (uint32_t k = 0; k < old.memberCount; ++k) {
                if (std::strcmp(old.members[k].id, m.id) == 0) {
                    m.mutedByMe = old.members[k].mutedByMe;
                }
            }
        }
        CopyText(lobby_.ownerId, sizeof(lobby_.ownerId), lobby_.members[0].id);
        if (lobby_.state == LobbyState::Joining) {
            lobby_.state = LobbyState::InLobby;
            MYE_LOG_INFO("[online] joined local lobby %s (owner %s)", lobby_.joinCode, lobby_.ownerId);
        }
        return;
    }
    case kFull:
        if (lobby_.state == LobbyState::Joining && from == hostAddr_) {
            ResetLobby();
            lobby_.state = LobbyState::Failed;
            Fail("the lobby is full");
        }
        return;
    case kClosed:
        if (!lobby_.isOwner && from == hostAddr_) {
            ResetLobby();
            lobby_.state = LobbyState::Failed;
            Fail("the host closed the lobby");
        }
        return;
    default:
        return;
    }
}

void LocalOnlineService::Tick()
{
    if (!socket_.IsOpen()) {
        return;
    }
    if (lobby_.state == LobbyState::Joining) {
        const uint64_t now = NowMs();
        if (now - joinStartMs_ > kJoinTimeoutMs) {
            ResetLobby();
            lobby_.state = LobbyState::Failed;
            Fail("no lobby answered for that join code");
        } else if (now - lastJoinSendMs_ >= kJoinRetryMs) {
            lastJoinSendMs_ = now;
            std::vector<uint8_t> frame(1 + displayName_.size());
            frame[0] = kJoinRequest;
            std::memcpy(frame.data() + 1, displayName_.data(), displayName_.size());
            SendControl(hostAddr_, frame);
        }
    }
    for (int i = 0; i < 1024; ++i) {
        NetAddress from;
        const int n = socket_.Recv(recvBuf_.data(), recvBuf_.size(), from);
        if (n <= 0) {
            break;
        }
        const uint8_t* data = recvBuf_.data();
        if (data[0] != kData) {
            HandleControl(from, data, static_cast<size_t>(n));
            continue;
        }
        if (lobby_.state != LobbyState::InLobby) {
            continue;
        }
        // P2P はロビーのメンバーからだけ受ける (EOS の AcceptConnection と同じ線引き)
        const std::string id = IdOf(from);
        bool member = false;
        for (uint32_t k = 0; k < lobby_.memberCount; ++k) {
            member = member || id == lobby_.members[k].id;
        }
        if (!member) {
            continue;
        }
        inbox_.push_back(Incoming{ id, std::vector<uint8_t>(data + 1, data + n) });
    }
}

bool LocalOnlineService::SendPacket(const char* peerId, const void* data, size_t size)
{
    NetAddress to;
    if (lobby_.state != LobbyState::InLobby || size > kOnlineMaxPacket || !AddressOfId(peerId, to)) {
        return false;
    }
    std::vector<uint8_t> frame(1 + size);
    frame[0] = kData;
    std::memcpy(frame.data() + 1, data, size);
    return socket_.Send(to, frame.data(), frame.size());
}

bool LocalOnlineService::ReceivePacket(char (&outPeerId)[kOnlineIdChars], void* buf, size_t capacity,
                                       size_t& outSize)
{
    if (inbox_.empty()) {
        return false;
    }
    Incoming& in = inbox_.front();
    CopyText(outPeerId, kOnlineIdChars, in.from);
    outSize = (std::min)(in.bytes.size(), capacity);
    std::memcpy(buf, in.bytes.data(), outSize);
    inbox_.pop_front();
    return true;
}

void LocalOnlineService::SetMemberMuted(const char* memberId, bool muted)
{
    for (uint32_t i = 0; i < lobby_.memberCount; ++i) {
        if (std::strcmp(lobby_.members[i].id, memberId) == 0) {
            lobby_.members[i].mutedByMe = muted;
        }
    }
}

} // namespace mye
