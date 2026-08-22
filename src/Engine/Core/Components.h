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
    // 2=distortion (M42d): 歪みバッファへ描き postfx で UV オフセット。既存 Int32 の値域
    // 拡張のみ = フィールド追加なし = hash 不変。強度は color のアルファで表現。
    // v1 制限: CPU バックエンド限定 (GPU は skip) / enablePostFx=false では描かれない
    int32_t blendMode = 0; // 0=additive 1=alpha 2=distortion
    // ---- 決定論 ----
    uint32_t seed = 12345; // エミッタ別 RNG ストリームのシード (spec 7.3)
    int32_t maxParticles = 100000;
    // ---- ライフサイクル (M32a: 末尾 append。既定 = 従来挙動と同一) ----
    int32_t playing = 1;       // 0=放出停止 (生存粒子は生きる)。age/emitAccum 凍結
    int32_t durationTicks = 0; // 放出ウィンドウ長 (tick)。0=無限
    int32_t looping = 1;       // durationTicks>0 のとき 1=ウィンドウ末で巻き戻し再放出
    int32_t burstCount = 0;    // ウィンドウ先頭 (age==0) の単発放出数
    // ---- 多点グラデーション (M32a: begin/end + opt-in 中間キー。T∈(0,1) で有効、0=無効) ----
    DirectX::XMFLOAT4 colorMid1 = { 1.0f, 1.0f, 1.0f, 1.0f };
    float colorMidT1 = 0.0f;
    DirectX::XMFLOAT4 colorMid2 = { 1.0f, 1.0f, 1.0f, 1.0f };
    float colorMidT2 = 0.0f;
    float sizeMidScale = 1.0f;
    float sizeMidT = 0.0f;
    // ---- テクスチャ / フリップブック (M32b) ----
    AssetID texture = {};  // 空=procedural ソフト円
    int32_t flipTilesX = 1;
    int32_t flipTilesY = 1;
    float flipCycles = 1.0f; // 寿命あたりのフリップブック周回数
    // ---- ソフトパーティクル (M32c) ----
    float softFadeDistance = 0.0f; // >0 で深度フェード
    // ---- 深度バッファ衝突 (M42e: 末尾 append。hash 対象フィールド追加 = golden 再記録済) ----
    // GPU バックエンド限定の見た目効果 (spec 7.5 の等価規約に例外明記)。CPU は素通し。
    // 深度は前フレームの描画結果 = 1 フレーム遅延 / 画面外・空ピクセルとは衝突しない
    int32_t depthCollision = 0;   // 1 で GPU sim がシーン深度と衝突
    float collisionBounce = 0.4f; // 反発係数 (0..1)
    // ---- 実行時 (非登録=非ハッシュ・非シリアライズ。スクリプト/エディタが即時バーストを積む) ----
    int32_t pendingBurst = 0; // 次の Update で消費され 0 に戻る (常に tick 末ハッシュ前に 0)
    static inline ComponentTypeId sTypeId = kInvalidComponentType;
};

// 衝突形状 (M7 トリガー / M20 ソリッド / M28a 形状拡張)。判定は Physics/Shapes.cpp に統合。
// box はエンティティ回転を考慮する OBB (M28a)。無回転なら M20 の AABB 判定とビット同一。
// 球はスケールの最大成分で拡大、capsule はローカル Y 軸・radius は max(sx,sz) スケール。
struct ColliderComponent {
    int32_t shape = 0; // 0=sphere 1=box(OBB) 2=capsule(ローカル Y 軸) 3=mesh (静的専用、M41)
    float radius = 0.5f; // sphere / capsule
    DirectX::XMFLOAT3 halfExtents = { 0.5f, 0.5f, 0.5f }; // box
    // M51 後続で int32→bool 化 + 既定をソリッドへ (旧データの 0/1 数値は FieldFromJson が受理)。
    // 1B 化で WorldHash のバイト列が変わるため golden は再記録済み。後続 3B はパディング
    // (ハッシュ/シリアライズはフィールド単位なので不算入)
    bool isTrigger = false;
    // ---- M28a 追加 (末尾 append = シーン/リプレイ互換維持) ----
    float height = 2.0f;   // capsule 全高 (両端の半球を含む)。線分半長 = max(0, height/2 − radius)
    float friction = 0.5f; // クーロン摩擦係数 (M28b のソルバで使用。ペアは sqrt(μa·μb))
    // ---- M36a 追加: 衝突レイヤー (hash 対象のフィールド追加 → golden 再記録済) ----
    int32_t layer = 0;          // 所属レイヤー 0..31 (名前は project_settings.json、sim は index のみ)
    uint32_t mask = 0xFFFFFFFFu; // 衝突相手レイヤーのビット集合。判定は双方向 (CanCollide)
    AssetID meshAsset = {};     // M41 予約: 静的メッシュコライダー (空 = 従来形状。M36 では未使用)
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
    // M48c: 入れ子インスタンスのルートが「外側ベースのどのエントリか」を指す localId。
    // 0 = 入れ子でない (シーンに直接配置)。**PrefabLink.localId とはドメインが違う** —
    // 入れ子ルートの PrefabLink は内側ベースのドメイン (常に 1) なので、外側での位置を
    // 記録する場所がここにしかない。詳細は Prefab.h の「ID ドメイン」節
    uint64_t outerLocalId = 0;
    static inline ComponentTypeId sTypeId = kInvalidComponentType;
};

