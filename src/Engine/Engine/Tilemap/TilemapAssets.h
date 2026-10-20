//====================================================================================
//                          TilemapAssets.h
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                          タイルパレット / タイルマップ資産と登録表
//====================================================================================
#pragma once
#include <array>
#include <compare>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

#include "Engine/Core/Ecs/EntityID.h"

namespace mye {

// チャンクは 16 x 16 セル。セル座標からチャンクへは床除算 (負の座標も連続する)
inline constexpr int32_t kTilemapChunkSize = 16;
inline constexpr size_t kTilemapChunkCells = static_cast<size_t>(kTilemapChunkSize) * kTilemapChunkSize;
inline constexpr size_t kMaxTilemapLayers = 8;
inline constexpr uint32_t kTileIdMax = 65535;
inline constexpr size_t kTileNeighborCount = 8; // ルールタイルの隣接指定の長さ (NW N NE W E SW S SE)

// セルの値 (uint32): bit0-15 tileId (0 = 空) / bit16 flipX / bit17 flipY / bit18-19 回転 (0/90/180/270、反時計回り) /
// bit24-31 damage。bit20-23 は予約 (読み書きでそのまま保つ)
namespace tilecell {
inline constexpr uint32_t kIdMask = 0xFFFFu;
inline constexpr uint32_t kFlipX = 1u << 16;
inline constexpr uint32_t kFlipY = 1u << 17;
inline constexpr uint32_t kRotShift = 18;
inline constexpr uint32_t kRotMask = 3u << kRotShift;
inline constexpr uint32_t kDamageShift = 24;

inline uint32_t TileId(uint32_t v) { return v & kIdMask; }
inline uint32_t Rotation(uint32_t v) { return (v & kRotMask) >> kRotShift; } // 0..3 (= 90 度の個数)
inline uint32_t Damage(uint32_t v) { return v >> kDamageShift; }
inline uint32_t Make(uint32_t tileId, bool flipX, bool flipY, uint32_t rotation, uint32_t damage = 0)
{
    return (tileId & kIdMask) | (flipX ? kFlipX : 0u) | (flipY ? kFlipY : 0u) | ((rotation & 3u) << kRotShift)
           | (damage << kDamageShift);
}
} // namespace tilecell

// ---- タイルパレット (.tilepalette.json) ----

// ルールタイルの変種 1 つ。neighbors は 8 文字 (x = 同種が必要 / o = 空が必要 / . = 任意)
struct TileVariant {
    std::string neighbors = "........";
    AssetID sprite = {};
    bool operator==(const TileVariant&) const = default;
};

// タイル 1 種。見た目の選び方: anim (コマ) > rule (隣接) > sprite
struct TileDef {
    uint32_t id = 0; // 1..kTileIdMax
    std::string name;
    AssetID sprite = {};
    int32_t collider = 0;   // 0 None / 1 Grid
    bool oneWay = false;
    int32_t hp = 0;         // 0 = 壊れない、1..255
    uint32_t destroyedTile = 0; // 壊れた後のタイル (0 = 空)
    AssetID destroyEffect = {}; // 壊れたときに生成するプレハブ (0 = 無し)
    bool hasRule = false;
    std::vector<TileVariant> variants;
    bool hasAnim = false;
    int32_t ticksPerFrame = 8;
    std::vector<AssetID> animFrames;
    bool operator==(const TileDef&) const = default;
};

struct TilePalette {
    uint64_t hash = 0; // = GUID (TilePaletteLibrary のキー)
    std::string name;
    std::wstring path;
    std::vector<TileDef> tiles; // id 昇順

    const TileDef* Find(uint32_t id) const;
    // 次に割り当てる id (最大 + 1)。使い切っていたら 0
    uint32_t NextId() const;
    // 末尾に 1 種足す。id が尽きていたら nullptr。返したポインタは次の変更まで有効
    TileDef* Add(const std::string& name, AssetID sprite);
};

// ルールタイルが見る隣接 8 マスの tileId (添字は NW N NE W E SW S SE。N = +y)。同じレイヤーの実効セルから作る
using TileNeighbors = std::array<uint32_t, kTileNeighborCount>;

// tileId のタイルの変種を順に見て、最初に合う neighbors (x = 同じ tileId / o = 空 / . = 任意) の sprite を返す。
// 合わない / ルール無し / 未登録の id は tileId の既定 sprite (id が無ければ null)
AssetID ResolveRuleVariant(const TilePalette& palette, uint32_t tileId, const TileNeighbors& neighbors);

// アニメのコマ番号 = (tick / ticksPerFrame) % frameCount。frameCount が 0 なら -1。tick は sim の tick (描画フレーム数ではない)
int32_t TileAnimFrameIndex(int32_t ticksPerFrame, size_t frameCount, uint64_t tick);

// def の現在の絵。anim (frames が空でない) があればそのコマ、無ければ rule、無ければ sprite
AssetID ResolveTileSprite(const TilePalette& palette, const TileDef& def, const TileNeighbors& neighbors, uint64_t tick);

struct TilePaletteEntry {
    uint64_t hash = 0;
    std::string name;
};

// 登録済み .tilepalette.json の管理 (PhysMatLibrary と同じ範型)。EngineLoop / HeadlessSim が所有し tilepalette:: で注入。
// 描画専用ではなく衝突 (sub-11) も引くが、sim 状態ではなく「再生時に同じ資産がある」前提の資産
class TilePaletteLibrary {
public:
    static uint64_t HashForPath(const std::wstring& path);

