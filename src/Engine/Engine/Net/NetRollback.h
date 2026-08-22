#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Engine/Engine/Replay/SimSnapshot.h"
#include "Engine/Platform/Input.h"

namespace mye {

class CrashRing;
struct HashDump;

// 予測ロールバックのリング (M52i、決定台帳 2)。
//
// 遅延ロックステップ (M52h) は「全 peer の入力がそろうまで tick を進めない」。
// ロールバックは**そろっていなくても進む**: 未着レーンを予測 (直近の確定値の繰り返し)
// で埋めて先へ行き、後から本物が届いて予測と食い違ったら、その tick の**直前**の
// スナップショットへ戻して確定入力で再シムする。
//
// ★再シムは通常 tick と**同じ RunOneTick** を通す (決定台帳 2)。タイムトラベルの
//   シーク (M52e) と同じ流儀で、ここは「リングの器と方針」だけを持つ純データ構造にし、
//   Restore と再シムは EngineLoop が回す (TickServices は EngineLoop のスコープにしかない)。
//
// ★このクラスは**確定 (confirmed) と予測 (predicted) の区別を持つ唯一の場所**。
//   .rep へ書いてよいのも、desync 検出で相手と突き合わせてよいのも確定 tick のハッシュだけ。
//   予測で走った tick のハッシュを外へ出すと、正常なロールバックが desync に化ける。
//
// ★スナップショットは「その tick が走る**前**」の状態を tick 末に 1 枚ずつ撮る。
//   tick 頭で撮らないのは、撮影点の前提 (構造変更が空) が保証されるのが
//   RunOneTick 直後だけだから (World::SnapshotWrite の MYE_CHECK)。

// 予測で先行できる上限 tick 数。超えたら stall する (= M52h の挙動へ落ちる)。
// 8 tick = 133ms。これ以上先行すると「巻き戻ったときの見た目の飛び」が実用にならない
inline constexpr uint32_t kNetMaxSpeculation = 8;
// リングの実長。[confirmed, current] の最大 kNetMaxSpeculation+1 本を常に保持できること
inline constexpr uint32_t kNetSpecRing = kNetMaxSpeculation + 4;
// 確定ハッシュの保持長 (desync 照合用)。相手の確定点はネットワーク遅延ぶん遅れて届く
inline constexpr uint32_t kNetHashRing = 256;

// 1 tick 分の投機記録
struct NetSpecTick {
    InputSnapshot inputs[kMaxPlayers] = {}; // その tick が実際に消費した入力
    uint64_t hashAfter = 0;                 // tick 末のワールドハッシュ
    bool predicted = false;                 // 未確定レーンを予測で埋めて走った
    // その tick で sim を進めたか (ポーズ tick も prevTickInput を動かすので飛ばせない)
    bool simulated = true;
};

class NetRollback {
public:
    // startTick が走る**前**の状態を 1 枚撮って開始する。撮影に失敗したら無効のまま
    bool Begin(const SimRefs& refs, uint64_t startTick);
    void Clear();
    bool Active() const { return active_; }

    // tick が走り切った直後に呼ぶ。投機記録を残し、「次 tick が走る前」を 1 枚撮る
    void OnTickEnd(const SimRefs& refs, uint64_t ranTick, const InputSnapshot* inputs,
                   uint32_t playerCount, uint64_t hashAfter, bool predicted, bool simulated);

    const NetSpecTick* Entry(uint64_t tick) const;
    // 予測フラグを下ろす (届いた確定値が予測と一致した = もう巻き戻す理由が無い)
    void MarkConfirmed(uint64_t tick);
    // 記録済みの入力と lanes が **バイト一致**するか (playerCount 本だけ見る)
    bool InputsMatch(uint64_t tick, const InputSnapshot* lanes, uint32_t playerCount) const;

    // 「tick が走る前」の状態 blob (無ければ nullptr)
    const std::vector<std::byte>* SnapshotBefore(uint64_t tick) const;

    // ---- 確定フロンティア ----
    uint64_t ConfirmedTick() const { return confirmed_; }
    void SetConfirmedTick(uint64_t tick) { confirmed_ = tick; }

    // ---- 確定ハッシュ (desync 照合) ----
    void NoteCommitted(uint64_t tick, uint64_t hash);
    bool CommittedHash(uint64_t tick, uint64_t& outHash) const;

    // ---- 統計 ----
    void NoteRollback(uint64_t depth);
    uint64_t RollbackCount() const { return rollbacks_; }
    uint64_t RollbackTicks() const { return rollbackTicks_; }
    uint64_t MaxRollbackDepth() const { return maxDepth_; }
    uint64_t PredictedTicks() const { return predictedTicks_; }
    size_t SnapshotBytes() const { return snapBytes_; }

private:
    struct Slot {
        uint64_t tick = ~0ull; // この blob は「tick が走る前」の状態
        std::vector<std::byte> blob;
    };

    bool TakeSnapshot(const SimRefs& refs, uint64_t tick);

    bool active_ = false;
    uint64_t confirmed_ = 0;
    uint64_t rollbacks_ = 0;
    uint64_t rollbackTicks_ = 0;
    uint64_t maxDepth_ = 0;
    uint64_t predictedTicks_ = 0;
    size_t snapBytes_ = 0;

    Slot snaps_[kNetSpecRing];
    NetSpecTick spec_[kNetSpecRing];
    uint64_t specTick_[kNetSpecRing] = {};
    bool specValid_[kNetSpecRing] = {};

    uint64_t hashTick_[kNetHashRing] = {};
    uint64_t hashValue_[kNetHashRing] = {}; // 0 = 空き (Replay.h と同じ予約)
};

// ---- desync バンドル (M52i) ----
// 「2 台のワールドハッシュが割れた」= リプレイ決定論が壊れた瞬間そのもの。
// M52f のクラッシュバンドルと同じ思想で「再現可能な報告を 1 フォルダに残す」が、
// ★**クラッシュハンドラの外**で走るので確保も直列化も自由に使ってよい
//   (壊れているのはヒープではなくシミュレーションなので、制約の理由が無い)。
//
//   crash\desync_<tick>\
//     desync.txt … 割れた tick / 双方のハッシュ / 自分の役とレーン / 起動オプション /
//                  次に打つコマンド (--rep-diff → --hash-diff の手順)
//     local.rep  … CrashRing が常時維持している .rep イメージ (埋め込みスナップショット付き)
//     local.dump … 検出時点のフィールド単位ダンプ (M52a の書式)
struct NetDesyncReport {
    uint64_t tick = 0;    // ハッシュが割れた tick (双方が確定済みの tick)
    uint64_t nowTick = 0; // 検出した時点の tick (確定点より先行していることがある)
    uint64_t localHash = 0;
    uint64_t peerHash = 0;
    uint32_t localPlayer = 0;
    int role = 0; // NetRole の生値
};

// 成功で true。outDir に実際に作ったフォルダの絶対パスを返す
bool WriteNetDesyncBundle(const std::wstring& crashRoot, const NetDesyncReport& rep,
                          const CrashRing& ring, const HashDump& dump, std::wstring& outDir);

} // namespace mye
