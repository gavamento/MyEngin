#include "Engine/Engine/Net/NetSession.h"

#include <chrono>
#include <cstring>
#include <thread>

#include "Engine/Core/Log.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {
namespace {

constexpr uint64_t kNoTick = ~0ull;
constexpr uint32_t kJoinRetryMs = 100;

// セッション内経過 ms の基準。steady_clock の起点 (= 起動時刻) からの ms を 32bit へ
// 落として使う。**これは実時間だが sim レーンには 1 バイトも届かない** —
// 用途は「いつ送るか」「いつ諦めるか」だけで、tick が消費する入力には影響しない
uint32_t MonoMs()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

// セッション ID の種。**乱数規則 (規則 8) の対象外** — ここで欲しいのは決定論ではなく
// 「前回実行の残党パケットと区別できること」で、値そのものは .rep にもハッシュにも入らない。
// 同一ホストで 2 プロセスがほぼ同時に立つので、粗い時計 1 本だと衝突しうる。
// 単調時計 + 壁時計 + ローカルポートの 3 本を混ぜる
uint64_t MakeSessionId(uint16_t localPort)
{
    uint64_t v = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    v ^= static_cast<uint64_t>(std::chrono::system_clock::now().time_since_epoch().count()) << 17;
    v ^= static_cast<uint64_t>(localPort) << 43;
    v *= 0x9E3779B97F4A7C15ull; // 下位ビットの偏りをならす
    v ^= v >> 29;
    return v == 0 ? 1ull : v; // 0 は「未確定」の予約値
}

} // namespace

const char* NetRejectName(NetReject r)
{
    switch (r) {
    case NetReject::None: return "none";
    case NetReject::Proto: return "protocol version";
    case NetReject::ApiVersion: return "MYE_API_VERSION";
    case NetReject::RepVersion: return "replay file version";
    case NetReject::SnapshotVersion: return "sim snapshot version";
    case NetReject::PlayerCount: return "player count";
    case NetReject::InputDelay: return "input delay";
    case NetReject::ConfigBits: return "launch options (--synth-input / --no-jobs / caches)";
    case NetReject::WorldHash: return "starting world hash (different scene or build)";
    case NetReject::Busy: return "host is already connected to another peer";
    }
    return "?";
}

NetReject CompareNetIdentity(const NetIdentity& a, const NetIdentity& b)
{
    if (a.proto != b.proto) return NetReject::Proto;
    if (a.apiVersion != b.apiVersion) return NetReject::ApiVersion;
    if (a.repVersion != b.repVersion) return NetReject::RepVersion;
    if (a.snapshotVersion != b.snapshotVersion) return NetReject::SnapshotVersion;
    if (a.playerCount != b.playerCount) return NetReject::PlayerCount;
    if (a.inputDelay != b.inputDelay) return NetReject::InputDelay;
    if (a.configBits != b.configBits) return NetReject::ConfigBits;
    if (a.startWorldHash != b.startWorldHash) return NetReject::WorldHash;
    return NetReject::None;
}

NetSession::~NetSession()
{
    Close();
}

uint32_t NetSession::NowMs() const
{
    return MonoMs() - startMs_;
}