// インスタンス内の **全エンティティ** に付く。プレハブ内ローカル fileId (localId) を保持し、
// ベースとのオーバーライド diff / 伝播に使う。所属インスタンスのルートは親を上に辿り、
// 最初に PrefabInstanceComponent を持つ祖先 (= 自身含む)。rootFileId を持たないため、
// 複製 (CloneSubtree) しても壊れない (localId はそのまま流用でよい)。
// **入れ子 (M48c): localId は「自分が直接所属するインスタンスのベース」のドメインの値**。
// 内側インスタンスのメンバは内側ベースの番号を持ち、外側の連番では上書きされない
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
    // ---- M28b 追加 (末尾 append = シーン互換維持)。回転剛体 ----
    DirectX::XMFLOAT3 angularVelocity = { 0.0f, 0.0f, 0.0f }; // rad/s (ワールド)。sim 状態 = hash 対象
    float angularDamping = 0.05f;  // 毎 tick の角速度減衰率 (スタック静止安定の柱の 1 つ)
    int32_t freezeRotation = 0;    // 1 = 回転積分・角応答をしない (M28a 以前の並進のみ挙動)
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
    char text[256] = {};  // kind 1/2 のテキスト/ラベル (UTF-8。M34 で日本語対応 + 256B 化)
    // ---- M35 拡張 (末尾 append、NoHash なので旧シーンは既定値ロードで互換) ----
    float fillAmount = 1.0f; // kind0 の塗り率 0..1 (HP バー。fillMode!=0 で有効)
    int32_t fillMode = 0;    // 0=off 1=水平(左→右) 2=垂直(下→上)
    DirectX::XMFLOAT4 sliceBorder = { 0, 0, 0, 0 }; // 9-slice 境界 px (l,t,r,b)。sliced!=0 で有効
    int32_t sliced = 0;      // kind0 + texture 有りで 9-slice 描画
    int32_t focusable = 0;   // パッドナビ候補 (状態はスクリプト側 — UINav.h 参照)
    int32_t focused = 0;     // フォーカス枠の表示 (表示専用。スクリプトが書く)
    // ---- M51e 拡張 (末尾 append、NoHash なので旧シーンは既定値ロードで互換) ----
    int32_t space = 0;        // 0=screen 基準 / 1=最寄りの UIElement 祖先の解決済み矩形基準 (UILayout.h)
    int32_t clipChildren = 0; // !=0 で子孫要素を自矩形へシザークリップ (自分自身は切らない)
    int32_t align = 0;        // kind1(text) の矩形内整列 (anchor と同じ 9-grid 0..8)。ボタンラベルは中央固定
    int32_t wrap = 0;         // kind1(text) の文字単位折返し (幅 w で折る、日本語前提)。0=off
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

