#pragma once
#include "Engine/Core/EntityID.h"
#include "Shared/EngineAPI.h" // MyeRaycastHit / MyeVec3

namespace mye {

class World;

// 剛体物理 (M20)。決定論契約に従う簡易射影ソルバ:
//   収集 (entity.index 昇順) → 積分 (重力 / 減衰 / 速度) → 接触生成 (index 順) →
//   固定反復・固定順ソルバ (貫通押し出し + 法線速度反発) → LocalTransform / velocity 書戻し。
// RigidbodyComponent を持つエンティティのみが動的ボディ。ソリッドな ColliderComponent
// (isTrigger==0) が衝突面。RigidbodyComponent 非存在シーンでは完全 no-op = 既存リプレイ不変。
// ルート (非親子) エンティティ前提: LocalTransform.position をワールド位置として扱う。
// 状態は全てコンポーネントに常駐 (velocity=Rigidbody, position=LocalTransform) → システムはステートレス。
class PhysicsSystem {
public:
    // TransformSystem の直前に呼ぶ (Play / 検証時のみ)。dt は固定 tick (1/60)。
    void Update(World& world, float dt);
};

// ワールドのコライダーに対するレイキャスト (M20、ABI Raycast の実装本体)。
// origin/dir はワールド座標。dir は非正規化でよい (内部で正規化する)。
// 最近ヒットを outHit に書いて 1 を返す。ヒット無しで 0。走査は entity.index 昇順 (決定論)。
// トリガー / ソリッドを問わず全コライダーが対象 (汎用空間クエリ)。WorldMatrix ベース。
int RaycastWorld(World& world, MyeVec3 origin, MyeVec3 dir, float maxDist, MyeRaycastHit* outHit);

} // namespace mye
