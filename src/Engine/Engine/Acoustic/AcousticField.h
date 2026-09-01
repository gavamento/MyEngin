//====================================================================================
//                          AcousticField.h
//  MyEngine/ 秋田蓮音                                                      09/01/2026
//                                          音響の場（波スロット表＝sim 状態／占有グリッド＝導出値）
//====================================================================================
#pragma once
#include <cstdint>
#include <vector>

#include "Engine/Core/EntityID.h"
#include "Engine/Engine/Acoustic/AcousticGrid.h"

namespace mye {

class World;

// 音響の「場」(M65a、計画 hushed-rippling-beacon)。
//
// ★**ECS 外の sim 状態の 3 例目** (1 = CpuParticleBackend / 2 = XpbdBackend)。
//   sim 状態は必ず 3 点セットで運ぶこと:
//     池 (ここの waves_) + ハッシュ節 (WorldHasher::HashAcousticWaves)
//     + snapshot 節 (SimSnapshot の 'ACU1')。
//   片方だけ足すと「リプレイは通るのに巻き戻し/ロールバックで割れる」型のバグになる。
//
// ★このクラスで **sim 状態なのは波スロット表 (waves_) だけ**。占有グリッドも、
//   波ごとの距離場も、残光も**全部導出値**である。そう作れる理由は
//   「到達エネルギーを整数チャンファ距離の純関数にしたから」で、伝播中に材質で減衰
//   させるとエネルギーが経路依存になり、**セル配列そのものが sim 状態に落ちて**
//   snapshot が数 MB になる (計画 判断 3。だから床材は発音時の振幅と到達上限にだけ効かせる)。
//
// ★復元 (snapshot / ロールバック) の後は Invalidate() を呼ぶこと。次の Update が
//   ring 0 から現リングまで**引き直す**。「増分で育てた場」と「引き直した場」が
//   ビット同一であることが本システムで最も重要な不変条件で、そこに selftest を当てる。
//
// M65a は器と占有ベイクだけ (波は 1 本も出さない)。伝播は M65b。
class AcousticField {
public:
    // 波 1 本。**全状態がここに閉じる** — 原点セル・現リング・振幅・音色があれば、
    // 距離場は占有グリッドから何度でも同じものが引き直せる。
    // ★member を足したら HashAcousticWaves と SimSnapshot の Write/ReadAcoustic の
    //   両方へ同時に足すこと (3 点セット契約)。blob レイアウトが変わるので
    //   kSimSnapshotVersion の bump も要る
    struct Wave {
        uint32_t active = 0;             // 0 = 空きスロット
        EntityID source = kNullEntity;   // 発音元 (自分の音を自分で聞かない除外に使う)
        int32_t ox = 0, oy = 0, oz = 0;  // 原点セル
        uint32_t ring = 0;               // 現在のリング (0 = 原点だけ確定)
        uint32_t maxRing = 0;            // ここまで進んだら消える
        uint32_t ticksPerRing = 1;       // 分周
        uint32_t phase = 0;              // 分周のカウンタ
        float amplitude = 0.0f;          // 原点での大きさ
        uint32_t tone = 0;               // 音色 0..3
        uint64_t bornTick = 0;           // 診断用 (どの tick に生まれたか)
    };

    // 同時に走れる波の本数。★満杯のときの Emit は**最古を潰さず false を返す** —
    // 潰す実装にすると「満杯時の挙動が到着順に依存する」= 決定論の穴になる
    static constexpr uint32_t kMaxWaves = 16;

    AcousticField();

    // 波ごとの距離場 (M65b)。★**導出値** — ハッシュにも snapshot にも入らない。
    // 復元後は Wave 表だけから ring 0 まで巻き戻して引き直す。
    // 「増分で育てた場」と「引き直した場」がビット同一であることが、この設計が
    // 成立している唯一の条件 (AcousticSelfTest が memcmp で固定する)
    struct WaveField {
        // 原点セルを含む局所ボックス (グリッドでクリップ済み)。
        // 面コストが 11 で最大距離が maxRing*11 なので、軸方向 maxRing セルより外へは届かない
        int32_t x0 = 0, y0 = 0, z0 = 0;
        int32_t sx = 0, sy = 0, sz = 0;
        uint32_t maxDist = 0;                     // maxRing * kFaceCost
        std::vector<uint16_t> dist;               // kUnreached = 未到達
        std::vector<uint8_t> parentDir;           // **親の方角** (kNeighbors の index)。kNoParent = 無し
        std::vector<std::vector<int32_t>> buckets; // Dial 法のバケット (index = チャンファ距離)
    };
    static constexpr uint16_t kUnreached = 0xFFFFu;
    static constexpr uint8_t kNoParent = 0xFFu;

    // ボリュームを探し、必要なら占有と距離場を焼き直す。音響フェーズ (3.4) の先頭で毎 tick。
    // **AcousticVolumeComponent が 1 個も無ければ最初の走査で return する** (存在ゲート)
    void Sync(World& world);

