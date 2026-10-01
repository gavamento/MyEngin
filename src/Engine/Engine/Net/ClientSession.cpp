//====================================================================================
//                          ClientSession.cpp
//  MyEngin/ 秋田蓮音                                                     10/02/2026
//                                          入力確定型サーバへ繋ぐクライアントのセッション実装
//====================================================================================
#include "Engine/Engine/Net/ClientSession.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Util/Hash.h"
#include "Engine/Engine/Session/Provenance.h"

namespace mye {
namespace {

constexpr double kTickMs = 1000.0 / 60.0;
// 追いつきの許容幅。これ以内の遅れは速度係数 (±2%) に任せる
constexpr int64_t kCatchUpSlackTicks = 2;
constexpr uint32_t kResyncRetryMs = 200;
constexpr uint32_t kMaxSnapshotBytes = 256u * 1024u * 1024u;

} // namespace

NetPacketHeader ClientSession::BaseHeader(uint64_t nowMs) const
{
    NetPacketHeader h;
    h.sessionId = sessionId_;
    h.playerIndex = lane_;
    h.lastAckTick = frontier_;
    h.sendTimeMs = static_cast<uint32_t>(nowMs);
    h.confirmTick = localConfirmTick_;
    h.confirmHash = localConfirmHash_;
    return h;
}

void ClientSession::Send(NetMsg type, const NetPacketHeader& h, const void* payload, size_t payloadSize,
                         const void* tail, size_t tailSize)
{
    NetBuildPacket(scratch_, type, h, payload, payloadSize, tail, tailSize);
    send_(scratch_.data(), scratch_.size());
    ++stats_.packetsOut;
    lastSendMs_ = now_;
}

void ClientSession::Start(const ClientSessionConfig& cfg, ClientSendFn send, uint64_t nowMs)
{
    now_ = nowMs;
    cfg_ = cfg;
    send_ = std::move(send);
    stats_ = ClientStats{};
    state_ = ClientState::Connecting;
    reject_ = NetReject::None;
    failReason_.clear();
    sessionId_ = 0;
    lane_ = 0;
    playerId_ = cfg.playerId;
    config_ = SessionConfig{};
    meta_ = SnapshotMeta{};
    blob_.clear();
    serial_ = 0;
    chunkCount_ = 0;
    chunkGot_.clear();
    recvBase_ = 0;
    attempts_ = 0;
    awaitingWelcome_ = false;
    waitingFirstConfirmed_ = false;
    conf_.assign(kNetHistoryTicks, NetConfirmedTick{});
    stamp_.assign(kNetHistoryTicks, 0);
    frontier_ = consumedFloor_ = 0;
    lastSeenEventSeq_ = 0;
    std::memset(svCp_, 0, sizeof(svCp_));
    local_.assign(kNetRingTicks, InputSnapshot{});
    localStamp_.assign(kNetRingTicks, 0);
    newestLocal_ = 0;
    hasLocal_ = false;
    localConfirmTick_ = localConfirmHash_ = 0;
    marginMs_ = 0.0;
    marginValid_ = false;
    rttMs_ = 0.0;
    serverFrontier_ = 0;
    startMs_ = lastRecvMs_ = lastAckMs_ = lastResyncReqMs_ = nowMs;
    lastSendMs_ = nowMs;
    SendHello(nowMs);
}

void ClientSession::Fail(const char* why, NetReject reason)
{
    if (state_ == ClientState::Failed || state_ == ClientState::Closed) {
        return;
    }
    MYE_LOG_ERROR("[client] session failed: %s", why);
    failReason_ = why;
    reject_ = reason;
    state_ = ClientState::Failed;
}

void ClientSession::SendHello(uint64_t nowMs)
{
    NetHelloPayload hp = {};
    hp.prov = cfg_.provenance;
    hp.prov.initialSnapshotHash = 0;
    hp.playerId = cfg_.playerId;
    hp.fontMetricsHash = cfg_.fontMetricsHash;
    hp.configBits = cfg_.configBits;
    hp.referenceW = cfg_.referenceW;
    hp.referenceH = cfg_.referenceH;
    std::strncpy(hp.playerSessionId, cfg_.playerSessionId.c_str(), kNetPlayerSessionIdLen - 1);
    NetPacketHeader h;
    h.sendTimeMs = static_cast<uint32_t>(nowMs);
    Send(NetMsg::Hello, h, &hp, sizeof(hp));
    lastHelloMs_ = nowMs;
}

void ClientSession::SendAck(uint64_t nowMs)
{
    NetSnapshotAckPayload a = {};
    a.serial = serial_;
    a.base = recvBase_;
    for (uint32_t i = 0; i < kNetAckWindow; ++i) {
        const uint32_t idx = recvBase_ + i;
        if (idx >= chunkCount_) {
            break;
        }
        if (chunkGot_[idx] != 0) {
            a.mask[i / 64] |= (1ull << (i % 64));
        }
    }
    Send(NetMsg::SnapshotAck, BaseHeader(nowMs), &a, sizeof(a));
    lastAckMs_ = nowMs;
}

void ClientSession::SendInputPacket(uint64_t upToTick, uint64_t nowMs)
{
    // upToTick で終わる連続した直近 kNetRedundancy 本 (保持している範囲)
    uint32_t n = 0;
    while (n < kNetRedundancy && n <= upToTick) {
        const uint64_t t = upToTick - n;
        if (localStamp_[t % kNetRingTicks] != t + 1) {
            break;
        }
        ++n;
    }
    const uint64_t first = upToTick + 1 - n;
    std::vector<InputSnapshot> ins(n);
    for (uint32_t i = 0; i < n; ++i) {
        ins[i] = local_[(first + i) % kNetRingTicks];
    }
    NetPacketHeader h = BaseHeader(nowMs);
    h.baseTick = first;
    h.count = n;
    Send(NetMsg::ClientInput, h, nullptr, 0, ins.data(), n * sizeof(InputSnapshot));
}

void ClientSession::SubmitLocalInput(uint64_t tick, const InputSnapshot& in, uint64_t nowMs)
{
    if (state_ != ClientState::Running) {
        return;
    }
    now_ = (std::max)(now_, nowMs);
    const size_t slot = tick % kNetRingTicks;
    if (localStamp_[slot] == tick + 1) {
        return; // 一度決めた値は変えない (再同期後の再投入など)
    }
    local_[slot] = in;
    localStamp_[slot] = tick + 1;
    newestLocal_ = hasLocal_ ? (std::max)(newestLocal_, tick) : tick;
    hasLocal_ = true;
    SendInputPacket(tick, nowMs);
}

bool ClientSession::LocalInput(uint64_t tick, InputSnapshot& out) const
{
    const size_t slot = tick % kNetRingTicks;
    if (localStamp_.empty() || localStamp_[slot] != tick + 1) {
        return false;
    }
    out = local_[slot];
    return true;
}

void ClientSession::ResetStream(uint64_t fromTick)
{
    std::fill(stamp_.begin(), stamp_.end(), 0ull);
    frontier_ = consumedFloor_ = fromTick;
    serverFrontier_ = fromTick;
    lastSeenEventSeq_ = meta_.lastEventSeq;
    std::memset(svCp_, 0, sizeof(svCp_));
    marginValid_ = false;
}

void ClientSession::OnPacket(const void* data, size_t size, uint64_t nowMs)
{
    now_ = (std::max)(now_, nowMs);
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    NetPacketHeader h;
    if (state_ == ClientState::Idle || state_ == ClientState::Failed || state_ == ClientState::Closed) {
        return;
    }
    ++stats_.packetsIn;
    if (!NetParseHeader(bytes, size, h)) {
        ++stats_.badPackets;
        return;
    }
    const uint8_t* body = bytes + sizeof(NetPacketHeader);
    const size_t bodySize = size - sizeof(NetPacketHeader);
    const NetMsg type = static_cast<NetMsg>(h.type);

    if (type == NetMsg::Reject) {
        NetRejectPayload r = {};
        if (bodySize >= sizeof(r) && state_ == ClientState::Connecting) {
            std::memcpy(&r, body, sizeof(r));
            Fail(NetRejectName(static_cast<NetReject>(r.reason)), static_cast<NetReject>(r.reason));
        }
        return;
    }
    if (type == NetMsg::Welcome) {
        OnWelcome(h, body, bodySize, nowMs);
        return;
    }
    if (sessionId_ == 0 || h.sessionId != sessionId_) {
        return; // 残党パケット
    }
    lastRecvMs_ = nowMs;
    if (type == NetMsg::SnapshotChunk) {
        OnChunk(body, bodySize, nowMs);
    } else if (type == NetMsg::Confirmed) {
        OnConfirmed(h, body, bodySize, nowMs);
    }
}

void ClientSession::OnWelcome(const NetPacketHeader& h, const uint8_t* body, size_t bodySize,
                              uint64_t nowMs)
{
    NetWelcomePayload w;
    if (bodySize < sizeof(w)) {
        ++stats_.badPackets;
        return;
    }
    std::memcpy(&w, body, sizeof(w));
    if (state_ == ClientState::Connecting) {
        // サーバの出自を自分と突き合わせる (サーバ側の照合を通っても、こちらからも見る)
        SimProvenance mine = cfg_.provenance;
        SimProvenance theirs = w.meta.provenance;
        mine.initialSnapshotHash = 0;
        theirs.initialSnapshotHash = 0;
        const NetReject bad =
            NetRejectFromProvenance(CompareProvenance(mine, theirs, cfg_.allowGameMismatch));
        if (bad != NetReject::None) {
            Fail(NetRejectName(bad), bad);
            return;
        }
        sessionId_ = h.sessionId;
        playerId_ = w.playerId;
    } else if (sessionId_ == 0 || h.sessionId != sessionId_ || w.playerId != playerId_) {
        return;
    }
    if (w.serial == serial_) {
        return; // 同じ送付の Welcome の再送
    }
    const uint64_t bytes = w.snapshotBytes;
    if (w.snapshotBytes == 0 || w.snapshotBytes > kMaxSnapshotBytes
        || w.chunkCount != (bytes + kNetSnapshotChunkBytes - 1) / kNetSnapshotChunkBytes) {
        Fail("the server sent a malformed Welcome");
        return;
    }
    meta_ = w.meta;
    config_ = w.meta.config;
    lane_ = w.lane;
    serial_ = w.serial;
    chunkCount_ = w.chunkCount;
    blob_.assign(w.snapshotBytes, std::byte{ 0 });
    chunkGot_.assign(chunkCount_, 0);
    recvBase_ = 0;
    awaitingWelcome_ = false;
    waitingFirstConfirmed_ = false;
    state_ = ClientState::Downloading;
    lastRecvMs_ = nowMs;
    ResetStream(meta_.tick);
    MYE_LOG_INFO("[client] welcome: lane %u, player %llu, snapshot %u bytes in %u chunks (from tick %llu)",
                 lane_, static_cast<unsigned long long>(playerId_), w.snapshotBytes, chunkCount_,
                 static_cast<unsigned long long>(meta_.tick));
    SendAck(nowMs);
}

void ClientSession::OnChunk(const uint8_t* body, size_t bodySize, uint64_t nowMs)
{
    NetSnapshotChunkPayload c;
    if (state_ != ClientState::Downloading || awaitingWelcome_ || bodySize < sizeof(c)) {
        return;
    }
    std::memcpy(&c, body, sizeof(c));
    if (c.serial != serial_ || c.index >= chunkCount_ || c.size > kNetSnapshotChunkBytes
        || bodySize < sizeof(c) + c.size) {
        return;
    }
    const size_t off = static_cast<size_t>(c.index) * kNetSnapshotChunkBytes;
    if (off + c.size > blob_.size()) {
        ++stats_.badPackets;
        return;
    }
    if (chunkGot_[c.index] == 0) {
        std::memcpy(blob_.data() + off, body + sizeof(c), c.size);
        chunkGot_[c.index] = 1;
    }
    while (recvBase_ < chunkCount_ && chunkGot_[recvBase_] != 0) {
        ++recvBase_;
    }
    if (recvBase_ >= chunkCount_) {
        if (HashBytes(blob_.data(), blob_.size()) != meta_.blobHash) {
            // 通信路で壊れた (UDP のチェックサムをすり抜けた) か、サーバの取り違え。撮り直してもらう
            MYE_LOG_ERROR("[client] snapshot blob hash mismatch");
            RequestResync(NetResyncReason::BadSnapshot, nowMs);
            return;
        }
        state_ = ClientState::SnapshotReady;
        SendAck(nowMs);
    }
}

void ClientSession::OnSnapshotApplied(bool worldHashMatches, uint64_t nowMs)
{
    if (state_ != ClientState::SnapshotReady) {
        return;
    }
    if (worldHashMatches) {
        state_ = ClientState::Running;
        attempts_ = 0;
        ++stats_.snapshotsApplied;
        waitingFirstConfirmed_ = true;
        lastRecvMs_ = nowMs;
        blob_.clear();
        blob_.shrink_to_fit();
        SendAck(nowMs); // 受信完了をサーバへ確実に伝える (以後 Confirmed の配信が始まる)
        return;
    }
    ++attempts_;
    MYE_LOG_ERROR("[client] restored world hash differs from the snapshot meta (attempt %u/%u)", attempts_,
                  cfg_.maxSnapshotAttempts);
    if (attempts_ >= cfg_.maxSnapshotAttempts) {
        Fail("the snapshot could not be restored to the server's world hash");
        return;
    }
    RequestResync(NetResyncReason::BadSnapshot, nowMs);
}

const NetConfirmedTick* ClientSession::Confirmed(uint64_t tick) const
{
    return HasConfirmed(tick) ? &conf_[tick % kNetHistoryTicks] : nullptr;
}

InputSnapshot ClientSession::PredictLane(uint64_t tick, uint32_t lane) const
{
    if (frontier_ > meta_.tick && lane < kMaxPlayers) {
        uint64_t t = (std::min)(tick, frontier_ - 1);
        for (uint32_t k = 0; k < kNetRedundancy * 2; ++k) {
            if (HasConfirmed(t)) {
                return SubstituteLateInput(conf_[t % kNetHistoryTicks].inputs[lane]);
            }
            if (t == 0 || t <= meta_.tick) {
                break;
            }
            --t;
        }
    }
    return InputSnapshot{};
}

void ClientSession::AdvanceFrontier(uint64_t nowMs)
{
    while (IsStored(frontier_) && frontier_ >= meta_.tick) {
        const NetConfirmedTick& t = conf_[frontier_ % kNetHistoryTicks];
        // eventSeq は 1 ずつ増えるはず。欠番 = 受け取っていないイベントがある (サーバの不具合 / 改竄)
        uint64_t seen = lastSeenEventSeq_;
        for (uint32_t i = 0; i < t.sys.eventCount; ++i) {
            if (t.sys.events[i].eventSeq != seen + 1) {
                ++stats_.eventGaps;
                MYE_LOG_ERROR("[client] eventSeq gap at tick %llu: expected %llu, got %llu - asking for a resync",
                              static_cast<unsigned long long>(frontier_),
                              static_cast<unsigned long long>(seen + 1),
                              static_cast<unsigned long long>(t.sys.events[i].eventSeq));
                RequestResync(NetResyncReason::EventGap, nowMs);
                return;
            }
            seen = t.sys.events[i].eventSeq;
        }
        lastSeenEventSeq_ = seen;
        ++frontier_;
    }
}

void ClientSession::OnConfirmed(const NetPacketHeader& h, const uint8_t* body, size_t bodySize,
                                uint64_t nowMs)
{
    NetConfirmedPayload pl;
    if ((state_ != ClientState::Running && state_ != ClientState::SnapshotReady) || bodySize < sizeof(pl)) {
        return;
    }
    std::memcpy(&pl, body, sizeof(pl));
    serverFrontier_ = (std::max)(serverFrontier_, h.lastAckTick);
    if ((pl.flags & kNetConfirmedFlagMarginValid) != 0) {
        const double sample = static_cast<double>(pl.marginMs);
        marginMs_ = marginValid_ ? marginMs_ + (sample - marginMs_) / 8.0 : sample;
        marginValid_ = true;
    }
    if (h.echoTimeMs != 0) {
        const uint32_t elapsed = static_cast<uint32_t>(nowMs) - h.echoTimeMs;
        if (elapsed < 10000) {
            rttMs_ = (rttMs_ == 0.0) ? elapsed : rttMs_ + (static_cast<double>(elapsed) - rttMs_) / 8.0;
        }
    }
    for (const NetCheckpoint& c : pl.checkpoints) {
        if (c.hash != 0 && c.tick % kNetHashCheckpoint == 0) {
            svCp_[(c.tick / kNetHashCheckpoint) % 64] = c;
        }
    }
    size_t at = sizeof(pl);
    for (uint32_t i = 0; i < h.count; ++i) {
        const uint64_t tick = h.baseTick + i;
        NetConfirmedTick t;
        const size_t n = NetReadTickRecord(body + at, bodySize - at, tick, t);
        if (n == 0) {
            ++stats_.badPackets;
            break;
        }
        at += n;
        if (tick < meta_.tick || tick >= consumedFloor_ + kNetHistoryTicks) {
            continue;
        }
        if (IsStored(tick)) {
            ++stats_.duplicateRecords;
            continue;
        }
        conf_[tick % kNetHistoryTicks] = t;
        stamp_[tick % kNetHistoryTicks] = tick + 1;
    }
    waitingFirstConfirmed_ = false;
    AdvanceFrontier(nowMs);
}

void ClientSession::OnTickConsumed(uint64_t tick)
{
    consumedFloor_ = (std::max)(consumedFloor_, tick + 1);
}

void ClientSession::SetLocalConfirmed(uint64_t tick, uint64_t hash)
{
    localConfirmTick_ = tick;
    localConfirmHash_ = hash;
}

bool ClientSession::ServerCheckpointHash(uint64_t tick, uint64_t& outHash) const
{
    if (tick % kNetHashCheckpoint != 0) {
        return false;
    }
    const NetCheckpoint& c = svCp_[(tick / kNetHashCheckpoint) % 64];
    if (c.tick != tick || c.hash == 0) {
        return false;
    }
    outHash = c.hash;
    return true;
}

void ClientSession::RequestResync(NetResyncReason reason, uint64_t nowMs)
{
    if (state_ == ClientState::Idle || state_ == ClientState::Connecting || state_ == ClientState::Failed
        || state_ == ClientState::Closed || awaitingWelcome_) {
        return;
    }
    now_ = (std::max)(now_, nowMs);
    ++stats_.resyncs;
    MYE_LOG_WARN("[client] requesting a resync: %s", NetResyncReasonName(reason));
    resyncReason_ = reason;
    awaitingWelcome_ = true;
    state_ = ClientState::Downloading;
    lastResyncReqMs_ = nowMs;
    NetResyncPayload r = {};
    r.reason = static_cast<uint32_t>(reason);
    r.haveTick = frontier_;
    Send(NetMsg::ResyncRequest, BaseHeader(nowMs), &r, sizeof(r));
}

void ClientSession::Close(uint64_t nowMs)
{
    if (state_ == ClientState::Idle || state_ == ClientState::Failed || state_ == ClientState::Closed) {
        return;
    }
    now_ = (std::max)(now_, nowMs);
    if (sessionId_ != 0) {
        Send(NetMsg::Bye, BaseHeader(nowMs), nullptr, 0);
    }
    state_ = ClientState::Closed;
}

void ClientSession::Poll(uint64_t nowMs)
{
    now_ = (std::max)(now_, nowMs);
    switch (state_) {
    case ClientState::Connecting:
        if (nowMs - startMs_ > cfg_.connectTimeoutMs) {
            Fail("connect timeout (no Welcome from the server)");
        } else if (nowMs - lastHelloMs_ >= cfg_.helloRetryMs) {
            SendHello(nowMs);
        }
        break;
    case ClientState::Downloading:
        if (nowMs - lastRecvMs_ > cfg_.serverTimeoutMs) {
            Fail("server timeout while downloading the snapshot");
        } else if (awaitingWelcome_) {
            if (nowMs - lastResyncReqMs_ >= kResyncRetryMs) {
                lastResyncReqMs_ = nowMs;
                NetResyncPayload r = {};
                r.reason = static_cast<uint32_t>(resyncReason_);
                r.haveTick = frontier_;
                Send(NetMsg::ResyncRequest, BaseHeader(nowMs), &r, sizeof(r));
            }
        } else if (nowMs - lastAckMs_ >= cfg_.ackIntervalMs) {
            SendAck(nowMs);
        }
        break;
    case ClientState::SnapshotReady:
        if (nowMs - lastAckMs_ >= cfg_.ackIntervalMs) {
            SendAck(nowMs);
        }
        break;
    case ClientState::Running:
        if (nowMs - lastRecvMs_ > cfg_.serverTimeoutMs) {
            Fail("server timeout");
            break;
        }
        if (waitingFirstConfirmed_ && nowMs - lastAckMs_ >= cfg_.ackIntervalMs) {
            SendAck(nowMs); // 最後の ack が落ちていても、Confirmed が届くまで繰り返す
        }
        if (nowMs - lastSendMs_ >= cfg_.keepAliveMs) {
            NetPacketHeader h = BaseHeader(nowMs);
            h.baseTick = newestLocal_;
            Send(NetMsg::ClientInput, h, nullptr, 0);
        }
        break;
    default:
        break;
    }
}

double ClientSession::SpeedFactor() const
{
    if (!marginValid_) {
        return 1.0;
    }
    const double err = (static_cast<double>(cfg_.targetMarginMs) - marginMs_) / kTickMs;
    return 1.0 + 0.02 * (std::max)(-1.0, (std::min)(1.0, err));
}

int64_t ClientSession::CatchUpTicks(uint64_t clientTick) const
{
    // 入力 (tick + inputDelay) がサーバの確定フロンティアに間に合う最小の自 tick。
    // フロンティアは RTT/2 前の値なので、往復ぶん先を見込む
    const int64_t rttTicks = static_cast<int64_t>(std::ceil(rttMs_ / kTickMs));
    const int64_t minTick = static_cast<int64_t>(serverFrontier_) + rttTicks
        - static_cast<int64_t>(config_.inputDelay) + 1;
    const int64_t behind = minTick - static_cast<int64_t>(clientTick);
    return behind > kCatchUpSlackTicks ? behind : 0;
}

} // namespace mye
