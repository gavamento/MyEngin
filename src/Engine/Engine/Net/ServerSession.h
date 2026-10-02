//====================================================================================
//                          ServerSession.h
//  MyEngin/ 秋田蓮音                                                     10/02/2026
//                                          入力確定型サーバのセッション (確定入力の決定・配信・途中参加・再接続)
//====================================================================================
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Engine/Engine/Net/NetProtocol.h"
#include "Engine/Engine/Session/SessionTypes.h"

namespace mye {

// 入力確定型サーバのセッション (M81d、spec 4.1)。
//
// 役割は「各 tick が消費する確定入力 (レーン入力 + SystemInputTick) を決めて配る」ことだけ。
// ★サーバは投機せず巻き戻さない。決めた値は一度も変えない。
// ★sim へ入る値は TryConfirm が返す NetConfirmedTick の 1 本だけ。実時間・到着順・Hosting の都合は
//   「どの tick に何を入れたか」(= 入力の値とイベント列) に変換されてここで尽きる。
//   それ以外の経路 (ctx / World / Scene) には一切書かない。
//
// 1 プロセス内の偽トランスポートでも実 UDP でも同じコードで回るよう、時計もソケットも内部で読まない:
//   時刻 … 呼び出し側が nowMs (単調増加 ms) を渡す
//   送信 … ServerHooks::send (peer は不透明なキー)
//   受信 … OnPacket へ push
// 呼び出し側の 1 サイクル (spec 4.1.4 の順序):
//   OnPacket*(受信キュー) → TryConfirm → [sim を回す] → OnTickRan → (.rep へ記録) → Pump
// 確定した tick は記録してから外へ出す。Confirmed / Welcome / チャンクの送信は Pump だけが行う。

// 締め切り (サーバが確定を待つ tick 数) と、切断レーンの予約期間 (tick) の既定値。
// 締め切りは「遅い入力を待つ長さ」で、長いほど遅い 1 人が全員の確定を遅らせる (遅れは締め切りまでで頭打ち)。
// 予約期間は切断から Release までで、再接続の猶予
inline constexpr uint32_t kServerDefaultDeadlineTicks = 3;
inline constexpr uint32_t kServerDefaultRejoinTimeoutTicks = 60 * 30;

// 専用サーバの SessionConfig の既定値 (role = Server、tickRate = 60)。残りの項目は呼び出し側が埋める
inline SessionConfig DefaultServerSessionConfig(uint32_t playerCount, uint32_t inputDelay)
{
    SessionConfig c = {};
    c.role = static_cast<uint32_t>(SessionRole::Server);
    c.playerCount = playerCount;
    c.tickRate = 60;
    c.inputDelay = inputDelay;
    c.deadlineTicks = kServerDefaultDeadlineTicks;
    c.rejoinTimeoutTicks = kServerDefaultRejoinTimeoutTicks;
    return c;
}

struct ServerSessionConfig {
    SessionConfig session = {};  // role = Server、playerCount = 最大人数 (1..kMaxPlayers)
    SimProvenance provenance = {};
    uint64_t sessionId = 1;
    uint64_t startTick = 0;      // 最初に確定する tick
    uint64_t startMs = 0;        // startTick の予定時刻 (予定時刻 = startMs + (tick - startTick) / 60Hz)
    uint32_t peerTimeoutMs = 3000;
    uint32_t maxChunksPerPump = 32;   // スナップショットのチャンクを 1 回の Pump で送る上限
    uint32_t chunkResendMs = 100;
    uint32_t confirmedResendMs = 100; // ack が進まないとき、未 ack の最古の tick から撒き直す間隔
    uint32_t keepAliveMs = 100;
};

struct ServerHooks {
    NetSendFn send;
    // 参加の検証 (player session ID)。null は常に通す
    std::function<bool(const char* playerSessionId)> validatePlayer;
    // 参加者の離脱通知 (ホスティングが管理する player session の解放など)。null 可
    std::function<void(const char* playerSessionId)> playerLeft;
    // レーンの予約が解放された (Release イベントの適用時 = 切断確定)。ホスティングの player session の片付け用。null 可
    std::function<void(const char* playerSessionId)> playerReleased;
    // いまの sim 状態 (= 次に確定する tick が走る前) を撮り、その時のワールドハッシュを返す。
    // 呼ばれるのは TryConfirm の中だけで、直前の tick を走らせ終えている
    std::function<bool(std::vector<std::byte>& blob, uint64_t& worldHash)> captureSnapshot;
};

struct ServerStats {
    uint64_t joins = 0, rejoins = 0, leaves = 0, releases = 0, rejects = 0;
    uint64_t confirmedTicks = 0;
    uint64_t lateSubstitutions = 0; // 締め切り超過で代替入力にした (レーン, tick) の数
    uint64_t lateInputsDropped = 0; // 確定済みの tick に間に合わなかった入力 (冗長送信の重複は数えない)
    uint64_t snapshotsSent = 0, resyncsServed = 0;
    uint64_t chunksSent = 0, chunkResends = 0;
    uint64_t clientDesyncReports = 0; // クライアントが主張した checkpoint がサーバの値と違った
    uint64_t packetsIn = 0, packetsOut = 0, badPackets = 0;
};

class ServerSession {
public:
    bool Init(const ServerSessionConfig& cfg, const ServerHooks& hooks);

