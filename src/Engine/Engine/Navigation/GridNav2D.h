//====================================================================================
//                          GridNav2D.h
//  MyEngin/ 秋田蓮音                                                       10/22/2026
//                                          タイルマップ上の 2D グリッド A* (GridAgent2D を歩かせる)
//====================================================================================
#pragma once
#include <cstdint>
#include <memory>
#include <vector>

#include "Engine/Core/Ecs/EntityID.h"

namespace mye {

class World;
class TilemapSystem;
struct GridNavView;

// GridAgent2D の経路を TilemapSystem の collision レイヤーから引き、plane2D の CharacterController へ
// moveInput.x / jumpSpeed を書く。NavSystem (3.4b) の直後、物理 (3.6) の前に tick ごとに Update を呼ぶ。
//
// 状態はコンポーネントだけに置く (実行系は導出値の作業バッファしか持たない = SimRefs / ハッシュ / スナップショットの対象外)。
// 経路は持たず、「現在のセルから目的地まで」を A* で引き直して 1 つ先のセルだけ GridAgent2D に覚える。
// 引き直す契機: セルへ着いた / 目的地セルが変わった / replanInterval tick が経った / 辺が成り立たなくなった。
// 同じ入力から同じ経路が引けるので、スナップショットから戻っても履歴と一致する
class GridNavSystem {
public:
    // 1 回の探索で展開するノード数の上限。超えたら届いた中で目的地に最も近いノードへの経路で動き、
    // 次の引き直しで続きを探す (status は Moving のまま)
    static constexpr int32_t kMaxExpansionsPerPlan = 4096;
    // 目的地セルが歩行不可のとき、置き換えを探す範囲 (セル、チェビシェフ距離)
    static constexpr int32_t kGoalSearchRadius = 2;
    // セル中心とみなす x の許容 (ワールド単位)
    static constexpr float kCellArriveTolerance = 0.1f;
    // 進展が無いままこの tick 経つと Stuck
    static constexpr int32_t kStuckTicks = 30;

    GridNavSystem();
    ~GridNavSystem();
    GridNavSystem(const GridNavSystem&) = delete;
    GridNavSystem& operator=(const GridNavSystem&) = delete;

    // GridAgent2D を entity.index 昇順に処理する。tiles が null なら何もしない。
    // GridAgent2D が 1 つも無いシーンでは走査だけで終わる (ハッシュ・RNG に触らない)
    void Update(World& world, const TilemapSystem* tiles, float dt);

private:
    struct ViewEntry {
        EntityID owner = kNullEntity;
        std::unique_ptr<GridNavView> view; // null = 引けない Tilemap (資産なし / 無効)
    };
    GridNavView* ViewFor(World& world, const TilemapSystem& tiles, EntityID tilemapEntity);
    void UpdateAgent(World& world, const TilemapSystem& tiles, EntityID e, float dt);

    std::vector<ViewEntry> views_; // Update の間だけ有効な作業バッファ
    std::vector<EntityID> agents_; // 同上
};

} // namespace mye
