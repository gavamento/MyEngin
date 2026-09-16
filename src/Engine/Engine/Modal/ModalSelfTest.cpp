//====================================================================================
//                          ModalSelfTest.cpp
//  MyEngine/ 秋田蓮音                                                      09/16/2026
//                                          Voxelizer のヘッドレス検査の実装
//====================================================================================
#include "Engine/Engine/Modal/ModalSelfTest.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <set>
#include <vector>

#include <DirectXPackedVector.h>

#include "Engine/Core/AssetKeyResolver.h"
#include "Engine/Core/Log.h"
#include "Engine/Engine/Modal/CpuModalBackend.h"
#include "Engine/Engine/Modal/DmNet.h"
#include "Engine/Engine/Modal/ModalFeatureMap.h"
#include "Engine/Engine/Modal/ModalSoundLibrary.h"
#include "Engine/Engine/Modal/TriangleSoup.h"
#include "Engine/Engine/Modal/Voxelizer.h"
#include "Engine/Engine/Physics/ConvexColliderLibrary.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/GpuResources.h"

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

// ---- sub-05: CpuModalBackend の低レベル畳み込みを独立に照合する「素朴な参照実装」 ----
// 決定的だが sim の乱数契約とは無関係のテストデータ生成 (黄金比刻み。PCG32 を持ち出すほどの
// ものではない — 生成される値そのものに意味は無く、単に非対称で再現可能な数列であればよい)
float DeterministicFill(size_t i)
{
    return std::fmod(static_cast<float>(i) * 0.6180339887f, 1.0f) * 2.0f - 1.0f;
}

// PyTorch の F.conv3d と同じ定義そのままの 6 重ループ (CpuModalBackend.cpp の im2col+GEMM 実装とは
// 独立のコード)。weight は [cout,cin,k,k,k]、src/dst は [C,D,H,W] (W 最内)
void NaiveConv3d(const std::vector<float>& src, int cin, int d, int h, int w,
                 const std::vector<float>& weight, const std::vector<float>& bias, int cout, int k,
                 int stride, int pad, std::vector<float>& dst, int outD, int outH, int outW)
{
    dst.assign(static_cast<size_t>(cout) * outD * outH * outW, 0.0f);
    for (int co = 0; co < cout; ++co) {
        for (int od = 0; od < outD; ++od) {
            for (int oh = 0; oh < outH; ++oh) {
                for (int ow = 0; ow < outW; ++ow) {
                    float acc = bias[co];
                    for (int ci = 0; ci < cin; ++ci) {
                        for (int kd = 0; kd < k; ++kd) {
                            const int id = od * stride - pad + kd;
                            if (id < 0 || id >= d) {
                                continue;
                            }
                            for (int kh = 0; kh < k; ++kh) {
                                const int ih = oh * stride - pad + kh;
                                if (ih < 0 || ih >= h) {
                                    continue;
                                }
                                for (int kw = 0; kw < k; ++kw) {
                                    const int iw = ow * stride - pad + kw;
                                    if (iw < 0 || iw >= w) {
                                        continue;
                                    }
                                    const float wv = weight[((((static_cast<size_t>(co) * cin + ci)
                                                              * k + kd) * k + kh) * k) + kw];
                                    const float sv =
                                        src[(((static_cast<size_t>(ci) * d + id) * h + ih) * w) + iw];
                                    acc += wv * sv;
                                }
                            }
                        }
                    }
                    dst[(((static_cast<size_t>(co) * outD + od) * outH + oh) * outW) + ow] = acc;
                }
            }
        }
    }
}

