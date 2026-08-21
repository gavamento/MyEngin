#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Engine/Platform/Input.h"

namespace mye {

// リプレイファイル (.rep) — engine_spec.md 11.3。
// 形式 (リトルエンディアン、バイナリ):
//   MyeReplayHeader
//   埋め込み sim スナップショット (snapshotSize バイト、0 なら無し)
//   tick 毎: InputSnapshot × playerCount + uint64 worldHash
// InputSnapshot / WorldHasher のレイアウトが変わったら version を上げること。
//
// v4 (M52d、決定台帳 3): 版を上げるのは M52 を通してこの 1 回だけ。使い始めるのは
// 後のサブでも、フォーマットの改版は 1 回に束ねる (M51h の ABI 束ねと同じ思想):
//   snapshotSize … ヘッダ直後に置く「開始時点の sim 状態」。これがあると .rep は
//                  シーン非依存に再生できる (M52f のクラッシュ再現が本命)
//   playerCount  … tick レコードあたりの入力本数 (M52g のマルチ入力レーン)
//
// ★**worldHash == 0 は「期待値なし (未完了 tick)」の予約値** (M52f)。
//   クラッシュ .rep の最後の 1 本は「入力は確定したが走り切らなかった tick」で、
//   期待ハッシュが原理的に存在しない。ここに嘘の値を書くと、再現しなかったときに
//   MISMATCH という別の事故に化けるので、値そのもので「照合しない」を表す。
//   検証側 (TickRunner) は 0 のレコードを照合せず unverifiedTicks へ数える。
//   ★記録側は 0 を書かない: 実ハッシュが偶然 0 になる確率は 2^-64 で、その場合も
//     「その 1 tick が未照合になる」だけで誤検出にはならない (安全側に倒れる)。
//   この予約は v4 のレイアウトを一切変えない = 版は上げない (決定台帳 3)。
struct MyeReplayHeader {
    uint32_t magic = 0x5045524Du; // 'MREP'
    uint32_t version = 4;
    float fixedDt = 1.0f / 60.0f;
    uint32_t inputSize = sizeof(InputSnapshot);
    uint64_t tickCount = 0;   // 終了時に確定
    uint64_t rngState = 0;    // 記録開始時のワールド RNG (再生時に復元)
    uint64_t rngInc = 0;
    uint32_t entityCount = 0; // 記録開始時 (サニティチェック)
    uint32_t playerCount = 1; // v4: 1 = 従来のシングル入力
    uint64_t snapshotSize = 0; // v4: 埋め込みスナップショットのバイト数 (0 = 無し)
};

// 記録: tick 毎の入力 + ワールドハッシュを蓄積し、Finish でファイルへ書き出す
class ReplayRecorder {
public:
    // snapshot 非 null で「開始時点の sim 状態」をヘッダ直後へ埋め込む (M52f が使う)。
    // 埋め込みの有無は再生側が header.snapshotSize で判断する。
    // playerCount = 入力レーン数 (M52g)。1 なら v4 以前と 1 バイトも変わらない列になる
    void Start(const std::wstring& path, uint64_t rngState, uint64_t rngInc, uint32_t entityCount,
               uint32_t playerCount = 1, const std::byte* snapshot = nullptr,
               size_t snapshotSize = 0);
    // lanes は playerCount 本の配列。**Start で宣言した本数と一致すること** —
    // ここが食い違うとファイルの tick レコード長と中身がずれる
    void RecordTick(const InputSnapshot* lanes, uint32_t playerCount, uint64_t worldHash);
    bool Finish(); // ファイル書き出し
    bool IsActive() const { return active_; }
    uint64_t TickCount() const { return hashes_.size(); }

private:
    std::wstring path_;
    MyeReplayHeader header_;
    std::vector<std::byte> snapshot_;
    std::vector<InputSnapshot> inputs_; // playerCount 本ずつ tick 順に並ぶ
    std::vector<uint64_t> hashes_;
    bool active_ = false;
};

// 再生 + 検証: 記録済み入力でフェーズ 1 を置換し、tick 毎のハッシュを照合する
class ReplayPlayer {
public:
    bool Load(const std::wstring& path);
    bool IsActive() const { return active_; }
    uint64_t TickCount() const { return hashes_.size(); }
    uint64_t RngState() const { return header_.rngState; }
    uint64_t RngInc() const { return header_.rngInc; }
    // 1 tick あたりの入力レーン数 (M52g)。**EngineLoop はこの値を ctx.playerCount へ
    // 採用する** — レコード長はファイル側で決まっているので、--local-players の指定より
    // .rep が優先される
    uint32_t PlayerCount() const { return header_.playerCount; }
    // 埋め込みスナップショット (空 = 無し)。EngineLoop はこれがあれば
    // シーンロードの代わりに Restore して再生を始められる (M52f)
    const std::vector<std::byte>& Snapshot() const { return snapshot_; }

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
    // 0 = 期待値なし (未完了 tick)。クラッシュ .rep の最後の 1 本がこれになる
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
    std::vector<uint64_t> hashes_;
    bool active_ = false;
};

} // namespace mye