    uint64_t LoadFromFile(const std::wstring& path); // 失敗時 0
    uint64_t Register(const std::wstring& path, TilePalette palette);

    const TilePalette* Get(uint64_t hash) const;
    bool Contains(uint64_t hash) const { return palettes_.find(hash) != palettes_.end(); }
    std::vector<TilePaletteEntry> Enumerate() const; // 名前昇順
    // 登録 / 再登録のたびに増える。描画側のキャッシュ無効化に使う
    uint64_t Epoch() const { return epoch_; }

    static nlohmann::json ToJson(const TilePalette& p);
    // 読み値は Sanitize 済み。パレットでない JSON ("tilepalette" キー無し) は false
    static bool FromJson(const nlohmann::json& j, TilePalette& out);
    // 範囲外 id / 重複 id を捨て、フィールドを範囲へ丸め、id 昇順に揃える
    static void Sanitize(TilePalette& p);

private:
    std::map<uint64_t, TilePalette> palettes_;
    uint64_t epoch_ = 0;
};

// ---- タイルマップ (.tilemap.json) ----

// チャンクの添字。JSON と走査は (cy, cx) の昇順
struct ChunkKey {
    int32_t cy = 0;
    int32_t cx = 0;
    auto operator<=>(const ChunkKey&) const = default;
};

struct TileChunk {
    std::array<uint32_t, kTilemapChunkCells> cells{}; // 行優先、添字 = ly * 16 + lx (ly = 0 が下)
    bool IsEmpty() const;
};

struct TilemapLayer {
    std::string name;
    int32_t sortingLayerId = 0;
    int32_t orderInLayer = 0;
    bool collision = true;
    std::map<ChunkKey, TileChunk> chunks; // 空のチャンクは持たない

    // セル座標 (負も可) の値。チャンクが無ければ 0
    uint32_t GetCell(int32_t x, int32_t y) const;
    // 書く。0 を書いて空になったチャンクは消す
    void SetCell(int32_t x, int32_t y, uint32_t value);
};

struct TilemapAsset {
    uint64_t hash = 0; // = GUID (TilemapLibrary のキー)
    std::string name;
    std::wstring path;
    uint64_t palette = 0; // TilePalette の AssetID
    float cellSize[2] = { 1.0f, 1.0f };
    std::vector<TilemapLayer> layers; // 先頭が奥 (描画順は sortingLayerId / orderInLayer が決める)

    size_t ChunkCount() const;
    // 全レイヤーの使用チャンクを囲む範囲 (両端を含む)。チャンクが 1 つも無ければ false
    bool ChunkBounds(int32_t& minCx, int32_t& minCy, int32_t& maxCx, int32_t& maxCy) const;
};

struct TilemapEntry {
    uint64_t hash = 0;
    std::string name;
};

class TilemapLibrary {
public:
    static uint64_t HashForPath(const std::wstring& path);

    uint64_t LoadFromFile(const std::wstring& path); // 失敗時 0
    uint64_t Register(const std::wstring& path, TilemapAsset map);

    const TilemapAsset* Get(uint64_t hash) const;
    bool Contains(uint64_t hash) const { return maps_.find(hash) != maps_.end(); }
    std::vector<TilemapEntry> Enumerate() const; // 名前昇順
    uint64_t Epoch() const { return epoch_; }

    // 空のチャンクは書かない。往復 (FromJson → ToJson) で同一の JSON になる
    static nlohmann::json ToJson(const TilemapAsset& m);
    static bool FromJson(const nlohmann::json& j, TilemapAsset& out);
    // セル寸法・レイヤー数を範囲へ丸め、空のチャンクを捨てる
    static void Sanitize(TilemapAsset& m);

private:
    std::map<uint64_t, TilemapAsset> maps_;
    uint64_t epoch_ = 0;
};

// 床除算 / 剰余 (負のセル座標でもチャンク内座標が 0..15)
inline int32_t TilemapFloorDiv(int32_t v, int32_t d)
{
    int32_t q = v / d;
    if ((v % d != 0) && ((v < 0) != (d < 0))) {
        --q;
    }
    return q;
}
inline int32_t TilemapFloorMod(int32_t v, int32_t d)
{
    return v - TilemapFloorDiv(v, d) * d;
}

// モジュール注入 (spriteanim:: と同じ流儀)。EngineLoop / HeadlessSim が起動時に Install し、
// 終了時にライブラリ破棄前に外す。メインスレッド専用
namespace tilepalette {
void Install(TilePaletteLibrary* lib);
TilePaletteLibrary* Library();              // 未接続 = nullptr
const TilePalette* Resolve(uint64_t hash);  // 未接続 / 未登録 / 0 = nullptr
} // namespace tilepalette

namespace tilemap {
void Install(TilemapLibrary* lib);
TilemapLibrary* Library();
const TilemapAsset* Resolve(AssetID id);
} // namespace tilemap

} // namespace mye