// PyTorch の F.conv_transpose3d の定義そのまま (出力位置から id=(od+pad-kd)/stride の整除性を
// 直接判定する)。CpuModalBackend.cpp の「dilate + pad + 反転カーネル」実装とは別経路の照合になる。
// weight は [cin,cout,k,k,k] (ConvTranspose3d の PyTorch テンソル形状そのまま)
void NaiveConvTranspose3d(const std::vector<float>& src, int cin, int d, int h, int w,
                          const std::vector<float>& weight, const std::vector<float>& bias,
                          int cout, int k, int stride, int pad, int outPad, std::vector<float>& dst,
                          int outD, int outH, int outW)
{
    (void)outPad; // 出力サイズは呼び出し側が式から求めて渡す (ここでは判定に使わない)
    dst.assign(static_cast<size_t>(cout) * outD * outH * outW, 0.0f);
    for (int co = 0; co < cout; ++co) {
        for (int od = 0; od < outD; ++od) {
            for (int oh = 0; oh < outH; ++oh) {
                for (int ow = 0; ow < outW; ++ow) {
                    float acc = bias[co];
                    for (int ci = 0; ci < cin; ++ci) {
                        for (int kd = 0; kd < k; ++kd) {
                            const int td = od + pad - kd;
                            if (td < 0 || td % stride != 0) {
                                continue;
                            }
                            const int id = td / stride;
                            if (id < 0 || id >= d) {
                                continue;
                            }
                            for (int kh = 0; kh < k; ++kh) {
                                const int th = oh + pad - kh;
                                if (th < 0 || th % stride != 0) {
                                    continue;
                                }
                                const int ih = th / stride;
                                if (ih < 0 || ih >= h) {
                                    continue;
                                }
                                for (int kw = 0; kw < k; ++kw) {
                                    const int tw = ow + pad - kw;
                                    if (tw < 0 || tw % stride != 0) {
                                        continue;
                                    }
                                    const int iw = tw / stride;
                                    if (iw < 0 || iw >= w) {
                                        continue;
                                    }
                                    const float wv = weight[((((static_cast<size_t>(ci) * cout + co)
                                                              * k + kd) * k + kh) * k) + kw];
                                    const float sv =
                                        src[(((static_cast<size_t>(ci) * d + id) * h + ih) * w) + iw];
                                    acc += wv * sv;
                                }
                            }
                        }
                    }
                    dst[(((static_cast<size_t>(co) * outD + od) * outH + oh) * outW) + ow] = acc;
                }
            }
        }
    }
}

float MaxAbsDiff(const std::vector<float>& a, const std::vector<float>& b)
{
    float m = 0.0f;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
        m = (std::max)(m, std::fabs(a[i] - b[i]));
    }
    return m;
}

