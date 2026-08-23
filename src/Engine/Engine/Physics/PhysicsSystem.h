#pragma once
#include <vector>

#include "Engine/Core/EntityID.h"
#include "Shared/EngineAPI.h" // MyeRaycastHit / MyeVec3

namespace mye {

class World;
struct ColliderComponent;
struct RigidbodyComponent;
struct PhysMat;

// ソリッド接触ペア (M28c)。PhysicsSystem が tick 毎に最終ソルバ反復で検出したペアを
// key 昇順で出力し、CollisionSystem が前 tick 差分から OnCollisionEnter/Stay/Exit を配信する。
// key = (小 index << 32) | 大 index。normal は大 index 側→小 index 側 (小側から見た
// 「相手→自分」方向)。PhysicsSystem 自身は状態を持たない (per-tick 出力 = ステートレス維持)
struct SolidContact {
    uint64_t key = 0;
    float nx = 0, ny = 1, nz = 0;
};

// 剛体物理 (M20、形状拡張 M28a、回転剛体 M28b)。決定論契約に従う簡易逐次インパルスソルバ:
//   収集 (entity.index 昇順) → 積分 (重力 / 減衰 / 速度 / 角速度 / クォータニオン) →
//   接触マニフォールド生成 (index 順、最大 4 点) → 固定反復・固定順ソルバ
//   (貫通押し出し[並進のみ] + 接触点毎の法線インパルス + クーロン摩擦) → 書戻し。
// RigidbodyComponent を持つエンティティのみが動的ボディ。ソリッドな ColliderComponent
// (isTrigger==0) が衝突面。RigidbodyComponent 非存在シーンでは完全 no-op = 既存リプレイ不変。
// 形状判定 (sphere / OBB / capsule) は Physics/Shapes.cpp に統合 (M28a)。
// 慣性テンソルは形状+質量から毎 tick 導出 (コンポーネントに持たない = ステートレス維持)。
// freezeRotation=1 で回転積分・角応答を無効化 (M28a 以前の並進のみ挙動)。スリープ機構は無し。
// 親子階層対応 (M28d): 親チェーンを LocalTransform から scalar 合成してワールド姿勢で sim し、
// 親フレームの逆変換でローカルに書き戻す。親は運動学的フレーム扱い (同 tick の親の積分結果は
// 子に伝播しない)。velocity / angularVelocity は常にワールド系。ジョイント/複合コライダーは対象外。
// ブロードフェーズ (M28d): 毎 tick 再構築の 1 軸 sort & sweep (Broadphase.cpp)。
// 状態は全てコンポーネントに常駐 (velocity=Rigidbody, position=LocalTransform) → システムはステートレス。
class PhysicsSystem {
public:
    // TransformSystem の直前に呼ぶ (Play / 検証時のみ)。dt は固定 tick (1/60)。
    // outContacts 非 null なら clear してソリッド接触ペアを key 昇順で書き込む (M28c)。
    // 両方不動 (静的/kinematic 同士) のペアはソルバ対象外なので出力されない。
    void Update(World& world, float dt, std::vector<SolidContact>* outContacts = nullptr);

