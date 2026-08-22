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
#include "Engine/Engine/Asset/TerrainAsset.h"
#include "Engine/Engine/TerrainSystem.h"
#include "Engine/Renderer/FrustumCull.h"
#include "Engine/Renderer/GpuResources.h"

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
        MeshLibrary meshes; // Init を呼ばない = GPU バッファを作らない CPU 専用モード
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
        const uint32_t total = terrain.Collect(world, meshes, root.wstring(), wide, view, items);
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
        terrain.Collect(world, meshes, root.wstring(), wide, view, items);
        check(terrain.CacheSize() == 1 && !items.empty() && items[0].mesh == firstMesh,
              "terrain: a second Collect reuses the cached chunk meshes");

        // 無効エンティティは収集しない
        world.AddComponent<ActiveComponent>(e)->enabled = 0;
        terrain.Collect(world, meshes, root.wstring(), wide, view, items);
        check(items.empty(), "terrain: an inactive entity contributes no chunks");
        world.GetComponent<ActiveComponent>(e)->enabled = 1;

        // 空パス / 存在しないパスは黙って 0 件 (毎フレーム開き直さないよう失敗もキャッシュ)
        tc = world.GetComponent<TerrainComponent>(e);
        tc->source[0] = '\0';
        terrain.Collect(world, meshes, root.wstring(), wide, view, items);
        check(items.empty(), "terrain: an empty source path is a no-op");
        std::snprintf(tc->source, sizeof(tc->source), "terrain/missing.terrain.json");
        const size_t cacheBefore = terrain.CacheSize();
        terrain.Collect(world, meshes, root.wstring(), wide, view, items);
        terrain.Collect(world, meshes, root.wstring(), wide, view, items);
        check(items.empty() && terrain.CacheSize() == cacheBefore + 1,
              "terrain: a missing asset fails once and is remembered (no per-frame retry)");

        fs::remove_all(root, ec);
    }

    MYE_LOG_INFO("==== Terrain self test: %s ====", failCount == 0 ? "PASS" : "FAIL");
    return failCount == 0;
}

} // namespace mye