std::vector<uint8_t> ReadFileBytes(const std::wstring& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return {};
    }
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
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

    // ---- (10) CpuModalBackend の Conv3dRaw を素朴な参照実装 (6 重ループ) と 1e-6 で照合 ----
    // odd kernel / stride 1 と 2 / pad あり、の組を確認する (spec §5 受け入れ条件 11)
    {
        struct Case {
            const char* name;
            int cin, d, h, w, cout, k, stride, pad;
        };
        const Case cases[] = {
            { "3x3x3 same conv (k=3,stride=1,pad=1)", 2, 5, 5, 5, 3, 3, 1, 1 },
            { "3x3x3 downsample (k=3,stride=2,pad=1)", 2, 5, 5, 5, 3, 3, 2, 1 },
            { "1x1x1 pointwise (k=1,stride=1,pad=0)", 4, 3, 3, 3, 2, 1, 1, 0 },
        };
        for (const Case& c : cases) {
            int outD = 0, outH = 0, outW = 0;
            Conv3dOutSize(c.d, c.h, c.w, c.k, c.stride, c.pad, outD, outH, outW);
            std::vector<float> src(static_cast<size_t>(c.cin) * c.d * c.h * c.w);
            std::vector<float> weight(static_cast<size_t>(c.cout) * c.cin * c.k * c.k * c.k);
            std::vector<float> bias(static_cast<size_t>(c.cout));
            for (size_t i = 0; i < src.size(); ++i) {
                src[i] = DeterministicFill(i + 1);
            }
            for (size_t i = 0; i < weight.size(); ++i) {
                weight[i] = DeterministicFill(i * 7 + 3);
            }
            for (size_t i = 0; i < bias.size(); ++i) {
                bias[i] = DeterministicFill(i * 11 + 5) * 0.1f;
            }
            std::vector<float> got(static_cast<size_t>(c.cout) * outD * outH * outW);
            Conv3dRaw(src.data(), c.cin, c.d, c.h, c.w, weight.data(), bias.data(), c.cout, c.k,
                     c.stride, c.pad, got.data(), outD, outH, outW);
            std::vector<float> expected;
            NaiveConv3d(src, c.cin, c.d, c.h, c.w, weight, bias, c.cout, c.k, c.stride, c.pad,
                       expected, outD, outH, outW);
            const float maxDiff = MaxAbsDiff(got, expected);
            char what[192];
            std::snprintf(what, sizeof(what), "Conv3dRaw matches naive 6-loop reference: %s (max|d|=%g)",
                         c.name, static_cast<double>(maxDiff));
            check(maxDiff < 1.0e-6f, what);
        }
    }

    // ---- (11) ConvTranspose3dRaw を素朴な参照実装 (出力位置の整除判定) と 1e-6 で照合 ----
    // k=4/stride=2/pad=1/outPad=0 (実ネットの up1/up2 と同じ形) と、
    // k=3/stride=2/pad=1/outPad=1 (奇数 k + outPad あり) の両方を確認する
    {
        struct Case {
            const char* name;
            int cin, d, h, w, cout, k, stride, pad, outPad;
        };
        const Case cases[] = {
            { "k=4,stride=2,pad=1,outPad=0 (up1/up2 と同型)", 3, 4, 4, 4, 2, 4, 2, 1, 0 },
            { "k=3,stride=2,pad=1,outPad=1 (奇数 k + outPad)", 2, 3, 3, 3, 3, 3, 2, 1, 1 },
        };
        for (const Case& c : cases) {
            int outD = 0, outH = 0, outW = 0;
            ConvTranspose3dOutSize(c.d, c.h, c.w, c.k, c.stride, c.pad, c.outPad, outD, outH, outW);
            std::vector<float> src(static_cast<size_t>(c.cin) * c.d * c.h * c.w);
            std::vector<float> weight(static_cast<size_t>(c.cin) * c.cout * c.k * c.k * c.k);
            std::vector<float> bias(static_cast<size_t>(c.cout));
            for (size_t i = 0; i < src.size(); ++i) {
                src[i] = DeterministicFill(i + 2);
            }
            for (size_t i = 0; i < weight.size(); ++i) {
                weight[i] = DeterministicFill(i * 13 + 1);
            }
            for (size_t i = 0; i < bias.size(); ++i) {
                bias[i] = DeterministicFill(i * 17 + 9) * 0.1f;
            }
            std::vector<float> got(static_cast<size_t>(c.cout) * outD * outH * outW);
            ConvTranspose3dRaw(src.data(), c.cin, c.d, c.h, c.w, weight.data(), bias.data(), c.cout,
                              c.k, c.stride, c.pad, c.outPad, got.data(), outD, outH, outW);
            std::vector<float> expected;
            NaiveConvTranspose3d(src, c.cin, c.d, c.h, c.w, weight, bias, c.cout, c.k, c.stride,
                                c.pad, c.outPad, expected, outD, outH, outW);
            const float maxDiff = MaxAbsDiff(got, expected);
            char what[192];
            std::snprintf(what, sizeof(what),
                         "ConvTranspose3dRaw matches naive reference: %s (max|d|=%g)", c.name,
                         static_cast<double>(maxDiff));
            check(maxDiff < 1.0e-6f, what);
        }
    }

    // ---- (12) fixture.dmnet + fixture_in.mvox → 64 cell x 192 が fixture_out.bin と
    //          max|delta| < 1e-3 で一致する (spec §5 受け入れ条件 11、「インストール済み
    //          バックエンドに対して回す」= バックエンドを引数に取る形にして、将来の
    //          D3d11ModalBackend も同じ関数で検査できるようにしてある) ----
    {
        const std::wstring repoRoot = FindEngineRepoRoot();
        const std::wstring fixtureDir = repoRoot + L"\\tests\\deepmodal\\";
        DmNet net;
        std::string err;
        const bool loaded = LoadDmNet(fixtureDir + L"fixture.dmnet", net, &err);
        check(loaded, ("fixture.dmnet loads and passes the weightsHash check (" + err + ")").c_str());
        if (loaded) {
            modal::VoxelGrid grid;
            const std::vector<uint8_t> mvoxBytes = ReadFileBytes(fixtureDir + L"fixture_in.mvox");
            const bool gridOk = modal::DeserializeVox(mvoxBytes, grid);
            check(gridOk, "fixture_in.mvox deserializes");

            const std::vector<uint8_t> expectedBytes = ReadFileBytes(fixtureDir + L"fixture_out.bin");
            check(expectedBytes.size() == 64 * (4 + static_cast<size_t>(kModalChannels) * 4),
                  "fixture_out.bin has the expected size (64 cells x (index + 192 float32))");

            // インストール済みバックエンド (このサブでは CpuModalBackend) に対して回す。
            // 将来 D3d11ModalBackend を差し込んでもこの関数をもう一度呼べば同じ検査になる。
            // 戻り値は max|Δ| (呼び出し側が両経路の実測値を比較するのに使う、sub-09)
            auto runFixture = [&](ModalInferenceBackend& backend, const char* label) -> float {
                std::string prepErr;
                const bool prepared = backend.Prepare(net, &prepErr);
                check(prepared, (std::string(label) + ": Prepare() succeeds").c_str());
                if (!prepared || !gridOk || expectedBytes.size() < 64 * (4 + 192 * 4)) {
                    return -1.0f;
                }
                std::vector<float> out;
                std::string inferErr;
                const bool inferred = backend.Infer(grid, out, &inferErr);
                check(inferred, (std::string(label) + ": Infer() succeeds").c_str());
                if (!inferred) {
                    return -1.0f;
                }
                float worst = 0.0f;
                for (int i = 0; i < 64; ++i) {
                    const size_t off = static_cast<size_t>(i) * (4 + kModalChannels * 4);
                    int32_t flatIndex = 0;
                    std::memcpy(&flatIndex, expectedBytes.data() + off, sizeof(flatIndex));
                    for (int c = 0; c < kModalChannels; ++c) {
                        float expectedV = 0.0f;
                        std::memcpy(&expectedV, expectedBytes.data() + off + 4 + c * 4, sizeof(float));
                        const float gotV = out[static_cast<size_t>(c) * 4096 + static_cast<size_t>(flatIndex)];
                        worst = (std::max)(worst, std::fabs(gotV - expectedV));
                    }
                }
                char what[128];
                std::snprintf(what, sizeof(what), "%s: fixture inference matches fixture_out.bin (max|d|=%g)",
                             label, static_cast<double>(worst));
                check(worst < 1.0e-3f, what);
                return worst;
            };

            // ---- sub-09 (M76e2): AVX2 経路とスカラー経路の両方を通す ----
            // AVX2 対応機でも SetForceScalar(true) でスカラーへ強制できることを selftest から
            // 確かめる (spec §5 受け入れ条件 23「両経路が腐らない」)。ハードウェアが AVX2 非対応の
            // 機種では runFixture 呼び出し 2 本とも自動的にスカラー経路を通る (UsingAvx2() が
            // false を返すため) — その場合でも「腐っていないか」を通す目的は達成される
            CpuModalBackend cpuAuto;
            const float diffAuto = runFixture(cpuAuto, "CpuModalBackend (auto)");
            check(diffAuto >= 0.0f, "CpuModalBackend (auto): fixture inference produced a result");
            MYE_LOG_INFO("  [sub-09] auto path avx2=%d threads=%d max|d|=%g", cpuAuto.UsingAvx2() ? 1 : 0,
                        cpuAuto.EffectiveThreadCount(), static_cast<double>(diffAuto));

            CpuModalBackend cpuScalar;
            cpuScalar.SetForceScalar(true);
            const float diffScalar = runFixture(cpuScalar, "CpuModalBackend (forced scalar)");
            check(diffScalar >= 0.0f, "CpuModalBackend (forced scalar): fixture inference produced a result");
            check(!cpuScalar.UsingAvx2(), "SetForceScalar(true) makes UsingAvx2() return false");
            MYE_LOG_INFO("  [sub-09] forced-scalar path avx2=%d threads=%d max|d|=%g",
                        cpuScalar.UsingAvx2() ? 1 : 0, cpuScalar.EffectiveThreadCount(),
                        static_cast<double>(diffScalar));

            // ---- sub-09 round 2: スレッド数を変えても結果がビット一致する (受け入れ条件 23) ----
            // GEMM のリダクション (K 次元) をスレッドで割らないだけでは不十分だった (round 1 の
            // 見逃し、CpuModalBackend.cpp の ParallelSpan コメント参照) — AVX2 の 8 列ブロック
            // (FMA = 1 回丸め) とスカラー端数 (乗算+加算 = 2 回丸め) は丸めが違うので、
            // チャンク境界がスレッド数で動くと「どの列が AVX2 でどの列が端数か」が変わって
            // 結果が変わりうる。**SIMD 幅 (8) で割り切れないスレッド数 (3 / 5) を含めないと
            // 検出できない** (2 冪だけならチャンク境界が常に 8 に揃ってしまい、原理的に
            // バグを踏まない — round 1 で実際にこれで見逃した)。CLI レベル
            // (--modal-bake を MYE_MODAL_THREADS=1/3/4/5 で焼いて .msfm を比較) は手動検証で
            // 別途確認済みだが、ここでは Infer() の戻り値そのものを memcmp する
            if (gridOk) {
                std::vector<float> baseline;
                std::string baseErr;
                CpuModalBackend cpuBase;
                cpuBase.SetThreadCountOverride(1);
                const bool baseOk = cpuBase.Prepare(net, &baseErr) && cpuBase.Infer(grid, baseline, &baseErr);
                check(baseOk, "CpuModalBackend: thread-count baseline (T=1) Infer() succeeds");

                const int threadCounts[] = { 2, 3, 4, 5, 8 }; // 3 / 5 が SIMD 幅で割り切れない本数
                for (const int tcount : threadCounts) {
                    CpuModalBackend cpuT;
                    cpuT.SetThreadCountOverride(tcount);
                    std::string errT;
                    std::vector<float> outT;
                    const bool okT = cpuT.Prepare(net, &errT) && cpuT.Infer(grid, outT, &errT);

                    char label[96];
                    std::snprintf(label, sizeof(label), "CpuModalBackend: T=%d Infer() succeeds", tcount);
                    check(okT, label);

                    const bool sameSize = baseOk && okT && baseline.size() == outT.size();
                    const bool bitIdentical = sameSize
                        && std::memcmp(baseline.data(), outT.data(), baseline.size() * sizeof(float)) == 0;
                    std::snprintf(label, sizeof(label),
                                 "CpuModalBackend: Infer() output is bit-identical T=1 vs T=%d", tcount);
                    check(bitIdentical, label);
                }
            }
        }
    }

    // ---- (13) .msfm 表の往復: memcmp 一致 (ConvexColliderLibrary の .mcvx 表と同型) ----
    {
        ModalFeatureMap mapA;
        mapA.version = kMsfmVersion;
        mapA.modelHash = 0x1122334455667788ULL;
        mapA.frame.origin[0] = 1.0f;
        mapA.frame.origin[1] = 2.0f;
        mapA.frame.origin[2] = 3.0f;
        mapA.frame.voxelSize = 0.125f;
        mapA.frame.longestEdge = 4.0f;
        mapA.validCount = 2;
        mapA.cellSlot[0] = 0;
        mapA.cellSlot[1] = 1;
        for (int c = 2; c < 4096; ++c) {
            mapA.cellSlot[c] = 0; // 全部 cell 0 へ丸める (テストなので意味は問わない)
        }
        mapA.feat.assign(static_cast<size_t>(2) * kModalChannels, 0);
        for (size_t i = 0; i < mapA.feat.size(); ++i) {
            mapA.feat[i] = static_cast<uint16_t>(i * 37 + 5);
        }
        std::vector<std::pair<std::string, ModalFeatureMap>> table;
        table.emplace_back("guid://0000000000000001#mesh0#prim0", mapA);
        table.emplace_back("guid://0000000000000002#mesh1#prim0", mapA);

        std::vector<uint8_t> bytesA;
        SerializeModalTable(table, bytesA);
        std::vector<std::pair<std::string, ModalFeatureMap>> roundTrip;
        const bool ok = DeserializeModalTable(bytesA, roundTrip);
        check(ok, ".msfm table deserializes successfully");
        std::vector<uint8_t> bytesB;
        SerializeModalTable(roundTrip, bytesB);
        check(bytesA == bytesB, ".msfm table serialize -> deserialize -> serialize is byte-identical");
        check(roundTrip.size() == 2 && roundTrip[0].second.validCount == 2,
              ".msfm table round-trip preserves entry count and fields");

        // RowOf/CellFeature: cellSlot 経由で有効 cell へ丸め、feat の格納順 (cell index 昇順) での
        // 行を返す (無効 cell はすべて 0 へ丸めてあるので row 0 に落ちる)
        check(mapA.RowOf(0) == 0 && mapA.RowOf(1) == 1 && mapA.RowOf(500) == 0,
              "ModalFeatureMap::RowOf resolves raw cells to their packed row");
        ModalCellFeature feature;
        const bool gotFeature = mapA.CellFeature(1, feature);
        check(gotFeature
                  && std::fabs(feature.v[0]
                               - DirectX::PackedVector::XMConvertHalfToFloat(mapA.feat[kModalChannels]))
                      < 1.0e-6f,
              "ModalFeatureMap::CellFeature decompresses the fp16 row it points at");

        // 壊れた blob (version 違い) は false + out.clear()
        std::vector<uint8_t> corrupt = bytesA;
        corrupt[0] = 0xFF;
        std::vector<std::pair<std::string, ModalFeatureMap>> corruptOut;
        check(!DeserializeModalTable(corrupt, corruptOut) && corruptOut.empty(),
              "a corrupt .msfm table (bad version) is rejected");
    }

    // ---- (14) ModalSoundLibrary: Register->Get / NoModel / BakeSync (fixture + cube) ----
    {
        RenderResources resources;
        const AssetID cubeId = resources.meshes.Cube();

        ModalSoundLibrary lib;
        lib.Init(&resources);
        check(lib.Request(cubeId) == ModalState::NoModel,
              "Request() with no model loaded returns NoModel (does not crash)");

        ModalFeatureMap manual;
        manual.validCount = 1;
        manual.feat.assign(kModalChannels, 0);
        lib.Register(cubeId, manual);
        check(lib.Get(cubeId) != nullptr && lib.Get(cubeId)->validCount == 1,
              "Register() then Get() returns what was registered");
        lib.Clear();
        check(lib.Get(cubeId) == nullptr, "Clear() forgets registered entries");

        check(lib.SetBackendByName("cpu"), "SetBackendByName(\"cpu\") succeeds");
        const std::wstring repoRoot = FindEngineRepoRoot();
        const bool modelLoaded = lib.LoadModel(repoRoot + L"\\tests\\deepmodal\\fixture.dmnet");
        check(modelLoaded, "ModalSoundLibrary::LoadModel loads the fixture .dmnet");
        if (modelLoaded) {
            check(lib.BakeSync(cubeId), "BakeSync(builtin cube) succeeds with the fixture model");
            const ModalFeatureMap* map = lib.Get(cubeId);
            check(map != nullptr && lib.Request(cubeId) == ModalState::Ready,
                  "after BakeSync the cube is Ready");
            if (map != nullptr) {
                // validCount を Voxelizer から独立に (BuildCellSlotTable 経由で) 数え直して照合する
                Mesh* mesh = resources.meshes.Get(cubeId);
                modal::VoxelGrid grid;
                modal::VoxelizeMesh(mesh->positions.data(), mesh->positions.size(),
                                    mesh->indices.data(), mesh->indices.size(), grid);
                uint16_t independentSlot[4096];
                modal::BuildCellSlotTable(grid, independentSlot);
                uint32_t independentValid = 0;
                for (int c = 0; c < 4096; ++c) {
                    if (independentSlot[c] == c) {
                        ++independentValid;
                    }
                }
                check(map->validCount == independentValid,
                      "BakeSync validCount matches an independently counted valid-cell count");
            }
        }

        // BakeSync は未登録メッシュに対して false を返す (mesh が無い AssetID)
        const AssetID bogus{ 0xDEADBEEFULL };
        check(!lib.BakeSync(bogus), "BakeSync on an unregistered mesh fails cleanly");
    }

    // ---- (15) SourcePathForSubAssetKey: builtin:// は空、guid:// キーは
    //           ConvexCookSourcePath と同じ経路 (M76e で 1 本化) ----
    {
        check(assetkey::SourcePathForSubAssetKey("builtin://cube").empty(),
              "SourcePathForSubAssetKey(\"builtin://cube\") is empty (not a cookable source)");
        // resolver 未設定の selftest では guid:// も未解決 = 空 (Convex 側と同じ結果になること自体が
        // 検査したい契約 — どちらも ParseSubAssetKey + assetguid::ResolvePath の 1 本を通る)
        const std::string key = "guid://0000000000000042#mesh0#prim0";
        check(assetkey::SourcePathForSubAssetKey(key) == ConvexCookSourcePath(key),
              "SourcePathForSubAssetKey and (delegating) ConvexCookSourcePath agree");
    }

    // ---- (16) バックエンド名の解決: cpu はそのまま、d3d11cs は WARN + cpu へ縮退、
    //           綴り違いは false ----
    {
        ModalSoundLibrary lib;
        check(lib.SetBackendByName("cpu") && std::string(lib.BackendName()) == "cpu",
              "SetBackendByName(\"cpu\") installs the cpu backend");
        check(lib.SetBackendByName("d3d11cs") && std::string(lib.BackendName()) == "cpu",
              "SetBackendByName(\"d3d11cs\") falls back to cpu (Name() == \"cpu\")");
        check(!lib.SetBackendByName("foo"), "SetBackendByName(\"foo\") is rejected");
    }

    if (failCount == 0) {
        MYE_LOG_INFO("==== Modal (Voxelizer) self test: ALL PASS ====");
    } else {
        MYE_LOG_ERROR("==== Modal (Voxelizer) self test: %d FAILED ====", failCount);
    }
    return failCount == 0;
}

} // namespace mye
