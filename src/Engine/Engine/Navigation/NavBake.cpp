//====================================================================================
//                          NavBake.cpp
//  MyEngin/ 秋田蓮音                                                     10/03/2026
//                                          ナビメッシュのベイク実装
//====================================================================================
#include "Engine/Engine/Navigation/NavBake.h"

#include <algorithm>

#include "Engine/Core/Ecs/World.h"

namespace mye {
namespace {

// 1 セッションで salt が巡回しないために残すビット数 (ADR-023)。tile ビット + poly ビット <= 32 - この値
constexpr uint32_t kMinSaltBits = 10;
// 解析で得た最大ポリゴン数に掛ける余裕 (障害物の切り抜きでポリゴンが増える分)
constexpr int kPolyMargin = 2;
constexpr int kMinPolysPerTile = 64;
// ベイク直後に最大ポリゴン数を測るための仮の上限
constexpr int kProbeMaxPolys = 4096;
constexpr int kMaxTileGridSide = 4096;
constexpr int kMaxTileCount = 1 << 14;

int NextPow2(int v)
{
    int p = 1;
    while (p < v) {
        p <<= 1;
    }
    return p;
}

int Log2Ceil(int v)
{
    int bits = 0;
    while ((1 << bits) < v) {
        ++bits;
    }
    return bits;
}

void HashConfig(uint64_t& h, const NavBakeConfig& c)
{
    const float floats[] = { c.cellSize, c.cellHeight, c.agentHeight, c.agentRadius, c.agentMaxClimb,
                             c.agentMaxSlopeDeg, c.maxEdgeLen, c.maxSimplificationError, c.boundsMin[0],
                             c.boundsMin[1], c.boundsMin[2], c.boundsMax[0], c.boundsMax[1], c.boundsMax[2] };
    const int32_t ints[] = { c.tileSize, c.minRegionArea, c.mergeRegionArea };
    h = NavFnv1a(h, floats, sizeof(floats));
    h = NavFnv1a(h, ints, sizeof(ints));
}

} // namespace

bool NavPrepareBakeInputs(World& world, EntityID surface, NavBakeInputs& out)
{
    if (world.GetComponent<NavMeshSurfaceComponent>(surface) == nullptr) {
        return false;
    }
    // 無効な Surface はグループに入らない。単独で焼く (エディタで無効にした Surface の Bake を拒まない)
    NavSurfaceGroup group;
    if (!NavFindSurfaceGroup(world, surface, group)) {
        group.leader = surface;
        group.members = { surface };
    }
    const auto matrixOf = [&](EntityID e) {
        DirectX::XMFLOAT4X4 m;
        DirectX::XMStoreFloat4x4(&m, DirectX::XMMatrixIdentity());
        if (const auto* wm = world.GetComponent<WorldMatrixComponent>(e)) {
            m = wm->value;
        }
        return m;
    };
    out.config = NavMakeBakeConfig(*world.GetComponent<NavMeshSurfaceComponent>(group.leader), matrixOf(group.leader));
    out.clipBoxes.clear();
    std::vector<NavCollectRange> ranges;
    for (const EntityID member : group.members) {
        const auto* comp = world.GetComponent<NavMeshSurfaceComponent>(member);
        const NavBakeConfig range = NavMakeBakeConfig(*comp, matrixOf(member));
        NavBakeClipBox& box = out.clipBoxes.emplace_back();
        NavCollectRange& collect = ranges.emplace_back();
        for (int i = 0; i < 3; ++i) {
            box.boundsMin[i] = collect.boundsMin[i] = range.boundsMin[i];
            box.boundsMax[i] = collect.boundsMax[i] = range.boundsMax[i];
            out.config.boundsMin[i] = (std::min)(out.config.boundsMin[i], range.boundsMin[i]);
            out.config.boundsMax[i] = (std::max)(out.config.boundsMax[i], range.boundsMax[i]);
        }
        collect.collectLayerMask = comp->collectLayerMask;
    }
    out.soup = NavTriangleSoup{};
    NavCollectTriangles(world, ranges, out.soup);
    return true;
}

uint64_t NavComputeInputHash(const NavBakeConfig& config, const NavTriangleSoup& soup,
                             const std::vector<NavBakeClipBox>& clipBoxes)
{
    uint64_t h = kNavFnvSeed;
    const uint32_t version = NavMeshAsset::kNavBakeVersion;
    h = NavFnv1a(h, &version, sizeof(version));
    HashConfig(h, config);
    h = NavFnv1a(h, soup.verts.data(), soup.verts.size() * sizeof(float));
    h = NavFnv1a(h, soup.tris.data(), soup.tris.size() * sizeof(int));
    for (const NavBakeClipBox& box : clipBoxes) {
        h = NavFnv1a(h, box.boundsMin, sizeof(box.boundsMin));
        h = NavFnv1a(h, box.boundsMax, sizeof(box.boundsMax));
    }
    return h;
}

bool NavBakeTile(const NavBakeConfig& config, const NavTriangleSoup& soup, const std::vector<NavBakeClipBox>& clipBoxes,
                 int tx, int ty, std::vector<std::vector<uint8_t>>& outLayers)
{
    // タイルの XZ 範囲 + 継ぎ目の余白。この外の三角形はこのタイルに影響しない
    const float tileSpan = static_cast<float>(config.tileSize) * config.cellSize;
    const float border = static_cast<float>(NavTileBorderCells(config)) * config.cellSize;
    const float minX = config.boundsMin[0] + static_cast<float>(tx) * tileSpan - border;
    const float maxX = config.boundsMin[0] + static_cast<float>(tx + 1) * tileSpan + border;
    const float minZ = config.boundsMin[2] + static_cast<float>(ty) * tileSpan - border;
    const float maxZ = config.boundsMin[2] + static_cast<float>(ty + 1) * tileSpan + border;

    NavTriangleSoup local;
    const int triCount = soup.TriangleCount();
    for (int t = 0; t < triCount; ++t) {
        const float* v[3] = { &soup.verts[static_cast<size_t>(soup.tris[static_cast<size_t>(t) * 3 + 0]) * 3],
                              &soup.verts[static_cast<size_t>(soup.tris[static_cast<size_t>(t) * 3 + 1]) * 3],
                              &soup.verts[static_cast<size_t>(soup.tris[static_cast<size_t>(t) * 3 + 2]) * 3] };
        const float lo[2] = { (std::min)({ v[0][0], v[1][0], v[2][0] }), (std::min)({ v[0][2], v[1][2], v[2][2] }) };
        const float hi[2] = { (std::max)({ v[0][0], v[1][0], v[2][0] }), (std::max)({ v[0][2], v[1][2], v[2][2] }) };
        if (hi[0] < minX || lo[0] > maxX || hi[1] < minZ || lo[1] > maxZ) {
            continue;
        }
        local.AddTriangle(v[0], v[1], v[2]);
    }
    return NavBakeTileLayers(config, local.View(), tx, ty, outLayers, clipBoxes.data(),
                             static_cast<int>(clipBoxes.size()));
}

NavBakeOutput NavBakeAsset(const NavBakeConfig& config, const NavTriangleSoup& soup,
                           const std::vector<NavBakeClipBox>& clipBoxes, NavBakeControl* control)
{
    NavBakeOutput out;
    int tilesX = 0;
    int tilesY = 0;
    NavCalcTileGrid(config, tilesX, tilesY);
    if (tilesX <= 0 || tilesY <= 0 || tilesX > kMaxTileGridSide || tilesY > kMaxTileGridSide
        || tilesX * tilesY > kMaxTileCount) {
        out.status = NavBakeStatus::Failed;
        out.message = "the bake range is empty or needs too many tiles; enlarge the cell size or shrink the range";
        return out;
    }
    if (control != nullptr) {
        control->tilesTotal.store(tilesX * tilesY);
        control->tilesDone.store(0);
    }

    NavMeshAsset::Data& data = out.data;
    data.bakeVersion = NavMeshAsset::kNavBakeVersion;
    data.inputHash = NavComputeInputHash(config, soup, clipBoxes);
    data.config = config;
    data.tilesX = tilesX;
    data.tilesY = tilesY;
    data.inputTriangleCount = soup.TriangleCount();
    data.maxObstacles = kNavMaxObstacles;

    // 層は (ty, tx, layer) 昇順に並べる (Deserialize の検査と NavTileStore の内部順に合わせる)
    for (int ty = 0; ty < tilesY; ++ty) {
        for (int tx = 0; tx < tilesX; ++tx) {
            if (control != nullptr && control->cancel.load()) {
                out.status = NavBakeStatus::Cancelled;
                out.message = "cancelled";
                data = NavMeshAsset::Data{};
                return out;
            }
            std::vector<std::vector<uint8_t>> layers;
            if (!NavBakeTile(config, soup, clipBoxes, tx, ty, layers)) {
                out.status = NavBakeStatus::Failed;
                out.message = "the tile bake failed at tile (" + std::to_string(tx) + ", " + std::to_string(ty) + ")";
                data = NavMeshAsset::Data{};
                return out;
            }
            for (size_t i = 0; i < layers.size(); ++i) {
                NavMeshAsset::LayerRecord record;
                record.tx = tx;
                record.ty = ty;
                record.layer = static_cast<int32_t>(i);
                record.blob = std::move(layers[i]);
                data.layers.push_back(std::move(record));
            }
            if (!layers.empty()) {
                ++out.tileCount;
            }
            if (control != nullptr) {
                control->tilesDone.fetch_add(1);
            }
        }
    }
    if (data.layers.empty()) {
        out.status = NavBakeStatus::Empty;
        out.message = "no walkable surface was found in the bake range";
        return out;
    }

    // dtNavMeshParams を決める: 一度組み立てて最大ポリゴン数を測り、余裕を掛ける (ADR-023)
    const int layerCount = static_cast<int>(data.layers.size());
    data.maxTiles = (std::max)((layerCount * 3 + 1) / 2, layerCount + 4);
    data.maxPolysPerTile = kProbeMaxPolys;
    NavTileStore probe;
    if (!NavMeshAsset::BuildStore(data, probe)) {
        out.status = NavBakeStatus::Failed;
        out.message = "the baked layers could not be assembled into a navigation mesh";
        data = NavMeshAsset::Data{};
        return out;
    }
    int maxPolys = 0;
    const dtNavMesh* probeMesh = probe.NavMesh();
    for (int i = 0; i < probeMesh->getMaxTiles(); ++i) {
        const dtMeshTile* tile = probeMesh->getTile(i);
        if (tile != nullptr && tile->header != nullptr) {
            maxPolys = (std::max)(maxPolys, tile->header->polyCount);
            out.polyCount += tile->header->polyCount;
        }
    }
    data.maxPolysPerTile = (std::max)(kMinPolysPerTile, NextPow2(maxPolys * kPolyMargin));
    const int refBits = Log2Ceil(NextPow2(data.maxTiles)) + Log2Ceil(data.maxPolysPerTile);
    if (static_cast<uint32_t>(refBits) + kMinSaltBits > 32u) {
        out.status = NavBakeStatus::Failed;
        out.message = "the navigation mesh is too large to keep enough salt bits; enlarge the tile or cell size";
        data = NavMeshAsset::Data{};
        return out;
    }
    out.status = NavBakeStatus::Ok;
    return out;
}

} // namespace mye
