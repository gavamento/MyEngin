//====================================================================================
//                          SessionTypes.h
//  MyEngin/ 秋田蓮音                                                     10/02/2026
//                                          セッション型 (SessionConfig / システム入力 / レーン状態)
//====================================================================================
#pragma once
#include <cstdint>
#include <string>
#include <type_traits>

#include "Engine/Platform/Input.h"

namespace mye {

// 専用サーバ (M81) のセッションを sim 側から見た型の置き場。
// ★ここは sim 側 (Net/ を include しない)。Net/ → Session/ は許可、逆は check_rules 規則 13-b が禁止する。
//   全て POD で、バイト列がそのまま .rep (v9) に載る。レイアウトを変えたら kReplayFileVersion を上げること。
// 入力の流れ: 呼び出し側が tick ごとに InputSnapshot[kMaxPlayers] + SystemInputTick を確定させ、
// RunOneTick が ApplySystemInput を 1 回呼ぶ。実時間・受信順・SDK 由来の値はここへ入れない。

// SessionConfig.role の値。NetRole (Net/NetSession.h) の生値と同じ数値で、
// 記録者の役割を表すだけ — **再生結果は role に依存しない**
enum class SessionRole : uint32_t { None = 0, Host = 1, Join = 2, Server = 3, Client = 4 };

// この役割の記録がシステム入力 (SystemInputTick) を持つか。.rep のヘッダ flags と
// ワールドハッシュの SessionLanes 節のゲートに使う
constexpr bool RoleHasSystemInput(uint32_t role)
{
    return role == static_cast<uint32_t>(SessionRole::Server)
        || role == static_cast<uint32_t>(SessionRole::Client);
}

// セッション開始時に確定し、走行中は変えない設定。0 は「不明/未設定」の予約値
struct SessionConfig {
    uint32_t role;               // SessionRole
    uint32_t playerCount;        // 最大人数 (入力レーン数)
    uint32_t tickRate;           // 60 固定 (照合用)
    uint32_t inputDelay;         // tick
    uint32_t deadlineTicks;      // 締め切り (サーバの確定待ち)
    uint32_t rejoinTimeoutTicks; // 切断レーンの予約期間
    uint32_t configBits;         // 決定論に効く起動オプション
    uint32_t referenceW;         // UI 基準解像度
    uint32_t referenceH;
    uint32_t pad0;
    uint64_t seed;
    uint64_t fontMetricsHash;
    uint64_t reserved[4];
};
static_assert(sizeof(SessionConfig) == 88, "SessionConfig layout is part of the replay format");

// 出自情報。照合は sub-03 の CompareProvenance が担う。0 は「不明」
struct SimProvenance {
    uint64_t engineVersion;
    uint32_t protocolVersion;
    uint32_t apiVersion;
    uint32_t schemaVersion;
    uint32_t replayVersion;
    uint64_t gameVersion;
    uint64_t contentHash;
    uint64_t initialSnapshotHash; // 開始スナップショット blob のバイト列ハッシュ
};
static_assert(sizeof(SimProvenance) == 48, "SimProvenance layout is part of the replay format");

// スナップショットの素性。RNG は持たない (blob の World 節が真値)。0 は「未計算」
struct SnapshotMeta {
    uint64_t tick;         // このスナップショットから回し始める tick
    uint64_t worldHash;
    uint64_t blobHash;
    uint64_t lastEventSeq; // 撮影時点で適用済みの最後の eventSeq
    SessionConfig config;
    SimProvenance provenance;
};
static_assert(sizeof(SnapshotMeta) == 168, "SnapshotMeta layout is part of the replay format");

// ---- システム入力 ----

enum class SystemEventKind : uint8_t { None = 0, Join = 1, Leave = 2, Rejoin = 3, Release = 4 };

inline constexpr uint32_t kMaxSystemEventsPerTick = 8;

// eventSeq はセッション全体で 1 本の単調増加 (1 始まり)。playerId = 初回 Join の eventSeq。
// lane は Leave / Release では対象レーンの指定 (入力)、Join / Rejoin では無視され、
// 適用結果 (SessionLanes::applied) にだけ割り当てられたレーンが入る
struct SystemEvent {
    uint64_t eventSeq;
    uint64_t playerId;
    uint8_t kind; // SystemEventKind
    uint8_t lane;
    uint8_t pad[6];
};
static_assert(sizeof(SystemEvent) == 24, "SystemEvent layout is part of the replay format");

// 1 tick 分のシステム入力。events[eventCount..] と pad は 0 埋め (記録側が正規化する)
struct SystemInputTick {
    uint32_t eventCount;
    uint32_t pad;
    SystemEvent events[kMaxSystemEventsPerTick];
};
static_assert(sizeof(SystemInputTick) == 200, "SystemInputTick layout is part of the replay format");

// ---- レーン状態 (sim 状態。Scene が持ち、スナップショットとハッシュに入る) ----

enum class LaneState : uint32_t { Empty = 0, Connected = 1, Reserved = 2 };

struct LaneSlot {
    uint32_t state; // LaneState
    uint32_t pad;
    uint64_t playerId; // Empty は 0
};

struct SessionLanes {
    // 1 = この記録はシステム入力を持つ。ワールドハッシュの SessionLanes 節のゲート
    // (システム入力の無い記録のハッシュ列を 1 bit も動かさないため)
    uint32_t systemInput;
    uint32_t appliedCount; // 直近の ApplySystemInput で適用したイベント数
    uint64_t lastEventSeq;
    LaneSlot lanes[kMaxPlayers];
    // 直近の ApplySystemInput で**適用された**イベントの写し (無視したものは含まない)。
    // lane には割り当て結果が入る。ゲームから「この tick の参加・離脱」として読まれる
    SystemEvent applied[kMaxSystemEventsPerTick];
};
static_assert(sizeof(SessionLanes) == 16 + sizeof(LaneSlot) * kMaxPlayers + sizeof(SystemEvent) * kMaxSystemEventsPerTick,
              "SessionLanes has no implicit padding");
static_assert(std::is_trivially_copyable_v<SessionLanes>, "SessionLanes must be a POD");

inline constexpr int kNoLane = -1;

// ---- 純関数 (サーバ / クライアント / 再生で同じ判定になる) ----

// tick 頭にシステム入力を適用する。eventSeq 昇順 (同順位は入力順) に処理し、
// 不正なイベント (存在しないレーンへの Leave、未知の kind、既に適用済みの eventSeq など) は
// エラーログを出して無視する (MYE_CHECK にしない: 判定が全員で同じになる純関数の中の出来事)
void ApplySystemInput(SessionLanes& lanes, const SystemInputTick& input);

// Empty の最小レーン。満員なら kNoLane
int AllocateLane(const SessionLanes& lanes);

// 締め切り超過の代替入力 = 前 tick の確定入力から消費型フィールド
// (chars / charCount / mouseDeltaX / mouseDeltaY / wheelDelta) を 0 にしたもの。
// 消費型をそのまま繰り返すと文字が 2 回打たれ、視点が 2 回回る
InputSnapshot SubstituteLateInput(const InputSnapshot& prev);

// システム入力の無い構成 (オフライン / P2P) の既定のレーン状態:
// [0, playerCount) が Connected (playerId 0)、イベント 0 件
SessionLanes DefaultLanesFor(uint32_t playerCount);

// 範囲外の eventCount を丸め、events[eventCount..] と pad を 0 にした写しを返す。
// .rep へ書く前に通して、ファイルのバイト列を決定的にする
SystemInputTick NormalizeSystemInput(const SystemInputTick& in);

// 2 つの SystemInputTick の最初の食い違い項目名 (空 = 一致)。--rep-diff 用
std::string FirstDifferentSystemInputField(const SystemInputTick& a, const SystemInputTick& b);

} // namespace mye
