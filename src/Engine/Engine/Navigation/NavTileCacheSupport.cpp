//====================================================================================
//                          NavTileCacheSupport.cpp
//  MyEngin/ 秋田蓮音                                                     10/03/2026
//                                          NavMesh の TileCache 支援 (無圧縮・層の所有・状態の直列化・復元)
//====================================================================================
#include "Engine/Engine/Navigation/NavTileCacheSupport.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <utility>

#include "DetourAlloc.h"
#include "DetourCommon.h"
#include "DetourLocalBoundary.h"
#include "DetourTileCacheBuilder.h"
#include "Recast.h"

// dtLocalBoundary の private メンバーを読み書きする。PATCHES.md の friend 宣言に対応する
struct dtLocalBoundaryAccess {
    static void Write(const dtLocalBoundary& b, mye::NavByteWriter& w)
    {
        w.Bytes(b.m_center, sizeof(b.m_center));
        w.Pod(b.m_nsegs);
        for (int i = 0; i < b.m_nsegs; ++i) {
            w.Bytes(b.m_segs[i].s, sizeof(b.m_segs[i].s));
            w.Pod(b.m_segs[i].d);
        }
        w.Pod(b.m_npolys);
        for (int i = 0; i < b.m_npolys; ++i) {
            w.Pod(b.m_polys[i]);
        }
    }

    static bool Read(dtLocalBoundary& b, mye::NavByteReader& r)
    {
        b.reset();
        int nsegs = 0;
        if (!r.Bytes(b.m_center, sizeof(b.m_center)) || !r.Pod(nsegs)) {
            return false;
        }
        if (nsegs < 0 || nsegs > dtLocalBoundary::MAX_LOCAL_SEGS) {
            return false;
        }
        for (int i = 0; i < nsegs; ++i) {
            if (!r.Bytes(b.m_segs[i].s, sizeof(b.m_segs[i].s)) || !r.Pod(b.m_segs[i].d)) {
                return false;
            }
        }
        b.m_nsegs = nsegs;
        int npolys = 0;
        if (!r.Pod(npolys) || npolys < 0 || npolys > dtLocalBoundary::MAX_LOCAL_POLYS) {
            return false;
        }
        for (int i = 0; i < npolys; ++i) {
            if (!r.Pod(b.m_polys[i])) {
                return false;
            }
        }
        b.m_npolys = npolys;
        return true;
    }
};

