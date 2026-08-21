#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "Engine/Core/Random.h"
#include "Engine/Platform/Input.h"
#include "Engine/Platform/Net/UdpSocket.h"

namespace mye {

// 遅延ロックステップのネットセッション (M52h、決定台帳 5)。
//
// 役割はただ 1 つ: **「tick T が消費する入力レーンを全 peer 分そろえること」**。
// そろうまで tick を進めない (stall) のが遅延ロックステップで、予測して先へ進むのは M52i。
//
// ★ネット層は sim 状態を 1 バイトも書かない。「いつ tick が回るか」は実時間依存でよいが
//   「tick が何を消費するか」は確定入力だけで決まる — この分離が保てている限り、2 台の
//   .rep は**バイト一致する**。tools\net_verify.bat はそれを機械検証する。
//
// ★各レーンの値は「そのレーンを持つ peer だけが決め、一度決めたら二度と変えない」。
//   同じ tick へ 2 回違う値を送ると、受信側が先に消費した値と送信側の値が食い違って
//   即 desync する。SubmitLocalInput は tick ごとに 1 回だけ呼ぶこと (EngineLoop は
//   「tick t を回す直前に t + inputDelay を確定させる」形で 1 回を保証している)。
//
// 再送機構は持たない。**直近 kNetRedundancy tick 分を毎回まるごと送り直す**ことで
// ロスを吸収する (ロスに強く、順序も重複も気にしなくてよい)。

inline constexpr uint32_t kNetProtoVersion = 1;
inline constexpr uint32_t kNetMagic = 0x4E45594Du; // 'MYEN'
inline constexpr uint32_t kNetRedundancy = 8;  // 1 パケットに載せる直近 tick 数
inline constexpr uint32_t kNetRingTicks = 512; // 入力リングの深さ (tick)
inline constexpr uint32_t kNetKeepAliveMs = 50;

enum class NetRole : int { None = 0, Host = 1, Join = 2 };

enum class NetState : int {
    Idle,       // 未開始
    Connecting, // ハンドシェイク中
    Running,    // 入力交換中
    Failed,     // 接続拒否 / タイムアウト (呼び出し側は異常終了させる)
    Closed,     // 正常終了
};

struct NetConfig {
    NetRole role = NetRole::None;
    uint16_t port = 7777;      // --net-host PORT (ホスト側の待受ポート)
    std::wstring joinTarget;   // --net-join HOST:PORT
    uint32_t playerCount = 2;  // M52 は 2 人 P2P 固定 (3 人以上は M53 候補)
    uint32_t inputDelay = 3;   // --net-delay N (tick)
    uint32_t lossPercent = 0;  // --net-loss N (入力パケットを故意に捨てる割合)
    uint32_t connectTimeoutMs = 30000;
    uint32_t stallTimeoutMs = 20000; // この間 1 パケットも来なければ切断とみなす
};

// 接続前に照合する「同じものを走らせているか」の指紋 (desync の最大要因を入口で潰す)。
// **POD のままパケットに載る** — バイト順は無変換 (同一アーキテクチャ間の P2P 前提)
struct NetIdentity {
    uint32_t proto = kNetProtoVersion;
    uint32_t apiVersion = 0;      // MYE_API_VERSION
    uint32_t repVersion = 0;      // ReplayFile の版
    uint32_t snapshotVersion = 0; // SimSnapshot blob の版
    uint32_t playerCount = 0;
    uint32_t inputDelay = 0;
    uint32_t configBits = 0;      // 決定論に効く起動オプション (NetConfigBits)
    uint32_t pad = 0;
    uint64_t startWorldHash = 0;  // 開始時点のワールドハッシュ (= 同じシーンか)
};
static_assert(sizeof(NetIdentity) == 40, "NetIdentity is part of the wire format");

// configBits の内訳。**「ビット同一のはず」と分かっているものも入れる** —
// 分かっているのは検証済みの構成だけで、食い違ったまま何時間も desync を追うより
// 入口で弾くほうが安い
enum NetConfigBits : uint32_t {
    kNetCfgSynthInput = 1u << 0,
    kNetCfgJobs = 1u << 1,
    kNetCfgSimCache = 1u << 2,
    kNetCfgCookCache = 1u << 3,
};

// 拒否理由 (ログに出す。参加側が「何が違うのか」を 1 行で分かるように)
enum class NetReject : uint32_t {
    None = 0,
    Proto,
    ApiVersion,
    RepVersion,
    SnapshotVersion,
    PlayerCount,
    InputDelay,
    ConfigBits,
    WorldHash,
    Busy, // 既に別の相手と繋がっている
};

const char* NetRejectName(NetReject r);

// 指紋の照合。**最初に食い違ったものを返す** (全部並べるより原因が 1 行で読める)
NetReject CompareNetIdentity(const NetIdentity& a, const NetIdentity& b);

// ---- パケット (リトルエンディアン固定・無変換) ----
enum class NetMsg : uint16_t { Join = 1, Accept = 2, Reject = 3, Input = 4, Bye = 5 };

struct NetPacketHeader {
    uint32_t magic = kNetMagic;
    uint16_t proto = static_cast<uint16_t>(kNetProtoVersion);
    uint16_t type = 0;            // NetMsg
    uint64_t sessionId = 0;       // ホストが採番。前回実行の残党パケットを弾く
    uint32_t playerIndex = 0;     // 送信者のレーン
    uint32_t count = 0;           // 後続 InputSnapshot の本数 (制御メッセージは 0)
    uint64_t baseTick = 0;        // 後続 InputSnapshot[0] の tick
    uint64_t lastAckTick = 0;     // 送信者が次に消費する tick (診断用: どこで詰まっているか)
    uint32_t sendTimeMs = 0;      // 送信者のセッション内経過 ms
    uint32_t echoTimeMs = 0;      // 直前に受け取った相手の sendTimeMs (RTT 計測)
};
static_assert(sizeof(NetPacketHeader) == 48, "NetPacketHeader is part of the wire format");

// Join / Accept / Reject の本体
struct NetHandshakePayload {
    NetIdentity id;
    uint32_t assignedIndex = 0; // Accept: 受信者が使うレーン
    uint32_t reason = 0;        // Reject: NetReject
};
static_assert(sizeof(NetHandshakePayload) == 48, "NetHandshakePayload is part of the wire format");

// パケット 1 個の最大長 (ヘッダ + 冗長分の入力)
inline constexpr size_t kNetMaxPacket =
    sizeof(NetPacketHeader) + kNetRedundancy * sizeof(InputSnapshot);

class NetSession {
public:
    NetSession() = default;
    ~NetSession();
    NetSession(const NetSession&) = delete;
    NetSession& operator=(const NetSession&) = delete;

