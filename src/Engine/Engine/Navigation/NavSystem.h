//====================================================================================
//                          NavSystem.h
//  MyEngin/ 秋田蓮音                                                     10/03/2026
//                                          ナビメッシュの実行時の持ち主 (Surface ごとの読み込みと輪郭の描画)
//====================================================================================
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "Engine/Core/Ecs/EntityID.h"
#include "Engine/Engine/Navigation/NavMeshAsset.h"
#include "Engine/Engine/Rendering/DebugDraw.h"

namespace mye {

class World;

enum class NavSurfaceState : uint8_t {
    NoAsset, // navAsset が未設定 (未ベイク)
    Loaded,
    Failed,  // 読み込み失敗。この Surface だけ無効で、他の Surface は動く
};

// Surface 1 つ分の実行時の状態。層 (TileCache の入力) と dtNavMesh は store が所有する
struct NavSurfaceRuntime {
    EntityID entity;
    uint64_t assetGuid = 0;
    NavSurfaceState state = NavSurfaceState::NoAsset;
    std::unique_ptr<NavTileStore> store;
    int layerCount = 0;
    int tileCount = 0;
    int polyCount = 0;
    // 描画用の線 (ワールド座標)。読み込み時に 1 回だけ作る。NavMesh が変わらない tick では作り直さない
    std::vector<DebugLineCmd> outlineLines;
    std::vector<DebugLineCmd> tileBoundLines;
};

// Surface の .mnav を読み込んで dtNavMesh を持つ。sim 状態ではなく「アセットから作る導出値」:
// 同じシーン・同じアセットなら同じ内容になるので、SimSnapshot には入れない (M82c で Nav 節を足す)。
// Surface が 1 つも無いシーンでは Update は走査だけで何もしない。
class NavSystem {
public:
    // tick ごとに呼ぶ (stepSim の中、物理より前)。Surface の構成 (エンティティ・navAsset) が
    // 変わっていたら読み直す。読み込みはメインスレッド専用 (ファイル I/O + ログ)
    void Update(World& world);

    // 旧シーンの NavMesh を捨てる (シーン遷移)。次の Update が読み直す
    void Reset();

    // 表示フラグの立った Surface の線を out へ足す。描画レーン (sim 状態に触れない)
    void AppendDebugLines(World& world, std::vector<DebugLineCmd>& out) const;

    const std::vector<NavSurfaceRuntime>& Surfaces() const { return surfaces_; }

private:
    struct Key {
        EntityID entity;
        uint64_t assetGuid = 0;
    };
    static void Load(NavSurfaceRuntime& surface, const char* name);

    std::vector<NavSurfaceRuntime> surfaces_;
    std::vector<Key> loadedKeys_;
    std::vector<Key> scanKeys_; // Update の作業用 (毎 tick の確保を避ける)
};

} // namespace mye
