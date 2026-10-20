//====================================================================================
//                          TilemapSystem.cpp
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                          タイルマップの実効セルと座標変換の実装
//====================================================================================
#include "Engine/Engine/Tilemap/TilemapSystem.h"

#include <cmath>

#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"

using namespace DirectX;

namespace mye {
namespace {

uint64_t sDirtyEpoch = 1;

const TilemapLayer* LayerOf(const TilemapAsset* map, int32_t layer)
{
    if (map == nullptr || layer < 0 || static_cast<size_t>(layer) >= map->layers.size()) {
        return nullptr;
    }
    return &map->layers[static_cast<size_t>(layer)];
}

} // namespace

const TilemapAsset* TilemapSystem::AssetOf(World& world, EntityID e)
{
    const auto* tm = world.GetComponent<TilemapComponent>(e);
    return (tm != nullptr) ? tilemap::Resolve(tm->tilemap) : nullptr;
}

XMFLOAT3 TilemapSystem::OriginOf(World& world, EntityID e)
{
    if (const auto* wm = world.GetComponent<WorldMatrixComponent>(e)) {
        return { wm->value._41, wm->value._42, wm->value._43 };
    }
    if (const auto* lt = world.GetComponent<LocalTransform>(e)) {
        return lt->position;
    }
    return { 0.0f, 0.0f, 0.0f };
}

uint32_t TilemapSystem::GetCell(World& world, EntityID e, int32_t layer, int32_t x, int32_t y)
{
    const TilemapLayer* l = LayerOf(AssetOf(world, e), layer);
    return (l != nullptr) ? l->GetCell(x, y) : 0u;
}

bool TilemapSystem::GetChunkCells(World& world, EntityID e, int32_t layer, ChunkKey key,
                                  std::array<uint32_t, kTilemapChunkCells>& out)
{
    const TilemapLayer* l = LayerOf(AssetOf(world, e), layer);
    if (l == nullptr) {
        return false;
    }
    out.fill(0u);
    const auto it = l->chunks.find(key);
    if (it != l->chunks.end()) {
        out = it->second.cells;
    }
    return true;
}

void TilemapSystem::EnumerateChunks(World& world, EntityID e, int32_t layer, std::vector<ChunkKey>& out)
{
    out.clear();
    const TilemapLayer* l = LayerOf(AssetOf(world, e), layer);
    if (l == nullptr) {
        return;
    }
    for (const auto& [key, chunk] : l->chunks) {
        out.push_back(key);
    }
}

void TilemapSystem::WorldToCell(World& world, EntityID e, const XMFLOAT3& worldPos, int32_t& cellX,
                                int32_t& cellY)
{
    const TilemapAsset* map = AssetOf(world, e);
    const float sx = (map != nullptr) ? map->cellSize[0] : 1.0f;
    const float sy = (map != nullptr) ? map->cellSize[1] : 1.0f;
    const XMFLOAT3 o = OriginOf(world, e);
    cellX = static_cast<int32_t>(std::floor((worldPos.x - o.x) / sx));
    cellY = static_cast<int32_t>(std::floor((worldPos.y - o.y) / sy));
}

XMFLOAT3 TilemapSystem::CellToWorld(World& world, EntityID e, int32_t cellX, int32_t cellY)
{
    const TilemapAsset* map = AssetOf(world, e);
    const float sx = (map != nullptr) ? map->cellSize[0] : 1.0f;
    const float sy = (map != nullptr) ? map->cellSize[1] : 1.0f;
    const XMFLOAT3 o = OriginOf(world, e);
    return { o.x + (static_cast<float>(cellX) + 0.5f) * sx, o.y + (static_cast<float>(cellY) + 0.5f) * sy, o.z };
}

void TilemapSystem::MarkAllDirty()
{
    ++sDirtyEpoch;
}

uint64_t TilemapSystem::DirtyEpoch()
{
    return sDirtyEpoch;
}

} // namespace mye
