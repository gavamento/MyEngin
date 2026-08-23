#include "Engine/Engine/Physics/TerrainColliderLibrary.h"

#include "Engine/Core/AssetGuidResolver.h"
#include "Engine/Core/Log.h"

namespace mye {
namespace {

TerrainColliderLibrary* gLib = nullptr;

// 高さの範囲を 1 回だけ測る。空の地形は heightBase の一点に潰す
void MeasureHeights(const TerrainAsset::TerrainData& d, float& outMin, float& outMax)
{
    if (d.heights.empty()) {
        outMin = d.heightBase;
        outMax = d.heightBase;
        return;
    }
    uint16_t lo = 65535, hi = 0;
    for (uint16_t h : d.heights) {
        if (h < lo) {
            lo = h;
        }
        if (h > hi) {
            hi = h;
        }
    }
    const float inv = 1.0f / 65535.0f;
    outMin = d.heightBase + static_cast<float>(lo) * inv * d.heightScale;
    outMax = d.heightBase + static_cast<float>(hi) * inv * d.heightScale;
}

} // namespace

const TerrainCollisionData* TerrainColliderLibrary::Get(AssetID terrainAsset)
{
    if (terrainAsset.value == 0) {
        return nullptr;
    }
    auto it = cache_.find(terrainAsset.value);
    if (it != cache_.end()) {
        return it->second.get();
    }
    if (failed_.find(terrainAsset.value) != failed_.end()) {
        return nullptr; // 負のキャッシュ: 毎 tick ロードを試し続けない
    }
    const std::wstring path = assetguid::ResolvePath(terrainAsset.value);
    TerrainAsset::TerrainData data;
    if (path.empty() || !TerrainAsset::Load(path, data) || !data.Valid()) {
        failed_[terrainAsset.value] = true;
        return nullptr;
    }
    Register(terrainAsset, std::move(data));
    return cache_[terrainAsset.value].get();
}

void TerrainColliderLibrary::Register(AssetID id, TerrainAsset::TerrainData data)
{
    auto entry = std::make_unique<TerrainCollisionData>();
    entry->data = std::move(data);
    MeasureHeights(entry->data, entry->minHeight, entry->maxHeight);
    failed_.erase(id.value);
    cache_[id.value] = std::move(entry);
}

namespace terraincol {

void Install(TerrainColliderLibrary* lib)
{
    gLib = lib;
}

TerrainColliderLibrary* Library()
{
    return gLib;
}

const TerrainCollisionData* Resolve(AssetID id)
{
    if (!gLib || id.value == 0) {
        return nullptr;
    }
    return gLib->Get(id);
}

} // namespace terraincol
} // namespace mye