bool NetSession::Start(const NetConfig& cfg, const NetIdentity& localId, uint64_t startTick)
{
    Close();
    cfg_ = cfg;
    localId_ = localId;
    startTick_ = startTick;
    nextNeededTick_ = startTick;
    lastSubmitted_ = 0;
    hasSubmitted_ = false;
    peerSaidBye_ = false;
    reject_ = NetReject::None;
    startMs_ = MonoMs();
    packetsSent_ = packetsRecv_ = packetsDropped_ = stallCount_ = 0;
    stallMs_ = 0.0;
    pingMs_ = 0.0f;

    if (cfg_.playerCount == 0 || cfg_.playerCount > kMaxPlayers) {
        MYE_LOG_ERROR("[net] player count %u is out of range (1..%u)", cfg_.playerCount,
                      kMaxPlayers);
        state_ = NetState::Failed;
        return false;
    }
    ring_.assign(static_cast<size_t>(kNetRingTicks) * kMaxPlayers, InputSnapshot{});
    stamp_.assign(static_cast<size_t>(kNetRingTicks) * kMaxPlayers, kNoTick);

    if (cfg_.role == NetRole::Join) {
        if (!ResolveEndpoint(cfg_.joinTarget, peer_)) {
            state_ = NetState::Failed;
            return false;
        }
    }
    // ホストは指定ポート、参加側は任意ポート (返信は recvfrom の送信元へ返す)
    const uint16_t bindPort = (cfg_.role == NetRole::Host) ? cfg_.port : 0;
    if (!socket_.Open(bindPort)) {
        state_ = NetState::Failed;
        return false;
    }
    // ロス注入の乱数。**sim の RNG とは完全に別のストリーム**で、ワールドハッシュにも
    // .rep にも影響しない (捨てるのは「いつ届くか」だけ)
    lossRng_.Seed(static_cast<uint64_t>(socket_.LocalPort()) * 0x9E3779B9ull + 1,
                  static_cast<uint64_t>(cfg_.role));

    if (cfg_.role == NetRole::Host) {
        sessionId_ = MakeSessionId(socket_.LocalPort());
        localIndex_ = 0;
        MYE_LOG_INFO("[net] hosting on UDP port %u (players %u, input delay %u ticks, loss %u%%)",
                     static_cast<unsigned>(socket_.LocalPort()), cfg_.playerCount,
                     cfg_.inputDelay, cfg_.lossPercent);
    } else {
        sessionId_ = 0; // Accept で確定する
        localIndex_ = 0;
        MYE_LOG_INFO("[net] joining %s from local port %u (input delay %u ticks, loss %u%%)",
                     NetAddressToString(peer_).c_str(),
                     static_cast<unsigned>(socket_.LocalPort()), cfg_.inputDelay,
                     cfg_.lossPercent);
    }
    lastRecvMs_ = NowMs();
    lastSendMs_ = NowMs();
    lastJoinMs_ = 0;
    state_ = NetState::Connecting;
    return true;
}

void NetSession::Close()
{
    socket_.Close();
    if (state_ == NetState::Running || state_ == NetState::Connecting) {
        state_ = NetState::Closed;
    }
}

void NetSession::SendTo(NetMsg type, const void* payload, size_t payloadSize, uint32_t count,
                        uint64_t baseTick, bool lossy)
{
    if (!socket_.IsOpen() || !peer_.Valid()) {
        return;
    }
    if (lossy && cfg_.lossPercent > 0 && (lossRng_.NextU32() % 100u) < cfg_.lossPercent) {
        ++packetsDropped_;
        return; // --net-loss: 送らずに捨てる (2 者間なら送信側で落とすのと受信側で
                // 落とすのは等価。送信側でやると受信経路に検証用の分岐が入らない)
    }
    uint8_t buf[kNetMaxPacket] = {};
    NetPacketHeader h;
    h.type = static_cast<uint16_t>(type);
    h.sessionId = sessionId_;
    h.playerIndex = localIndex_;
    h.count = count;
    h.baseTick = baseTick;
    h.lastAckTick = nextNeededTick_;
    h.sendTimeMs = NowMs();
    h.echoTimeMs = peerSendTimeMs_;
    std::memcpy(buf, &h, sizeof(h));
    if (payload != nullptr && payloadSize > 0) {
        std::memcpy(buf + sizeof(h), payload, payloadSize);
    }
    if (socket_.Send(peer_, buf, sizeof(h) + payloadSize)) {
        ++packetsSent_;
        lastSendMs_ = NowMs();
    }
}

void NetSession::SendHandshake(NetMsg type, NetReject reason, uint32_t assignedIndex)
{
    NetHandshakePayload p;
    p.id = localId_;
    p.assignedIndex = assignedIndex;
    p.reason = static_cast<uint32_t>(reason);
    SendTo(type, &p, sizeof(p), 0, 0, false); // 制御メッセージはロス注入の対象外
}

