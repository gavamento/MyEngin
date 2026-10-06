//====================================================================================
//                          ServerSession.cpp
//  MyEngin/ 秋田蓮音                                                     10/02/2026
//                                          入力確定型サーバのセッション実装
//====================================================================================
#include "Engine/Engine/Net/ServerSession.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Util/Hash.h"
#include "Engine/Engine/Net/NetRollback.h"
#include "Engine/Engine/Session/Provenance.h"

namespace mye {
namespace {

constexpr uint64_t kNoTickMark = ~0ull;

int32_t ClampMargin(int64_t v)
{
    return static_cast<int32_t>((std::max<int64_t>)(-5000, (std::min<int64_t>)(5000, v)));
}

} // namespace

bool ServerSession::Init(const ServerSessionConfig& cfg, const ServerHooks& hooks)
{
    if (cfg.session.playerCount < 1 || cfg.session.playerCount > kMaxPlayers || !hooks.send) {
        MYE_LOG_ERROR("[server] bad session config (playerCount %u, send hook %s)",
                      cfg.session.playerCount, hooks.send ? "set" : "missing");
        return false;
    }
    cfg_ = cfg;
    hooks_ = hooks;
    stats_ = ServerStats{};
    nextTick_ = cfg.startTick;
    eventSeq_ = 0;
    lanes_ = SessionLanes{};
    pending_.clear();
    peers_.clear();
    history_.assign(kNetHistoryTicks, NetConfirmedTick{});
    confirmMs_.assign(kNetHistoryTicks, 0);
    for (uint32_t l = 0; l < kMaxPlayers; ++l) {
        for (uint32_t i = 0; i < kInputRing; ++i) {
            laneInputs_[l][i] = InputSlot{};
        }
        prevInput_[l] = InputSnapshot{};
        std::memset(laneSid_[l], 0, sizeof(laneSid_[l]));
        reservedSinceTick_[l] = 0;
        laneOwner_[l] = 0;
        releaseQueued_[l] = false;
    }
    std::memset(checkpoints_, 0, sizeof(checkpoints_));
    nextSerial_ = 1;
    chunkRover_ = 0;
    lastNowMs_ = cfg.startMs;
    inited_ = true;
    return true;
}

uint64_t ServerSession::TickDueMs(uint64_t tick) const
{
    return cfg_.startMs + NetTicksToMs(tick - cfg_.startTick);
}

uint64_t ServerSession::DeadlineMs(uint64_t tick) const
{
    return TickDueMs(tick) + NetTicksToMs(cfg_.session.deadlineTicks);
}

uint32_t ServerSession::LivePeerCount() const
{
    uint32_t n = 0;
    for (const Peer& p : peers_) {
        if (p.phase == Phase::Live) {
            ++n;
        }
    }
    return n;
}

uint32_t ServerSession::ActivePeerCount() const
{
    uint32_t n = 0;
    for (const Peer& p : peers_) {
        if (p.phase != Phase::Gone) {
            ++n;
        }
    }
    return n;
}

bool ServerSession::CheckpointHash(uint64_t tick, uint64_t& outHash) const
{
    if (tick % kNetHashCheckpoint != 0) {
        return false;
    }
    const NetCheckpoint& c = checkpoints_[(tick / kNetHashCheckpoint) % 64];
    if (c.tick != tick || c.hash == 0) {
        return false;
    }
    outHash = c.hash;
    return true;
}

ServerSession::Peer* ServerSession::FindPeer(uint32_t key)
{
    for (Peer& p : peers_) {
        if (p.key == key) {
            return &p;
        }
    }
    return nullptr;
}

bool ServerSession::HasPeer(uint32_t key) const
{
    for (const Peer& p : peers_) {
        if (p.key == key) {
            return true;
        }
    }
    return false;
}

ServerSession::Peer* ServerSession::FindPeerByEvent(uint64_t eventSeq)
{
    for (Peer& p : peers_) {
        if (p.eventSeq == eventSeq && eventSeq != 0) {
            return &p;
        }
    }
    return nullptr;
}

ServerSession::Peer* ServerSession::FindLivePeerOfLane(int lane)
{
    for (Peer& p : peers_) {
        if (p.phase == Phase::Live && p.lane == lane) {
            return &p;
        }
    }
    return nullptr;
}

ServerSession::Peer* ServerSession::FindOwnerOfLane(int lane)
{
    for (Peer& p : peers_) {
        if (p.phase != Phase::Gone && p.lane == lane && p.key == laneOwner_[lane]) {
            return &p;
        }
    }
    return nullptr;
}

// 到着余裕の標本を 1 つ足す (tick ごとにちょうど 1 つ)。Confirmed へはその平均を載せる。
// 標本を「最後の値」で送ると、遅れて着いた入力の負の値が、同じ周に確定する別の tick の正の値で上書きされ、
// クライアントは実際より余裕があると思い込む (遅れた tick が多いほど偏る)
void ServerSession::NoteMargin(Peer& p, int64_t marginMs)
{
    const int32_t v = ClampMargin(marginMs);
    p.marginSum += v;
    ++p.marginCount;

    if (p.marginRingCount == kMarginWindow) {
        const int64_t old = p.marginRing[p.marginRingNext];
        p.ringSum -= old;
        p.ringSumSq -= old * old;
        p.ringLate -= (old < 0) ? 1 : 0;
    } else {
        ++p.marginRingCount;
    }
    p.marginRing[p.marginRingNext] = v;
    p.marginRingNext = (p.marginRingNext + 1) % kMarginWindow;
    p.ringSum += v;
    p.ringSumSq += static_cast<int64_t>(v) * v;
    p.ringLate += (v < 0) ? 1 : 0;
}

// 窓内の標本の標準偏差 (1/4 ms 単位、16 bit に収まる範囲)。標本が少ないうちは 0 (= 目標は 1 tick のまま)
uint32_t ServerSession::MarginSigmaQuarterMs(const Peer& p)
{
    constexpr uint32_t kMinSamples = 8;
    if (p.marginRingCount < kMinSamples) {
        return 0;
    }
    const double n = static_cast<double>(p.marginRingCount);
    const double mean = static_cast<double>(p.ringSum) / n;
    const double var = (std::max)(0.0, static_cast<double>(p.ringSumSq) / n - mean * mean);
    const double q = std::sqrt(var) * kNetConfirmedSigmaUnitsPerMs;
    return static_cast<uint32_t>((std::min)(q, 65535.0));
}

// 窓の標本のほぼ全部が締め切り後 = 予測上限 (kNetMaxSpeculationClient tick) に届かず、入力が確定に
// 間に合い続けていない。往復がおよそ (上限 + inputDelay) tick を超える回線で起きる。救済はしない (ログだけ)
void ServerSession::WarnIfUnreachable(Peer& p)
{
    constexpr uint32_t kLateTenths = 9; // 窓の 9/10 以上が遅れたら警告
    if (p.marginRingCount < kMarginWindow) {
        return;
    }
    const bool hopeless = p.ringLate * 10 >= kMarginWindow * kLateTenths;
    if (!hopeless) {
        if (p.ringLate * 2 < kMarginWindow) {
            p.unreachableWarned = false;
        }
        return;
    }
    if (p.unreachableWarned) {
        return;
    }
    p.unreachableWarned = true;
    ++stats_.unreachableWarnings;
    const double tickMs = 1000.0 / static_cast<double>(kNetTickRateHz);
    const double limitMs = (kNetMaxSpeculationClient + cfg_.session.inputDelay) * tickMs;
    const double lateMs = -static_cast<double>(p.ringSum) / static_cast<double>(p.marginRingCount);
    // 遅れは往復時間の推定には使えない。同じ PC で CPU を奪い合っているだけでも同じ遅れになる
    MYE_LOG_WARN("[server] peer %u (lane %d) cannot keep up: its inputs arrive %.0f ms after the deadline on average "
                 "(%u of %u samples late). A round trip above about %.0f ms (speculation %u ticks + input delay %u "
                 "ticks) causes this, and so does a client or server too starved for CPU to hold the tick rate",
                 p.key, p.lane, lateMs, p.ringLate, p.marginRingCount, limitMs, kNetMaxSpeculationClient,
                 cfg_.session.inputDelay);
}

NetPacketHeader ServerSession::BaseHeader() const
{
    NetPacketHeader h;
    h.sessionId = cfg_.sessionId;
    return h;
}

void ServerSession::Send(uint32_t key, NetMsg type, const NetPacketHeader& base, const void* payload,
                         size_t payloadSize, const void* tail, size_t tailSize)
{
    NetBuildPacket(scratch_, type, base, payload, payloadSize, tail, tailSize);
    hooks_.send(key, scratch_.data(), scratch_.size());
    ++stats_.packetsOut;
}

void ServerSession::SendReject(uint32_t key, NetReject reason)
{
    NetRejectPayload pl = {};
    pl.reason = static_cast<uint32_t>(reason);
    Send(key, NetMsg::Reject, BaseHeader(), &pl, sizeof(pl));
    ++stats_.rejects;
    MYE_LOG_WARN("[server] rejected peer %u: %s", key, NetRejectName(reason));
}

uint64_t ServerSession::QueueEvent(SystemEventKind kind, uint64_t playerId, uint8_t lane)
{
    SystemEvent ev = {};
    ev.eventSeq = ++eventSeq_;
    ev.playerId = (kind == SystemEventKind::Join) ? ev.eventSeq : playerId; // playerId = 初回 Join の eventSeq
    ev.kind = static_cast<uint8_t>(kind);
    ev.lane = lane;
    pending_.push_back(ev);
    return ev.eventSeq;
}

uint32_t ServerSession::FreeLaneCount() const
{
    uint32_t used = 0;
    for (uint32_t l = 0; l < kMaxPlayers; ++l) {
        if (lanes_.lanes[l].state != static_cast<uint32_t>(LaneState::Empty)) {
            ++used;
        }
    }
    for (const SystemEvent& ev : pending_) {
        if (ev.kind == static_cast<uint8_t>(SystemEventKind::Join)) {
            ++used;
        }
    }
    return used < cfg_.session.playerCount ? cfg_.session.playerCount - used : 0;
}

void ServerSession::DropPeerInternal(Peer& p, const char* why)
{
    if (p.phase == Phase::Gone) {
        return;
    }
    MYE_LOG_INFO("[server] peer %u (player %llu, lane %d) dropped: %s", p.key,
                 static_cast<unsigned long long>(p.playerId), p.lane, why);
    p.phase = Phase::Gone;
    p.blob.reset();
    // レーンの持ち主のときだけ Leave を積む (乗っ取られた古い peer が新しい持ち主を追い出さない)
    if (p.lane >= 0 && !p.leaveQueued && laneOwner_[p.lane] == p.key
        && lanes_.lanes[p.lane].state == static_cast<uint32_t>(LaneState::Connected)
        && lanes_.lanes[p.lane].playerId == p.playerId) {
        QueueEvent(SystemEventKind::Leave, p.playerId, static_cast<uint8_t>(p.lane));
        p.leaveQueued = true;
    }
    if (!p.leftNotified && p.sid[0] != '\0' && hooks_.playerLeft) {
        hooks_.playerLeft(p.sid);
    }
    p.leftNotified = true;
}

void ServerSession::DropPeer(uint32_t key)
{
    if (Peer* p = FindPeer(key)) {
        DropPeerInternal(*p, "transport closed");
    }
}

void ServerSession::OnPacket(uint32_t key, const void* data, size_t size, uint64_t nowMs)
{
    if (!inited_) {
        return;
    }
    lastNowMs_ = (std::max)(lastNowMs_, nowMs);
    ++stats_.packetsIn;
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    NetPacketHeader h;
    if (!NetParseHeader(bytes, size, h)) {
        ++stats_.badPackets;
        return;
    }
    const uint8_t* body = bytes + sizeof(NetPacketHeader);
    const size_t bodySize = size - sizeof(NetPacketHeader);
    const NetMsg type = static_cast<NetMsg>(h.type);

    if (type == NetMsg::Hello) {
        HandleHello(key, h, body, bodySize, nowMs);
        return;
    }
    Peer* p = FindPeer(key);
    if (p == nullptr || p->phase == Phase::Gone || h.sessionId != cfg_.sessionId) {
        return; // 知らない相手 / 残党パケット
    }
    p->lastRecvMs = nowMs;
    p->clientSendMs = h.sendTimeMs;
    switch (type) {
    case NetMsg::ClientInput:
        HandleClientInput(*p, h, body, bodySize, nowMs);
        break;
    case NetMsg::SnapshotAck:
        HandleSnapshotAck(*p, body, bodySize);
        break;
    case NetMsg::ResyncRequest: {
        NetResyncPayload r = {};
        if (bodySize >= sizeof(r)) {
            std::memcpy(&r, body, sizeof(r));
        }
        if (p->phase == Phase::Live || p->phase == Phase::Syncing) {
            MYE_LOG_WARN("[server] peer %u asked for a resync: %s (it needs tick %llu)", p->key,
                         NetResyncReasonName(static_cast<NetResyncReason>(r.reason)),
                         static_cast<unsigned long long>(r.haveTick));
            p->resyncPending = true;
        }
        break;
    }
    case NetMsg::Bye:
        DropPeerInternal(*p, "bye");
        break;
    default:
        break;
    }
}

void ServerSession::HandleHello(uint32_t key, const NetPacketHeader& h, const uint8_t* body,
                                size_t bodySize, uint64_t nowMs)
{
    NetHelloPayload hp;
    if (bodySize < sizeof(hp)) {
        ++stats_.badPackets;
        return;
    }
    std::memcpy(&hp, body, sizeof(hp));
    hp.playerSessionId[kNetPlayerSessionIdLen - 1] = '\0';

    if (Peer* existing = FindPeer(key)) {
        if (existing->phase != Phase::Gone) {
            existing->lastRecvMs = nowMs; // Hello の再送。Welcome は Pump が出す
            return;
        }
        peers_.erase(peers_.begin() + (existing - peers_.data()));
    }

    // ---- 出自 / 設定の照合 (最初の食い違いを 1 つ返す) ----
    SimProvenance mine = cfg_.provenance;
    SimProvenance theirs = hp.prov;
    mine.initialSnapshotHash = 0; // 参加時のスナップショットは接続ごとに違うので照合しない
    theirs.initialSnapshotHash = 0;
    const bool allowGame = (cfg_.session.configBits & kCfgAllowGameMismatch) != 0;
    NetReject bad = NetRejectFromProvenance(CompareProvenance(mine, theirs, allowGame));
    if (bad == NetReject::None
        && ((hp.configBits ^ cfg_.session.configBits) & ~static_cast<uint32_t>(kCfgAllowGameMismatch)) != 0) {
        bad = NetReject::ConfigBits;
    }
    if (bad == NetReject::None
        && (hp.referenceW != cfg_.session.referenceW || hp.referenceH != cfg_.session.referenceH)) {
        bad = NetReject::ReferenceSize;
    }
    if (bad == NetReject::None && hp.fontMetricsHash != cfg_.session.fontMetricsHash) {
        bad = NetReject::FontMetrics;
    }
    if (bad != NetReject::None) {
        SendReject(key, bad);
        return;
    }
    if (hooks_.validatePlayer && !hooks_.validatePlayer(hp.playerSessionId)) {
        SendReject(key, NetReject::PlayerRejected);
        return;
    }
    if (peers_.size() >= static_cast<size_t>(kMaxPlayers) * 2) {
        SendReject(key, NetReject::ServerFull); // 接続処理中の peer が溜まりすぎ
        return;
    }

    Peer peer;
    peer.key = key;
    peer.lastRecvMs = nowMs;
    peer.clientSendMs = h.sendTimeMs;
    std::memcpy(peer.sid, hp.playerSessionId, kNetPlayerSessionIdLen);
    peer.sid[kNetPlayerSessionIdLen] = '\0';

    if (hp.playerId != 0) {
        // ---- 再接続 ----
        int lane = kNoLane;
        for (uint32_t l = 0; l < kMaxPlayers; ++l) {
            const LaneSlot& s = lanes_.lanes[l];
            if (s.playerId == hp.playerId
                && (s.state == static_cast<uint32_t>(LaneState::Connected)
                    || s.state == static_cast<uint32_t>(LaneState::Reserved))) {
                lane = static_cast<int>(l);
            }
        }
        bool rejoinQueued = false;
        for (const SystemEvent& ev : pending_) {
            rejoinQueued = rejoinQueued || (ev.playerId == hp.playerId
                                            && ev.kind == static_cast<uint8_t>(SystemEventKind::Rejoin));
        }
        if (lane == kNoLane || std::strcmp(laneSid_[lane], peer.sid) != 0 || releaseQueued_[lane]
            || rejoinQueued) {
            SendReject(key, NetReject::UnknownPlayer);
            return;
        }
        if (lanes_.lanes[lane].state == static_cast<uint32_t>(LaneState::Connected)) {
            // 古い接続がまだ生きている扱い (クライアントが落ちて即再起動した等)。乗っ取る:
            // Leave → Rejoin を同じ tick に積めば、レーンは同じまま新しい peer に移る
            // 旧 peer が同じ受信周で Bye / タイムアウトにより Gone 済みなら、Leave はもう積まれている
            // (FindOwnerOfLane は Gone を返さない)。2 本目の Leave は適用時に「not connected」の ERROR になり、
            // 無効なイベントが確定入力 (.rep) に残る
            bool leaveAlreadyQueued = false;
            for (const SystemEvent& ev : pending_) {
                leaveAlreadyQueued = leaveAlreadyQueued || (ev.kind == static_cast<uint8_t>(SystemEventKind::Leave)
                                                            && ev.playerId == hp.playerId);
            }
            if (Peer* old = FindOwnerOfLane(lane)) {
                DropPeerInternal(*old, "taken over by a reconnect");
            } else if (!leaveAlreadyQueued) {
                QueueEvent(SystemEventKind::Leave, hp.playerId, static_cast<uint8_t>(lane));
            }
        }
        peer.playerId = hp.playerId;
        peer.eventSeq = QueueEvent(SystemEventKind::Rejoin, hp.playerId, static_cast<uint8_t>(lane));
        ++stats_.rejoins;
    } else {
        // ---- 新規参加 ----
        if (FreeLaneCount() == 0) {
            SendReject(key, NetReject::ServerFull);
            return;
        }
        peer.eventSeq = QueueEvent(SystemEventKind::Join, 0, 0);
        peer.playerId = peer.eventSeq;
        ++stats_.joins;
    }
    MYE_LOG_INFO("[server] hello from peer %u: %s player %llu (event %llu)", key,
                 hp.playerId != 0 ? "rejoin" : "join", static_cast<unsigned long long>(peer.playerId),
                 static_cast<unsigned long long>(peer.eventSeq));
    peers_.push_back(peer);
}

void ServerSession::HandleClientInput(Peer& p, const NetPacketHeader& h, const uint8_t* body,
                                      size_t bodySize, uint64_t nowMs)
{
    if (p.phase != Phase::Live || p.lane < 0 || h.playerIndex != static_cast<uint32_t>(p.lane)) {
        return;
    }
    const uint32_t n = (std::min)(h.count, kNetRedundancy);
    if (bodySize < static_cast<size_t>(n) * sizeof(InputSnapshot)) {
        ++stats_.badPackets;
        return;
    }
    if (h.lastAckTick > p.ackTick) {
        p.ackTick = (std::min)(h.lastAckTick, nextTick_);
        p.lastAckAdvanceMs = nowMs;
    }
    const uint32_t lane = static_cast<uint32_t>(p.lane);
    for (uint32_t i = 0; i < n; ++i) {
        const uint64_t tick = h.baseTick + i;
        if (tick < nextTick_) {
            // 確定済み。冗長送信で受け取り済みの重複は数えず、間に合わなかった入力だけ数える
            const InputSlot& done = SlotOf(lane, tick);
            // 冗長送信の同じ tick は二度数えない (数えた最新の tick より新しいものだけ)
            if (!(done.valid && done.tick == tick) && (p.lateSampledTick == ~0ull || tick > p.lateSampledTick)) {
                p.lateSampledTick = tick;
                ++stats_.lateInputsDropped;
                // 間に合わなかった入力が実際にいつ着いたか = 本当の遅れ (負の到着余裕)。確定時点の代替入力は
                // 「まだ着いていない」としか言えないので、遅れの大きさはここでだけ分かる。
                // クライアントの追いつきが参加直後・停止後の何 tick もの遅れを測る唯一の手がかり。
                // 待たれていないレーン (参加直後で最初の入力がまだ有効になっていない) はサーバが待たずに確定したので、
                // 基準は締め切りではなく実際に確定した時刻
                const uint64_t confirmedAt = confirmMs_[tick % kNetHistoryTicks];
                if (tick + kNetHistoryTicks > nextTick_ && confirmedAt != 0) {
                    const uint64_t reference = (std::min)(DeadlineMs(tick), confirmedAt);
                    NoteMargin(p, static_cast<int64_t>(reference) - static_cast<int64_t>(nowMs));
                }
            }
            continue;
        }
        if (tick >= nextTick_ + kNetInputFutureWindow) {
            continue;
        }
        InputSlot& s = SlotOf(lane, tick);
        if (s.valid && s.tick == tick) {
            continue; // 一度決めた値は変えない (先着)
        }
        s.tick = tick;
        s.valid = true;
        s.arrivalMs = nowMs;
        std::memcpy(&s.in, body + i * sizeof(InputSnapshot), sizeof(InputSnapshot));
        if (p.inputsFrom == kNoTickMark) {
            p.inputsFrom = tick;
        }
    }
    // クライアントが主張する checkpoint。サーバは止まらない (1 人の不具合で試合を落とさない)
    if (h.confirmHash != 0) {
        uint64_t mine = 0;
        if (CheckpointHash(h.confirmTick, mine) && mine != h.confirmHash
            && p.lastDesyncReportTick != h.confirmTick) {
            p.lastDesyncReportTick = h.confirmTick;
            ++stats_.clientDesyncReports;
            MYE_LOG_ERROR("[server] peer %u (lane %d) reports a different hash at tick %llu "
                          "(server %016llX / client %016llX)",
                          p.key, p.lane, static_cast<unsigned long long>(h.confirmTick),
                          static_cast<unsigned long long>(mine),
                          static_cast<unsigned long long>(h.confirmHash));
        }
    }
}

void ServerSession::HandleSnapshotAck(Peer& p, const uint8_t* body, size_t bodySize)
{
    NetSnapshotAckPayload a;
    if (p.phase != Phase::Syncing || bodySize < sizeof(a)) {
        return;
    }
    std::memcpy(&a, body, sizeof(a));
    if (a.serial != p.serial) {
        return; // 前の送付への ack
    }
    p.gotAck = true;
    const uint32_t base = (std::min)(a.base, p.chunkCount);
    for (uint32_t i = 0; i < base; ++i) {
        p.chunkAcked[i] = 1;
    }
    for (uint32_t i = 0; i < kNetAckWindow; ++i) {
        const uint32_t idx = base + i;
        if (idx >= p.chunkCount) {
            break;
        }
        if ((a.mask[i / 64] >> (i % 64)) & 1u) {
            p.chunkAcked[idx] = 1;
        }
    }
    p.ackBase = (std::max)(p.ackBase, base);
    while (p.ackBase < p.chunkCount && p.chunkAcked[p.ackBase] != 0) {
        ++p.ackBase;
    }
    if (p.ackBase >= p.chunkCount) {
        // 受信完了。ここから確定 tick の配信を始める (tick meta.tick から)
        p.phase = Phase::Live;
        p.blob.reset();
        p.ackTick = p.sentCursor = p.meta.tick;
        p.lastAckAdvanceMs = lastNowMs_;
        MYE_LOG_INFO("[server] peer %u (lane %d) finished the snapshot download (tick %llu)", p.key,
                     p.lane, static_cast<unsigned long long>(p.meta.tick));
    }
}

void ServerSession::StartSnapshot(Peer& p, const std::shared_ptr<const std::vector<std::byte>>& blob,
                                  const SnapshotMeta& meta)
{
    p.blob = blob;
    p.meta = meta;
    p.serial = nextSerial_++;
    p.chunkCount = static_cast<uint32_t>((blob->size() + kNetSnapshotChunkBytes - 1) / kNetSnapshotChunkBytes);
    p.gotAck = false;
    p.lastWelcomeMs = 0;
    p.ackBase = 0;
    p.chunkAcked.assign(p.chunkCount, 0);
    p.chunkSentMs.assign(p.chunkCount, 0);
    p.phase = Phase::Syncing;
    p.resyncPending = false;
    p.ackTick = p.sentCursor = meta.tick;
    ++stats_.snapshotsSent;
}

bool ServerSession::WaitsOnLane(uint32_t lane, uint64_t tick)
{
    if (lanes_.lanes[lane].state != static_cast<uint32_t>(LaneState::Connected)) {
        return false;
    }
    const Peer* p = FindLivePeerOfLane(static_cast<int>(lane));
    return p != nullptr && p->inputsFrom != kNoTickMark && tick >= p->inputsFrom;
}

void ServerSession::QueueReleases(uint64_t tick)
{
    if (cfg_.session.rejoinTimeoutTicks == 0) {
        return; // 0 = 予約を解放しない
    }
    for (uint32_t l = 0; l < kMaxPlayers; ++l) {
        const LaneSlot& s = lanes_.lanes[l];
        if (s.state != static_cast<uint32_t>(LaneState::Reserved) || releaseQueued_[l]
            || tick < reservedSinceTick_[l] + cfg_.session.rejoinTimeoutTicks) {
            continue;
        }
        bool rejoinQueued = false;
        for (const SystemEvent& ev : pending_) {
            rejoinQueued = rejoinQueued
                || (ev.playerId == s.playerId && ev.kind == static_cast<uint8_t>(SystemEventKind::Rejoin));
        }
        if (rejoinQueued) {
            continue;
        }
        QueueEvent(SystemEventKind::Release, s.playerId, static_cast<uint8_t>(l));
        releaseQueued_[l] = true;
    }
}

void ServerSession::AfterApply(uint64_t tick)
{
    for (uint32_t i = 0; i < lanes_.appliedCount; ++i) {
        const SystemEvent ev = lanes_.applied[i];
        switch (static_cast<SystemEventKind>(ev.kind)) {
        case SystemEventKind::Join:
        case SystemEventKind::Rejoin: {
            Peer* p = FindPeerByEvent(ev.eventSeq);
            if (p != nullptr && p->phase != Phase::Gone) {
                p->lane = ev.lane;
                laneOwner_[ev.lane] = p->key;
                std::memcpy(laneSid_[ev.lane], p->sid, sizeof(laneSid_[ev.lane]));
                MYE_LOG_INFO("[server] tick %llu: player %llu %s lane %u (peer %u)",
                             static_cast<unsigned long long>(tick),
                             static_cast<unsigned long long>(ev.playerId),
                             ev.kind == static_cast<uint8_t>(SystemEventKind::Join) ? "joined" : "rejoined",
                             static_cast<unsigned>(ev.lane), p->key);
            } else {
                // 適用される前に相手が消えた。入れ物だけ空けるため、すぐ Leave を積む
                if (p != nullptr) {
                    p->lane = ev.lane;
                }
                QueueEvent(SystemEventKind::Leave, ev.playerId, ev.lane);
                if (p != nullptr) {
                    p->leaveQueued = true;
                }
            }
            releaseQueued_[ev.lane] = false;
            break;
        }
        case SystemEventKind::Leave:
            reservedSinceTick_[ev.lane] = tick;
            ++stats_.leaves;
            MYE_LOG_INFO("[server] tick %llu: player %llu left lane %u (reserved)",
                         static_cast<unsigned long long>(tick),
                         static_cast<unsigned long long>(ev.playerId), static_cast<unsigned>(ev.lane));
            break;
        case SystemEventKind::Release:
            if (laneSid_[ev.lane][0] != '\0' && hooks_.playerReleased) {
                hooks_.playerReleased(laneSid_[ev.lane]);
            }
            std::memset(laneSid_[ev.lane], 0, sizeof(laneSid_[ev.lane]));
            releaseQueued_[ev.lane] = false;
            ++stats_.releases;
            MYE_LOG_INFO("[server] tick %llu: lane %u released (player %llu)",
                         static_cast<unsigned long long>(tick), static_cast<unsigned>(ev.lane),
                         static_cast<unsigned long long>(ev.playerId));
            break;
        default:
            break;
        }
    }
    // 適用されたのにレーンが付かなかった Join / Rejoin (sim が無視した) は参加失敗
    for (Peer& p : peers_) {
        if ((p.phase != Phase::Pending && p.phase != Phase::Syncing) || p.eventSeq == 0
            || p.eventSeq > lanes_.lastEventSeq) {
            continue;
        }
        if (p.lane < 0) {
            SendReject(p.key, NetReject::ServerFull);
            p.phase = Phase::Gone;
        }
    }
}

bool ServerSession::TryConfirm(uint64_t nowMs, NetConfirmedTick& out)
{
    if (!inited_) {
        return false;
    }
    lastNowMs_ = (std::max)(lastNowMs_, nowMs);
    const uint64_t tick = nextTick_;
    // 予定時刻の inputDelay tick 前より早くは確定しない (クライアントの先行入力ぶんだけ前倒しを許す)。
    // レーンが 1 本も待たれていないとき (空のサーバ) に tick が暴走しないための歯止めでもある
    if (nowMs + NetTicksToMs(cfg_.session.inputDelay) < TickDueMs(tick)) {
        return false;
    }
    QueueReleases(tick);

    bool complete = true;
    for (uint32_t l = 0; l < kMaxPlayers && complete; ++l) {
        if (WaitsOnLane(l, tick)) {
            const InputSlot& s = SlotOf(l, tick);
            complete = s.valid && s.tick == tick;
        }
    }
    if (!complete && nowMs < DeadlineMs(tick)) {
        return false;
    }

    NetConfirmedTick ct;
    ct.tick = tick;
    for (uint32_t l = 0; l < kMaxPlayers; ++l) {
        if (lanes_.lanes[l].state == static_cast<uint32_t>(LaneState::Connected)) {
            const InputSlot& s = SlotOf(l, tick);
            Peer* p = FindLivePeerOfLane(static_cast<int>(l));
            if (WaitsOnLane(l, tick)) {
                ++stats_.laneWaitedTicks[l];
            }
            if (s.valid && s.tick == tick) {
                ct.inputs[l] = s.in;
                if (p != nullptr) {
                    NoteMargin(*p, static_cast<int64_t>(DeadlineMs(tick)) - static_cast<int64_t>(s.arrivalMs));
                }
            } else {
                // 締め切り超過 (または未稼働)。前 tick の確定入力から消費型を落とした値で埋める
                ct.inputs[l] = SubstituteLateInput(prevInput_[l]);
                if (WaitsOnLane(l, tick)) {
                    ++stats_.lateSubstitutions;
                    ++stats_.laneLateSubst[l];
                    // 到着余裕はここでは更新しない: 着いていない入力の遅れは分からない (HandleClientInput が
                    // 遅れて着いた時に測る)。ここで 0 付近の値を書くと、本当の遅れを毎 tick 上書きして隠してしまう
                }
            }
        }
        prevInput_[l] = ct.inputs[l];
    }

    // ---- システムイベント (発行順 = eventSeq 昇順、1 tick 最大 8 本。超過は次 tick へ) ----
    const uint32_t nEv = static_cast<uint32_t>((std::min<size_t>)(pending_.size(), kMaxSystemEventsPerTick));
    ct.sys.eventCount = nEv;
    for (uint32_t i = 0; i < nEv; ++i) {
        ct.sys.events[i] = pending_[i];
    }
    pending_.erase(pending_.begin(), pending_.begin() + nEv);

    // ---- スナップショット: この tick が走る前の状態を、参加 / 再接続 / 再同期の相手へ ----
    std::vector<Peer*> needSnap;
    for (Peer& p : peers_) {
        bool wants = false;
        if (p.phase == Phase::Pending) {
            for (uint32_t i = 0; i < nEv; ++i) {
                wants = wants || (ct.sys.events[i].eventSeq == p.eventSeq);
            }
        } else if ((p.phase == Phase::Live || p.phase == Phase::Syncing) && p.resyncPending) {
            wants = true;
        }
        if (wants) {
            needSnap.push_back(&p);
        }
    }
    if (!needSnap.empty()) {
        auto blob = std::make_shared<std::vector<std::byte>>();
        uint64_t worldHash = 0;
        const bool ok = hooks_.captureSnapshot && hooks_.captureSnapshot(*blob, worldHash);
        if (ok) {
            SnapshotMeta meta = {};
            meta.tick = tick;
            meta.worldHash = worldHash;
            meta.blobHash = HashBytes(blob->data(), blob->size());
            meta.lastEventSeq = lanes_.lastEventSeq;
            meta.config = cfg_.session;
            meta.provenance = cfg_.provenance;
            for (Peer* p : needSnap) {
                if (p->resyncPending) {
                    ++stats_.resyncsServed;
                }
                StartSnapshot(*p, blob, meta);
            }
        } else {
            MYE_LOG_ERROR("[server] could not capture the snapshot at tick %llu - dropping %zu peer(s)",
                          static_cast<unsigned long long>(tick), needSnap.size());
            for (Peer* p : needSnap) {
                DropPeerInternal(*p, "snapshot capture failed");
            }
        }
    }

    ApplySystemInput(lanes_, ct.sys);
    AfterApply(tick);

    history_[tick % kNetHistoryTicks] = ct;
    confirmMs_[tick % kNetHistoryTicks] = nowMs == 0 ? 1 : nowMs;
    ++nextTick_;
    ++stats_.confirmedTicks;
    out = ct;
    return true;
}

void ServerSession::OnTickRan(uint64_t tick, uint64_t hashAfter)
{
    if (tick % kNetHashCheckpoint == 0) {
        checkpoints_[(tick / kNetHashCheckpoint) % 64] = NetCheckpoint{ tick, hashAfter };
    }
}

void ServerSession::PumpSnapshot(Peer& p, uint64_t nowMs, uint32_t& budget)
{
    if (p.lane < 0 || !p.blob) {
        return; // レーンはイベント適用後に決まる
    }
    if (!p.gotAck && (p.lastWelcomeMs == 0 || nowMs - p.lastWelcomeMs >= cfg_.chunkResendMs)) {
        NetWelcomePayload w = {};
        w.meta = p.meta;
        w.playerId = p.playerId;
        w.lane = static_cast<uint32_t>(p.lane);
        w.snapshotBytes = static_cast<uint32_t>(p.blob->size());
        w.chunkCount = p.chunkCount;
        w.serial = p.serial;
        NetPacketHeader h = BaseHeader();
        h.playerIndex = w.lane;
        h.baseTick = p.meta.tick;
        h.sendTimeMs = static_cast<uint32_t>(nowMs);
        h.echoTimeMs = p.clientSendMs;
        Send(p.key, NetMsg::Welcome, h, &w, sizeof(w));
        p.lastWelcomeMs = nowMs == 0 ? 1 : nowMs;
    }
    const uint32_t end = (std::min)(p.chunkCount, p.ackBase + kNetAckWindow);
    for (uint32_t i = p.ackBase; i < end && budget > 0; ++i) {
        if (p.chunkAcked[i] != 0) {
            continue;
        }
        if (p.chunkSentMs[i] != 0 && nowMs - p.chunkSentMs[i] < cfg_.chunkResendMs) {
            continue;
        }
        NetSnapshotChunkPayload c = {};
        c.serial = p.serial;
        c.index = i;
        const size_t off = static_cast<size_t>(i) * kNetSnapshotChunkBytes;
        c.size = static_cast<uint32_t>((std::min<size_t>)(kNetSnapshotChunkBytes, p.blob->size() - off));
        NetPacketHeader h = BaseHeader();
        h.baseTick = p.meta.tick;
        h.sendTimeMs = static_cast<uint32_t>(nowMs);
        Send(p.key, NetMsg::SnapshotChunk, h, &c, sizeof(c), p.blob->data() + off, c.size);
        if (p.chunkSentMs[i] != 0) {
            ++stats_.chunkResends;
        }
        p.chunkSentMs[i] = nowMs == 0 ? 1 : nowMs;
        ++stats_.chunksSent;
        --budget;
    }
}

void ServerSession::PumpConfirmed(Peer& p, uint64_t nowMs)
{
    const uint64_t frontier = nextTick_;
    if (frontier - p.ackTick >= kNetHistoryTicks - 8) {
        // 履歴から溢れる (クライアントの ack が止まっている) — 再送では追いつけない
        if (!p.resyncPending) {
            ++stats_.forcedResyncs;
            MYE_LOG_WARN("[server] peer %u fell %llu ticks behind - forcing a resync", p.key,
                         static_cast<unsigned long long>(frontier - p.ackTick));
        }
        p.resyncPending = true;
        return;
    }
    // ack が止まっているのに送り済みがある = 取りこぼし。未 ack の最古から撒き直す
    if (p.ackTick < p.sentCursor && nowMs - p.lastAckAdvanceMs >= cfg_.confirmedResendMs) {
        p.sentCursor = p.ackTick;
        p.lastAckAdvanceMs = nowMs;
    }
    const uint64_t from = (std::max)(p.sentCursor, p.ackTick);
    const bool hasNew = from < frontier;
    if (!hasNew && nowMs - p.lastSendMs < cfg_.keepAliveMs) {
        return;
    }
    uint64_t t = from;
    if (hasNew) {
        t = from - (std::min<uint64_t>)(kNetConfirmedRedundancy, from - p.ackTick);
    } else {
        t = frontier; // keepalive: レコード無し
    }
    WarnIfUnreachable(p);
    NetConfirmedPayload pl = {};
    // 前回の送信から標本が 1 つでもあれば、その平均。無ければ「標本なし」(クライアントは古い値を何度も数えない)
    if (p.marginCount > 0) {
        pl.marginMs = static_cast<int32_t>(p.marginSum / static_cast<int64_t>(p.marginCount));
        pl.flags = kNetConfirmedFlagMarginValid | (MarginSigmaQuarterMs(p) << kNetConfirmedSigmaShift);
        p.marginSum = 0;
        p.marginCount = 0;
    }
    uint32_t cp = 0;
    const uint64_t latest = (frontier > 0) ? (frontier - 1) / kNetHashCheckpoint * kNetHashCheckpoint : 0;
    for (uint32_t k = 0; k < 8 && cp < kNetCheckpointsPerPacket; ++k) {
        if (latest < static_cast<uint64_t>(k) * kNetHashCheckpoint) {
            break;
        }
        const uint64_t ct = latest - k * kNetHashCheckpoint;
        const NetCheckpoint& c = checkpoints_[(ct / kNetHashCheckpoint) % 64];
        if (c.tick == ct && c.hash != 0) {
            pl.checkpoints[cp++] = c;
        }
    }
    uint32_t packets = 0;
    do {
        std::vector<uint8_t> recs;
        const uint64_t first = t;
        uint32_t count = 0;
        while (t < frontier) {
            const NetConfirmedTick& rec = history_[t % kNetHistoryTicks];
            if (rec.tick != t) {
                break; // 上書き済み (上の履歴検査で弾いているはず)
            }
            if (!recs.empty() && recs.size() + NetTickRecordBytes(rec) > kNetConfirmedBudget) {
                break;
            }
            NetWriteTickRecord(rec, recs);
            ++t;
            ++count;
        }
        NetPacketHeader h = BaseHeader();
        h.playerIndex = p.lane >= 0 ? static_cast<uint32_t>(p.lane) : 0;
        h.count = count;
        h.baseTick = first;
        h.lastAckTick = frontier;
        h.sendTimeMs = static_cast<uint32_t>(nowMs);
        h.echoTimeMs = p.clientSendMs;
        Send(p.key, NetMsg::Confirmed, h, &pl, sizeof(pl), recs.data(), recs.size());
        ++packets;
    } while (t < frontier && packets < 4);
    p.sentCursor = (std::max)(p.sentCursor, t);
    p.lastSendMs = nowMs;
}

void ServerSession::Pump(uint64_t nowMs)
{
    if (!inited_) {
        return;
    }
    lastNowMs_ = (std::max)(lastNowMs_, nowMs);
    for (Peer& p : peers_) {
        if (p.phase != Phase::Gone && nowMs > p.lastRecvMs && nowMs - p.lastRecvMs > cfg_.peerTimeoutMs) {
            DropPeerInternal(p, "timeout");
        }
    }
    uint32_t budget = cfg_.maxChunksPerPump;
    const size_t n = peers_.size();
    for (size_t k = 0; k < n; ++k) {
        Peer& p = peers_[(chunkRover_ + k) % n];
        if (p.phase == Phase::Syncing) {
            PumpSnapshot(p, nowMs, budget);
        }
    }
    ++chunkRover_;
    for (Peer& p : peers_) {
        if (p.phase == Phase::Live) {
            PumpConfirmed(p, nowMs);
        }
    }
    peers_.erase(std::remove_if(peers_.begin(), peers_.end(),
                                [](const Peer& p) { return p.phase == Phase::Gone; }),
                 peers_.end());
}

} // namespace mye
