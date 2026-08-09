#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "Engine/Core/EntityID.h"

namespace mye {

class World;

// Transform 階層更新 (engine_spec.md 4.4)。
// 深度でソートされた線形配列を先頭から走査してワールド行列を更新する。
// 再帰は使わない (キャッシュ効率優先)。配列は構造変更時のみ再構築 (dirty フラグ)。
//
// M25 並列化: 同一深度のエンティティは親 (より浅い深度) が更新済みなら互いに独立に計算できる。
// そこで深度レベルを順に処理し、各レベル内を JobSystem で並列化する (レベル間はバリア)。
// WorldMatrixComponent は kComponentNoSerialize でハッシュ非対象 + レンジ非依存の独立書き込みな
// ので、直列と完全にビット一致する (jobs on/off で hash 不変 = 決定論契約を維持)。
//
// M51c 比較スキップ: LocalTransform は生ポインタで書かれるため dirty フラグは組めない。
// 代わりに前回 TRS (10 float) を側テーブルに保持し、ビット比較で不変 + 親も今 tick 不変なら
// ワールド行列を前回値のまま温存する (スキップ = 再計算とビット同一。WorldMatrixComponent の
// 書き手が本システム唯一で、他の参照が全て const 読みであることが前提)。構造変更 (生成/破棄/
// 再ペアレント/Clear) は必ず HierarchyDirty → Rebuild を通り側テーブルを全無効化するため、
// 親リンク変化やエンティティスロット再利用の取りこぼしは構造的に起きない。
// World::SimCacheEnabled() == false で従来の全件再計算に素通しする (M51a と同じ A/B ゲート)。
class TransformSystem {
public:
    void Update(World& world);

    // 直近 Update の再計算/スキップ件数 (M51c の selftest / プロファイル用)
    struct Stats {
        uint32_t computed = 0;
        uint32_t skipped = 0;
    };
    const Stats& LastStats() const { return stats_; }

private:
    void Rebuild(World& world);
    void UpdateCached(World& world, EntityID e);

    std::vector<EntityID> sorted_; // (depth, EntityID.index) 昇順
    // sorted_ 内の深度レベル境界 [begin,end)。Rebuild で算出 (同深度は連続)。
    std::vector<std::pair<size_t, size_t>> levels_;

    // ---- M51c: 比較スキップの側テーブル (EntityID.index 添字、Rebuild で全無効化) ----
    struct TrsBits {
        float v[10]; // LocalTransform の position(3) + rotation(4) + scale(3) 生ビット
    };
    static constexpr uint8_t kSlotValid = 1;   // lastTrs_[i] が有効
    static constexpr uint8_t kSlotChanged = 2; // 今 tick 再計算した (子の据え置き判定に使う)
    std::vector<TrsBits> lastTrs_;
    // 並列時も各エンティティは自分のスロットのみ書き、親のスロットは前レベル (バリア済み)
    // の値を読むだけ。uint8_t の隣接要素は別オブジェクトなのでデータ競合にならない
    std::vector<uint8_t> state_;
    Stats stats_;
};

} // namespace mye
