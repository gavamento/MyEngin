#pragma once
#include <DirectXMath.h>

#include "Engine/Core/ComponentRegistry.h"
#include "Engine/Core/EntityID.h"

namespace mye {

// 組み込みコンポーネント。
// 規約:
//   - trivially copyable な POD であること (ECS カラムは memcpy 移動)
//   - 全メンバを宣言時初期化する (spec 11.2 規則 3)
//   - DirectXMath のアライン付き型 (XMMATRIX/XMVECTOR) は格納禁止 — XMFLOAT* を使う

struct NameComponent {
    char value[64] = {};
    static inline ComponentTypeId sTypeId = kInvalidComponentType;
};

struct LocalTransform {
    DirectX::XMFLOAT3 position = { 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT4 rotation = { 0.0f, 0.0f, 0.0f, 1.0f }; // クォータニオン (x,y,z,w)
    DirectX::XMFLOAT3 scale = { 1.0f, 1.0f, 1.0f };
    static inline ComponentTypeId sTypeId = kInvalidComponentType;
};

// TransformSystem が毎 tick 再計算する派生値 (シリアライズ対象外)
struct WorldMatrixComponent {
    DirectX::XMFLOAT4X4 value = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    static inline ComponentTypeId sTypeId = kInvalidComponentType;
};

// 親子関係 (engine_spec.md 4.4)。書き換えは World::SetParent 経由のみ
// (直接編集すると子リスト/深度の整合が壊れる)。depth は TransformSystem の再構築で更新される
struct HierarchyComponent {
    EntityID parent = kNullEntity;
    EntityID firstChild = kNullEntity;
    EntityID nextSibling = kNullEntity;
    uint32_t depth = 0;
    uint32_t pad = 0; // 明示パディング (ハッシュ対象になるため未初期化バイトを残さない)
    static inline ComponentTypeId sTypeId = kInvalidComponentType;
};

struct MeshRendererComponent {
    AssetID mesh = {};
    AssetID material = {};
    static inline ComponentTypeId sTypeId = kInvalidComponentType;
};

struct CameraComponent {
    float fovYDeg = 60.0f;
    float nearZ = 0.1f;
    float farZ = 1000.0f;
    int32_t isPrimary = 1;
    static inline ComponentTypeId sTypeId = kInvalidComponentType;
};

// ライト (spec 6.2)。向きはエンティティの前方 (+Z)、位置はワールド行列から取る。
// type: 0=Directional(平行光) / 1=Point(点光源) / 2=Spot(スポット)
struct LightComponent {
    DirectX::XMFLOAT3 color = { 1.0f, 1.0f, 1.0f };
    float intensity = 1.0f;
    DirectX::XMFLOAT3 ambient = { 0.15f, 0.16f, 0.18f };
    int32_t type = 0;           // 0=Directional 1=Point 2=Spot
    float range = 15.0f;        // Point/Spot: 減衰半径
    float spotInnerDeg = 25.0f; // Spot: フル強度の内角 (度)
    float spotOuterDeg = 35.0f; // Spot: 減衰端の外角 (度)
    float pad = 0.0f;
    static inline ComponentTypeId sTypeId = kInvalidComponentType;
};

// シーンファイル上の永続 ID (engine_spec.md 8.3 の差分適用で使用)。
// 保存時に未割り当てなら Scene の nextFileId から採番される
struct FileIdComponent {
    uint64_t value = 0;
    static inline ComponentTypeId sTypeId = kInvalidComponentType;
};

// パーティクルエミッタ定義 (engine_spec.md 7 章)。
// CPU / GPU 両バックエンドが同じこのデータを解釈する (spec 7.1)。
// ランタイム状態 (プール/乱数ストリーム/放出累積) はバックエンド側が
// EntityID で管理し、このコンポーネントは純粋な定義データ + シード
struct ParticleEmitterComponent {
    // ---- Emission ----
    float rate = 200.0f;   // 個/秒
    int32_t shape = 2;     // 0=point 1=sphere 2=cone 3=box
    float shapeRadius = 0.15f;
    float coneAngleDeg = 20.0f;
    DirectX::XMFLOAT3 boxExtents = { 0.5f, 0.5f, 0.5f };
    // ---- 初期値 (範囲) ----
    float lifetimeMin = 1.2f;
    float lifetimeMax = 2.2f;
    float speedMin = 2.0f;
    float speedMax = 3.5f;
    float sizeMin = 0.10f;
    float sizeMax = 0.22f;
    DirectX::XMFLOAT4 colorBegin = { 1.0f, 0.85f, 0.35f, 1.0f };
    DirectX::XMFLOAT4 colorEnd = { 1.0f, 0.15f, 0.02f, 0.0f };
    float sizeEndScale = 0.25f; // 寿命終端でのサイズ倍率 (線形)
    // ---- 力場 ----
    DirectX::XMFLOAT3 gravity = { 0.0f, 1.5f, 0.0f };
    DirectX::XMFLOAT3 wind = { 0.0f, 0.0f, 0.0f };
    float turbulence = 0.0f;
    // ---- 描画 ----
    int32_t blendMode = 0; // 0=additive 1=alpha
    // ---- 決定論 ----
    uint32_t seed = 12345; // エミッタ別 RNG ストリームのシード (spec 7.3)
    int32_t maxParticles = 100000;
    static inline ComponentTypeId sTypeId = kInvalidComponentType;
};

// 衝突形状 (M7 トリガー / M20 ソリッド / M28a 形状拡張)。判定は Physics/Shapes.cpp に統合。
// box はエンティティ回転を考慮する OBB (M28a)。無回転なら M20 の AABB 判定とビット同一。
// 球はスケールの最大成分で拡大、capsule はローカル Y 軸・radius は max(sx,sz) スケール。
struct ColliderComponent {
    int32_t shape = 0; // 0=sphere 1=box(OBB) 2=capsule(ローカル Y 軸)
    float radius = 0.5f; // sphere / capsule
    DirectX::XMFLOAT3 halfExtents = { 0.5f, 0.5f, 0.5f }; // box
    int32_t isTrigger = 1;
    // ---- M28a 追加 (末尾 append = シーン/リプレイ互換維持) ----
    float height = 2.0f;   // capsule 全高 (両端の半球を含む)。線分半長 = max(0, height/2 − radius)
    float friction = 0.5f; // クーロン摩擦係数 (M28b のソルバで使用。ペアは sqrt(μa·μb))
    static inline ComponentTypeId sTypeId = kInvalidComponentType;
};

// 有効/無効フラグ (M10)。**このコンポーネントが無ければ有効**。
// enabled==0 で自身を sim (スクリプト/衝突/パーティクル) と描画から外す。
// 既存シーンは ActiveComponent を持たない → 挙動もワールドハッシュも不変 (ReplayFile bump 不要)。
// 注: 現状は自エンティティのみ判定 (階層伝播は将来拡張)
struct ActiveComponent {
    int32_t enabled = 1;
    static inline ComponentTypeId sTypeId = kInvalidComponentType;
};

// ---- プレハブ (M13) ----
// インスタンスの **ルートのみ** に付く。元 .prefab.json をパスハッシュで指す
// (PrefabLibrary のキー = PrefabInstanceComponent.prefabHash)。
// **無ければプレハブインスタンスでない** (opt-in → 既存シーンは挙動もハッシュも不変)。
// シリアライズ+ハッシュ対象だが、どのシステムにも参加しない純データタグ (sim 非影響)
struct PrefabInstanceComponent {
    uint64_t prefabHash = 0;
    static inline ComponentTypeId sTypeId = kInvalidComponentType;
};

// インスタンス内の **全エンティティ** に付く。プレハブ内ローカル fileId (localId) を保持し、
// ベースとのオーバーライド diff / 伝播に使う。所属インスタンスのルートは親を上に辿り、
// 最初に PrefabInstanceComponent を持つ祖先 (= 自身含む)。rootFileId を持たないため、
// 複製 (CloneSubtree) しても壊れない (localId はそのまま流用でよい)
struct PrefabLinkComponent {
    uint64_t localId = 0;
    static inline ComponentTypeId sTypeId = kInvalidComponentType;
};

// ---- アニメーション (M14) ----
// AnimationClip (.anim.json) を再生する。**無ければ何もしない** (opt-in → 既存シーン不変)。
// 時間は **tick カウント (int)** で持つ (float 秒累積は決定論違反リスク)。
// トラックの補間係数は整数キー位置の比なのでプラットフォーム非依存。serialize+hash 対象
struct AnimatorComponent {
    AssetID clip = {};       // AnimationClip アセット (AnimationLibrary のキー = パスハッシュ)
    int32_t timeTicks = 0;   // 現在の再生位置 (clip 内 tick)
    int32_t speed = 1;       // 1 update あたりに進める tick 数 (負で逆再生)
    int32_t loop = 1;        // 0=一度きり(末尾停止) 1=ループ
    int32_t playing = 1;     // 0=停止
    static inline ComponentTypeId sTypeId = kInvalidComponentType;
};

// ---- スケルタルスキニング (M18) ----
// スキン付き glTF メッシュ。ポーズ (ボーン行列) は描画専用のため **hash しない**
// (kComponentNoHash → 既存シーンのリプレイ不変 = bump 不要)。時刻は tick で保持し、
// RenderSystem がフレーム毎に SkinnedModel からサンプルしてボーンパレットを構築する。
struct SkinnedMeshComponent {
    AssetID model = {};    // SkinnedModelLibrary のキー (glTF skin 由来)
    int32_t clip = 0;      // 再生クリップ index
    int32_t timeTicks = 0; // 再生位置 (tick、60Hz 前提でサンプル秒 = timeTicks/60)
    int32_t playing = 1;   // 0=停止
    static inline ComponentTypeId sTypeId = kInvalidComponentType;
};

// ---- 剛体物理 (M20) ----
// 重力・速度で動く動的ボディ。**無ければ物理は関与しない** (opt-in → 既存シーンのハッシュ/リプレイ不変)。
// ソリッドな ColliderComponent (isTrigger==0) が衝突面。位置は LocalTransform に書き戻される
// (ルート = ワールド位置前提)。velocity は sim 状態なので **hash 対象** (決定論的に積分される)。
// PhysicsSystem が TransformSystem 直前に積分 + 固定反復ソルバで貫通を解消する。
struct RigidbodyComponent {
    DirectX::XMFLOAT3 velocity = { 0.0f, 0.0f, 0.0f }; // m/s (ワールド、ルート)
    float mass = 1.0f;             // >0。<=0 は 1 として扱う
    float linearDamping = 0.0f;    // 毎 tick の速度減衰率 (0=無、0.02≈2%/tick)
    float restitution = 0.0f;      // 反発係数 0..1 (接触ペアは min を採用)
    float gravityScale = 1.0f;     // 重力倍率 (0=無重力)
    int32_t isKinematic = 0;       // 1=物理で動かさない (スクリプト制御。他はブロックする)
    static inline ComponentTypeId sTypeId = kInvalidComponentType;
};

// ---- ゲーム内 UI (M21) ----
// スクリーン空間の UI 要素。**無ければ何も描かない** (opt-in)。描画専用なので **kComponentNoHash**
// (ワールドハッシュ非対象 → 既存シーンのリプレイ不変 = bump 不要)。ただしシリアライズはされる
// (シーンに UI を保存できる)。ボタン操作は描画に非関与 — スクリプトが InputSnapshot の
// マウス (決定論) でヒットテストして gameplay を駆動する (エンジンは描画のみ)。
// 座標は anchor 基準のピクセルオフセット (解像度非依存)。
struct UIElementComponent {
    int32_t kind = 0;     // 0=panel/image, 1=text, 2=button
    int32_t anchor = 0;   // 9-grid: 0=左上 1=上中 2=右上 3=左中 4=中央 5=右中 6=左下 7=下中 8=右下
    float x = 0.0f;       // anchor 基準の水平オフセット (px)
    float y = 0.0f;       // anchor 基準の垂直オフセット (px)
    float w = 160.0f;     // 幅 (px)。kind==1(text) は背景を描かない
    float h = 40.0f;      // 高さ (px)
    DirectX::XMFLOAT4 color = { 1.0f, 1.0f, 1.0f, 1.0f }; // panel/button 背景色 or text 色
    AssetID texture = {}; // kind==0 の画像 (0=単色)
    float fontScale = 1.0f; // text/button ラベルのフォント倍率
    int32_t order = 0;    // 描画順 (小さいほど奥)。同値は entity.index 昇順
    char text[64] = {};   // kind 1/2 のテキスト/ラベル (UTF-8、ASCII 描画)
    static inline ComponentTypeId sTypeId = kInvalidComponentType;
};

// ---- Animator Controller (M22) ----
// ステートマシンでアニメーションクリップを切替・ブレンドする。**無ければ何もしない** (opt-in)。
// LocalTransform (ハッシュ対象) を駆動するので状態は決定論・**hash 対象** (kComponentNoHash を付けない)。
// 時刻は tick、ブレンド係数は transitionTick/duration の整数比 → プラットフォーム非依存。
// params は整数パラメータ (遷移条件が参照)。遷移中は transitionTo>=0。
struct AnimatorControllerComponent {
    AssetID controller = {};         // .controller.json (ControllerLibrary のキー = パスハッシュ)
    int32_t currentState = 0;        // 現在の state index
    int32_t stateTimeTicks = 0;      // 現 state の再生位置 (tick)
    int32_t transitionTo = -1;       // 遷移中の目標 state (-1 = 遷移なし)
    int32_t transitionTick = 0;      // 遷移経過 tick
    int32_t transitionDuration = 0;  // 遷移全長 tick
    int32_t transitionToTime = 0;    // 遷移先 state の再生位置 (tick)
    int32_t params[4] = { 0, 0, 0, 0 }; // 整数パラメータ (遷移条件が index 0..3 を参照)
    static inline ComponentTypeId sTypeId = kInvalidComponentType;
};

class World;

// エンティティが有効か (ActiveComponent が無ければ有効 / enabled==0 なら無効)
bool IsEntityActive(World& world, EntityID e);

// 固定順で登録する (TypeId の決定論)。多重呼び出しは無害
void RegisterBuiltinComponents();

} // namespace mye
