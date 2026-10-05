//====================================================================================
//                          NavMeshAsset.cpp
//  MyEngin/ 秋田蓮音                                                     10/03/2026
//                                          ナビメッシュ資産 (.mnav) の直列化とファイル入出力
//====================================================================================
#include "Engine/Engine/Navigation/NavMeshAsset.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>

#include "Engine/Core/Asset/AssetGuidResolver.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Util/ByteIo.h"
#include "Engine/Platform/PathUtil.h"

namespace fs = std::filesystem;

namespace mye {
namespace NavMeshAsset {
namespace {

// ディスク上で 'M','N','A','V' の順に並ぶ (x64 は LE 固定)
constexpr uint32_t kMagic = static_cast<uint32_t>('M') | (static_cast<uint32_t>('N') << 8)
    | (static_cast<uint32_t>('A') << 16) | (static_cast<uint32_t>('V') << 24);

// 壊れた blob の巨大な値で確保しないための上限
constexpr int32_t kMaxTileGrid = 4096;
constexpr int32_t kMaxLayers = 1 << 20;
constexpr int32_t kMaxLayerBytes = 1 << 24;
constexpr size_t kLinkRecordBytes = sizeof(uint64_t) + sizeof(float) * 7 + 2 + sizeof(uint32_t);

std::map<uint64_t, Data>& MemoryAssets()
{
    static std::map<uint64_t, Data> assets;
    return assets;
}

bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>& out)
{
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec) {
        return false;
    }
    std::ifstream f(fs::path{ path }, std::ios::binary);
    if (!f) {
        return false;
    }
    out.resize(static_cast<size_t>(size));
    if (!out.empty()) {
        f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
        if (f.gcount() != static_cast<std::streamsize>(out.size())) {
            return false;
        }
    }
    return true;
}

void WriteConfig(ByteWriter& w, const NavBakeConfig& c)
{
    // 並びは形式 1 の後ろに形式 2 の値を足したもの
    w.F32(c.cellSize);
    w.F32(c.cellHeight);
    w.I32(c.tileSize);
    w.F32(c.agentHeight);
    w.F32(c.agentRadius);
    w.F32(c.agentMaxClimb);
    w.F32(c.agentMaxSlopeDeg);
    w.F32(c.maxEdgeLen);
    w.F32(c.maxSimplificationError);
    w.I32(c.minRegionArea);
    w.I32(c.mergeRegionArea);
    for (int i = 0; i < 3; ++i) {
        w.F32(c.boundsMin[i]);
    }
    for (int i = 0; i < 3; ++i) {
        w.F32(c.boundsMax[i]);
    }
    w.I32(c.generateLinks);
    w.F32(c.linkDropHeight);
    w.F32(c.linkJumpDistance);
}

void ReadConfig(ByteReader& r, uint32_t version, NavBakeConfig& c)
{
    c.cellSize = r.F32();
    c.cellHeight = r.F32();
    c.tileSize = r.I32();
    c.agentHeight = r.F32();
    c.agentRadius = r.F32();
    c.agentMaxClimb = r.F32();
    c.agentMaxSlopeDeg = r.F32();
    c.maxEdgeLen = r.F32();
    c.maxSimplificationError = r.F32();
    c.minRegionArea = r.I32();
    c.mergeRegionArea = r.I32();
    for (int i = 0; i < 3; ++i) {
        c.boundsMin[i] = r.F32();
    }
    for (int i = 0; i < 3; ++i) {
        c.boundsMax[i] = r.F32();
    }
    if (version >= 2) {
        c.generateLinks = r.I32();
        c.linkDropHeight = r.F32();
        c.linkJumpDistance = r.F32();
    }
}

bool ConfigIsSane(const NavBakeConfig& c)
{
    const float values[] = { c.cellSize, c.cellHeight, c.agentHeight, c.agentRadius, c.agentMaxClimb,
                             c.agentMaxSlopeDeg, c.maxEdgeLen, c.maxSimplificationError, c.boundsMin[0],
                             c.boundsMin[1], c.boundsMin[2], c.boundsMax[0], c.boundsMax[1], c.boundsMax[2],
                             c.linkDropHeight, c.linkJumpDistance };
    for (const float v : values) {
        if (!std::isfinite(v)) {
            return false;
        }
    }
    return c.cellSize > 0.0f && c.cellHeight > 0.0f && c.tileSize >= 1 && c.tileSize <= 255;
}

} // namespace

