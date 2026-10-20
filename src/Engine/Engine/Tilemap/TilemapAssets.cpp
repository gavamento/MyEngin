//====================================================================================
//                          TilemapAssets.cpp
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                          タイルパレット / タイルマップ資産と登録表の実装
//====================================================================================
#include "Engine/Engine/Tilemap/TilemapAssets.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>

#include "Engine/Core/Asset/AssetKeyResolver.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Platform/PathUtil.h"

namespace fs = std::filesystem;

namespace mye {

using nlohmann::json;

namespace {

constexpr float kMinCellSize = 1e-3f;
constexpr float kMaxCellSize = 1000.0f;
constexpr int32_t kMaxHp = 255;
constexpr int32_t kMaxTicksPerFrame = 3600;
constexpr char kNeighborAny = '.';

// "X.tilepalette.json" → "X"
std::string NameFromPath(const std::wstring& path, const char* middleSuffix)
{
    std::string name = WideToUtf8(fs::path(path).stem().wstring()); // "X.tilepalette"
    const std::string suf = middleSuffix;
    if (name.size() > suf.size() && name.compare(name.size() - suf.size(), suf.size(), suf) == 0) {
        name.resize(name.size() - suf.size());
    }
    return name;
}

uint64_t ReadU64(const json& j, const char* key)
{
    if (j.contains(key) && j[key].is_number_unsigned()) {
        return j[key].get<uint64_t>();
    }
    return 0;
}

int32_t ReadI32(const json& j, const char* key, int32_t def)
{
    if (j.contains(key) && j[key].is_number()) {
        return static_cast<int32_t>(j[key].get<int64_t>());
    }
    return def;
}

bool ReadBool(const json& j, const char* key, bool def)
{
    if (j.contains(key) && j[key].is_boolean()) {
        return j[key].get<bool>();
    }
    return def;
}

std::string ReadString(const json& j, const char* key)
{
    if (j.contains(key) && j[key].is_string()) {
        return j[key].get<std::string>();
    }
    return {};
}

bool ReadJsonFile(const std::wstring& path, json& out, const char* tag)
{
    std::ifstream f(fs::path(path), std::ios::binary);
    if (!f) {
        return false;
    }
    try {
        f >> out;
    } catch (const json::exception&) {
        MYE_LOG_WARN("[%s] JSON parse failed: %s", tag, WideToUtf8(path).c_str());
        return false;
    }
    return true;
}

// 隣接指定は 8 文字の x / o / . だけ。外れたものは「任意」へ落とす
bool IsValidNeighbors(const std::string& s)
{
    if (s.size() != kTileNeighborCount) {
        return false;
    }
    return std::all_of(s.begin(), s.end(), [](char c) { return c == 'x' || c == 'o' || c == kNeighborAny; });
}

} // namespace

// ==== TilePalette ====

const TileDef* TilePalette::Find(uint32_t id) const
{
    const auto it = std::lower_bound(tiles.begin(), tiles.end(), id,
                                     [](const TileDef& t, uint32_t v) { return t.id < v; });
    return (it != tiles.end() && it->id == id) ? &*it : nullptr;
}

uint32_t TilePalette::NextId() const
{
    const uint32_t maxId = tiles.empty() ? 0u : tiles.back().id;
    return (maxId >= kTileIdMax) ? 0u : maxId + 1u;
}

TileDef* TilePalette::Add(const std::string& tileName, AssetID sprite)
{
    const uint32_t id = NextId();
    if (id == 0) {
        return nullptr;
    }
    TileDef t;
    t.id = id;
    t.name = tileName;
    t.sprite = sprite;
    tiles.push_back(std::move(t));
    return &tiles.back();
}

namespace {

// 変種 1 つの neighbors が隣接に合うか
bool NeighborsMatch(const std::string& pattern, uint32_t tileId, const TileNeighbors& neighbors)
{
    if (pattern.size() != kTileNeighborCount) {
        return false;
    }
    for (size_t i = 0; i < kTileNeighborCount; ++i) {
        const uint32_t n = tilecell::TileId(neighbors[i]);
        if ((pattern[i] == 'x' && n != tileId) || (pattern[i] == 'o' && n != 0)) {
            return false;
        }
    }
    return true;
}

} // namespace

AssetID ResolveRuleVariant(const TilePalette& palette, uint32_t tileId, const TileNeighbors& neighbors)
{
    const TileDef* def = palette.Find(tileId);
    if (def == nullptr) {
        return AssetID{};
    }
    if (def->hasRule) {
        for (const TileVariant& v : def->variants) {
            if (NeighborsMatch(v.neighbors, tileId, neighbors)) {
                return v.sprite;
            }
        }
    }
    return def->sprite;
}

int32_t TileAnimFrameIndex(int32_t ticksPerFrame, size_t frameCount, uint64_t tick)
{
    if (frameCount == 0) {
        return -1;
    }
    const uint64_t perFrame = static_cast<uint64_t>((std::max)(ticksPerFrame, 1));
    return static_cast<int32_t>((tick / perFrame) % frameCount);
}

AssetID ResolveTileSprite(const TilePalette& palette, const TileDef& def, const TileNeighbors& neighbors, uint64_t tick)
{
    if (def.hasAnim) {
        const int32_t frame = TileAnimFrameIndex(def.ticksPerFrame, def.animFrames.size(), tick);
        if (frame >= 0) {
            return def.animFrames[static_cast<size_t>(frame)];
        }
    }
    return ResolveRuleVariant(palette, def.id, neighbors);
}

// ==== TilePaletteLibrary ====

uint64_t TilePaletteLibrary::HashForPath(const std::wstring& path)
{
    // 移動 / 改名済みアセットは .meta の GUID がキー (未移動は path-hash と同値)
    return assetkey::Resolve(NormalizePathKey(path));
}

uint64_t TilePaletteLibrary::Register(const std::wstring& path, TilePalette palette)
{
    const uint64_t hash = HashForPath(path);
    palette.hash = hash;
    palette.path = path;
    if (palette.name.empty()) {
        palette.name = NameFromPath(path, ".tilepalette");
    }
    palettes_[hash] = std::move(palette);
    ++epoch_;
    return hash;
}

uint64_t TilePaletteLibrary::LoadFromFile(const std::wstring& path)
{
    json j;
    if (!ReadJsonFile(path, j, "tilepalette")) {
        return 0;
    }
    TilePalette p;
    if (!FromJson(j, p)) {
        MYE_LOG_WARN("[tilepalette] not a tile palette: %s", WideToUtf8(path).c_str());
        return 0;
    }
    return Register(path, std::move(p));
}

const TilePalette* TilePaletteLibrary::Get(uint64_t hash) const
{
    const auto it = palettes_.find(hash);
    return it != palettes_.end() ? &it->second : nullptr;
}

std::vector<TilePaletteEntry> TilePaletteLibrary::Enumerate() const
{
    std::vector<TilePaletteEntry> out;
    out.reserve(palettes_.size());
    for (const auto& [hash, p] : palettes_) {
        out.push_back(TilePaletteEntry{ hash, p.name });
    }
    std::sort(out.begin(), out.end(), [](const TilePaletteEntry& a, const TilePaletteEntry& b) {
        if (a.name != b.name) {
            return a.name < b.name;
        }
        return a.hash < b.hash;
    });
    return out;
}

json TilePaletteLibrary::ToJson(const TilePalette& p)
{
    json j;
    j["engine"] = "MyEngine";
    j["tilepalette"] = 1;
    json tiles = json::array();
    for (const TileDef& t : p.tiles) {
        json tj;
        tj["id"] = t.id;
        tj["name"] = t.name;
        tj["sprite"] = t.sprite.value;
        tj["collider"] = t.collider;
        tj["oneWay"] = t.oneWay;
        tj["hp"] = t.hp;
        tj["destroyedTile"] = t.destroyedTile;
        tj["destroyEffect"] = t.destroyEffect.value;
        if (t.hasRule) {
            json variants = json::array();
            for (const TileVariant& v : t.variants) {
                variants.push_back({ { "neighbors", v.neighbors }, { "sprite", v.sprite.value } });
            }
            tj["rule"] = { { "variants", std::move(variants) } };
        }
        if (t.hasAnim) {
            json frames = json::array();
            for (const AssetID f : t.animFrames) {
                frames.push_back(f.value);
            }
            tj["anim"] = { { "ticksPerFrame", t.ticksPerFrame }, { "frames", std::move(frames) } };
        }
        tiles.push_back(std::move(tj));
    }
    j["tiles"] = std::move(tiles);
    return j;
}

bool TilePaletteLibrary::FromJson(const json& j, TilePalette& out)
{
    if (!j.is_object() || !j.contains("tilepalette")) {
        return false; // 種別キー必須 (別の資産を黙ってパレットに読まない)
    }
    out.tiles.clear();
    if (j.contains("tiles") && j["tiles"].is_array()) {
        for (const json& tj : j["tiles"]) {
            if (!tj.is_object()) {
                continue;
            }
            TileDef t;
            const int64_t id = tj.contains("id") && tj["id"].is_number() ? tj["id"].get<int64_t>() : 0;
            t.id = (id >= 1 && id <= kTileIdMax) ? static_cast<uint32_t>(id) : 0u;
            t.name = ReadString(tj, "name");
            t.sprite = AssetID{ ReadU64(tj, "sprite") };
            t.collider = ReadI32(tj, "collider", 0);
            t.oneWay = ReadBool(tj, "oneWay", false);
            t.hp = ReadI32(tj, "hp", 0);
            const int32_t destroyed = ReadI32(tj, "destroyedTile", 0);
            t.destroyedTile = (destroyed >= 0 && destroyed <= static_cast<int32_t>(kTileIdMax))
                                  ? static_cast<uint32_t>(destroyed) : 0u;
            t.destroyEffect = AssetID{ ReadU64(tj, "destroyEffect") };
            if (tj.contains("rule") && tj["rule"].is_object()) {
                t.hasRule = true;
                const json& rj = tj["rule"];
                if (rj.contains("variants") && rj["variants"].is_array()) {
                    for (const json& vj : rj["variants"]) {
                        if (!vj.is_object()) {
                            continue;
                        }
                        TileVariant v;
                        v.neighbors = ReadString(vj, "neighbors");
                        v.sprite = AssetID{ ReadU64(vj, "sprite") };
                        t.variants.push_back(std::move(v));
                    }
                }
            }
            if (tj.contains("anim") && tj["anim"].is_object()) {
                t.hasAnim = true;
                const json& aj = tj["anim"];
                t.ticksPerFrame = ReadI32(aj, "ticksPerFrame", 8);
                if (aj.contains("frames") && aj["frames"].is_array()) {
                    for (const json& fj : aj["frames"]) {
                        t.animFrames.push_back(AssetID{ fj.is_number_unsigned() ? fj.get<uint64_t>() : 0u });
                    }
                }
            }
            out.tiles.push_back(std::move(t));
        }
    }
    Sanitize(out);
    return true;
}

void TilePaletteLibrary::Sanitize(TilePalette& p)
{
    std::set<uint32_t> seen;
    std::vector<TileDef> kept;
    kept.reserve(p.tiles.size());
    for (TileDef& t : p.tiles) {
        if (t.id == 0 || t.id > kTileIdMax) {
            MYE_LOG_WARN("[tilepalette] tile id %u is out of 1..%u: dropped", t.id, kTileIdMax);
            continue;
        }
        if (!seen.insert(t.id).second) {
            MYE_LOG_WARN("[tilepalette] duplicate tile id %u: the first one is kept", t.id);
            continue;
        }
        t.collider = std::clamp(t.collider, 0, 1);
        t.hp = std::clamp(t.hp, 0, kMaxHp);
        t.ticksPerFrame = std::clamp(t.ticksPerFrame, 1, kMaxTicksPerFrame);
        for (TileVariant& v : t.variants) {
            if (!IsValidNeighbors(v.neighbors)) {
                MYE_LOG_WARN("[tilepalette] tile %u: neighbors \"%s\" is not 8 chars of x/o/.: treated as any",
                             t.id, v.neighbors.c_str());
                v.neighbors.assign(kTileNeighborCount, kNeighborAny);
            }
        }
        kept.push_back(std::move(t));
    }
    std::sort(kept.begin(), kept.end(), [](const TileDef& a, const TileDef& b) { return a.id < b.id; });
    p.tiles = std::move(kept);
}

// ==== TileChunk / TilemapLayer / TilemapAsset ====

bool TileChunk::IsEmpty() const
{
    return std::all_of(cells.begin(), cells.end(), [](uint32_t v) { return v == 0; });
}

uint32_t TilemapLayer::GetCell(int32_t x, int32_t y) const
{
    const ChunkKey key{ TilemapFloorDiv(y, kTilemapChunkSize), TilemapFloorDiv(x, kTilemapChunkSize) };
    const auto it = chunks.find(key);
    if (it == chunks.end()) {
        return 0;
    }
    const int32_t lx = TilemapFloorMod(x, kTilemapChunkSize);
    const int32_t ly = TilemapFloorMod(y, kTilemapChunkSize);
    return it->second.cells[static_cast<size_t>(ly) * kTilemapChunkSize + static_cast<size_t>(lx)];
}

void TilemapLayer::SetCell(int32_t x, int32_t y, uint32_t value)
{
    const ChunkKey key{ TilemapFloorDiv(y, kTilemapChunkSize), TilemapFloorDiv(x, kTilemapChunkSize) };
    const int32_t lx = TilemapFloorMod(x, kTilemapChunkSize);
    const int32_t ly = TilemapFloorMod(y, kTilemapChunkSize);
    const size_t idx = static_cast<size_t>(ly) * kTilemapChunkSize + static_cast<size_t>(lx);
    auto it = chunks.find(key);
    if (it == chunks.end()) {
        if (value == 0) {
            return;
        }
        it = chunks.emplace(key, TileChunk{}).first;
    }
    it->second.cells[idx] = value;
    if (value == 0 && it->second.IsEmpty()) {
        chunks.erase(it);
    }
}

size_t TilemapAsset::ChunkCount() const
{
    size_t n = 0;
    for (const TilemapLayer& l : layers) {
        n += l.chunks.size();
    }
    return n;
}

bool TilemapAsset::ChunkBounds(int32_t& minCx, int32_t& minCy, int32_t& maxCx, int32_t& maxCy) const
{
    bool any = false;
    for (const TilemapLayer& l : layers) {
        for (const auto& [key, chunk] : l.chunks) {
            if (!any) {
                minCx = maxCx = key.cx;
                minCy = maxCy = key.cy;
                any = true;
                continue;
            }
            minCx = (std::min)(minCx, key.cx);
            maxCx = (std::max)(maxCx, key.cx);
            minCy = (std::min)(minCy, key.cy);
            maxCy = (std::max)(maxCy, key.cy);
        }
    }
    return any;
}

// ==== TilemapLibrary ====

uint64_t TilemapLibrary::HashForPath(const std::wstring& path)
{
    return assetkey::Resolve(NormalizePathKey(path));
}

uint64_t TilemapLibrary::Register(const std::wstring& path, TilemapAsset map)
{
    const uint64_t hash = HashForPath(path);
    map.hash = hash;
    map.path = path;
    if (map.name.empty()) {
        map.name = NameFromPath(path, ".tilemap");
    }
    maps_[hash] = std::move(map);
    ++epoch_;
    return hash;
}

uint64_t TilemapLibrary::LoadFromFile(const std::wstring& path)
{
    json j;
    if (!ReadJsonFile(path, j, "tilemap")) {
        return 0;
    }
    TilemapAsset m;
    if (!FromJson(j, m)) {
        MYE_LOG_WARN("[tilemap] not a tilemap: %s", WideToUtf8(path).c_str());
        return 0;
    }
    return Register(path, std::move(m));
}

const TilemapAsset* TilemapLibrary::Get(uint64_t hash) const
{
    const auto it = maps_.find(hash);
    return it != maps_.end() ? &it->second : nullptr;
}

std::vector<TilemapEntry> TilemapLibrary::Enumerate() const
{
    std::vector<TilemapEntry> out;
    out.reserve(maps_.size());
    for (const auto& [hash, m] : maps_) {
        out.push_back(TilemapEntry{ hash, m.name });
    }
    std::sort(out.begin(), out.end(), [](const TilemapEntry& a, const TilemapEntry& b) {
        if (a.name != b.name) {
            return a.name < b.name;
        }
        return a.hash < b.hash;
    });
    return out;
}

json TilemapLibrary::ToJson(const TilemapAsset& m)
{
    json j;
    j["engine"] = "MyEngine";
    j["tilemap"] = 1;
    j["palette"] = m.palette;
    j["cellSize"] = json::array({ m.cellSize[0], m.cellSize[1] });
    json layers = json::array();
    for (const TilemapLayer& l : m.layers) {
        json lj;
        lj["name"] = l.name;
        lj["sortingLayerId"] = l.sortingLayerId;
        lj["orderInLayer"] = l.orderInLayer;
        lj["collision"] = l.collision;
        json chunks = json::array();
        for (const auto& [key, chunk] : l.chunks) {
            if (chunk.IsEmpty()) {
                continue; // 空のチャンクは書かない
            }
            json rows = json::array();
            for (int32_t ly = 0; ly < kTilemapChunkSize; ++ly) {
                json row = json::array();
                for (int32_t lx = 0; lx < kTilemapChunkSize; ++lx) {
                    row.push_back(chunk.cells[static_cast<size_t>(ly) * kTilemapChunkSize + static_cast<size_t>(lx)]);
                }
                rows.push_back(std::move(row));
            }
            chunks.push_back({ { "cx", key.cx }, { "cy", key.cy }, { "cells", std::move(rows) } });
        }
        lj["chunks"] = std::move(chunks);
        layers.push_back(std::move(lj));
    }
    j["layers"] = std::move(layers);
    return j;
}

bool TilemapLibrary::FromJson(const json& j, TilemapAsset& out)
{
    if (!j.is_object() || !j.contains("tilemap")) {
        return false;
    }
    out.palette = ReadU64(j, "palette");
    out.cellSize[0] = 1.0f;
    out.cellSize[1] = 1.0f;
    if (j.contains("cellSize") && j["cellSize"].is_array() && j["cellSize"].size() >= 2
        && j["cellSize"][0].is_number() && j["cellSize"][1].is_number()) {
        out.cellSize[0] = j["cellSize"][0].get<float>();
        out.cellSize[1] = j["cellSize"][1].get<float>();
    }
    out.layers.clear();
    if (j.contains("layers") && j["layers"].is_array()) {
        for (const json& lj : j["layers"]) {
            if (!lj.is_object()) {
                continue;
            }
            TilemapLayer l;
            l.name = ReadString(lj, "name");
            l.sortingLayerId = ReadI32(lj, "sortingLayerId", 0);
            l.orderInLayer = ReadI32(lj, "orderInLayer", 0);
            l.collision = ReadBool(lj, "collision", true);
            if (lj.contains("chunks") && lj["chunks"].is_array()) {
                for (const json& cj : lj["chunks"]) {
                    if (!cj.is_object() || !cj.contains("cells") || !cj["cells"].is_array()) {
                        continue;
                    }
                    TileChunk chunk;
                    const json& rows = cj["cells"];
                    for (size_t ly = 0; ly < rows.size() && ly < static_cast<size_t>(kTilemapChunkSize); ++ly) {
                        if (!rows[ly].is_array()) {
                            continue;
                        }
                        const json& row = rows[ly];
                        for (size_t lx = 0; lx < row.size() && lx < static_cast<size_t>(kTilemapChunkSize); ++lx) {
                            if (row[lx].is_number_unsigned()) {
                                chunk.cells[ly * kTilemapChunkSize + lx] = static_cast<uint32_t>(row[lx].get<uint64_t>());
                            }
                        }
                    }
                    l.chunks[ChunkKey{ ReadI32(cj, "cy", 0), ReadI32(cj, "cx", 0) }] = chunk;
                }
            }
            out.layers.push_back(std::move(l));
        }
    }
    Sanitize(out);
    return true;
}

void TilemapLibrary::Sanitize(TilemapAsset& m)
{
    for (float& c : m.cellSize) {
        c = std::isfinite(c) ? std::clamp(c, kMinCellSize, kMaxCellSize) : 1.0f;
    }
    if (m.layers.size() > kMaxTilemapLayers) {
        MYE_LOG_WARN("[tilemap] %zu layers; only the first %zu are kept", m.layers.size(), kMaxTilemapLayers);
        m.layers.resize(kMaxTilemapLayers);
    }
    for (TilemapLayer& l : m.layers) {
        for (auto it = l.chunks.begin(); it != l.chunks.end();) {
            it = it->second.IsEmpty() ? l.chunks.erase(it) : std::next(it);
        }
    }
}

// ==== モジュール注入 ====

namespace tilepalette {
namespace {
TilePaletteLibrary* sLibrary = nullptr;
} // namespace

void Install(TilePaletteLibrary* lib)
{
    sLibrary = lib;
}

TilePaletteLibrary* Library()
{
    return sLibrary;
}

const TilePalette* Resolve(uint64_t hash)
{
    return (sLibrary != nullptr && hash != 0) ? sLibrary->Get(hash) : nullptr;
}
} // namespace tilepalette

namespace tilemap {
namespace {
TilemapLibrary* sLibrary = nullptr;
} // namespace

void Install(TilemapLibrary* lib)
{
    sLibrary = lib;
}

TilemapLibrary* Library()
{
    return sLibrary;
}

const TilemapAsset* Resolve(AssetID id)
{
    return (sLibrary != nullptr && !id.IsNull()) ? sLibrary->Get(id.value) : nullptr;
}
} // namespace tilemap

} // namespace mye
