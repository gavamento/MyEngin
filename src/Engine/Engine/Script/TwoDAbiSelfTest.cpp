//====================================================================================
//                          TwoDAbiSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                       10/21/2026
//                                          ABI v31 (2D: 箱のクエリ・スプライト名・タイルマップ) の回帰テスト
//====================================================================================
#include "Engine/Engine/Script/TwoDAbiSelfTest.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>
#include <vector>

#include "Engine/Core/Asset/ImportMetaResolver.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Asset/AssetDatabase.h"
#include "Engine/Engine/Physics/Rigid/PhysicsSystem.h"
#include "Engine/Engine/Rendering/Sprite2D/SpriteLibrary.h"
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Engine/Scene/TransformSystem.h"
#include "Engine/Engine/Script/EngineApiTable.h"
#include "Engine/Engine/Tilemap/TilemapAssets.h"
#include "Engine/Engine/Tilemap/TilemapSystem.h"
#include "Engine/Platform/PathUtil.h"

namespace fs = std::filesystem;

namespace mye {
namespace {

constexpr uint32_t kTileFloor = 1;  // 衝突あり
constexpr uint32_t kTileCrate = 2;  // 衝突あり、hp 2、壊れると kTileRubble
constexpr uint32_t kTileRubble = 3; // 衝突なし
constexpr int32_t kTileLayerBit = 5; // タイルの箱の Collider.layer (通常の Collider は 0)
constexpr uint32_t kTileMask = 1u << kTileLayerBit;
constexpr uint32_t kPlainMask = 1u;

uint32_t Cell(uint32_t tileId) { return tilecell::Make(tileId, false, false, 0); }

MyeEntityId Shared(EntityID id) { return MyeEntityId{ id.index, id.generation }; }

bool SameEntity(MyeEntityId a, EntityID b) { return a.index == b.index && a.generation == b.generation; }

bool Near(float a, float b, float tol = 1e-3f) { return std::fabs(a - b) <= tol; }

TilePalette MakePalette()
{
    TilePalette p;
    p.Add("floor", AssetID{ 101 })->collider = 1;
    TileDef* crate = p.Add("crate", AssetID{ 102 });
    crate->collider = 1;
    crate->hp = 2;
    crate->destroyedTile = kTileRubble;
    p.Add("rubble", AssetID{ 103 });
    return p;
}

// layer 0: 床 (y = 0、x = -2..5) と木箱 (3, 1)。layer 1: 衝突しない瓦礫 (1, 1)。layer 2: 空
TilemapAsset MakeMap(uint64_t paletteHash)
{
    TilemapAsset m;
    m.palette = paletteHash;
    TilemapLayer ground;
    ground.name = "Ground";
    for (int32_t x = -2; x <= 5; ++x) {
        ground.SetCell(x, 0, Cell(kTileFloor));
    }
    ground.SetCell(3, 1, Cell(kTileCrate));
    m.layers.push_back(std::move(ground));
    TilemapLayer back;
    back.name = "Back";
    back.collision = false;
    back.SetCell(1, 1, Cell(kTileRubble));
    m.layers.push_back(std::move(back));
    TilemapLayer empty;
    empty.name = "Empty";
    m.layers.push_back(std::move(empty));
    return m;
}

// MyeEngineApi を実体の ScriptApiContext から組んだ 1 本
struct ApiRig {
    ScriptApiContext ctx;
    MyeEngineApi api{};
    void Build() { BuildEngineApi(api, &ctx); }
    void* Engine() { return api.engine; }
};

void WriteText(const fs::path& path, const std::string& text)
{
    std::ofstream f(path, std::ios::binary);
    f << text;
}

} // namespace

bool RunTwoDAbiSelfTest()
{
    MYE_LOG_INFO("==== TwoDAbi (ABI v31: 2D queries / SpriteByName / Tilemap) self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    // ---- 足場: ライブラリ・シーン・物理の供給口 ----
    TilePaletteLibrary palettes;
    TilemapLibrary maps;
    TilePaletteLibrary* previousPalettes = tilepalette::Library();
    TilemapLibrary* previousMaps = tilemap::Library();
    tilepalette::Install(&palettes);
    tilemap::Install(&maps);
    const uint64_t paletteHash = palettes.Register(L"C:\\sel\\abi2d\\a.tilepalette.json", MakePalette());
    const AssetID mapId{ maps.Register(L"C:\\sel\\abi2d\\a.tilemap.json", MakeMap(paletteHash)) };
    check(paletteHash != 0 && mapId.value != 0, "fixture: palette and map registered");

    Scene scene;
    World& world = scene.GetWorld();
    TransformSystem transforms;
    TilemapSystem tiles;
    PhysicsSystem phys;
    phys.AddStaticShapeSource(&tiles);
    physicsquery::Install(&phys);

    // Tilemap は原点 (10, 20, 0)。タイルの箱は layer = kTileLayerBit
    GameObject mapGo = scene.CreateGameObjectTracked("Map");
    mapGo.SetLocalPosition(10.0f, 20.0f, 0.0f);
    {
        auto* tm = mapGo.AddComponent<TilemapComponent>();
        tm->tilemap = mapId;
        tm->layer = kTileLayerBit;
    }
    // 通常の Collider (球、layer 0)。タイルの箱と同じクエリに並ぶ
    GameObject ballGo = scene.CreateGameObjectTracked("Ball");
    ballGo.SetLocalPosition(11.5f, 23.0f, 0.0f);
    {
        auto* col = ballGo.AddComponent<ColliderComponent>();
        col->shape = collidershape::kSphere;
        col->radius = 0.25f;
        col->layer = 0;
    }
    // Tilemap でも Collider でもないエンティティ (Tilemap* の対象外)
    GameObject plainGo = scene.CreateGameObjectTracked("Plain");
    plainGo.SetLocalPosition(0.0f, 0.0f, 0.0f);
    world.ApplyStructuralChanges();
    transforms.Update(world);
    const EntityID mapEntity = mapGo.Id();
    const EntityID ballEntity = ballGo.Id();
    const MyeEntityId map = Shared(mapEntity);
    const MyeEntityId plain = Shared(plainGo.Id());

    ApiRig rig;
    rig.ctx.scene = &scene;
    rig.ctx.tilemap = &tiles;
    rig.Build();
    const MyeEngineApi& api = rig.api;
    void* const eng = rig.Engine();

    // ---- 10 スロットが充填されている ----
    check(api.version == MYE_API_VERSION && api.OverlapBoxMasked != nullptr && api.BoxCast != nullptr
              && api.SpriteByName != nullptr && api.TilemapGetCell != nullptr && api.TilemapSetCell != nullptr
              && api.TilemapFillRect != nullptr && api.TilemapDamageCell != nullptr && api.TilemapWorldToCell != nullptr
              && api.TilemapCellToWorld != nullptr && api.TilemapGetBounds != nullptr,
          "slots: all 10 v31 slots are non-null");

    // ---- GetCell / GetBounds ----
    check(api.TilemapGetCell(eng, map, 0, 0, 0) == Cell(kTileFloor) && api.TilemapGetCell(eng, map, 0, 0, 1) == 0u
              && api.TilemapGetCell(eng, map, 0, 3, 1) == Cell(kTileCrate)
              && api.TilemapGetCell(eng, map, 1, 1, 1) == Cell(kTileRubble),
          "GetCell: reads the asset's cells on layer 0 and 1 (empty = 0)");
    check(api.TilemapGetCell(eng, map, 9, 0, 0) == 0u && api.TilemapGetCell(eng, map, -1, 0, 0) == 0u
              && api.TilemapGetCell(eng, plain, 0, 0, 0) == 0u,
          "GetCell: a bad layer and a non-Tilemap entity give 0");
    int32_t bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    check(api.TilemapGetBounds(eng, map, 0, &bx0, &by0, &bx1, &by1) == 1 && bx0 == -2 && by0 == 0 && bx1 == 5 && by1 == 1,
          "GetBounds: layer 0 is (-2, 0)..(5, 1)");
    check(api.TilemapGetBounds(eng, map, 1, &bx0, &by0, &bx1, &by1) == 1 && bx0 == 1 && by0 == 1 && bx1 == 1 && by1 == 1,
          "GetBounds: layer 1 is the single cell (1, 1)");
    bx0 = by0 = bx1 = by1 = 77;
    check(api.TilemapGetBounds(eng, map, 2, &bx0, &by0, &bx1, &by1) == 0 && api.TilemapGetBounds(eng, map, 9, &bx0, &by0, &bx1, &by1) == 0
              && api.TilemapGetBounds(eng, plain, 0, &bx0, &by0, &bx1, &by1) == 0
              && api.TilemapGetBounds(eng, map, 0, nullptr, &by0, &bx1, &by1) == 0 && bx0 == 77 && by1 == 77,
          "GetBounds: an empty layer, a bad layer, a non-Tilemap entity and a null out all fail without touching the outs");

    // ---- WorldToCell / CellToWorld (原点 (10, 20, 0)、cellSize 1) ----
    {
        int32_t cx = 0, cy = 0;
        check(api.TilemapWorldToCell(eng, map, { 13.5f, 21.2f, 0.0f }, &cx, &cy) == 1 && cx == 3 && cy == 1,
              "WorldToCell: (13.5, 21.2) is cell (3, 1)");
        check(api.TilemapWorldToCell(eng, map, { 9.9f, 19.9f, 0.0f }, &cx, &cy) == 1 && cx == -1 && cy == -1,
              "WorldToCell: just below / left of the origin is (-1, -1) (floor division)");
        MyeVec3 p = {};
        check(api.TilemapCellToWorld(eng, map, 3, 1, &p) == 1 && Near(p.x, 13.5f) && Near(p.y, 21.5f) && Near(p.z, 0.0f),
              "CellToWorld: cell (3, 1) centre is (13.5, 21.5, 0)");
        bool roundTrip = true;
        for (int32_t y = -3; y <= 4; ++y) {
            for (int32_t x = -4; x <= 7; ++x) {
                int32_t rx = 99, ry = 99;
                roundTrip = roundTrip && api.TilemapCellToWorld(eng, map, x, y, &p) == 1
                            && api.TilemapWorldToCell(eng, map, p, &rx, &ry) == 1 && rx == x && ry == y;
            }
        }
        check(roundTrip, "WorldToCell(CellToWorld(c)) == c over a block that spans the origin");
        cx = cy = 55;
        const float nan = std::numeric_limits<float>::quiet_NaN();
        check(api.TilemapWorldToCell(eng, map, { nan, 0.0f, 0.0f }, &cx, &cy) == 0 && cx == 55
                  && api.TilemapWorldToCell(eng, map, { 0.0f, 0.0f, 0.0f }, nullptr, &cy) == 0
                  && api.TilemapWorldToCell(eng, plain, { 0.0f, 0.0f, 0.0f }, &cx, &cy) == 0
                  && api.TilemapCellToWorld(eng, plain, 0, 0, &p) == 0 && api.TilemapCellToWorld(eng, map, 0, 0, nullptr) == 0,
              "WorldToCell / CellToWorld: NaN, a null out and a non-Tilemap entity fail without touching the outs");
    }

    // ---- 空間クエリがタイルの箱を見る (R12 (b)) ----
    {
        MyeRaycastHit hit = {};
        const MyeVec3 down = { 0.0f, -1.0f, 0.0f };
        // x = 12.5 はセル 2 の真上 (床だけ、床の上面 y = 21)。球 (x = 11.5) は外れる
        check(RaycastWorld(world, { 12.5f, 25.0f, 0.0f }, down, 20.0f, &hit) == 1 && SameEntity(hit.entity, mapEntity)
                  && Near(hit.distance, 4.0f) && Near(hit.normal.y, 1.0f),
              "Raycast: a ray down onto the tile floor hits it, owner = the Tilemap entity, distance 4");
        check(api.Raycast(eng, { 12.5f, 25.0f, 0.0f }, down, 20.0f, &hit) == 1 && SameEntity(hit.entity, mapEntity)
                  && Near(hit.distance, 4.0f),
              "ABI Raycast: the same hit through the table");
        check(RaycastWorld(world, { 13.5f, 25.0f, 0.0f }, down, 20.0f, &hit) == 1 && Near(hit.distance, 3.0f),
              "Raycast: over the crate the top is y = 22 (distance 3)");
        // 通常の Collider (球 y = 23) はタイルより手前
        check(RaycastWorld(world, { 11.5f, 25.0f, 0.0f }, down, 20.0f, &hit) == 1 && SameEntity(hit.entity, ballEntity)
                  && Near(hit.distance, 1.75f),
              "Raycast: the ball above the floor is hit first (plain colliders and tiles are one pool)");
        check(api.RaycastMasked(eng, { 11.5f, 25.0f, 0.0f }, down, 20.0f, kTileMask, &hit) == 1
                  && SameEntity(hit.entity, mapEntity) && Near(hit.distance, 4.0f),
              "ABI RaycastMasked: the tile layer mask skips the ball and finds the floor");
        check(RaycastWorld(world, { 12.5f, 25.0f, 0.0f }, down, 20.0f, &hit, kPlainMask) == 0,
              "Raycast: a mask without the tile layer ignores the tiles");
        check(RaycastWorld(world, { 40.0f, 25.0f, 0.0f }, down, 20.0f, &hit) == 0,
              "Raycast: a ray beside the map misses");

        // OverlapBoxMasked (ABI): 床に触れる箱
        MyeEntityId out[4] = {};
        check(api.OverlapBoxMasked(eng, { 12.5f, 21.0f, 0.0f }, { 0.25f, 0.25f, 0.25f }, kTileMask, out, 4) == 1
                  && SameEntity(out[0], mapEntity),
              "OverlapBoxMasked: a box touching the floor returns the Tilemap entity");
        check(api.OverlapBoxMasked(eng, { 12.5f, 21.0f, 0.0f }, { 0.25f, 0.25f, 0.25f }, kPlainMask, out, 4) == 0,
              "OverlapBoxMasked: a mask without the tile layer excludes the tile");
        check(api.OverlapBoxMasked(eng, { 12.5f, 24.0f, 0.0f }, { 0.25f, 0.25f, 0.25f }, kTileMask, out, 4) == 0,
              "OverlapBoxMasked: a box in the air finds nothing");
        // 床の箱と木箱の箱 (別の箱) とボールを覆う箱 = 持ち主 2 つ (Tilemap は 1 回だけ数える)
        check(api.OverlapBoxMasked(eng, { 12.0f, 22.0f, 0.0f }, { 3.0f, 2.0f, 1.0f }, 0xFFFFFFFFu, out, 4) == 2
                  && SameEntity(out[0], mapEntity) && SameEntity(out[1], ballEntity),
              "OverlapBoxMasked: several tile boxes count as one owner, ordered by entity index with the ball");
        check(api.OverlapBoxMasked(eng, { 12.0f, 22.0f, 0.0f }, { 3.0f, 2.0f, 1.0f }, 0xFFFFFFFFu, nullptr, 0) == 2,
              "OverlapBoxMasked: a null out only counts");
        check(OverlapSphereWorld(world, { 12.5f, 21.2f, 0.0f }, 0.3f, out, 4, kTileMask) == 1 && SameEntity(out[0], mapEntity),
              "OverlapSphere: a sphere touching the floor returns the Tilemap entity");

        // BoxCast (ABI): 箱の底が床の上面 (y = 21) に着くまで 25 - 0.25 - 21 = 3.75
        check(api.BoxCast(eng, { 12.5f, 25.0f, 0.0f }, { 0.25f, 0.25f, 0.25f }, down, 10.0f, kTileMask, &hit) == 1
                  && SameEntity(hit.entity, mapEntity) && Near(hit.distance, 3.75f),
              "BoxCast: a box falling onto the floor stops at distance 3.75, owner = the Tilemap entity");
        check(api.BoxCast(eng, { 13.5f, 25.0f, 0.0f }, { 0.25f, 0.25f, 0.25f }, down, 10.0f, kTileMask, &hit) == 1
                  && Near(hit.distance, 2.75f),
              "BoxCast: over the crate it stops 1 m earlier");
        check(api.BoxCast(eng, { 12.5f, 25.0f, 0.0f }, { 0.25f, 0.25f, 0.25f }, down, 10.0f, kPlainMask, &hit) == 0
                  && api.BoxCast(eng, { 12.5f, 25.0f, 0.0f }, { 0.25f, 0.25f, 0.25f }, { 0.0f, 1.0f, 0.0f }, 10.0f, kTileMask, &hit) == 0
                  && api.BoxCast(eng, { 12.5f, 25.0f, 0.0f }, { 0.25f, 0.25f, 0.25f }, down, 2.0f, kTileMask, &hit) == 0,
              "BoxCast: the wrong mask, the wrong direction and a too-short distance all miss");
        check(api.BoxCast(eng, { 12.5f, 21.1f, 0.0f }, { 0.25f, 0.25f, 0.25f }, down, 10.0f, kTileMask, &hit) == 1
                  && Near(hit.distance, 0.0f) && Near(hit.normal.y, 1.0f),
              "BoxCast: a box already overlapping the tile reports distance 0 and the normal -dir");
        check(SphereCastWorld(world, { 12.5f, 25.0f, 0.0f }, down, 0.25f, 10.0f, &hit, kTileMask) == 1
                  && SameEntity(hit.entity, mapEntity) && Near(hit.distance, 3.75f, 0.01f),
              "SphereCast: a sphere falling onto the floor hits the tile");

        // 供給口を外すとタイルは見えない (クエリの対象はインストールされた PhysicsSystem の登録)
        physicsquery::Install(nullptr);
        check(RaycastWorld(world, { 12.5f, 25.0f, 0.0f }, down, 20.0f, &hit) == 0
                  && api.OverlapBoxMasked(eng, { 12.5f, 21.0f, 0.0f }, { 0.25f, 0.25f, 0.25f }, kTileMask, out, 4) == 0,
              "queries: with no source installed the tiles are invisible (control)");
        physicsquery::Install(&phys);
    }

    // ---- DamageCell: 木箱 hp 2 ----
    {
        MyeRaycastHit hit = {};
        const MyeVec3 down = { 0.0f, -1.0f, 0.0f };
        check(api.TilemapDamageCell(eng, map, 0, 3, 1, 1) == 1 && tilecell::Damage(api.TilemapGetCell(eng, map, 0, 3, 1)) == 1
                  && tilecell::TileId(api.TilemapGetCell(eng, map, 0, 3, 1)) == kTileCrate,
              "DamageCell: 1 damage on the hp 2 crate is 'damaged' (1) and shows in the cell");
        check(RaycastWorld(world, { 13.5f, 25.0f, 0.0f }, down, 20.0f, &hit) == 1 && Near(hit.distance, 3.0f),
              "DamageCell: a damaged crate still blocks");
        check(api.TilemapDamageCell(eng, map, 0, 3, 1, 1) == 2 && tilecell::TileId(api.TilemapGetCell(eng, map, 0, 3, 1)) == kTileRubble,
              "DamageCell: reaching hp destroys it (2) and swaps in the rubble tile");
        check(RaycastWorld(world, { 13.5f, 25.0f, 0.0f }, down, 20.0f, &hit) == 1 && Near(hit.distance, 4.0f),
              "DamageCell: once destroyed the rubble has no collision (the ray reaches the floor)");
        check(api.TilemapDamageCell(eng, map, 0, 3, 1, 1) == 0 && api.TilemapDamageCell(eng, map, 0, 0, 0, 5) == 0
                  && api.TilemapDamageCell(eng, map, 0, 0, 5, 1) == 0 && api.TilemapDamageCell(eng, map, 0, 0, 0, 0) == 0,
              "DamageCell: rubble, an unbreakable floor, an empty cell and amount 0 are 'nothing happened' (0)");
        check(api.TilemapDamageCell(eng, map, 9, 0, 0, 1) == -1 && api.TilemapDamageCell(eng, map, 0, 40000, 0, 1) == -1
                  && api.TilemapDamageCell(eng, plain, 0, 0, 0, 1) == -1,
              "DamageCell: a bad layer, a cell out of range and a non-Tilemap entity are 'not applicable' (-1)");
    }

    // ---- SetCell / FillRect と WorldHash の対象になる書き換え ----
    {
        MyeRaycastHit hit = {};
        const MyeVec3 down = { 0.0f, -1.0f, 0.0f };
        check(RaycastWorld(world, { 16.5f, 25.0f, 0.0f }, down, 20.0f, &hit) == 0,
              "SetCell: before the edit there is no floor at cell x = 6");
        check(api.TilemapSetCell(eng, map, 0, 6, 0, Cell(kTileFloor)) == 1
                  && api.TilemapGetCell(eng, map, 0, 6, 0) == Cell(kTileFloor) && tiles.Overrides().size() >= 1,
              "SetCell: writes the cell and records an override");
        check(RaycastWorld(world, { 16.5f, 25.0f, 0.0f }, down, 20.0f, &hit) == 1 && Near(hit.distance, 4.0f),
              "SetCell: the new floor cell blocks a ray at the next query");
        check(api.TilemapSetCell(eng, map, 9, 0, 0, 1u) == 0 && api.TilemapSetCell(eng, map, 0, 40000, 0, 1u) == 0
                  && api.TilemapSetCell(eng, map, 0, 0, -40000, 1u) == 0 && api.TilemapSetCell(eng, plain, 0, 0, 0, 1u) == 0,
              "SetCell: a bad layer, cells out of range and a non-Tilemap entity are refused (0)");
        check(api.TilemapFillRect(eng, map, 0, 22, 2, 20, 0, Cell(kTileFloor)) == 9
                  && api.TilemapGetCell(eng, map, 0, 21, 1) == Cell(kTileFloor),
              "FillRect: reversed corners fill 3x3 = 9 cells and return the count");
        check(api.TilemapFillRect(eng, map, 0, 0, 0, 0, 0, Cell(kTileFloor)) == 1,
              "FillRect: a single cell is 1");
        check(api.TilemapFillRect(eng, map, 0, -30000, -30000, 30000, 30000, 1u) == 0
                  && api.TilemapFillRect(eng, map, 9, 0, 0, 1, 1, 1u) == 0 && api.TilemapFillRect(eng, plain, 0, 0, 0, 1, 1, 1u) == 0
                  && api.TilemapFillRect(eng, map, 0, 0, 0, 40000, 1, 1u) == 0,
              "FillRect: an oversized rect, a bad layer, a non-Tilemap entity and a cell out of range return 0");
        check(api.TilemapGetBounds(eng, map, 0, &bx0, &by0, &bx1, &by1) == 1 && bx0 == -2 && by0 == 0 && bx1 == 22 && by1 == 2,
              "GetBounds: follows the written cells (overrides included)");
        // 書いた値がアセットと同じなら override は消える
        api.TilemapSetCell(eng, map, 0, 6, 0, 0u);
        api.TilemapFillRect(eng, map, 0, 22, 2, 20, 0, 0u);
        api.TilemapSetCell(eng, map, 0, 3, 1, Cell(kTileCrate));
        check(tiles.Overrides().empty(), "SetCell / FillRect: writing the asset's values back clears every override");
    }

    // ---- Tilemap の無い文脈 (tilemap == nullptr) は失敗値 ----
    {
        ApiRig bare;
        bare.ctx.scene = &scene;
        bare.Build();
        void* const e2 = bare.Engine();
        int32_t a = 0, b = 0, c = 0, d = 0;
        MyeVec3 p = {};
        check(bare.api.TilemapGetCell(e2, map, 0, 0, 0) == 0u && bare.api.TilemapSetCell(e2, map, 0, 0, 0, 1u) == 0
                  && bare.api.TilemapFillRect(e2, map, 0, 0, 0, 1, 1, 1u) == 0 && bare.api.TilemapDamageCell(e2, map, 0, 0, 0, 1) == -1
                  && bare.api.TilemapWorldToCell(e2, map, { 0.0f, 0.0f, 0.0f }, &a, &b) == 0
                  && bare.api.TilemapCellToWorld(e2, map, 0, 0, &p) == 0 && bare.api.TilemapGetBounds(e2, map, 0, &a, &b, &c, &d) == 0,
              "no TilemapSystem: every Tilemap slot returns its failure value");
        MyeEntityId out[2] = {};
        check(bare.api.OverlapBoxMasked(e2, { 12.5f, 21.0f, 0.0f }, { 0.25f, 0.25f, 0.25f }, kTileMask, out, 2) == 1,
              "no TilemapSystem: the queries do not depend on the context's tilemap (they follow the installed physics)");
    }

    // ---- SpriteByName ----
    {
        std::error_code ec;
        const fs::path root = fs::temp_directory_path(ec) / L"mye_twodabi_selftest";
        fs::remove_all(root, ec);
        fs::create_directories(root / L"textures", ec);
        const fs::path tex = root / L"textures" / L"hero.png";
        WriteText(tex, "dummy");
        AssetMeta m;
        m.guid = 0xAB34CD56EF789012ull;
        m.type = AssetType::Texture;
        m.version = 2;
        const auto addSlice = [&m](uint32_t id, const char* name, int x) {
            importmeta::SpriteSlice s;
            s.id = id;
            std::snprintf(s.name, sizeof(s.name), "%s", name);
            s.x = x;
            s.w = 32;
            s.h = 32;
            m.tex.sprites.push_back(s);
        };
        addSlice(1, "idle", 0);
        addSlice(2, "run", 32);
        AssetDatabase::WriteMeta(tex.wstring() + L".meta", m);
        AssetDatabase db;
        db.ScanAndSync(root.wstring());
        db.InstallAsKeyResolver();
        SpriteLibrary sprites;
        SpriteLibrary* previousSprites = spritelib::Library();
        spritelib::Install(&sprites);

        ApiRig withRoot;
        withRoot.ctx.scene = &scene;
        withRoot.ctx.assetsRoot = root.wstring();
        withRoot.Build();
        const uint64_t expected = SpriteLibrary::SliceId(tex.wstring(), 2).value;
        check(expected != 0 && withRoot.api.SpriteByName(withRoot.Engine(), "textures/hero.png", "run") == expected,
              "SpriteByName: an assets-relative path resolves to the slice's AssetID (= Sprite2D.sprite)");
        check(withRoot.api.SpriteByName(withRoot.Engine(), WideToUtf8(tex.wstring()).c_str(), "run") == expected
                  && withRoot.api.SpriteByName(withRoot.Engine(), "textures/hero.png", "idle") == SpriteLibrary::SliceId(tex.wstring(), 1).value,
              "SpriteByName: an absolute path gives the same id, and each name gives its own slice");
        check(withRoot.api.SpriteByName(withRoot.Engine(), "textures/hero.png", "nope") == 0
                  && withRoot.api.SpriteByName(withRoot.Engine(), "textures/missing.png", "run") == 0
                  && withRoot.api.SpriteByName(withRoot.Engine(), nullptr, "run") == 0
                  && withRoot.api.SpriteByName(withRoot.Engine(), "textures/hero.png", nullptr) == 0
                  && withRoot.api.SpriteByName(withRoot.Engine(), "", "run") == 0,
              "SpriteByName: an unknown name, a missing texture, null and empty arguments give 0");
        spritelib::Install(nullptr);
        check(withRoot.api.SpriteByName(withRoot.Engine(), "textures/hero.png", "run") == 0,
              "SpriteByName: with no SpriteLibrary installed it gives 0");
        spritelib::Install(previousSprites);
        AssetDatabase::UninstallKeyResolver();
        fs::remove_all(root, ec);
    }

    physicsquery::Install(nullptr);
    tilepalette::Install(previousPalettes);
    tilemap::Install(previousMaps);

    if (failCount == 0) {
        MYE_LOG_INFO("TwoDAbi self test: ALL PASS");
        return true;
    }
    MYE_LOG_ERROR("TwoDAbi self test: %d FAIL", failCount);
    return false;
}

} // namespace mye