    // ソケットを開き Connecting へ。startTick = このセッションが最初に回す tick
    bool Start(const NetConfig& cfg, const NetIdentity& localId, uint64_t startTick);
    // ハンドシェイク完了まで回す。pump は「ウィンドウメッセージを流す」フック
    // (false を返したらユーザーが閉じたとみなして中断)。空でも可
    bool WaitUntilReady(const std::function<bool()>& pump);

    void Poll();                    // 受信処理 + キープアライブ (毎フレーム呼ぶ)
    bool HasInputs(uint64_t tick) const;
    // 足りなければ maxWaitMs だけ受信を回して待つ (フレームのカクつきを減らすためだけの
    // もので、sim には一切影響しない)
    bool WaitForInputs(uint64_t tick, uint32_t maxWaitMs);
    // kMaxPlayers 本ぶんの連続領域。HasInputs が true のときだけ意味がある
    const InputSnapshot* InputsFor(uint64_t tick) const;
    // 自レーンの tick 入力を確定して送る。**同じ tick へ 2 回呼ばないこと**
    void SubmitLocalInput(uint64_t tick, const InputSnapshot& in);
    void OnTickConsumed(uint64_t tick); // リングの下限を進める
    void Finish();                      // 終了時: 最後の入力を冗長送信して Bye
    void Close();

    NetState State() const { return state_; }
    bool Running() const { return state_ == NetState::Running; }
    NetRole Role() const { return cfg_.role; }
    uint32_t LocalPlayerIndex() const { return localIndex_; }
    uint32_t PlayerCount() const { return cfg_.playerCount; }
    uint32_t InputDelay() const { return cfg_.inputDelay; }
    uint16_t LocalPort() const { return socket_.LocalPort(); }
    float PingMs() const { return pingMs_; }
    NetReject RejectReason() const { return reject_; }
    bool PeerFinished() const { return peerSaidBye_; } // 相手が Bye を送って正常終了した

    // 統計 (診断ログ / M52i の NetWindow 用)
    uint64_t PacketsSent() const { return packetsSent_; }
    uint64_t PacketsRecv() const { return packetsRecv_; }
    uint64_t PacketsDropped() const { return packetsDropped_; } // --net-loss で捨てた分
    uint64_t StallCount() const { return stallCount_; }
    double StallMs() const { return stallMs_; }
    void NoteStall(double ms);

private:
    uint32_t NowMs() const;
    void SendTo(NetMsg type, const void* payload, size_t payloadSize, uint32_t count,
                uint64_t baseTick, bool lossy);
    void SendHandshake(NetMsg type, NetReject reason, uint32_t assignedIndex);
    void SendInputPacket(uint64_t upToTick);
    void HandlePacket(const NetAddress& from, const uint8_t* data, int size);
    void StoreInput(uint64_t tick, uint32_t player, const InputSnapshot& in);
    size_t Slot(uint64_t tick) const
    {
        return static_cast<size_t>(tick % kNetRingTicks) * kMaxPlayers;
    }

    NetConfig cfg_;
    NetIdentity localId_;
    UdpSocket socket_;
    NetAddress peer_;
    NetState state_ = NetState::Idle;
    NetReject reject_ = NetReject::None;
    uint64_t sessionId_ = 0;
    uint32_t localIndex_ = 0;
    uint64_t startTick_ = 0;
    uint64_t nextNeededTick_ = 0; // これ未満の tick は受け取っても捨てる (リング保護)
    uint64_t lastSubmitted_ = 0;
    bool hasSubmitted_ = false;
    bool peerSaidBye_ = false;

    std::vector<InputSnapshot> ring_; // [tick % kNetRingTicks][player]
    std::vector<uint64_t> stamp_;     // 同じ添字。入っている tick 番号

    Pcg32 lossRng_;
    uint32_t startMs_ = 0;
    uint32_t lastRecvMs_ = 0;
    uint32_t lastSendMs_ = 0;
    uint32_t lastJoinMs_ = 0;
    uint32_t peerSendTimeMs_ = 0;
    float pingMs_ = 0.0f;
    uint64_t packetsSent_ = 0;
    uint64_t packetsRecv_ = 0;
    uint64_t packetsDropped_ = 0;
    uint64_t stallCount_ = 0;
    double stallMs_ = 0.0;
};

} // namespace mye
