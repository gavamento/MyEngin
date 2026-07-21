#pragma once
#include <cstddef>
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
class TransformSystem {
public:
    void Update(World& world);

private:
    void Rebuild(World& world);

    std::vector<EntityID> sorted_; // (depth, EntityID.index) 昇順
    // sorted_ 内の深度レベル境界 [begin,end)。Rebuild で算出 (同深度は連続)。
    std::vector<std::pair<size_t, size_t>> levels_;
};

} // namespace mye
