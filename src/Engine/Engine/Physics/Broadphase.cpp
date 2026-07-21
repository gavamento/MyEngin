#include "Engine/Engine/Physics/Broadphase.h"

#include <algorithm>
#include <numeric>

namespace mye {

void ComputeCandidatePairs(const std::vector<BroadphaseEntry>& entries,
                           std::vector<uint64_t>& outPairs)
{
    outPairs.clear();
    const size_t n = entries.size();
    if (n < 2) {
        return;
    }
    // (minX, id) でソート — float 同値タイは id で決定論化 (spec 11.2 規則 7)
    std::vector<uint32_t> order(n);
    std::iota(order.begin(), order.end(), 0u);
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        if (entries[a].minX != entries[b].minX) {
            return entries[a].minX < entries[b].minX;
        }
        return entries[a].id < entries[b].id;
    });
    // X スイープ: 自分の maxX を超える minX が現れたら打ち切り。Y/Z は重なりで枝刈り
    for (size_t i = 0; i < n; ++i) {
        const BroadphaseEntry& a = entries[order[i]];
        for (size_t j = i + 1; j < n; ++j) {
            const BroadphaseEntry& b = entries[order[j]];
            if (b.minX > a.maxX) {
                break; // minX 昇順なのでこれ以降は重ならない
            }
            if (b.minY > a.maxY || a.minY > b.maxY || b.minZ > a.maxZ || a.minZ > b.maxZ) {
                continue;
            }
            const uint32_t lo = (a.id < b.id) ? a.id : b.id;
            const uint32_t hi = (a.id < b.id) ? b.id : a.id;
            outPairs.push_back((static_cast<uint64_t>(lo) << 32) | hi);
        }
    }
    std::sort(outPairs.begin(), outPairs.end()); // 走査順を (小,大) 昇順に固定 (決定論)
}

} // namespace mye
