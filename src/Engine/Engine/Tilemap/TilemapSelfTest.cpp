//====================================================================================
//                          TilemapSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                          タイルパレット / タイルマップ資産と座標変換の回帰テスト
//====================================================================================
#include "Engine/Engine/Tilemap/TilemapSelfTest.h"

#include <algorithm>
#include <array>
#include <chrono>
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
#include "Engine/Core/Util/ByteIo.h"
#include "Engine/Engine/Demo/DemoContent.h"
#include "Engine/Engine/Loop/EngineLoop.h"
#include "Engine/Engine/Loop/TickRunner.h"
#include "Engine/Engine/Physics/Rigid/PhysicsSystem.h"
#include "Engine/Engine/Rendering/Sprite2D/Sprite2DRenderer.h"
#include "Engine/Engine/Replay/SimSnapshot.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/Scene/Prefab.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Engine/Scene/TransformSystem.h"
#include "Engine/Engine/Script/EngineApiTable.h"
#include "Engine/Renderer/Device/GpuResources.h"
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


// Tilemap の無いシーン (plane2D の球が箱の床へ落ちる 120 tick) の HashWorld と snapshot の指紋
struct BareSceneResult {
    uint64_t worldHash = 0;
    uint64_t blobDigest = 0;
    size_t blobSize = 0;
    bool sectionStripped = false; // TM2D 節 (空) が 1 つ見つかって除かれた
};

uint64_t Fnv1a(const std::byte* p, size_t n, uint64_t h)
{
    for (size_t i = 0; i < n; ++i) {
        h = (h ^ static_cast<uint64_t>(p[i])) * 0x100000001B3ull;
    }
    return h;
}

BareSceneResult RunBareScene()
{
    Scene scene;
    GameObject ground = scene.CreateGameObjectTracked("Ground");
    ground.SetLocalPosition(0.0f, -0.5f, 0.0f);
    {
        auto* col = ground.AddComponent<ColliderComponent>();
        col->shape = collidershape::kBox;
        col->isTrigger = false;
        col->halfExtents = { 5.0f, 0.5f, 1.0f };
    }
    GameObject ball = scene.CreateGameObjectTracked("Ball");
    ball.SetLocalPosition(0.0f, 3.0f, 0.0f);
    {
        auto* col = ball.AddComponent<ColliderComponent>();
        col->shape = collidershape::kSphere;
        col->isTrigger = false;
        col->radius = 0.25f;
        auto* rb = ball.AddComponent<RigidbodyComponent>();
        rb->mass = 1.0f;
        rb->plane2D = true;
    }
    scene.GetWorld().ApplyStructuralChanges();
    PhysicsSystem phys;
    for (int tick = 0; tick < 120; ++tick) {
        phys.Update(scene.GetWorld(), 1.0f / 60.0f);
    }
    BareSceneResult r;
    r.worldHash = HashWorld(scene.GetWorld(), SimSourcesOf(scene, nullptr, nullptr, nullptr));
    SimRefs refs;
    refs.scene = &scene;
    std::vector<std::byte> blob;
    CaptureSimSnapshot(refs, blob);
    // 版 (先頭 4 バイトの次の 4 バイト) と、空の TM2D 節 (magic + 件数 0) を除いた指紋。版と節の足し算以外で
    // blob が変わっていないことを見る
    constexpr size_t kVersionAt = 4;
    for (size_t i = 0; i < sizeof(uint32_t) && kVersionAt + i < blob.size(); ++i) {
        blob[kVersionAt + i] = std::byte{ 0 };
    }
    const std::byte section[12] = { std::byte{ 'T' }, std::byte{ 'M' }, std::byte{ '2' }, std::byte{ 'D' } };
    size_t at = blob.size();
    for (size_t i = 0; i + sizeof(section) <= blob.size(); ++i) {
        if (std::equal(section, section + sizeof(section), blob.begin() + static_cast<std::ptrdiff_t>(i))) {
            at = i;
            break;
        }
    }
    r.sectionStripped = at < blob.size();
    if (at < blob.size()) {
        blob.erase(blob.begin() + static_cast<std::ptrdiff_t>(at), blob.begin() + static_cast<std::ptrdiff_t>(at + sizeof(section)));
    }
    r.blobSize = blob.size();
    r.blobDigest = Fnv1a(blob.data(), blob.size(), 0xCBF29CE484222325ull);
    return r;
}

// ---- タイル衝突 / セル書き換え / 破壊の検査用の足場 ----

constexpr float kDt = 1.0f / 60.0f;
constexpr uint32_t kTileFloor = 1;    // 通常の床
constexpr uint32_t kTilePlatform = 2; // 一方通行
constexpr uint32_t kTileCrate = 3;    // hp 3、壊れると kTileRubble + エフェクト
constexpr uint32_t kTileRubble = 4;   // 衝突なし

TilePalette MakeTestPalette(AssetID effect)
{
    TilePalette p;
    p.Add("floor", AssetID{ 101 })->collider = 1;
    TileDef* platform = p.Add("platform", AssetID{ 102 });
    platform->collider = 1;
    platform->oneWay = true;
    TileDef* crate = p.Add("crate", AssetID{ 103 });
    crate->collider = 1;
    crate->hp = 3;
    crate->destroyedTile = kTileRubble;
    crate->destroyEffect = effect;
    p.Add("rubble", AssetID{ 104 });
    return p;
}

// 床 (y = 0、x = -8..16。チャンク境界 x = 0 / 16 をまたぐ) + 一方通行の台 (y = 3、x = 10..16) + 木箱 (15, 1)
TilemapAsset MakeArenaMap(uint64_t paletteHash)
{
    TilemapAsset m;
    m.palette = paletteHash;
    TilemapLayer ground;
    ground.name = "Ground";
    for (int32_t x = -8; x <= 16; ++x) {
        ground.SetCell(x, 0, tilecell::Make(kTileFloor, false, false, 0));
    }
    for (int32_t x = 10; x <= 16; ++x) {
        ground.SetCell(x, 3, tilecell::Make(kTilePlatform, false, false, 0));
    }
    ground.SetCell(15, 1, tilecell::Make(kTileCrate, false, false, 0));
    m.layers.push_back(std::move(ground));
    return m;
}