namespace mye {
namespace {

constexpr uint32_t kStateMagic = 0x3153564Eu; // "NVS1"
constexpr uint32_t kStateVersion = 4; // v2: 障害物に yaw / v3: 障害物にエリア (塗り替え) / v4: Off-Mesh Link
constexpr uint32_t kCrowdMagic = 0x31574F52u; // "ROW1"
constexpr int kCrowdMaxPath = 256;            // dtCrowd の m_maxPathResult (MAX_PATH_RES)
constexpr int kUpdateGuard = 4096;            // dtTileCache::update の暴走防止
// dtTileCache::update が 1 回に積める tile 更新は 64 件 (MAX_UPDATE)、1 つの障害物が触れる層は 8 枚まで
// (DT_MAX_TOUCHED_TILES)。8 件を超えて積むと tile 更新が黙って落ちるので、8 件ごとに処理する
constexpr int kMaxQueuedRequests = 8;
constexpr int kMaxLayersPerTile = 32;
// 状態の読み込みで受け付ける Link の本数の上限 (壊れた blob が巨大な確保をさせないための歯止め)
constexpr uint32_t kMaxSavedLinks = 4096;
// NavTileStore::Generation の発番。store をまたいで一意にする (表示側が別の store の同じ番号と取り違えない)
std::atomic<uint64_t> gGenerationSequence{0};

uint64_t HashPod(uint64_t h, const void* p, size_t n) { return NavFnv1a(h, p, n); }

template <class T>
uint64_t HashValue(uint64_t h, const T& v) { return NavFnv1a(h, &v, sizeof(T)); }

bool KeyLess(const NavTileKey& a, int ty, int tx, int layer)
{
    if (a.ty != ty) return a.ty < ty;
    if (a.tx != tx) return a.tx < tx;
    return a.layer < layer;
}

// 2 つの XZ 範囲が重なるか
bool OverlapXZ(const float* aMin, const float* aMax, const float* bMin, const float* bMax)
{
    return aMin[0] <= bMax[0] && aMax[0] >= bMin[0] && aMin[2] <= bMax[2] && aMax[2] >= bMin[2];
}

// dtMeshTile の観測可能な内容を畳む。pool 内の添字 (firstLink / links[].next) や未使用の領域は含めない
uint64_t HashMeshTile(const dtNavMesh& nav, const dtMeshTile& tile, bool includeRefs, uint64_t h)
{
    const dtMeshHeader& hd = *tile.header;
    h = HashValue(h, hd);
    h = HashPod(h, tile.verts, sizeof(float) * 3 * static_cast<size_t>(hd.vertCount));
    for (int i = 0; i < hd.polyCount; ++i) {
        const dtPoly& p = tile.polys[i];
        h = HashPod(h, p.verts, sizeof(p.verts));
        h = HashPod(h, p.neis, sizeof(p.neis));
        h = HashValue(h, p.flags);
        h = HashValue(h, p.vertCount);
        h = HashValue(h, p.areaAndtype);
        for (unsigned int li = p.firstLink; li != DT_NULL_LINK; li = tile.links[li].next) {
            const dtLink& l = tile.links[li];
            const dtMeshTile* nt = nullptr;
            const dtPoly* np = nullptr;
            if (dtStatusFailed(nav.getTileAndPolyByRef(l.ref, &nt, &np))) {
                h = HashValue(h, static_cast<uint32_t>(0xDEADBEEFu));
                continue;
            }
            const int nx = nt->header->x;
            const int ny = nt->header->y;
            const int nl = nt->header->layer;
            const unsigned int ip = nav.decodePolyIdPoly(l.ref);
            h = HashValue(h, nx);
            h = HashValue(h, ny);
            h = HashValue(h, nl);
            h = HashValue(h, ip);
            h = HashValue(h, l.edge);
            h = HashValue(h, l.side);
            h = HashValue(h, l.bmin);
            h = HashValue(h, l.bmax);
            if (includeRefs) {
                h = HashValue(h, l.ref);
            }
        }
        h = HashValue(h, static_cast<uint32_t>(0xFFFFFFFFu)); // 連鎖の区切り
    }
    for (int i = 0; i < hd.offMeshConCount; ++i) {
        const dtOffMeshConnection& c = tile.offMeshCons[i];
        h = HashPod(h, c.pos, sizeof(c.pos));
        h = HashValue(h, c.rad);
        h = HashValue(h, c.poly);
        h = HashValue(h, c.flags);
        h = HashValue(h, c.side);
        h = HashValue(h, c.userId);
    }
    for (int i = 0; i < hd.detailMeshCount; ++i) {
        const dtPolyDetail& d = tile.detailMeshes[i];
        h = HashValue(h, d.vertBase);
        h = HashValue(h, d.triBase);
        h = HashValue(h, d.vertCount);
        h = HashValue(h, d.triCount);
    }
    h = HashPod(h, tile.detailVerts, sizeof(float) * 3 * static_cast<size_t>(hd.detailVertCount));
    h = HashPod(h, tile.detailTris, sizeof(unsigned char) * 4 * static_cast<size_t>(hd.detailTriCount));
    return h;
}

} // namespace

uint64_t NavFnv1a(uint64_t hash, const void* data, size_t size)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= p[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

void NavByteWriter::Bytes(const void* data, size_t size)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    buf_.insert(buf_.end(), p, p + size);
}

bool NavByteReader::Bytes(void* out, size_t size)
{
    if (!ok_ || size > size_ - pos_) {
        ok_ = false;
        return false;
    }
    if (size > 0) {
        std::memcpy(out, data_ + pos_, size);
    }
    pos_ += size;
    return true;
}

int NavRawCompressor::maxCompressedSize(const int bufferSize)
{
    return bufferSize;
}

dtStatus NavRawCompressor::compress(const unsigned char* buffer, const int bufferSize,
                                    unsigned char* compressed, const int maxCompressedSize, int* compressedSize)
{
    if (bufferSize > maxCompressedSize) {
        return DT_FAILURE | DT_BUFFER_TOO_SMALL;
    }
    std::memcpy(compressed, buffer, static_cast<size_t>(bufferSize));
    *compressedSize = bufferSize;
    return DT_SUCCESS;
}

dtStatus NavRawCompressor::decompress(const unsigned char* compressed, const int compressedSize,
                                      unsigned char* buffer, const int maxBufferSize, int* bufferSize)
{
    if (compressedSize > maxBufferSize) {
        return DT_FAILURE | DT_BUFFER_TOO_SMALL;
    }
    std::memcpy(buffer, compressed, static_cast<size_t>(compressedSize));
    *bufferSize = compressedSize;
    return DT_SUCCESS;
}

NavFixedAllocator::NavFixedAllocator(size_t capacity)
    : buffer_(static_cast<uint8_t*>(dtAlloc(capacity, DT_ALLOC_PERM))), capacity_(capacity)
{
}

NavFixedAllocator::~NavFixedAllocator()
{
    dtFree(buffer_);
}

void* NavFixedAllocator::alloc(const size_t size)
{
    // 8 バイト境界へ揃える
    const size_t aligned = (size + 7u) & ~static_cast<size_t>(7u);
    if (!buffer_ || top_ + aligned > capacity_) {
        return nullptr;
    }
    void* mem = buffer_ + top_;
    top_ += aligned;
    return mem;
}

void NavMeshProcess::process(struct dtNavMeshCreateParams* params, unsigned char* polyAreas, unsigned short* polyFlags)
{
    for (int i = 0; i < params->polyCount; ++i) {
        if (polyAreas[i] == DT_TILECACHE_WALKABLE_AREA) {
            polyAreas[i] = kNavAreaWalkable;
        }
        const int area = polyAreas[i];
        polyFlags[i] = (area == kNavAreaNotWalkable || area >= kNavAreaCountMax) ? 0 : static_cast<uint16_t>(1u << area);
    }
    rebuilt.push_back({params->tileX, params->tileY, params->tileLayer});

    // Off-Mesh Link。入口がこのタイルの範囲に入るものだけを dtCreateNavMeshData がタイルに持つ
    const size_t count = links != nullptr ? links->size() : 0;
    if (count == 0) {
        return;
    }
    conVerts_.resize(count * 6);
    conRad_.resize(count);
    conFlags_.resize(count);
    conAreas_.resize(count);
    conDir_.resize(count);
    conUserId_.resize(count);
    // 層の高さの範囲は地面の天面をセルの高さで量子化した値で、地面ぴったりの y の入口が範囲を僅かに外れて落ちることがある。
    // 登れる段差 + 1 セル以内の外れは範囲の端へ寄せる (入口の頂点は後で最寄りのポリゴン上の点へ吸着される)
    const float yTolerance = params->walkableClimb + params->ch;
    for (size_t i = 0; i < count; ++i) {
        const NavLinkSpec& link = (*links)[i];
        float* verts = &conVerts_[i * 6];
        std::memcpy(verts, link.start, sizeof(float) * 3);
        std::memcpy(verts + 3, link.end, sizeof(float) * 3);
        if (verts[1] < params->bmin[1] && verts[1] >= params->bmin[1] - yTolerance) {
            verts[1] = params->bmin[1];
        } else if (verts[1] > params->bmax[1] && verts[1] <= params->bmax[1] + yTolerance) {
            verts[1] = params->bmax[1];
        }
        conRad_[i] = link.radius;
        const int area = (std::min)(static_cast<int>(link.area), kNavAreaCountMax - 1);
        conAreas_[i] = static_cast<unsigned char>(area);
        conFlags_[i] = area == kNavAreaNotWalkable ? 0 : static_cast<unsigned short>(1u << area);
        conDir_[i] = link.bidirectional != 0 ? DT_OFFMESH_CON_BIDIR : 0;
        conUserId_[i] = link.userId;
    }
    params->offMeshConVerts = conVerts_.data();
    params->offMeshConRad = conRad_.data();
    params->offMeshConFlags = conFlags_.data();
    params->offMeshConAreas = conAreas_.data();
    params->offMeshConDir = conDir_.data();
    params->offMeshConUserID = conUserId_.data();
    params->offMeshConCount = static_cast<int>(count);
}

uint8_t NavAreaToLayerArea(int area)
{
    if (area <= kNavAreaWalkable) {
        return DT_TILECACHE_WALKABLE_AREA;
    }
    if (area == kNavAreaNotWalkable) {
        return DT_TILECACHE_NULL_AREA;
    }
    return static_cast<uint8_t>((std::min)(area, kNavAreaCountMax - 1));
}

// ---------------------------------------------------------------------------------
// NavTileStore
// ---------------------------------------------------------------------------------

NavTileStore::~NavTileStore()
{
    dtFreeTileCache(cache_);
    dtFreeNavMesh(nav_);
}

bool NavTileStore::Init(const NavTileStoreConfig& config)
{
    if (nav_ || config.mesh.maxTiles < config.cache.maxTiles) {
        return false;
    }
    config_ = config;
    process_.links = &links_;
    nav_ = dtAllocNavMesh();
    cache_ = dtAllocTileCache();
    if (!nav_ || !cache_) {
        return false;
    }
    if (dtStatusFailed(nav_->init(&config_.mesh))) {
        return false;
    }
    if (dtStatusFailed(cache_->init(&config_.cache, &allocator_, &compressor_, &process_))) {
        return false;
    }
    const uint32_t tileBits = dtIlog2(dtNextPow2(static_cast<unsigned int>(config.mesh.maxTiles)));
    const uint32_t polyBits = dtIlog2(dtNextPow2(static_cast<unsigned int>(config.mesh.maxPolys)));
    saltBits_ = (std::min)(31u, 32u - tileBits - polyBits);
    slotSalt_.assign(static_cast<size_t>(config.mesh.maxTiles), 1u);
    return true;
}

int NavTileStore::FindEntry(int tx, int ty, int layer) const
{
    const auto it = std::lower_bound(entries_.begin(), entries_.end(), 0,
        [&](const LayerEntry& e, int) { return KeyLess({e.tx, e.ty, e.layer}, ty, tx, layer); });
    if (it == entries_.end() || it->tx != tx || it->ty != ty || it->layer != layer) {
        return -1;
    }
    return static_cast<int>(it - entries_.begin());
}

bool NavTileStore::SampleSurfaceHeight(float x, float z, float yHint, float& outY) const
{
    const float tileSpan = static_cast<float>(config_.cache.width) * config_.cache.cs;
    if (!(tileSpan > 0.0f) || !std::isfinite(x) || !std::isfinite(z)) {
        return false;
    }
    const int tx = static_cast<int>(std::floor((x - config_.cache.orig[0]) / tileSpan));
    const int ty = static_cast<int>(std::floor((z - config_.cache.orig[2]) / tileSpan));
    const float cs = config_.cache.cs;
    const float ch = config_.cache.ch;
    constexpr uint8_t kNoHeight = 0xFF;
    // 隣のセルとの高さの差がこの勾配 (高さ / 水平距離) 以内なら同じ斜面とみなし、超えれば段差として勾配に使わない
    constexpr float kMaxSlopeGradient = 1.2f;
    const size_t headerSize = static_cast<size_t>(dtAlign4(sizeof(dtTileCacheLayerHeader)));

    bool found = false;
    float bestY = 0.0f;
    float bestGap = 0.0f;
    for (int layerIndex = 0;; ++layerIndex) {
        const int entryIndex = FindEntry(tx, ty, layerIndex);
        if (entryIndex < 0) {
            break;
        }
        const std::vector<uint8_t>& blob = entries_[static_cast<size_t>(entryIndex)].blob;
        if (blob.size() < headerSize) {
            continue;
        }
        dtTileCacheLayerHeader header;
        std::memcpy(&header, blob.data(), sizeof(header));
        const int w = header.width;
        const int h = header.height;
        const size_t gridSize = static_cast<size_t>(w) * static_cast<size_t>(h);
        if (blob.size() < headerSize + gridSize * 3u) {
            continue;
        }
        const uint8_t* heights = blob.data() + headerSize;
        const uint8_t* areas = heights + gridSize;
        const auto cellValue = [&](int ix, int iz) -> int {
            if (ix < 0 || iz < 0 || ix >= w || iz >= h) {
                return -1;
            }
            const size_t index = static_cast<size_t>(ix + iz * w);
            return (heights[index] == kNoHeight || areas[index] == DT_TILECACHE_NULL_AREA) ? -1 : heights[index];
        };
        // 隣のセルとの勾配 (高さ / 水平距離)。段差・縁・高さの無いセルは使わず、両隣が使えれば中央差分
        const auto gradient = [&](int value, int ix, int iz, int stepX, int stepZ) -> float {
            const int lo = cellValue(ix - stepX, iz - stepZ);
            const int hi = cellValue(ix + stepX, iz + stepZ);
            const float limit = kMaxSlopeGradient * cs / ch;
            const bool okLo = lo >= 0 && std::fabs(static_cast<float>(value - lo)) <= limit;
            const bool okHi = hi >= 0 && std::fabs(static_cast<float>(hi - value)) <= limit;
            if (okLo && okHi) {
                return static_cast<float>(hi - lo) * ch / (2.0f * cs);
            }
            if (okHi) {
                return static_cast<float>(hi - value) * ch / cs;
            }
            return okLo ? static_cast<float>(value - lo) * ch / cs : 0.0f;
        };
        const int cx = static_cast<int>(std::floor((x - header.bmin[0]) / cs));
        const int cz = static_cast<int>(std::floor((z - header.bmin[2]) / cs));

        // 真上のセルを優先し、高さが無い (縁・歩けない) ときだけ隣の 8 セルから借りる
        float layerY = 0.0f;
        bool layerFound = false;
        for (int ring = 0; ring <= 1 && !layerFound; ++ring) {
            float layerGap = 0.0f;
            for (int dz = -ring; dz <= ring; ++dz) {
                for (int dx = -ring; dx <= ring; ++dx) {
                    if (ring == 1 && dx == 0 && dz == 0) {
                        continue;
                    }
                    const int ix = cx + dx;
                    const int iz = cz + dz;
                    const int v = cellValue(ix, iz);
                    if (v < 0) {
                        continue;
                    }
                    // セルの値は、セルの範囲の面の最大高さを切り上げて (最低 1 セルの厚みで) 量子化したもの。
                    // 切り上げの平均 (半セル) と、斜面で範囲の隅が中心より高い分を引き、斜面に沿ってセル内の位置へ寄せる
                    const float gx = gradient(v, ix, iz, 1, 0);
                    const float gz = gradient(v, ix, iz, 0, 1);
                    const float centerX = header.bmin[0] + (static_cast<float>(ix) + 0.5f) * cs;
                    const float centerZ = header.bmin[2] + (static_cast<float>(iz) + 0.5f) * cs;
                    const float y = header.bmin[1] + static_cast<float>(v) * ch - 0.5f * ch
                                  - 0.5f * cs * (std::fabs(gx) + std::fabs(gz)) + gx * (x - centerX) + gz * (z - centerZ);
                    const float gap = std::fabs(y - yHint);
                    if (!layerFound || gap < layerGap) {
                        layerFound = true;
                        layerGap = gap;
                        layerY = y;
                    }
                }
            }
        }
        if (layerFound && (!found || std::fabs(layerY - yHint) < bestGap)) {
            found = true;
            bestGap = std::fabs(layerY - yHint);
            bestY = layerY;
        }
    }
    if (found) {
        outY = bestY;
    }
    return found;
}

int NavTileStore::FindBase(int tx, int ty, int layer) const
{
    for (size_t i = 0; i < baseBlobs_.size(); ++i) {
        const NavTileKey& k = baseBlobs_[i].key;
        if (k.tx == tx && k.ty == ty && k.layer == layer) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int NavTileStore::LowestFreeSlot() const
{
    std::vector<uint8_t> used(slotSalt_.size(), 0);
    for (const LayerEntry& e : entries_) {
        used[static_cast<size_t>(e.navSlot)] = 1;
    }
    for (size_t i = 0; i < used.size(); ++i) {
        if (!used[i]) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int NavTileStore::InsertEntry(const std::vector<uint8_t>& blob, bool isBase)
{
    if (blob.size() < sizeof(dtTileCacheLayerHeader)) {
        return -1;
    }
    const auto* h = reinterpret_cast<const dtTileCacheLayerHeader*>(blob.data());
    if (h->magic != DT_TILECACHE_MAGIC || h->version != DT_TILECACHE_VERSION) {
        return -1;
    }
    if (FindEntry(h->tx, h->ty, h->tlayer) >= 0) {
        return -1;
    }
    const int slot = LowestFreeSlot();
    if (slot < 0) {
        return -1;
    }
    LayerEntry entry;
    entry.tx = h->tx;
    entry.ty = h->ty;
    entry.layer = h->tlayer;
    entry.isBase = isBase;
    entry.navSlot = slot;
    entry.hash = NavFnv1a(kNavFnvSeed, blob.data(), blob.size());
    entry.blob = blob;
    const auto it = std::lower_bound(entries_.begin(), entries_.end(), 0,
        [&](const LayerEntry& e, int) { return KeyLess({e.tx, e.ty, e.layer}, entry.ty, entry.tx, entry.layer); });
    const auto pos = entries_.insert(it, std::move(entry));
    // 層のバッファは entries_ の要素と一緒に動くが、vector の中身 (ヒープ領域) は動かない
    if (dtStatusFailed(cache_->addTile(pos->blob.data(), static_cast<int>(pos->blob.size()), 0, &pos->cacheRef))) {
        entries_.erase(pos);
        return -1;
    }
    return static_cast<int>(pos - entries_.begin());
}

void NavTileStore::EraseGroup(int tx, int ty)
{
    for (size_t i = 0; i < entries_.size();) {
        LayerEntry& e = entries_[i];
        if (e.tx == tx && e.ty == ty) {
            cache_->removeTile(e.cacheRef, nullptr, nullptr);
            BumpSalt(e.navSlot);
            entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

void NavTileStore::BumpSalt(int slot)
{
    const uint32_t mask = (1u << saltBits_) - 1u;
    uint32_t& s = slotSalt_[static_cast<size_t>(slot)];
    s = (s + 1u) & mask;
    if (s == 0) {
        s = 1;
    }
}

bool NavTileStore::AddBaseLayer(const uint8_t* data, int size)
{
    if (!cache_ || size <= 0) {
        return false;
    }
    std::vector<uint8_t> blob(data, data + size);
    const int idx = InsertEntry(blob, true);
    if (idx < 0) {
        return false;
    }
    const LayerEntry& e = entries_[static_cast<size_t>(idx)];
    BaseBlob base;
    base.key = {e.tx, e.ty, e.layer};
    base.hash = e.hash;
    base.blob = std::move(blob);
    baseBlobs_.push_back(std::move(base));
    return true;
}

bool NavTileStore::BuildAll()
{
    bool ok = true;
    for (size_t i = 0; i < entries_.size(); ++i) {
        const LayerEntry& e = entries_[i];
        if (i > 0 && entries_[i - 1].tx == e.tx && entries_[i - 1].ty == e.ty) {
            continue; // 同じタイルの層はまとめて 1 回
        }
        if (dtStatusFailed(cache_->buildNavMeshTilesAt(e.tx, e.ty, nav_))) {
            ok = false;
        }
    }
    process_.rebuilt.clear();
    return Canonicalize() && ok;
}

bool NavTileStore::FlushUpdates()
{
    queuedRequests_ = 0;
    bool ok = true;
    bool upToDate = false;
    for (int guard = 0; !upToDate; ++guard) {
        if (guard > kUpdateGuard) {
            return false;
        }
        if (dtStatusFailed(cache_->update(0.0f, nav_, &upToDate))) {
            ok = false;
        }
    }
    return ok;
}

bool NavTileStore::QueueObstacle(ObstacleEntry& entry)
{
    // 溜めすぎる前に処理する (kMaxQueuedRequests の理由は定義側)
    if (queuedRequests_ >= kMaxQueuedRequests && !FlushUpdates()) {
        return false;
    }
    dtStatus st = DT_FAILURE;
    if (entry.type == DT_OBSTACLE_CYLINDER) {
        st = cache_->addObstacle(entry.v, entry.v[3], entry.v[4], &entry.ref);
    } else if (entry.type == DT_OBSTACLE_ORIENTED_BOX) {
        st = cache_->addBoxObstacle(entry.v, entry.v + 3, entry.yaw, &entry.ref);
    } else {
        st = cache_->addBoxObstacle(entry.v, entry.v + 3, &entry.ref);
    }
    if (dtStatusFailed(st)) {
        return false;
    }
    ++queuedRequests_;
    if (entry.area != kNavNoPaint
        && dtStatusFailed(cache_->setObstaclePaint(entry.ref, NavAreaToLayerArea(entry.area), entry.key))) {
        // 塗り替えにできなかった障害物は切り抜きになってしまうので、追加ごと取り消す
        cache_->removeObstacle(entry.ref);
        ++queuedRequests_;
        return false;
    }
    return true;
}

bool NavTileStore::QueueObstacleRemoval(dtObstacleRef ref)
{
    if (queuedRequests_ >= kMaxQueuedRequests && !FlushUpdates()) {
        return false;
    }
    if (dtStatusFailed(cache_->removeObstacle(ref))) {
        return false;
    }
    ++queuedRequests_;
    return true;
}

bool NavTileStore::AddBoxObstacle(uint64_t key, const float* bmin, const float* bmax, uint8_t area)
{
    ObstacleEntry e;
    e.key = key;
    e.area = area;
    e.type = DT_OBSTACLE_BOX;
    std::memcpy(e.v, bmin, sizeof(float) * 3);
    std::memcpy(e.v + 3, bmax, sizeof(float) * 3);
    const auto it = std::lower_bound(obstacles_.begin(), obstacles_.end(), key,
        [](const ObstacleEntry& o, uint64_t k) { return o.key < k; });
    if (it != obstacles_.end() && it->key == key) {
        return false;
    }
    if (!QueueObstacle(e)) {
        return false;
    }
    obstacles_.insert(it, e);
    return true;
}

bool NavTileStore::AddOrientedBoxObstacle(uint64_t key, const float* center, const float* halfExtents, float yawRadians,
                                           uint8_t area)
{
    ObstacleEntry e;
    e.key = key;
    e.area = area;
    e.type = DT_OBSTACLE_ORIENTED_BOX;
    std::memcpy(e.v, center, sizeof(float) * 3);
    std::memcpy(e.v + 3, halfExtents, sizeof(float) * 3);
    e.yaw = yawRadians;
    const auto it = std::lower_bound(obstacles_.begin(), obstacles_.end(), key,
        [](const ObstacleEntry& o, uint64_t k) { return o.key < k; });
    if (it != obstacles_.end() && it->key == key) {
        return false;
    }
    if (!QueueObstacle(e)) {
        return false;
    }
    obstacles_.insert(it, e);
    return true;
}

bool NavTileStore::AddCylinderObstacle(uint64_t key, const float* pos, float radius, float height, uint8_t area)
{
    ObstacleEntry e;
    e.key = key;
    e.area = area;
    e.type = DT_OBSTACLE_CYLINDER;
    std::memcpy(e.v, pos, sizeof(float) * 3);
    e.v[3] = radius;
    e.v[4] = height;
    const auto it = std::lower_bound(obstacles_.begin(), obstacles_.end(), key,
        [](const ObstacleEntry& o, uint64_t k) { return o.key < k; });
    if (it != obstacles_.end() && it->key == key) {
        return false;
    }
    if (!QueueObstacle(e)) {
        return false;
    }
    obstacles_.insert(it, e);
    return true;
}

bool NavTileStore::RemoveObstacle(uint64_t key)
{
    const auto it = std::lower_bound(obstacles_.begin(), obstacles_.end(), key,
        [](const ObstacleEntry& o, uint64_t k) { return o.key < k; });
    if (it == obstacles_.end() || it->key != key) {
        return false;
    }
    if (!QueueObstacleRemoval(it->ref)) {
        return false;
    }
    obstacles_.erase(it);
    return true;
}

namespace {

// 2 本の Link が同じか。exact = false なら入口・出口の動きが kLinkMoveThreshold 以内を同じとみなす
bool SameLink(const NavLinkSpec& a, const NavLinkSpec& b, bool exact)
{
    if (a.key != b.key || a.radius != b.radius || a.bidirectional != b.bidirectional || a.area != b.area
        || a.userId != b.userId) {
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        if (exact ? (a.start[i] != b.start[i] || a.end[i] != b.end[i])
                  : (std::fabs(a.start[i] - b.start[i]) > kLinkMoveThreshold
                     || std::fabs(a.end[i] - b.end[i]) > kLinkMoveThreshold)) {
            return false;
        }
    }
    return true;
}

bool ContainsSameLink(const std::vector<NavLinkSpec>& list, const NavLinkSpec& link, bool exact)
{
    const auto it = std::lower_bound(list.begin(), list.end(), link.key,
                                     [](const NavLinkSpec& l, uint64_t k) { return l.key < k; });
    return it != list.end() && SameLink(*it, link, exact);
}

} // namespace

bool NavTileStore::RebuildColumnsOfLinks(const std::vector<NavLinkSpec>& before, const std::vector<NavLinkSpec>& after)
{
    // 入口を持つタイル列 (dtCreateNavMeshData は入口の側のタイルにだけ Link を持つ)。前後で違う Link の入口のある列を作り直す
    const float tileSpan = static_cast<float>(config_.cache.width) * config_.cache.cs;
    std::vector<std::pair<int, int>> columns;
    const auto addColumn = [&](const NavLinkSpec& link) {
        const int tx = static_cast<int>(std::floor((link.start[0] - config_.cache.orig[0]) / tileSpan));
        const int ty = static_cast<int>(std::floor((link.start[2] - config_.cache.orig[2]) / tileSpan));
        columns.push_back({ ty, tx });
    };
    for (const NavLinkSpec& link : before) {
        if (!ContainsSameLink(after, link, true)) {
            addColumn(link);
        }
    }
    for (const NavLinkSpec& link : after) {
        if (!ContainsSameLink(before, link, true)) {
            addColumn(link);
        }
    }
    std::sort(columns.begin(), columns.end());
    columns.erase(std::unique(columns.begin(), columns.end()), columns.end());
    bool ok = true;
    for (const auto& column : columns) {
        if (dtStatusFailed(cache_->buildNavMeshTilesAt(column.second, column.first, nav_))) {
            ok = false;
        }
    }
    return ok;
}

int NavTileStore::ReplaceLinks(const std::vector<NavLinkSpec>& wanted)
{
    if (!cache_) {
        return -1;
    }
    // 動きが閾値以内の Link は前回の値を残す (毎 tick の小さな揺れで、保存した状態とずれたり作り直したりしない)
    std::vector<NavLinkSpec> next;
    next.reserve(wanted.size());
    int changed = 0;
    for (const NavLinkSpec& link : wanted) {
        const auto it = std::lower_bound(links_.begin(), links_.end(), link.key,
                                         [](const NavLinkSpec& l, uint64_t k) { return l.key < k; });
        if (it != links_.end() && SameLink(*it, link, false)) {
            next.push_back(*it);
        } else {
            next.push_back(link);
            ++changed;
        }
    }
    for (const NavLinkSpec& link : links_) {
        // 外れた Link (別の値へ変わった Link は上で数え済み)
        const auto it = std::lower_bound(next.begin(), next.end(), link.key,
                                         [](const NavLinkSpec& l, uint64_t k) { return l.key < k; });
        if (it == next.end() || it->key != link.key) {
            ++changed;
        }
    }
    if (changed == 0) {
        return 0;
    }
    if (!FlushUpdates()) {
        return -1;
    }
    const std::vector<NavLinkSpec> before = std::move(links_);
    links_ = std::move(next);
    return RebuildColumnsOfLinks(before, links_) ? changed : -1;
}

int NavTileStore::ConnectedLinkCount() const
{
    const dtNavMesh* nav = nav_;
    int count = 0;
    for (int i = 0; nav != nullptr && i < nav->getMaxTiles(); ++i) {
        const dtMeshTile* tile = nav->getTile(i);
        if (tile == nullptr || tile->header == nullptr) {
            continue;
        }
        for (int c = 0; c < tile->header->offMeshConCount; ++c) {
            // 入口の側 (edge 0) と出口の側 (edge 1) の両方が歩行面につながったものだけを数える
            bool hasStart = false;
            bool hasEnd = false;
            const dtPoly& poly = tile->polys[tile->offMeshCons[c].poly];
            for (unsigned int li = poly.firstLink; li != DT_NULL_LINK; li = tile->links[li].next) {
                hasStart = hasStart || tile->links[li].edge == 0;
                hasEnd = hasEnd || tile->links[li].edge == 1;
            }
            count += hasStart && hasEnd ? 1 : 0;
        }
    }
    return count;
}

uint64_t NavTileStore::HashLinks() const
{
    uint64_t h = kNavFnvSeed;
    for (const NavLinkSpec& l : links_) {
        h = HashValue(h, l.key);
        h = NavFnv1a(h, l.start, sizeof(l.start));
        h = NavFnv1a(h, l.end, sizeof(l.end));
        h = HashValue(h, l.radius);
        h = HashValue(h, l.bidirectional);
        h = HashValue(h, l.area);
        h = HashValue(h, l.userId);
    }
    return h;
}

bool NavTileStore::ReplaceTileLayers(int tx, int ty, const std::vector<std::vector<uint8_t>>& layers)
{
    return ReplaceGroup(tx, ty, layers, std::vector<uint8_t>(layers.size(), 0));
}

bool NavTileStore::ReplaceGroup(int tx, int ty, const std::vector<std::vector<uint8_t>>& layers,
                                const std::vector<uint8_t>& baseFlags)
{
    if (!cache_ || layers.size() != baseFlags.size() || layers.size() > static_cast<size_t>(kMaxLayersPerTile)) {
        return false;
    }
    if (!FlushUpdates()) {
        return false;
    }

    // この列 (tx, ty) に重なる障害物は、層を入れ替える前に外す。残すと touched が古い層の ref を指し、
    // 新しい層には効かず、後で外すときにも失敗する
    const float tileSpan = static_cast<float>(config_.cache.width) * config_.cache.cs;
    const float colMin[3] = {config_.cache.orig[0] + static_cast<float>(tx) * tileSpan, 0.0f,
                             config_.cache.orig[2] + static_cast<float>(ty) * tileSpan};
    const float colMax[3] = {colMin[0] + tileSpan, 0.0f, colMin[2] + tileSpan};
    std::vector<size_t> touched;
    for (size_t i = 0; i < obstacles_.size(); ++i) {
        const dtTileCacheObstacle* ob = cache_->getObstacleByRef(obstacles_[i].ref);
        if (!ob) {
            return false;
        }
        float bmin[3];
        float bmax[3];
        cache_->getObstacleBounds(ob, bmin, bmax);
        if (OverlapXZ(bmin, bmax, colMin, colMax)) {
            touched.push_back(i);
            if (!QueueObstacleRemoval(obstacles_[i].ref)) {
                return false;
            }
        }
    }
    if (!touched.empty() && !FlushUpdates()) {
        return false;
    }

    EraseGroup(tx, ty);
    for (size_t i = 0; i < layers.size(); ++i) {
        const int idx = InsertEntry(layers[i], baseFlags[i] != 0);
        if (idx < 0 || entries_[static_cast<size_t>(idx)].tx != tx || entries_[static_cast<size_t>(idx)].ty != ty) {
            return false;
        }
    }
    if (dtStatusFailed(cache_->buildNavMeshTilesAt(tx, ty, nav_))) {
        return false;
    }

    for (const size_t i : touched) {
        if (!QueueObstacle(obstacles_[i])) {
            return false;
        }
    }
    return true;
}

bool NavTileStore::ConsumeRebuilt(bool bump)
{
    bool anyChange = false;
    std::vector<uint8_t> changed(entries_.size(), 0);
    for (const NavTileKey& k : process_.rebuilt) {
        const int idx = FindEntry(k.tx, k.ty, k.layer);
        if (idx >= 0) {
            changed[static_cast<size_t>(idx)] = 1;
        }
    }
    process_.rebuilt.clear();
    for (size_t i = 0; i < entries_.size(); ++i) {
        LayerEntry& e = entries_[i];
        const bool present = nav_->getTileAt(e.tx, e.ty, e.layer) != nullptr;
        if (changed[i] || present != e.navPresent) {
            anyChange = true;
            if (bump) {
                BumpSalt(e.navSlot);
            }
        }
        e.navPresent = present;
    }
    return anyChange;
}

bool NavTileStore::Canonicalize()
{
    if (!canonicalize_) {
        return true;
    }
    struct Pending {
        int slot;
        dtTileRef ref;
        unsigned char* data;
        int size;
    };
    std::vector<Pending> pending;
    for (LayerEntry& e : entries_) {
        const dtMeshTile* tile = nav_->getTileAt(e.tx, e.ty, e.layer);
        e.navPresent = tile != nullptr;
        if (!tile) {
            continue;
        }
        auto* copy = static_cast<unsigned char*>(dtAlloc(static_cast<size_t>(tile->dataSize), DT_ALLOC_PERM));
        if (!copy) {
            return false;
        }
        std::memcpy(copy, tile->data, static_cast<size_t>(tile->dataSize));
        pending.push_back({e.navSlot, nav_->encodePolyId(slotSalt_[static_cast<size_t>(e.navSlot)],
                                                         static_cast<unsigned int>(e.navSlot), 0),
                           copy, tile->dataSize});
    }
    const dtNavMesh* constNav = nav_; // 非 const の getTile は private
    for (int i = 0; i < constNav->getMaxTiles(); ++i) {
        const dtMeshTile* tile = constNav->getTile(i);
        if (tile->header) {
            nav_->removeTile(nav_->getTileRef(tile), nullptr, nullptr);
        }
    }
    std::sort(pending.begin(), pending.end(), [](const Pending& a, const Pending& b) { return a.slot < b.slot; });
    bool ok = true;
    for (const Pending& p : pending) {
        if (dtStatusFailed(nav_->addTile(p.data, p.size, DT_TILE_FREE_DATA, p.ref, nullptr))) {
            dtFree(p.data);
            ok = false;
        }
    }
    generation_ = ++gGenerationSequence;
    return ok;
}

bool NavTileStore::Commit()
{
    bool ok = FlushUpdates();
    // どのタイルも作り直されず、有無も変わらなければ dtNavMesh は前回の正規化のまま (入れ直しても同じ形になる)
    if (!ConsumeRebuilt(true)) {
        return ok;
    }
    return Canonicalize() && ok;
}

void NavTileStore::SaveState(NavByteWriter& w, bool includeBaseLayers) const
{
    w.Pod(kStateMagic);
    w.Pod(kStateVersion);
    w.Pod(static_cast<uint32_t>(slotSalt_.size()));
    w.Bytes(slotSalt_.data(), slotSalt_.size() * sizeof(uint32_t));
    w.Pod(static_cast<uint32_t>(entries_.size()));
    for (const LayerEntry& e : entries_) {
        w.Pod(static_cast<int32_t>(e.tx));
        w.Pod(static_cast<int32_t>(e.ty));
        w.Pod(static_cast<int32_t>(e.layer));
        w.Pod(static_cast<uint8_t>(e.isBase ? 1 : 0));
        w.Pod(static_cast<int32_t>(e.navSlot));
        w.Pod(static_cast<uint8_t>(e.navPresent ? 1 : 0));
        w.Pod(e.hash);
        const bool withBytes = !e.isBase || includeBaseLayers;
        w.Pod(static_cast<uint8_t>(withBytes ? 1 : 0));
        if (withBytes) {
            w.Pod(static_cast<uint32_t>(e.blob.size()));
            w.Bytes(e.blob.data(), e.blob.size());
        }
    }
    w.Pod(static_cast<uint32_t>(obstacles_.size()));
    for (const ObstacleEntry& o : obstacles_) {
        w.Pod(o.key);
        w.Pod(o.type);
        w.Bytes(o.v, sizeof(o.v));
        w.Pod(o.yaw);
        w.Pod(o.area);
    }
    w.Pod(static_cast<uint32_t>(links_.size()));
    for (const NavLinkSpec& l : links_) {
        w.Pod(l.key);
        w.Bytes(l.start, sizeof(l.start));
        w.Bytes(l.end, sizeof(l.end));
        w.Pod(l.radius);
        w.Pod(l.bidirectional);
        w.Pod(l.area);
        w.Pod(l.userId);
    }
}

bool NavTileStore::LoadState(NavByteReader& r)
{
    uint32_t magic = 0;
    uint32_t version = 0;
    uint32_t slotCount = 0;
    if (!r.Pod(magic) || !r.Pod(version) || !r.Pod(slotCount) || magic != kStateMagic || version != kStateVersion
        || slotCount != slotSalt_.size()) {
        return false;
    }
    std::vector<uint32_t> savedSalt(slotCount);
    if (!r.Bytes(savedSalt.data(), savedSalt.size() * sizeof(uint32_t))) {
        return false;
    }
    uint32_t layerCount = 0;
    if (!r.Pod(layerCount) || layerCount > slotCount) {
        return false;
    }
    std::vector<SavedLayer> saved(layerCount);
    for (SavedLayer& s : saved) {
        int32_t tx = 0, ty = 0, layer = 0, slot = 0;
        uint8_t isBase = 0, present = 0, withBytes = 0;
        if (!r.Pod(tx) || !r.Pod(ty) || !r.Pod(layer) || !r.Pod(isBase) || !r.Pod(slot) || !r.Pod(present)
            || !r.Pod(s.hash) || !r.Pod(withBytes)) {
            return false;
        }
        s.key = {tx, ty, layer};
        s.isBase = isBase != 0;
        s.navSlot = slot;
        s.navPresent = present != 0;
        if (slot < 0 || static_cast<uint32_t>(slot) >= slotCount) {
            return false;
        }
        if (withBytes) {
            uint32_t size = 0;
            if (!r.Pod(size) || size > r.Remaining()) {
                return false;
            }
            s.blob.resize(size);
            if (!r.Bytes(s.blob.data(), size)) {
                return false;
            }
        }
    }
    uint32_t obstacleCount = 0;
    if (!r.Pod(obstacleCount) || obstacleCount > static_cast<uint32_t>(config_.cache.maxObstacles)) {
        return false;
    }
    std::vector<ObstacleEntry> savedObstacles(obstacleCount);
    for (ObstacleEntry& o : savedObstacles) {
        if (!r.Pod(o.key) || !r.Pod(o.type) || !r.Bytes(o.v, sizeof(o.v)) || !r.Pod(o.yaw) || !r.Pod(o.area)
            || (o.area != kNavNoPaint && o.area >= kNavAreaCountMax)) {
            return false;
        }
    }

    uint32_t linkCount = 0;
    if (!r.Pod(linkCount) || linkCount > kMaxSavedLinks) {
        return false;
    }
    std::vector<NavLinkSpec> savedLinks(linkCount);
    for (NavLinkSpec& l : savedLinks) {
        if (!r.Pod(l.key) || !r.Bytes(l.start, sizeof(l.start)) || !r.Bytes(l.end, sizeof(l.end)) || !r.Pod(l.radius)
            || !r.Pod(l.bidirectional) || !r.Pod(l.area) || !r.Pod(l.userId) || l.area >= kNavAreaCountMax) {
            return false;
        }
    }
    for (size_t i = 1; i < savedLinks.size(); ++i) {
        if (savedLinks[i - 1].key >= savedLinks[i].key) {
            return false; // key 昇順・重複なし
        }
    }

    bool dirty = false;
    bool ok = FlushUpdates();

    // Link: 層を入れ替えるタイル (下の ReplaceGroup) も新しい Link で作るよう、先に差し替える。
    // 入口の列の作り直しは障害物の復元の後に行う
    const std::vector<NavLinkSpec> oldLinks = links_;
    bool sameLinks = oldLinks.size() == savedLinks.size();
    for (size_t i = 0; sameLinks && i < oldLinks.size(); ++i) {
        sameLinks = SameLink(oldLinks[i], savedLinks[i], true);
    }
    if (!sameLinks) {
        links_ = savedLinks;
    }

    // (ty, tx) の組を現在と保存の和集合で、キー順に処理する
    std::vector<std::pair<int, int>> groups;
    for (const LayerEntry& e : entries_) {
        groups.push_back({e.ty, e.tx});
    }
    for (const SavedLayer& s : saved) {
        groups.push_back({s.key.ty, s.key.tx});
    }
    std::sort(groups.begin(), groups.end());
    groups.erase(std::unique(groups.begin(), groups.end()), groups.end());
    for (const auto& g : groups) {
        const int ty = g.first;
        const int tx = g.second;
        std::vector<const SavedLayer*> want;
        for (const SavedLayer& s : saved) {
            if (s.key.tx == tx && s.key.ty == ty) {
                want.push_back(&s);
            }
        }
        std::sort(want.begin(), want.end(), [](const SavedLayer* a, const SavedLayer* b) { return a->key.layer < b->key.layer; });
        std::vector<const LayerEntry*> have;
        for (const LayerEntry& e : entries_) {
            if (e.tx == tx && e.ty == ty) {
                have.push_back(&e);
            }
        }
        bool identical = want.size() == have.size();
        for (size_t i = 0; identical && i < want.size(); ++i) {
            const int bi = FindBase(tx, ty, want[i]->key.layer);
            identical = want[i]->key.layer == have[i]->layer && want[i]->isBase == have[i]->isBase
                && have[i]->hash == want[i]->hash && (!want[i]->isBase || bi >= 0);
        }
        if (identical) {
            continue;
        }
        std::vector<std::vector<uint8_t>> layers;
        std::vector<uint8_t> baseFlags;
        for (const SavedLayer* s : want) {
            if (s->isBase) {
                const int bi = FindBase(tx, ty, s->key.layer);
                if (bi < 0 || baseBlobs_[static_cast<size_t>(bi)].hash != s->hash) {
                    return false;
                }
                layers.push_back(baseBlobs_[static_cast<size_t>(bi)].blob);
            } else {
                if (s->blob.empty() || NavFnv1a(kNavFnvSeed, s->blob.data(), s->blob.size()) != s->hash) {
                    return false;
                }
                layers.push_back(s->blob);
            }
            baseFlags.push_back(s->isBase ? 1 : 0);
        }
        if (!ReplaceGroup(tx, ty, layers, baseFlags)) {
            return false;
        }
        dirty = true;
    }

    // 障害物: 同じなら触らない (毎 tick の復元を安く保つ)
    bool sameObstacles = savedObstacles.size() == obstacles_.size();
    for (size_t i = 0; sameObstacles && i < savedObstacles.size(); ++i) {
        sameObstacles = savedObstacles[i].key == obstacles_[i].key && savedObstacles[i].type == obstacles_[i].type
            && std::memcmp(savedObstacles[i].v, obstacles_[i].v, sizeof(float) * 6) == 0
            && std::memcmp(&savedObstacles[i].yaw, &obstacles_[i].yaw, sizeof(float)) == 0
            && savedObstacles[i].area == obstacles_[i].area;
    }
    if (!sameObstacles) {
        for (const ObstacleEntry& o : obstacles_) {
            if (!QueueObstacleRemoval(o.ref)) {
                return false;
            }
        }
        obstacles_.clear();
        ok = FlushUpdates() && ok;
        for (ObstacleEntry o : savedObstacles) {
            if (!QueueObstacle(o)) {
                return false;
            }
            obstacles_.push_back(o);
        }
        ok = FlushUpdates() && ok;
        dirty = true;
    }

    if (!sameLinks) {
        ok = FlushUpdates() && ok;
        if (!RebuildColumnsOfLinks(oldLinks, links_)) {
            return false;
        }
        dirty = true;
    }

    // スロット表と salt を保存したものに合わせる
    std::vector<uint8_t> usedSlot(slotCount, 0);
    for (const SavedLayer& s : saved) {
        const int idx = FindEntry(s.key.tx, s.key.ty, s.key.layer);
        if (idx < 0 || usedSlot[static_cast<size_t>(s.navSlot)]) {
            return false;
        }
        usedSlot[static_cast<size_t>(s.navSlot)] = 1;
        if (entries_[static_cast<size_t>(idx)].navSlot != s.navSlot) {
            dirty = true;
        }
    }
    if (savedSalt != slotSalt_) {
        dirty = true;
    }
    if (!dirty) {
        process_.rebuilt.clear();
        return ok;
    }
    for (const SavedLayer& s : saved) {
        entries_[static_cast<size_t>(FindEntry(s.key.tx, s.key.ty, s.key.layer))].navSlot = s.navSlot;
    }
    slotSalt_ = savedSalt;
    process_.rebuilt.clear();
    ok = Canonicalize() && ok;
    for (const SavedLayer& s : saved) {
        if (entries_[static_cast<size_t>(FindEntry(s.key.tx, s.key.ty, s.key.layer))].navPresent != s.navPresent) {
            return false;
        }
    }
    return ok;
}

uint64_t NavTileStore::HashNavMesh(bool includeRefs) const
{
    uint64_t h = kNavFnvSeed;
    for (size_t i = 0; i < entries_.size(); ++i) {
        const LayerEntry& e = entries_[i];
        h = HashValue(h, e.tx);
        h = HashValue(h, e.ty);
        h = HashValue(h, e.layer);
        const dtMeshTile* tile = nav_->getTileAt(e.tx, e.ty, e.layer);
        if (!tile) {
            h = HashValue(h, static_cast<uint32_t>(0));
            continue;
        }
        h = HashMeshTile(*nav_, *tile, includeRefs, h);
        if (includeRefs) {
            h = HashValue(h, nav_->getTileRef(tile));
        }
        // posLookup の連鎖順 (getTilesAt の返す順) も観測できる内容
        if (i == 0 || entries_[i - 1].tx != e.tx || entries_[i - 1].ty != e.ty) {
            const dtMeshTile* tiles[kMaxLayersPerTile];
            const int n = nav_->getTilesAt(e.tx, e.ty, tiles, kMaxLayersPerTile);
            for (int j = 0; j < n; ++j) {
                h = HashValue(h, tiles[j]->header->layer);
                if (includeRefs) {
                    h = HashValue(h, nav_->getTileRef(tiles[j]));
                }
            }
        }
    }
    return h;
}

uint64_t NavTileStore::HashLayers() const
{
    uint64_t h = kNavFnvSeed;
    for (const LayerEntry& e : entries_) {
        h = HashValue(h, e.tx);
        h = HashValue(h, e.ty);
        h = HashValue(h, e.layer);
        h = HashValue(h, e.hash); // 層の中身の FNV (登録時に計算済み)。毎 tick 全バイトを畳まない
    }
    return h;
}

uint64_t NavTileStore::HashObstacles() const
{
    uint64_t h = kNavFnvSeed;
    for (const ObstacleEntry& o : obstacles_) {
        h = HashValue(h, o.key);
        h = HashValue(h, o.type);
        h = NavFnv1a(h, o.v, sizeof(o.v));
        h = HashValue(h, o.yaw);
        h = HashValue(h, o.area);
    }
    return h;
}

// ---------------------------------------------------------------------------------
// ベイク
// ---------------------------------------------------------------------------------

void NavCalcTileGrid(const NavBakeConfig& config, int& outTilesX, int& outTilesY)
{
    int gw = 0;
    int gh = 0;
    rcCalcGridSize(config.boundsMin, config.boundsMax, config.cellSize, &gw, &gh);
    outTilesX = (gw + config.tileSize - 1) / config.tileSize;
    outTilesY = (gh + config.tileSize - 1) / config.tileSize;
}

int NavTileBorderCells(const NavBakeConfig& config)
{
    return static_cast<int>(std::ceil(config.agentRadius / config.cellSize)) + 3;
}

NavTileStoreConfig NavMakeStoreConfig(const NavBakeConfig& config, int maxTiles, int maxPolysPerTile, int maxObstacles)
{
    NavTileStoreConfig out;
    rcVcopy(out.cache.orig, config.boundsMin);
    out.cache.cs = config.cellSize;
    out.cache.ch = config.cellHeight;
    out.cache.width = config.tileSize;
    out.cache.height = config.tileSize;
    out.cache.walkableHeight = config.agentHeight;
    out.cache.walkableRadius = config.agentRadius;
    out.cache.walkableClimb = config.agentMaxClimb;
    out.cache.maxSimplificationError = config.maxSimplificationError;
    out.cache.maxTiles = maxTiles;
    out.cache.maxObstacles = maxObstacles;
    rcVcopy(out.mesh.orig, config.boundsMin);
    out.mesh.tileWidth = static_cast<float>(config.tileSize) * config.cellSize;
    out.mesh.tileHeight = static_cast<float>(config.tileSize) * config.cellSize;
    out.mesh.maxTiles = maxTiles;
    out.mesh.maxPolys = maxPolysPerTile;
    return out;
}

namespace {

struct BakeScratch {
    rcHeightfield* solid = nullptr;
    rcCompactHeightfield* chf = nullptr;
    rcHeightfieldLayerSet* lset = nullptr;
    std::vector<unsigned char> areas;

    ~BakeScratch()
    {
        rcFreeHeightField(solid);
        rcFreeCompactHeightfield(chf);
        rcFreeHeightfieldLayerSet(lset);
    }
};

} // namespace

namespace {

// 箱のどれにも入らない歩行面を歩けなくする (M84b)。フィルタの後・コンパクト化の前に呼ぶので、
// 箱の縁は壁と同じく agentRadius だけ削られる。箱が重なる所・接する所には縁ができない
void ClipWalkableToBoxes(rcHeightfield& solid, const NavBakeClipBox* boxes, int boxCount)
{
    for (int z = 0; z < solid.height; ++z) {
        const float cz = solid.bmin[2] + (static_cast<float>(z) + 0.5f) * solid.cs;
        for (int x = 0; x < solid.width; ++x) {
            const float cx = solid.bmin[0] + (static_cast<float>(x) + 0.5f) * solid.cs;
            for (rcSpan* span = solid.spans[x + z * solid.width]; span != nullptr; span = span->next) {
                if (span->area == RC_NULL_AREA) {
                    continue;
                }
                const float top = solid.bmin[1] + static_cast<float>(span->smax) * solid.ch;
                bool inside = false;
                for (int b = 0; b < boxCount && !inside; ++b) {
                    const NavBakeClipBox& box = boxes[b];
                    inside = cx >= box.boundsMin[0] && cx <= box.boundsMax[0] && cz >= box.boundsMin[2]
                        && cz <= box.boundsMax[2] && top >= box.boundsMin[1] && top <= box.boundsMax[1];
                }
                if (!inside) {
                    span->area = RC_NULL_AREA;
                }
            }
        }
    }
}

} // namespace

bool NavBakeTileLayers(const NavBakeConfig& config, const NavTriangleInput& input, int tx, int ty,
                       std::vector<std::vector<uint8_t>>& outLayers, const NavBakeClipBox* clipBoxes,
                       int clipBoxCount)
{
    outLayers.clear();
    rcContext ctx(false);
    NavRawCompressor compressor;

    rcConfig cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.cs = config.cellSize;
    cfg.ch = config.cellHeight;
    cfg.walkableSlopeAngle = config.agentMaxSlopeDeg;
    cfg.walkableHeight = static_cast<int>(std::ceil(config.agentHeight / cfg.ch));
    cfg.walkableClimb = static_cast<int>(std::floor(config.agentMaxClimb / cfg.ch));
    cfg.walkableRadius = static_cast<int>(std::ceil(config.agentRadius / cfg.cs));
    cfg.maxEdgeLen = static_cast<int>(config.maxEdgeLen / config.cellSize);
    cfg.maxSimplificationError = config.maxSimplificationError;
    cfg.minRegionArea = config.minRegionArea;
    cfg.mergeRegionArea = config.mergeRegionArea;
    cfg.maxVertsPerPoly = DT_VERTS_PER_POLYGON;
    cfg.tileSize = config.tileSize;
    cfg.borderSize = NavTileBorderCells(config); // 隣のタイルとの継ぎ目に必要な余白 (walkableRadius + 3)
    cfg.width = cfg.tileSize + cfg.borderSize * 2;
    cfg.height = cfg.tileSize + cfg.borderSize * 2;

    const float tileSpan = static_cast<float>(cfg.tileSize) * cfg.cs;
    cfg.bmin[0] = config.boundsMin[0] + static_cast<float>(tx) * tileSpan - static_cast<float>(cfg.borderSize) * cfg.cs;
    cfg.bmin[1] = config.boundsMin[1];
    cfg.bmin[2] = config.boundsMin[2] + static_cast<float>(ty) * tileSpan - static_cast<float>(cfg.borderSize) * cfg.cs;
    cfg.bmax[0] = config.boundsMin[0] + static_cast<float>(tx + 1) * tileSpan + static_cast<float>(cfg.borderSize) * cfg.cs;
    cfg.bmax[1] = config.boundsMax[1];
    cfg.bmax[2] = config.boundsMin[2] + static_cast<float>(ty + 1) * tileSpan + static_cast<float>(cfg.borderSize) * cfg.cs;

    BakeScratch s;
    s.solid = rcAllocHeightfield();
    if (!s.solid || !rcCreateHeightfield(&ctx, *s.solid, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch)) {
        return false;
    }
    s.areas.assign(static_cast<size_t>(input.triCount), 0);
    rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, input.verts, input.vertCount, input.tris, input.triCount,
                            s.areas.data());
    if (!rcRasterizeTriangles(&ctx, input.verts, input.vertCount, input.tris, s.areas.data(), input.triCount,
                              *s.solid, cfg.walkableClimb)) {
        return false;
    }
    rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *s.solid);
    rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *s.solid);
    rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *s.solid);
    if (clipBoxCount > 0) {
        ClipWalkableToBoxes(*s.solid, clipBoxes, clipBoxCount);
    }

    s.chf = rcAllocCompactHeightfield();
    if (!s.chf || !rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *s.solid, *s.chf)) {
        return false;
    }
    if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *s.chf)) {
        return false;
    }
    s.lset = rcAllocHeightfieldLayerSet();
    if (!s.lset || !rcBuildHeightfieldLayers(&ctx, *s.chf, cfg.borderSize, cfg.walkableHeight, *s.lset)) {
        return false;
    }

    for (int i = 0; i < s.lset->nlayers; ++i) {
        const rcHeightfieldLayer& layer = s.lset->layers[i];
        dtTileCacheLayerHeader header;
        std::memset(&header, 0, sizeof(header));
        header.magic = DT_TILECACHE_MAGIC;
        header.version = DT_TILECACHE_VERSION;
        header.tx = tx;
        header.ty = ty;
        header.tlayer = i;
        dtVcopy(header.bmin, layer.bmin);
        dtVcopy(header.bmax, layer.bmax);
        header.width = static_cast<unsigned char>(layer.width);
        header.height = static_cast<unsigned char>(layer.height);
        header.minx = static_cast<unsigned char>(layer.minx);
        header.maxx = static_cast<unsigned char>(layer.maxx);
        header.miny = static_cast<unsigned char>(layer.miny);
        header.maxy = static_cast<unsigned char>(layer.maxy);
        header.hmin = static_cast<unsigned short>(layer.hmin);
        header.hmax = static_cast<unsigned short>(layer.hmax);

        unsigned char* data = nullptr;
        int dataSize = 0;
        if (dtStatusFailed(dtBuildTileCacheLayer(&compressor, &header, layer.heights, layer.areas, layer.cons,
                                                 &data, &dataSize))) {
            outLayers.clear();
            return false;
        }
        outLayers.emplace_back(data, data + dataSize);
        dtFree(data);
    }
    return true;
}

// ---------------------------------------------------------------------------------
// dtCrowd
// ---------------------------------------------------------------------------------

bool NavSaveCrowd(dtCrowd& crowd, NavByteWriter& w)
{
    const int count = crowd.getAgentCount();
    w.Pod(kCrowdMagic);
    w.Pod(static_cast<int32_t>(count));
    for (int i = 0; i < count; ++i) {
        const dtCrowdAgent* ag = crowd.getAgent(i);
        w.Pod(static_cast<uint8_t>(ag->active ? 1 : 0));
        if (!ag->active) {
            continue;
        }
        // 経路要求が途中の状態は dtPathQueue (private) に残るので表せない
        if (ag->targetState == DT_CROWDAGENT_TARGET_WAITING_FOR_PATH) {
            return false;
        }
        w.Pod(ag->state);
        w.Pod(static_cast<uint8_t>(ag->partial ? 1 : 0));
        const int npath = ag->corridor.getPathCount();
        w.Pod(static_cast<int32_t>(npath));
        w.Bytes(ag->corridor.getPos(), sizeof(float) * 3);
        w.Bytes(ag->corridor.getTarget(), sizeof(float) * 3);
        w.Bytes(ag->corridor.getPath(), sizeof(dtPolyRef) * static_cast<size_t>(npath));
        dtLocalBoundaryAccess::Write(ag->boundary, w);
        w.Pod(ag->topologyOptTime);
        w.Pod(static_cast<int32_t>(ag->nneis));
        for (int k = 0; k < ag->nneis; ++k) {
            w.Pod(ag->neis[k].idx);
            w.Pod(ag->neis[k].dist);
        }
        w.Pod(ag->desiredSpeed);
        w.Bytes(ag->npos, sizeof(float) * 3);
        w.Bytes(ag->disp, sizeof(float) * 3);
        w.Bytes(ag->dvel, sizeof(float) * 3);
        w.Bytes(ag->nvel, sizeof(float) * 3);
        w.Bytes(ag->vel, sizeof(float) * 3);
        const dtCrowdAgentParams& p = ag->params;
        w.Pod(p.radius);
        w.Pod(p.height);
        w.Pod(p.maxAcceleration);
        w.Pod(p.maxSpeed);
        w.Pod(p.collisionQueryRange);
        w.Pod(p.pathOptimizationRange);
        w.Pod(p.separationWeight);
        w.Pod(p.updateFlags);
        w.Pod(p.obstacleAvoidanceType);
        w.Pod(p.queryFilterType);
        w.Pod(p.avoidancePriority); // M84d
        w.Pod(static_cast<int32_t>(ag->ncorners));
        w.Bytes(ag->cornerVerts, sizeof(float) * 3 * static_cast<size_t>(ag->ncorners));
        w.Bytes(ag->cornerFlags, static_cast<size_t>(ag->ncorners));
        w.Bytes(ag->cornerPolys, sizeof(dtPolyRef) * static_cast<size_t>(ag->ncorners));
        w.Pod(ag->targetState);
        w.Pod(ag->targetRef);
        w.Bytes(ag->targetPos, sizeof(float) * 3);
        w.Pod(ag->targetPathqRef);
        w.Pod(static_cast<uint8_t>(ag->targetReplan ? 1 : 0));
        w.Pod(ag->targetReplanTime);
    }
    return true;
}

bool NavLoadCrowd(dtCrowd& crowd, NavByteReader& r)
{
    uint32_t magic = 0;
    int32_t count = 0;
    if (!r.Pod(magic) || !r.Pod(count) || magic != kCrowdMagic || count != crowd.getAgentCount()) {
        return false;
    }
    for (int i = 0; i < count; ++i) {
        if (crowd.getAgent(i)->active) {
            crowd.removeAgent(i);
        }
    }
    // addAgent は空きスロットの最小番号を返す。歯抜けは仮のエージェントで埋めて最後に外す
    std::vector<int> fillers;
    const float origin[3] = {0.0f, 0.0f, 0.0f};
    dtCrowdAgentParams fillParams;
    std::memset(&fillParams, 0, sizeof(fillParams));
    fillParams.radius = 0.1f;
    fillParams.height = 0.1f;
    fillParams.maxSpeed = 1.0f;
    fillParams.maxAcceleration = 1.0f;
    fillParams.collisionQueryRange = 1.0f;
    fillParams.pathOptimizationRange = 1.0f;
    for (int i = 0; i < count; ++i) {
        uint8_t active = 0;
        if (!r.Pod(active)) {
            return false;
        }
        if (!active) {
            if (crowd.addAgent(origin, &fillParams) != i) {
                return false;
            }
            fillers.push_back(i);
            continue;
        }
        if (crowd.addAgent(origin, &fillParams) != i) {
            return false;
        }
        dtCrowdAgent* ag = crowd.getEditableAgent(i);
        uint8_t partial = 0;
        int32_t npath = 0;
        float pos[3];
        float target[3];
        if (!r.Pod(ag->state) || !r.Pod(partial) || !r.Pod(npath) || npath < 1 || npath > kCrowdMaxPath
            || !r.Bytes(pos, sizeof(pos)) || !r.Bytes(target, sizeof(target))) {
            return false;
        }
        dtPolyRef path[kCrowdMaxPath];
        if (!r.Bytes(path, sizeof(dtPolyRef) * static_cast<size_t>(npath))) {
            return false;
        }
        ag->partial = partial != 0;
        ag->corridor.reset(path[0], pos);
        ag->corridor.setCorridor(target, path, npath);
        if (!dtLocalBoundaryAccess::Read(ag->boundary, r)) {
            return false;
        }
        int32_t nneis = 0;
        if (!r.Pod(ag->topologyOptTime) || !r.Pod(nneis) || nneis < 0 || nneis > DT_CROWDAGENT_MAX_NEIGHBOURS) {
            return false;
        }
        ag->nneis = nneis;
        for (int k = 0; k < nneis; ++k) {
            if (!r.Pod(ag->neis[k].idx) || !r.Pod(ag->neis[k].dist)) {
                return false;
            }
        }
        dtCrowdAgentParams p = ag->params;
        int32_t ncorners = 0;
        uint8_t replan = 0;
        if (!r.Pod(ag->desiredSpeed) || !r.Bytes(ag->npos, sizeof(float) * 3) || !r.Bytes(ag->disp, sizeof(float) * 3)
            || !r.Bytes(ag->dvel, sizeof(float) * 3) || !r.Bytes(ag->nvel, sizeof(float) * 3)
            || !r.Bytes(ag->vel, sizeof(float) * 3) || !r.Pod(p.radius) || !r.Pod(p.height)
            || !r.Pod(p.maxAcceleration) || !r.Pod(p.maxSpeed) || !r.Pod(p.collisionQueryRange)
            || !r.Pod(p.pathOptimizationRange) || !r.Pod(p.separationWeight) || !r.Pod(p.updateFlags)
            || !r.Pod(p.obstacleAvoidanceType) || !r.Pod(p.queryFilterType) || !r.Pod(p.avoidancePriority)
            || !r.Pod(ncorners) || ncorners < 0
            || ncorners > DT_CROWDAGENT_MAX_CORNERS) {
            return false;
        }
        p.userData = nullptr;
        ag->params = p;
        ag->ncorners = ncorners;
        if (!r.Bytes(ag->cornerVerts, sizeof(float) * 3 * static_cast<size_t>(ncorners))
            || !r.Bytes(ag->cornerFlags, static_cast<size_t>(ncorners))
            || !r.Bytes(ag->cornerPolys, sizeof(dtPolyRef) * static_cast<size_t>(ncorners))
            || !r.Pod(ag->targetState) || !r.Pod(ag->targetRef) || !r.Bytes(ag->targetPos, sizeof(float) * 3)
            || !r.Pod(ag->targetPathqRef) || !r.Pod(replan) || !r.Pod(ag->targetReplanTime)) {
            return false;
        }
        ag->targetReplan = replan != 0;
        ag->active = true;
    }
    for (const int idx : fillers) {
        crowd.removeAgent(idx);
    }
    return true;
}

uint64_t NavHashCrowd(dtCrowd& crowd)
{
    uint64_t h = kNavFnvSeed;
    const int count = crowd.getAgentCount();
    for (int i = 0; i < count; ++i) {
        const dtCrowdAgent* ag = crowd.getAgent(i);
        if (!ag->active) {
            continue;
        }
        h = HashValue(h, i);
        h = HashValue(h, ag->state);
        h = HashValue(h, ag->targetState);
        h = HashValue(h, ag->partial);
        h = HashValue(h, ag->ncorners);
        h = HashPod(h, ag->npos, sizeof(float) * 3);
        h = HashPod(h, ag->vel, sizeof(float) * 3);
        h = HashPod(h, ag->dvel, sizeof(float) * 3);
        h = HashPod(h, ag->nvel, sizeof(float) * 3);
        h = HashPod(h, ag->corridor.getTarget(), sizeof(float) * 3);
        const int npath = ag->corridor.getPathCount();
        h = HashValue(h, npath);
        h = HashPod(h, ag->corridor.getPath(), sizeof(dtPolyRef) * static_cast<size_t>(npath));
    }
    return h;
}

} // namespace mye
