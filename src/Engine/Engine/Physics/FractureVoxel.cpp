//====================================================================================
//                          FractureVoxel.cpp
//  MyEngin/ 秋田蓮音                                                     09/25/2026
//                                          開いたメッシュのボクセル化+surface nets の実装
//====================================================================================
#include "Engine/Engine/Physics/FractureVoxel.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace DirectX;

namespace mye {
namespace {

// ---- グリッド座標 (解像度可変。Modal/Voxelizer.h の 32^3 固定グリッドとは別実装) ----
struct GridDims {
    int32_t nx = 0, ny = 0, nz = 0;
    int64_t Total() const { return static_cast<int64_t>(nx) * ny * nz; }
};

int64_t IndexOf(const GridDims& d, int32_t x, int32_t y, int32_t z)
{
    return static_cast<int64_t>(x) + static_cast<int64_t>(d.nx) * (static_cast<int64_t>(y) + static_cast<int64_t>(d.ny) * z);
}

// 外周に置く空きセルの層数 (spec: 1 セル以上)。内部セル数の外側にこの枚数だけ余白を足すので、
// AABB に触れる三角形が外周セルへ及ぶことは構造的に無い (内部セルが AABB を覆いきる)
constexpr int32_t kPadCells = 1;

// AABB・セルサイズ・グリッド寸法を決める。縮退 (頂点0件 / 全頂点が同一点で最長辺が0) は false
bool ComputeGridFrame(const FractureMesh& mesh, int32_t resolution, XMFLOAT3& origin, float& h,
                      GridDims& dims, int32_t& resolutionUsed)
{
    if (mesh.verts.empty()) {
        return false;
    }
    XMFLOAT3 lo = mesh.verts[0].position;
    XMFLOAT3 hi = lo;
    for (const FractureVertex& v : mesh.verts) {
        lo.x = (std::min)(lo.x, v.position.x);
        lo.y = (std::min)(lo.y, v.position.y);
        lo.z = (std::min)(lo.z, v.position.z);
        hi.x = (std::max)(hi.x, v.position.x);
        hi.y = (std::max)(hi.y, v.position.y);
        hi.z = (std::max)(hi.z, v.position.z);
    }
    const float ex = hi.x - lo.x, ey = hi.y - lo.y, ez = hi.z - lo.z;
    const float longest = (std::max)(ex, (std::max)(ey, ez));
    if (!(longest > 0.0f)) {
        return false; // 縮退メッシュ (単一点等)
    }
    resolutionUsed = std::clamp(resolution, kFractureVoxelMinResolution, kFractureVoxelMaxResolution);
    h = longest / static_cast<float>(resolutionUsed);

    // 各軸の内部セル数 = ceil(extent/h) (最短 1)。extent が h の整数倍に近いと内部領域の境界が
    // AABB とぴったり重なり、パディングセルが SAT のマージン (1e-6*h) で誤って占有される。
    // 余白がセル 0.1 個分未満なら 1 セル足して境界からの余白を常に確保する
    auto cellsFor = [&](float extent) {
        const float raw = extent / h;
        int32_t cells = (std::max)(1, static_cast<int32_t>(std::ceil(raw)));
        if (static_cast<float>(cells) - raw < 0.1f) {
            ++cells;
        }
        return cells;
    };
    const int32_t cx = cellsFor(ex), cy = cellsFor(ey), cz = cellsFor(ez);
    dims = { cx + 2 * kPadCells, cy + 2 * kPadCells, cz + 2 * kPadCells };

    const XMFLOAT3 center{ (lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f };
    origin = {
        center.x - (static_cast<float>(cx) * 0.5f + kPadCells) * h,
        center.y - (static_cast<float>(cy) * 0.5f + kPadCells) * h,
        center.z - (static_cast<float>(cz) * 0.5f + kPadCells) * h,
    };
    return true;
}

// サンプル点 (セル中心) の位置。surface nets はこの位置を頂点位置の平均元にする
XMFLOAT3 SamplePos(const XMFLOAT3& origin, float h, int32_t x, int32_t y, int32_t z)
{
    return { origin.x + (static_cast<float>(x) + 0.5f) * h, origin.y + (static_cast<float>(y) + 0.5f) * h,
             origin.z + (static_cast<float>(z) + 0.5f) * h };
}

// ---- Akenine-Möller の三角形/箱オーバーラップ判定 (9 軸: 辺×箱軸 + 1 軸: 面法線 + 3 軸: AABB)。
// Modal/Voxelizer.cpp と同じアルゴリズムだが、Deep-Modal と契約を共有しない別モジュールなので
// 独立実装として持つ (共有しない、sub-04 の既定判断) ----
bool AxisTestX(float a, float b, float fa, float fb, const float v0[3], const float v1[3],
              const float boxHalf[3])
{
    const float p0 = a * v0[1] - b * v0[2];
    const float p1 = a * v1[1] - b * v1[2];
    const float minP = (std::min)(p0, p1);
    const float maxP = (std::max)(p0, p1);
    const float rad = fa * boxHalf[1] + fb * boxHalf[2];
    return !(minP > rad || maxP < -rad);
}

bool AxisTestY(float a, float b, float fa, float fb, const float v0[3], const float v1[3],
              const float boxHalf[3])
{
    const float p0 = -a * v0[0] + b * v0[2];
    const float p1 = -a * v1[0] + b * v1[2];
    const float minP = (std::min)(p0, p1);
    const float maxP = (std::max)(p0, p1);
    const float rad = fa * boxHalf[0] + fb * boxHalf[2];
    return !(minP > rad || maxP < -rad);
}

bool AxisTestZ(float a, float b, float fa, float fb, const float v0[3], const float v1[3],
              const float boxHalf[3])
{
    const float p0 = a * v0[0] - b * v0[1];
    const float p1 = a * v1[0] - b * v1[1];
    const float minP = (std::min)(p0, p1);
    const float maxP = (std::max)(p0, p1);
    const float rad = fa * boxHalf[0] + fb * boxHalf[1];
    return !(minP > rad || maxP < -rad);
}

bool PlaneBoxOverlap(const float normal[3], const float vert[3], const float boxHalf[3])
{
    float vmin[3], vmax[3];
    for (int q = 0; q < 3; ++q) {
        const float v = vert[q];
        if (normal[q] > 0.0f) {
            vmin[q] = -boxHalf[q] - v;
            vmax[q] = boxHalf[q] - v;
        } else {
            vmin[q] = boxHalf[q] - v;
            vmax[q] = -boxHalf[q] - v;
        }
    }
    const float dotMin = normal[0] * vmin[0] + normal[1] * vmin[1] + normal[2] * vmin[2];
    if (dotMin > 0.0f) {
        return false;
    }
    const float dotMax = normal[0] * vmax[0] + normal[1] * vmax[1] + normal[2] * vmax[2];
    return dotMax >= 0.0f;
}

bool TriBoxOverlap(const float boxCenter[3], const float boxHalfSize[3], const float tri[3][3])
{
    const float v0[3] = { tri[0][0] - boxCenter[0], tri[0][1] - boxCenter[1], tri[0][2] - boxCenter[2] };
    const float v1[3] = { tri[1][0] - boxCenter[0], tri[1][1] - boxCenter[1], tri[1][2] - boxCenter[2] };
    const float v2[3] = { tri[2][0] - boxCenter[0], tri[2][1] - boxCenter[1], tri[2][2] - boxCenter[2] };

    const float e0[3] = { v1[0] - v0[0], v1[1] - v0[1], v1[2] - v0[2] };
    const float e1[3] = { v2[0] - v1[0], v2[1] - v1[1], v2[2] - v1[2] };
    const float e2[3] = { v0[0] - v2[0], v0[1] - v2[1], v0[2] - v2[2] };

    float fe[3] = { std::fabs(e0[0]), std::fabs(e0[1]), std::fabs(e0[2]) };
    if (!AxisTestX(e0[2], e0[1], fe[2], fe[1], v0, v2, boxHalfSize)) {
        return false;
    }
    if (!AxisTestY(e0[2], e0[0], fe[2], fe[0], v0, v2, boxHalfSize)) {
        return false;
    }
    if (!AxisTestZ(e0[1], e0[0], fe[1], fe[0], v1, v2, boxHalfSize)) {
        return false;
    }

    fe[0] = std::fabs(e1[0]);
    fe[1] = std::fabs(e1[1]);
    fe[2] = std::fabs(e1[2]);
    if (!AxisTestX(e1[2], e1[1], fe[2], fe[1], v0, v2, boxHalfSize)) {
        return false;
    }
    if (!AxisTestY(e1[2], e1[0], fe[2], fe[0], v0, v2, boxHalfSize)) {
        return false;
    }
    if (!AxisTestZ(e1[1], e1[0], fe[1], fe[0], v0, v1, boxHalfSize)) {
        return false;
    }

    fe[0] = std::fabs(e2[0]);
    fe[1] = std::fabs(e2[1]);
    fe[2] = std::fabs(e2[2]);
    if (!AxisTestX(e2[2], e2[1], fe[2], fe[1], v0, v1, boxHalfSize)) {
        return false;
    }
    if (!AxisTestY(e2[2], e2[0], fe[2], fe[0], v0, v1, boxHalfSize)) {
        return false;
    }
    if (!AxisTestZ(e2[1], e2[0], fe[1], fe[0], v1, v2, boxHalfSize)) {
        return false;
    }

    for (int ax = 0; ax < 3; ++ax) {
        const float mn = (std::min)({ v0[ax], v1[ax], v2[ax] });
        const float mx = (std::max)({ v0[ax], v1[ax], v2[ax] });
        if (mn > boxHalfSize[ax] || mx < -boxHalfSize[ax]) {
            return false;
        }
    }

    const float normal[3] = { e0[1] * e1[2] - e0[2] * e1[1], e0[2] * e1[0] - e0[0] * e1[2],
                              e0[0] * e1[1] - e0[1] * e1[0] };
    return PlaneBoxOverlap(normal, v0, boxHalfSize);
}

// ---- 占有格子 ----
void MarkOccupancy(const FractureMesh& mesh, const XMFLOAT3& origin, float h, const GridDims& dims,
                   std::vector<uint8_t>& occ)
{
    const float halfBox = h * 0.5f + 1.0e-6f * h;
    const float boxHalf[3] = { halfBox, halfBox, halfBox };
    const float originArr[3] = { origin.x, origin.y, origin.z };
    const int32_t dimsArr[3] = { dims.nx, dims.ny, dims.nz };
    auto clampAxis = [](int32_t v, int32_t n) { return (std::max)(0, (std::min)(n - 1, v)); };

    const int32_t triCount = mesh.TriCount();
    for (int32_t t = 0; t < triCount; ++t) {
        const XMFLOAT3& p0 = mesh.verts[static_cast<size_t>(mesh.indices[static_cast<size_t>(t) * 3 + 0])].position;
        const XMFLOAT3& p1 = mesh.verts[static_cast<size_t>(mesh.indices[static_cast<size_t>(t) * 3 + 1])].position;
        const XMFLOAT3& p2 = mesh.verts[static_cast<size_t>(mesh.indices[static_cast<size_t>(t) * 3 + 2])].position;
        const float tri[3][3] = { { p0.x, p0.y, p0.z }, { p1.x, p1.y, p1.z }, { p2.x, p2.y, p2.z } };
        const float triLo[3] = { (std::min)({ p0.x, p1.x, p2.x }), (std::min)({ p0.y, p1.y, p2.y }),
                                 (std::min)({ p0.z, p1.z, p2.z }) };
        const float triHi[3] = { (std::max)({ p0.x, p1.x, p2.x }), (std::max)({ p0.y, p1.y, p2.y }),
                                 (std::max)({ p0.z, p1.z, p2.z }) };

        int32_t lo[3], hi[3];
        for (int ax = 0; ax < 3; ++ax) {
            const float loF = (triLo[ax] - originArr[ax]) / h - 1.0f;
            const float hiF = (triHi[ax] - originArr[ax]) / h + 1.0f;
            lo[ax] = clampAxis(static_cast<int32_t>(std::floor(loF)), dimsArr[ax]);
            hi[ax] = clampAxis(static_cast<int32_t>(std::ceil(hiF)), dimsArr[ax]);
        }

        for (int32_t z = lo[2]; z <= hi[2]; ++z) {
            for (int32_t y = lo[1]; y <= hi[1]; ++y) {
                for (int32_t x = lo[0]; x <= hi[0]; ++x) {
                    const int64_t vi = IndexOf(dims, x, y, z);
                    if (occ[static_cast<size_t>(vi)]) {
                        continue; // 既に占有
                    }
                    const float boxCenter[3] = { origin.x + (x + 0.5f) * h, origin.y + (y + 0.5f) * h,
                                                 origin.z + (z + 0.5f) * h };
                    if (TriBoxOverlap(boxCenter, boxHalf, tri)) {
                        occ[static_cast<size_t>(vi)] = 1;
                    }
                }
            }
        }
    }
}

// pad リング (外周1層。ComputeGridFrame の余白により必ず非占有) から6近傍で外部を塗り、
// 到達しない非表面セルを内部として occ に立てる。明示スタック + 固定順 (unordered を
// 使わない) なので同入力 → 同出力 (Modal/Voxelizer.cpp の FloodFillInterior と同じ考え方)
void FloodFillInterior(const GridDims& dims, std::vector<uint8_t>& occ)
{
    const int64_t total = dims.Total();
    const int64_t planeSize = static_cast<int64_t>(dims.nx) * dims.ny;
    std::vector<uint8_t> visited(static_cast<size_t>(total), 0);
    std::vector<int64_t> stack;
    stack.reserve(static_cast<size_t>(total / 4 + 1));

    for (int32_t z = 0; z < dims.nz; ++z) {
        for (int32_t y = 0; y < dims.ny; ++y) {
            for (int32_t x = 0; x < dims.nx; ++x) {
                const bool onPad = (x == 0 || x == dims.nx - 1 || y == 0 || y == dims.ny - 1 || z == 0
                                    || z == dims.nz - 1);
                if (!onPad) {
                    continue;
                }
                const int64_t idx = IndexOf(dims, x, y, z);
                if (!occ[static_cast<size_t>(idx)] && !visited[static_cast<size_t>(idx)]) {
                    visited[static_cast<size_t>(idx)] = 1;
                    stack.push_back(idx);
                }
            }
        }
    }

    static constexpr int32_t kDx[6] = { -1, 1, 0, 0, 0, 0 };
    static constexpr int32_t kDy[6] = { 0, 0, -1, 1, 0, 0 };
    static constexpr int32_t kDz[6] = { 0, 0, 0, 0, -1, 1 };
    while (!stack.empty()) {
        const int64_t idx = stack.back();
        stack.pop_back();
        const int32_t z = static_cast<int32_t>(idx / planeSize);
        const int64_t rem = idx % planeSize;
        const int32_t y = static_cast<int32_t>(rem / dims.nx);
        const int32_t x = static_cast<int32_t>(rem % dims.nx);
        for (int k = 0; k < 6; ++k) {
            const int32_t nx = x + kDx[k], ny = y + kDy[k], nz = z + kDz[k];
            if (nx < 0 || nx >= dims.nx || ny < 0 || ny >= dims.ny || nz < 0 || nz >= dims.nz) {
                continue;
            }
            const int64_t nidx = IndexOf(dims, nx, ny, nz);
            if (occ[static_cast<size_t>(nidx)] || visited[static_cast<size_t>(nidx)]) {
                continue;
            }
            visited[static_cast<size_t>(nidx)] = 1;
            stack.push_back(nidx);
        }
    }

    for (int64_t i = 0; i < total; ++i) {
        if (!occ[static_cast<size_t>(i)] && !visited[static_cast<size_t>(i)]) {
            occ[static_cast<size_t>(i)] = 1;
        }
    }
}

// ---- 曖昧な配置 (2x2 の対角占有) の事前解消 ----
// 格子面 (3方向いずれか) を張る隣接4セルのうち、対角のペアだけが占有される配置は、
// その面を挟む2キューブが独立に「どちら向きに繋ぐか」を決めることになり、非多様体の辺を生む。
// メッシュ化の前に、空きセルのうち格子 index が最小のものを占有にして解消する
enum class GroupAxis : int8_t { kFixedX = 0, kFixedY = 1, kFixedZ = 2 };

// 2x2 グループ。axis の固定座標 (kFixedX なら x) と、面内の低い方の格子座標 (u, v)
struct AmbiguousGroup {
    GroupAxis axis;
    int32_t fixed, u, v;
};

// グループの4セルの格子座標を (u,v)=(0,0),(1,0),(0,1),(1,1) の順で返す (対角は 0↔3、1↔2)
void GroupCellCoords(const AmbiguousGroup& g, int32_t coord[4][3])
{
    const int32_t base[4][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } };
    for (int c = 0; c < 4; ++c) {
        switch (g.axis) {
        case GroupAxis::kFixedX:
            coord[c][0] = g.fixed;
            coord[c][1] = g.u + base[c][0];
            coord[c][2] = g.v + base[c][1];
            break;
        case GroupAxis::kFixedY:
            coord[c][0] = g.u + base[c][0];
            coord[c][1] = g.fixed;
            coord[c][2] = g.v + base[c][1];
            break;
        case GroupAxis::kFixedZ:
            coord[c][0] = g.u + base[c][0];
            coord[c][1] = g.v + base[c][1];
            coord[c][2] = g.fixed;
            break;
        }
    }
}

// セル (x,y,z) を含みうる最大12グループを固定順で積む (kFixedX→Y→Z、各々 u,v は小さい方から)
void AppendGroupsContainingCell(int32_t x, int32_t y, int32_t z, const GridDims& dims,
                                std::vector<AmbiguousGroup>& out)
{
    for (int32_t du = -1; du <= 0; ++du) {
        const int32_t u = y + du;
        if (u < 0 || u > dims.ny - 2) {
            continue;
        }
        for (int32_t dv = -1; dv <= 0; ++dv) {
            const int32_t v = z + dv;
            if (v < 0 || v > dims.nz - 2) {
                continue;
            }
            out.push_back({ GroupAxis::kFixedX, x, u, v });
        }
    }
    for (int32_t du = -1; du <= 0; ++du) {
        const int32_t u = x + du;
        if (u < 0 || u > dims.nx - 2) {
            continue;
        }
        for (int32_t dv = -1; dv <= 0; ++dv) {
            const int32_t v = z + dv;
            if (v < 0 || v > dims.nz - 2) {
                continue;
            }
            out.push_back({ GroupAxis::kFixedY, y, u, v });
        }
    }
    for (int32_t du = -1; du <= 0; ++du) {
        const int32_t u = x + du;
        if (u < 0 || u > dims.nx - 2) {
            continue;
        }
        for (int32_t dv = -1; dv <= 0; ++dv) {
            const int32_t v = y + dv;
            if (v < 0 || v > dims.ny - 2) {
                continue;
            }
            out.push_back({ GroupAxis::kFixedZ, z, u, v });
        }
    }
}

// 1 セルの反映 (占有への反転) がどこまで波及するかは事前に読めないので、影響を受け得る
// グループだけをその場でスタックに積んで汲み尽くす (グリッド全体を並べたキューは持たない)。
// 占有は単調に増えるだけなので必ず止まる (spec §4.1)
void ResolveAmbiguousConfigurations(const GridDims& dims, std::vector<uint8_t>& occ)
{
    std::vector<AmbiguousGroup> ripple;

    auto tryFix = [&](const AmbiguousGroup& g) {
        int32_t coord[4][3];
        GroupCellCoords(g, coord);
        int64_t idx[4];
        uint8_t o[4];
        for (int c = 0; c < 4; ++c) {
            idx[c] = IndexOf(dims, coord[c][0], coord[c][1], coord[c][2]);
            o[c] = occ[static_cast<size_t>(idx[c])];
        }
        if (!(o[0] == o[3] && o[1] == o[2] && o[0] != o[1])) {
            return; // 対角配置ではない
        }
        int64_t bestIdx = -1;
        int32_t bestCoord[3] = { 0, 0, 0 };
        for (int c = 0; c < 4; ++c) {
            if (o[c] != 0) {
                continue;
            }
            if (bestIdx < 0 || idx[c] < bestIdx) {
                bestIdx = idx[c];
                bestCoord[0] = coord[c][0];
                bestCoord[1] = coord[c][1];
                bestCoord[2] = coord[c][2];
            }
        }
        if (bestIdx < 0) {
            return; // 対角判定が真なら空きは必ず2個あるはずだが、念のため
        }
        occ[static_cast<size_t>(bestIdx)] = 1;
        AppendGroupsContainingCell(bestCoord[0], bestCoord[1], bestCoord[2], dims, ripple);
    };

    // 全グループを固定順で1回ずつ直接確認する。直した影響 (ripple) は次のグループへ
    // 進む前に汲み尽くすので、あとから曖昧になったグループも取りこぼさない
    for (int32_t x = 0; x < dims.nx; ++x) {
        for (int32_t u = 0; u <= dims.ny - 2; ++u) {
            for (int32_t v = 0; v <= dims.nz - 2; ++v) {
                tryFix({ GroupAxis::kFixedX, x, u, v });
                while (!ripple.empty()) {
                    const AmbiguousGroup g = ripple.back();
                    ripple.pop_back();
                    tryFix(g);
                }
            }
        }
    }
    for (int32_t y = 0; y < dims.ny; ++y) {
        for (int32_t u = 0; u <= dims.nx - 2; ++u) {
            for (int32_t v = 0; v <= dims.nz - 2; ++v) {
                tryFix({ GroupAxis::kFixedY, y, u, v });
                while (!ripple.empty()) {
                    const AmbiguousGroup g = ripple.back();
                    ripple.pop_back();
                    tryFix(g);
                }
            }
        }
    }
    for (int32_t z = 0; z < dims.nz; ++z) {
        for (int32_t u = 0; u <= dims.nx - 2; ++u) {
            for (int32_t v = 0; v <= dims.ny - 2; ++v) {
                tryFix({ GroupAxis::kFixedZ, z, u, v });
                while (!ripple.empty()) {
                    const AmbiguousGroup g = ripple.back();
                    ripple.pop_back();
                    tryFix(g);
                }
            }
        }
    }
}

// ---- surface nets ----
// 双対セル (占有格子の隣接8サンプルからなるキューブ) の格子。cx∈[0,dims.nx-2] 等
struct CubeDims {
    int32_t nx = 0, ny = 0, nz = 0;
    int64_t Total() const { return static_cast<int64_t>(nx) * ny * nz; }
};

int64_t CubeIndexOf(const CubeDims& c, int32_t x, int32_t y, int32_t z)
{
    return static_cast<int64_t>(x) + static_cast<int64_t>(c.nx) * (static_cast<int64_t>(y) + static_cast<int64_t>(c.ny) * z);
}

// キューブの12辺 (8隅を (di,dj,dk)∈{0,1}^3 の2点で結ぶ)
constexpr int32_t kCubeEdges[12][2][3] = {
    { { 0, 0, 0 }, { 1, 0, 0 } }, { { 0, 1, 0 }, { 1, 1, 0 } }, { { 0, 0, 1 }, { 1, 0, 1 } },
    { { 0, 1, 1 }, { 1, 1, 1 } }, // X方向4本
    { { 0, 0, 0 }, { 0, 1, 0 } }, { { 1, 0, 0 }, { 1, 1, 0 } }, { { 0, 0, 1 }, { 0, 1, 1 } },
    { { 1, 0, 1 }, { 1, 1, 1 } }, // Y方向4本
    { { 0, 0, 0 }, { 0, 0, 1 } }, { { 1, 0, 0 }, { 1, 0, 1 } }, { { 0, 1, 0 }, { 0, 1, 1 } },
    { { 1, 1, 0 }, { 1, 1, 1 } }, // Z方向4本
};

// 8隅の占有が混在するキューブ (双対セル) ごとに頂点を1つ置く。位置は、12辺のうち
// 占有が変わる辺の中点の平均 (反復の平滑化はしない、純関数で決定的)
void ComputeActiveCubeVertices(const GridDims& dims, const std::vector<uint8_t>& occ, const XMFLOAT3& origin,
                               float h, CubeDims& cubeDims, std::vector<uint8_t>& active,
                               std::vector<XMFLOAT3>& vertexPos)
{
    cubeDims = { dims.nx - 1, dims.ny - 1, dims.nz - 1 };
    const int64_t total = cubeDims.Total();
    active.assign(static_cast<size_t>(total), 0);
    vertexPos.assign(static_cast<size_t>(total), XMFLOAT3{ 0, 0, 0 });

    for (int32_t cz = 0; cz < cubeDims.nz; ++cz) {
        for (int32_t cy = 0; cy < cubeDims.ny; ++cy) {
            for (int32_t cx = 0; cx < cubeDims.nx; ++cx) {
                uint8_t corner[2][2][2];
                for (int di = 0; di < 2; ++di) {
                    for (int dj = 0; dj < 2; ++dj) {
                        for (int dk = 0; dk < 2; ++dk) {
                            corner[di][dj][dk]
                                = occ[static_cast<size_t>(IndexOf(dims, cx + di, cy + dj, cz + dk))];
                        }
                    }
                }
                bool allSame = true;
                for (int di = 0; di < 2 && allSame; ++di) {
                    for (int dj = 0; dj < 2 && allSame; ++dj) {
                        for (int dk = 0; dk < 2 && allSame; ++dk) {
                            if (corner[di][dj][dk] != corner[0][0][0]) {
                                allSame = false;
                            }
                        }
                    }
                }
                if (allSame) {
                    continue;
                }

                double sx = 0.0, sy = 0.0, sz = 0.0;
                int32_t crossCount = 0;
                for (const auto& e : kCubeEdges) {
                    const uint8_t oa = corner[e[0][0]][e[0][1]][e[0][2]];
                    const uint8_t ob = corner[e[1][0]][e[1][1]][e[1][2]];
                    if (oa == ob) {
                        continue;
                    }
                    const XMFLOAT3 pa = SamplePos(origin, h, cx + e[0][0], cy + e[0][1], cz + e[0][2]);
                    const XMFLOAT3 pb = SamplePos(origin, h, cx + e[1][0], cy + e[1][1], cz + e[1][2]);
                    sx += (static_cast<double>(pa.x) + pb.x) * 0.5;
                    sy += (static_cast<double>(pa.y) + pb.y) * 0.5;
                    sz += (static_cast<double>(pa.z) + pb.z) * 0.5;
                    ++crossCount;
                }
                // 8隅が全て同じではない (allSame==false) ので、辺で連結された8隅のどこかに
                // 必ず占有の変化がある (crossCount >= 1 が保証される)
                const int64_t ci = CubeIndexOf(cubeDims, cx, cy, cz);
                active[static_cast<size_t>(ci)] = 1;
                vertexPos[static_cast<size_t>(ci)] = { static_cast<float>(sx / crossCount),
                                                       static_cast<float>(sy / crossCount),
                                                       static_cast<float>(sz / crossCount) };
            }
        }
    }
}

// 占有が変わるサンプル辺 (軸方向) を挟む最大4キューブの座標を、外向き (occupied→empty) から
// 見て CCW になる順で返す。周回パターンは FractureBake.cpp の MakeBoxFaces / 旧ブロック抽出の
// kDirs と同じ規則 (X/Z 軸は (0,0)→(1,0)→(1,1)→(0,1)、Y 軸だけ (0,0)→(0,1)→(1,1)→(1,0)。
// 逆向きは同じ周回を逆順にたどるだけ) — 「セルのオフセット (0/1)」を「隣接キューブの
// オフセット (-1/0)」に読み替えて使う
void EdgeNeighborCubes(int32_t axis, int32_t x, int32_t y, int32_t z, bool outwardPositive,
                       int32_t cubeCoord[4][3])
{
    // axis: 0=X方向辺 (x,y,z)-(x+1,y,z)、1=Y方向辺、2=Z方向辺。perp1,perp2 は軸に垂直な2座標
    const int32_t perp1Base = (axis == 0) ? y : (axis == 1) ? x : x;
    const int32_t perp2Base = (axis == 0) ? z : (axis == 1) ? z : y;
    // 周回パターン (0/1) → (offset-1, offset) への写像で cube 座標を得る
    const int32_t patternStd[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };   // X,Z軸用
    const int32_t patternY[4][2] = { { 0, 0 }, { 0, 1 }, { 1, 1 }, { 1, 0 } };     // Y軸用
    const int32_t(&pattern)[4][2] = (axis == 1) ? patternY : patternStd;

    int32_t order[4] = { 0, 1, 2, 3 };
    if (!outwardPositive) {
        order[0] = 0;
        order[1] = 3;
        order[2] = 2;
        order[3] = 1; // 周回を逆順に (0,3,2,1 は (0,1,2,3) の逆順を先頭0基準で書いたもの)
    }
    for (int k = 0; k < 4; ++k) {
        const int32_t p1 = perp1Base + pattern[order[k]][0] - 1;
        const int32_t p2 = perp2Base + pattern[order[k]][1] - 1;
        if (axis == 0) {
            cubeCoord[k][0] = x;
            cubeCoord[k][1] = p1;
            cubeCoord[k][2] = p2;
        } else if (axis == 1) {
            cubeCoord[k][0] = p1;
            cubeCoord[k][1] = y;
            cubeCoord[k][2] = p2;
        } else {
            cubeCoord[k][0] = p1;
            cubeCoord[k][1] = p2;
            cubeCoord[k][2] = z;
        }
    }
}

float DistSq(const XMFLOAT3& a, const XMFLOAT3& b)
{
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

// 四角形 (v0,v1,v2,v3、CCW) を三角形2枚に割る。対角は短い方を選ぶ (同値は v0-v2 側)。
// surface nets の頂点は完全平面上とは限らないので、対角の選び方で三角形の質が変わる
void AppendQuad(const XMFLOAT3 v[4], const XMFLOAT3& normal, int32_t uvAxis, FractureMesh& out)
{
    const int32_t base = static_cast<int32_t>(out.verts.size());
    for (int c = 0; c < 4; ++c) {
        FractureVertex fv;
        fv.position = v[c];
        fv.normal = normal;
        fv.uv = (uvAxis == 0) ? XMFLOAT2{ v[c].y, v[c].z }
              : (uvAxis == 1) ? XMFLOAT2{ v[c].x, v[c].z }
                              : XMFLOAT2{ v[c].x, v[c].y };
        out.verts.push_back(fv);
    }
    const bool diag02Shorter = DistSq(v[0], v[2]) <= DistSq(v[1], v[3]);
    if (diag02Shorter) {
        out.indices.push_back(base + 0);
        out.indices.push_back(base + 1);
        out.indices.push_back(base + 2);
        out.indices.push_back(base + 0);
        out.indices.push_back(base + 2);
        out.indices.push_back(base + 3);
    } else {
        out.indices.push_back(base + 1);
        out.indices.push_back(base + 2);
        out.indices.push_back(base + 3);
        out.indices.push_back(base + 1);
        out.indices.push_back(base + 3);
        out.indices.push_back(base + 0);
    }
}

FractureMesh BuildSurfaceNetsMesh(const GridDims& dims, const std::vector<uint8_t>& occ, const CubeDims& cubeDims,
                                  const std::vector<uint8_t>& active, const std::vector<XMFLOAT3>& vertexPos)
{
    FractureMesh mesh;

    auto cubeVertex = [&](const int32_t c[3], bool& ok) -> XMFLOAT3 {
        if (c[0] < 0 || c[0] >= cubeDims.nx || c[1] < 0 || c[1] >= cubeDims.ny || c[2] < 0
            || c[2] >= cubeDims.nz) {
            ok = false;
            return { 0, 0, 0 };
        }
        const int64_t ci = CubeIndexOf(cubeDims, c[0], c[1], c[2]);
        if (!active[static_cast<size_t>(ci)]) {
            ok = false;
            return { 0, 0, 0 };
        }
        return vertexPos[static_cast<size_t>(ci)];
    };

    auto emitAlongAxis = [&](int32_t axis, int32_t nx1, int32_t ny1, int32_t nz1) {
        for (int32_t z = 0; z < nz1; ++z) {
            for (int32_t y = 0; y < ny1; ++y) {
                for (int32_t x = 0; x < nx1; ++x) {
                    const int32_t hx = x + (axis == 0 ? 1 : 0);
                    const int32_t hy = y + (axis == 1 ? 1 : 0);
                    const int32_t hz = z + (axis == 2 ? 1 : 0);
                    const uint8_t oa = occ[static_cast<size_t>(IndexOf(dims, x, y, z))];
                    const uint8_t ob = occ[static_cast<size_t>(IndexOf(dims, hx, hy, hz))];
                    if (oa == ob) {
                        continue;
                    }
                    const bool outwardPositive = (oa != 0); // occupied→empty が外向き
                    int32_t cubeCoord[4][3];
                    EdgeNeighborCubes(axis, x, y, z, outwardPositive, cubeCoord);
                    XMFLOAT3 v[4];
                    bool ok = true;
                    for (int c = 0; c < 4 && ok; ++c) {
                        v[c] = cubeVertex(cubeCoord[c], ok);
                    }
                    if (!ok) {
                        continue; // 構造的に起きないはずだが、閉じ判定が最後に検知する安全網
                    }
                    const float sign = outwardPositive ? 1.0f : -1.0f;
                    const XMFLOAT3 normal = (axis == 0) ? XMFLOAT3{ sign, 0, 0 }
                                          : (axis == 1)  ? XMFLOAT3{ 0, sign, 0 }
                                                         : XMFLOAT3{ 0, 0, sign };
                    AppendQuad(v, normal, axis, mesh);
                }
            }
        }
    };

    emitAlongAxis(0, dims.nx - 1, dims.ny, dims.nz);
    emitAlongAxis(1, dims.nx, dims.ny - 1, dims.nz);
    emitAlongAxis(2, dims.nx, dims.ny, dims.nz - 1);
    return mesh;
}

} // namespace

bool VoxelizeMeshForFracture(const FractureMesh& source, int32_t resolution, FractureVoxelizeResult& out)
{
    out = FractureVoxelizeResult{};
    if (source.verts.empty() || source.indices.empty()) {
        out.failReason = "ソースメッシュが空";
        return false;
    }

    XMFLOAT3 origin{};
    float h = 0.0f;
    GridDims dims{};
    int32_t resolutionUsed = 0;
    if (!ComputeGridFrame(source, resolution, origin, h, dims, resolutionUsed)) {
        out.failReason = "縮退したメッシュ (AABB の最長辺が 0)";
        return false;
    }
    out.resolutionUsed = resolutionUsed;

    std::vector<uint8_t> occ(static_cast<size_t>(dims.Total()), 0);
    MarkOccupancy(source, origin, h, dims, occ);
    FloodFillInterior(dims, occ);
    ResolveAmbiguousConfigurations(dims, occ);

    CubeDims cubeDims{};
    std::vector<uint8_t> active;
    std::vector<XMFLOAT3> vertexPos;
    ComputeActiveCubeVertices(dims, occ, origin, h, cubeDims, active, vertexPos);

    FractureMesh mesh = BuildSurfaceNetsMesh(dims, occ, cubeDims, active, vertexPos);
    if (mesh.indices.empty()) {
        out.failReason = "占有セルが1つも無い (ボクセル化の結果が空)";
        return false;
    }

    const ClosedMeshCheck check = CheckClosedMesh(mesh);
    if (!check.closed) {
        out.failReason = "ボクセル化結果が閉じていない (境界辺 " + std::to_string(check.boundaryEdges)
                        + " 本 / 非多様体辺 " + std::to_string(check.nonManifoldEdges) + " 本 / 向き不一致 "
                        + std::to_string(check.orientationMismatches) + " 本)";
        return false;
    }

    out.mesh = std::move(mesh);
    out.success = true;
    return true;
}

bool BuildSurfaceNetsFromOccupancy(const RawOccupancyGrid& grid, float h, FractureMesh& outMesh)
{
    outMesh = FractureMesh{};
    if (grid.nx < 3 || grid.ny < 3 || grid.nz < 3) {
        return false; // パディング1セルずつ+内部1セル以上が最小構成
    }
    const GridDims dims{ grid.nx, grid.ny, grid.nz };
    if (grid.occ.size() != static_cast<size_t>(dims.Total())) {
        return false;
    }
    std::vector<uint8_t> occ = grid.occ;
    ResolveAmbiguousConfigurations(dims, occ);

    CubeDims cubeDims{};
    std::vector<uint8_t> active;
    std::vector<XMFLOAT3> vertexPos;
    ComputeActiveCubeVertices(dims, occ, XMFLOAT3{ 0, 0, 0 }, h, cubeDims, active, vertexPos);

    outMesh = BuildSurfaceNetsMesh(dims, occ, cubeDims, active, vertexPos);
    return true;
}

} // namespace mye