    void OnPacket(uint32_t peer, const void* data, size_t size, uint64_t nowMs);

    // 次の tick を確定できるなら out を埋めて true。条件: (全 Connected レーンの入力が届いた または
    // 締め切りを過ぎた) かつ 予定時刻の inputDelay tick 前を過ぎている。
    // true を返したら、呼び出し側は out を sim へ渡して tick を回し、OnTickRan を呼ぶこと
    bool TryConfirm(uint64_t nowMs, NetConfirmedTick& out);
    // TryConfirm した tick を走らせ終えた。tick 末のワールドハッシュを渡す (checkpoint の配信元)
    void OnTickRan(uint64_t tick, uint64_t hashAfter);

    // 送信 (Welcome / チャンク / Confirmed / keepalive) と peer のタイムアウト検出
    void Pump(uint64_t nowMs);

    // トランスポートが相手を失った (切断を知った)。Leave を発行する
    void DropPeer(uint32_t peer);

    uint64_t NextTick() const { return nextTick_; }
    uint64_t TickDueMs(uint64_t tick) const;
    uint64_t DeadlineMs(uint64_t tick) const;
    const SessionLanes& Lanes() const { return lanes_; }
    const ServerStats& Stats() const { return stats_; }
    uint32_t LivePeerCount() const;
    // 切断済み (Gone) 以外の peer の数 (接続処理中を含む)。「全員が出ていった」判定用
    uint32_t ActivePeerCount() const;
    // この tick までの checkpoint ハッシュ (ログ / selftest)
    bool CheckpointHash(uint64_t tick, uint64_t& outHash) const;

    // selftest 専用。本番 (Server.exe) では呼ばない: eventSeq を n 欠番にして、クライアントの欠番検出を試す
    void TestSkipEventSeq(uint32_t n) { eventSeq_ += n; }

private:
    enum class Phase : uint8_t { Pending, Syncing, Live, Gone };

    struct Peer {
        uint32_t key = 0;
        Phase phase = Phase::Pending;
        uint64_t playerId = 0;
        uint64_t eventSeq = 0;   // この peer の Join / Rejoin イベント
        int lane = -1;
        char sid[kNetPlayerSessionIdLen + 1] = {};
        uint64_t lastRecvMs = 0;
        uint32_t clientSendMs = 0; // 直近に受け取ったクライアントの sendTimeMs (RTT のエコー用)
        bool leaveQueued = false;
        bool resyncPending = false;
        bool leftNotified = false;
        // ---- スナップショット送付 ----
        std::shared_ptr<const std::vector<std::byte>> blob;
        SnapshotMeta meta = {};
        uint32_t serial = 0;
        uint32_t chunkCount = 0;
        bool gotAck = false;
        uint64_t lastWelcomeMs = 0;
        uint32_t ackBase = 0;
        std::vector<uint8_t> chunkAcked;
        std::vector<uint64_t> chunkSentMs; // 0 = 未送信
        // ---- Confirmed の配信 ----
        uint64_t ackTick = 0;     // クライアントが次に必要とする確定 tick
        uint64_t sentCursor = 0;  // 送ったところまで (次に送る新しい tick)
        uint64_t lastSendMs = 0;
        uint64_t lastAckAdvanceMs = 0;
        // ---- 入力 ----
        uint64_t inputsFrom = ~0ull; // この tick 以降はレーンの入力を待つ (最初に届いた有効な入力の tick)
        int32_t marginMs = 0;
        bool marginValid = false;
        uint64_t lastDesyncReportTick = ~0ull; // 同じ checkpoint の不一致を何度も数えない
    };

