//====================================================================================
//                          TilemapSystem.cpp
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                          タイルマップの実効セル・衝突・書き換え (sim 状態) の実装
//====================================================================================
#include "Engine/Engine/Tilemap/TilemapSystem.h"

#include <algorithm>
#include <cmath>
#include <tuple>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Core/Util/ByteIo.h"
#include "Engine/Engine/Physics/Rigid/PhysicsSystem.h"
#include "Engine/Engine/Physics/Rigid/Shapes.h"

using namespace DirectX;

namespace mye {
namespace {

uint64_t sDirtyEpoch = 1;
TilemapSystem* sSystem = nullptr;

// 添字の隣り合う 2 値を (entity, 世代, レイヤー, cellKey) の順で比べる
bool OverrideLess(const TileOverride& a, const TileOverride& b)
{
    return std::tie(a.entityIndex, a.entityGeneration, a.layer, a.cellKey)
           < std::tie(b.entityIndex, b.entityGeneration, b.layer, b.cellKey);
}

const TilemapLayer* LayerOf(const TilemapAsset* map, int32_t layer)
{
    if (map == nullptr || layer < 0 || static_cast<size_t>(layer) >= map->layers.size()) {
        return nullptr;
    }
    return &map->layers[static_cast<size_t>(layer)];
}

bool CellInRange(int32_t x, int32_t y)
{
    return x >= TilemapSystem::kCellCoordMin && x <= TilemapSystem::kCellCoordMax && y >= TilemapSystem::kCellCoordMin
           && y <= TilemapSystem::kCellCoordMax;
}

ChunkKey ChunkOfCell(int32_t x, int32_t y)
{
    return ChunkKey{ TilemapFloorDiv(y, kTilemapChunkSize), TilemapFloorDiv(x, kTilemapChunkSize) };
}


// chunkKeys (昇順) のうち、動かす物体の範囲のどれかと交わるチャンクだけを残す (昇順のまま)。
// 範囲ごとに、重なるチャンクの数と全チャンクの数の小さい方だけ調べる
void SelectChunksWithin(const std::vector<StaticRegion>& regions, float originX, float originY, float originZ,
                        const TilemapAsset& map, float zHalf, std::vector<ChunkKey>& chunkKeys)
{
    const float chunkW = static_cast<float>(kTilemapChunkSize) * map.cellSize[0];
    const float chunkH = static_cast<float>(kTilemapChunkSize) * map.cellSize[1];
    std::vector<ChunkKey> wanted;
    for (const StaticRegion& r : regions) {
        if (r.maxZ < originZ - zHalf || r.minZ > originZ + zHalf) {
            continue;
        }
        // 範囲を覆うチャンクの添字。実数のまま範囲を取り、巨大な範囲 (~1e30) は int64 に収まる値へ丸める
        constexpr double kChunkIndexLimit = 1.0e9;
        const auto toIndex = [](double v) { return static_cast<int64_t>(std::floor(std::clamp(v, -kChunkIndexLimit, kChunkIndexLimit))); };
        const int64_t cx0 = toIndex((static_cast<double>(r.minX) - originX) / chunkW);
        const int64_t cx1 = toIndex((static_cast<double>(r.maxX) - originX) / chunkW);
        const int64_t cy0 = toIndex((static_cast<double>(r.minY) - originY) / chunkH);
        const int64_t cy1 = toIndex((static_cast<double>(r.maxY) - originY) / chunkH);
        const int64_t span = (cx1 - cx0 + 1) * (cy1 - cy0 + 1);
        if (span > static_cast<int64_t>(chunkKeys.size())) {
            for (const ChunkKey& k : chunkKeys) {
                if (k.cx >= cx0 && k.cx <= cx1 && k.cy >= cy0 && k.cy <= cy1) {
                    wanted.push_back(k);
                }
            }
        } else {
            for (int64_t cy = cy0; cy <= cy1; ++cy) {
                for (int64_t cx = cx0; cx <= cx1; ++cx) {
                    const ChunkKey k{ static_cast<int32_t>(cy), static_cast<int32_t>(cx) };
                    if (std::binary_search(chunkKeys.begin(), chunkKeys.end(), k)) {
                        wanted.push_back(k);
                    }
                }
            }
        }
    }
    std::sort(wanted.begin(), wanted.end());
    wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());
    chunkKeys = std::move(wanted);
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

bool TilemapSystem::AssetChunkCells(World& world, EntityID e, int32_t layer, ChunkKey key,
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

void TilemapSystem::AssetEnumerateChunks(World& world, EntityID e, int32_t layer, std::vector<ChunkKey>& out)
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

uint32_t TilemapSystem::MakeCellKey(int32_t cellX, int32_t cellY)
{
    return (static_cast<uint32_t>(cellY - kCellCoordMin) << 16) | static_cast<uint32_t>(cellX - kCellCoordMin);
}

void TilemapSystem::SplitCellKey(uint32_t key, int32_t& cellX, int32_t& cellY)
{
    cellX = static_cast<int32_t>(key & 0xFFFFu) + kCellCoordMin;
    cellY = static_cast<int32_t>(key >> 16) + kCellCoordMin;
}

void TilemapSystem::MarkAllDirty()
{
    ++sDirtyEpoch;
}

uint64_t TilemapSystem::DirtyEpoch()
{
    return sDirtyEpoch;
}

// ---- 実効セル ----

size_t TilemapSystem::LowerBound(EntityKey owner, uint32_t layer, uint32_t cellKey) const
{
    const TileOverride probe{ owner.index, owner.generation, layer, cellKey, 0u };
    return static_cast<size_t>(std::lower_bound(overrides_.begin(), overrides_.end(), probe, OverrideLess)
                               - overrides_.begin());
}

uint32_t TilemapSystem::GetCell(World& world, EntityID e, int32_t layer, int32_t x, int32_t y) const
{
    const TilemapLayer* l = LayerOf(AssetOf(world, e), layer);
    if (l == nullptr) {
        return 0u;
    }
    if (!overrides_.empty() && CellInRange(x, y)) {
        const EntityKey owner{ e.index, e.generation };
        const uint32_t key = MakeCellKey(x, y);
        const size_t i = LowerBound(owner, static_cast<uint32_t>(layer), key);
        if (i < overrides_.size() && overrides_[i].entityIndex == owner.index
            && overrides_[i].entityGeneration == owner.generation && overrides_[i].layer == static_cast<uint32_t>(layer)
            && overrides_[i].cellKey == key) {
            return overrides_[i].value;
        }
    }
    return l->GetCell(x, y);
}

bool TilemapSystem::GetChunkCells(World& world, EntityID e, int32_t layer, ChunkKey key,
                                  std::array<uint32_t, kTilemapChunkCells>& out) const
{
    if (!AssetChunkCells(world, e, layer, key, out)) {
        return false;
    }
    if (overrides_.empty()) {
        return true;
    }
    // 鍵は (cy, cx) 昇順なので、チャンクの行範囲の先頭から走り、行を過ぎたら止める
    const int64_t cellY0 = static_cast<int64_t>(key.cy) * kTilemapChunkSize;
    const int64_t cellY1 = cellY0 + kTilemapChunkSize - 1;
    if (cellY1 < kCellCoordMin || cellY0 > kCellCoordMax) {
        return true;
    }
    const EntityKey owner{ e.index, e.generation };
    const uint32_t layerIndex = static_cast<uint32_t>(layer);
    const int32_t startY = static_cast<int32_t>(std::max<int64_t>(cellY0, kCellCoordMin));
    for (size_t i = LowerBound(owner, layerIndex, MakeCellKey(kCellCoordMin, startY)); i < overrides_.size(); ++i) {
        const TileOverride& o = overrides_[i];
        if (o.entityIndex != owner.index || o.entityGeneration != owner.generation || o.layer != layerIndex) {
            break;
        }
        int32_t x = 0;
        int32_t y = 0;
        SplitCellKey(o.cellKey, x, y);
        if (y > cellY1) {
            break;
        }
        if (ChunkOfCell(x, y) == key) {
            out[static_cast<size_t>(TilemapFloorMod(y, kTilemapChunkSize)) * kTilemapChunkSize
                + static_cast<size_t>(TilemapFloorMod(x, kTilemapChunkSize))] = o.value;
        }
    }
    return true;
}

void TilemapSystem::EnumerateChunks(World& world, EntityID e, int32_t layer, std::vector<ChunkKey>& out) const
{
    AssetEnumerateChunks(world, e, layer, out);
    if (LayerOf(AssetOf(world, e), layer) == nullptr || overrides_.empty()) {
        return;
    }
    const EntityKey owner{ e.index, e.generation };
    const uint32_t layerIndex = static_cast<uint32_t>(layer);
    for (size_t i = LowerBound(owner, layerIndex, 0u); i < overrides_.size(); ++i) {
        const TileOverride& o = overrides_[i];
        if (o.entityIndex != owner.index || o.entityGeneration != owner.generation || o.layer != layerIndex) {
            break;
        }
        int32_t x = 0;
        int32_t y = 0;
        SplitCellKey(o.cellKey, x, y);
        out.push_back(ChunkOfCell(x, y));
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

bool TilemapSystem::GetLayerBounds(World& world, EntityID e, int32_t layer, int32_t& minX, int32_t& minY,
                                   int32_t& maxX, int32_t& maxY) const
{
    std::vector<ChunkKey> chunks;
    EnumerateChunks(world, e, layer, chunks);
    bool found = false;
    std::array<uint32_t, kTilemapChunkCells> cells{};
    for (const ChunkKey& chunk : chunks) {
        if (!GetChunkCells(world, e, layer, chunk, cells)) {
            continue;
        }
        for (int32_t ly = 0; ly < kTilemapChunkSize; ++ly) {
            for (int32_t lx = 0; lx < kTilemapChunkSize; ++lx) {
                if (tilecell::TileId(cells[static_cast<size_t>(ly) * kTilemapChunkSize + static_cast<size_t>(lx)]) == 0u) {
                    continue;
                }
                const int32_t x = chunk.cx * kTilemapChunkSize + lx;
                const int32_t y = chunk.cy * kTilemapChunkSize + ly;
                if (!found) {
                    minX = maxX = x;
                    minY = maxY = y;
                    found = true;
                } else {
                    minX = std::min(minX, x);
                    maxX = std::max(maxX, x);
                    minY = std::min(minY, y);
                    maxY = std::max(maxY, y);
                }
            }
        }
    }
    return found;
}

// ---- 書き換え ----

void TilemapSystem::InvalidateChunk(EntityKey owner, int32_t layerIndex, ChunkKey chunk)
{
    const auto it = collision_.find(owner);
    if (it != collision_.end()) {
        it->second.chunks.erase(ChunkCacheKey{ layerIndex, chunk });
    }
}

void TilemapSystem::WriteCell(EntityID e, const TilemapLayer& layer, int32_t layerIndex, int32_t x, int32_t y,
                              uint32_t value)
{
    const EntityKey owner{ e.index, e.generation };
    const uint32_t key = MakeCellKey(x, y);
    const size_t i = LowerBound(owner, static_cast<uint32_t>(layerIndex), key);
    const bool exists = i < overrides_.size() && overrides_[i].entityIndex == owner.index
                        && overrides_[i].entityGeneration == owner.generation
                        && overrides_[i].layer == static_cast<uint32_t>(layerIndex) && overrides_[i].cellKey == key;
    if (value == layer.GetCell(x, y)) {
        if (exists) {
            overrides_.erase(overrides_.begin() + static_cast<std::ptrdiff_t>(i));
        }
    } else if (exists) {
        overrides_[i].value = value;
    } else {
        overrides_.insert(overrides_.begin() + static_cast<std::ptrdiff_t>(i),
                          TileOverride{ owner.index, owner.generation, static_cast<uint32_t>(layerIndex), key, value });
    }
    InvalidateChunk(owner, layerIndex, ChunkOfCell(x, y));
}

void TilemapSystem::WakeBodiesNear(World& world, EntityID e, int32_t x0, int32_t y0, int32_t x1, int32_t y1) const
{
    const TilemapAsset* map = AssetOf(world, e);
    if (map == nullptr) {
        return;
    }
    const float cellW = map->cellSize[0];
    const float cellH = map->cellSize[1];
    const XMFLOAT3 origin = OriginOf(world, e);
    // 変えたセル矩形の中心と半幅。剛体の大きさとセル 1 個ぶんの余裕を足して、近いものだけ起こす
    const float centerX = origin.x + 0.5f * static_cast<float>(x0 + x1 + 1) * cellW;
    const float centerY = origin.y + 0.5f * static_cast<float>(y0 + y1 + 1) * cellH;
    const float halfW = 0.5f * static_cast<float>(x1 - x0 + 1) * cellW + cellW;
    const float halfH = 0.5f * static_cast<float>(y1 - y0 + 1) * cellH + cellH;
    constexpr float kMinBodyExtent = 1.0f; // 形状の大きさが分からない剛体 (メッシュ等) の見積もり
    const ComponentTypeId req[] = { RigidbodyComponent::sTypeId, LocalTransform::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int ri = arch.FindTypeIndex(RigidbodyComponent::sTypeId);
        const int li = arch.FindTypeIndex(LocalTransform::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            auto* rb = static_cast<RigidbodyComponent*>(arch.GetPtr(ri, row));
            if (!rb->isSleeping) {
                continue;
            }
            const auto* lt = static_cast<const LocalTransform*>(arch.GetPtr(li, row));
            float extent = kMinBodyExtent;
            if (const auto* col = world.GetComponent<ColliderComponent>(arch.EntityAt(row))) {
                const float scale = std::max({ std::fabs(lt->scale.x), std::fabs(lt->scale.y), std::fabs(lt->scale.z) });
                const float local = std::max({ col->radius, col->height * 0.5f, col->halfExtents.x, col->halfExtents.y,
                                               col->halfExtents.z });
                extent = std::max(extent, local * scale);
            }
            if (std::fabs(lt->position.x - centerX) <= halfW + extent && std::fabs(lt->position.y - centerY) <= halfH + extent) {
                rb->isSleeping = false;
                rb->sleepTicks = 0;
            }
        }
    });
}

bool TilemapSystem::SetCell(World& world, EntityID e, int32_t layer, int32_t x, int32_t y, uint32_t value)
{
    const TilemapLayer* l = LayerOf(AssetOf(world, e), layer);
    if (l == nullptr || !CellInRange(x, y)) {
        return false;
    }
    const uint32_t before = GetCell(world, e, layer, x, y);
    WriteCell(e, *l, layer, x, y, value);
    MarkAllDirty();
    if (tilecell::TileId(before) != tilecell::TileId(value)) {
        WakeBodiesNear(world, e, x, y, x, y);
    }
    return true;
}

bool TilemapSystem::FillRect(World& world, EntityID e, int32_t layer, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                             uint32_t value)
{
    const TilemapLayer* l = LayerOf(AssetOf(world, e), layer);
    if (l == nullptr || !CellInRange(x0, y0) || !CellInRange(x1, y1)) {
        return false;
    }
    if (x0 > x1) {
        std::swap(x0, x1);
    }
    if (y0 > y1) {
        std::swap(y0, y1);
    }
    const int64_t cells = (static_cast<int64_t>(x1) - x0 + 1) * (static_cast<int64_t>(y1) - y0 + 1);
    if (cells > kMaxFillCells) {
        return false;
    }
    bool tileChanged = false;
    for (int32_t y = y0; y <= y1; ++y) {
        for (int32_t x = x0; x <= x1; ++x) {
            tileChanged = tileChanged || tilecell::TileId(GetCell(world, e, layer, x, y)) != tilecell::TileId(value);
            WriteCell(e, *l, layer, x, y, value);
        }
    }
    MarkAllDirty();
    if (tileChanged) {
        WakeBodiesNear(world, e, x0, y0, x1, y1);
    }
    return true;
}

TileDamageResult TilemapSystem::DamageCell(World& world, EntityID e, int32_t layer, int32_t x, int32_t y,
                                           int32_t amount)
{
    const TilemapAsset* map = AssetOf(world, e);
    const TilemapLayer* l = LayerOf(map, layer);
    const TilePalette* palette = (map != nullptr) ? tilepalette::Resolve(map->palette) : nullptr;
    if (l == nullptr || palette == nullptr || amount <= 0 || !CellInRange(x, y)) {
        return TileDamageResult::kNone;
    }
    const uint32_t value = GetCell(world, e, layer, x, y);
    const TileDef* def = palette->Find(tilecell::TileId(value));
    if (def == nullptr || def->hp <= 0) {
        return TileDamageResult::kNone;
    }
    constexpr uint32_t kDamageMax = 0xFFu;
    const uint32_t damage = std::min<uint32_t>(kDamageMax, tilecell::Damage(value) + static_cast<uint32_t>(amount));
    if (damage < static_cast<uint32_t>(def->hp)) {
        const uint32_t damagedValue = (value & ~(kDamageMax << tilecell::kDamageShift)) | (damage << tilecell::kDamageShift);
        WriteCell(e, *l, layer, x, y, damagedValue);
        MarkAllDirty();
        return TileDamageResult::kDamaged;
    }
    // 壊れた: 向き (flip / 回転 / 予約ビット) は残して絵と damage だけ置き換える。空になるなら全ビットを 0 にする
    uint32_t brokenValue = 0u;
    if (def->destroyedTile != 0) {
        const uint32_t keepMask = ~(tilecell::kIdMask | (kDamageMax << tilecell::kDamageShift));
        brokenValue = (value & keepMask) | (def->destroyedTile & tilecell::kIdMask);
    }
    WriteCell(e, *l, layer, x, y, brokenValue);
    MarkAllDirty();
    WakeBodiesNear(world, e, x, y, x, y);
    if (!def->destroyEffect.IsNull()) {
        pendingEffects_.push_back(TileEffectRequest{ def->destroyEffect.value, CellToWorld(world, e, x, y) });
    }
    return TileDamageResult::kDestroyed;
}

void TilemapSystem::DrainEffects(std::vector<TileEffectRequest>& out)
{
    out.insert(out.end(), pendingEffects_.begin(), pendingEffects_.end());
    pendingEffects_.clear();
}

void TilemapSystem::Sweep(World& world)
{
    std::erase_if(overrides_, [&world](const TileOverride& o) {
        EntityID id;
        id.index = o.entityIndex;
        id.generation = o.entityGeneration;
        return !world.IsAlive(id) || !world.HasComponent(id, TilemapComponent::sTypeId);
    });
}

void TilemapSystem::Reset()
{
    overrides_.clear();
    pendingEffects_.clear();
    collision_.clear();
    MarkAllDirty();
}

// ---- SimSnapshot の TM2D 節 ----

void TilemapSystem::SaveSnapshot(ByteWriter& w) const
{
    w.Count(overrides_.size());
    for (const TileOverride& o : overrides_) {
        w.U32(o.entityIndex);
        w.U32(o.entityGeneration);
        w.U32(o.layer);
        w.U32(o.cellKey);
        w.U32(o.value);
    }
}

bool TilemapSystem::ReadSnapshot(ByteReader& r, TilemapSnapshot& out)
{
    constexpr size_t kEntryBytes = sizeof(uint32_t) * 5;
    const size_t count = r.Count(kEntryBytes);
    out.overrides.clear();
    out.overrides.reserve(count);
    for (size_t i = 0; i < count && r.Ok(); ++i) {
        TileOverride o;
        o.entityIndex = r.U32();
        o.entityGeneration = r.U32();
        o.layer = r.U32();
        o.cellKey = r.U32();
        o.value = r.U32();
        // 表は厳密な昇順 (重複なし)・レイヤーは範囲内。壊れた blob を sim 状態へ入れない
        const bool ordered = out.overrides.empty() || OverrideLess(out.overrides.back(), o);
        if (!ordered || o.layer >= kMaxTilemapLayers) {
            MYE_LOG_ERROR("[snapshot] tilemap override table is corrupt (entry %zu)", i);
            return false;
        }
        out.overrides.push_back(o);
    }
    return r.Ok();
}

void TilemapSystem::ApplySnapshot(World& world, TilemapSnapshot&& snapshot)
{
    overrides_ = std::move(snapshot.overrides);
    pendingEffects_.clear();
    collision_.clear();
    Sweep(world);
    MarkAllDirty();
}

// ---- 衝突 ----

void TilemapSystem::BuildChunkBoxes(World& world, EntityID e, const TilemapAsset& map, const TilePalette& palette,
                                    int32_t layerIndex, ChunkKey chunk, ChunkBoxes& out) const
{
    std::array<uint32_t, kTilemapChunkCells> cells;
    if (!GetChunkCells(world, e, layerIndex, chunk, cells)) {
        return;
    }
    const float cellW = map.cellSize[0];
    const float cellH = map.cellSize[1];
    const auto isKind = [&](int32_t lx, int32_t ly, bool wantOneWay) {
        const TileDef* def =
            palette.Find(tilecell::TileId(cells[static_cast<size_t>(ly) * kTilemapChunkSize + static_cast<size_t>(lx)]));
        return def != nullptr && def->collider != 0 && def->oneWay == wantOneWay;
    };
    // 一方通行は別の列にする (通常の箱と一緒に結合すると、通れるセルと通れないセルが 1 つの箱になる)
    for (int pass = 0; pass < 2; ++pass) {
        const bool wantOneWay = (pass == 1);
        std::vector<LocalBox>& dst = wantOneWay ? out.oneWay : out.solid;
        for (int32_t ly = 0; ly < kTilemapChunkSize; ++ly) {
            int32_t lx = 0;
            while (lx < kTilemapChunkSize) {
                if (!isKind(lx, ly, wantOneWay)) {
                    ++lx;
                    continue;
                }
                const int32_t start = lx;
                while (lx < kTilemapChunkSize && isKind(lx, ly, wantOneWay)) {
                    ++lx;
                }
                const float runW = static_cast<float>(lx - start) * cellW;
                const float x0 = static_cast<float>(chunk.cx * kTilemapChunkSize + start) * cellW;
                const float y0 = static_cast<float>(chunk.cy * kTilemapChunkSize + ly) * cellH;
                dst.push_back(LocalBox{ x0 + runW * 0.5f, y0 + cellH * 0.5f, runW * 0.5f, cellH * 0.5f });
            }
        }
    }
}

void TilemapSystem::Collect(World& world, std::vector<StaticShape>& out)
{
    CollectImpl(world, nullptr, out);
}

void TilemapSystem::CollectWithin(World& world, MotionRegions& regions, std::vector<StaticShape>& out)
{
    CollectImpl(world, &regions, out);
}

void TilemapSystem::RefreshCacheEpochs()
{
    // 参照先のアセットが差し替わったら全部作り直す
    const TilemapLibrary* maps = tilemap::Library();
    const TilePaletteLibrary* palettes = tilepalette::Library();
    const uint64_t mapEpoch = (maps != nullptr) ? maps->Epoch() : 0;
    const uint64_t paletteEpoch = (palettes != nullptr) ? palettes->Epoch() : 0;
    if (mapEpoch != cachedMapEpoch_ || paletteEpoch != cachedPaletteEpoch_) {
        collision_.clear();
        cachedMapEpoch_ = mapEpoch;
        cachedPaletteEpoch_ = paletteEpoch;
    }
}

void TilemapSystem::CollectImpl(World& world, MotionRegions* regions, std::vector<StaticShape>& out)
{
    lastBoxCount_ = 0;
    RefreshCacheEpochs();

    struct Live {
        EntityID entity;
        TilemapComponent tilemap;
    };
    std::vector<Live> live;
    const ComponentTypeId req[] = { TilemapComponent::sTypeId, LocalTransform::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int ti = arch.FindTypeIndex(TilemapComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            if (!IsEntityActive(world, e)) {
                continue;
            }
            live.push_back({ e, *static_cast<const TilemapComponent*>(arch.GetPtr(ti, row)) });
        }
    });
    // アーキタイプの走査順に依存しない: 持ち主の index 昇順 (明示キー)
    std::sort(live.begin(), live.end(), [](const Live& a, const Live& b) { return a.entity.index < b.entity.index; });

    // 生きていない持ち主のキャッシュを捨てる (live は index 昇順なので、鍵の並びも昇順)
    std::vector<EntityKey> liveKeys;
    liveKeys.reserve(live.size());
    for (const Live& l : live) {
        liveKeys.push_back(EntityKey{ l.entity.index, l.entity.generation });
    }
    for (auto it = collision_.begin(); it != collision_.end();) {
        it = std::binary_search(liveKeys.begin(), liveKeys.end(), it->first) ? std::next(it) : collision_.erase(it);
    }

    for (const Live& l : live) {
        const size_t before = out.size();
        CollectEntity(world, l.entity, l.tilemap, regions, out);
        lastBoxCount_ += out.size() - before;
    }
}

void TilemapSystem::CollectEntity(World& world, EntityID e, const TilemapComponent& tm, MotionRegions* regions,
                                  std::vector<StaticShape>& out)
{
    const EntityKey owner{ e.index, e.generation };
    const TilemapAsset* map = tilemap::Resolve(tm.tilemap);
    const TilePalette* palette = (map != nullptr) ? tilepalette::Resolve(map->palette) : nullptr;
    if (map == nullptr || palette == nullptr) {
        collision_.erase(owner);
        return;
    }
    EntityCollision& ec = collision_[owner];
    if (ec.assetId != tm.tilemap.value) {
        ec.chunks.clear();
        ec.assetId = tm.tilemap.value;
    }
    // コンポーネントの値は毎 tick 写す (Inspector での変更が次の tick から効く)
    ec.solid = ColliderComponent{};
    ec.solid.shape = collidershape::kBox;
    ec.solid.isTrigger = false;
    ec.solid.layer = tm.layer;
    ec.solid.mask = tm.mask;
    ec.solid.friction = tm.friction;
    ec.solid.physMaterial = tm.physMaterial;
    ec.oneWay = ec.solid;
    ec.oneWay.oneWay = true;

    float px = 0.0f;
    float py = 0.0f;
    float pz = 0.0f;
    float qx = 0.0f;
    float qy = 0.0f;
    float qz = 0.0f;
    float qw = 1.0f;
    ComposeEntityWorldPose(world, e, px, py, pz, qx, qy, qz, qw);
    // z の厚みは 1 セルぶん (中心は原点の z)。plane2D の物体が z をずらしていても当たる余裕
    const float zHalf = 0.5f * std::max(map->cellSize[0], map->cellSize[1]);

    const auto emit = [&](const std::vector<LocalBox>& boxes, const ColliderComponent* col) {
        for (const LocalBox& b : boxes) {
            ShapePose pose;
            pose.shape = collidershape::kBox;
            pose.px = px + b.cx;
            pose.py = py + b.cy;
            pose.pz = pz;
            pose.hx = b.hx;
            pose.hy = b.hy;
            pose.hz = zHalf;
            out.push_back(StaticShape{ e, pose, col });
        }
    };
    std::vector<ChunkKey> chunkKeys;
    for (size_t li = 0; li < map->layers.size(); ++li) {
        if (!map->layers[li].collision) {
            continue;
        }
        const int32_t layerIndex = static_cast<int32_t>(li);
        EnumerateChunks(world, e, layerIndex, chunkKeys);
        if (regions != nullptr) {
            SelectChunksWithin(regions->Get(), px, py, pz, *map, zHalf, chunkKeys);
        }
        for (const ChunkKey& key : chunkKeys) {
            const ChunkCacheKey cacheKey{ layerIndex, key };
            auto cached = ec.chunks.find(cacheKey);
            if (cached == ec.chunks.end()) {
                ChunkBoxes boxes;
                BuildChunkBoxes(world, e, *map, *palette, layerIndex, key, boxes);
                cached = ec.chunks.emplace(cacheKey, std::move(boxes)).first;
            }
            emit(cached->second.solid, &ec.solid);
            emit(cached->second.oneWay, &ec.oneWay);
        }
    }
}

void TilemapSystem::AppendCollisionShapes(World& world, EntityID e, std::vector<StaticShape>& out)
{
    const auto* tm = world.GetComponent<TilemapComponent>(e);
    if (tm == nullptr) {
        return;
    }
    RefreshCacheEpochs();
    CollectEntity(world, e, *tm, nullptr, out);
}

namespace tilemap {

void InstallSystem(TilemapSystem* system)
{
    sSystem = system;
}

TilemapSystem* System()
{
    return sSystem;
}

} // namespace tilemap

} // namespace mye