// タイルを含む sim 1 本ぶん。物理・Transform・TilemapSystem を持ち、タイルの箱を物理の静的収集へ登録する
// TilemapSystem の Collect だけを物理へ渡す (CollectWithin の既定が絞らない Collect になる)。絞り込みの有無を比べる用
class UnculledTiles final : public StaticShapeSource {
public:
    explicit UnculledTiles(TilemapSystem& tiles) : tiles_(tiles) {}
    void Collect(World& world, std::vector<StaticShape>& out) override { tiles_.Collect(world, out); }

private:
    TilemapSystem& tiles_;
};

struct TileSim {
    Scene scene;
    PhysicsSystem phys;
    TransformSystem transforms;
    TilemapSystem tiles;
    UnculledTiles unculled{ tiles };
    EntityID mapEntity = kNullEntity;

    // culled = false なら物理は絞り込み前の全箱を受け取る
    explicit TileSim(bool culled = true)
    {
        if (culled) {
            phys.AddStaticShapeSource(&tiles);
        } else {
            phys.AddStaticShapeSource(&unculled);
        }
    }
    TileSim(const TileSim&) = delete;
    TileSim& operator=(const TileSim&) = delete;

    World& GetWorld() { return scene.GetWorld(); }
    void Step(int ticks)
    {
        for (int i = 0; i < ticks; ++i) {
            phys.Update(GetWorld(), kDt);
            transforms.Update(GetWorld());
        }
    }
    // 絞り込まない箱の数 (物理が受け取る数ではない)
    size_t AllBoxCount()
    {
        std::vector<StaticShape> all;
        tiles.Collect(GetWorld(), all);
        return all.size();
    }
    SimRefs Refs()
    {
        SimRefs refs;
        refs.scene = &scene;
        refs.tilemap = &tiles;
        return refs;
    }
};

void AddPhysicsEnvironment(Scene& s)
{
    GameObject env = s.CreateGameObjectTracked("Env");
    env.AddComponent<PhysicsEnvironmentComponent>();
}

EntityID AddMap(TileSim& sim, AssetID tilemapId, float x, float y, float z)
{
    GameObject go = sim.scene.CreateGameObjectTracked("Map");
    go.SetLocalPosition(x, y, z);
    go.AddComponent<TilemapComponent>()->tilemap = tilemapId;
    sim.mapEntity = go.Id();
    return go.Id();
}

GameObject AddBall(Scene& s, const char* name, float x, float y, float vx = 0.0f, float vy = 0.0f)
{
    GameObject go = s.CreateGameObjectTracked(name);
    go.SetLocalPosition(x, y, 0.0f);
    auto* col = go.AddComponent<ColliderComponent>();
    col->shape = collidershape::kSphere;
    col->isTrigger = false;
    col->radius = 0.25f;
    auto* rb = go.AddComponent<RigidbodyComponent>();
    rb->mass = 1.0f;
    rb->plane2D = true;
    rb->velocity = { vx, vy, 0.0f };
    return go;
}

GameObject AddCharacter(Scene& s, const char* name, float x, float y)
{
    GameObject go = s.CreateGameObjectTracked(name);
    go.SetLocalPosition(x, y, 0.0f);
    go.AddComponent<CharacterControllerComponent>()->plane2D = true;
    return go;
}

float PosY(GameObject go) { return go.GetComponent<LocalTransform>()->position.y; }
float PosX(GameObject go) { return go.GetComponent<LocalTransform>()->position.x; }

// 2 つの TileSim を同じ刺激で並走させる (g) 用。決定的な小さなシナリオ: 転がる球・歩く CC・途中のセル書き換えと damage
struct ScenarioActors {
    EntityID roller = kNullEntity;
    EntityID walker = kNullEntity;
};

ScenarioActors BuildScenario(TileSim& sim, AssetID mapId)
{
    AddPhysicsEnvironment(sim.scene);
    AddMap(sim, mapId, 0.0f, 0.0f, 0.0f);
    ScenarioActors a;
    a.roller = AddBall(sim.scene, "Roller", -7.0f, 3.0f, 6.0f, 0.0f).Id();
    a.walker = AddCharacter(sim.scene, "Walker", 11.0f, 1.9f).Id();
    sim.GetWorld().ApplyStructuralChanges();
    sim.transforms.Update(sim.GetWorld());
    return a;
}