// ---- 定常力 (M29a) ----
// Rigidbody へ毎 tick 力/トルクを加え続ける。**無ければ何もしない** (opt-in → 既存シーン不変)。
// Rigidbody の velocity (hash 対象) を決定論的に駆動する入力データなので **hash 対象**。
// Rigidbody 非所持・kinematic のエンティティでは無効。
struct ConstantForceComponent {
    DirectX::XMFLOAT3 force = { 0.0f, 0.0f, 0.0f };  // N。毎 tick v += F/m·dt
    DirectX::XMFLOAT3 torque = { 0.0f, 0.0f, 0.0f }; // N·m。毎 tick ω += I⁻¹·τ·dt (freezeRotation 中は無効)
    int32_t relative = 0; // 0=World 1=Local (エンティティのワールド姿勢で回転して適用)
    static inline ComponentTypeId sTypeId = kInvalidComponentType;
};

// ---- 距離バネジョイント (M29a) ----
// 自エンティティと connectedEntity を自然長 restLength のばねで繋ぐ。**無ければ何もしない** (opt-in)。
// 速度レベルの単一インパルス (ステートレス = warm starting 不要) で、減衰は implicit
// (分母に組込) なので damping はいくら大きくても発散しない。剛性の安定目安:
// stiffness·dt²/mass < 4 (dt=1/60 → mass=1 で k < 14400)。
// どちらか一方が動的 Rigidbody なら機能する (もう一方は不動アンカー扱い)。hash 対象。
struct SpringJointComponent {
    EntityID connectedEntity = kNullEntity; // 接続先 (null = 何もしない)
    float restLength = 2.0f; // 自然長 (m)
    float stiffness = 50.0f; // ばね定数 k (N/m)
    float damping = 5.0f;    // 減衰係数 c (N·s/m)
    static inline ComponentTypeId sTypeId = kInvalidComponentType;
};

// ---- キャラクターコントローラ (M29b) ----
// カプセル形状のキネマティック移動体 (move-then-depenetrate + 接地判定)。**無ければ何もしない**
// (opt-in)。Rigidbody と両方持つ場合は Rigidbody が優先され CC は無効。カプセルは常にワールド
// Y 軸 (エンティティの回転は形状に影響しない)。LocalTransform (hash 対象) を駆動する sim
// 状態なので **hash 対象**。moveInput / jumpSpeed は決定論スクリプトが ABI で書く sim 入力。
// ソリッドな Collider (capsule) を併用すると剛体側からもブロック面として見える (推奨パターン。
// その場合 collider は tick 頭の位置で判定される = 1 tick 遅延は許容)。
struct CharacterControllerComponent {
    float radius = 0.3f;         // カプセル半径 (m)
    float height = 1.8f;         // 全高 (両端の半球込み)。線分半長 = max(0, height/2 − radius)
    float slopeLimitDeg = 45.0f; // これ以下の傾斜の面を「接地」とみなす (面法線と Y の角度)
    float skinWidth = 0.02f;     // 接地プローブの探り距離
    float gravityScale = 1.0f;
    DirectX::XMFLOAT3 moveInput = { 0.0f, 0.0f, 0.0f }; // 水平移動速度 m/s (y 無視)。保持される
    DirectX::XMFLOAT3 velocity = { 0.0f, 0.0f, 0.0f };  // y=重力積分状態、x/z=前 tick の実効速度
    float jumpSpeed = 0.0f; // >0 なら次 tick 接地時に vy=jumpSpeed (接地可否に関わらず消費)
    int32_t isGrounded = 0; // 前 tick の接地判定 (読み取り専用)
    static inline ComponentTypeId sTypeId = kInvalidComponentType;
};

