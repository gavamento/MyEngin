#pragma once
#include <vector>

#include "Engine/Core/EntityID.h"

namespace mye {

class World;

// Transform 階層更新 (engine_spec.md 4.4)。
// 深度でソートされた線形配列を先頭から走査してワールド行列を更新する。
// 再帰は使わない (キャッシュ効率優先)。配列は構造変更時のみ再構築 (dirty フラグ)。
class TransformSystem {
public:
    void Update(World& world);

private:
    void Rebuild(World& world);

    std::vector<EntityID> sorted_; // (depth, EntityID.index) 昇順
};

} // namespace mye