void NetSession::SendInputPacket(uint64_t upToTick)
{
    // 直近 kNetRedundancy tick を新しい順に集めてから反転する。
    // 「リングに実際に入っている自レーンの値」だけを載せるので、開始直後で
    // まだ 8 本たまっていなくても正しい長さのパケットになる
    InputSnapshot tmp[kNetRedundancy] = {};
    uint32_t n = 0;
    uint64_t t = upToTick;
    while (n < kNetRedundancy) {
        const size_t idx = Slot(t) + localIndex_;
        if (stamp_[idx] != t) {
            break;
        }
        tmp[n++] = ring_[idx];
        if (t == startTick_) {
            break;
        }
        --t;
    }
    if (n == 0) {
        return;
    }
    InputSnapshot payload[kNetRedundancy] = {};
    for (uint32_t i = 0; i < n; ++i) {
        payload[i] = tmp[n - 1 - i]; // 昇順へ戻す
    }
    SendTo(NetMsg::Input, payload, static_cast<size_t>(n) * sizeof(InputSnapshot), n,
           upToTick - (n - 1), true);
}

void NetSession::StoreInput(uint64_t tick, uint32_t player, const InputSnapshot& in)
{
    // リングの外 (消費済み or 遠すぎる未来) は捨てる。
    // ★ここを素通しにすると、512 tick 前の残党パケットが同じスロットへ書き込んで
    //   「今まさに要る tick」を静かに壊す
    if (tick < nextNeededTick_ || tick >= nextNeededTick_ + kNetRingTicks) {
        return;
    }
    if (player >= kMaxPlayers) {
        return;
    }
    const size_t idx = Slot(tick) + player;
    ring_[idx] = in;
    stamp_[idx] = tick;
}

