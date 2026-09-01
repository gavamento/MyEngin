//====================================================================================
//                          AcousticGrid.cpp
//  MyEngine/ 秋田蓮音                                                      09/01/2026
//                                          音響ボクセルグリッドの座標変換と 26 近傍表の実体
//====================================================================================
#include "Engine/Engine/Acoustic/AcousticGrid.h"

#include <cmath>

#include "Engine/Core/Log.h"

namespace mye {
namespace acoustic {

// (dz, dy, dx) の辞書順で中心 (0,0,0) だけを抜いた 26 本。
// 重みは非ゼロ成分の数で決まる (1 = 面 / 2 = 辺 / 3 = 角)。
// ★この並びは**ハッシュにも blob にも直接は入らない**が、伝播の訪問順を決めるので
//   実質的に決定論の一部。並べ替えると同コストのセルの親が変わり、AI が聞く方向が動く
const Neighbor kNeighbors[kNeighborCount] = {
    { -1, -1, -1, kCornerCost }, { 0, -1, -1, kEdgeCost },   { 1, -1, -1, kCornerCost },
    { -1, 0, -1, kEdgeCost },    { 0, 0, -1, kFaceCost },    { 1, 0, -1, kEdgeCost },
    { -1, 1, -1, kCornerCost },  { 0, 1, -1, kEdgeCost },    { 1, 1, -1, kCornerCost },
    { -1, -1, 0, kEdgeCost },    { 0, -1, 0, kFaceCost },    { 1, -1, 0, kEdgeCost },
    { -1, 0, 0, kFaceCost },     { 1, 0, 0, kFaceCost },     { -1, 1, 0, kEdgeCost },
    { 0, 1, 0, kFaceCost },      { 1, 1, 0, kEdgeCost },     { -1, -1, 1, kCornerCost },
    { 0, -1, 1, kEdgeCost },     { 1, -1, 1, kCornerCost },  { -1, 0, 1, kEdgeCost },
    { 0, 0, 1, kFaceCost },      { 1, 0, 1, kEdgeCost },     { -1, 1, 1, kCornerCost },
    { 0, 1, 1, kEdgeCost },      { 1, 1, 1, kCornerCost },
};

bool WorldToCell(const AcousticGridDesc& g, float x, float y, float z, int32_t& outX,
                 int32_t& outY, int32_t& outZ)
{
    if (!g.Valid()) {
        return false;
    }
    // std::floor は /fp:precise では構成に依らず同じ値を返す。負側を切り捨てで潰すと
    // 原点をまたいだ 2 セルが同じ index に落ちるので、必ず floor を通すこと
    const float fx = std::floor((x - g.minX) / g.cellSize);
    const float fy = std::floor((y - g.minY) / g.cellSize);
    const float fz = std::floor((z - g.minZ) / g.cellSize);
    // int32 へ落とす前に範囲で弾く (巨大座標の変換は未定義動作になる)
    if (fx < 0.0f || fy < 0.0f || fz < 0.0f || fx >= static_cast<float>(g.dimX)
        || fy >= static_cast<float>(g.dimY) || fz >= static_cast<float>(g.dimZ)) {
        return false;
    }
    outX = static_cast<int32_t>(fx);
    outY = static_cast<int32_t>(fy);
    outZ = static_cast<int32_t>(fz);
    return true;
}

void CellToWorldCenter(const AcousticGridDesc& g, int32_t cx, int32_t cy, int32_t cz, float& outX,
                       float& outY, float& outZ)
{
    outX = g.minX + (static_cast<float>(cx) + 0.5f) * g.cellSize;
    outY = g.minY + (static_cast<float>(cy) + 0.5f) * g.cellSize;
    outZ = g.minZ + (static_cast<float>(cz) + 0.5f) * g.cellSize;
}

bool MakeGridDesc(int32_t dimX, int32_t dimY, int32_t dimZ, float cellSize, float centerX,
                  float centerY, float centerZ, AcousticGridDesc& out)
{
    if (dimX <= 0 || dimY <= 0 || dimZ <= 0 || !(cellSize > 0.0f)) {
        return false;
    }
    if (dimX > kMaxDim || dimY > kMaxDim || dimZ > kMaxDim) {
        MYE_LOG_WARN("[acoustic] grid dimension exceeds %d (%d, %d, %d)", kMaxDim, dimX, dimY,
                     dimZ);
        return false;
    }
    const int64_t cells = static_cast<int64_t>(dimX) * dimY * dimZ;
    if (cells > kMaxCells) {
        MYE_LOG_WARN("[acoustic] grid has %lld cells, over the %lld cap",
                     static_cast<long long>(cells), static_cast<long long>(kMaxCells));
        return false;
    }
    out.dimX = dimX;
    out.dimY = dimY;
    out.dimZ = dimZ;
    out.cellSize = cellSize;
    // 中心を与えて最小角を導く。dim が奇数のときも「中心セルの中心が center」ではなく
    // 「箱の中心が center」— セルの境界と中心を混ぜないための一貫した規約
    out.minX = centerX - static_cast<float>(dimX) * cellSize * 0.5f;
    out.minY = centerY - static_cast<float>(dimY) * cellSize * 0.5f;
    out.minZ = centerZ - static_cast<float>(dimZ) * cellSize * 0.5f;
    return true;
}

bool SameGrid(const AcousticGridDesc& a, const AcousticGridDesc& b)
{
    return a.dimX == b.dimX && a.dimY == b.dimY && a.dimZ == b.dimZ && a.cellSize == b.cellSize
        && a.minX == b.minX && a.minY == b.minY && a.minZ == b.minZ;
}

} // namespace acoustic
} // namespace mye