// ---- スプライト/ビルボード (M29c) ----
// ワールド空間のテクスチャ付き板。**無ければ何も描かない** (opt-in)。描画専用なので
// **kComponentNoHash** (既存シーンのリプレイ不変 = bump 不要)。VfxRenderer が
// 透明メッシュの後・パーティクルの前に描く (透明メッシュとの相互ソートはしない)。
struct SpriteRendererComponent {
    AssetID texture = {};                          // 空 = 白 (単色板)
    DirectX::XMFLOAT4 color = { 1.0f, 1.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT2 size = { 1.0f, 1.0f };       // ワールド単位 (幅, 高さ)
    int32_t billboardMode = 0; // 0=Billboard(常にカメラ正対) 1=BillboardY(ヨーのみ) 2=World(姿勢基底)
    static inline ComponentTypeId sTypeId = kInvalidComponentType;
};

// ---- トレイル (M29c) ----
// 移動軌跡のリボン。**無ければ何も描かない** (opt-in)。描画専用 = **kComponentNoHash**。
// 点列はコンポーネント外 (VfxRenderer の TrailStore) に常駐し、tick 側で蓄積される
// (Render 側で蓄積すると SceneView/GameView の多重描画で多重サンプルされるため)。
struct TrailRendererComponent {
    float duration = 0.5f;          // 点の寿命 (秒)
    float width = 0.2f;             // リボン幅 (新しい端。古い端へ 0 にテーパ)
    DirectX::XMFLOAT4 colorBegin = { 1.0f, 1.0f, 1.0f, 1.0f }; // 新しい端
    DirectX::XMFLOAT4 colorEnd = { 1.0f, 1.0f, 1.0f, 0.0f };   // 古い端
    float minVertexDistance = 0.05f; // この距離以上動いたら点を追加
    int32_t emitting = 1;            // 0 = 新規点の追加停止 (既存点は寿命で消える)
    static inline ComponentTypeId sTypeId = kInvalidComponentType;
};

// ---- 3D テキスト (M29c) ----
// ワールド空間の中央揃え単一行テキスト。**無ければ何も描かない** (opt-in)。描画専用 =
// **kComponentNoHash**。フォントは UIRenderer の埋め込み 8x8 アトラスを共有。
// スクリプトからは ABI SetTextMeshText で文字列を設定できる (非 hash なので sim 安全)。
struct TextMeshComponent {
    char text[256] = "Text"; // UTF-8 (M34 で日本語対応 + 256B 化)
    float fontScale = 1.0f; // 1.0 で行高 ≈ 0.3 ワールド単位
    DirectX::XMFLOAT4 color = { 1.0f, 1.0f, 1.0f, 1.0f };
    int32_t billboardMode = 0; // SpriteRenderer と同じ enum
    static inline ComponentTypeId sTypeId = kInvalidComponentType;
};

// ---- スカイボックス (M29d) ----
// 背景の空。シーン内の **最初の active な 1 個** (entity.index 最小) を使用 (isPrimary カメラ前例)。
// **無ければ従来の clearColor 背景** (opt-in)。描画専用 = **kComponentNoHash**。
// mode=1 (Cubemap) は将来拡張の予約 — 現状は Gradient にフォールバックする。
struct SkyboxComponent {
    int32_t mode = 0; // 0=Gradient 1=Cubemap (予約。現状 Gradient フォールバック)
    DirectX::XMFLOAT4 topColor = { 0.24f, 0.42f, 0.83f, 1.0f };     // 天頂
    DirectX::XMFLOAT4 horizonColor = { 0.74f, 0.81f, 0.90f, 1.0f }; // 地平線
    DirectX::XMFLOAT4 bottomColor = { 0.28f, 0.25f, 0.22f, 1.0f };  // 地面方向
    AssetID cubemapTexture = {}; // 予約 (mode=1 用。DDS cubemap ローダは未実装)
    static inline ComponentTypeId sTypeId = kInvalidComponentType;
};

// ---- フォグ (M29d) ----
// 距離フォグ。シーン内の **最初の active な 1 個** を使用。**無ければフォグ無し** (opt-in)。
// 描画専用 = **kComponentNoHash**。不透明+透明メッシュに適用 (forward/deferred 両パス)。
// パーティクル/スプライト/スカイボックスには掛からない (v1 の制限)。
struct FogComponent {
    int32_t mode = 0; // 0=Linear (start..end) 1=Exp (1-e^-ρd) 2=Exp2 (1-e^-(ρd)²)
    DirectX::XMFLOAT4 color = { 0.65f, 0.70f, 0.75f, 1.0f };
    float density = 0.02f; // Exp/Exp2 用
    float start = 10.0f;   // Linear 用
    float end = 80.0f;     // Linear 用
    // ---- M43a: ハイトフォグ + 太陽インスキャッタ (末尾 append。既定 = 恒等 = 従来の見た目) ----
    float heightFalloff = 0.0f;      // 高度による密度の指数減衰係数 (0 = 高さ一様)
    float baseHeight = 0.0f;         // 密度基準の高さ (これより上で薄くなる)
    float inscatterIntensity = 0.0f; // 太陽方向へのフォグ色寄せ (0 = 無効)
    float inscatterPower = 8.0f;     // 寄せの鋭さ (大きいほど太陽周辺に集中)
    static inline ComponentTypeId sTypeId = kInvalidComponentType;
};

// ---- カメラ別ポストプロセス (M29e) ----
// シーンカメラに付けて既存 PostProcess::Settings をシーンオーサリングする。
// **無ければグローバル設定 (renderSystem.postFxSettings) のまま** (opt-in)。
// 描画専用 = **kComponentNoHash**。SceneView のエディタカメラ (CameraOverride) には
// 適用されない (エディタ操作視界は不変)。enablePostFx=false 時は無視される。
struct CameraPostFxComponent {
    float exposure = 1.0f;       // トーンマップ前の露出倍率
    int32_t tonemapMode = 1;     // 0=Passthrough 1=ACES 2=Reinhard
    int32_t bloomOn = 1;         // 0=Off 1=On
    float bloomThreshold = 1.0f; // bright-pass しきい値 (輝度)
    float bloomIntensity = 0.6f; // 合成強度
    int32_t fxaaOn = 1;          // 0=Off 1=On
    // ---- M32d: 追加ポスト効果 (末尾 append。既定 = 無効 = 従来の見た目) ----
    float chromAberration = 0.0f;   // 色収差 (UV スケール、0=off)
    float vignetteIntensity = 0.0f; // 周辺減光 (0=off)
    float vignetteRadius = 0.75f;   // 減光開始半径 (0..1)
    float saturation = 1.0f;        // 彩度 (1=変化なし)
    float contrast = 1.0f;          // コントラスト (1=変化なし)
    DirectX::XMFLOAT4 colorFilter = { 1.0f, 1.0f, 1.0f, 1.0f }; // 乗算カラーフィルタ
    // ---- M40d: SSAO パラメータ (Deferred のみ。末尾 append、NoHash なので hash 不変) ----
    float ssaoRadius = 0.8f;    // サンプル半球の半径 (ワールド単位)
    float ssaoIntensity = 1.0f; // 遮蔽の効き (0=off 相当)
    // ---- M43b: スクリーンスペースゴッドレイ (末尾 append、NoHash なので hash 不変) ----
    float godrayIntensity = 0.0f; // 空マスクの明るさ倍率 (0=off)
    float godrayDecay = 0.95f;    // 放射ブラーのタップ毎減衰
    // ---- M44a: カラーグレーディング LUT (末尾 append、NoHash なので hash 不変) ----
    AssetID lutTexture = {};   // 256x16 ストリップ (sRGB off でロードされる)
    float lutIntensity = 0.0f; // 0=off / 1=LUT 全適用
    // ---- M44b: 自動露出 (末尾 append、NoHash なので hash 不変) ----
    int32_t autoExposure = 0; // 0=off 1=on (輝度ヒストグラム → 露出適応)
    float aeSpeed = 3.0f;     // 適応速度 (1/s)
    float aeMin = 0.25f;      // 露出倍率の下限
    float aeMax = 4.0f;       // 上限
    // ---- M44c: 被写界深度 (末尾 append、NoHash なので hash 不変) ----
    float dofFocusDistance = 10.0f; // 焦点距離 (ビュー空間 z)
    float dofFocusRange = 5.0f;     // 焦点面からボケが最大に達するまでの距離
    float dofMaxRadius = 0.0f;      // 最大ボケ半径 (px、0=off)
    // ---- M44d: カメラモーションブラー (末尾 append、NoHash なので hash 不変) ----
    float motionBlurIntensity = 0.0f; // 0=off (SceneView は常に強制 0)
    float mbMaxPixels = 16.0f;        // 速度クランプ (px)
    static inline ComponentTypeId sTypeId = kInvalidComponentType;
};

// ---- 合成エフェクト (M32e) ----
// 複数エミッタ/スプライト/トレイルの子階層を 1 つのエフェクトとして束ね、寿命を管理する。
// エフェクト = プレハブ (子に複数エミッタ) + Animator (.anim.json で rate/color を時間変化) +
// この EffectComponent (ライフサイクル)。移動はルートの LocalTransform で行う (親子変換)。
// **DestroyEntity と子エミッタの playing を駆動する = sim 構造変更なので hash 対象** (NoHash 無し)。
// opt-in (無ければ EffectSystem 完全 no-op) なので既存シーンは不変 = golden 再記録不要。
struct EffectComponent {
    int32_t durationTicks = 120; // 放出フェーズ長 (tick)。0=手動制御 (自動停止しない)
    int32_t lingerTicks = 120;   // 放出停止後、残粒子の消滅を待つ猶予 (autoDestroy 用)
    int32_t elapsedTicks = 0;    // 経過 tick (ReadOnly、sim 状態)
    int32_t playing = 1;         // 0=停止 (elapsed 凍結)
    int32_t looping = 0;         // 1=duration 毎に elapsed 巻き戻し + 子エミッタ再開 (autoDestroy 無効)
    int32_t autoDestroy = 1;     // 1=duration+linger 経過で自エンティティ (子孫ごと) 破棄
    static inline ComponentTypeId sTypeId = kInvalidComponentType;
};

// ---- オーディオ (M45e) ----
// 3D 定位の基準点。シーン内の **最初の active な 1 個** (entity.index 最小) を使い、
// 無ければ primary カメラへフォールバックする (SkyboxComponent と同じ「最初の 1 個」規約)。
// 位置と向きは WorldMatrix から取る (+Z = 前、+Y = 上。X3DAudio もエンジンも左手系)。
// **オーディオは決定論レーン外の出力 sink なので kComponentNoHash** = 既存シーンのハッシュ不変。
struct AudioListenerComponent {
    int32_t enabled = 1; // 0 = このリスナーを無視 (複数置いて切り替える用)
    static inline ComponentTypeId sTypeId = kInvalidComponentType;
};

// シーンに置く音源。再生パラメータの本体は `.sound.json` (SoundAsset) 側にあり、
// ここはその **インスタンス毎の上書き**を持つ。**kComponentNoHash** (上と同じ理由)。
//
// 上書きの規約:
//   - volume / pitch は アセット値への **乗算** (1.0 = アセットのまま)
//   - loop は 3 値: -1 = アセット既定 / 0 = ループしない / 1 = ループする
//   - bus は空文字列 = アセット既定。非空なら実行中ミキサーの名前で解決
//   - priority は -1 = アセット既定
//   - **3D 系 (spatialBlend 以下) は overrideAttenuation != 0 のときだけ効く** —
//     既定ではアセット 1 箇所を直せば全インスタンスに効く方が扱いやすいため
struct AudioSourceComponent {
    // ★フィールド名は `sound` にすること。`clip` は AnimatorComponent.clip =
    //   アニメーションクリップの既存規約で、被せると Inspector の AssetRef ピッカーが壊れる
    AssetID sound = {};        // .sound.json の GUID
    int32_t playOnAwake = 1;   // Play 開始時 (と生成時) に自動再生する
    int32_t loop = -1;         // -1 = アセット既定 / 0 = 単発 / 1 = ループ
    float volume = 1.0f;       // アセット volume への乗算
    float pitch = 1.0f;        // アセット pitch への乗算
    int32_t mute = 0;          // 1 = 無音 (再生自体は続く)
    int32_t priority = -1;     // -1 = アセット既定。大きいほど重要 (VoicePolicy と同じ規約)
    char bus[64] = {};         // 空 = アセット既定
    int32_t overrideAttenuation = 0; // 1 = 以下の 3D 値でアセットを上書きする
    float spatialBlend = 1.0f; // 0 = 2D (定位も減衰もドップラーも無し) / 1 = フル 3D
    float minDistance = 1.0f;  // これより近ければ減衰なし
    float maxDistance = 50.0f; // これ以遠は無音 (SpatialMath.h の規約)
    int32_t rolloff = 0;       // SoundRolloff: 0=Logarithmic 1=Linear 2=Inverse
    float dopplerScale = 1.0f; // 0 = ドップラー無効 (速度推定ごとスキップされる)
    float reverbSend = 0.0f;   // リバーブバスへの送り量 (0 = ドライのみ)
    static inline ComponentTypeId sTypeId = kInvalidComponentType;
};

// ---- 部位 = ソケット (M48f) ----
// 「コードから引けるアタッチポイント」。UE のソケット相当で、`SetEffect(leg)` のように
// **アセット側で決めた場所へランタイムが物を付ける**のが本来の用途。
// 部位は特別な構文ではなく **Part コンポーネントを持つ普通の子エンティティ** —
// 変換は LocalTransform、階層は HierarchyComponent がそのまま担う。
//
// - **kComponentHidden にしない**: Inspector で編集する対象そのものだから
// - opt-in (TypeId 末尾 append) なので既存シーンは不変 = ReplayFile bump 不要
//   (ActiveComponent / Rigidbody と同じ前例)
// - `source` を EntityRef にしているのは、インスタンス化・複製で
//   `RemapEntityRefsInComponents` が自動で追従してくれるため (fileId で保存される)
struct PartComponent {
    // 部位タグ = `mye::HashStr(タグ名)`。0 = 未分類。
    // **sim が見るのはハッシュ値だけ**で、名前表はエディタ専用テーブル (PartTagNames)。
    // ★タグ名を変えると ID が変わる = 既存シーンの参照が切れる (レイヤー番号と違う性質)
    uint64_t tag = 0;
    // 追従するジョイント名 (空 = 静的ソケット = 親に対して固定)。
    // 実際に骨へ追従させるのは M48g の PartFollowSystem
    char joint[64] = {};
    // 骨の供給元 (SkinnedMesh を持つエンティティ)。null または無効なら
    // **最も近い SkinnedMesh 祖先**へフォールバックする (`Parts::ResolvePartSource` が唯一の実装。
    // PartFollowSystem と Inspector のジョイント一覧が同じ答えを見るため)
    EntityID source = kNullEntity;
    static inline ComponentTypeId sTypeId = kInvalidComponentType;
};

// ---- 部位の範囲 (M49) ----
// 部位に「クリックできる/レイが当たる広がり」を持たせる任意の添え物。ボーンの無い
// 静的オブジェクトでも「メッシュの一部分」を部位として選択・判定できるようにする。
// Part と同じエンティティに載せる想定 (Parts::RaycastParts の対象は Part + PartBounds 両持ちのみ)。
// 部位エンティティ自体が PartFollowSystem で骨に追従するので、範囲もそのまま追従する。
//
// ★shape の番号は ShapePose (0=球 1=箱) と**逆**。変換は Parts::MakePartBoundsPose の
//   1 箇所に閉じ込めてあり、対応は PartSelfTest が機械検査する
struct PartBoundsComponent {
    int32_t shape = 0;                                    // 0=箱 1=球
    DirectX::XMFLOAT3 center = { 0.0f, 0.0f, 0.0f };      // ローカルオフセット
    DirectX::XMFLOAT3 halfExtents = { 0.5f, 0.5f, 0.5f }; // 球は x を半径に使う (y/z 無視)
    static inline ComponentTypeId sTypeId = kInvalidComponentType;
};

// ---- 入力レーンの結び付け (M52g) ----
// 「このエンティティはどのプレイヤーの入力で動くか」。`playerIndex` 以外は
// **エンジン (PlayerInputSystem) が毎 tick 上書きする読み取り専用ミラー**で、
// アクションマップ (assets\input\actions.json) をそのレーンの入力で評価した結果を写す。
//
// なぜミラーを ECS に置くのか (2 つとも効いている):
//  1. **ハッシュ被覆**。アクション状態は ECS の外にある tick 限りの一時値なので、入力を
//     レーン化しただけではワールドハッシュに 1 ビットも現れない = replay_verify が
//     レーンの配線を検査できない (記録側と検証側で対称に壊れて一致してしまう)。
//     ミラーを登録フィールドにすると「レーン 1 がレーン 0 を読んでいる」類の配線ミスが
//     その tick のハッシュ差として必ず出る。
//  2. **ABI を増やさずにスクリプトから読める**。v11 の GetComponentField (名前ハッシュで
//     任意コンポーネントの登録フィールドを引く汎用スロット) がそのまま使える。
//     レーン別の専用スロット (GetActionForPlayer 等) は M52i の ABI v13 へ束ねる
//     (M48h / M51h と同じ「bump は 1 マイルストーン 1 回」運用)。
//
// 参照は**アクション名ではなく index** で行う: 名前で持つと固定長文字列表が要るうえ、
// actions.json を並べ替えるたびにシーンが壊れる。index は actions.json の定義順
// (= ProjectSettings の表示順) そのもの。定義が無い index は 0 / false になる。
//
// opt-in (TypeId 末尾 append) なので既存シーンのハッシュは 1 バイトも変わらない
// = ReplayFile bump 不要 (Part / PartBounds と同じ判断)
struct PlayerInputComponent {
    int32_t playerIndex = 0; // 読むレーン (0..kMaxPlayers-1)。作者が設定する唯一の値
    // ---- 以下はエンジンが毎 tick 書く (Inspector では読み取り専用) ----
    // そのレーンに入力源があるか。レーン 0 はキーボード/マウスがあるので常に true、
    // レーン n>0 は XInput スロット n の接続状態そのもの
    bool connected = false;
    DirectX::XMFLOAT4 axes = { 0.0f, 0.0f, 0.0f, 0.0f }; // 軸 index 0..3 の値 [-1,+1]
    uint32_t heldBits = 0;     // アクション index 0..31 の held (bit i = actions[i])
    uint32_t pressedBits = 0;  // 同 pressed (この tick で押された)
    uint32_t releasedBits = 0; // 同 released
    static inline ComponentTypeId sTypeId = kInvalidComponentType;
};

// ---- 地形 (M58b、spec §6.5) ----
// **描画専用 (kComponentNoHash)** — 地形は sim / ワールドハッシュに 1 バイトも触らない。
// 地形コリジョン (ハイトフィールド = sim レーン入り) は engine_spec §6.5 で M59 送り。
//
// アセットを AssetID ではなく**相対パス文字列**で持つ理由が 2 つある:
//  1. AssetRef の Inspector ピッカーはフィールド名から Mesh/Material/Texture 等の
//     ランタイムライブラリを推定する (InspectorWindow::DrawAssetRef) が、地形は
//     そのライブラリを持たない — TerrainSystem がパスをキーに直接キャッシュする。
//  2. 相対パスならシーン JSON がチェックアウト先に依存しない。モデル由来のサブアセット ID が
//     正規化絶対パスのハッシュだったせいでシーンをコミットできなくなった M51j の穴を踏まない。
//
// opt-in (TypeId 末尾 append) なので既存シーンは 1 バイトも変わらない
struct TerrainComponent {
    char source[64] = {}; // assets\ からの相対パス (例 "terrain/demo.terrain.json")。空 = 無効
    // チャンク 1 辺のタイル数。**範囲外の値は TerrainSystem::ClampChunkTiles が丸める**ので
    // ここでは検査しない (Core 層は Engine 層のヘッダを読めないため定数を共有できない —
    // 既定 32 / 範囲 2..256 の正本は Engine\Engine\TerrainSystem.h の kTerrain*ChunkTiles)
    int32_t chunkTiles = 32;
    // ---- LOD (M58e) ----
    // LOD 1 へ落とす**カメラ空間深度** (m)。0 = LOD 無効 (既定 = 従来と 1 画素も変わらない)。
    // LOD n の切替は lodDistance * 2^n で、段が上がるほど遠い。段数の正本は
    // Engine\Engine\TerrainSystem.h の kTerrainLodCount (Core 層は Engine を読めない)
    float lodDistance = 0.0f;
    // LOD 境界の隙間を塞ぐスカートの深さ (m)。**0 = 自動**が既定で、「LOD 対がこの地形の縁に
    // 作りうる最大段差」を測って使う (TerrainSystem の ComputeMaxLodEdgeGap)。
    // > 0 で明示指定、**< 0 でスカート無し** (クラックの A/B 用)。
    // lodDistance == 0 のときは無視される (スカートは LOD 有効時にしか出ない)
    float skirtDepth = 0.0f;
    static inline ComponentTypeId sTypeId = kInvalidComponentType;
};

class World;

// エンティティが有効か (ActiveComponent が無ければ有効 / enabled==0 なら無効)
bool IsEntityActive(World& world, EntityID e);

// 固定順で登録する (TypeId の決定論)。多重呼び出しは無害
void RegisterBuiltinComponents();

} // namespace mye
