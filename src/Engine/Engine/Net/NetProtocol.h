//====================================================================================
//                          NetProtocol.h
//  MyEngin/ 秋田蓮音                                                     10/02/2026
//                                          入力確定型サーバ構成のパケット本体とトランスポートの型
//====================================================================================
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <type_traits>
#include <vector>

#include "Engine/Engine/Net/NetSession.h"

namespace mye {

// 入力確定型の専用サーバ構成 (M81d) のパケット本体。ヘッダは P2P と同じ NetPacketHeader (64B) を流用し、
// 本体を NetMsg ごとに足す。リトルエンディアン固定・無変換 (同一アーキテクチャ前提、P2P と同じ)。
//
// 信頼性チャネルは SnapshotChunk / SnapshotAck だけ (欠落分のみ再送)。残りは
//   ClientInput … 直近 kNetRedundancy tick の入力を毎回載せる
//   Confirmed   … 相手の ack 以降の確定 tick を未 ack ぶん載せ続ける
// ことで、ロス・重複・並べ替えに強くしてある (どれも同じ確定値の冗長送信)。
//
// ヘッダの流用:
//   sessionId   サーバが採番 (Hello だけ 0)。前回実行の残党パケットを弾く
//   playerIndex ClientInput: 送信者のレーン
//   count       後続の入力 / 確定 tick レコードの本数
//   baseTick    先頭レコードの tick
//   lastAckTick ClientInput / SnapshotAck: クライアントが次に必要とする確定 tick
//               Confirmed: サーバの確定フロンティア (次に確定する tick)
//   confirmTick/confirmHash  ClientInput: クライアントの確定 checkpoint (サーバのログ用)

inline constexpr uint32_t kNetPlayerSessionIdLen = 64;
inline constexpr uint32_t kNetSnapshotChunkBytes = 1024;
// SnapshotAck のビットマップが覆うチャンク数 (base から数える)
inline constexpr uint32_t kNetAckWindow = 256;
inline constexpr uint32_t kNetCheckpointsPerPacket = 4;
// Confirmed パケットのレコード部の上限 (MTU 1500 から IP/UDP/ヘッダ/本体を引いた余裕)
inline constexpr uint32_t kNetConfirmedBudget = 1200;
// 新しい確定 tick を送るとき、さらに遡って載せる tick 数 (冗長度)
inline constexpr uint32_t kNetConfirmedRedundancy = 2;
// 確定履歴 (サーバの再送元 / クライアントの受信リング) の深さ。これを超えて遅れたら再同期
inline constexpr uint32_t kNetHistoryTicks = 1024;
// ClientInput を受け付ける未来の幅 (これより先の tick は捨てる)
inline constexpr uint32_t kNetInputFutureWindow = 64;

// ResyncRequest の理由
enum class NetResyncReason : uint32_t {
    None = 0,
    EventGap = 1,     // eventSeq の欠番
    Desync = 2,       // checkpoint ハッシュの不一致
    BadSnapshot = 3,  // 復元後のワールドハッシュが SnapshotMeta と一致しない
    Behind = 4,       // 履歴から溢れた
};
const char* NetResyncReasonName(NetResyncReason r);

// c -> s。initialSnapshotHash は 0 で送る (参加時のスナップショットは接続ごとに違うので照合しない)
struct NetHelloPayload {
    SimProvenance prov;
    uint64_t playerId;        // 0 = 新規参加、それ以外 = 再接続の主張 (前回の Welcome の値)
    uint64_t fontMetricsHash;
    uint32_t configBits;
    uint32_t referenceW;
    uint32_t referenceH;
    uint32_t pad;
    char playerSessionId[kNetPlayerSessionIdLen];
};
static_assert(sizeof(NetHelloPayload) == 144, "NetHelloPayload is part of the wire format");

// s -> c (type = Reject)
struct NetRejectPayload {
    uint32_t reason; // NetReject
    uint32_t pad;
};

// s -> c。参加 / 再接続 / 再同期のたびに、そのスナップショットの素性を載せて送る
struct NetWelcomePayload {
    SnapshotMeta meta;        // tick = 回し始める tick、worldHash = 復元直後に一致すべき値
    uint64_t playerId;
    uint32_t lane;
    uint32_t snapshotBytes;
    uint32_t chunkCount;
    uint32_t serial;          // 送付の通し番号。古い送付のチャンクを弾く
};
static_assert(sizeof(NetWelcomePayload) == 168 + 24, "NetWelcomePayload is part of the wire format");

// s -> c。本体の後ろに size バイトのデータが続く
struct NetSnapshotChunkPayload {
    uint32_t serial;
    uint32_t index;
    uint32_t size;
    uint32_t pad;
};

// c -> s。mask の bit i = チャンク (base + i) を受信済み。base 未満は全部受信済み
struct NetSnapshotAckPayload {
    uint32_t serial;
    uint32_t base;
    uint64_t mask[kNetAckWindow / 64];
};

// c -> s
struct NetResyncPayload {
    uint32_t reason; // NetResyncReason
    uint32_t pad;
    uint64_t haveTick; // クライアントが次に必要とする確定 tick (ログ用)
};

struct NetCheckpoint {
    uint64_t tick;
    uint64_t hash; // 0 = 空き
};

inline constexpr uint32_t kNetConfirmedFlagMarginValid = 1u;

// s -> c。本体の後ろに count 本の tick レコードが続く
struct NetConfirmedPayload {
    // 受信した最新の自レーン入力の到着余裕 (締め切りの何 ms 前に届いたか。負 = 締め切り後)
    int32_t marginMs;
    uint32_t flags; // bit0 = marginMs は実測値 (サーバがまだ自レーンの入力を受け取っていなければ 0)
    NetCheckpoint checkpoints[kNetCheckpointsPerPacket]; // 新しい順
};
static_assert(sizeof(NetConfirmedPayload) == 8 + 16 * kNetCheckpointsPerPacket,
              "NetConfirmedPayload is part of the wire format");

// tick レコードの先頭 (4B)。続けて laneMask の立っているレーンの InputSnapshot (昇順)、
// eventCount 本の SystemEvent。マスクの立たないレーンはゼロ入力 (= 未接続 / 予約 / 全部ゼロ)
struct NetTickRecordHeader {
    uint8_t laneMask;
    uint8_t eventCount;
    uint8_t pad[2];
};
static_assert(sizeof(NetTickRecordHeader) == 4, "NetTickRecordHeader is part of the wire format");

// 確定 tick 1 本分 (サーバが決めた値の全部)。履歴リングと配信の単位
struct NetConfirmedTick {
    uint64_t tick = 0;
    InputSnapshot inputs[kMaxPlayers] = {};
    SystemInputTick sys = {};
};

// 確定 tick のワイヤ表現
size_t NetTickRecordBytes(const NetConfirmedTick& t);
// out の末尾へ追記する
void NetWriteTickRecord(const NetConfirmedTick& t, std::vector<uint8_t>& out);
// data から 1 本読む。読めたら消費バイト数、壊れていれば 0。tick は呼び出し側が決める
size_t NetReadTickRecord(const uint8_t* data, size_t size, uint64_t tick, NetConfirmedTick& out);

// 同一 tick の中身が両者で同じか (入力レーンのバイト一致 + 正規化済みシステム入力のバイト一致)
bool NetConfirmedTickEqual(const NetConfirmedTick& a, const NetConfirmedTick& b, uint32_t playerCount);

// 送信 / 受信のトランスポート。実 UdpSocket と 1 プロセス内の偽トランスポートを差し替える口。
// 仮想関数の抽象クラスを増やさず std::function にした (NetSession が既に std::function のフックを
// 使っている流儀に合わせた。サーバは相手を不透明な peer キーで呼び分ける)。
// 受信は pull ではなく push (OnPacket) — 受信順に処理するかどうかは呼び出し側 (tick 境界の順序) が決める
using NetSendFn = std::function<void(uint32_t peer, const void* data, size_t size)>;

// パケットの組み立て / 検査の共通部。壊れていれば false
bool NetParseHeader(const uint8_t* data, size_t size, NetPacketHeader& outHeader);
// header + payload (+ tail) を out へ組む
void NetBuildPacket(std::vector<uint8_t>& out, NetMsg type, const NetPacketHeader& base,
                    const void* payload, size_t payloadSize, const void* tail = nullptr,
                    size_t tailSize = 0);

// 1 tick の長さ (ms)。整数演算で固定 (60Hz): tickDueMs = startMs + (tick - startTick) * 1000 / 60
inline constexpr uint64_t kNetTickRateHz = 60;
inline uint64_t NetTicksToMs(uint64_t ticks) { return ticks * 1000 / kNetTickRateHz; }

} // namespace mye
