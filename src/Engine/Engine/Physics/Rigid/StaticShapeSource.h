//====================================================================================
//                          StaticShapeSource.h
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                          静的衝突形状を複数まとめて物理へ渡す供給口
//====================================================================================
#pragma once
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

// 1 エンティティの Collider では表せない「複数の静的形状」(タイルの箱、辺チェーンなど) の供給口。
// PhysicsSystem は tick ごとに登録順で Collect を呼び、返った形状を通常の静的コライダーと同じ扱いで解く。
// 所有は呼び出し側で、PhysicsSystem から外すまで生かすこと。col の指す先は tick の間変わらないこと。
// 決定論: out への追加順を入力だけで決める (ハッシュコンテナの走査順やポインタ値に依存しない)
class StaticShapeSource {
public:
    virtual ~StaticShapeSource() = default;
    virtual void Collect(World& world, std::vector<StaticShape>& out) = 0;
};

} // namespace mye