// tick ごとの刺激 (両方の sim に同じものを与える)
void ApplyScenarioStimulus(TileSim& sim, const ScenarioActors& a, int tick)
{
    World& w = sim.GetWorld();
    if (tick == 30) {
        sim.tiles.SetCell(w, sim.mapEntity, 0, 0, 0, 0u);
        sim.tiles.SetCell(w, sim.mapEntity, 0, 1, 0, 0u);
        sim.tiles.SetCell(w, sim.mapEntity, 0, 5, 2, tilecell::Make(kTileFloor, false, false, 0));
    }
    if (tick == 40 || tick == 41 || tick == 42) {
        sim.tiles.DamageCell(w, sim.mapEntity, 0, 15, 1, 1);
    }
    if (tick == 90) {
        w.GetComponent<CharacterControllerComponent>(a.walker)->jumpSpeed = 9.0f;
    }
    if (tick == 200) {
        sim.tiles.FillRect(w, sim.mapEntity, 0, 3, 0, 4, 0, 0u);
    }
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

        TilemapSystem sim;
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

        check(sim.GetCell(world, e, 0, 0, 0) == tilecell::Make(1, false, false, 0), "GetCell: (0, 0)");
        check(sim.GetCell(world, e, 0, -1, -1) == tilecell::Make(65535, true, true, 3),
              "GetCell: negative cell (-1, -1) lives in chunk (-1, -1)");
        check(sim.GetCell(world, e, 0, 16, 0) == tilecell::Make(3, false, true, 2, 5),
              "GetCell: chunk boundary (16, 0)");
        check(sim.GetCell(world, e, 1, 3, 4) == tilecell::Make(4, false, false, 0), "GetCell: second layer");
        check(sim.GetCell(world, e, 0, 100, 100) == 0, "GetCell: outside the used chunks is 0");
        check(sim.GetCell(world, e, 2, 3, 4) == 0 && sim.GetCell(world, e, -1, 3, 4) == 0,
              "GetCell: layer out of range is 0");
        check(sim.GetCell(world, bare, 0, 0, 0) == 0, "GetCell: entity without a Tilemap is 0");

        std::vector<ChunkKey> keys;
        sim.EnumerateChunks(world, e, 0, keys);
        check(keys.size() == 3 && keys[0].cy == -1 && keys[1].cx == 0 && keys[2].cx == 1,
              "EnumerateChunks: (cy, cx) ascending");
        std::array<uint32_t, kTilemapChunkCells> cells{};
        check(sim.GetChunkCells(world, e, 0, ChunkKey{ 0, 1 }, cells)
                  && cells[0] == tilecell::Make(3, false, true, 2, 5),
              "GetChunkCells: chunk (cx 1, cy 0), cell (16, 0) is index 0");
        check(sim.GetChunkCells(world, e, 0, ChunkKey{ 50, 50 }, cells) && cells[0] == 0
                  && !sim.GetChunkCells(world, e, 5, ChunkKey{ 0, 0 }, cells),
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

    // ---- タイル衝突 / セル書き換え / 破壊 (M94k) ----
    {
        TilePaletteLibrary palettes;
        TilemapLibrary maps;
        TilePaletteLibrary* previousPalettes = tilepalette::Library();
        TilemapLibrary* previousMaps = tilemap::Library();
        tilepalette::Install(&palettes);
        tilemap::Install(&maps);

        // 破壊エフェクト用の最小プレハブ (1 エンティティ)
        PrefabLibrary prefabs;
        uint64_t effectHash = 0;
        {
            Scene src;
            GameObject fx = src.CreateGameObjectTracked("RubbleFx");
            src.GetWorld().ApplyStructuralChanges();
            effectHash = prefabs.Register(L"C:\\sel\\tilemap\\rubble_fx.prefab.json", "rubble_fx",
                                          Prefab::ExtractLocal(src, fx.Id()));
        }
        const uint64_t paletteHash =
            palettes.Register(L"C:\\sel\\tilemap\\arena.tilepalette.json", MakeTestPalette(AssetID{ effectHash }));
        const AssetID mapId{ maps.Register(L"C:\\sel\\tilemap\\arena.tilemap.json", MakeArenaMap(paletteHash)) };
        check(effectHash != 0 && paletteHash != 0 && mapId.value != 0, "tile collision: test assets registered");

        // ---- plane2D の球がタイルの床に乗り、チャンク境界を転がって渡る ----
        {
            TileSim sim;
            AddPhysicsEnvironment(sim.scene);
            AddMap(sim, mapId, 0.0f, 0.0f, 0.0f);
            GameObject rest = AddBall(sim.scene, "Rest", 8.0f, 4.0f);
            GameObject roll = AddBall(sim.scene, "Roll", -3.0f, 1.3f, 6.0f, 0.0f);
            sim.GetWorld().ApplyStructuralChanges();
            sim.transforms.Update(sim.GetWorld());
            float seamMinY = 1e9f;
            float seamMaxY = -1e9f;
            for (int tick = 0; tick < 180; ++tick) {
                sim.Step(1);
                if (PosX(roll) > -1.5f && PosX(roll) < 1.5f) {
                    seamMinY = (std::min)(seamMinY, PosY(roll));
                    seamMaxY = (std::max)(seamMaxY, PosY(roll));
                }
            }
            MYE_LOG_INFO("  [tilemap] floor: rest y %.4f / roll x %.3f y %.4f / seam y %.4f..%.4f / boxes %zu",
                         static_cast<double>(PosY(rest)), static_cast<double>(PosX(roll)),
                         static_cast<double>(PosY(roll)), static_cast<double>(seamMinY), static_cast<double>(seamMaxY),
                         sim.AllBoxCount());
            check(std::fabs(PosY(rest) - 1.25f) < 0.03f, "tile collision: a plane2D ball dropped on the tile floor rests on it (y = 1.25)");
            check(PosX(roll) > 1.5f && seamMinY > 1.2f && seamMaxY < 1.32f,
                  "tile collision: a ball rolls across the chunk seam (x = 0) without sinking or catching");
            check(sim.AllBoxCount() == 6,
                  "tile collision: 25 floor cells + 7 platform cells + 1 crate become 6 row-merged boxes");
            check(rest.GetComponent<LocalTransform>()->position.z == 0.0f
                      && rest.GetComponent<RigidbodyComponent>()->velocity.z == 0.0f,
                  "tile collision: the plane2D ball keeps z = 0 on the tile floor");

            // ---- SetCell で床を消すと落ちる ----
            const uint64_t hashWith = HashWorld(sim.GetWorld(), sim.Refs().HashSources());
            check(rest.GetComponent<RigidbodyComponent>()->isSleeping,
                  "tile edit: the ball resting on the floor has fallen asleep before the edit");
            const bool filled = sim.tiles.FillRect(sim.GetWorld(), sim.mapEntity, 0, 7, 0, 9, 0, 0u);
            check(filled && sim.tiles.Overrides().size() == 3 && sim.tiles.GetCell(sim.GetWorld(), sim.mapEntity, 0, 8, 0) == 0u
                      && sim.tiles.GetCell(sim.GetWorld(), sim.mapEntity, 0, 10, 0) == tilecell::Make(kTileFloor, false, false, 0),
                  "tile edit: FillRect over 3 floor cells leaves 3 overrides and the neighbours alone");
            check(HashWorld(sim.GetWorld(), sim.Refs().HashSources()) != hashWith,
                  "tile edit: the override table is part of the world hash");
            sim.Step(90);
            MYE_LOG_INFO("  [tilemap] hole: rest y %.3f", static_cast<double>(PosY(rest)));
            check(PosY(rest) < -3.0f && sim.AllBoxCount() == 7,
                  "tile edit: removing the floor under a resting ball makes it fall (the hole splits the row into more boxes)");
            // 元の値へ戻すと override は消える (差分が最小に保たれる)
            sim.tiles.FillRect(sim.GetWorld(), sim.mapEntity, 0, 7, 0, 9, 0, tilecell::Make(kTileFloor, false, false, 0));
            check(sim.tiles.Overrides().empty(), "tile edit: writing the asset value back removes the overrides");
            check(!sim.tiles.SetCell(sim.GetWorld(), sim.mapEntity, 5, 0, 0, 1u) && !sim.tiles.SetCell(sim.GetWorld(), sim.mapEntity, 0, 40000, 0, 1u)
                      && !sim.tiles.FillRect(sim.GetWorld(), sim.mapEntity, 0, -30000, -30000, 30000, 30000, 1u),
                  "tile edit: a bad layer, a cell out of range and an oversized rect are refused");

            // override だけで埋まるチャンクも列挙され、衝突する
            sim.tiles.SetCell(sim.GetWorld(), sim.mapEntity, 0, 40, 40, tilecell::Make(kTileFloor, false, false, 0));
            std::vector<ChunkKey> keys;
            sim.tiles.EnumerateChunks(sim.GetWorld(), sim.mapEntity, 0, keys);
            bool hasOverrideOnlyChunk = false;
            for (const ChunkKey& k : keys) {
                hasOverrideOnlyChunk = hasOverrideOnlyChunk || (k.cx == 2 && k.cy == 2);
            }
            std::array<uint32_t, kTilemapChunkCells> cells{};
            check(hasOverrideOnlyChunk && sim.tiles.GetChunkCells(sim.GetWorld(), sim.mapEntity, 0, ChunkKey{ 2, 2 }, cells)
                      && cells[static_cast<size_t>(8) * kTilemapChunkSize + 8] == tilecell::Make(kTileFloor, false, false, 0),
                  "tile edit: a chunk that exists only as an override is enumerated and readable");
            check(sim.AllBoxCount() == 7, "tile edit: a cell written into an empty chunk becomes a box");
        }

        // ---- 一方通行タイル: 剛体は下から抜けて上に乗り、CharacterController も同じ ----
        {
            TileSim sim;
            AddPhysicsEnvironment(sim.scene);
            AddMap(sim, mapId, 0.0f, 0.0f, 0.0f);
            GameObject up = AddBall(sim.scene, "Up", 13.0f, 1.8f, 0.0f, 11.0f);
            sim.GetWorld().ApplyStructuralChanges();
            sim.transforms.Update(sim.GetWorld());
            float maxY = -1e9f;
            for (int tick = 0; tick < 420; ++tick) {
                sim.Step(1);
                maxY = (std::max)(maxY, PosY(up));
            }
            MYE_LOG_INFO("  [tilemap] oneWay ball: max y %.3f final %.4f", static_cast<double>(maxY), static_cast<double>(PosY(up)));
            check(maxY > 5.0f, "tile oneWay: a plane2D ball thrown up from below passes through the platform tiles");
            check(std::fabs(PosY(up) - 4.25f) < 0.05f, "tile oneWay: ...then lands on top and rests there (y = 4.25)");
        }
        {
            TileSim sim;
            AddPhysicsEnvironment(sim.scene);
            AddMap(sim, mapId, 0.0f, 0.0f, 0.0f);
            GameObject ch = AddCharacter(sim.scene, "Jumper", 12.0f, 1.9f);
            sim.GetWorld().ApplyStructuralChanges();
            sim.transforms.Update(sim.GetWorld());
            auto* cc = ch.GetComponent<CharacterControllerComponent>();
            sim.Step(60);
            check(cc->isGrounded && std::fabs(PosY(ch) - 1.9f) < 0.02f && cc->groundEntity == sim.mapEntity,
                  "tile CC: a character stands on the tile floor and reports the Tilemap entity as the ground");
            cc->jumpSpeed = 9.0f;
            float jumpMaxY = PosY(ch);
            for (int tick = 0; tick < 240; ++tick) {
                sim.Step(1);
                jumpMaxY = (std::max)(jumpMaxY, PosY(ch));
            }
            check(jumpMaxY > 4.0f + 0.9f + 0.3f, "tile CC: a jump from below passes through the one-way tiles");
            check(cc->isGrounded && std::fabs(PosY(ch) - 4.9f) < 0.03f && cc->groundEntity == sim.mapEntity,
                  "tile CC: lands on top of the one-way tiles (y = 4.9)");
            cc->ignoreOneWay = true;
            sim.Step(120);
            check(std::fabs(PosY(ch) - 1.9f) < 0.03f, "tile CC: ignoreOneWay drops through to the floor");
        }

        // ---- 3 セル書き換え → capture → 新ワールドへ restore → 100 tick 連続実行と一致 ----
        {
            TileSim a;
            const ScenarioActors actorsA = BuildScenario(a, mapId);
            for (int tick = 0; tick < 50; ++tick) {
                ApplyScenarioStimulus(a, actorsA, tick);
                a.Step(1);
            }
            std::vector<std::byte> blob;
            const bool captured = CaptureSimSnapshot(a.Refs(), blob);
            check(captured && a.tiles.Overrides().size() == 4,
                  "tile snapshot: 3 cell rewrites + 1 destroyed crate are 4 overrides at the capture point");

            TileSim b;
            SimRefs refsB = b.Refs();
            const bool restored = captured && RestoreSimSnapshot(refsB, blob.data(), blob.size());
            check(restored, "tile snapshot: restore into a fresh world succeeds");
            if (restored) {
                const bool sameTable = a.tiles.Overrides().size() == b.tiles.Overrides().size()
                                       && std::equal(a.tiles.Overrides().begin(), a.tiles.Overrides().end(), b.tiles.Overrides().begin(),
                                                     [](const TileOverride& x, const TileOverride& y) {
                                                         return x.entityIndex == y.entityIndex && x.entityGeneration == y.entityGeneration
                                                                && x.layer == y.layer && x.cellKey == y.cellKey && x.value == y.value;
                                                     });
                check(sameTable, "tile snapshot: the restored override table equals the captured one");
                check(HashWorld(a.GetWorld(), a.Refs().HashSources()) == HashWorld(b.GetWorld(), refsB.HashSources()),
                      "tile snapshot: the world hash right after restore equals the captured one");
                b.mapEntity = a.mapEntity;
                bool allMatch = true;
                for (int tick = 50; tick < 150; ++tick) {
                    ApplyScenarioStimulus(a, actorsA, tick);
                    ApplyScenarioStimulus(b, actorsA, tick);
                    a.Step(1);
                    b.Step(1);
                    const uint64_t ha = HashWorld(a.GetWorld(), a.Refs().HashSources());
                    const uint64_t hb = HashWorld(b.GetWorld(), refsB.HashSources());
                    if (ha != hb && allMatch) {
                        allMatch = false;
                        MYE_LOG_ERROR("  tile snapshot diverged at tick %d: %016llX vs %016llX", tick,
                                      static_cast<unsigned long long>(ha), static_cast<unsigned long long>(hb));
                    }
                }
                check(allMatch, "tile snapshot: 100 ticks after restore (fresh collision cache) match the uninterrupted run tick by tick");
                check(a.GetWorld().GetComponent<CharacterControllerComponent>(actorsA.walker)->groundEntity
                          == b.GetWorld().GetComponent<CharacterControllerComponent>(actorsA.walker)->groundEntity,
                      "tile snapshot: the character stands on the same ground after the replay");
            }

            // 壊れた blob: 表の並びが昇順でない / レイヤーが範囲外
            {
                std::vector<std::byte> bad;
                ByteWriter w(bad);
                w.Count(2);
                for (uint32_t i = 0; i < 2; ++i) { // 2 件目が 1 件目より小さい
                    w.U32(1 - i);
                    w.U32(0);
                    w.U32(0);
                    w.U32(100);
                    w.U32(7);
                }
                ByteReader r(bad.data(), bad.size());
                TilemapSnapshot snap;
                check(!TilemapSystem::ReadSnapshot(r, snap), "tile snapshot: a table that is not strictly ascending is rejected");
                std::vector<std::byte> badLayer;
                ByteWriter w2(badLayer);
                w2.Count(1);
                w2.U32(1);
                w2.U32(0);
                w2.U32(static_cast<uint32_t>(kMaxTilemapLayers));
                w2.U32(100);
                w2.U32(7);
                ByteReader r2(badLayer.data(), badLayer.size());
                check(!TilemapSystem::ReadSnapshot(r2, snap), "tile snapshot: a layer past the limit is rejected");
            }
        }

        // ---- 死んだ持ち主の書き換えは tick 末に捨てられる ----
        {
            TileSim sim;
            AddMap(sim, mapId, 0.0f, 0.0f, 0.0f);
            sim.GetWorld().ApplyStructuralChanges();
            sim.tiles.SetCell(sim.GetWorld(), sim.mapEntity, 0, 1, 1, tilecell::Make(kTileFloor, false, false, 0));
            check(sim.tiles.HasLiveState(), "tile edit: one override makes the table hashable");
            sim.GetWorld().DestroyEntity(sim.mapEntity);
            sim.GetWorld().ApplyStructuralChanges();
            sim.tiles.Sweep(sim.GetWorld());
            check(!sim.tiles.HasLiveState(), "tile edit: Sweep drops the overrides of a destroyed Tilemap entity");
        }

        // ---- 破壊: hp 3 のタイルは 3 回で壊れ、エフェクトが 1 個生成される ----
        {
            TileSim sim;
            AddPhysicsEnvironment(sim.scene);
            AddMap(sim, mapId, 0.0f, 0.0f, 0.0f);
            sim.GetWorld().ApplyStructuralChanges();
            sim.transforms.Update(sim.GetWorld());
            World& w = sim.GetWorld();
            const EntityID map = sim.mapEntity;
            const auto crateValue = tilecell::Make(kTileCrate, false, false, 0);
            check(sim.tiles.GetCell(w, map, 0, 15, 1) == crateValue, "tile damage: the crate starts undamaged");
            const TileDamageResult r1 = sim.tiles.DamageCell(w, map, 0, 15, 1, 1);
            const uint32_t after1 = sim.tiles.GetCell(w, map, 0, 15, 1);
            const TileDamageResult r2 = sim.tiles.DamageCell(w, map, 0, 15, 1, 1);
            const uint32_t after2 = sim.tiles.GetCell(w, map, 0, 15, 1);
            check(r1 == TileDamageResult::kDamaged && tilecell::TileId(after1) == kTileCrate && tilecell::Damage(after1) == 1
                      && r2 == TileDamageResult::kDamaged && tilecell::Damage(after2) == 2,
                  "tile damage: the first two hits add damage 1 and 2 and keep the tile");
            std::vector<TileEffectRequest> early;
            sim.tiles.DrainEffects(early);
            check(early.empty(), "tile damage: no effect before the tile breaks");
            const TileDamageResult r3 = sim.tiles.DamageCell(w, map, 0, 15, 1, 1);
            check(r3 == TileDamageResult::kDestroyed && sim.tiles.GetCell(w, map, 0, 15, 1) == kTileRubble,
                  "tile damage: the third hit replaces the crate with destroyedTile (damage 0)");
            check(sim.tiles.DamageCell(w, map, 0, 15, 1, 1) == TileDamageResult::kNone
                      && sim.tiles.DamageCell(w, map, 0, 3, 0, 5) == TileDamageResult::kNone
                      && sim.tiles.DamageCell(w, map, 0, 15, 1, 0) == TileDamageResult::kNone,
                  "tile damage: rubble / an unbreakable floor / amount 0 do nothing");
            std::vector<TileEffectRequest> fx;
            sim.tiles.DrainEffects(fx);
            check(fx.size() == 1 && fx[0].prefabHash == effectHash && fx[0].pos.x == 15.5f && fx[0].pos.y == 1.5f && fx[0].pos.z == 0.0f,
                  "tile damage: exactly one effect request, at the cell center, with the palette's prefab");
            std::vector<TileEffectRequest> again;
            sim.tiles.DrainEffects(again);
            check(again.empty(), "tile damage: draining empties the queue");
            if (fx.size() == 1) {
                EffectSpawnRequest req;
                req.prefabHash = fx[0].prefabHash;
                req.pos = { fx[0].pos.x, fx[0].pos.y, fx[0].pos.z };
                const uint64_t rootFid = SpawnEffectRequest(sim.scene, prefabs, L"", req);
                GameObject spawned = (rootFid != 0) ? sim.scene.FindByFileId(rootFid) : GameObject();
                check(rootFid != 0 && spawned && std::string(spawned.Name()) == "RubbleFx"
                          && spawned.GetComponent<LocalTransform>()->position.x == 15.5f
                          && spawned.GetComponent<LocalTransform>()->position.y == 1.5f,
                      "tile damage: the effect request spawns the prefab at the cell center");
            }
            // 1 発で届く大きな amount も壊れる (damage は 8 bit で飽和する)
            sim.tiles.SetCell(w, map, 0, 16, 1, crateValue);
            check(sim.tiles.DamageCell(w, map, 0, 16, 1, 100000) == TileDamageResult::kDestroyed,
                  "tile damage: a hit larger than hp breaks the tile at once");
            check(sim.AllBoxCount() == 5, "tile damage: the broken crates no longer collide (5 boxes left)");
        }

        // ---- Tilemap の無いシーンは着手前と同一 (HashWorld と snapshot のバイト列) ----
        {
            const BareSceneResult bare = RunBareScene();
            MYE_LOG_INFO("  [tilemap] bare scene: HashWorld %016llX / snapshot digest %016llX (%zu bytes without version and TM2D)",
                         static_cast<unsigned long long>(bare.worldHash), static_cast<unsigned long long>(bare.blobDigest),
                         bare.blobSize);
            // 欄の追加 (M95 sub-05) で再採取した値。Debug == Release。
            // blob は手触りの合流 (snapshot v69: Scene 節に TimeControl の 6 語・FeelState・先行入力の窓) で再採取した。
            // WorldHash は手触りが内容ゲートなので不変
            constexpr uint64_t kBareWorldHash = 0x01DCBAE1E3D7C026ull;
            constexpr uint64_t kBareBlobDigest = 0xA70D1424062E91CCull;
            check(bare.worldHash == kBareWorldHash, "tile gate: the world hash of a scene without a Tilemap equals the pre-M94k value");
            check(bare.sectionStripped && bare.blobDigest == kBareBlobDigest,
                  "tile gate: the snapshot bytes equal the pre-M94k blob plus the version word and one empty TM2D section");
            // 空の TilemapSystem を配線しても畳まれない (内容ゲート)
            Scene scene;
            TilemapSystem empty;
            const uint64_t unwired = HashWorld(scene.GetWorld(), SimSourcesOf(scene, nullptr, nullptr, nullptr));
            const uint64_t wired = HashWorld(scene.GetWorld(), SimSourcesOf(scene, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &empty));
            check(unwired == wired, "tile gate: wiring an empty TilemapSystem does not move the world hash");
        }

        // ---- 2 ワールドの並走: 毎 tick の HashWorld が一致する ----
        {
            TileSim a;
            TileSim b;
            const ScenarioActors actorsA = BuildScenario(a, mapId);
            const ScenarioActors actorsB = BuildScenario(b, mapId);
            bool deterministic = true;
            uint64_t finalHash = 0;
            constexpr int kTicks = 600;
            for (int tick = 0; tick < kTicks; ++tick) {
                ApplyScenarioStimulus(a, actorsA, tick);
                ApplyScenarioStimulus(b, actorsB, tick);
                a.Step(1);
                b.Step(1);
                const uint64_t ha = HashWorld(a.GetWorld(), a.Refs().HashSources());
                const uint64_t hb = HashWorld(b.GetWorld(), b.Refs().HashSources());
                if (ha != hb && deterministic) {
                    deterministic = false;
                    MYE_LOG_ERROR("  tile determinism diverged at tick %d: %016llX vs %016llX", tick,
                                  static_cast<unsigned long long>(ha), static_cast<unsigned long long>(hb));
                }
                finalHash = ha;
            }
            check(deterministic, "tile determinism: two identical worlds hash-identical for 600 ticks");
            MYE_LOG_INFO("  [tilemap] scenario: roller x %.3f y %.3f / walker y %.3f / hash @600 = %016llX",
                         static_cast<double>(a.GetWorld().GetComponent<LocalTransform>(actorsA.roller)->position.x),
                         static_cast<double>(a.GetWorld().GetComponent<LocalTransform>(actorsA.roller)->position.y),
                         static_cast<double>(a.GetWorld().GetComponent<LocalTransform>(actorsA.walker)->position.y),
                         static_cast<unsigned long long>(finalHash));
            // Debug / Release 共通の期待値。0 は「未採取」 (採取値をログから読んで焼く)
            constexpr uint64_t kTileScenarioExpectedHash = 0x6E40334DD04B0FFCull;
            check(kTileScenarioExpectedHash == 0 || finalHash == kTileScenarioExpectedHash,
                  "tile determinism: the final world hash matches the baked value (Debug == Release)");
        }

        // ---- 動かす物体の範囲による絞り込み: 結果は絞らない場合と同じで、遠いチャンクの箱は渡らない ----
        {
            TileSim culled(true);
            TileSim full(false);
            const ScenarioActors actorsC = BuildScenario(culled, mapId);
            const ScenarioActors actorsF = BuildScenario(full, mapId);
            // 床から遠い位置にも箱がある状態にして、絞り込みの差が出るようにする (x = 90 の遠いブロック)
            culled.tiles.SetCell(culled.GetWorld(), culled.mapEntity, 0, 90, 5, tilecell::Make(kTileFloor, false, false, 0));
            full.tiles.SetCell(full.GetWorld(), full.mapEntity, 0, 90, 5, tilecell::Make(kTileFloor, false, false, 0));
            bool same = true;
            size_t minCulled = static_cast<size_t>(-1);
            for (int tick = 0; tick < 400; ++tick) {
                ApplyScenarioStimulus(culled, actorsC, tick);
                ApplyScenarioStimulus(full, actorsF, tick);
                culled.Step(1);
                full.Step(1);
                minCulled = (std::min)(minCulled, culled.tiles.LastCollectBoxCount());
                if (HashWorld(culled.GetWorld(), culled.Refs().HashSources()) != HashWorld(full.GetWorld(), full.Refs().HashSources())
                    && same) {
                    same = false;
                    MYE_LOG_ERROR("  tile culling changed the outcome at tick %d", tick);
                }
            }
            check(same, "tile culling: 400 ticks with the culled boxes hash-identical to the same scene given every box");
            check(minCulled < full.tiles.LastCollectBoxCount() && full.tiles.LastCollectBoxCount() == culled.AllBoxCount(),
                  "tile culling: the far block (x = 90) never reaches the physics while the actors are near x = 0..16");
        }

        // ---- 計測: 100 x 50 の床の箱数と、物理 1 tick の時間 (--physics-demo のシーンと比べる) ----
        {
            TilemapAsset big;
            big.palette = paletteHash;
            TilemapLayer ground;
            ground.name = "Ground";
            for (int32_t y = 0; y < 50; ++y) {
                for (int32_t x = 0; x < 100; ++x) {
                    ground.SetCell(x, y, tilecell::Make(kTileFloor, false, false, 0));
                }
            }
            big.layers.push_back(std::move(ground));
            const AssetID bigId{ maps.Register(L"C:\\sel\\tilemap\\big.tilemap.json", std::move(big)) };

            using Clock = std::chrono::steady_clock;
            constexpr int kTicks = 300;
            TileSim sim;
            AddPhysicsEnvironment(sim.scene);
            AddMap(sim, bigId, 0.0f, 0.0f, 0.0f);
            for (int i = 0; i < 20; ++i) {
                AddBall(sim.scene, "Ball", 3.0f + 4.5f * static_cast<float>(i), 52.0f + 0.1f * static_cast<float>(i % 3));
            }
            sim.GetWorld().ApplyStructuralChanges();
            sim.transforms.Update(sim.GetWorld());
            double tileMs = 0.0;
            for (int tick = 0; tick < kTicks; ++tick) {
                const Clock::time_point t0 = Clock::now();
                sim.phys.Update(sim.GetWorld(), kDt);
                tileMs += std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
                sim.transforms.Update(sim.GetWorld());
            }
            const size_t culledBoxes = sim.tiles.LastCollectBoxCount();
            const size_t boxes = sim.AllBoxCount();

            // 対照: 同じ 20 球を、1 枚の箱コライダーの床 (100 x 50) に落とす。タイルの箱が増やす分だけを切り出す
            double flatMs = 0.0;
            {
                Scene flat;
                AddPhysicsEnvironment(flat);
                GameObject floor = flat.CreateGameObjectTracked("Floor");
                floor.SetLocalPosition(50.0f, 25.0f, 0.0f);
                auto* floorCol = floor.AddComponent<ColliderComponent>();
                floorCol->shape = collidershape::kBox;
                floorCol->isTrigger = false;
                floorCol->halfExtents = { 50.0f, 25.0f, 0.5f };
                for (int i = 0; i < 20; ++i) {
                    AddBall(flat, "Ball", 3.0f + 4.5f * static_cast<float>(i), 52.0f + 0.1f * static_cast<float>(i % 3));
                }
                flat.GetWorld().ApplyStructuralChanges();
                PhysicsSystem flatPhys;
                for (int tick = 0; tick < kTicks; ++tick) {
                    const Clock::time_point t0 = Clock::now();
                    flatPhys.Update(flat.GetWorld(), kDt);
                    flatMs += std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
                }
            }

            // 基準: --physics-demo のシーン (BuildPhysicsShowcaseScene)。同じ tick 数で測る
            RenderResources resources;
            resources.InitHeadless();
            Scene demoScene;
            EngineContext ctx;
            ctx.scene = &demoScene;
            ctx.resources = &resources;
            BuildPhysicsShowcaseScene(ctx);
            demoScene.GetWorld().ApplyStructuralChanges();
            PhysicsSystem demoPhys;
            double demoMs = 0.0;
            for (int tick = 0; tick < kTicks; ++tick) {
                const Clock::time_point t0 = Clock::now();
                demoPhys.Update(demoScene.GetWorld(), kDt);
                demoMs += std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
            }
            const double tileAvg = tileMs / kTicks;
            const double demoAvg = demoMs / kTicks;
            MYE_LOG_INFO("  [tilemap] 100x50 floor: %zu boxes (%zu after culling) / physics %.3f ms per tick with 20 balls (one-box floor %.3f) vs physics-demo %.3f ms per tick (ratio %.2f)",
                         boxes, culledBoxes, tileAvg, flatMs / kTicks, demoAvg, demoAvg > 0.0 ? tileAvg / demoAvg : 0.0);
            check(culledBoxes > 0 && culledBoxes < boxes / 2,
                  "tile collision: the physics receives only the chunks near the 20 balls (culled to under half)");
            check(boxes == 350, "tile collision: a 100 x 50 filled floor becomes 350 row-merged boxes (50 rows x 7 chunk columns)");
        }

        tilepalette::Install(previousPalettes);
        tilemap::Install(previousMaps);
    }

    // ---- ルールタイルとアニメーションタイル (M94m) ----
    {
        constexpr uint32_t kGround = 1;
        constexpr uint32_t kWaterId = 2;
        const AssetID defaultSprite{ 100 };
        const AssetID leftSprite{ 101 };
        const AssetID midSprite{ 102 };
        const AssetID rightSprite{ 103 };
        const AssetID singleSprite{ 104 };
        TilePalette palette;
        {
            TileDef& ground = *palette.Add("ground", defaultSprite);
            ground.hasRule = true;
            // 左右 (W E) だけを見る地面の縁。上の行ほど優先
            ground.variants = { { "...ox...", leftSprite }, { "...xx...", midSprite }, { "...xo...", rightSprite },
                                { "...oo...", singleSprite } };
            TileDef& water = *palette.Add("water", AssetID{ 200 });
            water.hasAnim = true;
            water.ticksPerFrame = 8;
            water.animFrames = { AssetID{ 201 }, AssetID{ 202 }, AssetID{ 203 } };
            water.hasRule = true; // anim が優先される
            water.variants = { { "........", AssetID{ 299 } } };
        }

        // 同じレイヤーの 1 行 (y = 5) に地面を並べ、(x, y) の隣接を 3x3 チャンクの実効セルから読んで変種を引く
        TilemapLayer layer;
        for (int32_t x : { 0, 2, 3, 4, 15, 16, 17 }) {
            layer.SetCell(x, 5, tilecell::Make(kGround, false, false, 0));
        }
        layer.SetCell(3, 6, tilecell::Make(kWaterId, false, false, 0)); // 真上に別種
        auto aroundOf = [](const TilemapLayer& l, int32_t chunkX, int32_t chunkY) {
            Sprite2DRenderer::TileChunkAround a{};
            for (int32_t dy = -1; dy <= 1; ++dy) {
                for (int32_t dx = -1; dx <= 1; ++dx) {
                    const auto it = l.chunks.find(ChunkKey{ chunkY + dy, chunkX + dx });
                    if (it != l.chunks.end()) {
                        a[static_cast<size_t>((dy + 1) * 3 + (dx + 1))] = it->second.cells;
                    }
                }
            }
            return a;
        };
        auto spriteOf = [&](int32_t x, int32_t y) {
            const int32_t cx = TilemapFloorDiv(x, kTilemapChunkSize);
            const int32_t cy = TilemapFloorDiv(y, kTilemapChunkSize);
            const TileNeighbors n = Sprite2DRenderer::TileNeighborsAt(
                aroundOf(layer, cx, cy), TilemapFloorMod(x, kTilemapChunkSize), TilemapFloorMod(y, kTilemapChunkSize));
            return ResolveRuleVariant(palette, tilecell::TileId(layer.GetCell(x, y)), n).value;
        };

        // (a) 縁 4 種
        check(spriteOf(0, 5) == singleSprite.value, "rule tile: an isolated tile takes the single variant");
        check(spriteOf(2, 5) == leftSprite.value, "rule tile: the left end of a row takes the left-edge variant");
        check(spriteOf(3, 5) == midSprite.value, "rule tile: the middle of a row takes the middle variant");
        check(spriteOf(4, 5) == rightSprite.value, "rule tile: the right end of a row takes the right-edge variant");

        // 先頭から順に合うものを採る / 合わなければ既定の sprite / 未登録 id は null
        {
            TilePalette p2 = palette;
            TileDef* g = const_cast<TileDef*>(p2.Find(kGround));
            const TileNeighbors none{};
            g->variants = { { "........", AssetID{ 111 } }, { "...xx...", AssetID{ 112 } } };
            check(ResolveRuleVariant(p2, kGround, none).value == 111, "rule tile: the first matching variant wins (any-pattern first)");
            g->variants = { { "xxxxxxxx", AssetID{ 113 } } };
            check(ResolveRuleVariant(p2, kGround, none) == defaultSprite, "rule tile: no matching variant falls back to the tile sprite");
            TileNeighbors all;
            all.fill(kGround);
            check(ResolveRuleVariant(p2, kGround, all).value == 113, "rule tile: x needs the same tile id in all 8 neighbors");
            all[7] = tilecell::Make(kGround, true, false, 1, 3); // flip / 回転 / damage ビットは同種判定に影響しない
            check(ResolveRuleVariant(p2, kGround, all).value == 113, "rule tile: flip / rotation / damage bits do not affect same-tile matching");
            all[7] = kWaterId;
            check(ResolveRuleVariant(p2, kGround, all) == defaultSprite, "rule tile: a different tile id does not satisfy x");
            check(ResolveRuleVariant(p2, 99, none).IsNull(), "rule tile: an unknown tile id has no sprite");
        }

        // (b) チャンク境界: セル 15 の東はチャンク (0, 1) のセル 16 (西の 14 は空なので、隣を読めていれば左端、読めていなければ単独)、
        // セル 16 の西はチャンク (0, 0) のセル 15
        check(spriteOf(15, 5) == leftSprite.value,
              "rule tile: the east neighbor across the chunk border (cell 16) is read from the next chunk");
        check(spriteOf(16, 5) == midSprite.value,
              "rule tile: the west neighbor across the chunk border (cell 15) is read from the previous chunk");
        check(spriteOf(17, 5) == rightSprite.value, "rule tile: the end of a row in the next chunk takes the right-edge variant");
        {
            // 対角: チャンク (0, 0) の角 (15, 15) の NE はチャンク (1, 1) の (0, 0) = セル (16, 16)
            TilemapLayer corner;
            corner.SetCell(16, 16, tilecell::Make(kGround, false, false, 0));
            corner.SetCell(15, 15, tilecell::Make(kGround, false, false, 0));
            const TileNeighbors n = Sprite2DRenderer::TileNeighborsAt(aroundOf(corner, 0, 0), 15, 15);
            check(n[2] == kGround && n[0] == 0 && n[1] == 0 && n[3] == 0 && n[4] == 0 && n[5] == 0 && n[6] == 0 && n[7] == 0,
                  "rule tile: the NE neighbor at a chunk corner comes from the diagonal chunk (order NW N NE W E SW S SE)");
            const TileNeighbors sw = Sprite2DRenderer::TileNeighborsAt(aroundOf(corner, 1, 1), 0, 0);
            check(sw[5] == kGround && sw[0] == 0 && sw[7] == 0 && sw[2] == 0,
                  "rule tile: the SW neighbor at the opposite corner comes from chunk (0, 0)");
        }

        // (c) アニメ: (tick / ticksPerFrame) % frames。tick 8 でコマが進み、24 で戻る
        const TileDef* water = palette.Find(kWaterId);
        const TileNeighbors none{};
        auto animSprite = [&](uint64_t tick) { return ResolveTileSprite(palette, *water, none, tick).value; };
        check(animSprite(0) == 201 && animSprite(7) == 201, "anim tile: frame 0 for ticks 0..7");
        check(animSprite(8) == 202 && animSprite(15) == 202, "anim tile: the frame advances at tick 8");
        check(animSprite(16) == 203 && animSprite(23) == 203, "anim tile: frame 2 for ticks 16..23");
        check(animSprite(24) == 201 && animSprite(24 * 1000 + 8) == 202, "anim tile: it loops with the frame count");
        check(TileAnimFrameIndex(8, 0, 100) == -1, "anim tile: no frames has no frame index");
        check(TileAnimFrameIndex(0, 3, 5) == 2, "anim tile: ticksPerFrame below 1 is treated as 1");
        {
            TilePalette p3 = palette;
            TileDef* w = const_cast<TileDef*>(p3.Find(kWaterId));
            w->animFrames.clear();
            check(ResolveTileSprite(p3, *w, none, 8).value == 299, "anim tile: an empty frame list falls through to the rule / sprite");
        }

        // 往復 (rule / anim は sub-10 で読み書き済み): 変種の順序とコマ列が保たれる
        {
            TilePalette back;
            check(TilePaletteLibrary::FromJson(TilePaletteLibrary::ToJson(palette), back)
                      && back.Find(kGround)->variants.size() == 4 && back.Find(kGround)->variants[3].neighbors == "...oo..."
                      && back.Find(kWaterId)->animFrames.size() == 3 && back.Find(kWaterId)->animFrames[2].value == 203,
                  "palette round trip keeps the variant order and the frame list");
        }
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
