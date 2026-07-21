#pragma once
#include "Engine/Core/EntityID.h"
#include "Shared/EngineAPI.h" // MyeRaycastHit / MyeVec3

namespace mye {

class World;

// 剛体物理 (M20、形状拡張 M28a、回転剛体 M28b)。決定論契約に従う簡易逐次インパルスソルバ:
//   収集 (entity.index 昇順) → 積分 (重力 / 減衰 / 速度 / 角速度 / クォータニオン) →
//   接触マニフォールド生成 (index 順、最大 4 点) → 固定反復・固定順ソルバ
//   (貫通押し出し[並進のみ] + 接触点毎の法線インパルス + クーロン摩擦) → 書戻し。
// RigidbodyComponent を持つエンティティのみが動的ボディ。ソリッドな ColliderComponent
// (isTrigger==0) が衝突面。RigidbodyComponent 非存在シーンでは完全 no-op = 既存リプレイ不変。
// 形状判定 (sphere / OBB / capsule) は Physics/Shapes.cpp に統合 (M28a)。
// 慣性テンソルは形状+質量から毎 tick 導出 (コンポーネントに持たない = ステートレス維持)。
// freezeRotation=1 で回転積分・角応答を無効化 (M28a 以前の並進のみ挙動)。スリープ機構は無し。
// ルート (非親子) エンティティ前提: LocalTransform.position をワールド位置として扱う。
// 状態は全てコンポーネントに常駐 (velocity=Rigidbody, position=LocalTransform) → システムはステートレス。
class PhysicsSystem {
public:
    // TransformSystem の直前に呼ぶ (Play / 検証時のみ)。dt は固定 tick (1/60)。
    void Update(World& world, float dt);
};

// ワールドトルクを 1 tick 分適用 (M28b、ABI AddTorque の実装本体)。
// ω += I⁻¹·τ·dt を即時適用 (蓄積フィールドなし = ステートレス)。
// Rigidbody 非所持 / kinematic / freezeRotation は 0 を返す。
int ApplyTorqueWorld(World& world, EntityID e, MyeVec3 torque, float dt);

// ワールドのコライダーに対するレイキャスト (M20、ABI Raycast の実装本体)。
// origin/dir はワールド座標。dir は非正規化でよい (内部で正規化する)。
// 最近ヒットを outHit に書いて 1 を返す。ヒット無しで 0。走査は entity.index 昇順 (決定論)。
// トリガー / ソリッドを問わず全コライダーが対象 (汎用空間クエリ)。WorldMatrix ベース。
int RaycastWorld(World& world, MyeVec3 origin, MyeVec3 dir, float maxDist, MyeRaycastHit* outHit);

} // namespace mye