    struct InputSlot {
        uint64_t tick = 0;
        bool valid = false;
        uint64_t arrivalMs = 0;
        InputSnapshot in = {};
    };

    static constexpr uint32_t kInputRing = 256;

    Peer* FindPeer(uint32_t key);
    Peer* FindPeerByEvent(uint64_t eventSeq);
    Peer* FindLivePeerOfLane(int lane);
    Peer* FindOwnerOfLane(int lane);
    void HandleHello(uint32_t key, const NetPacketHeader& h, const uint8_t* body, size_t bodySize,
                     uint64_t nowMs);
    void HandleClientInput(Peer& p, const NetPacketHeader& h, const uint8_t* body, size_t bodySize,
                           uint64_t nowMs);
    void HandleSnapshotAck(Peer& p, const uint8_t* body, size_t bodySize);
    void SendReject(uint32_t key, NetReject reason);
    void Send(uint32_t key, NetMsg type, const NetPacketHeader& base, const void* payload,
              size_t payloadSize, const void* tail = nullptr, size_t tailSize = 0);
    NetPacketHeader BaseHeader() const;
    uint64_t QueueEvent(SystemEventKind kind, uint64_t playerId, uint8_t lane);
    void DropPeerInternal(Peer& p, const char* why);
    void QueueReleases(uint64_t tick);
    bool WaitsOnLane(uint32_t lane, uint64_t tick);
    void AfterApply(uint64_t tick);
    void StartSnapshot(Peer& p, const std::shared_ptr<const std::vector<std::byte>>& blob,
                       const SnapshotMeta& meta);
    void PumpSnapshot(Peer& p, uint64_t nowMs, uint32_t& budget);
    void PumpConfirmed(Peer& p, uint64_t nowMs);
    uint32_t FreeLaneCount() const;
    InputSlot& SlotOf(uint32_t lane, uint64_t tick) { return laneInputs_[lane][tick % kInputRing]; }

    ServerSessionConfig cfg_;
    ServerHooks hooks_;
    ServerStats stats_;
    bool inited_ = false;
    uint64_t nextTick_ = 0;
    uint64_t eventSeq_ = 0;
    SessionLanes lanes_ = {};   // sim の SessionLanes のミラー (同じ純関数 ApplySystemInput で更新)
    std::vector<SystemEvent> pending_; // eventSeq 昇順 (発行順)
    std::vector<Peer> peers_;
    std::vector<NetConfirmedTick> history_; // [tick % kNetHistoryTicks]
    InputSlot laneInputs_[kMaxPlayers][kInputRing];
    InputSnapshot prevInput_[kMaxPlayers] = {};
    char laneSid_[kMaxPlayers][kNetPlayerSessionIdLen + 1] = {};
    uint64_t reservedSinceTick_[kMaxPlayers] = {};
    uint32_t laneOwner_[kMaxPlayers] = {}; // レーンを今使っている peer のキー (Join / Rejoin の適用で更新)
    bool releaseQueued_[kMaxPlayers] = {};
    NetCheckpoint checkpoints_[64] = {};
    uint32_t nextSerial_ = 1;
    uint32_t chunkRover_ = 0;
    uint64_t lastNowMs_ = 0;
    std::vector<uint8_t> scratch_;
};

} // namespace mye
