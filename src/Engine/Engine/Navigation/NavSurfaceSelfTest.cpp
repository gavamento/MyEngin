//====================================================================================
//                          NavSurfaceSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                     10/03/2026
//                                          NavMeshSurface のベイク・.mnav・読み込みの回帰テスト実装
//====================================================================================
#include "Engine/Engine/Navigation/NavSurfaceSelfTest.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "Engine/Core/Asset/AssetGuidResolver.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Navigation/NavBake.h"
#include "Engine/Engine/Navigation/NavDebugDraw.h"
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
constexpr uint64_t kExpectedAssetHash = 0x17818EF048195584ull; // M84b: ベイク方式 2 (範囲の箱で切り詰め)
constexpr float kNavTestDt = 1.0f / 60.0f;
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
    comp->autoCellSize = false; // 期待ハッシュを固定するため、ベイク方式が変わっていないことの基準に旧設定を使う
    comp->maxClimb = 0.1f;
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
    NavBakeOutput bake = NavBakeAsset(inputs.config, inputs.soup, inputs.clipBoxes, &control);
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
    NavBakeOutput bakeAgain = NavBakeAsset(inputs.config, inputs.soup, inputs.clipBoxes, nullptr);
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
    ck.Check(bake.data.inputHash == NavComputeInputHash(inputs.config, inputs.soup, inputs.clipBoxes),
             "資産が入力ハッシュを持つ");

    // ---- 2b. セルサイズの自動決定 (M82d): 実際に使った値が入力ハッシュに入る ----
    {
        NavMeshSurfaceComponent def;
        const NavCellSize defCell = NavResolveCellSize(def);
        ck.Check(std::fabs(defCell.cellSize - 0.15f) < 1e-6f && !defCell.clampedToMinimum,
                 "既定の Surface は agentRadius / 2 = maxClimb / (2 tan 45) = 0.15 m のセルになる");
        ck.Check(std::fabs(defCell.cellHeight - 0.05f) < 1e-4f
                     && std::floor(def.maxClimb / defCell.cellHeight) == 6.0f,
                 "...セルの高さは min(cs / 2, maxClimb / 6) = 0.05 m で、maxClimb がちょうど 6 セル");
        ck.Check(std::fabs(NavEffectiveSlopeLimitDeg(def, defCell.cellSize) - 45.0f) < 0.01f && !NavSurfaceSlopeUnreachable(def),
                 "...実効の坂の上限は 45 度で、maxSlopeDeg 45 は警告にならない");

        NavMeshSurfaceComponent steep = def;
        steep.maxSlopeDeg = 60.0f;
        const NavCellSize steepCell = NavResolveCellSize(steep);
        ck.Check(steepCell.cellSize < defCell.cellSize && std::fabs(NavEffectiveSlopeLimitDeg(steep, steepCell.cellSize) - 60.0f) < 0.01f,
                 "maxSlopeDeg を 60 度にするとセルが細かくなり、実効の上限が 60 度に届く");

        NavMeshSurfaceComponent floorHit = def;
        floorHit.maxSlopeDeg = 80.0f;
        floorHit.maxClimb = 0.1f;
        const NavCellSize floorCell = NavResolveCellSize(floorHit);
        ck.Check(floorCell.clampedToMinimum && floorCell.cellSize == kNavMinCellSize && NavSurfaceSlopeUnreachable(floorHit),
                 "セルが下限 (0.05 m) に当たると実効の上限が設定を下回り、警告になる");

        NavMeshSurfaceComponent manual = def;
        manual.autoCellSize = false;
        manual.cellSize = 0.3f;
        ck.Check(NavResolveCellSize(manual).cellSize == 0.3f && NavSurfaceSlopeUnreachable(manual),
                 "autoCellSize を切ると cellSize をそのまま使い、実効の上限 (26.6 度) を超える maxSlopeDeg は警告になる");

        DirectX::XMFLOAT4X4 identity;
        DirectX::XMStoreFloat4x4(&identity, DirectX::XMMatrixIdentity());
        NavMeshSurfaceComponent asManual = def;
        asManual.autoCellSize = false;
        asManual.cellSize = defCell.cellSize;
        asManual.cellHeight = defCell.cellHeight;
        const uint64_t hAuto = NavComputeInputHash(NavMakeBakeConfig(def, identity), inputs.soup, {});
        const uint64_t hManual = NavComputeInputHash(NavMakeBakeConfig(asManual, identity), inputs.soup, {});
        const uint64_t hSteep = NavComputeInputHash(NavMakeBakeConfig(steep, identity), inputs.soup, {});
        ck.Check(hAuto == hManual, "自動で決まったセルと同じ値を手動で指定した Surface は同じ入力ハッシュ (使った値だけがハッシュに入る)");
        ck.Check(hAuto != hSteep, "自動決定の結果が変わる設定 (maxSlopeDeg 60) は別の入力ハッシュ");
    }

    // 層はタイル単位の関数の結果そのもの (実行時の再ベイクが同じ関数を呼べる)
    {
        bool sameLayers = true;
        size_t cursor = 0;
        for (int ty = 0; ty < bake.data.tilesY && sameLayers; ++ty) {
            for (int tx = 0; tx < bake.data.tilesX && sameLayers; ++tx) {
                std::vector<std::vector<uint8_t>> layers;
                sameLayers = NavBakeTile(inputs.config, inputs.soup, inputs.clipBoxes, tx, ty, layers);
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
                sameAsUnfiltered = NavBakeTile(inputs.config, inputs.soup, inputs.clipBoxes, tx, ty, filtered)
                    && NavBakeTileLayers(inputs.config, inputs.soup.View(), tx, ty, unfiltered, inputs.clipBoxes.data(),
                                         static_cast<int>(inputs.clipBoxes.size()))
                    && filtered == unfiltered;
            }
        }
        ck.Check(sameAsUnfiltered, "タイルごとの三角形の絞り込みは、全三角形を渡したベイクと同じバイトになる");
    }

    // キャンセルと空
    {
        NavBakeControl cancel;
        cancel.cancel.store(true);
        ck.Check(NavBakeAsset(inputs.config, inputs.soup, inputs.clipBoxes, &cancel).status == NavBakeStatus::Cancelled,
                 "キャンセルを受け付ける");
        NavTriangleSoup empty;
        ck.Check(NavBakeAsset(inputs.config, empty, {}, nullptr).status == NavBakeStatus::Empty,
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
            surf->autoCellSize = false;
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
            const NavBakeOutput out = NavBakeAsset(in.config, in.soup, in.clipBoxes, nullptr);
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

        // 旧シーン互換 (M82d): 末尾に足したフィールド (CC.stepOffset / Surface.autoCellSize) が無いデータは
        // 構造体の既定値 (0.3 / true) で読まれる。書いた値が戻ることも確かめ、キーの綴りが合っていることを保証する
        {
            Scene withValues;
            GameObject walker = withValues.CreateGameObjectTracked("Walker");
            walker.AddComponent<CharacterControllerComponent>()->stepOffset = 0.45f;
            GameObject flat = withValues.CreateGameObjectTracked("FlatSurface");
            flat.AddComponent<NavMeshSurfaceComponent>()->autoCellSize = false;
            const std::wstring valuesPath = (dir / L"new_fields.scene.json").wstring();
            ck.Check(SceneSerializer::SaveToFile(withValues, valuesPath), "stepOffset / autoCellSize を含むシーンを保存できる");
            Scene back2;
            SceneSerializer::LoadFromFile(back2, valuesPath);
            const auto* ccBack = back2.GetWorld().GetComponent<CharacterControllerComponent>(back2.Find("Walker").Id());
            const auto* sfBack = back2.GetWorld().GetComponent<NavMeshSurfaceComponent>(back2.Find("FlatSurface").Id());
            ck.Check(ccBack != nullptr && ccBack->stepOffset == 0.45f && sfBack != nullptr && !sfBack->autoCellSize,
                     "stepOffset / autoCellSize は保存 -> 読み込みで戻る");

            std::string text;
            {
                std::ifstream in(valuesPath, std::ios::binary);
                text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            }
            const auto rename = [&text](const char* key, const char* replacement) {
                for (size_t at = text.find(key); at != std::string::npos; at = text.find(key, at)) {
                    text.replace(at, std::strlen(key), replacement);
                }
            };
            rename("\"stepOffset\"", "\"stepOffsetGone\"");
            rename("\"autoCellSize\"", "\"autoCellSizeGone\"");
            const std::wstring oldPath = (dir / L"old_scene.scene.json").wstring();
            {
                std::ofstream out(oldPath, std::ios::binary | std::ios::trunc);
                out.write(text.data(), static_cast<std::streamsize>(text.size()));
            }
            Scene oldScene;
            SceneSerializer::LoadFromFile(oldScene, oldPath);
            const auto* ccOld = oldScene.GetWorld().GetComponent<CharacterControllerComponent>(oldScene.Find("Walker").Id());
            const auto* sfOld = oldScene.GetWorld().GetComponent<NavMeshSurfaceComponent>(oldScene.Find("FlatSurface").Id());
            ck.Check(ccOld != nullptr && ccOld->stepOffset == 0.3f && sfOld != nullptr && sfOld->autoCellSize,
                     "旧シーン (stepOffset / autoCellSize が無い) は 0.3 / true で読まれる");
        }

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
        nav.Update(rtWorld, kNavTestDt);
        ck.Check(nav.Surfaces().size() == 1 && nav.Surfaces()[0].state == NavSurfaceState::NoAsset,
                 "navAsset が未設定の Surface は NoAsset (落ちない)");
        rtWorld.GetComponent<NavMeshSurfaceComponent>(rtSurface)->navAsset = AssetID{ kTestAssetGuid };
        nav.Update(rtWorld, kNavTestDt);
        ck.Check(nav.Surfaces().size() == 1 && nav.Surfaces()[0].state == NavSurfaceState::Loaded
                     && nav.Surfaces()[0].polyCount == bake.polyCount,
                 "navAsset を設定すると読み込まれ、ポリゴン数がベイク結果と一致する");
        // Agent の経路線は NavSystem、ナビメッシュの輪郭・塗りは NavDebugView (M82e)。Agent が居なければ経路線は出ない
        std::vector<DebugLineCmd> lines;
        nav.AppendDebugLines(rtWorld, lines);
        ck.Check(lines.empty(), "Agent が居ない Surface では NavSystem の線は出ない");

        auto* drawFlags = rtWorld.GetComponent<NavMeshSurfaceComponent>(rtSurface);
        NavDebugView view;
        view.Refresh(rtWorld);
        const size_t outlineCount = view.Lines().size();
        const size_t fillCount = view.FillVertices().size();
        ck.Check(outlineCount > 0, "輪郭の線が出る");
        ck.Check(fillCount > 0 && fillCount % 3 == 0, "塗りの三角形が出る (3 頂点ずつ)");
        bool fillOk = true;
        for (const DebugFillVertex& v : view.FillVertices()) {
            fillOk = fillOk && v.a > 0.0f && v.a < 1.0f && v.b > 0.9f; // 既定のエリア 0 は水色 (b が強い)
        }
        ck.Check(fillOk, "塗りは半透明で、エリア 0 は水色");
        ck.Check(view.GetStats().rebuildCount == 1 && view.GetStats().loadCount == 1, "最初の Refresh で 1 回だけ作る");
        for (int i = 0; i < 100; ++i) {
            view.Refresh(rtWorld);
        }
        ck.Check(view.GetStats().rebuildCount == 1 && view.GetStats().refreshCount == 101,
                 "構成が変わらないフレームでは三角形を作り直さない (101 回呼んで作り直し 1 回)");

        const uint64_t serialBefore = view.FillSerial();
        drawFlags->drawTileBounds = true;
        view.Refresh(rtWorld);
        ck.Check(view.Lines().size() > outlineCount, "タイル境界を有効にすると線が増える");
        ck.Check(view.FillSerial() != serialBefore && view.GetStats().rebuildCount == 2 && view.GetStats().loadCount == 1,
                 "表示フラグを変えると作り直すが、.mnav は読み直さない");
        drawFlags->drawNavMeshFill = false;
        view.Refresh(rtWorld);
        ck.Check(view.FillVertices().empty() && !view.Lines().empty(), "塗りだけを切ると三角形は出ず、線は残る");
        drawFlags->drawNavMeshFill = true;
        drawFlags->drawNavMesh = false;
        drawFlags->drawTileBounds = false;
        view.Refresh(rtWorld);
        ck.Check(view.Lines().empty() && view.FillVertices().size() == fillCount, "輪郭を切ると線は出ず、塗りは残る");
        drawFlags->drawNavMeshFill = false;
        view.Refresh(rtWorld);
        ck.Check(view.Lines().empty() && view.FillVertices().empty(), "表示フラグを全部切ると何も出ない");
        drawFlags->drawNavMesh = true;
        drawFlags->drawNavMeshFill = true;
        drawFlags->navAsset = AssetID{ kTestAssetGuid + 1 };
        view.Refresh(rtWorld);
        ck.Check(view.Lines().empty() && view.FillVertices().empty(), "読めない GUID の Surface は何も出さず、落ちない");
        drawFlags->navAsset = AssetID{ kTestAssetGuid };
        view.Refresh(rtWorld);
        ck.Check(view.FillVertices().size() == fillCount, "元の資産に戻すと塗りが戻る");
        Scene bareView;
        NavDebugView viewBare;
        viewBare.Refresh(bareView.GetWorld());
        ck.Check(viewBare.GetStats().rebuildCount == 0 && viewBare.FillVertices().empty(),
                 "Surface が無いシーンでは何も作らない");

        const NavTileStore* before = nav.Surfaces()[0].store.get();
        nav.Update(rtWorld, kNavTestDt);
        ck.Check(nav.Surfaces()[0].store.get() == before, "構成が変わらない tick では読み直さない");
        nav.Reset();
        ck.Check(nav.Surfaces().empty(), "Reset で捨てる");

        rtWorld.GetComponent<NavMeshSurfaceComponent>(rtSurface)->navAsset = AssetID{ kTestAssetGuid + 1 };
        nav.Update(rtWorld, kNavTestDt);
        ck.Check(nav.Surfaces().size() == 1 && nav.Surfaces()[0].state == NavSurfaceState::Failed,
                 "解決できない GUID の Surface だけが Failed になる (落ちない)");

        Scene bare;
        NavSystem navBare;
        navBare.Update(bare.GetWorld(), kNavTestDt);
        ck.Check(navBare.Surfaces().empty(), "Surface が無いシーンでは何も持たない");
        assetguid::Install(nullptr, nullptr);
    }

    // ---- 6. Surface の接続 (M84b、UE 式): 同じ Agent Type の Surface は範囲を合わせて 1 つのナビメッシュに焼く ----
    {
        // 床は x = -15..15 に続いている。Surface は x の範囲 [aMin, aMax] と [bMin, bMax] だけを覆う
        const auto buildPair = [](Scene& s, float aMin, float aMax, float bMin, float bMax, int typeB, EntityID& a,
                                  EntityID& b) {
            AddBox(s, "Ground", 0.0f, -0.5f, 0.0f, 15.0f, 0.5f, 4.0f, false, false);
            const auto addSurface = [&](const char* name, float xMin, float xMax, int type) {
                GameObject go = s.CreateGameObjectTracked(name);
                go.SetLocalPosition((xMin + xMax) * 0.5f, 0.0f, 0.0f);
                auto* comp = go.AddComponent<NavMeshSurfaceComponent>();
                comp->agentTypeId = type;
                comp->center = { 0.0f, 1.0f, 0.0f };
                comp->size = { xMax - xMin, 6.0f, 8.0f };
                comp->autoCellSize = false;
                comp->cellSize = 0.3f;
                comp->tileSize = 32;
                return go.Id();
            };
            a = addSurface("SurfaceA", aMin, aMax, 0);
            b = addSurface("SurfaceB", bMin, bMax, typeB);
            s.GetWorld().ApplyStructuralChanges();
            TransformSystem transforms;
            transforms.Update(s.GetWorld());
        };
        // 経路が届くか (start / end がナビメッシュに乗るかも返す)。bakeFrom の Surface のグループを焼く
        const auto bakeAndPath = [&](World& w, EntityID bakeFrom, const float* from, const float* to, bool* startOn,
                                     bool* endOn, NavBakeInputs* inputsOut) {
            NavBakeInputs in;
            NavPrepareBakeInputs(w, bakeFrom, in);
            const NavBakeOutput out = NavBakeAsset(in.config, in.soup, in.clipBoxes, nullptr);
            if (inputsOut != nullptr) {
                *inputsOut = in;
            }
            NavTileStore store;
            if (out.status != NavBakeStatus::Ok || !NavMeshAsset::BuildStore(out.data, store)) {
                *startOn = *endOn = false;
                return false;
            }
            return PathReaches(store, from, to, startOn, endOn);
        };
        const float left[3] = { -8.0f, 0.0f, 0.0f };
        const float right[3] = { 8.0f, 0.0f, 0.0f };
        const float outside[3] = { 13.0f, 0.0f, 0.0f };
        bool startOn = false;
        bool endOn = false;

        Scene touching;
        EntityID a;
        EntityID b;
        buildPair(touching, -10.0f, 0.0f, 0.0f, 10.0f, 0, a, b);
        std::vector<NavSurfaceGroup> groups;
        NavCollectSurfaceGroups(touching.GetWorld(), groups);
        ck.Check(groups.size() == 1 && groups[0].leader == a && groups[0].members.size() == 2 && groups[0].members[1] == b,
                 "接続: 同じ種別の Surface は 1 つのグループで、leader はキーの小さい方");
        NavBakeInputs pairInputs;
        const bool reached = bakeAndPath(touching.GetWorld(), b, left, right, &startOn, &endOn, &pairInputs);
        ck.Check(pairInputs.clipBoxes.size() == 2 && pairInputs.config.boundsMin[0] <= -10.0f
                     && pairInputs.config.boundsMax[0] >= 10.0f,
                 "接続: どの Surface から焼いてもグループ全体の範囲 (箱 2 つを合わせた AABB) を焼く");
        ck.Check(reached, "接続: 接する 2 つの Surface をまたいで経路が届く");
        bool outStart = false;
        bool outEnd = false;
        bakeAndPath(touching.GetWorld(), a, left, outside, &outStart, &outEnd, nullptr);
        ck.Check(outStart && !outEnd, "接続: 床が続いていても、Surface の範囲の外は歩けない");

        Scene gap;
        buildPair(gap, -10.0f, -2.0f, 2.0f, 10.0f, 0, a, b);
        const bool gapReached = bakeAndPath(gap.GetWorld(), a, left, right, &startOn, &endOn, nullptr);
        ck.Check(startOn && endOn && !gapReached, "接続: 離れた 2 つの Surface の間 (どちらの範囲でもない床) は通れない");

        Scene separate;
        buildPair(separate, -10.0f, 0.0f, 0.0f, 10.0f, 1, a, b);
        bakeAndPath(separate.GetWorld(), a, left, right, &startOn, &endOn, nullptr);
        ck.Check(startOn && !endOn, "接続: 種別の違う Surface は別のナビメッシュ (相手の範囲は含まない)");

        // NavSystem はグループの leader だけを読み込み、Obstacle はグループのどの範囲に入っても効く
        NavBakeOutput pairBake = NavBakeAsset(pairInputs.config, pairInputs.soup, pairInputs.clipBoxes, nullptr);
        constexpr uint64_t kPairGuid = 0x4E41564D45534833ull; // "NAVMESH3"
        NavMeshAsset::RegisterInMemory(kPairGuid, pairBake.data);
        World& tw = touching.GetWorld();
        NavCollectSurfaceGroups(tw, groups);
        for (const EntityID member : groups[0].members) {
            tw.GetComponent<NavMeshSurfaceComponent>(member)->navAsset = AssetID{ kPairGuid };
        }
        NavSystem pairNav;
        pairNav.Update(tw, kNavTestDt);
        ck.Check(pairNav.Surfaces().size() == 1 && pairNav.Surfaces()[0].entity == groups[0].leader
                     && pairNav.Surfaces()[0].state == NavSurfaceState::Loaded,
                 "接続: NavSystem はグループの leader の .mnav だけを読み込む");
        NavObstacleSpec onB;
        onB.key = 1;
        onB.type = DT_OBSTACLE_BOX;
        const float onBBox[6] = { 6.0f, -1.0f, -1.0f, 7.0f, 1.0f, 1.0f };
        std::memcpy(onB.v, onBBox, sizeof(onBBox));
        std::vector<NavObstacleSpec> filtered;
        NavFilterSpecsToSurface(tw, groups[0].leader, { onB }, filtered);
        ck.Check(filtered.size() == 1, "接続: leader の範囲の外でも、グループの別の Surface の範囲にある障害物は付く");
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
