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
    // M54b: 影を落とすか (0/1)。旧 `float pad` の枠をそのまま使っているので sizeof も
    // 既存フィールドの offsetof も 1 バイト動いていない (シーン JSON も .rep も無風)。
    // 対象は**局所ライト (点/スポット)** — 平行光の影は既存の CSM が常に担当する。
    // bool ではなく int32_t なのはパディングの 4 バイトを潰さないため
    int32_t castShadow = 0;
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

// M59a2: Collider.materialOverrideBits のビット割当。材料 (.physmat.json) を割り当てたまま
// 特定プロパティだけ既存フィールドの値へ戻すための opt-out。将来の材料プロパティ
// (M59f2 静止摩擦/転がり抵抗、M59b Cd) は下へ append する — 値はシーン JSON に焼かれるので
// 既存ビットの再割当は不可
constexpr uint32_t kPhysMatOverrideFriction = 1u << 0;    // 摩擦は Collider.friction を使う
constexpr uint32_t kPhysMatOverrideRestitution = 1u << 1; // 反発は Rigidbody.restitution (静的は 0)
// M59f2: 転がり抵抗だけ材料から切り離す。★摩擦のビットは **μs と μd の両方**を覆う —
// Collider 側の格納庫が friction 1 本しかない以上、片方だけ既存フィールドへ戻す意味が無い。
// 転がり抵抗には既存フィールドが無いので、このビットは「0 = 従来値を使う」を意味する
constexpr uint32_t kPhysMatOverrideRolling = 1u << 2;

