//====================================================================================
//                          OnlineSession.cpp
//  MyEngin/ 秋田蓮音                                                     10/14/2026
//                                          ロビーとゲームのセッションをつなぐ (ホストのサーバ起動・P2P 中継)
//====================================================================================
#include "Engine/Engine/Online/OnlineSession.h"

#include <algorithm>
#include <cstring>

#include <Windows.h>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Util/Hash.h"

namespace mye {

namespace {

// 参加コードの字。読み間違えやすい I / O / 0 / 1 を除いた 32 字
constexpr char kJoinCodeAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";

// 断片の頭 3 バイト: [seq][index][count]。count = 1 なら分割なし
constexpr size_t kFragHeader = 3;
constexpr size_t kFragPayload = kOnlineMaxPacket - kFragHeader;

// ホストの Server.exe の起動を待つ時間。シーンの読み込み (ヘッドレス) が終わるまで Hello に返事が無い
constexpr uint32_t kHostConnectTimeoutMs = 30000;
constexpr uint32_t kJoinConnectTimeoutMs = 20000;

NetAddress LoopbackAddr(uint16_t port)
{
    NetAddress a;
    a.ipv4 = 0x0100007Fu; // 127.0.0.1 (ネットワークバイト順で 7F 00 00 01)
    a.port = static_cast<uint16_t>((port >> 8) | (port << 8));
    return a;
}

} // namespace

// ホストが参加者 1 人ごとに持つ中継。サーバにはこのソケットのポートが「その参加者の宛先」に見える
struct OnlineSession::HostPeer {
    std::string id;
    UdpSocket sock;
    uint8_t seq = 0;
    Reassembler reasm;
};

OnlineSession::OnlineSession() = default;

OnlineSession::~OnlineSession()
{
    ResetTransport();
}

void OnlineSession::Attach(IOnlineService* service, const HostServerLaunch& launch, const OnlineSettings& settings)
{
    service_ = service;
    launch_ = launch;
    settings_ = settings;
    pttKeyName_[0] = settings.pushToTalkKey;
    pttKeyName_[1] = '\0';
    if (service_ != nullptr) {
        // 押している間だけ送るなら、押すまでは送らない。ボイス自体が無効なら常に送らない
        service_->SetVoiceSending(settings.voiceEnabled && !settings.pushToTalk);
    }
}

bool OnlineSession::TakeLeaveRequest()
{
    const bool r = leaveRequested_;
    leaveRequested_ = false;
    return r;
}

void OnlineSession::UpdatePushToTalk(bool keyDown)
{
    if (service_ == nullptr || !settings_.voiceEnabled || !settings_.pushToTalk || keyDown == pttDown_) {
        return;
    }
    pttDown_ = keyDown;
    service_->SetVoiceSending(keyDown);
}

std::string OnlineSession::MakeJoinCode(uint64_t entropy)
{
    std::string code;
    uint64_t h = HashBytes(&entropy, sizeof(entropy));
    for (size_t i = 0; i < kOnlineJoinCodeChars; ++i) {
        code.push_back(kJoinCodeAlphabet[h & 31u]);
        h >>= 5;
    }
    return code;
}

void OnlineSession::Fragment(const uint8_t* data, size_t size, uint8_t seq, std::vector<std::vector<uint8_t>>& out)
{
    out.clear();
    const size_t count = size == 0 ? 1 : (size + kFragPayload - 1) / kFragPayload;
    if (count > kMaxFragments) {
        MYE_LOG_ERROR("[online] a game packet of %zu bytes is too large to relay (max %zu)", size,
                      kMaxFragments * kFragPayload);
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        const size_t begin = i * kFragPayload;
        const size_t len = (std::min)(kFragPayload, size - begin);
        std::vector<uint8_t> frag(kFragHeader + len);
        frag[0] = seq;
        frag[1] = static_cast<uint8_t>(i);
        frag[2] = static_cast<uint8_t>(count);
        if (len > 0) {
            std::memcpy(frag.data() + kFragHeader, data + begin, len);
        }
        out.push_back(std::move(frag));
    }
}

bool OnlineSession::Reassembler::Push(const uint8_t* frag, size_t size, std::vector<uint8_t>& whole)
{
    if (size < kFragHeader) {
        return false;
    }
    const uint8_t s = frag[0];
    const uint8_t index = frag[1];
    const uint8_t n = frag[2];
    if (n == 0 || n > kMaxFragments || index >= n) {
        return false;
    }
    if (n == 1) {
        whole.assign(frag + kFragHeader, frag + size);
        return true;
    }
    // 組み立て中は 1 パケット分だけ持つ。別の seq が来たら古い方は欠けたものとして捨てる
    if (count == 0 || s != seq || n != count) {
        seq = s;
        count = n;
        got = 0;
    }
    parts[index].assign(frag + kFragHeader, frag + size);
    got = static_cast<uint8_t>(got | (1u << index));
    if (got != static_cast<uint8_t>((1u << count) - 1u)) {
        return false;
    }
    whole.clear();
    for (uint8_t i = 0; i < count; ++i) {
        whole.insert(whole.end(), parts[i].begin(), parts[i].end());
    }
    count = 0;
    got = 0;
    return true;
}

void OnlineSession::Fail(const std::string& reason)
{
    phase_ = OnlinePhase::Error;
    message_ = reason;
    MYE_LOG_WARN("[online] %s", reason.c_str());
}

void OnlineSession::ResetTransport()
{
    server_.Stop();
    serverAddr_ = NetAddress{};
    peers_.clear();
    bridge_.Close();
    clientAddr_ = NetAddress{};
    fromHost_.reset();
    hasRequest_ = false;
}

void OnlineSession::Host(const std::string& codeOverride)
{
    if (service_ == nullptr || phase_ != OnlinePhase::Offline || service_->State() != OnlineState::Ready) {
        return;
    }
    LARGE_INTEGER qpc = {};
    QueryPerformanceCounter(&qpc);
    const uint64_t entropy = static_cast<uint64_t>(qpc.QuadPart) ^ (static_cast<uint64_t>(GetCurrentProcessId()) << 32);
    const std::string code = codeOverride.empty() ? MakeJoinCode(entropy) : codeOverride;
    isHost_ = true;
    message_.clear();
    phase_ = OnlinePhase::CreatingLobby;
    service_->CreateLobby(code.c_str(), kOnlineMaxMembers);
    MYE_LOG_INFO("[online] creating a lobby (join code %s)", code.c_str());
}

void OnlineSession::Join(const char* joinCode)
{
    if (service_ == nullptr || phase_ != OnlinePhase::Offline || service_->State() != OnlineState::Ready) {
        return;
    }
    isHost_ = false;
    message_.clear();
    phase_ = OnlinePhase::JoiningLobby;
    service_->JoinLobbyByCode(joinCode);
    MYE_LOG_INFO("[online] joining the lobby %s", joinCode);
}

void OnlineSession::Leave()
{
    if (service_ != nullptr) {
        service_->LeaveLobby();
    }
    ResetTransport();
    phase_ = OnlinePhase::Offline;
    message_.clear();
}

void OnlineSession::NotifySessionEnded(const char* reason, bool worldEnded)
{
    worldEnded_ = worldEnded_ || worldEnded;
    if (phase_ != OnlinePhase::InSession) {
        return;
    }
    if (service_ != nullptr) {
        service_->LeaveLobby();
    }
    ResetTransport();
    Fail(std::string("the game session ended: ") + reason);
}

bool OnlineSession::TakeConnectRequest(OnlineConnectRequest& out)
{
    if (!hasRequest_) {
        return false;
    }
    out = request_;
    hasRequest_ = false;
    return true;
}

void OnlineSession::SendToPeer(const char* peerId, const uint8_t* data, size_t size, uint8_t& seq)
{
    std::vector<std::vector<uint8_t>> frags;
    Fragment(data, size, seq++, frags);
    for (const std::vector<uint8_t>& f : frags) {
        service_->SendPacket(peerId, f.data(), f.size());
    }
}

void OnlineSession::Update()
{
    if (service_ == nullptr) {
        return;
    }
    const OnlineLobbyView& lobby = service_->Lobby();
    switch (phase_) {
    case OnlinePhase::CreatingLobby:
        if (lobby.state == LobbyState::Failed) {
            const std::string why = service_->LastError();
            service_->LeaveLobby();
            Fail("could not create the lobby: " + why);
        } else if (lobby.state == LobbyState::InLobby && lobby.isOwner) {
            HostServerLaunch launch = launch_;
            launch.port = FindFreeLoopbackPort();
            if (launch.port == 0 || !server_.Start(launch)) {
                service_->LeaveLobby();
                Fail("could not start the host server (see the log)");
                return;
            }
            serverAddr_ = LoopbackAddr(launch.port);
            request_.target = L"127.0.0.1:" + std::to_wstring(launch.port);
            request_.playerSessionId = service_->LocalId();
            request_.connectTimeoutMs = kHostConnectTimeoutMs;
            hasRequest_ = true;
            phase_ = OnlinePhase::InSession;
            MYE_LOG_INFO("[online] hosting lobby %s: the server is on 127.0.0.1:%u", lobby.joinCode,
                         static_cast<unsigned>(launch.port));
        }
        return;
    case OnlinePhase::JoiningLobby:
        if (lobby.state == LobbyState::Failed) {
            const std::string why = service_->LastError();
            service_->LeaveLobby();
            Fail("could not join the lobby: " + why);
        } else if (lobby.state == LobbyState::InLobby) {
            if (!bridge_.Open(0, true)) {
                service_->LeaveLobby();
                Fail("could not open the local relay socket");
                return;
            }
            fromHost_ = std::make_unique<Reassembler>();
            request_.target = L"127.0.0.1:" + std::to_wstring(bridge_.LocalPort());
            request_.playerSessionId = service_->LocalId();
            request_.connectTimeoutMs = kJoinConnectTimeoutMs;
            hasRequest_ = true;
            phase_ = OnlinePhase::InSession;
            MYE_LOG_INFO("[online] joined lobby %s (host %s): relaying through 127.0.0.1:%u", lobby.joinCode,
                         lobby.ownerId, static_cast<unsigned>(bridge_.LocalPort()));
        }
        return;
    case OnlinePhase::InSession:
        if (isHost_) {
            PumpHost();
        } else {
            PumpJoin();
        }
        return;
    case OnlinePhase::Offline:
    case OnlinePhase::Error:
        return;
    }
}

void OnlineSession::PumpHost()
{
    uint32_t exitCode = 0;
    if (server_.Exited(exitCode)) {
        service_->LeaveLobby();
        ResetTransport();
        Fail("the host server stopped (exit code " + std::to_string(exitCode) + ")");
        return;
    }
    const OnlineLobbyView& lobby = service_->Lobby();
    // 参加者 → サーバ。参加者ごとのソケットから送るので、サーバには別々の宛先に見える
    char peerId[kOnlineIdChars];
    size_t size = 0;
    std::vector<uint8_t> whole;
    while (service_->ReceivePacket(peerId, buf_.data(), buf_.size(), size)) {
        HostPeer* peer = nullptr;
        for (const std::unique_ptr<HostPeer>& p : peers_) {
            if (p->id == peerId) {
                peer = p.get();
            }
        }
        if (peer == nullptr) {
            auto created = std::make_unique<HostPeer>();
            created->id = peerId;
            if (!created->sock.Open(0, true)) {
                MYE_LOG_ERROR("[online] could not open a relay socket for %s", peerId);
                continue;
            }
            MYE_LOG_INFO("[online] relaying %s through 127.0.0.1:%u", peerId,
                         static_cast<unsigned>(created->sock.LocalPort()));
            peer = created.get();
            peers_.push_back(std::move(created));
        }
        if (peer->reasm.Push(buf_.data(), size, whole)) {
            peer->sock.Send(serverAddr_, whole.data(), whole.size());
        }
    }
    // サーバ → 参加者
    for (const std::unique_ptr<HostPeer>& p : peers_) {
        for (int i = 0; i < 512; ++i) {
            NetAddress from;
            const int n = p->sock.Recv(buf_.data(), buf_.size(), from);
            if (n <= 0) {
                break;
            }
            if (from == serverAddr_) {
                SendToPeer(p->id.c_str(), buf_.data(), static_cast<size_t>(n), p->seq);
            }
        }
    }
    // ロビーを抜けた参加者の中継を畳む。サーバ側はタイムアウトでレーンを予約にし、同じ ID で戻れば Rejoin になる
    for (size_t i = 0; i < peers_.size();) {
        bool stillMember = false;
        for (uint32_t k = 0; k < lobby.memberCount; ++k) {
            stillMember = stillMember || peers_[i]->id == lobby.members[k].id;
        }
        if (stillMember) {
            ++i;
        } else {
            MYE_LOG_INFO("[online] %s left the lobby: its relay is closed", peers_[i]->id.c_str());
            peers_.erase(peers_.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
}

void OnlineSession::PumpJoin()
{
    const OnlineLobbyView& lobby = service_->Lobby();
    if (lobby.state != LobbyState::InLobby) {
        const std::string why = service_->LastError();
        ResetTransport();
        Fail("left the lobby: " + why);
        return;
    }
    // 自分のクライアント → ホスト
    for (int i = 0; i < 512; ++i) {
        NetAddress from;
        const int n = bridge_.Recv(buf_.data(), buf_.size(), from);
        if (n <= 0) {
            break;
        }
        clientAddr_ = from; // 自分の ClientSession のソケット (1 つだけ)
        SendToPeer(lobby.ownerId, buf_.data(), static_cast<size_t>(n), toHostSeq_);
    }
    // ホスト → 自分のクライアント
    char peerId[kOnlineIdChars];
    size_t size = 0;
    std::vector<uint8_t> whole;
    while (service_->ReceivePacket(peerId, buf_.data(), buf_.size(), size)) {
        if (std::strcmp(peerId, lobby.ownerId) != 0 || !clientAddr_.Valid()) {
            continue;
        }
        if (fromHost_->Push(buf_.data(), size, whole)) {
            bridge_.Send(clientAddr_, whole.data(), whole.size());
        }
    }
}

} // namespace mye
