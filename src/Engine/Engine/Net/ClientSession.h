//====================================================================================
//                          ClientSession.h
//  MyEngin/ 秋田蓮音                                                     10/02/2026
//                                          入力確定型サーバへ繋ぐクライアントのセッション
//====================================================================================
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "Engine/Engine/Net/NetProtocol.h"
#include "Engine/Engine/Session/SessionTypes.h"

namespace mye {

// 入力確定型サーバへ繋ぐクライアントのセッション (M81d)。
//
// 役割: ハンドシェイク、スナップショットの受信、自レーン入力の送信、サーバの確定 tick の受信と保持。
// ★ここは sim に触らない。sim を回す側 (ClientSimRunner / EngineLoop) が
//   「確定 tick があればそれ、無ければ予測」で入力を組み、ロールバックする。
// サーバと同じく時計もソケットも内部で読まない (nowMs と ClientSendFn を呼び出し側が渡す)。

using ClientSendFn = std::function<void(const void* data, size_t size)>;

struct ClientSessionConfig {
    SimProvenance provenance = {};
    uint32_t configBits = 0;
    uint32_t referenceW = 0;
    uint32_t referenceH = 0;
    uint64_t fontMetricsHash = 0;
    std::string playerSessionId;
    uint64_t playerId = 0;          // 再接続のときだけ (前回の Welcome の値)
    bool allowGameMismatch = false;
    uint32_t helloRetryMs = 200;
    uint32_t connectTimeoutMs = 10000;
    uint32_t serverTimeoutMs = 3000;
    uint32_t ackIntervalMs = 30;    // スナップショット受信中の SnapshotAck 間隔
    uint32_t keepAliveMs = 50;
    uint32_t targetMarginMs = 16;   // 到着余裕の目標 (1 tick ぶん)
    uint32_t maxSnapshotAttempts = 3;
};

enum class ClientState : int {
    Idle,
    Connecting,    // Hello 送信中
    Downloading,   // スナップショット受信中 (再同期の待ちも含む)
    SnapshotReady, // 受信完了。呼び出し側が復元して OnSnapshotApplied を呼ぶ
    Running,
    Failed,
    Closed,
};

struct ClientStats {
    uint64_t resyncs = 0;           // 再同期を要求した回数
    uint64_t eventGaps = 0;
    uint64_t snapshotsApplied = 0;
    uint64_t packetsIn = 0, packetsOut = 0, badPackets = 0;
    uint64_t duplicateRecords = 0;
};

class ClientSession {
public:
    void Start(const ClientSessionConfig& cfg, ClientSendFn send, uint64_t nowMs);
    void OnPacket(const void* data, size_t size, uint64_t nowMs);
    void Poll(uint64_t nowMs);

    ClientState State() const { return state_; }
    bool Running() const { return state_ == ClientState::Running; }
    NetReject RejectReason() const { return reject_; }
    const std::string& FailReason() const { return failReason_; }

    // ---- スナップショット (State() == SnapshotReady のとき) ----
    const std::vector<std::byte>& SnapshotBlob() const { return blob_; }
    const SnapshotMeta& SnapshotMetaOf() const { return meta_; }
    // 復元して HashWorld が meta.worldHash と一致したか。一致 → Running (meta.tick から)。
    // 不一致 → 再要求 (maxSnapshotAttempts 回まで。超えたら Failed)
    void OnSnapshotApplied(bool worldHashMatches, uint64_t nowMs);

    // ---- Running ----
    uint32_t Lane() const { return lane_; }
    uint64_t PlayerId() const { return playerId_; }
    const SessionConfig& Config() const { return config_; }
    uint64_t StartTick() const { return meta_.tick; }
    uint32_t InputDelay() const { return config_.inputDelay; }
    uint32_t PlayerCount() const { return config_.playerCount; }

    // 連続して受信済みの次の tick (これ未満は全部 Confirmed で取れる)
    uint64_t ConfirmedFrontier() const { return frontier_; }
    bool HasConfirmed(uint64_t tick) const { return tick >= meta_.tick && tick < frontier_ && IsStored(tick); }
    const NetConfirmedTick* Confirmed(uint64_t tick) const;
    // 未確定 tick の他レーンの予測 = 確定済みの最新値の繰り返し (PredictLaneInput: 文字と wheelDelta は 0、
    // mouseDelta は繰り返す)。サーバの代替入力 (SubstituteLateInput) とは別。確定が 1 本も無ければゼロ入力
    InputSnapshot PredictLane(uint64_t tick, uint32_t lane) const;

    // 自レーンの tick 入力を確定させて送る。**同じ tick へ 2 回呼ばないこと** (一度決めた値は変えない)
    void SubmitLocalInput(uint64_t tick, const InputSnapshot& in, uint64_t nowMs);
    bool LocalInput(uint64_t tick, InputSnapshot& out) const;