void Serialize(const Data& d, std::vector<uint8_t>& out)
{
    std::vector<std::byte> buf;
    ByteWriter w(buf);
    w.U32(kMagic);
    w.U32(kVersion);
    w.U32(d.bakeVersion);
    w.U64(d.inputHash);
    WriteConfig(w, d.config);
    w.I32(d.tilesX);
    w.I32(d.tilesY);
    w.I32(d.maxTiles);
    w.I32(d.maxPolysPerTile);
    w.I32(d.maxObstacles);
    w.I32(d.inputTriangleCount);
    w.Count(d.layers.size());
    for (const LayerRecord& l : d.layers) {
        w.I32(l.tx);
        w.I32(l.ty);
        w.I32(l.layer);
        w.Blob(l.blob.data(), l.blob.size());
    }
    w.Count(d.links.size());
    for (const NavLinkSpec& l : d.links) {
        w.U64(l.key);
        for (int i = 0; i < 3; ++i) {
            w.F32(l.start[i]);
        }
        for (int i = 0; i < 3; ++i) {
            w.F32(l.end[i]);
        }
        w.F32(l.radius);
        w.U8(l.bidirectional);
        w.U8(l.area);
        w.U32(l.userId);
    }
    out.assign(reinterpret_cast<const uint8_t*>(buf.data()),
               reinterpret_cast<const uint8_t*>(buf.data()) + buf.size());
}

bool Deserialize(const std::vector<uint8_t>& in, Data& out)
{
    out = Data{};
    ByteReader r(reinterpret_cast<const std::byte*>(in.data()), in.size());
    const uint32_t magic = r.U32();
    const uint32_t version = r.U32();
    if (!r.Ok() || magic != kMagic || version < 1 || version > kVersion) {
        return false;
    }
    out.bakeVersion = r.U32();
    out.inputHash = r.U64();
    ReadConfig(r, version, out.config);
    out.tilesX = r.I32();
    out.tilesY = r.I32();
    out.maxTiles = r.I32();
    out.maxPolysPerTile = r.I32();
    out.maxObstacles = r.I32();
    out.inputTriangleCount = r.I32();
    if (!r.Ok() || !ConfigIsSane(out.config) || out.tilesX < 0 || out.tilesX > kMaxTileGrid || out.tilesY < 0
        || out.tilesY > kMaxTileGrid || out.maxPolysPerTile < 1 || out.maxObstacles < 0
        || out.inputTriangleCount < 0) {
        return false;
    }

    // 件数は resize 前に残りバイト数で検算する (壊れた blob の巨大な値をそのまま確保しない)
    const size_t layerCount = r.Count(sizeof(int32_t) * 3 + sizeof(uint64_t));
    if (!r.Ok() || layerCount > static_cast<size_t>(kMaxLayers) || out.maxTiles < static_cast<int32_t>(layerCount)) {
        return false;
    }
    out.layers.resize(layerCount);
    for (LayerRecord& l : out.layers) {
        l.tx = r.I32();
        l.ty = r.I32();
        l.layer = r.I32();
        const size_t bytes = r.Count(1);
        if (!r.Ok() || bytes > static_cast<size_t>(kMaxLayerBytes)) {
            return false;
        }
        l.blob.resize(bytes);
        r.Raw(l.blob.data(), bytes);
    }
    if (version >= 2) {
        const size_t linkCount = r.Count(kLinkRecordBytes);
        if (!r.Ok()) {
            return false;
        }
        out.links.resize(linkCount);
        for (size_t i = 0; i < linkCount; ++i) {
            NavLinkSpec& l = out.links[i];
            l.key = r.U64();
            for (int k = 0; k < 3; ++k) {
                l.start[k] = r.F32();
            }
            for (int k = 0; k < 3; ++k) {
                l.end[k] = r.F32();
            }
            l.radius = r.F32();
            l.bidirectional = r.U8();
            l.area = r.U8();
            l.userId = r.U32();
            // 生成した Link だけが入る。印・昇順・有限の座標・エリアの範囲を確かめる (壊れた値を store へ渡さない)
            bool finite = std::isfinite(l.radius);
            for (int k = 0; k < 3; ++k) {
                finite = finite && std::isfinite(l.start[k]) && std::isfinite(l.end[k]);
            }
            if (!r.Ok() || !finite || (l.key & kNavGeneratedLinkKeyBit) == 0
                || (l.userId & kNavGeneratedLinkUserIdBit) == 0 || l.area >= kNavAreaCountMax
                || (i > 0 && out.links[i - 1].key >= l.key)) {
                return false;
            }
        }
    }
    if (!r.Ok() || r.Remaining() != 0) {
        return false; // 余りがある = 別形式 / 継ぎ足し。丸ごと捨てる
    }

    // 境界検査: 層のキーが範囲内かつ昇順で、header が層のキーと一致すること
    for (size_t i = 0; i < out.layers.size(); ++i) {
        const LayerRecord& l = out.layers[i];
        if (l.tx < 0 || l.tx >= out.tilesX || l.ty < 0 || l.ty >= out.tilesY || l.layer < 0 || l.layer > 255
            || l.blob.size() < sizeof(dtTileCacheLayerHeader)) {
            return false;
        }
        if (i > 0) {
            const LayerRecord& p = out.layers[i - 1];
            const bool ascending = p.ty != l.ty ? p.ty < l.ty : (p.tx != l.tx ? p.tx < l.tx : p.layer < l.layer);
            if (!ascending) {
                return false;
            }
        }
        dtTileCacheLayerHeader header;
        std::memcpy(&header, l.blob.data(), sizeof(header));
        if (header.magic != DT_TILECACHE_MAGIC || header.version != DT_TILECACHE_VERSION || header.tx != l.tx
            || header.ty != l.ty || header.tlayer != l.layer) {
            return false;
        }
    }
    return true;
}

