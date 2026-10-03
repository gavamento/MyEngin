//====================================================================================
//                          NavMeshAsset.h
//  MyEngin/ 秋田蓮音                                                     10/03/2026
//                                          ナビメッシュ資産 (.mnav) の保存形式
//====================================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Engine/Engine/Navigation/NavTileCacheSupport.h"

namespace mye {
namespace NavMeshAsset {

inline constexpr const wchar_t* kNavExt = L".mnav";
inline constexpr uint32_t kVersion = 1;

// ベイク方式の版。ベイクの結果が変わる変更 (入力収集・Recast 設定・球の分割数) をしたら上げる。
// 入力ハッシュに混ぜるので、版が違えば別ファイルになる
inline constexpr uint32_t kNavBakeVersion = 1;

// TileCache の層 1 枚 (header 付き、無圧縮のバイト列)
struct LayerRecord {
    int32_t tx = 0;
    int32_t ty = 0;
    int32_t layer = 0;
    std::vector<uint8_t> blob;
};

// dtNavMeshParams / dtTileCacheParams を決める値と、ベイクした層。
// 同じ入力から同じバイト列になる (Debug / Release 一致の前提)。所要時間など非決定な値は持たない
struct Data {
    uint32_t bakeVersion = kNavBakeVersion;
    uint64_t inputHash = 0;
    NavBakeConfig config;
    int32_t tilesX = 0;
    int32_t tilesY = 0;
    int32_t maxTiles = 0;        // dtNavMeshParams::maxTiles。層数に余裕を見た値
    int32_t maxPolysPerTile = 0; // dtNavMeshParams::maxPolys
    int32_t maxObstacles = 0;
    int32_t inputTriangleCount = 0;
    std::vector<LayerRecord> layers; // (ty, tx, layer) 昇順
};

// blob <-> 構造体。Deserialize は境界検査つきで、壊れた blob でも false を返すだけで落ちない
void Serialize(const Data& d, std::vector<uint8_t>& out);
bool Deserialize(const std::vector<uint8_t>& in, Data& out);

// `.mnav` の読み書き。Save は書き切れたときだけ既存ファイルを置き換える
bool Save(const std::wstring& path, const Data& d);
bool Load(const std::wstring& path, Data& out);

// 資産から NavTileStore の設定を作る
NavTileStoreConfig MakeStoreConfig(const Data& d);

// 資産の層を NavTileStore へ入れて BuildAll まで進める。store は未 Init の空の状態であること
bool BuildStore(const Data& d, NavTileStore& store);

} // namespace NavMeshAsset
} // namespace mye
