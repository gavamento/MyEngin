#include "Editor/TerrainSelfTest.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/Components.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/Asset/CookedCache.h" // M58f: サイドカーとクックキャッシュの相互作用
#include "Engine/Engine/Asset/TerrainAsset.h"
#include "Engine/Engine/Asset/TerrainEdit.h" // M58f: ブラシ / パッチ / サイドカー
#include "Engine/Engine/TerrainSystem.h"
#include "Engine/Renderer/FrustumCull.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/TerrainPass.h" // M58c: 描画順 (TerrainDrawOrderLess)

namespace fs = std::filesystem;

using DirectX::XMFLOAT3;
using DirectX::XMFLOAT4X4;
using DirectX::XMMATRIX;

namespace mye {
namespace {

// 検査用の合成地形。ノイズを使わない (テストが TerrainAsset の生成器の変更で揺れないよう、
// 高さは座標から直接作る)。tiles = 16 x 8、ワールド 64 x 32 m
TerrainAsset::TerrainData MakeTestTerrain(uint32_t hw = 17, uint32_t hh = 9)
{
    TerrainAsset::TerrainData d;
    d.heightW = hw;
    d.heightH = hh;
    d.splatW = 4;
    d.splatH = 4;
    d.worldSizeX = 64.0f;
    d.worldSizeZ = 32.0f;
    d.heightBase = -2.0f;
    d.heightScale = 20.0f;
    d.heights.resize(static_cast<size_t>(hw) * hh);
    for (uint32_t z = 0; z < hh; ++z) {
        for (uint32_t x = 0; x < hw; ++x) {
            // x と z で別の周期を持たせる (行と列を取り違える実装ミスを PASS にしない)
            const uint32_t v = (x * 4099u + z * 907u) % 65536u;
            d.heights[static_cast<size_t>(z) * hw + x] = static_cast<uint16_t>(v);
        }
    }
    d.splat.assign(static_cast<size_t>(d.splatW) * d.splatH * 4, 0);
    for (size_t i = 0; i < d.splat.size(); i += 4) {
        d.splat[i] = static_cast<uint8_t>(TerrainAsset::kSplatWeightSum);
    }
    d.layers.push_back({ "base", "", "", 8.0f, 8.0f });
    return d;
}

// LOD の検査用に**わざと非線形**な地形 (M58e)。
// ★MakeTestTerrain は高さが x と z の 1 次式なので、LOD で間引いた制御点を線形補間しても
//   元の高さと**完全に一致してしまう** — 「LOD 境界の段差」が構造的にゼロになり、
//   クラック検査が何も検査しないまま緑になる (実際に一度そうなった)。
//   整数ハッシュで隣接 texel を無相関にして、間引きが必ず誤差を生む状態にする
TerrainAsset::TerrainData MakeBumpyTerrain(uint32_t hw = 17, uint32_t hh = 17)
{
    TerrainAsset::TerrainData d;
    d.heightW = hw;
    d.heightH = hh;
    d.splatW = 4;
    d.splatH = 4;
    d.worldSizeX = 64.0f;
    d.worldSizeZ = 64.0f;
    d.heightBase = -2.0f;
    d.heightScale = 20.0f;
    d.heights.resize(static_cast<size_t>(hw) * hh);
    for (uint32_t z = 0; z < hh; ++z) {
        for (uint32_t x = 0; x < hw; ++x) {
            uint32_t h = 2166136261u; // FNV-1a (実時間も rand() も混ざらない = 決定論)
            h = (h ^ (x + 1u)) * 16777619u;
            h = (h ^ (z + 1u)) * 16777619u;
            // ★雪崩化 (finalizer) が要る。FNV だけだと隣の texel との差が「低位ビットの差 x
            //   16777619」= 抽出する bit 8..23 でほぼ 1 しか動かず、**結果としてまた
            //   ほぼ 1 次式の地形**になる (段差 1.2m しか出ず、検査がほとんど効かなかった)
            h ^= h >> 15;
            h *= 2246822519u;
            h ^= h >> 13;
            d.heights[static_cast<size_t>(z) * hw + x] = static_cast<uint16_t>(h & 0xFFFFu);
        }
    }
    d.splat.assign(static_cast<size_t>(d.splatW) * d.splatH * 4, 0);
    for (size_t i = 0; i < d.splat.size(); i += 4) {
        d.splat[i] = static_cast<uint8_t>(TerrainAsset::kSplatWeightSum);
    }
    d.layers.push_back({ "base", "", "", 8.0f, 8.0f });
    return d;
}

// ブラシ検査用 (M58f)。高さは MakeBumpyTerrain と同じ非線形 (平滑化が効いたことを測れる) で、
// スプラットを 32x32 まで細かくして 4 レイヤ持たせる (4x4 だとブラシ 1 個で全 texel が
// 変わってしまい「半径の外は触らない」を検査できない)
TerrainAsset::TerrainData MakeBrushTerrain()
{
    TerrainAsset::TerrainData d = MakeBumpyTerrain(33, 33);
    d.splatW = 32;
    d.splatH = 32;
    d.splat.assign(static_cast<size_t>(d.splatW) * d.splatH * 4, 0);
    for (size_t i = 0; i < d.splat.size(); i += 4) {
        d.splat[i] = static_cast<uint8_t>(TerrainAsset::kSplatWeightSum);
    }
    d.layers.clear();
    for (const char* name : { "a", "b", "c", "d" }) {
        d.layers.push_back({ name, "", "", 8.0f, 8.0f });
    }
    return d;
}

// 頂点 texel → 地形ローカル XZ (TerrainEdit の内部と同じ規約。テスト側でも要る)
float TexelLocalX(const TerrainAsset::TerrainData& d, uint32_t x)
{
    return (static_cast<float>(x) / static_cast<float>(d.heightW - 1) - 0.5f) * d.worldSizeX;
}
float TexelLocalZ(const TerrainAsset::TerrainData& d, uint32_t z)
{
    return (static_cast<float>(z) / static_cast<float>(d.heightH - 1) - 0.5f) * d.worldSizeZ;
}

// 隣接 texel の高さ差の総和 (= 地表の粗さ)。平滑化が本当に均しているかの尺度
uint64_t Roughness(const TerrainAsset::TerrainData& d)
{
    uint64_t sum = 0;
    for (uint32_t z = 0; z < d.heightH; ++z) {
        for (uint32_t x = 0; x + 1 < d.heightW; ++x) {
            const size_t i = static_cast<size_t>(z) * d.heightW + x;
            sum += static_cast<uint64_t>(std::abs(static_cast<int32_t>(d.heights[i + 1])
                                                  - static_cast<int32_t>(d.heights[i])));
        }
    }
    return sum;
}

// X 方向に鏡映した地形 (走査順依存を炙り出すための変換。詳細は節 (14)(c))
TerrainAsset::TerrainData MirrorX(const TerrainAsset::TerrainData& s)
{
    TerrainAsset::TerrainData m = s;
    for (uint32_t z = 0; z < s.heightH; ++z) {
        for (uint32_t x = 0; x < s.heightW; ++x) {
            m.heights[static_cast<size_t>(z) * s.heightW + x] =
                s.heights[static_cast<size_t>(z) * s.heightW + (s.heightW - 1 - x)];
        }
    }
    return m;
}

// 行ベクトル規約の view*proj から視錐台を作る (RenderSystem と同じ手順)
Frustum MakeFrustum(const XMFLOAT3& eye, const XMFLOAT3& at, const XMFLOAT3& up, float fovDeg,
                    float nearZ, float farZ)
{
    const XMMATRIX v = DirectX::XMMatrixLookAtLH(
        DirectX::XMVectorSet(eye.x, eye.y, eye.z, 1.0f),
        DirectX::XMVectorSet(at.x, at.y, at.z, 1.0f),
        DirectX::XMVectorSet(up.x, up.y, up.z, 0.0f));
    const XMMATRIX p = DirectX::XMMatrixPerspectiveFovLH(DirectX::XMConvertToRadians(fovDeg), 1.0f,
                                                         nearZ, farZ);
    XMFLOAT4X4 vp;
    DirectX::XMStoreFloat4x4(&vp, v * p);
    return BuildFrustum(vp);
}

XMFLOAT4X4 Identity()
{
    return XMFLOAT4X4{ 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
}

} // namespace

bool RunTerrainSelfTest()
{
    MYE_LOG_INFO("==== Terrain (chunking / culling) self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    RegisterBuiltinComponents(); // sTypeId 解決 (冪等)

    // ---- (1) chunkTiles の丸め ----
    {
        check(ClampChunkTiles(0) == static_cast<uint32_t>(kTerrainMinChunkTiles)
                  && ClampChunkTiles(-100) == static_cast<uint32_t>(kTerrainMinChunkTiles)
                  && ClampChunkTiles(1) == static_cast<uint32_t>(kTerrainMinChunkTiles),
              "terrain: chunkTiles below the floor is clamped up (never 0 = division by zero)");
        check(ClampChunkTiles(1 << 20) == static_cast<uint32_t>(kTerrainMaxChunkTiles),
              "terrain: chunkTiles above the ceiling is clamped down");
        check(ClampChunkTiles(32) == 32u, "terrain: an in-range chunkTiles passes through");
    }

    // ---- (2) 分割: 割り切れる場合と端数が出る場合 ----
    {
        const TerrainAsset::TerrainData d = MakeTestTerrain(); // tiles 16 x 8
        TerrainChunkLayout even;
        check(BuildChunkLayout(d, 8, even) && even.countX == 2 && even.countZ == 1
                  && even.chunks.size() == 2,
              "terrain: an evenly divisible grid splits without a remainder chunk");

        TerrainChunkLayout odd;
        check(BuildChunkLayout(d, 6, odd) && odd.countX == 3 && odd.countZ == 2,
              "terrain: an indivisible grid rounds the chunk count up");
        const TerrainChunk* last = odd.At(2, 1);
        check(last != nullptr && last->tilesX == 4 && last->tilesZ == 2,
              "terrain: the trailing chunk carries the remainder tiles");
        check(odd.At(3, 0) == nullptr && odd.At(0, 2) == nullptr,
              "terrain: out-of-range chunk lookups return null instead of reading past the end");

        // 覆い: 全タイルがちょうど 1 回ずつ担当される (穴も重なりも無い)
        std::vector<int> cover(static_cast<size_t>(d.heightW - 1) * (d.heightH - 1), 0);
        for (const TerrainChunk& c : odd.chunks) {
            for (uint32_t z = c.tileZ0; z < c.tileZ0 + c.tilesZ; ++z) {
                for (uint32_t x = c.tileX0; x < c.tileX0 + c.tilesX; ++x) {
                    ++cover[static_cast<size_t>(z) * (d.heightW - 1) + x];
                }
            }
        }
        bool coveredOnce = true;
        for (int n : cover) {
            coveredOnce = coveredOnce && n == 1;
        }
        check(coveredOnce, "terrain: chunks tile the height grid exactly once (no gaps, no overlap)");

        // 隣接チャンクの AABB は境界面を共有する (隙間があるとカリングが縁を落とす)
        const TerrainChunk* c00 = odd.At(0, 0);
        const TerrainChunk* c10 = odd.At(1, 0);
        check(c00 != nullptr && c10 != nullptr && c00->localMax.x == c10->localMin.x,
              "terrain: adjacent chunk AABBs share their boundary plane exactly");

        // 端は地形の外周と一致する = ワールド寸法どおり
        const TerrainChunk* cLast = odd.At(2, 1);
        check(c00 != nullptr && cLast != nullptr
                  && std::fabs(c00->localMin.x + d.worldSizeX * 0.5f) < 1e-4f
                  && std::fabs(cLast->localMax.z - d.worldSizeZ * 0.5f) < 1e-4f,
              "terrain: the chunk grid spans exactly the authored world size (center origin)");
    }

    // ---- (3) 壊れた地形は分割しない ----
    {
        TerrainAsset::TerrainData bad = MakeTestTerrain();
        bad.heights.pop_back(); // 要素数と解像度が食い違う
        TerrainChunkLayout layout;
        check(!BuildChunkLayout(bad, 8, layout) && layout.chunks.empty(),
              "terrain: an inconsistent TerrainData yields no layout (never a partial one)");
    }

    // ---- (4) メッシュ生成: 個数・index 範囲・AABB 包含 ----
    {
        const TerrainAsset::TerrainData d = MakeTestTerrain();
        TerrainChunkLayout layout;
        BuildChunkLayout(d, 6, layout);
        std::vector<MeshVertex> verts;
        std::vector<uint32_t> indices;

        bool counts = true;
        bool inRange = true;
        bool inAabb = true;
        for (const TerrainChunk& c : layout.chunks) {
            BuildChunkMesh(d, c, verts, indices);
            counts = counts
                && verts.size() == static_cast<size_t>(c.tilesX + 1) * (c.tilesZ + 1)
                && indices.size() == static_cast<size_t>(c.tilesX) * c.tilesZ * 6;
            for (uint32_t i : indices) {
                inRange = inRange && i < verts.size();
            }
            for (const MeshVertex& mv : verts) {
                inAabb = inAabb && mv.position.x >= c.localMin.x && mv.position.x <= c.localMax.x
                    && mv.position.y >= c.localMin.y && mv.position.y <= c.localMax.y
                    && mv.position.z >= c.localMin.z && mv.position.z <= c.localMax.z;
            }
        }
        check(counts, "terrain: vertex and index counts follow the chunk tile counts");
        check(inRange, "terrain: every index addresses a vertex of its own chunk");
        // ★これが視錐台カリングの正しさの根拠 — AABB がメッシュを含まないと可視物が消える
        check(inAabb, "terrain: the chunk AABB contains every generated vertex");
    }

    // ---- (5) 継ぎ目: 隣接チャンクが共有する縁の頂点はビット一致 ----
    {
        const TerrainAsset::TerrainData d = MakeTestTerrain();
        TerrainChunkLayout layout;
        BuildChunkLayout(d, 4, layout); // 4 x 2 チャンク
        // 変数名に near / far を使わない (windows.h がマクロで潰す)
        std::vector<MeshVertex> left, right, behind;
        std::vector<uint32_t> idx;
        BuildChunkMesh(d, *layout.At(0, 0), left, idx);
        BuildChunkMesh(d, *layout.At(1, 0), right, idx);
        BuildChunkMesh(d, *layout.At(0, 1), behind, idx);

        const uint32_t lvx = layout.At(0, 0)->tilesX + 1;
        const uint32_t rvx = layout.At(1, 0)->tilesX + 1;
        bool seamX = true;
        for (uint32_t iz = 0; iz <= layout.At(0, 0)->tilesZ; ++iz) {
            const MeshVertex& a = left[static_cast<size_t>(iz) * lvx + (lvx - 1)]; // 右端の列
            const MeshVertex& b = right[static_cast<size_t>(iz) * rvx];            // 左端の列
            seamX = seamX && std::memcmp(&a.position, &b.position, sizeof(a.position)) == 0
                && std::memcmp(&a.normal, &b.normal, sizeof(a.normal)) == 0;
        }
        check(seamX, "terrain: the shared column of adjacent chunks matches bit-for-bit");

        bool seamZ = true;
        const uint32_t nvx = layout.At(0, 0)->tilesX + 1;
        const size_t lastRow = static_cast<size_t>(layout.At(0, 0)->tilesZ) * nvx;
        for (uint32_t ix = 0; ix < nvx; ++ix) {
            const MeshVertex& a = left[lastRow + ix]; // 奥端の行
            const MeshVertex& b = behind[ix];         // 手前端の行
            seamZ = seamZ && std::memcmp(&a.position, &b.position, sizeof(a.position)) == 0
                && std::memcmp(&a.normal, &b.normal, sizeof(a.normal)) == 0;
        }
        check(seamZ, "terrain: the shared row of adjacent chunks matches bit-for-bit");
    }

    // ---- (6) 平坦な地形: 法線 +Y、巻き順は builtin plane と同じ向き ----
    {
        TerrainAsset::TerrainData flat = MakeTestTerrain(9, 9);
        std::fill(flat.heights.begin(), flat.heights.end(), static_cast<uint16_t>(30000));
        TerrainChunkLayout layout;
        BuildChunkLayout(flat, 4, layout);
        std::vector<MeshVertex> verts;
        std::vector<uint32_t> indices;
        BuildChunkMesh(flat, layout.chunks[0], verts, indices);

        bool up = true;
        for (const MeshVertex& mv : verts) {
            up = up && mv.normal.x == 0.0f && mv.normal.y == 1.0f && mv.normal.z == 0.0f;
        }
        check(up, "terrain: a flat heightfield produces exactly +Y normals");

        // 幾何法線 = cross(v1-v0, v2-v0)。builtin plane (0,1,2) と同じく +Y であること。
        // ここが裏返ると裏面カリングで地形が丸ごと消える (絵を見ないと気付けない類の事故)
        const XMFLOAT3& p0 = verts[indices[0]].position;
        const XMFLOAT3& p1 = verts[indices[1]].position;
        const XMFLOAT3& p2 = verts[indices[2]].position;
        const XMFLOAT3 e1 = { p1.x - p0.x, p1.y - p0.y, p1.z - p0.z };
        const XMFLOAT3 e2 = { p2.x - p0.x, p2.y - p0.y, p2.z - p0.z };
        const float cy = e1.z * e2.x - e1.x * e2.z;
        check(cy > 0.0f, "terrain: triangle winding matches the builtin plane (front face is up)");

        check(verts[0].uv.x == 0.0f && verts[0].uv.y == 0.0f
                  && verts[verts.size() - 1].uv.x > 0.0f,
              "terrain: UV is the normalized terrain coordinate (layer tiling is a shader job)");
    }

    // ---- (7) 生成の決定論 (2 回組んでバイト一致) ----
    {
        const TerrainAsset::TerrainData d = MakeTestTerrain();
        TerrainChunkLayout layout;
        BuildChunkLayout(d, 5, layout);
        std::vector<MeshVertex> a, b;
        std::vector<uint32_t> ia, ib;
        BuildChunkMesh(d, layout.chunks[1], a, ia);
        BuildChunkMesh(d, layout.chunks[1], b, ib);
        check(ia == ib && a.size() == b.size()
                  && std::memcmp(a.data(), b.data(), a.size() * sizeof(MeshVertex)) == 0,
              "terrain: rebuilding a chunk yields identical bytes");
    }

    // ---- (8) 視錐台カリング ----
    {
        const TerrainAsset::TerrainData d = MakeTestTerrain();
        TerrainChunkLayout layout;
        BuildChunkLayout(d, 4, layout); // 4 x 2 = 8 チャンク
        std::vector<uint32_t> vis;

        const Frustum wide =
            MakeFrustum({ 0, 200, -200 }, { 0, 0, 0 }, { 0, 1, 0 }, 60.0f, 0.1f, 2000.0f);
        check(CullChunks(layout, wide, Identity(), vis) == layout.chunks.size(),
              "terrain: a frustum containing the terrain keeps every chunk");

        const Frustum away =
            MakeFrustum({ 0, 0, -2000 }, { 0, 0, -3000 }, { 0, 1, 0 }, 60.0f, 0.1f, 100.0f);
        check(CullChunks(layout, away, Identity(), vis) == 0 && vis.empty(),
              "terrain: a frustum pointing away keeps nothing");

        // 真上から狭い画角で覗く = 中央付近のチャンクだけ残る
        const Frustum narrow =
            MakeFrustum({ 0, 50, 0 }, { 0, 0, 0 }, { 0, 0, 1 }, 10.0f, 0.1f, 100.0f);
        const size_t partial = CullChunks(layout, narrow, Identity(), vis);
        check(partial > 0 && partial < layout.chunks.size(),
              "terrain: a narrow frustum keeps a strict subset of the chunks");

        // ワールド行列で地形ごと遠くへ動かすと同じ視錐台から外れる
        XMFLOAT4X4 moved = Identity();
        moved._42 = 5000.0f;
        check(CullChunks(layout, wide, moved, vis) == 0,
              "terrain: culling honours the entity world matrix");
    }

    // ---- (9) TerrainSystem: ECS からの収集 (ヘッドレス = GraphicsDevice 無し) ----
    {
        std::error_code ec;
        const fs::path root = fs::temp_directory_path(ec) / L"mye_terrain_selftest";
        fs::remove_all(root, ec);
        fs::create_directories(root / L"terrain", ec);
        const std::string src =
            R"({"type":"terrain","version":1,"worldSize":[64.0,64.0],)"
            R"("heightRes":[17,17],"splatRes":[8,8],"heightBase":0.0,"heightScale":8.0,)"
            R"("procedural":{"seed":3,"octaves":3,"frequency":2.0,"lacunarity":2.0,"gain":0.5},)"
            R"("layers":[{"name":"a"}]})";
        {
            std::ofstream f(root / L"terrain" / L"t.terrain.json", std::ios::binary | std::ios::trunc);
            f.write(src.data(), static_cast<std::streamsize>(src.size()));
        }

        World world;
        MeshLibrary meshes;      // Init を呼ばない = GPU バッファを作らない CPU 専用モード
        TextureLibrary textures; // 同上 (M58d のレイヤ解決は AssetID が空のまま返る)
        TerrainSystem terrain;
        const EntityID e = world.CreateEntity("terrain");
        auto* tc = world.AddComponent<TerrainComponent>(e);
        std::snprintf(tc->source, sizeof(tc->source), "terrain/t.terrain.json");
        tc->chunkTiles = 8; // 16 タイル / 8 = 2 x 2 チャンク

        const Frustum wide =
            MakeFrustum({ 0, 200, -200 }, { 0, 0, 0 }, { 0, 1, 0 }, 60.0f, 0.1f, 2000.0f);
        XMFLOAT4X4 view;
        DirectX::XMStoreFloat4x4(&view,
                                 DirectX::XMMatrixLookAtLH(DirectX::XMVectorSet(0, 200, -200, 1),
                                                           DirectX::XMVectorSet(0, 0, 0, 1),
                                                           DirectX::XMVectorSet(0, 1, 0, 0)));
        std::vector<TerrainDrawItem> items;
        const uint32_t total =
            terrain.Collect(world, meshes, textures, root.wstring(), wide, view, items);
        check(total == 4 && items.size() == 4,
              "terrain: Collect builds the chunk grid and reports every chunk visible");
        check(terrain.CacheSize() == 1, "terrain: the built terrain is cached once");
        bool meshed = true;
        bool depths = true;
        for (const TerrainDrawItem& it : items) {
            meshed = meshed && !it.mesh.IsNull() && meshes.Get(it.mesh) != nullptr
                && it.terrain != nullptr;
            depths = depths && it.viewZ > 0.0f; // カメラ前方
        }
        check(meshed, "terrain: every draw item points at a registered chunk mesh");
        // viewZ をエンティティ原点で作ると全チャンクが同じ値になり LOD もソートも死ぬ
        check(depths && items[0].viewZ != items[3].viewZ,
              "terrain: viewZ is per-chunk (the AABB center), not the entity origin");

        // 2 回目はキャッシュから (メッシュを作り直さない = AssetID が変わらない)
        const AssetID firstMesh = items[0].mesh;
        terrain.Collect(world, meshes, textures, root.wstring(), wide, view, items);
        check(terrain.CacheSize() == 1 && !items.empty() && items[0].mesh == firstMesh,
              "terrain: a second Collect reuses the cached chunk meshes");

        // 無効エンティティは収集しない
        world.AddComponent<ActiveComponent>(e)->enabled = 0;
        terrain.Collect(world, meshes, textures, root.wstring(), wide, view, items);
        check(items.empty(), "terrain: an inactive entity contributes no chunks");
        world.GetComponent<ActiveComponent>(e)->enabled = 1;

        // 空パス / 存在しないパスは黙って 0 件 (毎フレーム開き直さないよう失敗もキャッシュ)
        tc = world.GetComponent<TerrainComponent>(e);
        tc->source[0] = '\0';
        terrain.Collect(world, meshes, textures, root.wstring(), wide, view, items);
        check(items.empty(), "terrain: an empty source path is a no-op");
        std::snprintf(tc->source, sizeof(tc->source), "terrain/missing.terrain.json");
        const size_t cacheBefore = terrain.CacheSize();
        terrain.Collect(world, meshes, textures, root.wstring(), wide, view, items);
        terrain.Collect(world, meshes, textures, root.wstring(), wide, view, items);
        check(items.empty() && terrain.CacheSize() == cacheBefore + 1,
              "terrain: a missing asset fails once and is remembered (no per-frame retry)");

        fs::remove_all(root, ec);
    }

    // ---- (10) 描画順 (M58c): TerrainDrawOrderLess ----
    // 描画パスそのものは GPU が要るのでここでは回せないが、**描画順だけは純関数**で、
    // かつ決定論の規則 7 が直接掛かる場所なので単体で固定しておく
    {
        auto make = [](float viewZ, uint64_t mesh) {
            TerrainRenderItem it;
            it.viewZ = viewZ;
            it.mesh = AssetID{ mesh };
            return it;
        };
        // 近い順 (early-z が効く順)
        check(TerrainDrawOrderLess(make(3.0f, 1), make(9.0f, 1)),
              "terrain: draw order puts the nearer chunk first");
        check(!TerrainDrawOrderLess(make(9.0f, 1), make(3.0f, 1)),
              "terrain: draw order is antisymmetric in viewZ");
        // 同じ viewZ は AssetID で割る (元の並びに依存させない)
        check(TerrainDrawOrderLess(make(5.0f, 7), make(5.0f, 8))
                  && !TerrainDrawOrderLess(make(5.0f, 8), make(5.0f, 7)),
              "terrain: equal viewZ is broken by the mesh AssetID (deterministic key)");
        check(!TerrainDrawOrderLess(make(5.0f, 7), make(5.0f, 7)),
              "terrain: draw order is irreflexive (strict weak ordering)");
        // ★入力の並び順に結果が依存しないこと。ここが崩れると「収集順が揺れた日に
        //   描画順だけが変わる」= 再現しないピクセル差になる
        std::vector<TerrainRenderItem> a = { make(5.0f, 8), make(1.0f, 3), make(5.0f, 2),
                                             make(1.0f, 9) };
        std::vector<TerrainRenderItem> b(a.rbegin(), a.rend());
        std::sort(a.begin(), a.end(), TerrainDrawOrderLess);
        std::sort(b.begin(), b.end(), TerrainDrawOrderLess);
        bool same = a.size() == b.size();
        for (size_t i = 0; same && i < a.size(); ++i) {
            same = a[i].viewZ == b[i].viewZ && a[i].mesh == b[i].mesh;
        }
        check(same && a[0].mesh.value == 3 && a[1].mesh.value == 9 && a[2].mesh.value == 2
                  && a[3].mesh.value == 8,
              "terrain: sorting is independent of the input order");
    }

    // ---- (11) スプラットのレイヤ解決 (M58d) ----
    // シェーダのブレンドそのものは GPU が要るが、**ブレンドの入力を組む部分は純関数**。
    // ここが崩れる形は 2 つあって、どちらも絵では気付きにくい:
    //   ・レイヤ数に満たないスロットを殺し忘れる → 未使用チャンネルの重みが生き残り、
    //     シェーダの再正規化で有効レイヤの色が痩せる (「なんとなく暗い」としか見えない)
    //   ・tint を sRGB のまま CB へ載せる → 中間調だけがずれる (単色なら気付けない)
    {
        TextureLibrary textures; // ヘッドレス = GPU 生成なし。AssetID は空のまま返る
        TerrainAsset::TerrainData d = MakeTestTerrain();

        // (11a) レイヤ 2 枚 = 後半 2 スロットは tint.a = 0 で殺されていること
        d.layers.clear();
        d.layers.push_back({ "a", "", "", 4.0f, 6.0f, 1.0f, 1.0f, 1.0f });
        d.layers.push_back({ "b", "", "", 2.0f, 3.0f, 0.5f, 0.25f, 0.0f });
        TerrainSurface s = BuildTerrainSurface(d, L"C:\\nonexistent\\x.terrain.json", textures);
        check(s.layers[0].tint.w == 1.0f && s.layers[1].tint.w == 1.0f
                  && s.layers[2].tint.w == 0.0f && s.layers[3].tint.w == 0.0f,
              "terrain: layers beyond the authored count are disabled (tint.a = 0)");
        check(s.layers[0].tilingU == 4.0f && s.layers[0].tilingV == 6.0f
                  && s.layers[1].tilingU == 2.0f && s.layers[1].tilingV == 3.0f,
              "terrain: per-layer tiling is carried through to the binding");

        // (11b) tint は authored sRGB → リニア。白は 1 のまま (恒等)、中間調は必ず沈む
        check(s.layers[0].tint.x == 1.0f && s.layers[0].tint.y == 1.0f
                  && s.layers[0].tint.z == 1.0f,
              "terrain: a white tint is the identity after the sRGB conversion");
        check(s.layers[1].tint.x < 0.5f && s.layers[1].tint.x > 0.0f
                  && s.layers[1].tint.y < 0.25f && s.layers[1].tint.z == 0.0f,
              "terrain: layer tint is converted from authored sRGB to linear");

        // (11c) 有効フラグを掛けた後の重みが 1 に正規化できること。
        // シェーダ (terrain_common.hlsli) が毎ピクセルやっている計算を CPU で写して、
        // 「アセット側の不変量 + 有効フラグ」から必ず合計 1 が作れることを固定する
        auto normalizedSum = [](const TerrainSurface& surf, const uint8_t* texel) {
            float w[4] = {};
            float total = 0.0f;
            for (int i = 0; i < 4; ++i) {
                w[i] = static_cast<float>(texel[i]) / 255.0f * surf.layers[i].tint.w;
                total += w[i];
            }
            if (!(total > 1e-5f)) {
                return 1.0f; // シェーダはレイヤ 0 の 100% へ倒す = 合計 1
            }
            float sum = 0.0f;
            for (int i = 0; i < 4; ++i) {
                sum += w[i] / total;
            }
            return sum;
        };
        // 4 チャンネルすべてに重みが載った texel (= 手書きスプラットの最悪ケース)
        const uint8_t mixed[4] = { 64, 64, 64, 63 };
        check(std::fabs(normalizedSum(s, mixed) - 1.0f) < 1e-4f,
              "terrain: weights renormalize to 1 even when disabled layers carry weight");
        // 有効レイヤに重みが 1 つも無い texel (殺されたレイヤだけが 255)
        const uint8_t dead[4] = { 0, 0, 255, 0 };
        check(std::fabs(normalizedSum(s, dead) - 1.0f) < 1e-4f,
              "terrain: an all-disabled texel falls back to layer 0 instead of going black");

        // (11d) レイヤ表が空 = M58c の単色に倒す (既定値 = 従来の見た目)
        d.layers.clear();
        s = BuildTerrainSurface(d, L"C:\\nonexistent\\x.terrain.json", textures);
        check(s.layers[0].tint.w == 1.0f && s.layers[1].tint.w == 0.0f
                  && s.layers[0].tint.x > 0.1f && s.layers[0].tint.x < 0.2f
                  && s.layers[0].tint.y > s.layers[0].tint.x
                  && s.layers[0].tint.z < s.layers[0].tint.x,
              "terrain: a layerless terrain falls back to the M58c flat colour");

        // (11e) 5 枚目以降は kMaxLayers で切られる (アセット側の上限がここまで届くこと)
        d.layers.clear();
        for (int i = 0; i < 6; ++i) {
            d.layers.push_back({ "L", "", "", 1.0f, 1.0f, 1.0f, 1.0f, 1.0f });
        }
        s = BuildTerrainSurface(d, L"C:\\nonexistent\\x.terrain.json", textures);
        bool allEnabled = true;
        for (uint32_t i = 0; i < kTerrainLayerCount; ++i) {
            allEnabled = allEnabled && s.layers[i].tint.w == 1.0f;
        }
        check(allEnabled, "terrain: all four splat channels are usable");
    }

    // ---- (12) LOD + スカート (M58e) ----
    // ★このサブの主張は「LOD 境界に隙間が出ない」。絵で見ると 1〜2 画素の筋なので、
    //   目視は当てにならない。ここでは**生成したメッシュそのもの**から縁の折れ線を
    //   取り出して、LOD の違う 2 枚の縁の縦の食い違いがスカート深さ以下であることを測る
    //   (この不等式が成り立てば 2 枚の面の縦区間が必ず重なる = 幾何的に穴が開かない)。
    {
        // 折れ線 (t 昇順) を t で評価する。範囲外は端の値
        auto evalPoly = [](const std::vector<std::pair<float, float>>& pts, float t) {
            if (pts.empty()) {
                return 0.0f;
            }
            if (t <= pts.front().first) {
                return pts.front().second;
            }
            if (t >= pts.back().first) {
                return pts.back().second;
            }
            for (size_t i = 0; i + 1 < pts.size(); ++i) {
                if (t <= pts[i + 1].first) {
                    const float span = pts[i + 1].first - pts[i].first;
                    const float w = span > 0.0f ? (t - pts[i].first) / span : 0.0f;
                    return pts[i].second + (pts[i + 1].second - pts[i].second) * w;
                }
            }
            return pts.back().second;
        };
        // 生成済みメッシュの縁 1 列/1 行を (沿った座標, y) の折れ線として取り出す。
        // **メッシュから取る**のが要点 — ComputeMaxLodEdgeGap の内部式を写して比べると
        // 実装を実装で検算するだけになる
        auto rim = [](const std::vector<MeshVertex>& v, uint32_t vx, uint32_t vz, bool alongZ,
                      bool farSide, std::vector<std::pair<float, float>>& out) {
            out.clear();
            if (alongZ) {
                const uint32_t ix = farSide ? vx - 1 : 0;
                for (uint32_t iz = 0; iz < vz; ++iz) {
                    const MeshVertex& mv = v[static_cast<size_t>(iz) * vx + ix];
                    out.push_back({ mv.position.z, mv.position.y });
                }
            } else {
                const uint32_t iz = farSide ? vz - 1 : 0;
                for (uint32_t ix = 0; ix < vx; ++ix) {
                    const MeshVertex& mv = v[static_cast<size_t>(iz) * vx + ix];
                    out.push_back({ mv.position.x, mv.position.y });
                }
            }
        };
        // 格子の頂点数 (スカートを除いた本体ぶん)
        auto gridSize = [](const TerrainChunk& c, uint32_t lod, uint32_t& vx, uint32_t& vz) {
            std::vector<uint32_t> s;
            TerrainLodSamples(c.tileX0, c.tilesX, TerrainLodStride(lod), s);
            vx = static_cast<uint32_t>(s.size());
            TerrainLodSamples(c.tileZ0, c.tilesZ, TerrainLodStride(lod), s);
            vz = static_cast<uint32_t>(s.size());
        };

        // (12a) LOD 選択 — 純関数。lodDistance == 0 は「無効」であって「最遠」ではない
        check(SelectTerrainLod(0.0f, 0.0f) == 0 && SelectTerrainLod(1e9f, 0.0f) == 0,
              "terrain lod: lodDistance 0 disables LOD entirely (never picks a coarse level)");
        check(SelectTerrainLod(79.0f, 80.0f) == 0 && SelectTerrainLod(80.0f, 80.0f) == 1
                  && SelectTerrainLod(159.0f, 80.0f) == 1 && SelectTerrainLod(160.0f, 80.0f) == 2,
              "terrain lod: the switch distance doubles with each level");
        check(SelectTerrainLod(1e9f, 80.0f) == kTerrainLodCount - 1,
              "terrain lod: the level is clamped to the last built LOD");
        check(SelectTerrainLod(-50.0f, 80.0f) == 0
                  && SelectTerrainLod(std::nanf(""), 80.0f) == 0,
              "terrain lod: a chunk behind the camera (or NaN depth) falls back to LOD 0");

        // (12b) 刻み — 末尾がチャンクの端でないと同 LOD の隣接チャンクすら縁を共有できない
        {
            std::vector<uint32_t> s;
            TerrainLodSamples(0, 8, 1, s);
            check(s.size() == 9 && s.front() == 0 && s.back() == 8,
                  "terrain lod: stride 1 samples every texel (LOD 0 is the full grid)");
            TerrainLodSamples(8, 8, 4, s);
            check(s.size() == 3 && s[0] == 8 && s[1] == 12 && s[2] == 16,
                  "terrain lod: a stride that divides the chunk gives an even sample list");
            TerrainLodSamples(0, 5, 4, s);
            check(s.size() == 3 && s[0] == 0 && s[1] == 4 && s[2] == 5,
                  "terrain lod: a remainder chunk keeps its exact end texel (short last cell)");
            TerrainLodSamples(0, 2, 8, s);
            check(s.size() == 2 && s[0] == 0 && s[1] == 2,
                  "terrain lod: a chunk smaller than the stride collapses to a single cell");
        }

        const TerrainAsset::TerrainData d = MakeBumpyTerrain(); // tiles 16 x 16 (非線形)
        TerrainChunkLayout layout;
        BuildChunkLayout(d, 4, layout); // 4 x 4 = 16 チャンク (両方向に内側の縁がある)
        const float rawGap = ComputeMaxLodEdgeGap(d, layout, kTerrainLodCount);
        const float skirt = rawGap * kTerrainSkirtMargin;

        // (12c) 段差の計測そのもの
        check(rawGap > 0.0f, "terrain lod: a bumpy heightfield reports a non-zero LOD edge gap");
        check(ComputeMaxLodEdgeGap(d, layout, 1) == 0.0f,
              "terrain lod: with a single LOD there is no edge gap to close");
        {
            TerrainAsset::TerrainData flat = MakeBumpyTerrain();
            std::fill(flat.heights.begin(), flat.heights.end(), static_cast<uint16_t>(12345));
            TerrainChunkLayout fl;
            BuildChunkLayout(flat, 4, fl);
            check(ComputeMaxLodEdgeGap(flat, fl, kTerrainLodCount) == 0.0f,
                  "terrain lod: a flat heightfield needs no skirt at all (gap is exactly 0)");
        }

        // (12d) LOD メッシュの形 — 頂点が減り、index は自分の頂点だけを指す
        {
            const TerrainChunk& c = *layout.At(1, 0); // 内側 (4 辺すべてに隣が居るとは限らない)
            std::vector<MeshVertex> v0, v2;
            std::vector<uint32_t> i0, i2;
            BuildChunkMesh(d, c, v0, i0, 0, 0.0f);
            BuildChunkMesh(d, c, v2, i2, 2, 0.0f);
            uint32_t vx2 = 0, vz2 = 0;
            gridSize(c, 2, vx2, vz2);
            check(v2.size() == static_cast<size_t>(vx2) * vz2 && v2.size() < v0.size(),
                  "terrain lod: a coarser LOD builds a smaller vertex grid");
            bool inRange = true;
            for (uint32_t i : i2) {
                inRange = inRange && i < v2.size();
            }
            // ★index を chunk.tilesX で回すと LOD で間引いた分だけ頂点数を飛び越える
            check(inRange && !i2.empty(),
                  "terrain lod: coarse indices address the coarse grid (not the LOD 0 counts)");
            // 4 隅は LOD に依らず同じ texel = ビット一致 (縁の共有の土台)
            check(std::memcmp(&v0[0].position, &v2[0].position, sizeof(XMFLOAT3)) == 0
                      && std::memcmp(&v0[v0.size() - 1].position, &v2[v2.size() - 1].position,
                                     sizeof(XMFLOAT3)) == 0,
                  "terrain lod: the chunk corners are identical at every LOD");
        }

        // (12e) ★本命: LOD 境界の食い違い <= スカート深さ (= 隙間が幾何的に塞がる)
        {
            float worstSeen = 0.0f;
            bool covered = true;
            std::vector<MeshVertex> va, vb;
            std::vector<uint32_t> ia, ib;
            std::vector<std::pair<float, float>> pa, pb;
            // alongZ = x 方向に隣り合う 2 枚 (縁は z に沿う) / false = z 方向に隣り合う 2 枚。
            // **両方向を回す** — 片方だけだと「行と列を取り違えた」実装が緑のまま通る
            auto sweepPair = [&](const TerrainChunk& a, const TerrainChunk& b, bool alongZ) {
                for (uint32_t la = 0; la < kTerrainLodCount; ++la) {
                    for (uint32_t lb = 0; lb < kTerrainLodCount; ++lb) {
                        BuildChunkMesh(d, a, va, ia, la, skirt);
                        BuildChunkMesh(d, b, vb, ib, lb, skirt);
                        uint32_t ax = 0, az = 0, bx = 0, bz = 0;
                        gridSize(a, la, ax, az);
                        gridSize(b, lb, bx, bz);
                        rim(va, ax, az, alongZ, true, pa);  // a の +X / +Z 縁
                        rim(vb, bx, bz, alongZ, false, pb); // b の -X / -Z 縁
                        // 縁を 64 点で舐める (制御点の位置が LOD でずれるので、格子ではなく
                        // 連続なパラメータで比べないと最悪点を跨いでしまう)
                        for (int k = 0; k <= 64; ++k) {
                            const float t = pa.front().first
                                + (pa.back().first - pa.front().first)
                                    * (static_cast<float>(k) / 64.0f);
                            const float diff = std::fabs(evalPoly(pa, t) - evalPoly(pb, t));
                            worstSeen = std::max(worstSeen, diff);
                            covered = covered && diff <= skirt;
                        }
                    }
                }
            };
            for (uint32_t cz = 0; cz < layout.countZ; ++cz) {
                for (uint32_t cx = 0; cx + 1 < layout.countX; ++cx) {
                    sweepPair(*layout.At(cx, cz), *layout.At(cx + 1, cz), true);
                }
            }
            for (uint32_t cz = 0; cz + 1 < layout.countZ; ++cz) {
                for (uint32_t cx = 0; cx < layout.countX; ++cx) {
                    sweepPair(*layout.At(cx, cz), *layout.At(cx, cz + 1), false);
                }
            }
            // ★1 m 以上の食い違いを要求する。「> 0」だと、地形の高さが縁に沿って 1 次式に
            //   なっている fixture (旧 MakeTestTerrain がまさにそれだった) で丸め誤差だけの
            //   1e-5 が通ってしまい、クラック検査が何も検査しないまま緑になる
            check(worstSeen > 1.0f,
                  "terrain lod: mixed-LOD neighbours really do disagree along the shared edge");
            // ★これが「クラックが出ない」の機械証明。スカートは勘の値ではなく、
            //   ComputeMaxLodEdgeGap が測った最悪段差から導いている
            check(covered,
                  "terrain lod: the auto skirt is deeper than every LOD edge mismatch (no cracks)");
            MYE_LOG_INFO("  terrain lod: worst edge mismatch %.4f m, auto skirt %.4f m",
                         static_cast<double>(worstSeen), static_cast<double>(skirt));
        }

        // (12f) スカートの形 — 外周には出さない / 深さはちょうど skirt / index は範囲内
        {
            const TerrainChunk& corner = *layout.At(0, 0); // 隣は +X と +Z の 2 辺だけ
            const TerrainChunk& mid = *layout.At(1, 0);    // -X / +X / +Z の 3 辺
            std::vector<MeshVertex> v;
            std::vector<uint32_t> idx;
            uint32_t vx = 0, vz = 0;

            BuildChunkMesh(d, corner, v, idx, 0, skirt);
            gridSize(corner, 0, vx, vz);
            const size_t grid = static_cast<size_t>(vx) * vz;
            check(v.size() == grid + vz + vx,
                  "terrain lod: a corner chunk skirts only its two interior edges");

            BuildChunkMesh(d, mid, v, idx, 0, skirt);
            gridSize(mid, 0, vx, vz);
            const size_t gridMid = static_cast<size_t>(vx) * vz;
            check(v.size() == gridMid + 2 * vz + vx,
                  "terrain lod: an edge chunk skirts three edges (the outer one stays open)");

            bool depthOk = true;
            bool idxOk = true;
            for (size_t i = gridMid; i < v.size(); ++i) {
                // スカート頂点は必ず「格子のどこかの頂点のちょうど skirt 下」
                bool found = false;
                for (size_t g = 0; g < gridMid && !found; ++g) {
                    found = v[i].position.x == v[g].position.x
                        && v[i].position.z == v[g].position.z
                        && std::fabs((v[g].position.y - v[i].position.y) - skirt) < 1e-3f;
                }
                depthOk = depthOk && found;
            }
            for (uint32_t i : idx) {
                idxOk = idxOk && i < v.size();
            }
            check(depthOk, "terrain lod: every skirt vertex hangs exactly the skirt depth below");
            check(idxOk, "terrain lod: skirt indices stay inside the chunk's vertex buffer");

            // 単一チャンクの地形 = 割れる縁が無い = スカートを 1 枚も出さない
            TerrainChunkLayout single;
            BuildChunkLayout(d, kTerrainMaxChunkTiles, single);
            check(single.chunks.size() == 1, "terrain lod: the test terrain fits one chunk");
            BuildChunkMesh(d, single.chunks[0], v, idx, 0, skirt);
            uint32_t sx = 0, sz = 0;
            gridSize(single.chunks[0], 0, sx, sz);
            check(v.size() == static_cast<size_t>(sx) * sz,
                  "terrain lod: a single-chunk terrain grows no skirt (nothing can crack)");

            // スカート無し (skirtDepth <= 0) は M58d までとビット一致のメッシュ
            std::vector<MeshVertex> plain;
            std::vector<uint32_t> plainIdx;
            BuildChunkMesh(d, mid, plain, plainIdx, 0, 0.0f);
            std::vector<MeshVertex> legacy;
            std::vector<uint32_t> legacyIdx;
            BuildChunkMesh(d, mid, legacy, legacyIdx);
            check(plain.size() == legacy.size() && plainIdx == legacyIdx
                      && std::memcmp(plain.data(), legacy.data(),
                                     plain.size() * sizeof(MeshVertex)) == 0,
                  "terrain lod: LOD 0 without a skirt is bit-identical to the pre-LOD mesh");
        }

        // (12g) スカートの表はチャンクの外向き。裏返っていると裏面カリングで消え、
        //       絵の上では「隙間が塞がっていない」のと区別が付かない
        {
            const TerrainChunk& c = *layout.At(1, 0);
            std::vector<MeshVertex> v;
            std::vector<uint32_t> idx;
            BuildChunkMesh(d, c, v, idx, 1, skirt);
            uint32_t vx = 0, vz = 0;
            gridSize(c, 1, vx, vz);
            const size_t grid = static_cast<size_t>(vx) * vz;
            const float ccx = (c.localMin.x + c.localMax.x) * 0.5f;
            const float ccz = (c.localMin.z + c.localMax.z) * 0.5f;
            bool outward = true;
            int skirtTris = 0;
            for (size_t t = 0; t + 2 < idx.size(); t += 3) {
                if (idx[t] < grid && idx[t + 1] < grid && idx[t + 2] < grid) {
                    continue; // 地表の三角
                }
                ++skirtTris;
                const XMFLOAT3& p0 = v[idx[t]].position;
                const XMFLOAT3& p1 = v[idx[t + 1]].position;
                const XMFLOAT3& p2 = v[idx[t + 2]].position;
                const XMFLOAT3 e1 = { p1.x - p0.x, p1.y - p0.y, p1.z - p0.z };
                const XMFLOAT3 e2 = { p2.x - p0.x, p2.y - p0.y, p2.z - p0.z };
                const float nx = e1.y * e2.z - e1.z * e2.y;
                const float nz = e1.x * e2.y - e1.y * e2.x;
                const float cxm = (p0.x + p1.x + p2.x) / 3.0f - ccx;
                const float czm = (p0.z + p1.z + p2.z) / 3.0f - ccz;
                outward = outward && (nx * cxm + nz * czm) > 0.0f;
            }
            check(skirtTris > 0 && outward,
                  "terrain lod: skirt triangles face away from the chunk (visible from next door)");
        }

        // (12h) AABB はスカートの底まで含む (含まないとカリングが見えているチャンクを落とす)
        {
            TerrainChunkLayout expanded;
            BuildChunkLayout(d, 4, expanded);
            const float before = expanded.chunks[0].localMin.y;
            ExpandLayoutForSkirt(expanded, kTerrainLodCount, skirt);
            check(expanded.skirtDepth == skirt && expanded.lodCount == kTerrainLodCount
                      && std::fabs((before - expanded.chunks[0].localMin.y) - skirt) < 1e-4f,
                  "terrain lod: the layout records the skirt and drops every AABB floor by it");

            std::vector<MeshVertex> v;
            std::vector<uint32_t> idx;
            bool inAabb = true;
            for (const TerrainChunk& c : expanded.chunks) {
                for (uint32_t lod = 0; lod < kTerrainLodCount; ++lod) {
                    BuildChunkMesh(d, c, v, idx, lod, skirt);
                    for (const MeshVertex& mv : v) {
                        inAabb = inAabb && mv.position.x >= c.localMin.x
                            && mv.position.x <= c.localMax.x && mv.position.y >= c.localMin.y
                            && mv.position.y <= c.localMax.y && mv.position.z >= c.localMin.z
                            && mv.position.z <= c.localMax.z;
                    }
                }
            }
            check(inAabb, "terrain lod: the expanded AABB contains every LOD and skirt vertex");

            // LOD 無効ならスカートは出ない = AABB も動かない (既定 = 従来どおり)
            TerrainChunkLayout off;
            BuildChunkLayout(d, 4, off);
            const float floorBefore = off.chunks[0].localMin.y;
            ExpandLayoutForSkirt(off, 1, skirt);
            check(off.skirtDepth == 0.0f && off.chunks[0].localMin.y == floorBefore,
                  "terrain lod: with LOD off the skirt is suppressed and the AABB is untouched");
        }

        // (12i) 決定論 (LOD + スカートを 2 回組んでバイト一致)
        {
            std::vector<MeshVertex> a, b;
            std::vector<uint32_t> ia, ib;
            BuildChunkMesh(d, layout.chunks[1], a, ia, 2, skirt);
            BuildChunkMesh(d, layout.chunks[1], b, ib, 2, skirt);
            check(ia == ib && a.size() == b.size()
                      && std::memcmp(a.data(), b.data(), a.size() * sizeof(MeshVertex)) == 0,
                  "terrain lod: rebuilding a LOD chunk with a skirt yields identical bytes");
        }
    }

    // ---- (13) TerrainSystem 経由の LOD 選択 (M58e) ----
    {
        std::error_code ec;
        const fs::path root = fs::temp_directory_path(ec) / L"mye_terrain_lod_selftest";
        fs::remove_all(root, ec);
        fs::create_directories(root / L"terrain", ec);
        const std::string src =
            R"({"type":"terrain","version":1,"worldSize":[256.0,256.0],)"
            R"("heightRes":[65,65],"splatRes":[8,8],"heightBase":0.0,"heightScale":24.0,)"
            R"("procedural":{"seed":7,"octaves":4,"frequency":3.0,"lacunarity":2.0,"gain":0.5},)"
            R"("layers":[{"name":"a"}]})";
        {
            std::ofstream f(root / L"terrain" / L"t.terrain.json",
                            std::ios::binary | std::ios::trunc);
            f.write(src.data(), static_cast<std::streamsize>(src.size()));
        }

        World world;
        MeshLibrary meshes;
        TextureLibrary textures;
        TerrainSystem terrain;
        const EntityID e = world.CreateEntity("terrain");
        auto* tc = world.AddComponent<TerrainComponent>(e);
        std::snprintf(tc->source, sizeof(tc->source), "terrain/t.terrain.json");
        tc->chunkTiles = 16; // 64 タイル / 16 = 4 x 4 = 16 チャンク

        // 手前から奥まで一望する俯瞰カメラ (チャンクごとに viewZ が大きく違う配置)
        const Frustum wide =
            MakeFrustum({ 0, 120, -260 }, { 0, 0, 0 }, { 0, 1, 0 }, 70.0f, 0.1f, 2000.0f);
        XMFLOAT4X4 view;
        DirectX::XMStoreFloat4x4(&view,
                                 DirectX::XMMatrixLookAtLH(DirectX::XMVectorSet(0, 120, -260, 1),
                                                           DirectX::XMVectorSet(0, 0, 0, 1),
                                                           DirectX::XMVectorSet(0, 1, 0, 0)));
        std::vector<TerrainDrawItem> items;

        // LOD 無効 (既定) = 全チャンク LOD 0
        terrain.Collect(world, meshes, textures, root.wstring(), wide, view, items);
        bool allLod0 = !items.empty();
        float minZ = 1e30f, maxZ = -1e30f;
        for (const TerrainDrawItem& it : items) {
            allLod0 = allLod0 && it.lod == 0;
            minZ = std::min(minZ, it.viewZ);
            maxZ = std::max(maxZ, it.viewZ);
        }
        check(allLod0 && terrain.LastLodCount(0) == items.size(),
              "terrain lod: the default component (lodDistance 0) keeps every chunk at LOD 0");
        const size_t baseCount = items.size();
        const AssetID lod0Mesh = items.empty() ? AssetID{} : items[0].mesh;

        // 手前と奥の中間に切替距離を置くと LOD が割れる。**チャンクごとの viewZ を
        // 使って距離を決める** — 定数を焼くとカメラを触った日に無言で意味を失う
        tc = world.GetComponent<TerrainComponent>(e);
        tc->lodDistance = (minZ + maxZ) * 0.5f;
        terrain.Collect(world, meshes, textures, root.wstring(), wide, view, items);
        uint32_t lodBits = 0;
        for (const TerrainDrawItem& it : items) {
            lodBits |= 1u << it.lod;
        }
        // 可視数は「同じ」ではなく「減らない」— スカートぶん AABB が下に伸びるので、
        // 縁を掠めていたチャンクが増える方向にだけ動きうる
        check(items.size() >= baseCount && (lodBits & 1u) != 0 && lodBits != 1u,
              "terrain lod: a mid switch distance splits the visible chunks across LOD levels");
        check(terrain.LastLodCount(0) + terrain.LastLodCount(1) + terrain.LastLodCount(2)
                  == items.size(),
              "terrain lod: the per-level counters add up to the visible chunk count");
        check(terrain.CacheSize() == 2,
              "terrain lod: enabling LOD builds a separate cached instance (params are the key)");

        // 極端に短い切替距離 = 全チャンクが最粗 LOD。三角数が LOD 0 より確実に減っていること
        tc->lodDistance = 0.001f;
        terrain.Collect(world, meshes, textures, root.wstring(), wide, view, items);
        bool allCoarse = !items.empty();
        for (const TerrainDrawItem& it : items) {
            allCoarse = allCoarse && it.lod == kTerrainLodCount - 1;
        }
        const Mesh* coarse = items.empty() ? nullptr : meshes.Get(items[0].mesh);
        const Mesh* fine = meshes.Get(lod0Mesh);
        check(allCoarse && coarse != nullptr && fine != nullptr
                  && coarse->indexCount < fine->indexCount,
              "terrain lod: the coarsest level really draws fewer triangles than LOD 0");

        // スカート無し (負値) はメッシュを別物にする = キャッシュのキーに効いている
        tc->skirtDepth = -1.0f;
        const size_t cacheBefore = terrain.CacheSize();
        terrain.Collect(world, meshes, textures, root.wstring(), wide, view, items);
        check(terrain.CacheSize() == cacheBefore + 1,
              "terrain lod: the skirt setting is part of the cache key (meshes are rebuilt)");

        fs::remove_all(root, ec);
    }

    // ---- (14) ブラシ編集 + Undo/Redo (M58f) ----
    {
        const TerrainAsset::TerrainData base = MakeBrushTerrain();

        // (a) 上げ: 中心が上がり、**半径の外は 1 バイトも動かない**
        {
            TerrainAsset::TerrainData d = base;
            TerrainEdit::Brush b;
            b.mode = TerrainEdit::BrushMode::Raise;
            b.radius = 8.0f;
            b.strength = 2.0f;
            check(TerrainEdit::ApplyBrush(d, b), "terrain brush: raise reports that it changed the map");
            const uint32_t cx = (d.heightW - 1) / 2;
            const uint32_t cz = (d.heightH - 1) / 2;
            check(d.HeightAtTexel(cx, cz) > base.HeightAtTexel(cx, cz),
                  "terrain brush: raise lifts the texel under the brush centre");
            bool outsideIntact = true;
            for (uint32_t z = 0; z < d.heightH; ++z) {
                for (uint32_t x = 0; x < d.heightW; ++x) {
                    const float lx = TexelLocalX(d, x) - b.centerX;
                    const float lz = TexelLocalZ(d, z) - b.centerZ;
                    if (std::sqrt(lx * lx + lz * lz) <= b.radius) {
                        continue;
                    }
                    const size_t i = static_cast<size_t>(z) * d.heightW + x;
                    outsideIntact = outsideIntact && d.heights[i] == base.heights[i];
                }
            }
            check(outsideIntact, "terrain brush: nothing outside the brush radius is touched");
            check(d.splat == base.splat, "terrain brush: raise leaves the splat map untouched");

            // 強さ 0 / 半径 0 は「変化なし」を返すだけで、地形に触らない
            TerrainEdit::Brush zero = b;
            zero.strength = 0.0f;
            TerrainAsset::TerrainData z0 = base;
            const bool zeroChanged = TerrainEdit::ApplyBrush(z0, zero);
            zero.strength = 2.0f;
            zero.radius = 0.0f;
            TerrainAsset::TerrainData z1 = base;
            const bool zeroRadius = TerrainEdit::ApplyBrush(z1, zero);
            check(!zeroChanged && !zeroRadius && z0.heights == base.heights
                      && z1.heights == base.heights,
                  "terrain brush: a zero strength / zero radius dab is a true no-op");
        }

        // (b) 決定論: 同じ地形に同じブラシ → バイト一致
        {
            TerrainEdit::Brush b;
            b.mode = TerrainEdit::BrushMode::Raise;
            b.centerX = -7.5f;
            b.centerZ = 11.25f;
            b.radius = 13.0f;
            b.strength = -1.75f; // 掘る側も通す
            TerrainAsset::TerrainData a = base;
            TerrainAsset::TerrainData c = base;
            TerrainEdit::ApplyBrush(a, b);
            TerrainEdit::ApplyBrush(c, b);
            check(a.heights == c.heights,
                  "terrain brush: the same dab on the same terrain yields identical bytes");
        }

        // (c) 平滑化: 粗さが減る + **走査順に依存しない**
        {
            TerrainEdit::Brush b;
            b.mode = TerrainEdit::BrushMode::Smooth;
            b.radius = 40.0f; // 地形全体を覆う
            b.strength = 1.0f;
            TerrainAsset::TerrainData d = base;
            check(TerrainEdit::ApplyBrush(d, b) && Roughness(d) < Roughness(base),
                  "terrain brush: smoothing really flattens the neighbour-to-neighbour steps");

            // ★「近傍平均を書き込み前の高さから読む」の実証。ブラシは centerX=0 について
            //   左右対称なので、**鏡映してから平滑化して鏡映し戻す**と元と一致するはず。
            //   自分の書き込みを読み返す実装だと、x 昇順の走査が鏡映で別の順序になり
            //   ここで割れる (「決定論だが順序依存」を素通りさせないための検査)
            const TerrainAsset::TerrainData mirrored = MirrorX(base);
            TerrainAsset::TerrainData m = mirrored;
            TerrainEdit::ApplyBrush(m, b);
            check(MirrorX(m).heights == d.heights,
                  "terrain brush: smoothing reads the pre-dab heights (mirroring the input mirrors "
                  "the output exactly)");
        }

        // (d) 塗り: 合計 255 の不変量を保ち、対象レイヤが増え、半径の外は不変
        {
            TerrainEdit::Brush b;
            b.mode = TerrainEdit::BrushMode::Paint;
            b.radius = 10.0f;
            b.strength = 0.75f;
            b.layer = 2;
            TerrainAsset::TerrainData d = base;
            check(TerrainEdit::ApplyBrush(d, b), "terrain brush: paint reports that it changed the map");
            bool sums = true;
            bool outside = true;
            for (uint32_t z = 0; z < d.splatH; ++z) {
                for (uint32_t x = 0; x < d.splatW; ++x) {
                    const size_t i = (static_cast<size_t>(z) * d.splatW + x) * 4;
                    const uint32_t total = static_cast<uint32_t>(d.splat[i]) + d.splat[i + 1]
                        + d.splat[i + 2] + d.splat[i + 3];
                    sums = sums && total == TerrainAsset::kSplatWeightSum;
                    const float lx =
                        ((static_cast<float>(x) + 0.5f) / d.splatW - 0.5f) * d.worldSizeX - b.centerX;
                    const float lz =
                        ((static_cast<float>(z) + 0.5f) / d.splatH - 0.5f) * d.worldSizeZ - b.centerZ;
                    if (std::sqrt(lx * lx + lz * lz) > b.radius) {
                        outside = outside && std::memcmp(&d.splat[i], &base.splat[i], 4) == 0;
                    }
                }
            }
            check(sums, "terrain brush: every painted texel still sums to kSplatWeightSum");
            check(outside, "terrain brush: paint leaves the splat outside the radius untouched");
            const size_t centre =
                (static_cast<size_t>(d.splatH / 2) * d.splatW + d.splatW / 2) * 4;
            check(d.splat[centre + b.layer] > base.splat[centre + b.layer],
                  "terrain brush: paint raises the weight of the target layer under the centre");
            check(d.heights == base.heights, "terrain brush: paint leaves the heightmap untouched");
            TerrainEdit::Brush bad = b;
            bad.layer = TerrainAsset::kMaxLayers; // 存在しないレイヤ
            TerrainAsset::TerrainData d2 = base;
            check(!TerrainEdit::ApplyBrush(d2, bad) && d2.splat == base.splat,
                  "terrain brush: painting a layer index past the end is a no-op");
        }

        // (e) ★本命: 1 ストローク (重なる 5 ダブ) の Undo/Redo でバイト一致
        TerrainAsset::TerrainData work = base;
        TerrainEdit::TerrainPatch patch;
        {
            struct Step {
                TerrainEdit::BrushMode mode;
                float cx, cz, radius, strength;
                uint32_t layer;
            };
            const Step steps[] = {
                { TerrainEdit::BrushMode::Raise, -10.0f, -10.0f, 12.0f, 3.0f, 0 },
                { TerrainEdit::BrushMode::Raise, 6.0f, -4.0f, 10.0f, -2.0f, 0 }, // 掘る (重ねる)
                { TerrainEdit::BrushMode::Smooth, 0.0f, 0.0f, 16.0f, 1.0f, 0 },
                { TerrainEdit::BrushMode::Paint, 4.0f, 8.0f, 14.0f, 0.8f, 2 },
                { TerrainEdit::BrushMode::Paint, -6.0f, 2.0f, 9.0f, 0.5f, 1 }, // 重ね塗り
            };
            bool anyChange = false;
            for (const Step& s : steps) {
                TerrainEdit::Brush b;
                b.mode = s.mode;
                b.centerX = s.cx;
                b.centerZ = s.cz;
                b.radius = s.radius;
                b.strength = s.strength;
                b.layer = s.layer;
                anyChange = TerrainEdit::ApplyBrush(work, b) || anyChange;
            }
            check(anyChange && work.heights != base.heights && work.splat != base.splat,
                  "terrain brush: a 5-dab stroke moves both the heightmap and the splat map");

            check(TerrainEdit::MakeDiffPatch(base, work, patch) && !patch.Empty(),
                  "terrain brush: the stroke produces a non-empty undo patch");

            TerrainAsset::TerrainData undone = work;
            check(TerrainEdit::ApplyPatch(undone, patch, /*redo=*/false)
                      && undone.heights == base.heights && undone.splat == base.splat,
                  "terrain brush: UNDO restores the heightmap and splat map byte for byte");
            TerrainAsset::TerrainData redone = undone;
            check(TerrainEdit::ApplyPatch(redone, patch, /*redo=*/true)
                      && redone.heights == work.heights && redone.splat == work.splat,
                  "terrain brush: REDO reproduces the painted result byte for byte");
            for (int i = 0; i < 3; ++i) {
                TerrainEdit::ApplyPatch(redone, patch, false);
                TerrainEdit::ApplyPatch(redone, patch, true);
            }
            check(redone.heights == work.heights && redone.splat == work.splat,
                  "terrain brush: repeated undo/redo cycles do not drift a single byte");
        }

        // (f) パッチの直列化往復 (Undo エントリはバイト列として持ち回る)
        std::string patchBytes;
        {
            TerrainEdit::SerializePatch(patch, patchBytes);
            TerrainEdit::TerrainPatch back;
            check(TerrainEdit::DeserializePatch(patchBytes, back),
                  "terrain brush: the undo patch deserializes");
            std::string again;
            TerrainEdit::SerializePatch(back, again);
            check(patchBytes == again,
                  "terrain brush: the undo patch survives a byte-exact serialize round trip");
            TerrainAsset::TerrainData r = work;
            check(TerrainEdit::ApplyPatch(r, back, false) && r.heights == base.heights
                      && r.splat == base.splat,
                  "terrain brush: the deserialized patch undoes exactly like the original");
        }

        // (g) 壊れた入力 / 合わない地形では**何も書かない**
        {
            TerrainEdit::TerrainPatch bad;
            check(!TerrainEdit::DeserializePatch(patchBytes.substr(0, patchBytes.size() / 2), bad),
                  "terrain brush: a truncated patch is rejected instead of read past the end");
            check(!TerrainEdit::DeserializePatch(patchBytes + std::string(1, 'x'), bad),
                  "terrain brush: a patch with trailing junk is rejected");
            // ★ローカル変数に `small` は使えない (rpcndr.h の `#define small char` で潰れる。
            //   節 (12) の near/far と同じ罠)
            TerrainAsset::TerrainData tinyTerrain = MakeBumpyTerrain(5, 5);
            const TerrainAsset::TerrainData tinyCopy = tinyTerrain;
            check(!TerrainEdit::ApplyPatch(tinyTerrain, patch, false)
                      && tinyTerrain.heights == tinyCopy.heights
                      && tinyTerrain.splat == tinyCopy.splat,
                  "terrain brush: a patch that does not fit the terrain writes zero bytes");
            check(!TerrainEdit::MakeDiffPatch(base, tinyTerrain, bad),
                  "terrain brush: diffing two different resolutions fails instead of guessing");
        }

        // (h) パッチは**触った矩形だけ** (Undo エントリの大きさが地形の大きさに比例しない
        //     ことが、全量スナップショットではなく矩形を選んだ理由そのもの)
        {
            TerrainEdit::Brush b;
            b.mode = TerrainEdit::BrushMode::Raise;
            b.radius = 4.0f;
            b.strength = 1.0f;
            TerrainAsset::TerrainData one = base;
            TerrainEdit::ApplyBrush(one, b);
            TerrainEdit::TerrainPatch tiny;
            check(TerrainEdit::MakeDiffPatch(base, one, tiny) && tiny.hw > 0 && tiny.hw <= 7
                      && tiny.hh <= 7 && tiny.sw == 0,
                  "terrain brush: the undo patch covers only the rectangle the dab touched");
        }

        // (i) サイドカーの往復 / 決定論 / 拒否
        {
            std::vector<uint8_t> blob;
            TerrainEdit::SerializeSidecar(work, blob);
            std::vector<uint8_t> blob2;
            TerrainEdit::SerializeSidecar(work, blob2);
            check(blob == blob2,
                  "terrain edit: serializing the same pixels twice yields identical bytes");
            TerrainAsset::TerrainData recv = base;
            check(TerrainEdit::ApplySidecarBlob(blob, recv) && recv.heights == work.heights
                      && recv.splat == work.splat,
                  "terrain edit: the sidecar carries the edited pixels byte for byte");
            TerrainAsset::TerrainData other = MakeBumpyTerrain(9, 9);
            const TerrainAsset::TerrainData otherCopy = other;
            check(!TerrainEdit::ApplySidecarBlob(blob, other)
                      && other.heights == otherCopy.heights,
                  "terrain edit: a sidecar whose resolution disagrees is ignored (terrain intact)");
            const std::vector<uint8_t> cut(blob.begin(), blob.begin() + blob.size() / 2);
            TerrainAsset::TerrainData t = base;
            check(!TerrainEdit::ApplySidecarBlob(cut, t) && t.heights == base.heights,
                  "terrain edit: a truncated sidecar is rejected without touching the terrain");
        }

        // (j) パス変換
        {
            check(TerrainEdit::EditPathFor(L"C:\\a\\b.terrain.json") == L"C:\\a\\b.terrain.edit",
                  "terrain edit: the sidecar path is the source with .json swapped for .edit");
            check(TerrainEdit::EditPathFor(L"C:\\a\\b.json").empty()
                      && TerrainEdit::EditPathFor(L"").empty(),
                  "terrain edit: a non-terrain path has no sidecar");
        }
    }

    // ---- (15) クック経由の実地: サイドカーとクックキャッシュ (M58f) ----
    {
        std::error_code ec;
        const fs::path root = fs::temp_directory_path(ec) / L"mye_terrain_brush_selftest";
        fs::remove_all(root, ec);
        fs::create_directories(root / L"cooked", ec);
        const fs::path src = root / L"t.terrain.json";
        const std::string json =
            R"({"type":"terrain","version":1,"worldSize":[64.0,64.0],)"
            R"("heightRes":[17,17],"splatRes":[8,8],"heightBase":0.0,"heightScale":24.0,)"
            R"("procedural":{"seed":11,"octaves":3,"frequency":3.0,"lacunarity":2.0,"gain":0.5},)"
            R"("layers":[{"name":"a"},{"name":"b"}]})";
        {
            std::ofstream f(src, std::ios::binary | std::ios::trunc);
            f.write(json.data(), static_cast<std::streamsize>(json.size()));
        }
        const std::wstring srcPath = src.wstring();
        const std::wstring editPath = TerrainEdit::EditPathFor(srcPath);

        TerrainEdit::Brush dab;
        dab.mode = TerrainEdit::BrushMode::Raise;
        dab.radius = 20.0f;
        dab.strength = 5.0f;

        // (a) キャッシュ無効 (既定): SaveEdits → Load でブラシ結果が戻ってくる
        TerrainAsset::TerrainData plain;
        check(TerrainAsset::Load(srcPath, plain), "terrain edit: the fixture terrain cooks");
        TerrainAsset::TerrainData edited = plain;
        check(TerrainEdit::ApplyBrush(edited, dab) && TerrainEdit::SaveEdits(srcPath, edited),
              "terrain edit: SaveEdits writes the sidecar");
        {
            TerrainAsset::TerrainData reloaded;
            check(TerrainAsset::Load(srcPath, reloaded) && reloaded.heights == edited.heights,
                  "terrain edit: a fresh cook picks the brush result up from the sidecar");
            check(!reloaded.editSrc.relPath.empty()
                      && reloaded.editSrc.contentHash == edited.editSrc.contentHash,
                  "terrain edit: the cooked data stamps the sidecar (size + content hash)");
        }

        // (b) クックキャッシュ有効。★**キャッシュを焼いた後にサイドカーが現れた**ケース。
        //     `.terrain.json` は 1 ビットも変わらないので、mtime も deps も miss を出せない —
        //     EditSidecarStillMatches の「ディスク側の存在と blob の主張を突き合わせる」
        //     判定が無いと、ここでブラシ結果が**キャッシュに隠される**
        fs::remove(editPath, ec);
        CookedCache::Configure((root / L"cooked").wstring(), true);
        TerrainAsset::TerrainData cached0;
        check(TerrainAsset::Load(srcPath, cached0) && cached0.heights == plain.heights
                  && cached0.editSrc.relPath.empty(),
              "terrain edit: the un-edited terrain cooks into the cache");
        TerrainAsset::TerrainData behindBack = cached0;
        TerrainEdit::ApplyBrush(behindBack, dab);
        {
            // SaveEdits を通さずに**サイドカーだけ**置く (= よそのプロセス / VCS が置いた)
            std::vector<uint8_t> blob;
            TerrainEdit::SerializeSidecar(behindBack, blob);
            std::ofstream f(fs::path{ editPath }, std::ios::binary | std::ios::trunc);
            f.write(reinterpret_cast<const char*>(blob.data()),
                    static_cast<std::streamsize>(blob.size()));
        }
        TerrainAsset::TerrainData cached1;
        check(TerrainAsset::Load(srcPath, cached1) && cached1.heights == behindBack.heights,
              "terrain edit: the cooked cache does not hide a sidecar that appeared behind its back");
        fs::remove(editPath, ec);
        TerrainAsset::TerrainData cached2;
        check(TerrainAsset::Load(srcPath, cached2) && cached2.heights == plain.heights,
              "terrain edit: deleting the sidecar takes the terrain back to the recipe");
        CookedCache::Configure(L"", false); // 他スイートへ漏らさない (CookedCacheSelfTest と同じ後始末)

        fs::remove_all(root, ec);
    }

    MYE_LOG_INFO("==== Terrain self test: %s ====", failCount == 0 ? "PASS" : "FAIL");
    return failCount == 0;
}

} // namespace mye