bool Save(const std::wstring& path, const Data& d)
{
    std::vector<uint8_t> blob;
    Serialize(d, blob);
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    const std::string_view bytes(reinterpret_cast<const char*>(blob.data()), blob.size());
    if (!WriteFileReplacing(path, bytes)) {
        MYE_LOG_ERROR("[nav] failed to write .mnav: %s", WideToUtf8(path).c_str());
        return false;
    }
    return true;
}

bool Load(const std::wstring& path, Data& out)
{
    std::vector<uint8_t> blob;
    if (!ReadWholeFile(path, blob)) {
        return false;
    }
    return Deserialize(blob, out);
}

void RegisterInMemory(uint64_t guid, Data data)
{
    MemoryAssets()[guid] = std::move(data);
}

bool LoadByGuid(uint64_t guid, Data& out)
{
    const auto it = MemoryAssets().find(guid);
    if (it != MemoryAssets().end()) {
        out = it->second;
        return true;
    }
    const std::wstring path = assetguid::ResolvePath(guid);
    return !path.empty() && Load(path, out);
}

NavTileStoreConfig MakeStoreConfig(const Data& d)
{
    return NavMakeStoreConfig(d.config, d.maxTiles, d.maxPolysPerTile, d.maxObstacles);
}

bool BuildStore(const Data& d, NavTileStore& store)
{
    if (!store.Init(MakeStoreConfig(d))) {
        return false;
    }
    for (const LayerRecord& l : d.layers) {
        if (!store.AddBaseLayer(l.blob.data(), static_cast<int>(l.blob.size()))) {
            return false;
        }
    }
    if (!store.BuildAll()) {
        return false;
    }
    return d.links.empty() || (store.ReplaceLinks(d.links) >= 0 && store.Commit());
}

} // namespace NavMeshAsset
} // namespace mye
