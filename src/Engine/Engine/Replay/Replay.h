#pragma once
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "Engine/Engine/Session/SessionTypes.h"
#include "Engine/Platform/Input.h"

namespace mye {

// リプレイファイル (.rep) — engine_spec.md 11.3。
// 形式 (リトルエンディアン、バイナリ):
//   MyeReplayHeader
//   埋め込み sim スナップショット (snapshotSize バイト、0 なら無し)
//   tick 毎: InputSnapshot × playerCount + (flags.bit0 のとき) SystemInputTick + uint64 worldHash
// InputSnapshot / WorldHasher のレイアウトが変わったら version を上げること。
//
// ★**worldHash == 0 は「期待値なし」の予約値** (M52f)。
//   クラッシュ .rep の未完了 tick と、負荷を抑えるため checkpoint 外にした tick は
//   期待ハッシュを持たない。ここに嘘の値を書くと、再現しなかったときに
//   MISMATCH という別の事故に化けるので、値そのもので「照合しない」を表す。
//   検証側 (TickRunner) は 0 のレコードを照合せず unverifiedTicks へ数える。
//   ★通常の ReplayRecorder は実ハッシュを書き、CrashRing は checkpoint 外へ意図的に 0 を書く。
//     実ハッシュが偶然 0 になる確率は 2^-64 で、その場合も「その 1 tick が未照合に
//     なる」だけで誤検出にはならない (安全側に倒れる)。

// .rep のフォーマット版。ネットのハンドシェイク (M52h) でも照合するので、
// Replay.cpp の中に閉じずにここへ出してある。
// 版で弾く理由: ヘッダの inputSize は同サイズの別レイアウトを検出できず、WorldHasher の意味が
// 変わった旧 .rep は全 tick で MISMATCH になる — 「読めない」と言わせるほうが診断として正しい。
// v4 (M52d): ヘッダに snapshotSize (開始時点の sim 状態の埋め込み) と playerCount (入力レーン数)
// v5 (M64a): InputSnapshot 64 -> 72 バイト (生マウスデルタ mouseDeltaX/Y)
// v6 (M70b): InputSnapshot 72 -> 88 バイト (UI キャンバスの 4 値)
// v7 (M70c): WorldHasher に UI 対話状態の節 (InputSnapshot は不変)
// v8 (M75b): InputSnapshot 88 -> 112 バイト (ゲーム面 px + 面の寸法、文字キュー) + WorldHasher の UI 節に changed / ドラッグ状態
// v9 (M81b): ヘッダへ flags / SessionConfig / SimProvenance / 開始 SnapshotMeta、tick レコードへ
//            SystemInputTick (flags.bit0 のとき)。v8 も読める (新項目は「不明」= 0)
inline constexpr uint32_t kReplayFileVersion = 9;
inline constexpr uint32_t kReplayOldestReadableVersion = 8;

// flags.bit0: tick レコードに SystemInputTick が付く (SessionConfig.role が Server / Client の記録)
inline constexpr uint32_t kReplayFlagSystemInput = 1u << 0;

// v8 までのヘッダ (ディスク上のレイアウト)。v8 の読み込みだけに使う
struct MyeReplayHeaderV8 {
    uint32_t magic;
    uint32_t version;
    float fixedDt;
    uint32_t inputSize;
    uint64_t tickCount;
    uint64_t rngState;
    uint64_t rngInc;
    uint32_t entityCount;
    uint32_t playerCount;
    uint64_t snapshotSize;
};
static_assert(sizeof(MyeReplayHeaderV8) == 56, "the v8 header layout is fixed");

struct MyeReplayHeader {
    uint32_t magic = 0x5045524Du; // 'MREP'
    uint32_t version = kReplayFileVersion;
    float fixedDt = 1.0f / 60.0f;
    uint32_t inputSize = sizeof(InputSnapshot);
    uint64_t tickCount = 0;   // 終了時に確定
    uint64_t rngState = 0;    // 記録開始時のワールド RNG (再生時に復元)
    uint64_t rngInc = 0;
    uint32_t entityCount = 0; // 記録開始時 (サニティチェック)
    uint32_t playerCount = 1; // v4: 1 = 従来のシングル入力
    uint64_t snapshotSize = 0; // v4: 埋め込みスナップショットのバイト数 (0 = 無し)
    // ---- v9 (M81b)。先頭 56 バイトは v8 と同じ並び ----
    uint32_t flags = 0;        // kReplayFlag*
    uint32_t pad = 0;
    SessionConfig session = {};
    SimProvenance provenance = {};
    // 開始スナップショットの素性。0 は「未計算」
    SnapshotMeta startMeta = {};
};
static_assert(sizeof(MyeReplayHeader) == sizeof(MyeReplayHeaderV8) + 8 + sizeof(SessionConfig)
                                             + sizeof(SimProvenance) + sizeof(SnapshotMeta),
              "MyeReplayHeader has no implicit padding");

// tick レコード 1 本のバイト数 (Load の件数検証と CrashRing が使う)
inline uint64_t ReplayTickRecordBytes(uint32_t playerCount, uint32_t flags)
{
    return static_cast<uint64_t>(playerCount) * sizeof(InputSnapshot)
        + (((flags & kReplayFlagSystemInput) != 0) ? sizeof(SystemInputTick) : 0)
        + sizeof(uint64_t);
}

// 記録: tick 毎の入力 + ワールドハッシュを蓄積し、Finish でファイルへ書き出す。
// 逐次モード (Start の streamFlushTicks > 0): 開始時にヘッダ (tickCount = 0) と埋め込みスナップショットを
// 書き、tick は追記して streamFlushTicks 本ごとに flush する。Finish が tickCount を書き戻す。
// 異常終了で tickCount = 0 のまま残ったファイルは、ReplayPlayer::Load がファイル長から完了済みの
// tick 数を求めて読む (切れたレコードは捨てる)。一括モードと逐次モードの出力は 1 バイトも違わない。
class ReplayRecorder {
public:
    // snapshot 非 null で「開始時点の sim 状態」をヘッダ直後へ埋め込む (M52f が使う)。
    // 埋め込みの有無は再生側が header.snapshotSize で判断する。
    // playerCount = 入力レーン数 (M52g)。1 なら v4 以前と 1 バイトも変わらない列になる。
    // session.role が Server / Client ならヘッダ flags.bit0 が立ち、tick レコードに SystemInputTick が付く。
    // ★snapshot を埋めるとき rngState / rngInc は**そのスナップショットの World RNG と同じ値**を渡すこと
    //   (Load が blob と突き合わせ、食い違えば拒否する。真値を 2 つ持っても食い違えないようにする)
    // streamFlushTicks > 0: 逐次モード。ファイルを開けなければ IsActive() が false のまま (エラーログ付き)
    void Start(const std::wstring& path, uint64_t rngState, uint64_t rngInc, uint32_t entityCount,
               uint32_t playerCount = 1, const std::byte* snapshot = nullptr,
               size_t snapshotSize = 0, const SessionConfig& session = SessionConfig{},
               const SimProvenance& provenance = SimProvenance{},
               const SnapshotMeta& startMeta = SnapshotMeta{}, uint32_t streamFlushTicks = 0);
    // lanes は playerCount 本の配列。**Start で宣言した本数と一致すること** —
    // ここが食い違うとファイルの tick レコード長と中身がずれる。
    // systemInput は flags.bit0 の記録でだけ使われる (null はイベント無しの tick)
    void RecordTick(const InputSnapshot* lanes, uint32_t playerCount, uint64_t worldHash,
                    const SystemInputTick* systemInput = nullptr);
    // ファイル書き出し (逐次モードでは tickCount の書き戻しと close)。書き損じていれば false
    bool Finish();
    bool IsActive() const { return active_; }
    uint64_t TickCount() const { return tickCount_; }

private:
    std::wstring path_;
    MyeReplayHeader header_;
    std::vector<std::byte> snapshot_;
    std::vector<InputSnapshot> inputs_; // playerCount 本ずつ tick 順に並ぶ (一括モードのみ)
    std::vector<SystemInputTick> systemInputs_; // flags.bit0 のときだけ tick 毎に 1 本 (一括モードのみ)
    std::vector<uint64_t> hashes_;              // (一括モードのみ)
    uint64_t tickCount_ = 0;
    uint32_t streamFlushTicks_ = 0; // 0 = 一括モード
    std::ofstream stream_;
    bool streamFailed_ = false;
    bool active_ = false;
};

// 再生 + 検証: 記録済み入力でフェーズ 1 を置換し、tick 毎のハッシュを照合する
class ReplayPlayer {
public:
    bool Load(const std::wstring& path);
    bool IsActive() const { return active_; }
    uint64_t TickCount() const { return hashes_.size(); }
    // true = 逐次記録が閉じられないまま残った .rep (ヘッダの tickCount = 0) を、ファイル長から救って読んだ
    bool RecoveredUnfinished() const { return recovered_; }
    uint64_t RngState() const { return header_.rngState; }
    uint64_t RngInc() const { return header_.rngInc; }
    // 1 tick あたりの入力レーン数 (M52g)。**EngineLoop はこの値を ctx.playerCount へ
    // 採用する** — レコード長はファイル側で決まっているので、--local-players の指定より
    // .rep が優先される
    uint32_t PlayerCount() const { return header_.playerCount; }
    // 埋め込みスナップショット (空 = 無し)。EngineLoop はこれがあれば
    // シーンロードの代わりに Restore して再生を始められる (M52f)
    const std::vector<std::byte>& Snapshot() const { return snapshot_; }
    const MyeReplayHeader& Header() const { return header_; }
    // flags.bit0: この記録はシステム入力を持つ。EngineLoop / HeadlessSim は ctx.hasSystemInput へ写す
    bool HasSystemInput() const { return (header_.flags & kReplayFlagSystemInput) != 0; }
    // HasSystemInput() のときだけ有効
    const SystemInputTick& SystemInputForTick(uint64_t tick) const
    {
        return systemInputs_[static_cast<size_t>(tick)];
    }