void NetSession::HandlePacket(const NetAddress& from, const uint8_t* data, int size)
{
    if (size < static_cast<int>(sizeof(NetPacketHeader))) {
        return;
    }
    NetPacketHeader h;
    std::memcpy(&h, data, sizeof(h));
    if (h.magic != kNetMagic || h.proto != static_cast<uint16_t>(kNetProtoVersion)) {
        return; // 別プロトコル / 版違い — 静かに捨てる (待受ポートには何でも飛んでくる)
    }
    ++packetsRecv_;
    const NetMsg type = static_cast<NetMsg>(h.type);
    const uint8_t* body = data + sizeof(h);
    const int bodySize = size - static_cast<int>(sizeof(h));

    if (type == NetMsg::Join) {
        if (cfg_.role != NetRole::Host || bodySize < static_cast<int>(sizeof(NetHandshakePayload))) {
            return;
        }
        NetHandshakePayload p;
        std::memcpy(&p, body, sizeof(p));
        if (state_ == NetState::Running) {
            // ★Accept が落ちた場合、参加側は Join を撃ち続ける。同じ相手なら撃ち返す
            //   (ここを無視すると「接続できているのに参加側だけ待ち続ける」で固まる)
            if (from == peer_) {
                SendHandshake(NetMsg::Accept, NetReject::None, 1);
            } else {
                const NetAddress save = peer_;
                peer_ = from;
                SendHandshake(NetMsg::Reject, NetReject::Busy, 0);
                peer_ = save;
            }
            return;
        }
        if (state_ != NetState::Connecting) {
            return;
        }
        const NetReject bad = CompareNetIdentity(localId_, p.id);
        peer_ = from;
        if (bad != NetReject::None) {
            MYE_LOG_ERROR("[net] rejecting %s: %s", NetAddressToString(from).c_str(),
                          NetRejectName(bad));
            SendHandshake(NetMsg::Reject, bad, 0);
            peer_ = NetAddress{};
            return;
        }
        localIndex_ = 0;
        SendHandshake(NetMsg::Accept, NetReject::None, 1);
        state_ = NetState::Running;
        lastRecvMs_ = NowMs();
        peerSendTimeMs_ = h.sendTimeMs;
        MYE_LOG_INFO("[net] peer joined from %s as player %u (session %llu)",
                     NetAddressToString(from).c_str(), 1u,
                     static_cast<unsigned long long>(sessionId_));
        return;
    }

    if (type == NetMsg::Accept || type == NetMsg::Reject) {
        if (cfg_.role != NetRole::Join || bodySize < static_cast<int>(sizeof(NetHandshakePayload))) {
            return;
        }
        NetHandshakePayload p;
        std::memcpy(&p, body, sizeof(p));
        if (type == NetMsg::Reject) {
            reject_ = static_cast<NetReject>(p.reason);
            MYE_LOG_ERROR("[net] host rejected the connection: %s", NetRejectName(reject_));
            state_ = NetState::Failed;
            return;
        }
        if (state_ != NetState::Connecting) {
            return; // Accept の重複 (こちらの Join が重複していた) — 無視
        }
        const NetReject bad = CompareNetIdentity(localId_, p.id);
        if (bad != NetReject::None) {
            // ホスト側の照合を通っても、こちらからも見る (片側だけの検査にしない)
            reject_ = bad;
            MYE_LOG_ERROR("[net] refusing the host: %s", NetRejectName(bad));
            state_ = NetState::Failed;
            return;
        }
        if (p.assignedIndex >= cfg_.playerCount) {
            reject_ = NetReject::PlayerCount;
            MYE_LOG_ERROR("[net] host assigned lane %u but only %u lanes exist", p.assignedIndex,
                          cfg_.playerCount);
            state_ = NetState::Failed;
            return;
        }
        sessionId_ = h.sessionId;
        localIndex_ = p.assignedIndex;
        peer_ = from;
        state_ = NetState::Running;
        lastRecvMs_ = NowMs();
        peerSendTimeMs_ = h.sendTimeMs;
        MYE_LOG_INFO("[net] connected to %s as player %u (session %llu)",
                     NetAddressToString(from).c_str(), localIndex_,
                     static_cast<unsigned long long>(sessionId_));
        return;
    }

    // ---- ここから先はセッション確立後のメッセージ ----
    if (state_ != NetState::Running || h.sessionId != sessionId_ || from != peer_) {
        return; // 前回実行の残党 / 第三者
    }
    lastRecvMs_ = NowMs();
    peerSendTimeMs_ = h.sendTimeMs;
    if (h.echoTimeMs != 0) {
        // 相手が echo し返した「こちらの送信時刻」との差 = RTT (どちらも自分の時計で完結)
        const float rtt = static_cast<float>(static_cast<int32_t>(NowMs() - h.echoTimeMs));
        if (rtt >= 0.0f) {
            pingMs_ = (pingMs_ <= 0.0f) ? rtt : (pingMs_ * 0.9f + rtt * 0.1f);
        }
    }

    if (type == NetMsg::Bye) {
        peerSaidBye_ = true;
        return;
    }
    if (type != NetMsg::Input) {
        return;
    }
    if (h.playerIndex >= cfg_.playerCount || h.playerIndex == localIndex_) {
        return; // 自分のレーンを他人に書かせない
    }
    if (h.count > kNetRedundancy
        || bodySize < static_cast<int>(h.count * sizeof(InputSnapshot))) {
        return; // 壊れた / 想定外のパケット
    }
    for (uint32_t i = 0; i < h.count; ++i) {
        InputSnapshot in;
        std::memcpy(&in, body + static_cast<size_t>(i) * sizeof(InputSnapshot),
                    sizeof(InputSnapshot));
        StoreInput(h.baseTick + i, h.playerIndex, in);
    }
}

void NetSession::Poll()
{
    if (state_ != NetState::Connecting && state_ != NetState::Running) {
        return;
    }
    uint8_t buf[kNetMaxPacket];
    NetAddress from;
    for (int guard = 0; guard < 256; ++guard) { // 1 回の Poll で捌く上限 (受信洪水対策)
        const int got = socket_.Recv(buf, sizeof(buf), from);
        if (got <= 0) {
            break;
        }
        HandlePacket(from, buf, got);
    }
    if (state_ != NetState::Running) {
        return;
    }
    // ★キープアライブ兼再送。**これが無いと相互デッドロックする** — 自分が stall して
    //   いる間は SubmitLocalInput が呼ばれない = 1 パケットも出ないので、相手が待って
    //   いる tick を載せたパケットが落ちていた場合、誰も送り直さないまま双方が固まる
    const uint32_t now = NowMs();
    if (hasSubmitted_ && now - lastSendMs_ >= kNetKeepAliveMs) {
        SendInputPacket(lastSubmitted_);
    }
    if (now - lastRecvMs_ > cfg_.stallTimeoutMs) {
        MYE_LOG_ERROR("[net] peer timed out (%u ms without a packet, waiting for tick %llu)",
                      now - lastRecvMs_, static_cast<unsigned long long>(nextNeededTick_));
        state_ = NetState::Failed;
    }
}

