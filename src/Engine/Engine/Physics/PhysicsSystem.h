#pragma once
#include <vector>

#include "Engine/Core/EntityID.h"
#include "Shared/EngineAPI.h" // MyeRaycastHit / MyeVec3

namespace mye {

class World;
class XpbdBackend;
struct ColliderComponent;
struct RigidbodyComponent;
struct PhysMat;
struct PhysicsEnvironmentComponent;
struct ShapePose;

// ソリッド接触ペア (M28c)。PhysicsSystem が tick 毎に最終ソルバ反復で検出したペアを
// key 昇順で出力し、CollisionSystem が前 tick 差分から OnCollisionEnter/Stay/Exit を配信する。
// key = (小 index << 32) | 大 index。normal は大 index 側→小 index 側 (小側から見た
// 「相手→自分」方向)。PhysicsSystem 自身は状態を持たない (per-tick 出力 = ステートレス維持)
struct SolidContact {
    uint64_t key = 0;
    float nx = 0, ny = 1, nz = 0;
    // ---- M59e 追加 (末尾 append)。消費者は 可視化 (M59e) / 接触 ABI (M59k) /
    //      破壊の応力入力 (M61) / 衝撃音 (**M65c で消費開始**) の 4 者を想定した共通形 ----
    // 代表接触点 (ワールド)。マニフォールド最大 4 点の重心 = ソルバが中央法線インパルスと
    // 摩擦を効かせている点そのもの (別に平均を取り直していない)
    float px = 0, py = 0, pz = 0;
    // その tick にこのペアへ入った**法線インパルスの合計** [N*s] = 固定 8 反復ぶんの総和。
    // 静止した質量 m の物体を支えている接触ではちょうど m*g*dt になる (selftest が断言)。
    // ★最終反復ぶんだけでは駄目 — 静止接触は 1 回目でほぼ解決してしまい最後の反復は
    //   ほぼ 0 = 「載っている重さ」を表さない。M59g1 で蓄積インパルスに改装しても
    //   意味は変わらない (消費者は式を書き換えなくてよい)
    float impulse = 0.0f;
};

// M60'c: 親チェーンを LocalTransform から scalar 合成したワールド位置/回転。
// 物理フェーズ (3.6) は WorldMatrix を読めない (1 tick 古い) ための公開口で、
// 剛体収集の ComposeParentFrame/ApplyFrame と**同じ式**を通る (挙動は 1 ビットも変えない)。
// XpbdBackend::Sync が池の初期配置とピン追従にこれを使う。scale は畳み込み済みの
// 位置にだけ効き、回転は正規化前提 (剛体と同じ近似)
void ComposeEntityWorldPose(World& world, EntityID e, float& px, float& py, float& pz, float& qx,
                            float& qy, float& qz, float& qw);

// 剛体物理 (M20、形状拡張 M28a、回転剛体 M28b)。決定論契約に従う簡易逐次インパルスソルバ:
//   収集 (entity.index 昇順) → 積分 (重力 / 減衰 / 速度 / 角速度 / クォータニオン) →
//   接触マニフォールド生成 (index 順、最大 4 点) → 固定反復・固定順ソルバ
//   (貫通押し出し[並進のみ] + 接触点毎の法線インパルス + クーロン摩擦) → 書戻し。
// RigidbodyComponent を持つエンティティのみが動的ボディ。ソリッドな ColliderComponent
// (isTrigger==0) が衝突面。RigidbodyComponent 非存在シーンでは完全 no-op = 既存リプレイ不変。
// 形状判定 (sphere / OBB / capsule) は Physics/Shapes.cpp に統合 (M28a)。
// 慣性テンソルは形状+質量から毎 tick 導出 (コンポーネントに持たない = ステートレス維持)。
// freezeRotation で回転積分・角応答を無効化 (M28a 以前の並進のみ挙動)。
// スリープは M59h で追加 (PhysicsEnvironment の閾値が有効なときだけ働く)。
// 親子階層対応 (M28d): 親チェーンを LocalTransform から scalar 合成してワールド姿勢で sim し、
// 親フレームの逆変換でローカルに書き戻す。親は運動学的フレーム扱い (同 tick の親の積分結果は
// 子に伝播しない)。velocity / angularVelocity は常にワールド系。ジョイント/複合コライダーは対象外。
// ブロードフェーズ (M28d): 毎 tick 再構築の 1 軸 sort & sweep (Broadphase.cpp)。
// 状態は全てコンポーネントに常駐 (velocity=Rigidbody, position=LocalTransform) → システムはステートレス。
// CCD (M59j): Rigidbody.ccd を立てたボディだけ、1 サブステップの移動量が自分の外接球半径を
// 超えるときに掃引され、最初に触れる位置で止まって法線インパルスを 1 発受ける。
// 相手は不動として扱う近似で、止まった直後は起動しきい値を外れて通常の離散ソルバへ戻る
// (摩擦・スタック・接触の継続はそちらの担当)。CCD が作った接触も outContacts に載る。
class PhysicsSystem {
public:
    // TransformSystem の直前に呼ぶ (Play / 検証時のみ)。dt は固定 tick (1/60)。
    // outContacts 非 null なら clear してソリッド接触ペアを key 昇順で書き込む (M28c)。
    // 両方不動 (静的/kinematic 同士) のペアはソルバ対象外なので出力されない。
    // xpbd (M60'b): 変形体の粒子池。先頭で Sync だけ呼ぶ (ソルバ統合は M60'c から)
    void Update(World& world, float dt, std::vector<SolidContact>* outContacts = nullptr,
                XpbdBackend* xpbd = nullptr);

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
// M59f2: 静止摩擦係数 μs。**材料が無ければ μd と同じ値**を返す — これが
// 「材料を割り当てていないシーンはビット同一」の根拠 (μs == μd なら静止/動の分岐が
// 従来の 1 本のクランプに畳まれる)。override ビットは μs/μd をまとめて覆う
float SelectStaticFriction(const ColliderComponent& col, const PhysMat* mat);
// M59f2: 転がり抵抗係数 (無次元)。既存フィールドが無いので材料が無ければ 0 = 従来値
float SelectRollingResistance(const ColliderComponent& col, const PhysMat* mat);
float SelectRestitution(const ColliderComponent* col, const RigidbodyComponent* rb,
                        const PhysMat* mat);
// M60d: 粘着力 [N] = 接触が引っ張る側へ耐えられる上限。既存フィールドも override ビットも
// 無いので、材料が無ければ 0 = 法線インパルスの下限が従来どおり 0 (存在ゲート)。
// 結合則は **min** (弱いほうが勝つ = 反発と同じ)
float SelectAdhesion(const PhysMat* mat);
// M65c: 音響 3 本。**Collider 側に旧フィールドが無いので override ビットも取らない** —
// SelectAdhesion と全く同じ形で、「材料が無ければ無音」しか状態が無い = 未割当シーンの
// ビット同一が式の上で自明に立つ。黙らせたい床は材料を外せばよい。
// ★結合則を持たない (相手が誰でも同じ値) — 摩擦や反発と違い「鳴ったのは床」であって
//   ペアの性質ではないため。衝撃音で 2 材質が当たるときは**大きいほう**を呼び出し側が採る
float SelectAcousticLoudness(const PhysMat* mat);
float SelectAcousticRadiusM(const PhysMat* mat);
int32_t SelectAcousticTone(const PhysMat* mat);

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

// ---- 物理環境と等方空力 (M59b)。全て純関数 ----

// env 不在時の既定値。**「env を置く」= 新しい数式への opt-in** なので、これらは
// 「env が無いが Aero だけ付いている」ボディのための値であって、
// 「env を既定値で置けば挙動不変」を意味しない (M59 決定台帳 1 の存在ゲート)
inline constexpr float kDefaultAirDensity = 1.225f;      // kg/m^3 (海面 15 degC)
inline constexpr float kDefaultDragCoefficient = 0.47f;  // 球の Cd (PhysMat の既定と同値)
inline constexpr float kDefaultWaterDensity = 1000.0f;   // kg/m^3 (真水 4 degC)
inline constexpr float kDefaultWaterPlaneY = 0.0f;       // 水面のワールド Y

// シーンの物理環境 = **entity.index 最小の active な 1 個** (Skybox/Fog と同じ規約。
// RenderSystem::CollectEnvironment が前例)。無ければ nullptr = 従来の定数重力へ落ちる。
// 戻り値はアーキタイプ記憶域を指す — 構造変更を挟むと無効になるので tick 内で使い切ること
const PhysicsEnvironmentComponent* ResolvePhysicsEnvironment(World& world);

// 等方空力の基準面積 [m^2] = **Cauchy の平均投影面積 (凸形状の表面積 / 4)**。
// 「向きに依らない代表面積」の物理的に正しい唯一の選び方で、球で pi*r^2、
// 1 辺 a の立方体で 1.5*a^2 という既知の値に一致する。向きを見る正しい面積分は
// M59c/M59d の面サンプリングカーネルが担当する。
// スケール規約は ShapeVolumeWorld と同一。col == nullptr / mesh (shape=3) は
// 慣性導出 (LocalInertiaDiag) と同じ「半径 0.5 の球」既定へ落ちる
float MeanProjectedAreaWorld(const ColliderComponent* col, float sx, float sy, float sz);

// 水面 (ワールド Y = planeY) より下にある**体積の割合** [0,1] と、その没水部分の
// 体積重心の Y (outCentroidY) を返す (M59b2)。
// 球だけは球冠の解析式 — V = pi(R^2 t - t^3/3 + 2R^3/3) / M = pi(R^2 t^2/2 - t^4/4 - R^4/4)
// (t = planeY - 中心Y) で**多項式のみ**。三角関数も逆三角関数も通らないので、
// 決定論の観点で最も安全な閉形式になっている。
// box / capsule は保守 AABB の高さ比近似 (M59 の割り切り。向きを見た正しい積分は M59c)。
// 水面より上なら 0 を返し、呼び出し側は項ごとスキップする = 陸上のボディは fp 演算ゼロ
float SubmergedFractionWorld(const ShapePose& pose, float planeY, float& outCentroidY);

// ワールドトルクを 1 tick 分適用 (M28b、ABI AddTorque の実装本体)。
// ω += I⁻¹·τ·dt を即時適用 (蓄積フィールドなし = ステートレス)。
// Rigidbody 非所持 / kinematic / freezeRotation は 0 を返す。
int ApplyTorqueWorld(World& world, EntityID e, MyeVec3 torque, float dt);

// 作用点付きの力を 1 tick 分適用 (M59k、ABI AddForceAtPosition の実装本体)。
// Δv = F/m·dt と Δω = I⁻¹(r×F)·dt を同時に入れる (r = worldPoint − 質量中心)。
// **AddForce + AddTorque を呼び側で合成するのとは違い、質量と慣性が 1 回だけ解決される** —
// 質量が二義にならないのが独立スロットにしてある理由 (M59a2 の申し送り 3 と同じ原則)。
// freezeRotation のボディは並進のみ入れて 1 を返す (AddForce と同じ扱い。
// ApplyTorqueWorld が 0 を返すのとは意図的に非対称)。Rigidbody 非所持 / kinematic は 0。
// 姿勢の出所は LocalTransform (ApplyTorqueWorld と同じ規約 — 親付きのボディでは
// ソルバの親合成と厳密には一致しない既知の割り切り)
int ApplyForceAtWorldPoint(World& world, EntityID e, MyeVec3 force, MyeVec3 worldPoint, float dt);

// ワールド XZ の地形表面をサンプルする (M59k、ABI SampleTerrainHeight の実装本体)。
// **描画の地形ではなく当たる地形を引く** — shape=4 のコライダーを index 昇順に走査し、
// 各々のワールド AABB の天井から真下へ既存の地形レイキャスト (セル DDA) を撃つ。
// 回転・スケールした地形でも正しく、LOD やスカートの影響も受けない。
// ヒットで 1 (outHeight = ワールド Y、outNormal = 面法線。どちらも null 可)。
// 複数の地形が重なるときは最も高いヒット、同値は entity.index が小さい側 (決定論)
int SampleTerrainHeightWorld(World& world, float x, float z, float* outHeight,
                             MyeVec3* outNormal);

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
