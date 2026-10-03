//====================================================================================
//                          NavTileCacheSupport.h
//  MyEngin/ 秋田蓮音                                                     10/03/2026
//                                          NavMesh の TileCache 支援 (無圧縮・層の所有・状態の直列化・復元)
//====================================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "DetourCrowd.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"
#include "DetourNavMeshQuery.h"
#include "DetourTileCache.h"
#include "DetourTileCacheBuilder.h"

namespace mye {

// エリア ID。TileCache の層では 0 = 通行不可 (障害物の切り抜き) / 63 = 歩行可なので、
// NavMeshProcess が 63 -> kNavAreaWalkable へ写す。
constexpr uint8_t kNavAreaWalkable = 0;
constexpr uint8_t kNavAreaNotWalkable = 1;
constexpr uint16_t kNavFlagWalk = 0x01;

constexpr uint64_t kNavFnvSeed = 14695981039346656037ull;

// FNV-1a 64bit。ビット列をそのまま畳む (浮動小数の -0.0 と 0.0 も区別する)
uint64_t NavFnv1a(uint64_t hash, const void* data, size_t size);

// 状態の直列化に使う最小のバイト列ライター / リーダー
class NavByteWriter {
public:
    void Bytes(const void* data, size_t size);
    template <class T>
    void Pod(const T& value) { Bytes(&value, sizeof(T)); }
    const std::vector<uint8_t>& Data() const { return buf_; }
    size_t Size() const { return buf_.size(); }

private:
    std::vector<uint8_t> buf_;
};

class NavByteReader {
public:
    NavByteReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}
    bool Bytes(void* out, size_t size);
    template <class T>
    bool Pod(T& value) { return Bytes(&value, sizeof(T)); }
    bool Ok() const { return ok_; }
    size_t Remaining() const { return size_ - pos_; }

private:
    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
    bool ok_ = true;
};

// 無圧縮の dtTileCacheCompressor。圧縮ライブラリ (FastLZ) を入れず、層のバイト列をそのまま持つ
class NavRawCompressor final : public dtTileCacheCompressor {
public:
    int maxCompressedSize(const int bufferSize) override;
    dtStatus compress(const unsigned char* buffer, const int bufferSize,
                      unsigned char* compressed, const int maxCompressedSize, int* compressedSize) override;
    dtStatus decompress(const unsigned char* compressed, const int compressedSize,
                        unsigned char* buffer, const int maxBufferSize, int* bufferSize) override;
};

// 固定容量の線形アロケータ。dtTileCache が 1 タイルのビルドごとに reset() する
class NavFixedAllocator final : public dtTileCacheAlloc {
public:
    explicit NavFixedAllocator(size_t capacity);
    ~NavFixedAllocator() override;
    void reset() override { top_ = 0; }
    void* alloc(const size_t size) override;
    void free(void*) override {}

private:
    uint8_t* buffer_;
    size_t capacity_;
    size_t top_ = 0;
};

struct NavTileKey {
    int tx = 0;
    int ty = 0;
    int layer = 0;
};

// 層からポリゴンメッシュを作るたびに呼ばれ、エリア ID -> フラグの写像と再構築したタイルの記録をする
class NavMeshProcess final : public dtTileCacheMeshProcess {
public:
    void process(struct dtNavMeshCreateParams* params, unsigned char* polyAreas, unsigned short* polyFlags) override;
    std::vector<NavTileKey> rebuilt;
};

struct NavTileStoreConfig {
    dtTileCacheParams cache{};
    dtNavMeshParams mesh{};
};

// ベイク結果 (TileCache の層) と dtNavMesh / dtTileCache の所有者。
// 層は呼び出し側のバッファを借りず、コピーして持つ (将来の実行時再ベイクの層もアセット由来の層も同じ寿命)。
//
// 状態は「層 (差し替えたものだけ) + 障害物 + スロット表 (層 -> dtNavMesh のタイル番号) + スロットごとの salt」。
// dtNavMesh のリンク順は addTile / removeTile の履歴に依存するので、変更のたびに Commit() が
// 全タイルを層のキー順・スロット番号順に入れ直して履歴を消す (docs\adr\ADR-023-navmesh.md)。
// これで SaveState() -> LoadState() が連続実行と一致する。
class NavTileStore {
public:
    NavTileStore() = default;
    ~NavTileStore();
    NavTileStore(const NavTileStore&) = delete;
    NavTileStore& operator=(const NavTileStore&) = delete;

    bool Init(const NavTileStoreConfig& config);

    // 初回ベイクの層を追加する (アセット由来 = base)。BuildAll() の前に全部入れる
    bool AddBaseLayer(const uint8_t* data, int size);
    bool BuildAll();

    // タイル差し替え口: (tx, ty) の層を全部 layers へ入れ替える (空なら撤去)。
    // 重なる障害物は一度外して付け直す。dtNavMesh への反映は Commit() で確定する。
    bool ReplaceTileLayers(int tx, int ty, const std::vector<std::vector<uint8_t>>& layers);

    // 障害物。key は呼び出し側の決定的なキー (エンティティキー)。dtNavMesh への反映は Commit()
    bool AddBoxObstacle(uint64_t key, const float* bmin, const float* bmax);
    bool AddCylinderObstacle(uint64_t key, const float* pos, float radius, float height);
    bool RemoveObstacle(uint64_t key);

    // tick 境界の同期確定: 要求を全部処理して dtNavMesh を正規化する
    bool Commit();

