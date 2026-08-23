#pragma once
#include <memory>
#include <string>
#include <unordered_map>

#include "Engine/Core/EntityID.h"
#include "Engine/Engine/Asset/TerrainAsset.h"

namespace mye {

// 地形コライダー (M59i)。**sim レーン専用の地形データ**で、描画側の TerrainSystem が
// 持っているキャッシュとは意図的に別物。
//
// ★理由: 描画のキャッシュを物理が読むと「絵を出したかどうか」で sim が変わる。
//   ヘッドレスの selftest / replay / `--warp` / ウィンドウ最小化のどれでも同じ結果を
//   出さなければならない以上、物理は自分でロードして自分で持つしかない
//   (メッシュコライダー (M41) が RenderResources の CPU 頂点を借りているのとは
//   事情が違う — あちらは「メッシュ資産の中身」で、地形は「描画システムの作業表」)。
//
// AABB の Y 範囲を毎回求め直すと O(W*H) になるので、ロード時に 1 回だけ測って持つ。
struct TerrainCollisionData {
    TerrainAsset::TerrainData data;
    float minHeight = 0.0f; // ワールド Y (heightBase 込み)
    float maxHeight = 0.0f;
};

// AssetID → 地形コリジョンデータの lazy ロード + キャッシュ。EngineLoop が所有する。
// AssetID から `.terrain.json` のパスは assetguid::ResolvePath で引く
// (= .meta の GUID。チェックアウト先に依存しないので、シーン JSON に焼いてコミットできる)
class TerrainColliderLibrary {
public:
    // 未登録 / 解決できないパス / 壊れた地形 = nullptr (呼び出し側は shape=4 を無視する)
    const TerrainCollisionData* Get(AssetID terrainAsset);
    // 任意データの直接登録 (selftest / 手続き生成地形用)。同 ID は差し替え。
    // minHeight / maxHeight はここで測る
    void Register(AssetID id, TerrainAsset::TerrainData data);
    void Clear() { cache_.clear(); }

private:
    // 値が unique_ptr なのは、**返したポインタが以降の Get で無効にならない**ため
    // (rehash でノードは動くが実体は動かない)。MeshColliderLibrary と同じ理由
    std::unordered_map<uint64_t, std::unique_ptr<TerrainCollisionData>> cache_;
    // 解決に失敗した ID を覚えて毎 tick のロード試行を止める (負のキャッシュ)
    std::unordered_map<uint64_t, bool> failed_;
};

// モジュール注入 (meshcol:: / physmat:: と同じ流儀)。EngineLoop が起動時に Install し、
// 終了時にライブラリ破棄前に必ず外す。メインスレッド専用
namespace terraincol {
void Install(TerrainColliderLibrary* lib);
TerrainColliderLibrary* Library();                     // 未接続 = nullptr
const TerrainCollisionData* Resolve(AssetID id);       // 未接続 / 未登録 / null ID = nullptr
} // namespace terraincol

} // namespace mye
