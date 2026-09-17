//====================================================================================
//                          Voxelizer.cpp
//  MyEngine/ 秋田蓮音                                                      09/16/2026
//                                          32^3 ボクセル化の実装 (SAT 表面判定 + flood-fill 内部充填)
//====================================================================================
#include "Engine/Engine/Modal/Voxelizer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "Engine/Core/Check.h"

using namespace DirectX;

namespace mye {
namespace modal {
namespace {

// ---- Akenine-Möller の三角形/箱オーバーラップ判定 (9 軸: 三角形の 3 辺 × 箱の 3 軸) ----
// 各辺は箱のある座標軸との外積で分離軸を作るが、辺の両端点は分離軸への射影が等しいので
// (辺の方向は軸と直交する)、三角形側は「辺の代表点 1 個 + 残り 1 頂点」の 2 点だけ調べれば足りる。
// 引数の v0/v1 はどの 2 頂点を選ぶかを呼び出し側が決める (原著 (Akenine-Möller, 2001) と同じ選び方)。
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

// 三角形の平面と箱の分離判定 (10 本目の軸 = 三角形の面法線)
bool PlaneBoxOverlap(const float normal[3], const float vert[3], const float boxHalf[3])
{
    float vmin[3];
    float vmax[3];
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

void AppendU32(std::vector<uint8_t>& buf, uint32_t v)
{
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&v);
    buf.insert(buf.end(), b, b + sizeof(v));
}

void AppendF32(std::vector<uint8_t>& buf, float v)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    AppendU32(buf, bits);
}

} // namespace

bool TriBoxOverlap(const float boxCenter[3], const float boxHalfSize[3], const float tri[3][3])
{
    const float v0[3] = { tri[0][0] - boxCenter[0], tri[0][1] - boxCenter[1],
                          tri[0][2] - boxCenter[2] };
    const float v1[3] = { tri[1][0] - boxCenter[0], tri[1][1] - boxCenter[1],
                          tri[1][2] - boxCenter[2] };
    const float v2[3] = { tri[2][0] - boxCenter[0], tri[2][1] - boxCenter[1],
                          tri[2][2] - boxCenter[2] };

    const float e0[3] = { v1[0] - v0[0], v1[1] - v0[1], v1[2] - v0[2] };
    const float e1[3] = { v2[0] - v1[0], v2[1] - v1[1], v2[2] - v1[2] };
    const float e2[3] = { v0[0] - v2[0], v0[1] - v2[1], v0[2] - v2[2] };

    // 9 軸: e0/e1/e2 それぞれと箱の x/y/z 軸の外積
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

    // 3 軸: 三角形の AABB と箱の AABB (箱は原点中心なので ±boxHalfSize と比較するだけ)
    for (int ax = 0; ax < 3; ++ax) {
        const float mn = (std::min)({ v0[ax], v1[ax], v2[ax] });
        const float mx = (std::max)({ v0[ax], v1[ax], v2[ax] });
        if (mn > boxHalfSize[ax] || mx < -boxHalfSize[ax]) {
            return false;
        }
    }

    // 1 軸: 三角形の面法線 (退化三角形は法線が 0 になり、この軸は自動的に「分離しない」判定になる —
    // 退化三角形が辺として拾われるのは上の 9 軸のおかげ)
    const float normal[3] = { e0[1] * e1[2] - e0[2] * e1[1], e0[2] * e1[0] - e0[0] * e1[2],
                              e0[0] * e1[1] - e0[1] * e1[0] };
    return PlaneBoxOverlap(normal, v0, boxHalfSize);
}

bool VoxelizeMesh(const XMFLOAT3* pos, size_t n, const uint32_t* idx, size_t m, VoxelGrid& out)
{
    if (pos == nullptr || idx == nullptr || n == 0 || m == 0 || m % 3 != 0) {
        return false;
    }

    XMFLOAT3 mn = pos[0];
    XMFLOAT3 mx = pos[0];
    for (size_t i = 1; i < n; ++i) {
        mn.x = (std::min)(mn.x, pos[i].x);
        mn.y = (std::min)(mn.y, pos[i].y);
        mn.z = (std::min)(mn.z, pos[i].z);
        mx.x = (std::max)(mx.x, pos[i].x);
        mx.y = (std::max)(mx.y, pos[i].y);
        mx.z = (std::max)(mx.z, pos[i].z);
    }
    const float extentX = mx.x - mn.x;
    const float extentY = mx.y - mn.y;
    const float extentZ = mx.z - mn.z;
    const float longestEdge = (std::max)({ extentX, extentY, extentZ });
    if (!(longestEdge > 0.0f)) {
        return false; // 縮退メッシュ (単一点 / 面積ゼロの平坦すぎる形状)
    }

    out = VoxelGrid{};
    // spec §4.1 (M76b round 2 で訂正): h = L/28、origin = center - 16.5h。
    // 奇数 (29) で割ると AABB 中心が必ず voxel 境界 (16.0) に乗り、中心対称な薄い特徴が
    // 常に隣接 2 行に割れる (sub-02 round 1 で実測判明)。偶数 28 で割り、オフセットも
    // 半 voxel ずらす (16.5) ことで、AABB の両端 (2.5/30.5) も中心 (16.5) も
    // すべて「voxel の中心」に乗る = 境界ちょうどのケースが構造的に無くなる
    const float h = longestEdge / 28.0f;
    const float cx = (mn.x + mx.x) * 0.5f;
    const float cy = (mn.y + mx.y) * 0.5f;
    const float cz = (mn.z + mx.z) * 0.5f;
    out.frame.voxelSize = h;
    out.frame.longestEdge = longestEdge;
    out.frame.aabbMin[0] = mn.x;
    out.frame.aabbMin[1] = mn.y;
    out.frame.aabbMin[2] = mn.z;
    out.frame.aabbMax[0] = mx.x;
    out.frame.aabbMax[1] = mx.y;
    out.frame.aabbMax[2] = mx.z;
    out.frame.origin[0] = cx - 16.5f * h;
    out.frame.origin[1] = cy - 16.5f * h;
    out.frame.origin[2] = cz - 16.5f * h;

    // 三角形と重なる voxel だけを SAT で判定する (箱半径は h/2 に境界の誤検出防止マージンを足す)。
    // spec §4.1: 保守的表面は 26-分離 (6 近傍 flood-fill で漏れない) を成立させるためのマージン
    const float halfBox = h * 0.5f + 1.0e-6f * h;
    const float boxHalf[3] = { halfBox, halfBox, halfBox };
    const int N = kModalVoxelN;

    auto clampIndex = [&](int v) { return (std::max)(0, (std::min)(N - 1, v)); };

    for (size_t f = 0; f + 3 <= m; f += 3) {
        const XMFLOAT3& p0 = pos[idx[f + 0]];
        const XMFLOAT3& p1 = pos[idx[f + 1]];
        const XMFLOAT3& p2 = pos[idx[f + 2]];
        const float tri[3][3] = { { p0.x, p0.y, p0.z }, { p1.x, p1.y, p1.z },
                                  { p2.x, p2.y, p2.z } };

        // 三角形の voxel 空間 AABB (箱半径ぶん広げてから floor/ceil、SAT が偽陽性を弾く)
        int lo[3];
        int hi[3];
        for (int ax = 0; ax < 3; ++ax) {
            const float triMin = (std::min)({ tri[0][ax], tri[1][ax], tri[2][ax] });
            const float triMax = (std::max)({ tri[0][ax], tri[1][ax], tri[2][ax] });
            const float loF = (triMin - out.frame.origin[ax]) / h - 1.0f;
            const float hiF = (triMax - out.frame.origin[ax]) / h + 1.0f;
            lo[ax] = clampIndex(static_cast<int>(std::floor(loF)));
            hi[ax] = clampIndex(static_cast<int>(std::ceil(hiF)));
        }

        for (int z = lo[2]; z <= hi[2]; ++z) {
            for (int y = lo[1]; y <= hi[1]; ++y) {
                for (int x = lo[0]; x <= hi[0]; ++x) {
                    const int vi = VoxelIndexOf(x, y, z);
                    if (out.occ[vi]) {
                        continue; // 既に表面として立っている
                    }
                    const float boxCenter[3] = { out.frame.origin[0] + (x + 0.5f) * h,
                                                 out.frame.origin[1] + (y + 0.5f) * h,
                                                 out.frame.origin[2] + (z + 0.5f) * h };
                    if (TriBoxOverlap(boxCenter, boxHalf, tri)) {
                        out.occ[vi] = 1;
                        ++out.surfaceCount;
                    }
                }
            }
        }
    }

    FloodFillInterior(out);
    return true;
}

void FloodFillInterior(VoxelGrid& grid)
{
    constexpr int N = kModalVoxelN;
    constexpr size_t kTotal = static_cast<size_t>(N) * N * N;
    std::vector<uint8_t> visited(kTotal, 0); // 0=未訪問, 1=外部確定

    // 明示スタック (unordered を使わない = 決定論)。push 順は x→y→z の昇順で固定
    std::vector<int32_t> stack;
    stack.reserve(kTotal / 4);
    for (int z = 0; z < N; ++z) {
        for (int y = 0; y < N; ++y) {
            for (int x = 0; x < N; ++x) {
                const bool onPad = (x == 0 || x == N - 1 || y == 0 || y == N - 1 || z == 0
                                    || z == N - 1);
                if (!onPad) {
                    continue;
                }
                const int idx = VoxelIndexOf(x, y, z);
                if (!grid.occ[idx] && !visited[idx]) {
                    visited[idx] = 1;
                    stack.push_back(idx);
                }
            }
        }
    }

    // 6 近傍を固定順 (-X,+X,-Y,+Y,-Z,+Z) で辿る。到達可否は探索順に依らない
    // (最終的な visited 集合は「pad から 6-連結で届くか」だけで決まるため、
    //  スタック/キューのどちらでも・どの順で push しても結果は同じになる)
    static constexpr int kDx[6] = { -1, 1, 0, 0, 0, 0 };
    static constexpr int kDy[6] = { 0, 0, -1, 1, 0, 0 };
    static constexpr int kDz[6] = { 0, 0, 0, 0, -1, 1 };
    while (!stack.empty()) {
        const int idx = stack.back();
        stack.pop_back();
        const int z = idx / (N * N);
        const int rem = idx % (N * N);
        const int y = rem / N;
        const int x = rem % N;
        for (int k = 0; k < 6; ++k) {
            const int nx = x + kDx[k];
            const int ny = y + kDy[k];
            const int nz = z + kDz[k];
            if (nx < 0 || nx >= N || ny < 0 || ny >= N || nz < 0 || nz >= N) {
                continue;
            }
            const int nidx = VoxelIndexOf(nx, ny, nz);
            if (grid.occ[nidx] || visited[nidx]) {
                continue;
            }
            visited[nidx] = 1;
            stack.push_back(nidx);
        }
    }

    uint32_t interior = 0;
    for (size_t i = 0; i < kTotal; ++i) {
        if (!grid.occ[i] && !visited[i]) {
            grid.occ[i] = 1;
            ++interior;
        }
    }
    grid.interiorCount = interior;
}

uint16_t LocalPointToCell(const VoxelFrame& frame, const float p[3])
{
    int v[3];
    for (int ax = 0; ax < 3; ++ax) {
        const float f = std::floor((p[ax] - frame.origin[ax]) / frame.voxelSize);
        const int vi = static_cast<int>(f);
        v[ax] = (std::max)(0, (std::min)(kModalVoxelN - 1, vi));
    }
    const int cx = v[0] >> 1;
    const int cy = v[1] >> 1;
    const int cz = v[2] >> 1;
    return static_cast<uint16_t>(CellIndexOf(cx, cy, cz));
}

void BuildCellSlotTable(const VoxelGrid& grid, uint16_t cellSlot[4096])
{
    constexpr int M = kModalMapN;
    std::array<bool, 4096> valid{};
    for (int cz = 0; cz < M; ++cz) {
        for (int cy = 0; cy < M; ++cy) {
            for (int cx = 0; cx < M; ++cx) {
                bool occupied = false;
                for (int dz = 0; dz < 2 && !occupied; ++dz) {
                    for (int dy = 0; dy < 2 && !occupied; ++dy) {
                        for (int dx = 0; dx < 2 && !occupied; ++dx) {
                            const int vx = cx * 2 + dx;
                            const int vy = cy * 2 + dy;
                            const int vz = cz * 2 + dz;
                            if (grid.occ[VoxelIndexOf(vx, vy, vz)]) {
                                occupied = true;
                            }
                        }
                    }
                }
                valid[static_cast<size_t>(CellIndexOf(cx, cy, cz))] = occupied;
            }
        }
    }

    // 座標を先に作っておく (総当たり距離計算を毎回 3 重ループし直さないため)
    struct Coord {
        int x, y, z;
    };
    std::array<Coord, 4096> coordOf{};
    for (int cz = 0; cz < M; ++cz) {
        for (int cy = 0; cy < M; ++cy) {
            for (int cx = 0; cx < M; ++cx) {
                coordOf[static_cast<size_t>(CellIndexOf(cx, cy, cz))] = { cx, cy, cz };
            }
        }
    }

    for (int cell = 0; cell < 4096; ++cell) {
        if (valid[static_cast<size_t>(cell)]) {
            cellSlot[cell] = static_cast<uint16_t>(cell);
            continue;
        }
        const Coord& c = coordOf[static_cast<size_t>(cell)];
        int best = -1;
        int64_t bestDist = -1;
        for (int other = 0; other < 4096; ++other) {
            if (!valid[static_cast<size_t>(other)]) {
                continue;
            }
            const Coord& o = coordOf[static_cast<size_t>(other)];
            const int64_t dx = c.x - o.x;
            const int64_t dy = c.y - o.y;
            const int64_t dz = c.z - o.z;
            const int64_t dist = dx * dx + dy * dy + dz * dz;
            if (best < 0 || dist < bestDist) {
                // 同値は index の小さい方 (other は昇順に舐めているので、
                // 最初に見つかった最小距離を後から同値で上書きしないことが「小さい方を残す」になる)
                best = other;
                bestDist = dist;
            }
        }
        cellSlot[cell] = static_cast<uint16_t>(best < 0 ? cell : best);
    }
}

void SerializeVox(const VoxelGrid& grid, std::vector<uint8_t>& bytes)
{
    bytes.clear();
    bytes.reserve(kVoxHeaderBytes + kVoxOccBytes);
    AppendU32(bytes, kVoxMagic);
    AppendU32(bytes, kVoxVersion);
    AppendU32(bytes, static_cast<uint32_t>(kModalVoxelN));
    AppendU32(bytes, static_cast<uint32_t>(kModalVoxelPad));
    for (float v : grid.frame.origin) {
        AppendF32(bytes, v);
    }
    AppendF32(bytes, grid.frame.voxelSize);
    for (float v : grid.frame.aabbMin) {
        AppendF32(bytes, v);
    }
    for (float v : grid.frame.aabbMax) {
        AppendF32(bytes, v);
    }
    AppendF32(bytes, grid.frame.longestEdge);
    AppendU32(bytes, grid.surfaceCount);
    AppendU32(bytes, grid.interiorCount);
    AppendU32(bytes, 0u); // reserved
    // ヘッダが宣言どおりの長さになっているかを自己検査 (フィールドを足し引きしたときのズレ検知)
    MYE_CHECK(bytes.size() == kVoxHeaderBytes);
    bytes.insert(bytes.end(), grid.occ.begin(), grid.occ.end());
}

bool DeserializeVox(const std::vector<uint8_t>& bytes, VoxelGrid& grid)
{
    if (bytes.size() != kVoxHeaderBytes + kVoxOccBytes) {
        return false;
    }
    size_t pos = 0;
    auto readU32 = [&](uint32_t& v) {
        std::memcpy(&v, bytes.data() + pos, sizeof(uint32_t));
        pos += sizeof(uint32_t);
    };
    auto readF32 = [&](float& v) {
        uint32_t bits = 0;
        readU32(bits);
        std::memcpy(&v, &bits, sizeof(v));
    };

    uint32_t magic = 0, version = 0, n = 0, pad = 0;
    readU32(magic);
    readU32(version);
    readU32(n);
    readU32(pad);
    if (magic != kVoxMagic || version != kVoxVersion || n != static_cast<uint32_t>(kModalVoxelN)) {
        return false;
    }

    VoxelGrid out{};
    for (float& v : out.frame.origin) {
        readF32(v);
    }
    readF32(out.frame.voxelSize);
    for (float& v : out.frame.aabbMin) {
        readF32(v);
    }
    for (float& v : out.frame.aabbMax) {
        readF32(v);
    }
    readF32(out.frame.longestEdge);
    readU32(out.surfaceCount);
    readU32(out.interiorCount);
    uint32_t reserved = 0;
    readU32(reserved);
    if (pos != kVoxHeaderBytes) {
        return false; // ヘッダ長の宣言とここまでの読み取り量が食い違う (フィールド追加漏れ等)
    }
    std::memcpy(out.occ.data(), bytes.data() + pos, kVoxOccBytes);

    grid = out;
    return true;
}

} // namespace modal
} // namespace mye