    // 波を 1 本立てる。**最小 index の空きスロット**を使う (決定論)。
    // ★満杯なら**最古を潰さず false を返す** — 潰す実装にすると「満杯時の挙動が
    //   到着順に依存する」= 決定論の穴になる。
    // 原点セルが閉じているとき (足元が床コライダの中など) は 26 近傍を表の順に探して
    // 最初に見つかった開セルへ寄せる。見つからなければ false
    bool Emit(EntityID source, float wx, float wy, float wz, float loudness, float radiusM,
              uint32_t tone, uint32_t ticksPerRing, uint64_t tick);

    // AcousticEmitterComponent の発音要求を波に変える。**entity.index 昇順**で処理し、
    // 消費した pendingLoudness は 0 へ戻す (エンジンが書く sim 状態)。
    // ★スロットが満杯なら要求は**捨てる** (次 tick へ持ち越さない) — 持ち越すと
    //   「いつ鳴るか」が過去の混雑具合に依存して、原因の遠い非決定性の温床になる
    void DrainEmitters(World& world, uint64_t tick);

    // 全アクティブ波を 1 tick ぶん進める (分周を見て 1 リング前進、maxRing 超えで消す)
    void Advance();

    // ---- 距離場の読み出し (描画 / AI / デバッグ線が使う) ----
    // グリッドセル -> その波でのチャンファ距離。範囲外・未到達は kUnreached
    uint16_t DistanceAt(uint32_t slot, int32_t cx, int32_t cy, int32_t cz) const;
    // 同じく親の方角 (kNeighbors の index)。原点と未到達は kNoParent
    uint8_t ParentDirAt(uint32_t slot, int32_t cx, int32_t cy, int32_t cz) const;
    const WaveField& FieldOf(uint32_t slot) const { return fields_[slot]; }

    // 全アクティブ波の距離場を ring 0 から引き直す。占有が焼き直された直後 (Sync) と、
    // snapshot 復元後の最初の Sync で必ず走る。★セルフテストはこれと増分成長の結果を
    // memcmp で突き合わせる
    void Rebuild();

    // シーン遷移時の全消し (TickRunner の LoadScene 反映ブロックから)
    void Reset();

    // 導出値を捨てて次の Sync で引き直させる。**snapshot 復元の直後に必ず呼ぶ**
    void Invalidate() { derivedValid_ = false; }

    bool HasVolume() const { return grid_.Valid(); }
    const AcousticGridDesc& Grid() const { return grid_; }
    EntityID VolumeOwner() const { return owner_; }
    int32_t NavCellRatio() const { return navRatio_; }
    bool DerivedValid() const { return derivedValid_; }
    uint64_t StaticSignature() const { return staticSig_; }

    // 占有 (0 = 開 / 1 = 閉)。**閉セルは波が絶対に訪れない**ので、壁面は
    // 「開セルと閉セルの境界」に現れる — これが「壁に当たった面だけが光る」の正体
    const std::vector<uint8_t>& Occupancy() const { return occupancy_; }
    bool IsSolid(int32_t cx, int32_t cy, int32_t cz) const;

    // 波スロット表。**書き換えてよいのは SimSnapshot / セルフテスト / 音響システム本体だけ**
    // (CpuParticleBackend::PoolsForSnapshot と同じ契約)
    const std::vector<Wave>& Waves() const { return waves_; }
    std::vector<Wave>& WavesForSnapshot() { return waves_; }
    bool AnyWaveActive() const;

    // ---- セルフテスト専用 ----
    // World を組まずに占有を直接与える (迷路を手組みして伝播を検査するため)
    void DebugSetGrid(const AcousticGridDesc& grid, std::vector<uint8_t> occupancy);

private:
    // 静的コライダを走査して占有を焼く。**コライダの AABB 内セルだけ**を形状判定に掛ける
    // (全セル x 全コライダを回すと 52 万 x 数百で即死する)
    void BakeOccupancy(World& world, uint32_t blockLayerMask);

    // ---- 距離場 (M65b) ----
    void SeedWave(uint32_t slot);            // 局所ボックスを確保して原点だけ置く
    void AdvanceWaveOneRing(uint32_t slot);  // バケット [ring*11, (ring+1)*11) を処理

    AcousticGridDesc grid_;             // 導出値 (ボリュームのコンポーネントから毎 tick 組む)
    EntityID owner_ = kNullEntity;      // 採用したボリューム (entity.index 最小の active な 1 個)
    int32_t navRatio_ = 2;              // 導出値
    std::vector<uint8_t> occupancy_;    // 導出値
    uint64_t staticSig_ = 0;            // 導出値。静的コライダの署名 (変化で焼き直す)
    bool derivedValid_ = false;         // 導出値が現在の world と整合しているか
    std::vector<WaveField> fields_;     // 導出値。waves_ と同じ長さ・同じ slot
    std::vector<Wave> waves_;           // ★**sim 状態**。常に kMaxWaves 本 (空きは active=0)
};

} // namespace mye