// 衝突形状 (M7 トリガー / M20 ソリッド / M28a 形状拡張)。判定は Physics/Shapes.cpp に統合。
// box はエンティティ回転を考慮する OBB (M28a)。無回転なら M20 の AABB 判定とビット同一。
// 球はスケールの最大成分で拡大、capsule はローカル Y 軸・radius は max(sx,sz) スケール。
struct ColliderComponent {
    // 0=sphere 1=box(OBB) 2=capsule(ローカル Y 軸) 3=mesh (静的専用、M41)
    // 4=terrain heightfield (静的専用、M59i)。3 と 4 は meshAsset を共有する
    // (3 = メッシュ資産 / 4 = `.terrain.json`)。**自然な使い方は TerrainComponent と
    //  同じエンティティに置いて同じ地形を指すこと**
    int32_t shape = 0;
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
    // shape=3 なら静的メッシュ資産、shape=4 なら `.terrain.json` (M59i)。空 = 従来形状
    AssetID meshAsset = {};
    // ---- M59a2 追加: 物理マテリアル (末尾 append = シーン/リプレイ互換維持) ----
    // 摩擦/反発の解決順は「overrideBits のビット → 既存フィールド / 材料割当あり → .physmat 値 /
    // 未割当 → 既存フィールド」(PhysicsSystem.h の SelectFriction/SelectRestitution が正本)。
    // 未割当なら従来と同じメモリを同じ経路で読む = 既存シーンはビット同一
    AssetID physMaterial = {};         // .physmat.json (PhysMatLibrary のキー。空 = 未割当)
    uint32_t materialOverrideBits = 0; // kPhysMatOverride* の集合 (立てたビットは材料より既存フィールドが勝つ)
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
    // true = 回転積分・角応答をしない (M28a 以前の並進のみ挙動)。M59a2 後続で int32→bool 化
    // (M28b 当時は FieldType::Bool が無かっただけで機能上の理由は無い。旧シーンの 0/1 数値は
    // isTrigger と同じ FieldFromJson のシムが受理。1B 化でハッシュのバイト列は変わるが挙動不変)
    bool freezeRotation = false;
    // ---- M59a2 追加: 密度→質量導出 (opt-in、末尾 append = シーン互換維持) ----
    // true かつ コライダーに材料割当済みなら 質量 = 材料密度 × ワールドスケール済み形状体積
    // (PhysicsSystem.h の ResolveBodyMass が正本。ソルバと ABI の AddForce/AddImpulse/AddTorque
    // が同じ関数を通る = 質量は常に一義)。材料なし / コライダーなし / mesh 形状は mass へ
    // フォールバック (= 従来値)。ON 中も mass フィールドは保存され OFF で復帰する
    bool useDensity = false;
    // ---- M59f1 追加 (末尾 append) ----
    // ジャイロ項 ω×Iω を積むか。**既定 off = 既存シーンとビット同一** (存在ゲートと同じ理屈で、
    // 「入れた = 新しい運動方程式に opt-in した」と読む)。慣性主軸の値が全て等しい形状 (球) では
    // 数学的に効かないので、効くのは箱・カプセル・スケールで歪めた球だけ。
    // ★陽的に積むと必ず発散するので実装は**陰的 (Newton 反復 kGyroIterations 回)**。
    bool gyroscopic = false;
    // 質量中心のローカルオフセット (m、エンティティのローカル空間 = ワールドスケール適用前)。
    // 既定 (0,0,0) = 形状原点が質量中心 (従来どおり)。**非ゼロのときだけ**別経路へ分岐する
    // (値ゲートではなく分岐ゲート — `p + 0.0f` は -0.0f を +0.0f に化けさせるため、
    //  「0 を足しても同じ」に依存してはいけない。決定台帳 1 と同じ理由)。
    // ★慣性テンソルは**この質量中心まわり**として扱う (Unity / PhysX と同じ規約)。形状から
    //   導いた対角慣性を平行軸で移し替えることはしない — 重心をずらす = 質量分布が形状と
    //   違うという宣言なので、移し替えには根拠が無い
    DirectX::XMFLOAT3 centerOfMass = { 0.0f, 0.0f, 0.0f };
    // ---- M59h 追加: スリープ状態 (末尾 append) ----
    // **状態を Rigidbody に置くのが肝** — hash / JSON / SimSnapshot / DLL 移行が
    // フィールド表の既存機構で自動的に被覆される (専用の節を足さずに済む)。
    // ★入眠時に velocity / angularVelocity を**厳密に 0.0f** へ書く: 眠っているあいだ
    //   ワールドハッシュが完全に静止し、残留ビットが構成差を運ぶ余地も消える。
    // ★sleepTicks は閾値でクランプする — 眠っているあいだ増え続けると
    //   「ハッシュが動かない」という上の性質そのものが壊れる
    int32_t sleepTicks = 0;
    bool isSleeping = false;
    // ---- M59j 追加: 連続衝突判定 (CCD、末尾 append) ----
    // true にしたボディだけが、1 サブステップの移動量が自分の外接球半径を超えるときに
    // 掃引されて最初に触れる位置で止まる。**既定 off = 既存シーンとビット同一**
    // (存在ゲートではなくフィールドゲートだが、off のあいだは掃引パスが
    //  ボディ 1 個ぶんの bool 読みで抜けるので fp 演算がゼロという点は同じ)。
    // ★これは「速いときだけ」効く保険であって、通常の接触解決を置き換えるものではない。
    //   止まった直後は速度が落ちて掃引の起動しきい値を外れ、以降は通常の離散ソルバが
    //   摩擦も反発もスタックも面倒を見る (PhysicsSystem.cpp の CCD 節が正本)
    bool ccd = false;
    // ---- M60e 追加: 複合コライダー (末尾 append) ----
    // true にすると、**Rigidbody を持たない子孫の Collider を自分の形状として集約**する
    // (既定 false のあいだ、それらは従来どおり独立した静的コライダーとして収集される —
    //  挙動が変わるので既定 off は必須)。集約先は「最も近い Rigidbody 祖先」で、
    // AeroSurface (M59d) の探索と同じ規約。
    // ★質量中心と慣性は複合形状から導く (体積加重の重心 + 平行軸で合成した **3x3 フル
    //   テンソル**)。M59f1 の「形状から導いた対角慣性を移し替えない」は**単一形状の話** —
    //   複合では質量分布が実際に分かっているので移し替えが正当。意図的な非対称。
    // ★`centerOfMass` を明示指定していればそちらが勝つ (既存規約を壊さない)。
    // ★v1 の制限: 密度からの質量導出は**親自身の形状しか見ない** (複合の質量は
    //   `mass` で与える)。材料・レイヤーも body 単位で、親の Collider が無ければ
    //   最初の子から採る。ジャイロ項は複合では効かない (フルテンソルを要求するため)
    bool compoundColliders = false;
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
    // ---- ワールド追従 UI 拡張 (末尾 append、NoHash = 旧シーン互換・リプレイ不変) ----
    // 追従自体は**エンティティ構成による完全自動判定** (UILayout.h 冒頭): UI 専用でない
    // オブジェクト (メッシュ/コライダー等を持つ) に付いた UIElement はそのオブジェクトの
    // ワールド位置の射影点が基準になる。以下はその追従要素にだけ効くオプション
    bool distanceScale = false; // 距離で縮む (distanceRef の距離で等倍。子 space=1 にも伝播)
    float distanceRef = 5.0f;   // 等倍になるカメラ距離 (m)。<=0 は 1m 扱い
    bool clampToScreen = false; // 画面端クランプ (ON: 背面も方向反転で端に貼る / OFF: 背面は非表示)
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
    // ---- M55d: TAA (末尾 append、NoHash なので hash 不変) ----
    // **Deferred 専用** — 画面速度が GBuffer RT4 にしか無いため。Forward では
    // カメラジッタごと無効化する (TAA 無しでジッタだけ載ると画面が揺れるだけになる)
    int32_t taaOn = 0;        // 0=off 1=on
    float taaFeedback = 0.9f; // 履歴の残し率 [0,0.95]。大きいほど滑らかで残像も増える
    // ---- M56d: SSR (末尾 append、NoHash なので hash 不変) ----
    // **Deferred 専用** — GBuffer と HZB (min-Z ピラミッド) が前提。
    // ssrMaxRoughness 以上の粗さの面には厳密に 0 を足す (= その面は IBL のまま)
    int32_t ssrOn = 0;            // 0=off 1=on
    float ssrMaxRoughness = 0.6f; // これを超える粗さの面は反射しない (RT 反射と同じ既定値)
    float ssrIntensity = 1.0f;    // 1 = IBL スペキュラをちょうど反射で置き換える
    // ---- M57c: フロクセル・ボリュメトリック (末尾 append、NoHash なので hash 不変) ----
    // ★既定 0 = 恒等。1 にすると不透明 / 透明 / 地形 / スカイ / パーティクルの全部に
    //   合成され、同時に **ゴッドレイが自動 off** になる (フォグの三重計上の解消。M57d)
    int32_t froxelOn = 0;          // 0=off 1=on
    float froxelDensity = 0.02f;   // 基準の消散係数 σ_t [1/m] (高度スケール前)
    float froxelAnisotropy = 0.3f; // HG 位相関数の g (>0 = 前方散乱 = 光源側が明るい)
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

// ---- デカール (M56a / M56b、spec §6.6) ----
// **描画専用 (kComponentNoHash)** — 弾痕 / 汚れ / 路面標示のような「既に描かれた面の
// albedo (M56a) と法線 / roughness (M56b) を後から上描きする」投影ボックス。
// sim / ワールドハッシュには 1 バイトも触らない。
//
// 箱の形はエンティティのワールド行列そのもの (単位立方体 [-0.5,0.5]^3 をスケール/回転/移動
// したもの) で、**投影方向はローカル +Z**。ライトの向きと同じ規約 (RenderSystem が
// ワールド行列の第 3 行を使う) に揃えてあるので、「ライトを置くのと同じ感覚で向ける」で通る。
//
// ★**Deferred 専用 (v1)**。Forward には GBuffer が無く、「もう描かれた面の albedo」という
//   上描き先が存在しない。engine_spec.md §6.6 に制限として明記してある。
//
// opt-in (TypeId 末尾 append) なので既存シーンのハッシュは不変 = ReplayFile bump 不要
struct DecalComponent {
    AssetID texture = {}; // 貼る画像 (null = 白 = color がそのまま出る)
    // authored な sRGB 色 (RenderSystem がリニアへ)。**a = 不透明度** —
    // 色と濃さを 1 つの ColorEdit で扱えるようにするための相乗り (a<=0 は描かない)
    DirectX::XMFLOAT4 color = { 1.0f, 1.0f, 1.0f, 1.0f };
    // UV の切り出し (アトラスから 1 枚選ぶ用)。**v1 のサンプラは LINEAR/CLAMP 固定**
    // (デカールの縁で反対側の画素がにじむのを防ぐため) なので、
    // uvScale > 1 は「繰り返す」ではなく「端で伸びる」になる。この 2 本は [0,1] の
    // 内側に収まる部分矩形を選ぶためのもの
    DirectX::XMFLOAT2 uvScale = { 1.0f, 1.0f };
    DirectX::XMFLOAT2 uvOffset = { 0.0f, 0.0f };
    // 受け面の法線が投影方向からどれだけ傾いたら消えるか [度]。既定 90 = cos フェード
    // (正対で 1、直角で 0)。小さくするほど「正面を向いた面にしか乗らない」
    float angleFadeDeg = 90.0f;
    // 同じ画素に複数枚乗るときの順序 (小さいほど先 = 下)。同値は EntityID で決定論に割る
    int32_t sortOrder = 0;
    // ---- M56b: 受け面の法線 / roughness も上描きする (末尾 append) ----
    // ★**強度がそのままハードウェアのブレンド係数になる**設計。既定 0 は
    //   「src*0 + dst*1」= GBuffer を 1 ビットも変えない = 恒等が式として保証される
    //   (シェーダ側で「書かない」分岐を持つのではなく、係数 0 で殺す)。
    // 接線空間の法線マップ。null = 平坦 (0,0,1) = デカール面そのものの向き。
    // ★TBN はデカールの OBB 基底から作る (T = ローカル +X / B = ローカル -Y /
    //   N = -投影方向)。common.hlsli の PerturbNormal は使えない — posW の ddx/ddy が
    //   「投影ボックスの面」のものになって受け面の微分にならないため。
    // 注: Inspector のアセットピッカーの名前推定は小文字 "tex" を探す (大文字 T の
    //     "normalTex" は一致しない) ので、このフィールドは総当たりの一覧に落ちる
    AssetID normalTex = {};
    // 受け面の法線をデカールの法線へどれだけ寄せるか [0,1]。0 = 法線を書かない
    float normalStrength = 0.0f;
    // 上書きする roughness (GBuffer RT3 の g)。metallic / emissive は書込マスクで守る
    float roughness = 0.5f;
    float roughnessStrength = 0.0f; // 0 = roughness を書かない
    static inline ComponentTypeId sTypeId = kInvalidComponentType;
};

// ローカル反射プローブ (M56f)。**kComponentNoHash** — 「この場所から見た景色」を焼いた
// cubemap をスペキュラ環境項へ差し込むだけの描画レーンで、sim には 1 バイトも触らない。
//
// ★**ベイクは明示指示のときだけ** (`View > Rendering` のボタン / `--probe-bake-all`)。
//   置いただけでは 1 命令も走らず絵は 1 ビットも変わらない — 「見えたら焼く」にすると
//   撮影ごとに焼き上がりが変わって決定的撮影 (M52c) が根元から壊れる。
// ★**箱は軸平行** (v1)。エンティティの回転もスケールも見ない — 影響範囲も視差補正で
//   ぶつける内壁も下の extents だけで決まる (engine_spec §6.10 に制限として明記)。
// ★**Deferred 専用 (v1)**。Forward には差し替える環境項の受け皿 (GBuffer) が無い。
struct ReflectionProbeComponent {
    // 影響ボックスの半径 (エンティティのワールド位置が中心)。この箱が「どこに効くか」と
    // 「視差補正でぶつける内壁」の**両方**を兼ねる — 部屋なら部屋の寸法に合わせる
    DirectX::XMFLOAT3 extents = { 8.0f, 4.0f, 8.0f };
    // 箱の内側どれだけで重みが 0 → 1 へ立ち上がるか [world]。0 = 境界でいきなり切り替わる
    float blendDistance = 1.0f;
    float intensity = 1.0f;    // 焼いた放射輝度に掛ける倍率 (1 = そのまま)
    bool boxProjection = true; // false = 無限遠キューブ (視差補正なし)
    // キャプチャの near/far。far がシーンの奥行きに届かないと遠景が抜ける
    float nearZ = 0.1f;
    float farZ = 500.0f;
    static inline ComponentTypeId sTypeId = kInvalidComponentType;
};

// ---- 物理環境 (M59b、TypeId 36) ----
// 重力ベクトル / 空気 / 風 / 水面 を **シーン全体で 1 つ**持つ設定。消費は Skybox / Fog と
// 同じ「**entity.index 最小の active な 1 個**」規約 (RenderSystem::CollectEnvironment 前例)。
//
// ★**存在ゲート**が契約 (M59 決定台帳 1): このコンポーネントが無いシーンは
//   PhysicsSystem の従来式 `vy += kGravity * gravityScale * dt` を 1 文字も変えずに通る
//   分岐へ落ちる。「gravity を (0,-9.81,0) にしておけば置いても挙動不変」は**約束しない** —
//   -0.0f + 0.0f = +0.0f でビットが動くので、値ゲート (係数 0 で中立) は成立しない。
//   **置いた = 新しい数式に opt-in した**と読むこと。
//
// velocity (hash 対象) を駆動する sim 入力なので **hash 対象**。
// opt-in (TypeId 末尾 append) なので既存シーンは 1 バイトも変わらない = ReplayFile bump 不要。
//
// ★CharacterController は M59 では**この env に従わない** (kGravity 直参照のまま) —
//   接地判定が Y 軸前提で組まれており、任意重力ベクトルは CC の意味論ごと壊すため。
//   engine_spec 10.4 に制限として明記してある。
struct PhysicsEnvironmentComponent {
    // 重力加速度 (m/s^2、ワールド)。既定は従来の定数と同じ -9.81 (ただし上記のとおり
    // 「置くだけでビット不変」は約束しない)
    DirectX::XMFLOAT3 gravity = { 0.0f, -9.81f, 0.0f };
    float airDensity = 1.225f; // kg/m^3 (海面 15 degC)。Aero の抗力・マグヌスが使う
    // 一様定常風 (m/s、ワールド)。抗力は **相対速度 v - wind** に効く。
    // 乱流 (tick とセル座標から PCG32 で導出) は M59 のスコープ外 (予約事項 5)
    DirectX::XMFLOAT3 windVelocity = { 0.0f, 0.0f, 0.0f };
    float waterPlaneY = 0.0f;     // 水面の高さ (M59b2 の Buoyancy が使う。それまで未消費)
    float waterDensity = 1000.0f; // kg/m^3 (M59b2)
    // ---- M59g2 追加: サブステップ数 (末尾 append) ----
    // 1 tick を何分割して積分・解決するか。**定数にしないのは車両 (M60) が 8 を要求しがち**
    // だから (予約事項 4)。範囲は [1, 16] にクランプされる。
    // ★**env が無いシーンは 1 固定** — 存在ゲートの一部で、置いていないシーンは
    //   M59g1 までと同じ 1 回積分の経路をそのまま通る。
    // ★サブステップは反復回数を増やすより効く: 同じコストなら接触の解像度が上がり、
    //   反発の頂点保存も貫通も改善する。代償として「1 tick あたりの力」を使う項
    //   (ConstantForce / 空力 / 浮力 / 翼 / ばね) は h = dt / substeps 刻みで効く
    int32_t substeps = 4;
    // ---- M59h 追加: スリープ (末尾 append) ----
    // ★**閾値の置き場が env = 存在ゲートの内側**。env を置いていないシーンは
    //   スリープしない = M59f2 までとビット同一。sleepDelayTicks <= 0 でも無効。
    // ★判定は **int の tick カウンタ**で行う (秒の float 累積は禁止 — 加算順で割れる)。
    float sleepLinearThreshold = 0.05f;  // m/s。これ未満が続いたら候補
    float sleepAngularThreshold = 0.1f;  // rad/s
    int32_t sleepDelayTicks = 60;        // 連続して下回った tick 数がこれに達したら入眠
    static inline ComponentTypeId sTypeId = kInvalidComponentType;
};

// ---- 等方空力 (M59b、TypeId 37) ----
// 「向きを持たない代表面積」1 つで効かせる抗力・角抗力・マグヌス。**無ければ何もしない**
// (opt-in)。面ごとの正しい積分 (揚力・失速・風見安定) は M59c/M59d の面サンプリングが担当し、
// こちらは終端速度とカーブボールを安く出すための等方近似。
//
// 基準面積は **Cauchy の平均投影面積 (= 凸形状の表面積 / 4)**。球で pi*r^2 に一致する
// = 「向きに依らない代表面積」の物理的に正しい唯一の選び方 (PhysicsSystem.h の
// MeanProjectedAreaWorld が正本)。
//
// 抗力は**閉形式 implicit** (`v <- v / (1 + (k|v|/m) dt)`、除算のみ) なので係数をいくら
// 大きくしても発散しない。マグヌスだけは方向を変える陽的な項なので、SpringJoint の前例に
// ならって 1 tick の Delta-v に決定論的な上限クランプを掛けてある。
//
// velocity / angularVelocity (hash 対象) を駆動するので **hash 対象**。
// opt-in (TypeId 末尾 append) なので既存シーンは 1 バイトも変わらない。
// bool の OFF は**項の計算ごとスキップ**する (係数 0 を掛けるのではない = fp 演算が走らない)。
struct AeroComponent {
    bool enableDrag = true;        // 等方二次抗力 F = 0.5 rho Cd A |v_rel| v_rel
    bool enableAngularDrag = true; // 角二次抗力 (無いとマグヌスで回った球が永遠に回り続ける)
    bool enableMagnus = false;     // マグヌス力 F = S (omega x v_rel)。既定 OFF (球技用)
    // Cd の上書き。**<= 0 で「材料の dragCoefficient を使う」**(材料も無ければ球の 0.47)。
    // Cd は本来こちら (形状) 側が正 — 材料値はあくまで既定値
    float dragCoefficient = 0.0f;
    float areaScale = 1.0f; // 基準面積の倍率 (パラシュートを開く等の演出用)
    // 角抗力係数。トルク = 0.5 rho Cda A r^3 |omega| omega (r = 面積から作る等価半径)
    float angularDragCoefficient = 0.1f;
    float magnusCoefficient = 1.0f; // S = magnusCoefficient * 0.5 rho A r の倍率
    // ---- M59c 追加: 面ベース空力 (末尾 append) ----
    // true で抗力の計算を**向きを見る面サンプリング** (AeroSampling.h) に切り替える。
    // enableDrag が OFF なら無関係 (「抗力を出すか」と「どう出すか」の 2 段)。
    // 等方近似と違って**揚力・風見安定が式の別成分として自然に出る**代わりに、
    // 面ごとの陽的な力になるので 1 tick の Delta-v / Delta-omega をクランプしてある
    bool surfaceModel = false;
    float skinFriction = 0.01f; // 面モデルの表面摩擦係数 (風上/風下を問わず接線に効く)
    static inline ComponentTypeId sTypeId = kInvalidComponentType;
};

// ---- 浮力 (M59b2、TypeId 38) ----
// PhysicsEnvironment の waterPlaneY / waterDensity が定める**軸平行な水面**より下に沈んだ
// 排除体積ぶんの上向き力と、水中の抗力。**無ければ何もしない** (opt-in)。
// env が無いシーンでも既定の水 (y=0 / 1000 kg/m^3) で働く (Aero と同じ扱い)。
//
// 没水体積は球だけ球冠の解析式 (多項式のみ)、box / capsule は保守 AABB の高さ比近似。
//
// ★**v1 に復原モーメントは無い** — 没水重心の水平ずれを近似が拾わないため、傾いて浮かぶ
//   箱は傾いたまま (角度は水中角抗力で減衰するだけ) になる。船が自力で起き上がる挙動は
//   面ごとの圧力積分が要る = M59c の面サンプリングカーネルの仕事。
//   engine_spec 10.4 に制限として明記してある。
// ★水面は軸平行なので、重力ベクトルを傾けても**浮力は常に +Y** (大きさだけ |g| に従う)。
//
// velocity / angularVelocity (hash 対象) を駆動するので **hash 対象**。
// opt-in (TypeId 末尾 append) なので既存シーンは 1 バイトも変わらない。
struct BuoyancyComponent {
    // 排除体積の倍率 (中空の船体・積荷の近似)。**<= 0 で無効** (項ごとスキップ)
    float volumeScale = 1.0f;
    // 水中の線形抗力 [1/s]。没水割合で按分され、閉形式 implicit なので大きくても発散しない。
    // 浮力は陽的な復元力なので、これを 0 にすると軽い物体がいつまでも上下に揺れる
    float linearDrag = 2.0f;
    float angularDrag = 2.0f; // 水中の角抗力 [1/s] (同じ按分・同じ implicit)
    static inline ComponentTypeId sTypeId = kInvalidComponentType;
};

// ---- 翼面 (M59d、TypeId 39) ----
// 「1 枚の翼」= 揚力係数曲線を持つ面。**子エンティティに複数置く流儀** (Part と同じ) で、
// 力とトルクは**最も近い Rigidbody 祖先** (自分が持っていれば自分) へ入る。
//
// ★子に置くことが本質。M59c で確かめたとおり、対称な形状に一様流を当てても幾何中心
//   まわりのトルクは原理的に厳密 0 で、風見安定は「**圧力中心が質量中心からずれている**」
//   ことからしか出てこない。翼を子エンティティに置く = そのずれを LocalTransform で
//   authoring する、というのがこのコンポーネントの設計そのもの。
//
// 等方空力 (Aero) との違い: あちらは物体全体の抗力、こちらは 1 枚の面の揚力/抗力。
// 併用してよい (紙飛行機 = 胴体に Aero、主翼と尾翼に AeroSurface が素直な組み方)。
//
// velocity / angularVelocity (hash 対象) を駆動するので **hash 対象**。
// opt-in (TypeId 末尾 append) なので既存シーンは 1 バイトも変わらない。
struct AeroSurfaceComponent {
    // 翼面のローカル法線 (**正の迎角でこちら側へ揚力が出る**)。非単位でもよい (内部で正規化)
    DirectX::XMFLOAT3 normal = { 0.0f, 1.0f, 0.0f };
    float area = 0.5f;            // 翼面積 [m^2]。<= 0 で無効
    // 揚力傾斜 dCL/d(sin alpha)。薄翼理論の 2*pi が既定。
    // ★迎角そのものではなく **sin(alpha)** に対する傾斜として持つ — 小迎角では同値で、
    //   逆三角関数を一度も呼ばずに済む (決定論の観点で安い)
    float liftSlope = 6.2831853f;
    float stallAngleDeg = 15.0f;   // これを超えると平板挙動へ落ちる (失速)
    float dragCoefficient = 0.02f; // 有害抗力 CD0
    float inducedDrag = 0.05f;     // 誘導抗力 k (CD += k * CL^2)
    float stalledDrag = 1.2f;      // 90 度で流れに立ちはだかったときの CD
    static inline ComponentTypeId sTypeId = kInvalidComponentType;
};

// ---- 関節 (M60a) ----
// 2 つの剛体 (または剛体とワールドの不動点) を拘束する。**無ければ物理は何も足さない**
// (opt-in → 既存シーンのハッシュ/リプレイ不変)。
//
// ★内部表現は「1 自由度の拘束行 (Jacobian 1 行 + 蓄積λ) の集合」で、**`type` は
//   どの行を立てるかのプリセットにすぎない** (M60 決定台帳 1)。型ごとに別コンポーネントを
//   切らないのは、差が Inspector 上の見た目にしかならないから。将来の完全 D6 (軸ごとに
//   free/limited/locked) も同じ箱に入る。
// ★**関節は「子側」= 動かしたいほうのエンティティに付ける**。ECS のアーキタイプ SoA に
//   従い 1 エンティティ 1 関節 (決定台帳 2)。複数要るなら中間エンティティを挟む —
//   ラグドールの骨も車輪も自然に片側 1 個なので、実用上これで足りる。
// ★フィールドは M60a の時点で c/d の分まで**先に**切ってある。後から足すとシーンの
//   バイト列が動くため (M59a1 で確立した「スキーマを先に切る」流儀)。
// broken は sim 状態 = **hash 対象** (ソルバが書き、snapshot と JSON が運ぶ)。
struct JointComponent {
    EntityID connectedEntity = kNullEntity; // null = ワールドの不動アンカーへ繋ぐ
    // 0=Ball 1=Hinge 2=Fixed 3=Slider 4=Cone。**M60a が行を立てるのは Ball だけ**
    int32_t type = 0;
    // アンカー点。owner 側は**このエンティティのローカル**、相手側は**相手のローカル**。
    // ★相手が null のときだけ connectedAnchor は**ワールド座標**として読む
    //   (自動算出はしない — エディタ時ロジックが sim の契約の外に増えるため)
    DirectX::XMFLOAT3 anchor = { 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 connectedAnchor = { 0.0f, 0.0f, 0.0f };
    // 軸 (owner ローカル)。Hinge = 回転軸 / Slider = 滑る方向 / Cone = 円錐の中心軸。
    // Ball / Fixed は使わない。非単位でもよい (内部で正規化)
    DirectX::XMFLOAT3 axis = { 0.0f, 1.0f, 0.0f };
    // ---- リミット (M60c) ----
    // ★**関節の値は「owner が connectedEntity に対して軸方向にどれだけ動いたか」**。
    //   ドアを +30 度開いたら関節角も +30 度。相手が null (ワールド) なら owner 自身の
    //   軸まわり回転そのもの。restRotation が rest = 0 の位置を決める
    // ★useLimit=false なら**行を一切立てない** (Cone なら swing も twist も効かない = Ball)。
    //   limitMin > limitMax の逆転も「満たしようが無い」ので行を立てない (自由になる)
    // ★リミットは**片側不等式**なので、範囲外に出たときだけ行が立つ。行き過ぎは
    //   「行が立つ前に 1 サブステップ進むぶん」= 速度×h で、substeps を増やすと減る
    bool useLimit = false;
    float limitMin = -45.0f;     // Hinge: 角度[度] / Slider: 変位[m] / Cone: twist 角[度]
    float limitMax = 45.0f;      // 同 上限
    float swingLimitDeg = 45.0f; // Cone のみ: 軸の円錐半頂角[度]。useLimit で一緒に効く
    // ---- モータ (M60c)。**maxForce <= 0 なら行を立てない** (値ゲートではなく分岐ゲート) ----
    // 目標速度の符号もリミットと同じ規約 (+ なら owner が +軸まわり / +軸方向へ動く)。
    // maxForce は Hinge では**トルク** [N·m]、Slider では力 [N]。リミットとモータを
    // 両方付けた場合は**リミットが勝つ** (各反復でリミットを後に解いている)
    float motorTargetVelocity = 0.0f; // Hinge: rad/s / Slider: m/s
    float motorMaxForce = 0.0f;
    // ---- 破断 (M60d)。<= 0 = 無限 (壊れない) ----
    // ★判定量は「**その tick に関節が受け止めた力積 ÷ dt**」= 平均反力 [N] / [N·m]。
    //   tick 全体で溜めてから割るので **substeps を変えても閾値の意味が動かない**。
    // ★数えるのは等式行とリミット行 (= 反力) だけで、**モータ行は数えない** —
    //   駆動であって反力ではないため (数えるとモータを強くしただけで自分の関節が折れる)。
    // ★Ball は角ブロックを立てないので breakTorque が働く余地は無い。
    // ★判定は tick の末尾。折れた関節はその tick ぶんは働き、行が消えるのは次 tick から
    float breakForce = 0.0f;
    float breakTorque = 0.0f;
    // true のあいだ行を一切立てない (= 自由になる)。**コンポーネントを外すのではなく
    // フラグで落とす** — 構造変更をソルバ内から起こさないのが家風 (決定台帳 5)。
    // 復帰は Inspector / スクリプトが false へ戻すだけ
    bool broken = false;
    // ---- M60b 追加 (末尾 append) ----
    // 「相対姿勢がこれのとき拘束が満たされている」という**基準の相対回転** =
    // conj(qOwner) * qConnected (相手が null ならワールドなので conj(qOwner) そのもの)。
    // Hinge / Fixed / Slider / Cone が角度自由度を持つには基準が要る — 速度だけを
    // 拘束すると軸のずれが**積分誤差として累積する**ので、位置補正に基準が必要になる。
    // ★既定は単位 = 「両者のローカル軸が揃っているのが rest」。自動採取 (Unity の
    //   auto-configure 相当) はアンカーと同じ理由で入れない。両者が無回転で置かれている
    //   ふつうの authoring では既定のまま正しい。
    // ★M60a では切っていなかったフィールド。a の「c/d の分まで先に切る」は
    //   **リミット/モータ/破断しか見ていなかった** (申し送り M60b-1)
    DirectX::XMFLOAT4 restRotation = { 0.0f, 0.0f, 0.0f, 1.0f };
    static inline ComponentTypeId sTypeId = kInvalidComponentType;
};

// ---- ラグドール (M60g1) ----
// **SkinnedMesh を持つエンティティ側に付く小さな札**。骨そのものは
// `Part`(joint 名) + `Collider` + `Rigidbody` + `Joint` を持つ**直子エンティティ**で、
// これは M48g のボーン追従部位とまったく同じ形 — ラグドールは新しい骨の表現を
// 持ち込むのではなく、**既にある部位の駆動方向を反転させるだけ**の機能。
//
// ★`active` が「どちらが骨を駆動するか」の唯一のスイッチ:
//   - false (既定) = **アニメ → 骨 → 部位**。`PartFollowSystem` が部位の LocalTransform を
//     書き、物理は部位を kinematic として扱う (収集時に分岐 = Rigidbody は書き換えない)
//   - true          = **物理 → 部位 → 骨**。`PartFollowSystem` は該当部位を skip し、
//     描画側が部位の LocalTransform からボーンパレットを組み直す
// ★切替の連続性がタダで手に入るのがこの設計の要点 — false→true の瞬間、部位の
//   LocalTransform には**アニメが置いた骨姿勢そのもの**が入っているので、剛体の初期姿勢が
//   飛ばない。ブレンド (物理とアニメの混ぜ合わせ) は v1 では約束しない。
// ★**hash 対象** (sim 状態)。active が物理の収集を分岐させるので、snapshot と .rep が
//   これを運ばないと巻き戻しで駆動方向が食い違う。
// ★骨のポーズ自体は描画専用のまま (SkinnedMesh は kComponentNoHash)。ハッシュに載るのは
//   部位の LocalTransform = 従来から載っていたものだけで、決定論の面積は広がらない
struct RagdollComponent {
    bool active = false;
    static inline ComponentTypeId sTypeId = kInvalidComponentType;
};

// ---- 車輪 + レイキャストサスペンション (M60h1、TypeId 42) ----
// **車輪エンティティ (車体の子) に付ける**。力の入れ先は AeroSurface (M59d) と同じ
// 「最も近い Rigidbody 祖先」で、車輪自身は剛体でなくてよい。
//
// ★車輪を剛体にしてヒンジで吊るほうが物理的には素直だが、実用の車両が例外なく
//   レイキャストサスを採るのは安定性がまるで違うため — タイヤの接地を 1 本のレイで
//   代表すると、接触の解像度 (法線の跳び・マニフォールドの点数) に車体の挙動が
//   引きずられなくなる。
// ★レイは **PhysicsSystem が `bodies[].pose` へ直接撃つ**。`RaycastWorld` は
//   `WorldMatrixComponent` 基準で、`TransformSystem` は物理の**後**に走る以上
//   ソルバ実行中は 1 tick 古い (M59j の CCD が同じ罠を明記している)。
// ★レイの向きは**車輪エンティティのローカル -Y** = 「車輪から見た下」。車体が傾けば
//   サスも一緒に傾くし、キャンバー/キャスターを LocalTransform で authoring できる。
// ★サスは**押すだけで引かない**。伸び側でダンパが勝った瞬間に負の力を出すと、
//   接地したまま車体を地面へ吸い付けてしまう (レイキャストサスの定番の壊れ方)。
// ★v1 の制限: 反作用を**当たった相手へ返さない** (片側のみ)。動く板の上を走らせても
//   板は沈まない。返すには相手の島・起床の配線も要るので h2 以降の判断に回す。
//
// velocity / angularVelocity を駆動し、出力フィールドも sim 状態なので **hash 対象**。
// opt-in (TypeId 末尾 append) なので既存シーンは 1 バイトも変わらない。
struct WheelComponent {
    // サスの静止長 [m] = 無負荷のときの「取り付け点 → 車輪中心」の距離。
    // レイの長さは restLength + radius (= 完全に伸びきった車輪の接地点まで)
    float restLength = 0.4f;
    // ばね定数 [N/m]。静止時の沈み込みは解析的に **mg/k** (h1 の試験がこれを固定する)。
    // ★硬さはサブステップで買える (M59g2-7) — 車両が substeps 8 を要求しがちなのはこれ
    float stiffness = 20000.0f;
    float damping = 2000.0f; // ダンパ [N・s/m]。縮み側・伸び側とも同じ係数 (v1)
    float radius = 0.35f;    // 車輪半径 [m]
    // 圧縮の上限 [m] (**<= 0 で無制限**)。底付きの表現で、これを超える沈み込みはばねに
    // 乗らない。バンプストップのような「底付きで急に硬くなる」挙動は入れていない —
    // それ以上沈むのを止めるのは車体自身のコライダーの仕事
    float maxCompression = 0.3f;
    // ---- 出力 (kFieldReadOnly。ソルバが毎 tick 書く = **sim 状態**) ----
    // 範型は CharacterController の moveInput(入力) / isGrounded(出力) の書き分け
    int32_t isGrounded = 0;   // レイが車輪の接地面まで届いたか
    float compression = 0.0f; // 今の圧縮量 [m] (maxCompression でクランプ済み)
    // ---- M60h2 追加: タイヤ (末尾 append) ----
    // ★**タイヤの摩擦が効くのは車体に `Vehicle` が付いているときだけ**。Wheel 単体は
    //   「サス = 縦の支え」しか持たない (= M60h1 とビット同一の経路)。分岐ゲートを
    //   コンポーネントの有無に置いたので、h1 のシーンは 1 ビットも動かない。
    float steerFactor = 0.0f;   // ステア入力への追従率 (前輪 1 / 後輪 0 / 逆位相なら -1)
    float driveFactor = 0.0f;   // 駆動力の配分 (FF なら前 1 / 後 0)
    float brakeFactor = 1.0f;   // 制動力の配分 (サイドブレーキを作るなら後輪だけ 1)
    // 横力の傾き [N/rad]。**スリップ角に比例し μN で頭打ち**という v1 のモデル
    // (Pacejka の魔法の公式は入れない — 係数の意味が測定値と結びつかないと調整できない)。
    // 乗用車のタイヤ 1 本で 3 万 N/rad 前後が実測の目安
    float corneringStiffness = 30000.0f;
    // タイヤの μ。接地面の材料と **sqrt(積)** で結合する (接触ソルバと同じ結合則)。
    // ゴム-乾燥路面 ~1.0 が目安 (PhysMatLibrary.h の換算表)
    float friction = 1.0f;
    // 転がり抵抗。接地面の材料と **max** で結合する (これも接触ソルバと同じ —
    // sqrt(積) だと相手が 0 の瞬間に消える)。タイヤ-舗装 0.02 が目安
    float rollingResistance = 0.02f;
    // ---- 出力 (kFieldReadOnly。ソルバが毎 tick 書く = sim 状態) ----
    // ★**見た目 (LocalTransform) は書かない**。書くとハッシュ対象が増えるので、
    //   描画側がこの 2 つを読んで車輪メッシュを回す (ラグドールのパレットと同じ
    //   「決定論の面積を広げない」判断)。回すのは M60i の描画配線。
    float steerAngle = 0.0f;    // 今の切れ角 [rad] (steer * steerFactor * maxSteerAngleDeg)
    float rotationAngle = 0.0f; // 転がりの回転角 [rad]。[-pi, pi] に折り返す
    static inline ComponentTypeId sTypeId = kInvalidComponentType;
};

// ---- 車両 (M60h2、TypeId 43) ----
// **車輪が力を入れる剛体と同じエンティティ (= 車体) に付ける**。役割は 2 つだけ:
// ①運転入力の置き場 ②「この剛体の車輪はタイヤの摩擦を持つ」という宣言。
//
// ★**入力を ABI ではなく sim 状態フィールドに置く**のが設計の核心。スクリプトは既存の
//   `SetComponentField` で書けるので、**車両のために ABI スロットを 1 本も足していない**
//   (前例は `CharacterControllerComponent.moveInput`)。おまけに入力が sim 状態なので
//   snapshot / .rep / タイムトラベルが**何もしなくても**運転操作を運ぶ。
// ★Vehicle が無ければ車輪はサス (縦の支え) しか出さない = M60h1 とビット同一。
//   タイヤの摩擦をコンポーネントの有無で分岐ゲートしてあるので、既存シーンも
//   「Wheel だけ付けたシーン」も 1 ビットも動かない。
// ★v1 に**駆動系のモデルは無い** — エンジン回転もギアもデフも持たず、throttle に比例した
//   力を駆動輪へ直接入れる。トルク曲線を入れるなら「回転角を sim 状態に持っている」
//   ここから素直に伸ばせる (v1 の回転角は転がりから逆算した見た目用)。
//
// velocity / angularVelocity を駆動するので **hash 対象**。opt-in (TypeId 末尾 append)。
struct VehicleComponent {
    // ---- 運転入力 (スクリプトが毎 tick 書く。**hash 対象 = sim 状態**) ----
    float steer = 0.0f;    // -1..1 (+ で右へ切る)。範囲外はクランプされる
    float throttle = 0.0f; // -1..1 (- で後退)。範囲外はクランプされる
    float brake = 0.0f;    // 0..1
    // ---- 諸元 ----
    float maxSteerAngleDeg = 30.0f; // steer=1 のときの切れ角
    float motorForce = 4000.0f;     // 駆動輪 1 本あたりの最大駆動力 [N]
    float brakeForce = 6000.0f;     // 制動輪 1 本あたりの最大制動力 [N]
    static inline ComponentTypeId sTypeId = kInvalidComponentType;
};

class World;

// エンティティが有効か (ActiveComponent が無ければ有効 / enabled==0 なら無効)
bool IsEntityActive(World& world, EntityID e);

// 固定順で登録する (TypeId の決定論)。多重呼び出しは無害
void RegisterBuiltinComponents();

} // namespace mye