    // 等価性テスト用 (PhysicsSelfTest): true でブロードフェーズを総当たり候補に切替。
    // 挙動はビット同一のはず — selftest がハッシュ比較で常時検証する
    static inline bool sDisableBroadphaseForTest = false;
};

// ---- 物理マテリアル解決 (M59a2)。全て純関数 — ソルバ収集と ABI が共有する ----
// 摩擦/反発は fp 演算を挟まない「値の選択」のみ: overrideBits のビット → 既存フィールド /
// 材料割当あり → .physmat 値 / 未割当 → 既存フィールド。未割当シーンは従来と同じメモリを
// 同じ経路で読むので既存挙動はビット同一 (PhysicsSelfTest の body ビットパターンで機械照合)。
// 結合則は従来のまま: μ = sqrt(μa·μb) / e = min(ea, eb)。材料付き静的コライダーが
// e を主張できるのは新規能力 (従来は構造的に 0 — engine_spec 10.3)
float SelectFriction(const ColliderComponent& col, const PhysMat* mat);
float SelectRestitution(const ColliderComponent* col, const RigidbodyComponent* rb,
                        const PhysMat* mat);

// ワールドスケール済み衝突形状の体積 (m^3)。スケール規約は shapes::ApplyScaledExtents と
// 同一 (球 = 最大成分 / box = 成分別 / capsule = 半径 max(sx,sz)・高さ sy)。mesh (shape=3) は 0
float ShapeVolumeWorld(const ColliderComponent& col, float sx, float sy, float sz);

// 質量の解決: useDensity かつ材料割当ありなら 材料密度 × 形状体積、それ以外は従来の
// 「(mass>0) ? mass : 1」。導出値が 0 以下 (半径 0 等) も従来値へフォールバック (1/m の防波堤)
float ResolveBodyMass(const RigidbodyComponent& rb, const ColliderComponent* col,
                      const PhysMat* mat, float sx, float sy, float sz);

// ABI (AddForce/AddImpulse/AddTorque) 用の 1 発解決。スケールは LocalTransform 直読み
// (ApplyTorqueWorld の慣性導出と同じ規約。スケール付きの親を持つ剛体ではソルバの
// 親合成スケールと厳密には一致しない — 既存の慣性導出と同じ割り切り)
float EffectiveMassWorld(World& world, EntityID e, const RigidbodyComponent& rb);

// ワールドトルクを 1 tick 分適用 (M28b、ABI AddTorque の実装本体)。
// ω += I⁻¹·τ·dt を即時適用 (蓄積フィールドなし = ステートレス)。
// Rigidbody 非所持 / kinematic / freezeRotation は 0 を返す。
int ApplyTorqueWorld(World& world, EntityID e, MyeVec3 torque, float dt);

// ワールドのコライダーに対するレイキャスト (M20、ABI Raycast の実装本体)。
// origin/dir はワールド座標。dir は非正規化でよい (内部で正規化する)。
// 最近ヒットを outHit に書いて 1 を返す。ヒット無しで 0。走査は entity.index 昇順 (決定論)。
// トリガー / ソリッドを問わず全コライダーが対象 (汎用空間クエリ)。WorldMatrix ベース。
// mask (M36a): LayerHit(mask, collider.layer) のコライダーだけ対象。既定 = 全レイヤー = 従来。
int RaycastWorld(World& world, MyeVec3 origin, MyeVec3 dir, float maxDist, MyeRaycastHit* outHit,
                 uint32_t mask = 0xFFFFFFFFu);

// ---- 空間クエリ (M28c、ABI OverlapSphere/OverlapBox/SphereCast の実装本体) ----
// Raycast と同じ収集規約: トリガー含む全コライダー対象、WorldMatrix ベース、
// entity.index 昇順 (決定論)。実装は PhysicsQueries.cpp。mask の意味は Raycast と同じ (M36a)。
// Overlap 系: ヒットを outEntities に最大 maxCount 個 (index 昇順) 書き、
// 戻り値は切り捨て前の総ヒット数 (outEntities null / maxCount 0 は数えるだけ)。
int OverlapSphereWorld(World& world, MyeVec3 center, float radius, MyeEntityId* outEntities,
                       int maxCount, uint32_t mask = 0xFFFFFFFFu);
int OverlapBoxWorld(World& world, MyeVec3 center, MyeVec3 halfExtents, MyeQuat rotation,
                    MyeEntityId* outEntities, int maxCount, uint32_t mask = 0xFFFFFFFFu);
// 半径 radius の球を dir 方向に掃引し最近ヒットを返す (Raycast の太い版)。
// sphere/capsule 相手は解析解 (半径膨張レイ)、box 相手は固定 32 回の保守的前進 (決定論)。
int SphereCastWorld(World& world, MyeVec3 origin, MyeVec3 dir, float radius, float maxDist,
                    MyeRaycastHit* outHit, uint32_t mask = 0xFFFFFFFFu);

} // namespace mye
