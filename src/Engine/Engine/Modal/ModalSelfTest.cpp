//====================================================================================
//                          ModalSelfTest.cpp
//  MyEngine/ 秋田蓮音                                                      09/16/2026
//                                          Voxelizer のヘッドレス検査の実装
//====================================================================================
#include "Engine/Engine/Modal/ModalSelfTest.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <vector>

#include "Engine/Core/Log.h"
#include "Engine/Engine/Modal/TriangleSoup.h"
#include "Engine/Engine/Modal/Voxelizer.h"

using namespace DirectX;

namespace mye {
namespace {

// 半径 (ex/2, ey/2, ez/2) の直方体、中心 (cx,cy,cz)。面ごとに 2 三角形、計 12 三角形の閉じた箱。
// skipFace >= 0 のときはその面 (0=-X,1=+X,2=-Y,3=+Y,4=-Z,5=+Z) の 2 三角形を落とす (蓋なし箱用)
void MakeBox(float ex, float ey, float ez, float cx, float cy, float cz,
            std::vector<XMFLOAT3>& pos, std::vector<uint32_t>& idx, int skipFace = -1)
{
    const float hx = ex * 0.5f, hy = ey * 0.5f, hz = ez * 0.5f;
    pos = {
        { cx - hx, cy - hy, cz - hz }, // 0
        { cx + hx, cy - hy, cz - hz }, // 1
        { cx + hx, cy + hy, cz - hz }, // 2
        { cx - hx, cy + hy, cz - hz }, // 3
        { cx - hx, cy - hy, cz + hz }, // 4
        { cx + hx, cy - hy, cz + hz }, // 5
        { cx + hx, cy + hy, cz + hz }, // 6
        { cx - hx, cy + hy, cz + hz }, // 7
    };
    // 面ごとの 2 三角形 (向きは問わない。TriBoxOverlap は法線の向きを見ない)
    const uint32_t faces[6][6] = {
        { 0, 3, 7, 0, 7, 4 }, // -X
        { 1, 5, 6, 1, 6, 2 }, // +X
        { 0, 4, 5, 0, 5, 1 }, // -Y
        { 3, 2, 6, 3, 6, 7 }, // +Y
        { 0, 1, 2, 0, 2, 3 }, // -Z
        { 4, 6, 5, 4, 7, 6 }, // +Z
    };
    idx.clear();
    for (int f = 0; f < 6; ++f) {
        if (f == skipFace) {
            continue;
        }
        for (int k = 0; k < 6; ++k) {
            idx.push_back(faces[f][k]);
        }
    }
}

int RowOf(int voxelIndex, int axis)
{
    // VoxelIndexOf(x,y,z) = x + N*(y+N*z) の逆変換
    constexpr int N = kModalVoxelN;
    const int x = voxelIndex % N;
    const int y = (voxelIndex / N) % N;
    const int z = voxelIndex / (N * N);
    return axis == 0 ? x : (axis == 1 ? y : z);
}

// 4096 cell の cellSlot を BuildCellSlotTable とは独立に総当たりで作り直す (回帰テストの照合用)。
// 規則は同一 (有効 cell は自身、無効 cell は最近傍の有効 cell、同値は index の小さい方)
void BruteForceCellSlot(const modal::VoxelGrid& grid, uint16_t out[4096])
{
    constexpr int M = kModalMapN;
    std::vector<bool> valid(4096, false);
    std::vector<std::array<int, 3>> coord(4096);
    for (int cz = 0; cz < M; ++cz) {
        for (int cy = 0; cy < M; ++cy) {
            for (int cx = 0; cx < M; ++cx) {
                const int cell = modal::CellIndexOf(cx, cy, cz);
                coord[static_cast<size_t>(cell)] = { cx, cy, cz };
                bool occ = false;
                for (int dz = 0; dz < 2 && !occ; ++dz) {
                    for (int dy = 0; dy < 2 && !occ; ++dy) {
                        for (int dx = 0; dx < 2 && !occ; ++dx) {
                            if (grid.occ[static_cast<size_t>(
                                    modal::VoxelIndexOf(cx * 2 + dx, cy * 2 + dy, cz * 2 + dz))]) {
                                occ = true;
                            }
                        }
                    }
                }
                valid[static_cast<size_t>(cell)] = occ;
            }
        }
    }
    for (int cell = 0; cell < 4096; ++cell) {
        if (valid[static_cast<size_t>(cell)]) {
            out[cell] = static_cast<uint16_t>(cell);
            continue;
        }
        const auto& c = coord[static_cast<size_t>(cell)];
        int best = -1;
        long long bestDist = -1;
        for (int other = 0; other < 4096; ++other) {
            if (!valid[static_cast<size_t>(other)]) {
                continue;
            }
            const auto& o = coord[static_cast<size_t>(other)];
            const long long dx = c[0] - o[0], dy = c[1] - o[1], dz = c[2] - o[2];
            const long long d = dx * dx + dy * dy + dz * dz;
            if (best < 0 || d < bestDist) {
                best = other;
                bestDist = d;
            }
        }
        out[cell] = static_cast<uint16_t>(best);
    }
}

} // namespace

bool RunModalSelfTest()
{
    MYE_LOG_INFO("==== Modal (Voxelizer) self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    // ---- (1) 単位立方体: 24389 (29^3) / 中心 1 / 8 隅 0 / pad リング全 0 ----
    // M76b round 2: h = L/28, origin = center-16.5h (spec §4.1) に訂正済み
    modal::VoxelGrid cubeGrid;
    {
        std::vector<XMFLOAT3> pos;
        std::vector<uint32_t> idx;
        MakeBox(1.0f, 1.0f, 1.0f, 0, 0, 0, pos, idx);
        const bool ok = modal::VoxelizeMesh(pos.data(), pos.size(), idx.data(), idx.size(), cubeGrid);
        check(ok, "unit cube voxelizes successfully");
        check(cubeGrid.surfaceCount + cubeGrid.interiorCount == 24389,
              "unit cube occupies exactly 29^3 = 24389 voxels");

        constexpr int N = kModalVoxelN;
        check(cubeGrid.occ[static_cast<size_t>(modal::VoxelIndexOf(16, 16, 16))] != 0,
              "the grid center voxel is occupied");

        const int corners[8][3] = { { 0, 0, 0 },         { N - 1, 0, 0 },     { 0, N - 1, 0 },
                                    { 0, 0, N - 1 },     { N - 1, N - 1, 0 }, { N - 1, 0, N - 1 },
                                    { 0, N - 1, N - 1 }, { N - 1, N - 1, N - 1 } };
        bool cornersClear = true;
        for (const auto& c : corners) {
            cornersClear = cornersClear
                && cubeGrid.occ[static_cast<size_t>(modal::VoxelIndexOf(c[0], c[1], c[2]))] == 0;
        }
        check(cornersClear, "all 8 grid corners are unoccupied");

        bool padClear = true;
        for (int z = 0; z < N && padClear; ++z) {
            for (int y = 0; y < N && padClear; ++y) {
                for (int x = 0; x < N && padClear; ++x) {
                    const bool onPad =
                        (x == 0 || x == N - 1 || y == 0 || y == N - 1 || z == 0 || z == N - 1);
                    if (onPad && cubeGrid.occ[static_cast<size_t>(modal::VoxelIndexOf(x, y, z))]) {
                        padClear = false;
                    }
                }
            }
        }
        check(padClear, "the padding ring is fully unoccupied");
    }

    // ---- (2) 厚さ 0.001 の薄板: 厚み方向の占有 index はちょうど 1 個 ----
    // M76b round 2: origin = center-16.5h の規約により、メッシュ自身の AABB 中心は必ず
    // voxel 16 の**中心** (16.5) に一致する (voxel 境界には乗らない)。中心対称な薄板は
    // この 1 voxel の中に収まるので、ここは「1 個」を厳密に要求してよい
    {
        std::vector<XMFLOAT3> pos;
        std::vector<uint32_t> idx;
        MakeBox(10.0f, 0.001f, 10.0f, 0, 0, 0, pos, idx);
        modal::VoxelGrid grid;
        const bool ok = modal::VoxelizeMesh(pos.data(), pos.size(), idx.data(), idx.size(), grid);
        check(ok, "thin plate voxelizes successfully");
        std::set<int> rows;
        for (size_t i = 0; i < grid.occ.size(); ++i) {
            if (grid.occ[i]) {
                rows.insert(RowOf(static_cast<int>(i), 1));
            }
        }
        check(rows.size() == 1 && *rows.begin() == 16,
              "a 0.001-thick plate occupies exactly 1 voxel row (index 16) along its thin axis");
    }

    // ---- (3) 蓋なし箱: 開口部から flood-fill が漏れて interiorCount == 0 ----
    {
        std::vector<XMFLOAT3> pos;
        std::vector<uint32_t> idx;
        MakeBox(1.0f, 1.0f, 1.0f, 0, 0, 0, pos, idx, /*skipFace=*/3); // +Y の蓋を外す
        modal::VoxelGrid grid;
        const bool ok = modal::VoxelizeMesh(pos.data(), pos.size(), idx.data(), idx.size(), grid);
        check(ok, "open-lid box voxelizes successfully");
        check(grid.interiorCount == 0,
              "a box missing one face has no sealed interior (flood-fill leaks through the opening)");
    }

    // ---- (4)(5) 2:1:0.5 の AABB: h = L/28 (最長辺占有 29 voxel)、+X 面中心のセル座標 ----
    // M76b round 2: 中心が voxel 境界に乗らなくなったので、cell はどちらも "7|8" ではなく
    // (15, 8, 8) に一意に決まる
    modal::VoxelGrid boxGrid;
    {
        std::vector<XMFLOAT3> pos;
        std::vector<uint32_t> idx;
        MakeBox(2.0f, 1.0f, 0.5f, 0, 0, 0, pos, idx);
        const bool ok = modal::VoxelizeMesh(pos.data(), pos.size(), idx.data(), idx.size(), boxGrid);
        check(ok, "2:1:0.5 box voxelizes successfully");
        check(std::fabs(boxGrid.frame.longestEdge / boxGrid.frame.voxelSize - 28.0f) < 1.0e-3f,
              "the longest edge maps to h = L/28");

        std::set<int> xRows;
        for (size_t i = 0; i < boxGrid.occ.size(); ++i) {
            if (boxGrid.occ[i]) {
                xRows.insert(RowOf(static_cast<int>(i), 0));
            }
        }
        check(xRows.size() == 29, "the longest (X) axis occupies exactly 29 voxels");

        const float faceCenter[3] = { 1.0f, 0.0f, 0.0f }; // +X 面中心 (ex/2 = 1.0)
        const uint16_t cell = modal::LocalPointToCell(boxGrid.frame, faceCenter);
        const int cx = cell % kModalMapN;
        const int cy = (cell / kModalMapN) % kModalMapN;
        const int cz = cell / (kModalMapN * kModalMapN);
        check(cx == 15 && cy == 8 && cz == 8, "the +X face center maps uniquely to cell (15, 8, 8)");
    }

    // ---- (6) cellSlot: 有効セルは自身、無効セルは独立の総当たりと一致 ----
    {
        uint16_t slot[4096];
        modal::BuildCellSlotTable(cubeGrid, slot);
        uint16_t brute[4096];
        BruteForceCellSlot(cubeGrid, brute);
        bool same = true;
        bool validMapsToSelf = true;
        for (int cell = 0; cell < 4096; ++cell) {
            same = same && slot[cell] == brute[cell];
        }
        for (int cell = 0; cell < 4096; ++cell) {
            if (slot[cell] == cell) {
                // 自身にマップされる cell は「有効」のはず (少なくとも 1 voxel 占有)
                const int cx = cell % kModalMapN;
                const int cy = (cell / kModalMapN) % kModalMapN;
                const int cz = cell / (kModalMapN * kModalMapN);
                bool occ = false;
                for (int dz = 0; dz < 2 && !occ; ++dz) {
                    for (int dy = 0; dy < 2 && !occ; ++dy) {
                        for (int dx = 0; dx < 2 && !occ; ++dx) {
                            if (cubeGrid.occ[static_cast<size_t>(modal::VoxelIndexOf(
                                    cx * 2 + dx, cy * 2 + dy, cz * 2 + dz))]) {
                                occ = true;
                            }
                        }
                    }
                }
                validMapsToSelf = validMapsToSelf && occ;
            }
        }
        check(same, "BuildCellSlotTable matches an independently written brute-force reference");
        check(validMapsToSelf, "every cell that maps to itself is genuinely occupied");
    }

    // ---- (7) .mvox 往復: memcmp 一致 ----
    {
        std::vector<uint8_t> bytesA;
        modal::SerializeVox(cubeGrid, bytesA);
        modal::VoxelGrid roundTrip;
        const bool loaded = modal::DeserializeVox(bytesA, roundTrip);
        check(loaded, ".mvox bytes deserialize successfully");
        std::vector<uint8_t> bytesB;
        modal::SerializeVox(roundTrip, bytesB);
        check(bytesA == bytesB, ".mvox serialize -> deserialize -> serialize is byte-identical");
        check(bytesA.size() == modal::kVoxHeaderBytes + modal::kVoxOccBytes,
              ".mvox size matches header + 32768-byte occupancy");
    }

    // ---- (8) OFF/OBJ の最小リーダ ----
    {
        // 標準的な OFF ヘッダ (立方体、8 頂点 12 面)
        const std::string offText =
            "OFF\n"
            "8 12 0\n"
            "-0.5 -0.5 -0.5\n 0.5 -0.5 -0.5\n 0.5 0.5 -0.5\n-0.5 0.5 -0.5\n"
            "-0.5 -0.5 0.5\n 0.5 -0.5 0.5\n 0.5 0.5 0.5\n-0.5 0.5 0.5\n"
            "3 0 3 7\n3 0 7 4\n3 1 5 6\n3 1 6 2\n3 0 4 5\n3 0 5 1\n"
            "3 3 2 6\n3 3 6 7\n3 0 1 2\n3 0 2 3\n3 4 6 5\n3 4 7 6\n";
        modal::TriangleSoup soup;
        std::string err;
        const bool ok = modal::LoadOffText(offText, soup, &err);
        check(ok && soup.positions.size() == 8 && soup.indices.size() == 36,
              "a standard OFF cube parses to 8 vertices and 12 triangles");

        // ModelNet のクセ: "OFF" 直後に空白なしで最初の数字が続く
        const std::string offGlued =
            "OFF8 12 0\n"
            "-0.5 -0.5 -0.5\n 0.5 -0.5 -0.5\n 0.5 0.5 -0.5\n-0.5 0.5 -0.5\n"
            "-0.5 -0.5 0.5\n 0.5 -0.5 0.5\n 0.5 0.5 0.5\n-0.5 0.5 0.5\n"
            "3 0 3 7\n3 0 7 4\n3 1 5 6\n3 1 6 2\n3 0 4 5\n3 0 5 1\n"
            "3 3 2 6\n3 3 6 7\n3 0 1 2\n3 0 2 3\n3 4 6 5\n3 4 7 6\n";
        modal::TriangleSoup soupGlued;
        const bool okGlued = modal::LoadOffText(offGlued, soupGlued, &err);
        check(okGlued && soupGlued.positions.size() == 8 && soupGlued.indices.size() == 36,
              "the ModelNet 'OFF' + digits-glued header quirk parses identically");

        // OBJ: v/f のみ、負インデックス (末尾相対) と "i/j/k" 形式の先頭成分だけを使う
        const std::string objText =
            "# comment line\n"
            "v -0.5 -0.5 -0.5\nv 0.5 -0.5 -0.5\nv 0.5 0.5 -0.5\nv -0.5 0.5 -0.5\n"
            "v -0.5 -0.5 0.5\nv 0.5 -0.5 0.5\nv 0.5 0.5 0.5\nv -0.5 0.5 0.5\n"
            "f 1/1/1 4/2/1 8/3/1\nf -8 8 5\n"
            "f 2 6 7\nf 2 7 3\nf 1 5 6\nf 1 6 2\n"
            "f 4 3 7\nf 4 7 8\nf 1 2 3\nf 1 3 4\nf 5 7 6\nf 5 8 7\n";
        modal::TriangleSoup soupObj;
        const bool okObj = modal::LoadObjText(objText, soupObj, &err);
        check(okObj && soupObj.positions.size() == 8 && soupObj.indices.size() == 36,
              "a minimal OBJ (v/f, negative & i/j/k refs) parses to 8 vertices and 12 triangles");
    }

    // ---- (9) 決定論: 同入力 2 回 → memcmp 一致 ----
    {
        std::vector<XMFLOAT3> pos;
        std::vector<uint32_t> idx;
        MakeBox(1.0f, 1.0f, 1.0f, 0, 0, 0, pos, idx);
        modal::VoxelGrid gridA, gridB;
        modal::VoxelizeMesh(pos.data(), pos.size(), idx.data(), idx.size(), gridA);
        modal::VoxelizeMesh(pos.data(), pos.size(), idx.data(), idx.size(), gridB);
        std::vector<uint8_t> bytesA, bytesB;
        modal::SerializeVox(gridA, bytesA);
        modal::SerializeVox(gridB, bytesB);
        check(bytesA == bytesB, "voxelizing the same mesh twice is byte-identical");
    }

    if (failCount == 0) {
        MYE_LOG_INFO("==== Modal (Voxelizer) self test: ALL PASS ====");
    } else {
        MYE_LOG_ERROR("==== Modal (Voxelizer) self test: %d FAILED ====", failCount);
    }
    return failCount == 0;
}

} // namespace mye
