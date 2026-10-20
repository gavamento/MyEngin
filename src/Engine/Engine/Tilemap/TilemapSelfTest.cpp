//====================================================================================
//                          TilemapSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                          タイルパレット / タイルマップ資産と座標変換の回帰テスト
//====================================================================================
#include "Engine/Engine/Tilemap/TilemapSelfTest.h"

#include <array>
#include <DirectXMath.h>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Rendering/Sprite2D/Sprite2DRenderer.h"
#include "Engine/Engine/Tilemap/TilemapAssets.h"
#include "Engine/Engine/Tilemap/TilemapSystem.h"

namespace fs = std::filesystem;

namespace mye {
namespace {

// 負のチャンクを含む 2 レイヤー・3 チャンクのタイルマップ (空チャンクを 1 つ混ぜる)
TilemapAsset MakeSampleMap()
{
    TilemapAsset m;
    m.palette = 0x1234567890ABCDEFull;
    m.cellSize[0] = 0.5f;
    m.cellSize[1] = 2.0f;

    TilemapLayer ground;
    ground.name = "Ground";
    ground.sortingLayerId = 0;
    ground.orderInLayer = -3;
    ground.collision = true;
    ground.SetCell(0, 0, tilecell::Make(1, false, false, 0));
    ground.SetCell(15, 15, tilecell::Make(2, true, false, 1));
    ground.SetCell(16, 0, tilecell::Make(3, false, true, 2, 5));   // チャンク (cx 1, cy 0)
    ground.SetCell(-1, -1, tilecell::Make(65535, true, true, 3));  // チャンク (-1, -1)
    // 空のチャンクを直接差し込む (書き出しで落ちるはず)
    ground.chunks[ChunkKey{ 7, 7 }] = TileChunk{};
    m.layers.push_back(std::move(ground));

    TilemapLayer fore;
    fore.name = "Foreground";
    fore.sortingLayerId = 2;
    fore.orderInLayer = 10;
    fore.collision = false;
    fore.SetCell(3, 4, tilecell::Make(4, false, false, 0));
    m.layers.push_back(std::move(fore));
    return m;
}

size_t JsonChunkCount(const nlohmann::json& j)
{
    size_t n = 0;
    for (const auto& l : j["layers"]) {
        n += l["chunks"].size();
    }
    return n;
}

void WriteText(const fs::path& p, const std::string& text)
{
    std::ofstream f(p, std::ios::binary);
    f << text;
}

} // namespace

bool RunTilemapSelfTest()
{
    MYE_LOG_INFO("==== Tilemap self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    RegisterBuiltinComponents(); // 多重呼び出しは無害
    std::error_code ec;
    const fs::path root = fs::temp_directory_path(ec) / L"mye_tilemap_selftest";
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    // ---- 型の登録 ----
    {
        // TypeId は登録順そのもの。83 (SpriteMask2D) の次
        check(TilemapComponent::sTypeId == 84, "Tilemap is TypeId 84");
        const ComponentDesc& d = ComponentRegistry::Get().Desc(TilemapComponent::sTypeId);
        check((d.flags & kComponentNoHash) == 0, "Tilemap is hashed (sim input)");
    }

    // ---- (a) .tilemap.json の往復と空チャンク ----
    {
        const TilemapAsset original = MakeSampleMap();
        const nlohmann::json first = TilemapLibrary::ToJson(original);
        check(JsonChunkCount(first) == 4, "empty chunk is not written (4 chunks: 3 in Ground + 1 in Foreground)");
        check(first["layers"][0]["chunks"].size() == 3, "Ground has 3 written chunks");
        // チャンクは (cy, cx) 昇順
        const auto& cs = first["layers"][0]["chunks"];
        check(cs[0]["cy"] == -1 && cs[0]["cx"] == -1 && cs[1]["cy"] == 0 && cs[1]["cx"] == 0 && cs[2]["cy"] == 0
                  && cs[2]["cx"] == 1,
              "chunks are written in (cy, cx) ascending order");
        check(cs[1]["cells"].size() == 16 && cs[1]["cells"][0].size() == 16, "chunk cells are 16 rows x 16");

        TilemapAsset loaded;
        check(TilemapLibrary::FromJson(first, loaded), "FromJson accepts a tilemap");
        const nlohmann::json second = TilemapLibrary::ToJson(loaded);
        check(first.dump(2) == second.dump(2), "tilemap JSON round trip is identical");
        check(loaded.layers.size() == 2 && loaded.layers[0].chunks.size() == 3
                  && loaded.layers[0].chunks.count(ChunkKey{ 7, 7 }) == 0,
              "loaded: empty chunk is dropped, 3 chunks in Ground");
        check(loaded.palette == original.palette && loaded.cellSize[0] == 0.5f && loaded.cellSize[1] == 2.0f,
              "palette id and cell size survive");
        check(loaded.layers[0].GetCell(15, 15) == tilecell::Make(2, true, false, 1)
                  && loaded.layers[0].GetCell(-1, -1) == tilecell::Make(65535, true, true, 3)
                  && loaded.layers[0].GetCell(16, 0) == tilecell::Make(3, false, true, 2, 5),
              "cell bits (id / flip / rotation / damage) survive");

        // ファイル経由 (Register の GUID キー) でも同じ
        TilemapLibrary lib;
        const fs::path file = root / L"level.tilemap.json";
        WriteText(file, first.dump(2));
        const uint64_t hash = lib.LoadFromFile(file.wstring());
        const TilemapAsset* got = lib.Get(hash);
        check(hash != 0 && got != nullptr && TilemapLibrary::ToJson(*got).dump(2) == first.dump(2),
              "LoadFromFile registers an asset with the same JSON");
        check(got != nullptr && got->name == "level", "name comes from the file name");
        const uint64_t epoch = lib.Epoch();
        lib.LoadFromFile(file.wstring());
        check(lib.Epoch() > epoch, "re-registering bumps the epoch (render caches rebuild)");

        // 種別キーの無い JSON は別の資産として読まない
        TilemapAsset rejected;
        check(!TilemapLibrary::FromJson(nlohmann::json::parse("{\"foo\":1}"), rejected),
              "non-tilemap JSON is rejected");

        // レイヤー数の上限
        TilemapAsset many;
        many.layers.resize(kMaxTilemapLayers + 3);
        TilemapLibrary::Sanitize(many);
        check(many.layers.size() == kMaxTilemapLayers, "layer count is capped at 8");

        // SetCell(0) で空になったチャンクは消える
        TilemapLayer layer;
        layer.SetCell(5, 5, 9);
        check(layer.chunks.size() == 1, "SetCell creates a chunk");
        layer.SetCell(5, 5, 0);
        check(layer.chunks.empty(), "clearing the last cell removes the chunk");
    }

    // ---- (b) GetCell と座標変換 ----
    {
        TilemapLibrary lib;
        TilemapLibrary* previous = tilemap::Library();
        tilemap::Install(&lib);
        const fs::path file = root / L"world.tilemap.json";
        WriteText(file, TilemapLibrary::ToJson(MakeSampleMap()).dump(2));
        const uint64_t hash = lib.LoadFromFile(file.wstring());

        World world;
        const EntityID e = world.CreateEntity("map");
        auto* tm = world.AddComponent<TilemapComponent>(e);
        tm->tilemap = AssetID{ hash };
        world.GetComponent<LocalTransform>(e)->position = { 10.0f, 5.0f, 2.0f };
        auto* wm = world.GetComponent<WorldMatrixComponent>(e);
        wm->value._41 = 10.0f;
        wm->value._42 = 5.0f;
        wm->value._43 = 2.0f;
        const EntityID bare = world.CreateEntity("no tilemap");

        check(TilemapSystem::GetCell(world, e, 0, 0, 0) == tilecell::Make(1, false, false, 0), "GetCell: (0, 0)");
        check(TilemapSystem::GetCell(world, e, 0, -1, -1) == tilecell::Make(65535, true, true, 3),
              "GetCell: negative cell (-1, -1) lives in chunk (-1, -1)");
        check(TilemapSystem::GetCell(world, e, 0, 16, 0) == tilecell::Make(3, false, true, 2, 5),
              "GetCell: chunk boundary (16, 0)");
        check(TilemapSystem::GetCell(world, e, 1, 3, 4) == tilecell::Make(4, false, false, 0), "GetCell: second layer");
        check(TilemapSystem::GetCell(world, e, 0, 100, 100) == 0, "GetCell: outside the used chunks is 0");
        check(TilemapSystem::GetCell(world, e, 2, 3, 4) == 0 && TilemapSystem::GetCell(world, e, -1, 3, 4) == 0,
              "GetCell: layer out of range is 0");
        check(TilemapSystem::GetCell(world, bare, 0, 0, 0) == 0, "GetCell: entity without a Tilemap is 0");

        std::vector<ChunkKey> keys;
        TilemapSystem::EnumerateChunks(world, e, 0, keys);
        check(keys.size() == 3 && keys[0].cy == -1 && keys[1].cx == 0 && keys[2].cx == 1,
              "EnumerateChunks: (cy, cx) ascending");
        std::array<uint32_t, kTilemapChunkCells> cells{};
        check(TilemapSystem::GetChunkCells(world, e, 0, ChunkKey{ 0, 1 }, cells)
                  && cells[0] == tilecell::Make(3, false, true, 2, 5),
              "GetChunkCells: chunk (cx 1, cy 0), cell (16, 0) is index 0");
        check(TilemapSystem::GetChunkCells(world, e, 0, ChunkKey{ 50, 50 }, cells) && cells[0] == 0
                  && !TilemapSystem::GetChunkCells(world, e, 5, ChunkKey{ 0, 0 }, cells),
              "GetChunkCells: absent chunk is zero-filled, absent layer fails");

        // 原点 (10, 5)、セル 0.5 x 2.0。境界ちょうどは上側 / 右側のセルに属する
        int32_t cx = 0;
        int32_t cy = 0;
        TilemapSystem::WorldToCell(world, e, { 10.26f, 5.0f, 0.0f }, cx, cy);
        check(cx == 0 && cy == 0, "WorldToCell: (10.26, 5.0) -> (0, 0)");
        TilemapSystem::WorldToCell(world, e, { 9.99f, 4.99f, 0.0f }, cx, cy);
        check(cx == -1 && cy == -1, "WorldToCell: just below the origin -> (-1, -1)");
        TilemapSystem::WorldToCell(world, e, { 10.5f, 7.0f, 0.0f }, cx, cy);
        check(cx == 1 && cy == 1, "WorldToCell: exactly on the edge belongs to the upper / right cell");
        const DirectX::XMFLOAT3 c00 = TilemapSystem::CellToWorld(world, e, 0, 0);
        check(c00.x == 10.25f && c00.y == 6.0f && c00.z == 2.0f, "CellToWorld: cell (0, 0) center, z = origin z");
        bool roundTrip = true;
        for (int32_t y = -20; y <= 20 && roundTrip; ++y) {
            for (int32_t x = -20; x <= 20; ++x) {
                const DirectX::XMFLOAT3 w = TilemapSystem::CellToWorld(world, e, x, y);
                TilemapSystem::WorldToCell(world, e, w, cx, cy);
                if (cx != x || cy != y) {
                    roundTrip = false;
                    break;
                }
            }
        }
        check(roundTrip, "WorldToCell(CellToWorld(c)) == c for -20..20");

        const uint64_t dirtyBefore = TilemapSystem::DirtyEpoch();
        TilemapSystem::MarkAllDirty();
        check(TilemapSystem::DirtyEpoch() > dirtyBefore, "MarkAllDirty bumps the dirty epoch");
        tilemap::Install(previous);
    }

    // ---- (c) パレットの id 割り当てと往復 ----
    {
        TilePalette p;
        check(p.NextId() == 1, "empty palette: next id is 1");
        p.Add("grass", AssetID{ 11 });
        p.Add("dirt", AssetID{ 12 });
        p.Add("stone", AssetID{ 13 });
        check(p.tiles[0].id == 1 && p.tiles[1].id == 2 && p.tiles[2].id == 3, "ids are assigned 1, 2, 3");
        p.tiles.erase(p.tiles.begin() + 1); // id 2 を消しても、次は最大 + 1
        check(p.NextId() == 4, "removing a middle id does not free it (max + 1)");
        p.tiles.pop_back(); // 最大 (3) を消すと、残りの最大 (1) + 1 = 2 が次になる
        check(p.NextId() == 2, "removing the max id lowers the next id to (new max) + 1");
        TileDef* added = p.Add("max", AssetID{ 14 });
        check(added != nullptr && added->id == 2, "Add returns the new tile");
        added->id = kTileIdMax;
        check(p.NextId() == 0 && p.Add("overflow", AssetID{}) == nullptr, "exhausted ids: NextId 0, Add fails");
        added->id = 2;

        // 往復 (rule / anim 付き) と Sanitize
        TileDef& rich = p.tiles.back();
        rich.collider = 1;
        rich.oneWay = true;
        rich.hp = 3;
        rich.destroyedTile = 1;
        rich.destroyEffect = AssetID{ 99 };
        rich.hasRule = true;
        rich.variants.push_back(TileVariant{ "xo.x.o..", AssetID{ 21 } });
        rich.hasAnim = true;
        rich.ticksPerFrame = 8;
        rich.animFrames = { AssetID{ 31 }, AssetID{ 32 } };
        const nlohmann::json first = TilePaletteLibrary::ToJson(p);
        TilePalette loaded;
        check(TilePaletteLibrary::FromJson(first, loaded), "FromJson accepts a palette");
        check(first.dump(2) == TilePaletteLibrary::ToJson(loaded).dump(2),
              "palette JSON round trip is identical (rule / anim included)");
        const TileDef* found = loaded.Find(2);
        check(found != nullptr && found->hasRule && found->variants.size() == 1
                  && found->variants[0].neighbors == "xo.x.o.." && found->hasAnim && found->animFrames.size() == 2
                  && found->destroyEffect.value == 99,
              "rule / anim fields are read");
        check(loaded.Find(3) == nullptr && loaded.Find(1) != nullptr, "Find: missing id is nullptr");

        TilePalette messy;
        const nlohmann::json bad = nlohmann::json::parse(
            "{\"tilepalette\":1,\"tiles\":[{\"id\":0},{\"id\":70000},{\"id\":5,\"name\":\"a\"},"
            "{\"id\":5,\"name\":\"dup\"},{\"id\":2,\"hp\":999,\"collider\":7,"
            "\"rule\":{\"variants\":[{\"neighbors\":\"abc\"}]},\"anim\":{\"ticksPerFrame\":0}}]}");
        check(TilePaletteLibrary::FromJson(bad, messy), "FromJson accepts a messy palette");
        check(messy.tiles.size() == 2 && messy.tiles[0].id == 2 && messy.tiles[1].id == 5
                  && messy.tiles[1].name == "a",
              "Sanitize: id 0 / 70000 / duplicate dropped, sorted by id, first duplicate wins");
        check(messy.tiles[0].hp == 255 && messy.tiles[0].collider == 1 && messy.tiles[0].ticksPerFrame == 1
                  && messy.tiles[0].variants[0].neighbors == "........",
              "Sanitize: hp / collider / ticksPerFrame clamped, bad neighbors become any");

        // ライブラリ経由
        TilePaletteLibrary lib;
        const fs::path file = root / L"ground.tilepalette.json";
        WriteText(file, first.dump(2));
        const uint64_t hash = lib.LoadFromFile(file.wstring());
        const TilePalette* got = lib.Get(hash);
        check(hash != 0 && got != nullptr && got->name == "ground" && got->tiles.size() == loaded.tiles.size(),
              "palette LoadFromFile registers by GUID with the file name");
    }

    // ---- (d) flip / 回転の UV ----
    {
        float uv[4][2];
        auto is = [&](float a0, float a1, float b0, float b1, float c0, float c1, float d0, float d1) {
            return uv[0][0] == a0 && uv[0][1] == a1 && uv[1][0] == b0 && uv[1][1] == b1 && uv[2][0] == c0
                   && uv[2][1] == c1 && uv[3][0] == d0 && uv[3][1] == d1;
        };
        // 左上 (0,0) 右上 (1,0) 右下 (1,1) 左下 (0,1)
        Sprite2DRenderer::TileCornerUv(0, 0, 1, 1, false, false, 0, uv);
        check(is(0, 0, 1, 0, 1, 1, 0, 1), "TileCornerUv: identity");
        Sprite2DRenderer::TileCornerUv(0, 0, 1, 1, true, false, 0, uv);
        check(is(1, 0, 0, 0, 0, 1, 1, 1), "TileCornerUv: flipX swaps left and right");
        Sprite2DRenderer::TileCornerUv(0, 0, 1, 1, false, true, 0, uv);
        check(is(0, 1, 1, 1, 1, 0, 0, 0), "TileCornerUv: flipY swaps top and bottom");
        Sprite2DRenderer::TileCornerUv(0, 0, 1, 1, false, false, 1, uv);
        check(is(1, 0, 1, 1, 0, 1, 0, 0),
              "TileCornerUv: rotation 1 turns the picture 90 degrees counter-clockwise (top-left moves to bottom-left)");
        Sprite2DRenderer::TileCornerUv(0, 0, 1, 1, false, false, 2, uv);
        float flipped[4][2];
        Sprite2DRenderer::TileCornerUv(0, 0, 1, 1, true, true, 0, flipped);
        bool same = true;
        for (int i = 0; i < 4; ++i) {
            same = same && uv[i][0] == flipped[i][0] && uv[i][1] == flipped[i][1];
        }
        check(same, "TileCornerUv: rotation 2 == flipX + flipY");
        Sprite2DRenderer::TileCornerUv(0, 0, 1, 1, false, false, 4, uv);
        check(is(0, 0, 1, 0, 1, 1, 0, 1), "TileCornerUv: rotation 4 is identity");
        Sprite2DRenderer::TileCornerUv(0, 0, 1, 1, true, false, 1, uv);
        check(is(0, 0, 0, 1, 1, 1, 1, 0), "TileCornerUv: flip is applied before rotation");
    }

    // ---- (e) 正射影の視錐台 AABB (タイルのチャンクを画面外で切る) ----
    {
        using namespace DirectX;
        // カメラ (3, -2, -10)、+Z 向き、半高 4 / アスペクト 2 (幅 16 x 高さ 8)、near 0.5 / far 100
        XMFLOAT4X4 view;
        XMFLOAT4X4 ortho;
        XMFLOAT4X4 persp;
        XMStoreFloat4x4(&view, XMMatrixTranslation(-3.0f, 2.0f, 10.0f));
        XMStoreFloat4x4(&ortho, XMMatrixOrthographicLH(16.0f, 8.0f, 0.5f, 100.0f));
        XMStoreFloat4x4(&persp, XMMatrixPerspectiveFovLH(1.0f, 2.0f, 0.5f, 100.0f));
        XMFLOAT3 lo;
        XMFLOAT3 hi;
        auto close = [](float a, float b) { return std::fabs(a - b) < 1e-3f; };
        check(Sprite2DRenderer::OrthoWorldBounds(&view, &ortho, lo, hi) && close(lo.x, -5.0f) && close(hi.x, 11.0f)
                  && close(lo.y, -6.0f) && close(hi.y, 2.0f) && close(lo.z, -9.5f) && close(hi.z, 90.0f),
              "OrthoWorldBounds: x -5..11, y -6..2, z -9.5..90 for the camera at (3, -2, -10)");
        check(!Sprite2DRenderer::OrthoWorldBounds(&view, &persp, lo, hi), "OrthoWorldBounds: a perspective projection is not culled");
        check(!Sprite2DRenderer::OrthoWorldBounds(nullptr, &ortho, lo, hi)
                  && !Sprite2DRenderer::OrthoWorldBounds(&view, nullptr, lo, hi),
              "OrthoWorldBounds: missing matrices are not culled");
    }

    fs::remove_all(root, ec);

    if (failCount == 0) {
        MYE_LOG_INFO("==== Tilemap self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== Tilemap self test: %d FAILURE(S) ====", failCount);
    return false;
}

} // namespace mye