bool NetSession::WaitUntilReady(const std::function<bool()>& pump)
{
    if (state_ == NetState::Running) {
        return true;
    }
    if (state_ != NetState::Connecting) {
        return false;
    }
    const uint32_t begin = NowMs();
    while (state_ == NetState::Connecting) {
        Poll();
        if (state_ == NetState::Running) {
            break;
        }
        if (state_ == NetState::Failed) {
            return false;
        }
        if (cfg_.role == NetRole::Join) {
            const uint32_t now = NowMs();
            if (lastJoinMs_ == 0 || now - lastJoinMs_ >= kJoinRetryMs) {
                lastJoinMs_ = now == 0 ? 1 : now;
                SendHandshake(NetMsg::Join, NetReject::None, 0);
            }
        }
        if (pump && !pump()) {
            MYE_LOG_WARN("[net] connect aborted by the window");
            state_ = NetState::Failed;
            return false;
        }
        if (NowMs() - begin > cfg_.connectTimeoutMs) {
            MYE_LOG_ERROR("[net] connect timed out after %u ms", cfg_.connectTimeoutMs);
            state_ = NetState::Failed;
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    lastRecvMs_ = NowMs();
    return state_ == NetState::Running;
}

bool NetSession::HasInputs(uint64_t tick) const
{
    if (state_ != NetState::Running) {
        return false;
    }
    const size_t base = Slot(tick);
    for (uint32_t p = 0; p < cfg_.playerCount; ++p) {
        if (stamp_[base + p] != tick) {
            return false;
        }
    }
    return true;
}

bool NetSession::WaitForInputs(uint64_t tick, uint32_t maxWaitMs)
{
    const uint32_t begin = NowMs();
    for (;;) {
        Poll();
        if (HasInputs(tick)) {
            return true;
        }
        if (state_ != NetState::Running || NowMs() - begin >= maxWaitMs) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

const InputSnapshot* NetSession::InputsFor(uint64_t tick) const
{
    return &ring_[Slot(tick)];
}

void NetSession::SubmitLocalInput(uint64_t tick, const InputSnapshot& in)
{
    if (state_ != NetState::Running) {
        return;
    }
    StoreInput(tick, localIndex_, in);
    lastSubmitted_ = tick;
    hasSubmitted_ = true;
    SendInputPacket(tick);
}

void NetSession::OnTickConsumed(uint64_t tick)
{
    if (tick + 1 > nextNeededTick_) {
        nextNeededTick_ = tick + 1;
    }
}

void NetSession::NoteStall(double ms)
{
    ++stallCount_;
    stallMs_ += ms;
}

void NetSession::Finish()
{
    if (state_ != NetState::Running) {
        return;
    }
    // ★最後の入力を撃ち切ってから抜ける。相手はこちらの最後の数 tick をまだ受け取って
    //   いないかもしれず、ここで黙って終了すると相手だけが stall タイムアウトで落ちる。
    //   再送機構が無いぶん、終了時だけは冗長回数を厚くして確率で押し切る
    for (int i = 0; i < 16 && hasSubmitted_; ++i) {
        SendInputPacket(lastSubmitted_);
    }
    SendTo(NetMsg::Bye, nullptr, 0, 0, 0, false);
    MYE_LOG_INFO("[net] session finished: sent %llu / recv %llu packets, dropped %llu, "
                 "stalls %llu (%.1f ms), ping %.1f ms",
                 static_cast<unsigned long long>(packetsSent_),
                 static_cast<unsigned long long>(packetsRecv_),
                 static_cast<unsigned long long>(packetsDropped_),
                 static_cast<unsigned long long>(stallCount_), stallMs_,
                 static_cast<double>(pingMs_));
    state_ = NetState::Closed;
}

} // namespace mye
