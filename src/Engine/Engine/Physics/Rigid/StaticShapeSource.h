//====================================================================================
//                          StaticShapeSource.h
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                          静的衝突形状を複数まとめて物理へ渡す供給口
//====================================================================================
#pragma once
#include <functional>
#include <utility>
#include <vector>

#include "Engine/Core/Ecs/EntityID.h"
#include "Engine/Engine/Physics/Rigid/Shapes.h"

namespace mye {

class World;
struct ColliderComponent;

// 静的な衝突面 1 枚。pose はワールド姿勢で確定済みの形状 (箱や球など)
struct StaticShape {
    EntityID owner;                     // 接触の相手として現れる持ち主
    ShapePose pose;
    const ColliderComponent* col = nullptr; // 摩擦 / 反発 / 材料 / レイヤー / マスク / oneWay の出所。必須
};

// 物理が今 tick に動かしうる物体 (剛体 / キャラクター / 変形体の粒子) が届く範囲のワールド AABB。
// 速度と余裕を足してあるので、これと交わらない静的形状はこの tick の接触に現れない
struct StaticRegion {
    float minX = 0.0f, minY = 0.0f, minZ = 0.0f;
    float maxX = 0.0f, maxY = 0.0f, maxZ = 0.0f;
};

// StaticRegion の列。静的形状を持つ持ち主がいるときだけ、最初の Get() で 1 回だけ組む
// (タイルマップの無いシーンでは剛体の AABB 計算を 1 回も払わない)
class MotionRegions {
public:
    using Builder = std::function<void(std::vector<StaticRegion>&)>;
    explicit MotionRegions(Builder builder) : builder_(std::move(builder)) {}
    const std::vector<StaticRegion>& Get()
    {
        if (!built_) {
            built_ = true;
            builder_(regions_);
        }
        return regions_;
    }

private:
    Builder builder_;
    std::vector<StaticRegion> regions_;
    bool built_ = false;
};

// 1 エンティティの Collider では表せない「複数の静的形状」(タイルの箱、辺チェーンなど) の供給口。
// PhysicsSystem は tick ごとに登録順で Collect を呼び、返った形状を通常の静的コライダーと同じ扱いで解く。
// 所有は呼び出し側で、PhysicsSystem から外すまで生かすこと。col の指す先は tick の間変わらないこと。
// 決定論: out への追加順を入力だけで決める (ハッシュコンテナの走査順やポインタ値に依存しない)
class StaticShapeSource {
public:
    virtual ~StaticShapeSource() = default;
    virtual void Collect(World& world, std::vector<StaticShape>& out) = 0;
    // Collect の絞り込み版。動かしうる物体の範囲 (regions) に届かない形状は返さなくてよい。
    // 既定は絞らず Collect。大きな面を持つ供給元 (タイルマップ) が、広域探索の前に形状の数を減らすために使う
    virtual void CollectWithin(World& world, MotionRegions& regions, std::vector<StaticShape>& out)
    {
        (void)regions;
        Collect(world, out);
    }
};

} // namespace mye