    // 確定して消費した tick (受信リングの下限を進める)
    void OnTickConsumed(uint64_t tick);
    // 自分の確定 checkpoint (以後のパケットに載る)
    void SetLocalConfirmed(uint64_t tick, uint64_t hash);
    // サーバが主張した checkpoint (届いていなければ false)
    bool ServerCheckpointHash(uint64_t tick, uint64_t& outHash) const;

    // 再同期を要求する。State() は Downloading へ戻り、新しい Welcome を待つ
    void RequestResync(NetResyncReason reason, uint64_t nowMs);
    void Close(uint64_t nowMs); // Bye を送って閉じる

    // ---- 時刻同期 (サーバの到着余裕 → tick の進め方) ----
    // accumulator への加算係数。到着余裕が目標より小さい (入力が遅い) と速く、大きいと遅くする。±2% 上限。
    // ★「いつ tick が回るか」だけを変える。sim には入らない
    double SpeedFactor() const;
    // クライアントが「遅れすぎ」なとき、追いつくために余分に回すべき tick 数 (0 以下 = 遅れていない)。
    // 参加直後 / 再同期直後に生の速度係数 2% では何秒もかかる差を埋める
    int64_t CatchUpTicks(uint64_t clientTick) const;
    double MarginMs() const { return marginMs_; }
    double RttMs() const { return rttMs_; }
    uint64_t ServerFrontier() const { return serverFrontier_; }

    const ClientStats& Stats() const { return stats_; }

private:
    bool IsStored(uint64_t tick) const { return stamp_[tick % kNetHistoryTicks] == tick + 1; }
    void OnWelcome(const NetPacketHeader& h, const uint8_t* body, size_t bodySize, uint64_t nowMs);
    void OnChunk(const uint8_t* body, size_t bodySize, uint64_t nowMs);
    void OnConfirmed(const NetPacketHeader& h, const uint8_t* body, size_t bodySize, uint64_t nowMs);
    void SendHello(uint64_t nowMs);
    void SendAck(uint64_t nowMs);
    void SendInputPacket(uint64_t upToTick, uint64_t nowMs);
    void SendHeaderOnly(NetMsg type, const void* payload, size_t payloadSize, uint64_t nowMs);
    NetPacketHeader BaseHeader(uint64_t nowMs) const;
    void Send(NetMsg type, const NetPacketHeader& h, const void* payload, size_t payloadSize,
              const void* tail = nullptr, size_t tailSize = 0);
    void Fail(const char* why, NetReject reason = NetReject::None);
    void ResetStream(uint64_t fromTick);
    void AdvanceFrontier(uint64_t nowMs);

    ClientSessionConfig cfg_;
    ClientSendFn send_;
    ClientState state_ = ClientState::Idle;
    NetReject reject_ = NetReject::None;
    std::string failReason_;
    ClientStats stats_;

    uint64_t sessionId_ = 0;
    uint32_t lane_ = 0;
    uint64_t playerId_ = 0;
    SessionConfig config_ = {};

    // スナップショット受信
    SnapshotMeta meta_ = {};
    std::vector<std::byte> blob_;
    uint32_t serial_ = 0;
    uint32_t chunkCount_ = 0;
    std::vector<uint8_t> chunkGot_;
    uint32_t recvBase_ = 0;
    uint32_t attempts_ = 0;
    bool awaitingWelcome_ = false; // 再同期を要求して新しい Welcome を待っている
    bool waitingFirstConfirmed_ = false;

    // 確定 tick の受信リング
    std::vector<NetConfirmedTick> conf_;
    std::vector<uint64_t> stamp_; // tick + 1 (0 = 空き)
    uint64_t frontier_ = 0;
    uint64_t consumedFloor_ = 0;
    uint64_t lastSeenEventSeq_ = 0;
    NetCheckpoint svCp_[64] = {};

    // 自レーンの入力
    std::vector<InputSnapshot> local_;
    std::vector<uint64_t> localStamp_;
    uint64_t newestLocal_ = 0;
    bool hasLocal_ = false;

    uint64_t localConfirmTick_ = 0;
    uint64_t localConfirmHash_ = 0;

    // 時刻同期
    double marginMs_ = 0.0;
    bool marginValid_ = false;
    double rttMs_ = 0.0;
    uint64_t serverFrontier_ = 0;

    uint64_t now_ = 0; // 直近に渡された nowMs (送信時刻の記録用)
    uint64_t startMs_ = 0;
    uint64_t lastHelloMs_ = 0;
    uint64_t lastSendMs_ = 0;
    uint64_t lastRecvMs_ = 0;
    uint64_t lastAckMs_ = 0;
    uint64_t lastResyncReqMs_ = 0;
    NetResyncReason resyncReason_ = NetResyncReason::None;
    std::vector<uint8_t> scratch_;
};

} // namespace mye
