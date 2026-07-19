#pragma once
#include <vector>

#include "Engine/Core/EntityID.h"

namespace mye {

class World;
class ScriptHost;

// 簡易衝突判定 (球 / AABB オーバーラップ + トリガーイベント)。
// 毎 tick、Transform 確定後に総当たりで判定し、前 tick とのペア差分から
// OnTriggerEnter / OnTriggerExit を C ABI 経由でスクリプトへ配信する (spec 1.4 [決定済み])。
// ペアは (小 index, 大 index) の明示キーでソート — 配信順も決定論 (spec 11.2 規則 7)
class CollisionSystem {
public:
    void Update(World& world, ScriptHost* scripts);
    void Reset() { prevPairs_.clear(); }

private:
    std::vector<uint64_t> prevPairs_; // (aIdx<<32)|bIdx, aIdx<bIdx、昇順
};

} // namespace mye
