#pragma once
// ブロードフェーズ (M28d)。毎 tick 再構築の 1 軸 (X) ソート & スイープ。
// 永続 SAP の増分状態を持たないステートレス設計 (エンジンの物理システムと同じ哲学)。
// 決定論契約: ソートキーは (minX, id) の明示 tie-break、出力ペアは (小 id << 32 | 大 id)
// の昇順。候補列は真の接触ペア集合の純粋なスーパーセット (AABB 重なりは包含判定)。

#include <cstdint>
#include <vector>

namespace mye {

struct BroadphaseEntry {
    uint32_t id = 0; // 呼び出し側のインデックス (ペアキーに使われる)
    float minX = 0, minY = 0, minZ = 0;
    float maxX = 0, maxY = 0, maxZ = 0;
};

// X スイープ + Y/Z 重なり枝刈りで候補ペアを列挙する。
// outPairs は (小 id << 32) | 大 id の昇順 (呼び出し側の走査順が決定論になる)。
void ComputeCandidatePairs(const std::vector<BroadphaseEntry>& entries,
                           std::vector<uint64_t>& outPairs);

} // namespace mye
