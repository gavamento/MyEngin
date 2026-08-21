#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "Engine/Engine/Replay/SimSnapshot.h"
#include "Engine/Platform/Input.h"

namespace mye {

// タイムトラベル・デバッグのリング (M52e、決定台帳 2/8)。
//
// 「N tick ごとの sim スナップショット + **全 tick の入力**」を持ち、
// 「target 以下の最寄りスナップショットへ Restore → 記録入力で target まで描画なし再シム」
// で任意の過去 tick へ戻す。再シムは通常 tick と**同じ RunOneTick** を通るので、
// 「再シムのときだけ挙動が違う」種類のバグが構造的に入らない。
//
// ★ここは**リングの器と方針だけ**を持つ純データ構造にしてある (エンジンの参照を持たない)。
//   実際の Restore + 再シムは EngineLoop が回す — 再シムには tick 本体の参照束
//   (TickServices) が要り、それは EngineLoop のスコープにしか無いため。
//   この分離のおかげでリングの方針 (分岐時の切り捨て / 追い出し / 最寄り探索) は
//   ライブなワールド無しで selftest できる。
//
// ★**Engine 層に置いてある**のは M52f のクラッシュ用リングが同じ型の別インスタンスを
//   使うため (計画では src\Editor\ 配下だったが、Editor は Engine より上位なので
//   エンジン側から使えない)。エディタ固有なのは TimelineWindow の UI だけ。
//
// ★リングに載るのは **sim レーンだけ** (SimSnapshot.h の境界と同一)。
//   C# スクリプトの状態は巻き戻らない = 再シムの結果が割れうる。これは record/verify と
//   同じ制約で、シークの自己検証 (期待ハッシュとの照合) がその事実を毎回可視化する。

// リング 1 tick 分の記録
struct TimeTravelEntry {
    // その tick が消費した入力レーン (ライブ入力の確定値)。M52g から **kMaxPlayers 本**。
    // 実効レーン数に関わらず上限本数を持つのは、1 エントリ長を固定して
    // 「途中でレーン数が変わったリング」という状態を作らないため (64B × 4 = 256B/tick)
    InputSnapshot inputs[kMaxPlayers] = {};
    uint64_t hashAfter = 0; // tick 末のワールドハッシュ (シークの自己検証に使う)
    // その tick で sim を進めたか。ポーズ中の tick は false で記録する —
    // ★ポーズ tick も sim 状態を 1 つだけ動かす (prevTickInput) ので、
    //   「飛ばして良い tick」ではない。再シムでも同じ値で再現する
    bool simulated = false;
};

struct TimeTravelConfig {
    // sim tick 何個ごとに 1 枚撮るか。ポーズ tick は数えない (止まった世界を撮り続けない)
    uint64_t snapshotInterval = 30;
    size_t maxSnapshots = 120;                   // 30 * 120 = 3600 tick = 60 秒
    size_t maxBytes = 64ull * 1024ull * 1024ull; // 既定デモ 528 体で 1 枚 148KB
};

// シークの結果。**ハッシュ照合まで込み** — 戻して再シムした世界が元の tick と
// ビット一致したかをその場で判定し、UI と CLI プローブの両方が同じ根拠を見る
enum class SeekOutcome : uint8_t {
    None = 0,
    Ok,           // 再シム後のハッシュが記録と一致
    HashMismatch, // 一致しなかった (C# レーン混入 / 非決定論の混入)
    Failed,       // スナップショットが無い / 復元に失敗した
};

struct SeekReport {
    SeekOutcome outcome = SeekOutcome::None;
    uint64_t target = 0;
    uint64_t fromSnapshot = 0; // 復元に使ったスナップショットの tick (前進シークでは現在 tick)
    uint64_t resimTicks = 0;
    double ms = 0.0;
    uint64_t expectedHash = 0;
    uint64_t actualHash = 0;
};

class TimeTravel {
public:
    void Configure(const TimeTravelConfig& c) { config_ = c; }
    const TimeTravelConfig& Config() const { return config_; }

    // ---- 有効化 (エディタの Play/Stop、または CLI プローブが叩く) ----
    // 実際の Begin は EngineLoop が tick 境界で行う (撮影点の制約があるため)
    void SetEnabled(bool on);
    bool Enabled() const { return enabled_; }
    bool BeginPending() const { return want_ && !enabled_; }

    // tick 境界で 1 枚目を撮って記録を始める。撮影に失敗したら無効化する
    void Begin(const SimRefs& refs, uint64_t tick);
    void Clear();

    // tick 1 本走った直後に呼ぶ。ranTick = いま走り終えた tick の番号。
    // ranTick がリングの途中なら「シーク後に走った = 分岐」とみなして未来を捨てる。
    // inputs は playerCount 本のレーン配列 (残りはゼロ値で埋める)
    void OnTickEnd(const SimRefs& refs, uint64_t ranTick, const InputSnapshot* inputs,
                   uint32_t playerCount, bool simulated);

    // ---- シーク要求 (UI → EngineLoop) ----
    // 要求した時点でスクラブ状態に入る = EngineLoop は tick を進めなくなる。
    // ★これが無いと、シーク直後のポーズ tick が「分岐」として未来を消してしまい、
    //   スクラブで行ったり来たりできない (実装中に踏んだ罠)
    void RequestSeek(uint64_t tick);
    bool HasPendingSeek() const { return seekPending_; }
    uint64_t PendingSeek() const { return seekTarget_; }
    void ClearPendingSeek() { seekPending_ = false; }
    void ReportSeek(const SeekReport& r) { lastSeek_ = r; }
    const SeekReport& LastSeek() const { return lastSeek_; }

    bool Scrubbing() const { return scrubbing_; }
    void EndScrub() { scrubbing_ = false; } // 再生/ステップ再開 = ここから分岐する

    // ---- 参照 ----
    uint64_t FirstTick() const { return firstTick_; }
    uint64_t EndTick() const { return firstTick_ + entries_.size(); }
    bool HasTick(uint64_t t) const { return enabled_ && t >= firstTick_ && t < EndTick(); }
    const TimeTravelEntry* Entry(uint64_t t) const;
    // 「tick t が走る**前**の状態」のハッシュ。t == EndTick() なら現在の状態
    uint64_t HashAtTick(uint64_t t) const;
    // target 以下で最寄りのスナップショット (無ければ nullptr)
    const std::vector<std::byte>* SnapshotAtOrBefore(uint64_t target, uint64_t& outTick) const;

    size_t SnapshotCount() const { return snapshots_.size(); }
    size_t SnapshotBytes() const { return bytes_; }
    size_t EntryCount() const { return entries_.size(); }

private:
    struct Snap {
        uint64_t tick = 0; // この blob は「tick が走る前」の状態
        std::vector<std::byte> blob;
    };

    bool TakeSnapshot(const SimRefs& refs, uint64_t tick);
    void Evict();
    void DropSnapshotsAfter(uint64_t tick);
    static uint64_t HashOf(const SimRefs& refs);

    TimeTravelConfig config_;
    bool want_ = false;
    bool enabled_ = false;
    bool scrubbing_ = false;
    bool seekPending_ = false;
    uint64_t seekTarget_ = 0;
    uint64_t firstTick_ = 0;
    uint64_t startHash_ = 0;      // firstTick_ が走る前の状態のハッシュ
    uint64_t simSinceSnapshot_ = 0;
    size_t bytes_ = 0;
    std::vector<TimeTravelEntry> entries_; // entries_[i] = tick firstTick_+i
    std::vector<Snap> snapshots_;          // tick 昇順
    SeekReport lastSeek_;
};

} // namespace mye
