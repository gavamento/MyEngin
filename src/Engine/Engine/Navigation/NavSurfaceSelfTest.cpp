//====================================================================================
//                          NavSurfaceSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                     10/03/2026
//                                          NavMeshSurface のベイク・.mnav・読み込みの回帰テスト実装
//====================================================================================
#include "Engine/Engine/Navigation/NavSurfaceSelfTest.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "Engine/Core/Asset/AssetGuidResolver.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Navigation/NavBake.h"
#include "Engine/Engine/Physics/Collider/ConvexColliderLibrary.h"
#include "Engine/Engine/Physics/Collider/ConvexHull.h"
#include "Engine/Engine/Physics/Collider/MeshColliderLibrary.h"
#include "Engine/Engine/Physics/Collider/TerrainColliderLibrary.h"
#include "Engine/Engine/Navigation/NavMeshAsset.h"
#include "Engine/Engine/Navigation/NavSystem.h"
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Engine/Scene/SceneSerializer.h"
#include "Engine/Engine/Scene/TransformSystem.h"
#include "Engine/Platform/PathUtil.h"

namespace fs = std::filesystem;

namespace mye {
namespace {

// 期待値は Debug で採取し、Release で同じ値になることを確認して焼く (docs\adr\ADR-023-navmesh.md)
constexpr uint64_t kExpectedAssetHash = 0xA9EF6D223C161FE4ull;
constexpr uint64_t kTestAssetGuid = 0x4E41564D45534831ull; // "NAVMESH1"

struct Checker {
    int failCount = 0;
    void Check(bool cond, const char* what)
    {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    }
};

void AddBox(Scene& scene, const char* name, float x, float y, float z, float hx, float hy, float hz,
            bool trigger, bool dynamic)
{
    GameObject go = scene.CreateGameObjectTracked(name);
    go.SetLocalPosition(x, y, z);
    auto* col = go.AddComponent<ColliderComponent>();
    col->shape = collidershape::kBox;
    col->halfExtents = { hx, hy, hz };
    col->isTrigger = trigger;
    if (dynamic) {
        go.AddComponent<RigidbodyComponent>();
    }
}

// 床 + 箱 + 球 + カプセル。includeExcluded なら Rigidbody 持ちとトリガーも置く (入力に入らないはず)
EntityID BuildScene(Scene& scene, bool includeExcluded)
{
    AddBox(scene, "Ground", 0.0f, -0.5f, 0.0f, 9.5f, 0.5f, 9.5f, false, false);
    AddBox(scene, "Block", 3.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, false, false);
    {
        GameObject sphere = scene.CreateGameObjectTracked("Sphere");
        sphere.SetLocalPosition(-4.0f, 0.5f, -3.0f);
        auto* col = sphere.AddComponent<ColliderComponent>();
        col->shape = collidershape::kSphere;
        col->radius = 1.0f;
    }
    {
        GameObject capsule = scene.CreateGameObjectTracked("Capsule");
        capsule.SetLocalPosition(-4.0f, 1.0f, 4.0f);
        auto* col = capsule.AddComponent<ColliderComponent>();
        col->shape = collidershape::kCapsule;
        col->radius = 0.5f;
        col->height = 2.0f;
    }
    if (includeExcluded) {
        AddBox(scene, "DynamicBox", 0.0f, 3.0f, 5.0f, 1.0f, 1.0f, 1.0f, false, true);
        AddBox(scene, "TriggerBox", -3.0f, 1.0f, 4.0f, 1.0f, 1.0f, 1.0f, true, false);
    }
    GameObject surface = scene.CreateGameObjectTracked("Surface");
    auto* comp = surface.AddComponent<NavMeshSurfaceComponent>();
    comp->center = { 0.0f, 1.0f, 0.0f };
    comp->size = { 20.0f, 6.0f, 20.0f };
    comp->cellSize = 0.5f;
    comp->cellHeight = 0.2f;
    comp->tileSize = 32;
    scene.GetWorld().ApplyStructuralChanges();
    TransformSystem transforms;
    transforms.Update(scene.GetWorld());
    return surface.Id();
}

uint64_t HashSoup(const NavTriangleSoup& soup)
{
    uint64_t h = kNavFnvSeed;
    h = NavFnv1a(h, soup.verts.data(), soup.verts.size() * sizeof(float));
    h = NavFnv1a(h, soup.tris.data(), soup.tris.size() * sizeof(int));
    return h;
}

uint64_t HashBytes(const std::vector<uint8_t>& bytes)
{
    return NavFnv1a(kNavFnvSeed, bytes.data(), bytes.size());
}

std::wstring ResolveTestAsset(void* user, uint64_t guid)
{
    const auto* path = static_cast<const std::wstring*>(user);
    return guid == kTestAssetGuid ? *path : std::wstring();
}

// 手前の点 -> 奥の点で、最近傍ポリゴンから経路が最後まで届くか
bool PathReaches(NavTileStore& store, const float* from, const float* to, bool* startOnMesh, bool* endOnMesh)
{
    dtNavMeshQuery* query = dtAllocNavMeshQuery();
    bool reached = false;
    if (query != nullptr && dtStatusSucceed(query->init(store.NavMesh(), 2048))) {
        dtQueryFilter filter;
        filter.setIncludeFlags(0xFFFF);
        const float extents[3] = { 0.6f, 1.0f, 0.6f };
        dtPolyRef a = 0;
        dtPolyRef b = 0;
        float na[3] = {};
        float nb[3] = {};
        query->findNearestPoly(from, extents, &filter, &a, na);
        query->findNearestPoly(to, extents, &filter, &b, nb);
        *startOnMesh = a != 0;
        *endOnMesh = b != 0;
        if (a != 0 && b != 0) {
            dtPolyRef path[256];
            int count = 0;
            query->findPath(a, b, na, nb, &filter, path, &count, 256);
            reached = count > 0 && path[count - 1] == b;
        }
    }
    dtFreeNavMeshQuery(query);
    return reached;
}

} // namespace

bool RunNavSurfaceSelfTest()
{
    MYE_LOG_INFO("==== NavSurface (bake input / .mnav / NavSystem) self test ====");
    Checker ck;

    Scene scene;
    const EntityID surfaceId = BuildScene(scene, true);
    World& world = scene.GetWorld();

    // ---- 1. 入力収集 ----
    NavBakeInputs inputs;
    ck.Check(NavPrepareBakeInputs(world, surfaceId, inputs), "Surface から入力を集められる");
    ck.Check(inputs.soup.TriangleCount() > 0, "三角形が集まる");
    NavBakeInputs again;
    NavPrepareBakeInputs(world, surfaceId, again);
    ck.Check(HashSoup(again.soup) == HashSoup(inputs.soup), "同じ World からは同じ三角形列");
    {
        // 動く物とトリガーを置かない World と同じ三角形になる = 入力に入っていない
        Scene plain;
        const EntityID plainSurface = BuildScene(plain, false);
        NavBakeInputs plainInputs;
        NavPrepareBakeInputs(plain.GetWorld(), plainSurface, plainInputs);
        ck.Check(HashSoup(plainInputs.soup) == HashSoup(inputs.soup),
                 "Rigidbody 持ちとトリガーは入力に入らない");
    }
    {
        // 範囲外のコライダーは捨てる: 遠くへ離した箱は三角形を増やさない
        Scene far;
        const EntityID farSurface = BuildScene(far, false);
        AddBox(far, "Far", 100.0f, 0.0f, 100.0f, 1.0f, 1.0f, 1.0f, false, false);
        far.GetWorld().ApplyStructuralChanges();
        TransformSystem transforms;
        transforms.Update(far.GetWorld());
        NavBakeInputs farInputs;
        NavPrepareBakeInputs(far.GetWorld(), farSurface, farInputs);
        ck.Check(farInputs.soup.TriangleCount() == inputs.soup.TriangleCount(), "範囲外のコライダーは入力に入らない");
    }

    // ---- 2. ベイク ----
    NavBakeControl control;
    const auto t0 = std::chrono::steady_clock::now();
    NavBakeOutput bake = NavBakeAsset(inputs.config, inputs.soup, &control);
    const auto t1 = std::chrono::steady_clock::now();
    ck.Check(bake.status == NavBakeStatus::Ok, "ベイクが成功する");
    ck.Check(control.tilesDone.load() == control.tilesTotal.load() && control.tilesTotal.load() > 0,
             "進捗がタイル総数まで進む");
    ck.Check(bake.polyCount > 0 && bake.data.layers.size() >= 4, "ポリゴンと、2 x 2 タイルぶんの層ができる");
    MYE_LOG_INFO("  タイル %dx%d、層 %zu 枚、ポリゴン %d、maxTiles %d、maxPolys %d、%.0f ms", bake.data.tilesX,
                 bake.data.tilesY, bake.data.layers.size(), bake.polyCount, bake.data.maxTiles,
                 bake.data.maxPolysPerTile, std::chrono::duration<double, std::milli>(t1 - t0).count());

    std::vector<uint8_t> bytes;
    NavMeshAsset::Serialize(bake.data, bytes);
    NavBakeOutput bakeAgain = NavBakeAsset(inputs.config, inputs.soup, nullptr);
    std::vector<uint8_t> bytesAgain;
    NavMeshAsset::Serialize(bakeAgain.data, bytesAgain);
    ck.Check(bytes == bytesAgain, "同じ入力のベイクは同じバイト列");
    const uint64_t assetHash = HashBytes(bytes);
    MYE_LOG_INFO("NavSurface: asset = 0x%016llX (%zu bytes)", static_cast<unsigned long long>(assetHash), bytes.size());
    {
        char msg[96];
        std::snprintf(msg, sizeof(msg), "asset ハッシュが期待値と一致");
        ck.Check(assetHash == kExpectedAssetHash, msg);
    }
    ck.Check(bake.data.inputHash == NavComputeInputHash(inputs.config, inputs.soup),
             "資産が入力ハッシュを持つ");

    // 層はタイル単位の関数の結果そのもの (実行時の再ベイクが同じ関数を呼べる)
    {
        bool sameLayers = true;
        size_t cursor = 0;
        for (int ty = 0; ty < bake.data.tilesY && sameLayers; ++ty) {
            for (int tx = 0; tx < bake.data.tilesX && sameLayers; ++tx) {
                std::vector<std::vector<uint8_t>> layers;
                sameLayers = NavBakeTile(inputs.config, inputs.soup, tx, ty, layers);
                for (const auto& blob : layers) {
                    sameLayers = sameLayers && cursor < bake.data.layers.size()
                        && bake.data.layers[cursor].blob == blob;
                    ++cursor;
                }
            }
        }
        ck.Check(sameLayers && cursor == bake.data.layers.size(), "各層は NavBakeTile を 1 枚ずつ呼んだ結果と一致する");
    }
    {
        // タイルごとの三角形の絞り込みは結果を変えない (全三角形を渡したベイクと同じバイト)
        bool sameAsUnfiltered = true;
        for (int ty = 0; ty < bake.data.tilesY && sameAsUnfiltered; ++ty) {
            for (int tx = 0; tx < bake.data.tilesX && sameAsUnfiltered; ++tx) {
                std::vector<std::vector<uint8_t>> filtered;
                std::vector<std::vector<uint8_t>> unfiltered;
                sameAsUnfiltered = NavBakeTile(inputs.config, inputs.soup, tx, ty, filtered)
                    && NavBakeTileLayers(inputs.config, inputs.soup.View(), tx, ty, unfiltered)
                    && filtered == unfiltered;
            }
        }
        ck.Check(sameAsUnfiltered, "タイルごとの三角形の絞り込みは、全三角形を渡したベイクと同じバイトになる");
    }

    // キャンセルと空
    {
        NavBakeControl cancel;
        cancel.cancel.store(true);
        ck.Check(NavBakeAsset(inputs.config, inputs.soup, &cancel).status == NavBakeStatus::Cancelled,
                 "キャンセルを受け付ける");
        NavTriangleSoup empty;
        ck.Check(NavBakeAsset(inputs.config, empty, nullptr).status == NavBakeStatus::Empty,
                 "三角形が無いと Empty (落ちない)");
    }

    // ---- 3. .mnav の読み書き ----
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / L"mye_navsurface_selftest";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const std::wstring mnavPath = (dir / L"test.mnav").wstring();
    ck.Check(NavMeshAsset::Save(mnavPath, bake.data), ".mnav を保存できる");
    NavMeshAsset::Data loaded;
    ck.Check(NavMeshAsset::Load(mnavPath, loaded), ".mnav を読める");
    std::vector<uint8_t> bytesLoaded;
    NavMeshAsset::Serialize(loaded, bytesLoaded);
    ck.Check(bytesLoaded == bytes, "書く -> 読む -> 書くでバイト一致");
    {
        NavMeshAsset::Data junk;
        std::vector<uint8_t> broken = bytes;
        broken.pop_back();
        ck.Check(!NavMeshAsset::Deserialize(broken, junk), "末尾が欠けた資産は読み込みに失敗する");
        broken = bytes;
        broken.push_back(0);
        ck.Check(!NavMeshAsset::Deserialize(broken, junk), "余りのある資産は読み込みに失敗する");
        broken = bytes;
        broken[0] ^= 0xFF;
        ck.Check(!NavMeshAsset::Deserialize(broken, junk), "magic が違う資産は読み込みに失敗する");
        broken.assign(bytes.begin(), bytes.begin() + 7);
        ck.Check(!NavMeshAsset::Deserialize(broken, junk), "短すぎる資産は読み込みに失敗する");
        broken.clear();
        ck.Check(!NavMeshAsset::Deserialize(broken, junk), "空の資産は読み込みに失敗する");
        NavMeshAsset::Data bad = bake.data;
        bad.layers[0].tx += 1; // header と層のキーが食い違う
        std::vector<uint8_t> badBytes;
        NavMeshAsset::Serialize(bad, badBytes);
        ck.Check(!NavMeshAsset::Deserialize(badBytes, junk), "層のキーと header が食い違う資産は失敗する");
        ck.Check(!NavMeshAsset::Load((dir / L"missing.mnav").wstring(), junk), "存在しないファイルは失敗する");
    }

    // ---- 4. 読み込んだ NavMesh の経路 ----
    {
        NavTileStore store;
        ck.Check(NavMeshAsset::BuildStore(loaded, store), ".mnav から dtNavMesh を組める");
        ck.Check(store.SaltBits() >= 10, "ref の salt が 10 ビット以上残る");
        const float west[3] = { -6.0f, 0.0f, 6.0f };
        const float east[3] = { 6.0f, 0.0f, -6.0f };
        const float insideBlock[3] = { 3.0f, 0.0f, 0.0f };
        bool onA = false;
        bool onB = false;
        ck.Check(PathReaches(store, west, east, &onA, &onB) && onA && onB, "床の上の 2 点の経路が最後まで届く");
        bool onC = true;
        bool onD = false;
        PathReaches(store, insideBlock, east, &onC, &onD);
        ck.Check(!onC, "箱の内側 (床が塞がれている点) は NavMesh に乗らない");
    }

    // ---- 4a. 形状ごとの入力収集 (mesh / convex / terrain) ----
    {
        MeshColliderLibrary meshLib;
        ConvexColliderLibrary convexLib;
        TerrainColliderLibrary terrainLib;
        meshcol::Install(&meshLib);
        convexcol::Install(&convexLib);
        terraincol::Install(&terrainLib);

        constexpr uint64_t kMeshId = 0x101;
        constexpr uint64_t kConvexId = 0x102;
        constexpr uint64_t kTerrainId = 0x103;
        {
            // 6 x 6 m の床 1 枚 (法線 +Y)
            const std::vector<DirectX::XMFLOAT3> positions = { { -3, 0, -3 }, { 3, 0, -3 }, { 3, 0, 3 }, { -3, 0, 3 } };
            const std::vector<uint32_t> indices = { 0, 2, 1, 0, 3, 2 };
            MeshColliderData data;
            BuildMeshColliderData(positions, indices, data);
            meshLib.Register(AssetID{ kMeshId }, std::move(data));

            std::vector<DirectX::XMFLOAT3> corners;
            for (int i = 0; i < 8; ++i) {
                corners.push_back({ (i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f, (i & 4) ? 1.0f : -1.0f });
            }
            ConvexHullData hull;
            BuildConvexHull(corners, hull);
            convexLib.Register(AssetID{ kConvexId }, std::move(hull));

            TerrainAsset::TerrainData terrain;
            terrain.heightW = 3;
            terrain.heightH = 3;
            terrain.worldSizeX = 6.0f;
            terrain.worldSizeZ = 6.0f;
            terrain.heightBase = 0.0f;
            terrain.heightScale = 0.0f;
            terrain.heights.assign(9, 0);
            terrainLib.Register(AssetID{ kTerrainId }, std::move(terrain));
        }

        struct ShapeCase {
            const char* name;
            int32_t shape;
            uint64_t asset;
            float y;
            float scale;
            int minTris;
            int maxTris;
        };
        const ShapeCase cases[] = {
            { "mesh", collidershape::kMesh, kMeshId, 0.0f, 1.0f, 2, 2 },
            { "convex", collidershape::kConvex, kConvexId, -1.0f, 4.0f, 12, 12 },
            { "terrain", collidershape::kTerrain, kTerrainId, 0.0f, 1.0f, 8, 8 },
        };
        for (const ShapeCase& c : cases) {
            Scene shapeScene;
            GameObject go = shapeScene.CreateGameObjectTracked("Shape");
            go.SetLocalPosition(0.0f, c.y, 0.0f);
            go.SetLocalScale(c.scale, c.shape == collidershape::kConvex ? 1.0f : c.scale, c.scale);
            auto* col = go.AddComponent<ColliderComponent>();
            col->shape = c.shape;
            col->meshAsset = AssetID{ c.asset };
            GameObject sf = shapeScene.CreateGameObjectTracked("Surface");
            auto* surf = sf.AddComponent<NavMeshSurfaceComponent>();
            surf->size = { 14.0f, 4.0f, 14.0f };
            surf->cellSize = 0.25f;
            surf->cellHeight = 0.1f;
            surf->tileSize = 64;
            shapeScene.GetWorld().ApplyStructuralChanges();
            TransformSystem tf;
            tf.Update(shapeScene.GetWorld());
            NavBakeInputs in;
            NavPrepareBakeInputs(shapeScene.GetWorld(), sf.Id(), in);
            char msg[128];
            std::snprintf(msg, sizeof(msg), "%s コライダー: 三角形 %d 枚が入力に入る (期待 %d..%d)", c.name,
                          in.soup.TriangleCount(), c.minTris, c.maxTris);
            ck.Check(in.soup.TriangleCount() >= c.minTris && in.soup.TriangleCount() <= c.maxTris, msg);
            const NavBakeOutput out = NavBakeAsset(in.config, in.soup, nullptr);
            std::snprintf(msg, sizeof(msg), "%s コライダー: 上面が歩ける面としてベイクされる (ポリゴン %d)", c.name,
                          out.polyCount);
            ck.Check(out.status == NavBakeStatus::Ok && out.polyCount > 0, msg);
        }
        meshcol::Install(nullptr);
        convexcol::Install(nullptr);
        terraincol::Install(nullptr);
    }

    // ---- 4b. コンポーネントの保存・ハッシュ ----
    {
        Scene saved;
        const EntityID savedSurface = BuildScene(saved, false);
        auto* sc = saved.GetWorld().GetComponent<NavMeshSurfaceComponent>(savedSurface);
        sc->navAsset = AssetID{ kTestAssetGuid };
        sc->agentRadius = 0.45f;
        sc->areaCosts[7] = 3.5f;
        sc->collectLayerMask = 0x0000FF0Fu;
        const std::wstring scenePath = (dir / L"surface.scene.json").wstring();
        ck.Check(SceneSerializer::SaveToFile(saved, scenePath), "Surface を含むシーンを保存できる");
        Scene restored;
        ck.Check(SceneSerializer::LoadFromFile(restored, scenePath), "保存したシーンを読める");
        GameObject back = restored.Find("Surface");
        const auto* rc = back ? restored.GetWorld().GetComponent<NavMeshSurfaceComponent>(back.Id()) : nullptr;
        ck.Check(rc != nullptr && rc->navAsset.value == kTestAssetGuid && rc->agentRadius == 0.45f
                     && rc->areaCosts[7] == 3.5f && rc->areaCosts[0] == 1.0f && rc->collectLayerMask == 0x0000FF0Fu
                     && rc->size.x == 20.0f && rc->tileSize == 32,
                 "保存 -> 読み込みで設定値・navAsset・エリアコストが戻る");

        // 表示フラグはハッシュ対象外、設定値と navAsset は対象
        const uint64_t base = HashWorld(saved.GetWorld());
        sc = saved.GetWorld().GetComponent<NavMeshSurfaceComponent>(savedSurface);
        sc->drawNavMesh = !sc->drawNavMesh;
        sc->drawTileBounds = !sc->drawTileBounds;
        ck.Check(HashWorld(saved.GetWorld()) == base, "表示フラグを変えてもワールドのハッシュは変わらない");
        sc->navAsset = AssetID{};
        ck.Check(HashWorld(saved.GetWorld()) != base, "navAsset を変えるとワールドのハッシュが変わる");
        sc->navAsset = AssetID{ kTestAssetGuid };
        sc->areaCosts[3] = 2.0f;
        ck.Check(HashWorld(saved.GetWorld()) != base, "エリアコストを変えるとワールドのハッシュが変わる");
    }

    // ---- 5. NavSystem ----
    {
        std::wstring pathHolder = mnavPath;
        assetguid::Install(&ResolveTestAsset, &pathHolder);
        Scene runtimeScene;
        const EntityID rtSurface = BuildScene(runtimeScene, false);
        World& rtWorld = runtimeScene.GetWorld();
        NavSystem nav;
        nav.Update(rtWorld);
        ck.Check(nav.Surfaces().size() == 1 && nav.Surfaces()[0].state == NavSurfaceState::NoAsset,
                 "navAsset が未設定の Surface は NoAsset (落ちない)");
        rtWorld.GetComponent<NavMeshSurfaceComponent>(rtSurface)->navAsset = AssetID{ kTestAssetGuid };
        nav.Update(rtWorld);
        ck.Check(nav.Surfaces().size() == 1 && nav.Surfaces()[0].state == NavSurfaceState::Loaded
                     && nav.Surfaces()[0].polyCount == bake.polyCount,
                 "navAsset を設定すると読み込まれ、ポリゴン数がベイク結果と一致する");
        std::vector<DebugLineCmd> lines;
        nav.AppendDebugLines(rtWorld, lines);
        const size_t outlineCount = lines.size();
        ck.Check(outlineCount > 0, "輪郭の線が出る");
        lines.clear();
        rtWorld.GetComponent<NavMeshSurfaceComponent>(rtSurface)->drawTileBounds = true;
        nav.AppendDebugLines(rtWorld, lines);
        ck.Check(lines.size() > outlineCount, "タイル境界を有効にすると線が増える");
        lines.clear();
        rtWorld.GetComponent<NavMeshSurfaceComponent>(rtSurface)->drawNavMesh = false;
        rtWorld.GetComponent<NavMeshSurfaceComponent>(rtSurface)->drawTileBounds = false;
        nav.AppendDebugLines(rtWorld, lines);
        ck.Check(lines.empty(), "表示フラグを切ると線が出ない");
        const NavTileStore* before = nav.Surfaces()[0].store.get();
        nav.Update(rtWorld);
        ck.Check(nav.Surfaces()[0].store.get() == before, "構成が変わらない tick では読み直さない");
        nav.Reset();
        ck.Check(nav.Surfaces().empty(), "Reset で捨てる");

        rtWorld.GetComponent<NavMeshSurfaceComponent>(rtSurface)->navAsset = AssetID{ kTestAssetGuid + 1 };
        nav.Update(rtWorld);
        ck.Check(nav.Surfaces().size() == 1 && nav.Surfaces()[0].state == NavSurfaceState::Failed,
                 "解決できない GUID の Surface だけが Failed になる (落ちない)");

        Scene bare;
        NavSystem navBare;
        navBare.Update(bare.GetWorld());
        ck.Check(navBare.Surfaces().empty(), "Surface が無いシーンでは何も持たない");
        assetguid::Install(nullptr, nullptr);
    }

    fs::remove_all(dir, ec);
    if (ck.failCount == 0) {
        MYE_LOG_INFO("NavSurface self test: ALL PASS");
    } else {
        MYE_LOG_ERROR("NavSurface self test: %d FAILED", ck.failCount);
    }
    return ck.failCount == 0;
}

} // namespace mye