    const InputSnapshot& InputForTick(uint64_t tick) const
    {
        return inputs_[static_cast<size_t>(tick) * header_.playerCount];
    }
    const InputSnapshot& InputForTick(uint64_t tick, uint32_t player) const
    {
        return inputs_[static_cast<size_t>(tick) * header_.playerCount + player];
    }
    uint64_t ExpectedHash(uint64_t tick) const { return hashes_[static_cast<size_t>(tick)]; }
    bool HasTick(uint64_t tick) const { return tick < hashes_.size(); }
    // 0 = 期待値なし (未完了 tick または checkpoint 外)
    bool HasExpectedHash(uint64_t tick) const { return ExpectedHash(tick) != 0; }

    // 照合結果
    uint64_t verifiedTicks = 0;
    uint64_t unverifiedTicks = 0; // 期待値なしで走らせた tick (M52f)
    bool failed = false;
    uint64_t firstMismatchTick = 0;

private:
    MyeReplayHeader header_;
    std::vector<std::byte> snapshot_;
    std::vector<InputSnapshot> inputs_;
    std::vector<SystemInputTick> systemInputs_;
    std::vector<uint64_t> hashes_;
    bool recovered_ = false;
    bool active_ = false;
};

// --replay-verify の合否 (EngineLoop / HeadlessSim 共通)。照合した tick が 0 本の「合格」を作らない:
// 1 tick も比べていないものは通ったとは言えない。startTick = スナップショット復元後の tick
// (ReplayPlayer は tick を絶対値で引くので、0 以外から始まる .rep は範囲外になる)。
// 不合格なら理由を reason へ書く (合格のときは空)
bool JudgeReplayVerification(const ReplayPlayer& player, uint64_t startTick, std::string& reason);

// .rep 2 本の突き合わせ (M52h、--rep-diff A B)。
// ネット対戦の 2 プロセスが**本当に同じ tick 列を回したか**を機械判定するための道具。
// ★`fc /b` で済ませない理由は M52a と同じ: 割れたときに「どの tick の どのレーンの
//   どのフィールドか」まで出ないと、原因の切り分けにそのまま何時間も溶ける。
struct ReplayDiffResult {
    bool same = false;
    uint64_t firstDiffTick = 0; // same=false かつ tick 列で割れたときのみ意味を持つ
    std::string summary;        // 1 行の結論 (そのままログへ出す)
};
// overlapMinTicks > 0 (--rep-diff-overlap N): 開始 tick が違う 2 本の、tick が重なる区間だけを比べ、
// 重なりが N tick 未満なら失敗にする (サーバ .rep と途中参加クライアント .rep の突き合わせ)。0 = 従来の厳密比較
ReplayDiffResult DiffReplayFiles(const std::wstring& a, const std::wstring& b, uint64_t overlapMinTicks = 0);

} // namespace mye