    // 状態の保存と復元。includeBaseLayers = false なら差し替えた層だけを書く
    void SaveState(NavByteWriter& writer, bool includeBaseLayers) const;
    bool LoadState(NavByteReader& reader);

    // dtNavMesh の観測可能な内容のハッシュ。includeRefs = false なら salt / スロット番号を除く (リンク順の比較用)
    uint64_t HashNavMesh(bool includeRefs) const;
    uint64_t HashLayers() const;

    // 試験用: false にすると Commit() が履歴を消さない (履歴依存の実証に使う)
    void SetCanonicalize(bool enable) { canonicalize_ = enable; }

    dtNavMesh* NavMesh() { return nav_; }
    const dtNavMesh* NavMesh() const { return nav_; }
    dtTileCache* TileCache() { return cache_; }
    int LayerCount() const { return static_cast<int>(entries_.size()); }
    int ObstacleCount() const { return static_cast<int>(obstacles_.size()); }
    uint32_t SaltOfSlot(int slot) const { return slotSalt_[static_cast<size_t>(slot)]; }
    uint32_t SaltBits() const { return saltBits_; }

private:
    struct LayerEntry {
        int tx = 0, ty = 0, layer = 0;
        bool isBase = false;
        int navSlot = -1;
        bool navPresent = false;
        dtCompressedTileRef cacheRef = 0;
        uint64_t hash = 0;
        std::vector<uint8_t> blob;
    };
    struct BaseBlob {
        NavTileKey key;
        uint64_t hash = 0;
        std::vector<uint8_t> blob;
    };
    struct ObstacleEntry {
        uint64_t key = 0;
        uint8_t type = 0;
        float v[6] = {};
        dtObstacleRef ref = 0;
    };
    struct SavedLayer {
        NavTileKey key;
        bool isBase = false;
        int navSlot = -1;
        bool navPresent = false;
        uint64_t hash = 0;
        std::vector<uint8_t> blob;
    };

    int FindEntry(int tx, int ty, int layer) const;
    int FindBase(int tx, int ty, int layer) const;
    int LowestFreeSlot() const;
    int InsertEntry(const std::vector<uint8_t>& blob, bool isBase);
    void EraseGroup(int tx, int ty);
    bool FlushUpdates();
    bool QueueObstacle(ObstacleEntry& entry);
    bool QueueObstacleRemoval(dtObstacleRef ref);
    void BumpSalt(int slot);
    void ConsumeRebuilt(bool bump);
    bool Canonicalize();
    bool ReplaceGroup(int tx, int ty, const std::vector<std::vector<uint8_t>>& layers,
                      const std::vector<uint8_t>& baseFlags);

    NavTileStoreConfig config_{};
    NavRawCompressor compressor_;
    NavFixedAllocator allocator_{256 * 1024};
    NavMeshProcess process_;
    dtNavMesh* nav_ = nullptr;
    dtTileCache* cache_ = nullptr;
    uint32_t saltBits_ = 0;
    bool canonicalize_ = true;
    int queuedRequests_ = 0;
    std::vector<LayerEntry> entries_;
    std::vector<BaseBlob> baseBlobs_;
    std::vector<ObstacleEntry> obstacles_;
    std::vector<uint32_t> slotSalt_;
};

// ---- ベイク (層の生成) ----

struct NavBakeConfig {
    float cellSize = 0.3f;
    float cellHeight = 0.2f;
    int tileSize = 32;
    float agentHeight = 1.8f;
    float agentRadius = 0.3f;
    float agentMaxClimb = 0.4f;
    float agentMaxSlopeDeg = 45.0f;
    float maxEdgeLen = 12.0f;
    float maxSimplificationError = 1.3f;
    int minRegionArea = 8;
    int mergeRegionArea = 20;
    float boundsMin[3] = {};
    float boundsMax[3] = {};
};

struct NavTriangleInput {
    const float* verts = nullptr; // xyz * vertCount
    int vertCount = 0;
    const int* tris = nullptr;    // 3 * triCount
    int triCount = 0;
};

// 入力から (tx, ty) 1 枚分のタイルの層を作る。層ごとのバイト列 (header 付き、無圧縮) を返す。
// 空のタイルは 0 枚で成功。失敗なら false
bool NavBakeTileLayers(const NavBakeConfig& config, const NavTriangleInput& input, int tx, int ty,
                       std::vector<std::vector<uint8_t>>& outLayers);

void NavCalcTileGrid(const NavBakeConfig& config, int& outTilesX, int& outTilesY);

// タイルの継ぎ目のために、タイル 1 枚のベイクが入力へ広げる余白 (セル数)。
// 入力三角形をタイルごとに絞るときは、この分だけ広げた範囲で絞ること
int NavTileBorderCells(const NavBakeConfig& config);

// ベイク設定から NavTileStore の設定を作る
NavTileStoreConfig NavMakeStoreConfig(const NavBakeConfig& config, int maxTiles, int maxPolysPerTile, int maxObstacles);

// ---- dtCrowd の状態 ----

// 全エージェントの状態を書く。経路要求が途中のエージェントがいると false (PATCHES.md の MAX_ITERS_PER_UPDATE を前提とする)
bool NavSaveCrowd(dtCrowd& crowd, NavByteWriter& writer);
// 書いた状態を、同じ設定で init 済みの dtCrowd へ復元する (既存のエージェントは全部外す)
bool NavLoadCrowd(dtCrowd& crowd, NavByteReader& reader);
uint64_t NavHashCrowd(dtCrowd& crowd);

} // namespace mye
