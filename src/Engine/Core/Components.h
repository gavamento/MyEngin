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

// 平行光源 + アンビエント (spec 6.2)。向きはエンティティの前方 (+Z) を使う
struct LightComponent {
    DirectX::XMFLOAT3 color = { 1.0f, 1.0f, 1.0f };
    float intensity = 1.0f;
    DirectX::XMFLOAT3 ambient = { 0.15f, 0.16f, 0.18f };
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

// 簡易衝突判定 (球 / AABB のオーバーラップ + トリガーイベント)。
// 回転は無視した近似 (AABB はワールド軸平行、球はスケールの最大成分で拡大)
struct ColliderComponent {
    int32_t shape = 0; // 0=sphere 1=aabb
    float radius = 0.5f;
    DirectX::XMFLOAT3 halfExtents = { 0.5f, 0.5f, 0.5f };
    int32_t isTrigger = 1; // 現状トリガーのみ (物理応答は範囲外 — spec 1.4)
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

class World;

// エンティティが有効か (ActiveComponent が無ければ有効 / enabled==0 なら無効)
bool IsEntityActive(World& world, EntityID e);

// 固定順で登録する (TypeId の決定論)。多重呼び出しは無害
void RegisterBuiltinComponents();

} // namespace mye
