#pragma once
// エンジン → GameLogic.dll に渡す C 関数テーブル (engine_spec.md 8.4)。
// DLL 境界規則:
//   - extern "C" スタイルの関数ポインタのみ。C++ vtable / STL / 例外は越えない
//   - メモリの確保・解放は常にエンジン側 (この API 経由)。DLL 側 CRT ヒープに依存しない
//   - 文字列 (const char*) は呼び出しの間だけ有効。保持するならコピーすること

#include <stdint.h>

#include "Shared/MathPod.h"

// 互換性チェック用。テーブルや ScriptDesc のレイアウトを変えたら必ず上げること
// ★既存スロットの並びとシグネチャは変えない — Interop.cs が位置ベースでミラーしているため、
//   既存スロットをいじると C# 側が全てズレる。ミラー照合は tools\check_rules.ps1 規則 11
//   (順序・件数・名前・引数個数 + version⇄スロット数の同時性) が機械検査する。
// 版ごとの中身 (注意は各スロットの注記。足した経緯は docs\history\api-scripting-tools.md):
// v3 (M19): gamepad / Raycast / PlaySound / StopSound / LoadScene
// v4 (M28a): 剛体操作 (AddForce/AddImpulse/AddTorque/Get/SetVelocity) + Overlap*/SphereCast + OnCollision
// v5 (M29b): キャラクターコントローラ (CharacterMove/Jump/IsGrounded/GetVelocity) + SetTextMeshText
// v6 (M32f): エフェクト制御 (EmitterBurst/SetEmitterPlaying/RestartEffect/PlayEffect)
// v7 (M37): Instantiate / FindByFileId / AnimatorParam / 動的 UI + UIFocusNav / DebugDrawLine / マスク付きクエリ
// v8 (M45): オーディオ操作一式 (PlaySound2 以降)
// v9 (M48h): 部位 (ソケット) クエリ FindPart / FindPartsByTag
// v10 (M49): 部位ボリュームへのレイキャスト RaycastParts
// v11 (M50d): 汎用フィールドアクセス GetComponentField / SetComponentField
// v12 (M51h): GetMouseWheel / UI 矩形・レイアウト・テクスチャ・ヒットテスト / アクションマップ / TimeControl / Persist / Save・Load / SetPadVibration
// v13 (M52i): ネット対戦の状態参照 5 本 + 入力アクションのレーン指定版 2 本
// v14 (M59k): 付け外し 2 本 / AddForceAtPosition / GetContactInfo / SampleWind / SampleTerrainHeight / スリープ 2 本
// v15 (M64a): マウスルック GetMouseDelta / SetCursorMode
// v16 (M70c): UI の対話 UIButtonState / UIGetFocused / UISetFocused / MouseCanvasPos / GetUIRect + LoadPersist
// v17 (M71a): GetSceneName
// v18: IsDevelopmentRun (デバッグ機能を配布物で閉じる)
// v19: SetWindowMode / GetWindowMode (ウィンドウ / ボーダーレスの切り替え)
// v20: 汎用タグ TagIndex / HasTag / SetTag / FindEntitiesWithTag
// v21 (M78e): Compute ABI — CreateComputeBuffer / ReleaseComputeBuffer / SetComputeBuffer /
//             SetComputeFloat / SetComputeFloat4 / SetComputeTextureFromAsset / DispatchCompute
// v22 (M80l): 破壊 (M80) — ApplyFractureDamage スロット + MyeScriptDesc 末尾の onBreak イベント
// v23 (M81f): 専用サーバのレーン状態・playerId・参加/離脱イベント (NetLaneMask 以降の 5 本)
// v24 (M82i): NavMesh — NavSetDestination / NavStop / NavGetAgentState / NavFindPath / NavSamplePosition /
//             NavRaycast / NavFindRandomPoint / NavCompleteLink (NetGetSystemEvent の次の 8 本)
// v25 (M83b): AI の知覚 — PerceptionReportNoise / PerceptionReportDamage / PerceptionGetCount / PerceptionGet /
//             PerceptionCanSee (NavCompleteLink の次の 5 本)
// v26 (M84d2): NavMesh の続き — NavWarp / NavCalculatePath / NavSetPath と、.navfilter.json を渡せるクエリ 4 本
//             (NavFindPathFiltered / NavSamplePositionFiltered / NavRaycastFiltered / NavFindRandomPointFiltered。
//             既存スロットの引数は変えない規則なので別スロット)。PerceptionCanSee の次の 7 本
#define MYE_API_VERSION 26u

// PersistSet の 1 エントリ最大バイト数 (v12)。PersistStore は WorldHash / セーブ出力に
// 全量が載るため、無制限だと 1 キーでハッシュとセーブが肥大する
#define MYE_PERSIST_MAX_BLOB 65536

// v21 (M78e) Compute バッファの usageFlags (CreateComputeBuffer の第 3 引数)
#define MYE_COMPUTE_BUFFER_STRUCTURED 0x01u // 構造化バッファ (最低フラグ)
#define MYE_COMPUTE_BUFFER_UAV        0x02u // UAV ビューも作成する (書き込み可)

// MYE_LOG レベル (Engine/Core/Diagnostics/Log.h の LogLevel と同値)
enum MyeLogLevel {
    MYE_LOG_LEVEL_TRACE = 0,
    MYE_LOG_LEVEL_INFO = 1,
    MYE_LOG_LEVEL_WARN = 2,
    MYE_LOG_LEVEL_ERROR = 3,
};

// gamepad ボタンマスク (XINPUT_GAMEPAD_* と同値。DLL 側は <Xinput.h> 非依存)
enum MyePadButton {
    MYE_PAD_DPAD_UP = 0x0001,
    MYE_PAD_DPAD_DOWN = 0x0002,
    MYE_PAD_DPAD_LEFT = 0x0004,
    MYE_PAD_DPAD_RIGHT = 0x0008,
    MYE_PAD_START = 0x0010,
    MYE_PAD_BACK = 0x0020,
    MYE_PAD_LTHUMB = 0x0040,
    MYE_PAD_RTHUMB = 0x0080,
    MYE_PAD_LB = 0x0100,
    MYE_PAD_RB = 0x0200,
    MYE_PAD_A = 0x1000,
    MYE_PAD_B = 0x2000,
    MYE_PAD_X = 0x4000,
    MYE_PAD_Y = 0x8000,
};

// ウィンドウの表示モード (v19 SetWindowMode / GetWindowMode。Win32Window.h の WindowMode と同値)
enum MyeWindowMode {
    MYE_WINDOW_MODE_WINDOWED = 0,   // 枠付き・サイズ変更可
    MYE_WINDOW_MODE_BORDERLESS = 1, // 枠なしでモニタ全面
};

// Raycast のヒット結果 (M19 で予約、M20 の物理で実装)
struct MyeRaycastHit {
    MyeEntityId entity;
    MyeVec3 point;
    MyeVec3 normal;
    float distance;
};

// ソリッド接触 1 ペアの詳細 (v14、M59k)。GetContactInfo の出力。
// エンジン内部の SolidContact (M59e で拡張) を「自分から見た形」に直したもの
// UI 矩形 (v16 GetUIRect)。**単位はキャンバス座標** (基準 1920x1080、M70b) で、
// UIElementComponent の x/y/w/h と同じ土俵。左上原点
struct MyeUIRect {
    float x, y, w, h;
};

struct MyeContactInfo {
    MyeEntityId other;  // 相手のエンティティ
    MyeVec3 point;      // 代表接触点 (ワールド) = マニフォールド最大 4 点の重心
    MyeVec3 normal;     // **相手→自分**方向 (OnCollisionEnter に届く法線と同じ向き)
    // その tick にこのペアへ入った法線インパルスの合計 [N*s]。静止して載っているだけの
    // 接触では m*g*dt になる (= 「どれだけの重さが載っているか」)。衝突の瞬間は
    // 運動量変化そのものなので、着地音の音量や破壊の閾値にそのまま使える
    float impulse;
};

// v23 (M81f) NetGetSystemEvent の出力。SystemEvent (Engine/Session/SessionTypes.h) の
// 参加・離脱を、この tick に確定入力として適用された形で渡す
struct MyeNetSystemEvent {
    uint64_t eventSeq; // サーバ発行の単調増加 (1 始まり)
    uint64_t playerId; // レーンとは別。再接続しても変わらない
    uint32_t kind;     // 1 Join / 2 Leave / 3 Rejoin / 4 Release
    uint32_t lane;     // 適用結果のレーン
};

// v24 (M82i) NavGetAgentState の出力。NavMeshAgent の実行状態 (status は navagentstatus と同値:
// 0 Idle / 1 Moving / 2 Arrived / 3 NoPath / 4 OnLink / 5 Inactive / 6 Stuck)
struct MyeNavAgentState {
    int32_t status;
    float remainingDistance; // 経路に沿った残りの距離の見積り [m]
    int32_t pathPartial;     // 1 = 目的地まで届かず、届く限りの最寄りへ向かっている
    MyeVec3 velocity;        // CharacterController.velocity (CC が無ければ 0)
};

// v25 (M83b) PerceptionGet の出力。AIPerception の知覚している相手 1 件 (AIPercept と同じ並び、64 バイト)。
// senses のビット: 1 視覚 / 2 聴覚 / 4 ダメージ / 8 接触
struct MyePercept {
    MyeEntityId target;      // 相手。名乗らない音は無効なハンドル (index = 0xFFFFFFFF)
    uint64_t lastSensedTick; // 最後に知覚した tick
    uint32_t currentSenses;  // この tick に知覚した感覚 (0 = 記憶だけ)
    uint32_t lastSenses;     // 最後に知覚した tick の感覚
    MyeVec3 lastSensedPos;   // 最後に知覚した位置
    float strength;          // 最後に知覚した強さ
    MyeVec3 velocity;        // 視覚で続けて見た位置から求めた速度
    MyeVec3 predictedPos;    // 見失った後の予測位置 (見えている間は lastSensedPos)
};

// v26 (M84d2) NavCalculatePath の出力 / NavSetPath の入力 (Unity の NavMeshPath)。4112 バイト。
// polys は Detour のポリゴン参照で中身は不透明 (NavSetPath に渡すためだけ)。corners は角 (先頭は Agent の位置)
#define MYE_NAV_PATH_MAX_POLYS 256
#define MYE_NAV_PATH_MAX_CORNERS 256
struct MyeNavPath {
    int32_t status;      // 0 = 無効 / 1 = 目的地まで届く / 2 = 部分経路 (届く限りの最寄りまで)
    int32_t agentTypeId; // 経路を引いた Agent の種別 (違う種別の Agent には SetPath できない)
    int32_t polyCount;
    int32_t cornerCount;
    MyeVec3 end;         // 終点 (部分経路なら届く限りの最寄り)
    uint32_t polys[MYE_NAV_PATH_MAX_POLYS];
    MyeVec3 corners[MYE_NAV_PATH_MAX_CORNERS];
};

// v24 (M82i) NavRaycast の出力
struct MyeNavRaycastHit {
    int32_t hit;     // 1 = ナビメッシュの縁 (壁・歩けないエリア) で止まった / 0 = 終点まで歩けた (point = to)
    MyeVec3 point;   // 止まった点 (hit = 0 なら to)
    MyeVec3 normal;  // 壁の法線 (水平。hit = 0 なら 0)
    float distance;  // from を吸着した点から point までの距離 [m]
};

struct MyeEngineApi {
    uint32_t version; // MYE_API_VERSION
    void* engine;     // 不透明 (ScriptHost)。全関数の第 1 引数に渡す

    // ---- ログ ----
    void (*Log)(void* engine, int level, const char* message);

    // ---- 入力 (現在 tick のスナップショット — 決定論) ----
    int (*KeyDown)(void* engine, uint8_t vk);
    int (*MouseButton)(void* engine, int button); // 0:L 1:R 2:M
    void (*MousePos)(void* engine, int32_t* x, int32_t* y);

    // ---- エンティティ ----
    MyeEntityId (*CreateGameObject)(void* engine, const char* name);
    void (*DestroyGameObject)(void* engine, MyeEntityId id); // tick 末に適用
    int (*IsAlive)(void* engine, MyeEntityId id);
    MyeEntityId (*FindByName)(void* engine, const char* name);
    void (*SetParent)(void* engine, MyeEntityId child, MyeEntityId parent);

    // ---- Transform (戻り値 0 = 失敗) ----
    int (*GetLocalPosition)(void* engine, MyeEntityId id, MyeVec3* out);
    int (*SetLocalPosition)(void* engine, MyeEntityId id, MyeVec3 v);
    int (*GetLocalRotation)(void* engine, MyeEntityId id, MyeQuat* out);
    int (*SetLocalRotation)(void* engine, MyeEntityId id, MyeQuat q);
    int (*GetLocalScale)(void* engine, MyeEntityId id, MyeVec3* out);
    int (*SetLocalScale)(void* engine, MyeEntityId id, MyeVec3 v);

    // ---- 乱数 (エンジン管理の決定論ストリーム。spec 11.2 規則 8) ----
    float (*RandomFloat01)(void* engine);
    float (*RandomRange)(void* engine, float lo, float hi);

    // ---- コンポーネント操作 (v2) ----
    // 登録名でコンポーネントを追加 (例: "Collider")。成功で 1
    int (*AddComponentByName)(void* engine, MyeEntityId id, const char* componentName);
    // MeshRenderer を付与してメッシュ/マテリアルをアセットキー名で設定
    // (例: "builtin://cube", "mat_yellow")。実体の解決はエンジン側
    int (*SetMeshRenderer)(void* engine, MyeEntityId id, const char* meshKey,
                           const char* materialKey);

    // ---- gamepad (v3、パッド 0、現在 tick — 決定論。verify では記録値で透過) ----
    int (*PadConnected)(void* engine);
    int (*PadButton)(void* engine, uint16_t buttonMask);            // MYE_PAD_* の論理和
    void (*PadSticks)(void* engine, MyeVec2* left, MyeVec2* right); // 各成分 -1..1
    void (*PadTriggers)(void* engine, float* left, float* right);   // 0..1

    // ---- 物理 (v3 で予約、M20 で実装)。ヒットで 1、outHit に最近ヒットを書く ----
    int (*Raycast)(void* engine, MyeVec3 origin, MyeVec3 dir, float maxDist, MyeRaycastHit* outHit);

    // ---- オーディオ (v3 で予約、M19.3 で実装)。soundKey = .wav アセットキー ----
    // 戻り値: voice ハンドル (>0)、0=失敗。再生イベントは tick 内で決定論順に積まれ、
    // ハッシュ後にエンジンが XAudio2 へ流す (voice 状態は hashed state に絶対戻さない)
    int (*PlaySound)(void* engine, const char* soundKey, float volume);
    // v8: **非推奨** (v3 時代の疑似ハンドルは tick を跨ぐと衝突するため常に no-op だった)。
    //     停止は下の StopVoice (uint64 ハンドル) を使うこと。互換のためスロットは残す
    void (*StopSound)(void* engine, int voice);

    // ---- シーン遷移 (v3 で予約、M19.4 で実装)。tick 末に遅延ロードされる ----
    void (*LoadScene)(void* engine, const char* scenePath);

    // ---- 剛体操作 (v4、M28a)。Rigidbody 非所持は 0 を返す ----
    // AddForce は 1 tick 分の加速 (dv = F/m · fixedDt) を呼出時に即時適用する。
    // 蓄積フィールドは持たない (ステートレス) — Update 内で毎 tick 呼べば連続力になる。
    // AddImpulse は dv = J/m を即時適用。kinematic には 0 を返す (SetVelocity は許可)。
    int (*AddForce)(void* engine, MyeEntityId id, MyeVec3 force);
    int (*AddImpulse)(void* engine, MyeEntityId id, MyeVec3 impulse);
    int (*AddTorque)(void* engine, MyeEntityId id, MyeVec3 torque); // v4 予約、M28b で実装
    int (*GetVelocity)(void* engine, MyeEntityId id, MyeVec3* out);
    int (*SetVelocity)(void* engine, MyeEntityId id, MyeVec3 v);

    // ---- 空間クエリ (v4 で予約、M28c で実装)。トリガー含む全コライダー対象 ----
    // ★**Start() からは使えない** (dogfooding #3)。Start はスクリプト層 = フェーズ 3 で、
    //   TransformSystem はその後のフェーズ 4 (TickRunner.cpp) なので、シーンを読み込んだ
    //   直後の最初の tick では WorldMatrix がまだ確定していない = 何にも当たらない。
    //   エディタは Play 前に毎フレーム描画が走っていて行列が埋まっているため気づけず、
    //   **Runtime.exe (描画前に tick が回る) でだけ壊れる**という一番たちの悪い形になる。
    //   置き場所は Update にして「2 tick 目以降」を自分で待つこと。Raycast* / Overlap* /
    //   SphereCast* / RaycastParts と、位置を読む糖衣 (MyeGameObject::GetWorldPosition)
    //   のすべてに同じことが言える。
    // Overlap 系: ヒットしたエンティティを outEntities に最大 maxCount 個 (index 昇順) 書き、
    // 戻り値は「切り捨て前の総ヒット数」。バッファは呼び出し側が確保する (DLL 境界規則)。
    int (*OverlapSphere)(void* engine, MyeVec3 center, float radius, MyeEntityId* outEntities,
                         int maxCount);
    int (*OverlapBox)(void* engine, MyeVec3 center, MyeVec3 halfExtents, MyeQuat rotation,
                      MyeEntityId* outEntities, int maxCount);
    // 半径 radius の球を dir 方向に掃引し最近ヒットを返す (Raycast の太い版)
    int (*SphereCast)(void* engine, MyeVec3 origin, MyeVec3 dir, float radius, float maxDist,
                      MyeRaycastHit* outHit);

    // ---- キャラクターコントローラ (v5、M29b)。CC 非所持は 0 を返す ----
    // CharacterMove: 水平移動速度 (m/s) を設定する。値は保持される (毎 tick 設定推奨)。y は無視
    int (*CharacterMove)(void* engine, MyeEntityId id, MyeVec3 move);
    // CharacterJump: ★**接地判定は呼び出し側の責任** (dogfooding #6)。名前は「跳ぶ」だが
    //   実体は「跳躍要求を積む」だけで、接地可否に関わらずその tick で消費される —
    //   素で呼ぶと空中で何度でも跳べる。CharacterIsGrounded を確かめてから呼ぶこと
    //   (PlayerController / WalkerDemo がその形)。
    //   効果は「次の物理 tick で接地していれば vy=speed」
    int (*CharacterJump)(void* engine, MyeEntityId id, float speed);
    int (*CharacterIsGrounded)(void* engine, MyeEntityId id);           // 1=前 tick 接地
    int (*CharacterGetVelocity)(void* engine, MyeEntityId id, MyeVec3* out); // 実効速度

    // ---- UI テキスト (v5 で予約、M29c の TextMesh で実装)。描画専用 = 非 hash で sim 安全 ----
    int (*SetTextMeshText)(void* engine, MyeEntityId id, const char* text);

    // ---- エフェクト制御 (v6、M32f)。全て hash 対象フィールドへの決定論的書込 or tick 末 spawn ----
    // EmitterBurst: ParticleEmitter を持つ id に count 個の即時バーストを積む (次の粒子 Update で放出)。
    //               成功で 1、非所持は 0。
    int (*EmitterBurst)(void* engine, MyeEntityId id, int count);
    // SetEmitterPlaying: ParticleEmitter の連続放出を on(1)/off(0) する。成功で 1。
    int (*SetEmitterPlaying)(void* engine, MyeEntityId id, int playing);
    // RestartEffect: EffectComponent の経過を 0 に戻し子エミッタ + Animator を再開する。成功で 1。
    int (*RestartEffect)(void* engine, MyeEntityId id);
    // PlayEffect: prefabKey (.prefab.json、assets 相対。省略サフィックス可) を pos に生成する
    //             (fire-and-forget)。生成は tick 末の構造変更フェーズ (spawn キュー)。
    //             parent 有効ならその子 (pos はローカル)。sim 状態なので record/verify で再現される。
    void (*PlayEffect)(void* engine, const char* prefabKey, MyeVec3 pos, MyeEntityId parent);

    // ---- 汎用スポーン (v7、M37) ----
    // Instantiate: PlayEffect と同じ tick 末 spawn だが、呼出時にルートの fileId を予約して
    //              即返す。生成は tick 末なので EntityID は次 tick 以降に FindByFileId で解決する
    //              (呼出順は決定論 → 予約列も record/verify で一致)。失敗 (キュー未接続) は 0。
    uint64_t (*Instantiate)(void* engine, const char* prefabKey, MyeVec3 pos, MyeEntityId parent);
    // fileId → EntityID (未存在は null id)。Instantiate の遅延解決用
    MyeEntityId (*FindByFileId)(void* engine, uint64_t fileId);

    // ---- Animator Controller パラメータ (v7)。hash 対象への決定論的書込 (M22 の回収) ----
    int (*SetAnimatorParam)(void* engine, MyeEntityId id, int index, int value); // index 0..3
    int (*GetAnimatorParam)(void* engine, MyeEntityId id, int index, int* out);

    // ---- 動的 UI (v7)。UIElement は NoHash の描画状態 → 毎 tick 書いても sim 安全 ----
    int (*SetUIText)(void* engine, MyeEntityId id, const char* utf8);
    int (*SetUIFill)(void* engine, MyeEntityId id, float amount);   // fillAmount 0..1
    int (*SetUIColor)(void* engine, MyeEntityId id, MyeColor color);
    int (*SetUIFocused)(void* engine, MyeEntityId id, int focused); // フォーカス枠の表示
    // フォーカスナビ: 全 active focusable UIElement を**キャンバス座標**で解決し
    // dir (0=上 1=下 2=左 3=右) の最近傍を返す (無ければ current)。
    // ★M70b: キャンバス寸法は入力レーン 0 に記録された値 (アスペクト比だけの関数で、
    //   16:9 なら常に 1920x1080)。ライブのウィンドウ実寸は読まないので決定論のまま
    //   (フォーカス状態はスクリプト側が保持する)
    MyeEntityId (*UIFocusNav)(void* engine, MyeEntityId current, int dir);

    // ---- デバッグ描画 (v7)。描画専用 (非 hash) — 今 tick の線は次の描画フレームに出る ----
    void (*DebugDrawLine)(void* engine, MyeVec3 a, MyeVec3 b, MyeColor color);

    // ---- マスク付き空間クエリ (v7、M36a のレイヤー対応)。mask のビット = 対象レイヤー ----
    int (*RaycastMasked)(void* engine, MyeVec3 origin, MyeVec3 dir, float maxDist, uint32_t mask,
                         MyeRaycastHit* outHit);
    int (*OverlapSphereMasked)(void* engine, MyeVec3 center, float radius, uint32_t mask,
                               MyeEntityId* outEntities, int maxCount);
    int (*SphereCastMasked)(void* engine, MyeVec3 origin, MyeVec3 dir, float radius, float maxDist,
                            uint32_t mask, MyeRaycastHit* outHit);

    // ---- オーディオ (v8 で予約、M45g で実装) ----
    //
    // ★このブロックは意図的に **write-only** である。再生位置・再生中判定・バス音量・
    //   メーター値などの **読み取り API は今後も追加しない**。オーディオは実時間・
    //   デバイス依存の出力レーンなので、sim がそこから 1 bit でも読んだ瞬間に
    //   リプレイ (spec 11.3) が壊れる。
    //   一見安全に見える「tick 差分から算出した再生経過」も禁止 — hashed state が
    //   アセットの実バイト長に依存し、.wav を 1 サンプル差し替えただけで golden が壊れる。
    //
    // ハンドルは呼出時に予約される単調増加値 (v7 Instantiate の fileId 予約と同型)。
    // 採番はキューへの push 側で行われ記録/検証でもゲートされないので、同じ入力なら
    // 常に同じ値が返る。0 = 失敗 (キュー未接続)。
    uint64_t (*PlaySound2)(void* engine, const char* soundKey, float volume, float pitch);
    uint64_t (*PlaySoundAt)(void* engine, const char* soundKey, MyeVec3 worldPos, float volume);
    void (*StopVoice)(void* engine, uint64_t handle, float fadeSeconds);
    void (*SetVoiceVolume)(void* engine, uint64_t handle, float volume);
    void (*SetVoicePitch)(void* engine, uint64_t handle, float pitch);
    // AudioSource コンポーネント (M45e) を持つエンティティの再生/停止。非所持は 0
    int (*PlayAudioSource)(void* engine, MyeEntityId id);
    int (*StopAudioSource)(void* engine, MyeEntityId id, float fadeSeconds);
    // busName = "Master" / "BGM" / "SE" / "UI" (M45d 以降はミキサーアセットのバス名)
    void (*SetBusVolume)(void* engine, const char* busName, float volume);
    // BGM (M45f)。fadeSeconds > 0 で現行 BGM とクロスフェードする
    void (*PlayMusic)(void* engine, const char* soundKey, float fadeSeconds, int loop);
    void (*StopMusic)(void* engine, float fadeSeconds);
    // 3D リスナーを指定エンティティに固定する (M45e)。null id = 自動 (プライマリカメラ)
    void (*SetListenerEntity)(void* engine, MyeEntityId id);

    // ---- 部位 (ソケット) クエリ (v9、M48h) ----
    //
    // 「アセット側が決めた取り付け位置に、ランタイムが物を付ける」ための入口。
    // 取り付けそのものは既存 SetParent、位置取得は既存 Transform getter で足りるので、
    // ここに増やすのは **引く手段** の 2 本だけ (ScriptAPI.h に糖衣あり)。
    //
    // ★どちらも読むのはシーンデータ (階層 / 名前 / PartComponent) だけ = 決定論。
    //   スクリプト実行順は固定なので記録/検証で同じ答えが返る (オーディオのような
    //   実時間レーンからは 1 bit も読んでいない)。
    //
    // FindPart: root から '/' 区切りの名前パスで降下する ("Hips/LegL")。各セグメントは
    //   直子の名前と完全一致。空セグメント (先頭/末尾/連続の '/') は読み飛ばし、
    //   パスが空なら root 自身。途中が PartComponent を持つ必要はない。
    //   見つからなければ null id (MyeEntityIdIsNull で判定)
    MyeEntityId (*FindPart)(void* engine, MyeEntityId root, const char* utf8Path);
    // FindPartsByTag: root サブツリーの PartComponent から tag 一致を DFS 順 (root 先頭) に集める。
    //   out へ最大 cap 個書き、戻り値は **切り捨て前の総ヒット数** (Overlap* と同じ規約 —
    //   バッファが足りたかは戻り値 <= cap で判定する)。out=null / cap=0 で件数だけ数えられる。
    //   バッファは呼び出し側が確保する (DLL 境界規則)。
    //   tag は部位タグ名の FNV-1a 64bit (ScriptAPI.h の MyePartTag / C# の Engine.PartTag)。
    //   ★入れ子プレハブの境界では止めない (フラット走査) — 「ボス配下の全弱点」を
    //     1 回で引ける方を優先した設計判断
    int32_t (*FindPartsByTag)(void* engine, MyeEntityId root, uint64_t tag, MyeEntityId* out,
                              int32_t cap);
    // RaycastParts (v10、M49): 部位ボリューム (Part + PartBounds 両持ちのエンティティ) への
    //   レイキャスト。root null = シーン全体、tag 0 = 全部位。dir は**正規化済み**であること。
    //   ヒットで 1 を返し outHit に最近ヒット (entity = 部位)。同距離は低 index が勝つ (決定論)。
    //   読むのはシーンデータ (WorldMatrix / Part / PartBounds) だけ — WorldMatrix は
    //   前 tick の TransformSystem の結果 (既存の Raycast と同条件)
    int32_t (*RaycastParts)(void* engine, MyeEntityId root, uint64_t tag, MyeVec3 origin,
                            MyeVec3 dir, float maxDist, MyeRaycastHit* outHit);

    // ---- 汎用フィールドアクセス (v11、M50d) ----
    //
    // 登録フィールド (Reflection.h の FieldDesc) を名前ハッシュで読み書きする。
    // compNameHash / fieldNameHash は FNV-1a 64bit (ScriptAPI.h の MyeNameHash /
    // C# は生成定数)。ポインタは越境しない — 常に値コピー (DLL 境界規則)。
    //
    // ★**Get と Set は非対称** (M70d)。kComponentNoHash のコンポーネント
    //   (C# スクリプト状態 / UIElement / Fog / CameraPostFx … = 非決定論レーン) は
    //     - Get: 0 を返して**恒久的に閉じる**。そこから 1 bit でも sim へ読み込むと
    //       リプレイ (spec 11.3) が壊れる。C# レーンは record/verify 中に走らないので、
    //       C# が書いた値を sim が読み返した瞬間に「録画と再生で違う世界」になる。
    //     - Set: **通す**。書き込みは決定論レーン (C++ スクリプト) の副作用で、値は
    //       ハッシュに載らず、NoHash コンポーネントも World のカラムとして SimSnapshot に
    //       入る (Replay/SimSnapshot.h) ので巻き戻しでも復元される。
    //   これで Fog / Decal / TrailRenderer / SpriteRenderer / Skybox / Terrain /
    //   ReflectionProbe / SkinnedMesh / CameraPostFx (露出・ブルーム・DOF …) が
    //   **新スロット 0 本で**実行時に操作できる。
    //
    // GetComponentField: 成功でフィールドの実バイト数を返し buf へ値コピー、outType
    //   (null 可) へ MyeFieldType を書く。未知の comp/field・死んだエンティティ・
    //   bufSize 不足は 0。文字列型 (String64/256) は固定長全体をコピーする
    int32_t (*GetComponentField)(void* engine, MyeEntityId e, uint64_t compNameHash,
                                 uint64_t fieldNameHash, void* buf, int32_t bufSize,
                                 int32_t* outType);
    // SetComponentField: 成功で 1。size はフィールドの実バイト数と一致が必須
    //   (文字列型のみ size <= 固定長を許し、残りはエンジンがゼロ埋めする —
    //   String64 は終端以降もハッシュ対象なので尾部の残骸をここで断つ)
    int32_t (*SetComponentField)(void* engine, MyeEntityId e, uint64_t compNameHash,
                                 uint64_t fieldNameHash, const void* buf, int32_t size);

    // ---- v12 (M51h): 入力アクション / UI 拡張 / ゲームフロー / パッド振動 ----

    // このフレームに累積したマウスホイール生値 (WHEEL_DELTA=120 単位)。
    // InputSnapshot 由来 = 記録/検証で記録値が返る (決定論)
    int32_t (*GetMouseWheel)(void* engine);

    // ---- UI 矩形/レイアウト書込 (M51e の回収)。UIElement は NoHash の描画状態。
    //      書いた値そのものを読み返す口は無く、読めるのは v16 の GetUIRect (解決済みの
    //      矩形) だけ。★C# レーンは record/verify 中走らないため、C# から UI 幾何を
    //      書くとリプレイが壊れる (C# 側には開けていない)。成功で 1、UIElement 非所持は 0 ----
    // SetUIRect: anchor 基準オフセット (x,y) とサイズ (w,h) を設定。w/h < 0 は現値維持
    //            (書いた値を読み返さずに「位置だけ動かす」ための keep 意味論)。
    //            ★M75a: 書き先は RectTransform (anchoredPosition / sizeDelta)。署名は不変
    int (*SetUIRect)(void* engine, MyeEntityId id, float x, float y, float w, float h);
    // SetUILayout: anchor/align は 9-grid (0..8)、space/clipChildren/wrap は 0/1。
    //              負値はいずれも現値維持 (SetUIRect と同じ keep 意味論)。
    //              ★M75a: anchor → RectTransform の一致アンカー、space → basis (1=親 / 0=キャンバス)
    int (*SetUILayout)(void* engine, MyeEntityId id, int32_t anchor, int32_t space,
                       int32_t clipChildren, int32_t align, int32_t wrap);
    // SetUITexture: 登録テクスチャキー名 (SetMeshRenderer と同じ規約)。null/空 = 単色に戻す
    int (*SetUITexture)(void* engine, MyeEntityId id, const char* textureKey);
    // UIHitTest: **キャンバス座標** (UIFocusNav と同じ) で点 (x,y) を含む最前面の
    //   active UIElement を返す (order 最大、同値は entity.index 最大 = 描画で上のもの)。
    //   祖先クリップで見えない部分には当たらない。無ヒットは null id。
    //   ★M70b: 描画もキャンバス座標で解くので「見えている場所 = 押せる場所」が構造的に
    //     一致する。キャンバス寸法は入力レーン 0 に記録された値なので決定論。
    //   ★MousePos は**クライアント実 px**を返すので、そのまま渡すと解像度に応じてズレる。
    //     キャンバス座標のマウスは v16 の MouseCanvasPos で取る
    MyeEntityId (*UIHitTest)(void* engine, float x, float y);

    // ---- 入力アクションマップ (M51d の回収)。assets\input\actions.json で定義し、
    //      エンジンが tick 頭 (verify の入力置換後) に評価済み。名前は FNV-1a 64bit
    //      (MyeNameHash / C# は Engine 側でハッシュ)。未定義名は 0 ----
    uint32_t (*GetActionState)(void* engine, uint64_t nameHash); // bit0=held 1=pressed 2=released
    float (*GetAxisValue)(void* engine, uint64_t nameHash);      // [-1, +1]

    // ---- ゲームフロー (M51g の回収、決定台帳 5) ----
    // TimeControl は WorldHash 対象の sim 状態。スクリプト実行順は決定論なので
    // 直接書いてよい (SetLocalPosition と同格)。スクリプト層自体は非ゲート —
    // ポーズ中もスクリプトは走り続けるので、ここからいつでも解除できる。
    // scalePercent は 0..100 (100 = 等速)。範囲外はクランプ。ポーズ/スケールが
    // 止めるのはアニメ/物理/衝突/パーティクルで、入力・UI・スクリプトは動き続ける
    void (*SetTimeControl)(void* engine, int paused, int scalePercent);
    void (*GetTimeControl)(void* engine, int* outPaused, int* outScalePercent);
    // PersistStore (シーン跨ぎ永続 KV)。key は名前の FNV-1a 64bit。値は生バイト列の
    // 値コピー (DLL 境界規則)。WorldHash 対象 = sim 状態なので record/verify で再現される。
    // PersistSet: 成功で 1 (size < 0 / MYE_PERSIST_MAX_BLOB 超 / data null で size > 0 は 0)。
    //             size 0 は「空 blob あり」として保存される (不在と区別される)
    int (*PersistSet)(void* engine, uint64_t key, const void* data, int32_t size);
    // PersistGet: 不在は -1。存在すれば実バイト数を返し、buf へ min(実サイズ, cap) を書く
    int32_t (*PersistGet)(void* engine, uint64_t key, void* buf, int32_t cap);
    // セーブ/ロード要求 (tick 末に消費、同 tick 内は後勝ち)。slot < 0 は無視。
    // SaveGame は出力レーン (ハッシュ後の書出 = 決定論を汚さない)。
    // LoadGame は record/verify 中 no-op + WARN (「リプレイはセーブ読込を跨がない」)
    void (*SaveGame)(void* engine, int slot);
    void (*LoadGame)(void* engine, int slot);

    // ---- パッド振動 (出力レーン、XInput パッド 0)。値は 0..1 (範囲外クランプ)。
    //      record/verify 中とフォーカス喪失中は 0 に落とされ、終了時も 0 リセット。
    //      オーディオと同じ write-only — 振動状態の読み取りは存在しない ----
    void (*SetPadVibration)(void* engine, float left, float right);

    // ---- v13 (M52i): ネット対戦の状態参照 ----
    // ★**表示用**。返すのは機種依存の値 (自分がどちら側か / ping / 巻き戻し回数) なので、
    //   sim 状態へ書くとリプレイもネットも壊れる (2 台のワールドハッシュが割れ、
    //   desync 検出が desync バンドルを出して止まる)。
    //   セッションが張られていないときは 0 / 1 レーンの既定値を返す
    int (*NetIsConnected)(void* engine);      // 入力交換中なら 1
    uint32_t (*NetLocalPlayer)(void* engine); // 自分が動かすレーン (非ネットは 0)
    uint32_t (*NetPlayerCount)(void* engine); // セッションのレーン数 (非ネットは 1)
    float (*NetPingMs)(void* engine);         // ピギーバック RTT の移動平均 (ms)
    // これまでに巻き戻した回数。0 のまま増えないなら予測が当たり続けている
    uint64_t (*NetRollbackCount)(void* engine);

    // ---- v13 (M52i): 入力アクションのレーン指定版 ----
    // v12 の GetActionState / GetAxisValue と同じ評価結果の player レーンを引く。
    // player >= kMaxPlayers や未接続レーンは 0 (レーン 0 へフォールバックしない —
    // 黙って別プレイヤーの入力で動くのが一番たちの悪い壊れ方)
    uint32_t (*GetActionForPlayer)(void* engine, uint64_t nameHash, uint32_t player);
    float (*GetAxisForPlayer)(void* engine, uint64_t nameHash, uint32_t player);

    // ---- v14 (M59k): コンポーネントの付け外し ----
    // M59 の機能は全て「コンポーネントを付けたら効く」存在ゲートなので、ランタイムの
    // ON/OFF はここが入口になる (Aero を付けて空気抵抗を出す / 外して真空に戻す 等)。
    //
    // ★構造変更なのでアーキタイプ移動が起きる。**毎 tick の付け外しは非推奨** —
    //   常用する ON/OFF は「付けたまま bool フィールドを SetComponentField で倒す」ほうが
    //   桁違いに安い (決定台帳 10)。
    // ★**スクリプトから呼ぶ Add / Remove はどちらも tick 末に適用される** (ADR-005 の
    //   コマンドバッファ — スクリプト層はアーキタイプのイテレーション中に走るため)。
    //   したがって HasComponentByName が答えるのは常に「この tick の頭の状態」で、
    //   付けた直後は 0、外した直後は 1 が返る。**同じ tick 内で結果を見に行かないこと** —
    //   見えるようになるのは次の tick から。同 tick で初期値を書きたいときは
    //   AddComponentByName ではなく、次 tick の Update で SetComponentField を使う
    // RemoveComponentByName: 登録名で外す。名前が解決でき、かつ今その型を持っていたら 1。
    //   基本 4 コンポーネント (Name/LocalTransform/WorldMatrix/Hierarchy) は Transform /
    //   階層の前提が壊れるので**構造的に外せない** — 0 が返る
    int (*RemoveComponentByName)(void* engine, MyeEntityId id, const char* componentName);
    int (*HasComponentByName)(void* engine, MyeEntityId id, const char* componentName);

    // ---- v14 (M59k): 作用点付きの力 ----
    // AddForce と同じ「1 tick 分の力」規約 (dv = F/m · fixedDt)。違いは worldPoint で、
    // 質量中心 (Rigidbody.centerOfMass) からのオフセット r に対して Δω = I⁻¹(r×F)dt が
    // 同時に入る = 端を押せば回る。質量・慣性はソルバと同じ関数で解決するので、
    // AddForce + AddTorque を自分で合成するより「質量が二義にならない」点で安全。
    // Rigidbody 非所持 / kinematic は 0。freezeRotation のときは並進成分だけが入る
    int (*AddForceAtPosition)(void* engine, MyeEntityId id, MyeVec3 force, MyeVec3 worldPoint);

    // ---- v14 (M59k): 接触の詳細 ----
    // self と other が**今 tick 接触しているか**を引き、代表接触点・法線・法線インパルス
    // 合計を out に書く。接触していなければ 0 (out は触らない)。
    //
    // ★呼べる場所が決まっている: **OnCollisionEnter / Stay と LateUpdate だけ**。
    //   Update (フェーズ 3) は物理より前なので常に 0 が返る。これは決定論の要請で、
    //   接触列は毎 tick 使い回すバッファ = SimSnapshot 非被覆なので、前 tick の列を
    //   読ませるとタイムトラベル / ネットのロールバック後の再シムで割れる。
    // ★ソリッド接触だけ (トリガーは対象外)。両方不動のペアはソルバを通らないので出ない
    int (*GetContactInfo)(void* engine, MyeEntityId self, MyeEntityId other, MyeContactInfo* out);

    // ---- v14 (M59k): 環境と地形のサンプリング ----
    // SampleWind: その点の風速 (m/s、ワールド) を out に書く。PhysicsEnvironment が
    //   置かれていれば 1、無ければ 0 を書いて 0 を返す。
    //   ★point は **M59 では読まれない** (一様定常風のみ)。それでも引数に取ってあるのは、
    //     乱流 (tick とセル座標から PCG32 で導出する予定) を足すときに ABI を
    //     もう一度 bump せずに済ませるため
    int (*SampleWind)(void* engine, MyeVec3 point, MyeVec3* outWind);
    // SampleTerrainHeight: ワールド XZ の地形表面の高さ (ワールド Y) と面法線を返す。
    //   ヒットで 1、地形コライダー (Collider.shape = terrain) の範囲外は 0。
    //   ★返すのは**当たる地面**であって描画の地面ではない — 物理が実際に衝突判定に
    //     使っている三角形をそのまま引くので、LOD やスカートの影響を受けない。
    //   複数の地形が重なっていたら最も高いヒット (同値は entity.index が小さい側)。
    //   outHeight / outNormal は null 可
    int (*SampleTerrainHeight)(void* engine, float x, float z, float* outHeight,
                               MyeVec3* outNormal);

    // ---- v14 (M59k): スリープ (M59h) ----
    // WakeRigidbody: 眠っているボディを起こす (velocity は触らない)。Rigidbody 非所持は 0。
    //   ★力・速度を触る既存スロット (AddForce/AddImpulse/AddTorque/SetVelocity/
    //     AddForceAtPosition) は**自動で起こす**ので、通常これを呼ぶ必要は無い。
    //     要るのは「近くで何かが起きたから念のため起こす」ような外部要因のとき
    int (*WakeRigidbody)(void* engine, MyeEntityId id);
    // IsSleeping: 眠っていれば 1。Rigidbody 非所持も 0 (「眠っていない」に寄せる)
    int (*IsSleeping)(void* engine, MyeEntityId id);

    // ---- v15 (M64a): マウスルック ----
    // GetMouseDelta: この tick に積まれた**生マウスデルタ** (Raw Input のカウント)。
    //   InputSnapshot 由来なので GetMouseWheel と同じく record/verify では記録値が返る
    //   = 決定論レーン。outDx / outDy は null 可。
    //   ★MousePos の差分ではない。カーソルロック中は絶対座標が動かない (矩形に貼り付く)
    //     ので、差分方式では一人称の視点がロックした瞬間に止まる。
    //   ★**単位は生のマウスカウントで、DPI は機種依存**。感度をコードに直書きすると
    //     マウスを替えただけで別のゲームになる — 必ず調整値 (スキーマコンポーネント等)
    //     を通して割ること。上下は「下向きが正」(画面座標と同じ向き)
    void (*GetMouseDelta)(void* engine, int32_t* outDx, int32_t* outDy);

    // SetCursorMode: 0 = 通常 / 1 = ロック (クライアント矩形の中央へ毎フレーム固定して非表示)。
    //   エディタでは矩形が Game ビューの画像になり、Stop 後 / Pause 中は掴まない (2026-09-14)。
    //   **出力レーン** — SetPadVibration と同格で、要求を書くだけ。実際の
    //   ClipCursor/SetCursorPos/ShowCursor はフレーム末にエンジンが適用し、record/verify 中・
    //   フォーカス喪失中・タイムトラベルのスクラブ中は強制的に解除される。
    //   現在のモードを読み返す口は無い (状態は呼び出し側が持つ)。
    //
    // ★**Escape でエンジンがロックを手放す**。エディタで Play 中にロックしたまま
    //   Stop ボタンを押せなくなるのを防ぐための最後の逃げ道で、解除は出力レーン
    //   だけの判断なので sim には一切見えない (ハッシュは 1 bit も動かない)。
    //   一度手放したら、**ゲーム側が mode 0 を出し直すまで再ロックしない** —
    //   毎 tick 1 を書き続ける実装が Escape を握り潰すのを構造的に防ぐため。
    //   つまり作法は「Escape (= Pause) を見たら 0、再開の意思表示で 1」。
    void (*SetCursorMode)(void* engine, int mode);

    // ---- v16 (M70c): UI の対話をエンジンが持つ ----
    // 矩形の解決・押下判定・フォーカスはエンジンが 1 本で持つ。スクリプトに矩形を
    // 手書きさせると、UIElement 側のレイアウト変更で黙って食い違うため。
    // 状態は Scene が持つ sim 状態で **WorldHash 対象** = 配線が壊れれば replay が赤くなる。

    // UIButtonState: 要素の対話状態をビットで返す。
    //   bit0 hovered / bit1 pressed / bit2 clicked / bit3 focused (MyeUIButton* と同値)。
    //   UIElement 非所持・null id は 0。
    //   ★clicked は **1 tick だけ**立つ (「掴んだ要素の上で離した」瞬間、Unity 意味論)。
    //     フォーカス中の要素で UINavSubmit を押した tick も clicked が立つ = パッドと
    //     マウスでゲーム側の分岐を分けなくてよい。
    //   ★評価は**スクリプト層より前**なので、Update から読めるのは今 tick の値
    uint32_t (*UIButtonState)(void* engine, MyeEntityId id);

    // UIGetFocused / UISetFocused: エンジンが持つフォーカスの読み書き。
    //   UISetFocused に null id を渡すとフォーカスを外す。focusable でない要素や
    //   UIElement 非所持を渡した場合も 0 を返して**何も変えない**。
    //   ★UIElement.focused は毎 tick これのミラーとして書き直される表示専用の値。
    //     直接書いても次の tick で戻るので、フォーカスを動かすときは必ずこちらを使う
    //     (旧 SetUIFocused は互換のためこのスロットへ委譲する)
    MyeEntityId (*UIGetFocused)(void* engine);
    int (*UISetFocused)(void* engine, MyeEntityId id);

    // MouseCanvasPos: **キャンバス座標**のマウス位置 (M70b の基準 1920x1080 系)。
    //   UIHitTest / GetUIRect と同じ土俵なので、こちらを渡せば解像度に依らず当たる。
    //   MousePos (クライアント実 px) との違いはそこだけ。out は null 可
    void (*MouseCanvasPos)(void* engine, float* outX, float* outY);

    // GetUIRect: 解決済みのキャンバス矩形 (アンカー・親子 space・距離スケール適用後)。
    //   **UI 幾何の唯一の読み取り口**で、描画・ヒットテスト・フォーカスナビが使うのと
    //   同じ uilayout::ResolveRect を通る。UIElement 非所持は 0 (out は触らない)。
    //   祖先クリップは適用しない (クリップ後の可視矩形ではなく素の矩形を返す)
    int (*GetUIRect)(void* engine, MyeEntityId id, MyeUIRect* out);

    // LoadPersist: セーブスロットから **PersistStore だけ**を読む (シーンは動かさない)。
    //   LoadGame は保存時のシーンへ必ず遷移するので、「タイトル画面でハイスコアだけ読む」
    //   にはこちらを使う (dogfooding #16)。読めたら 1。
    //   ★LoadGame と同じく record/verify/netplay 中は no-op + WARN — セーブファイルは
    //     sim の外にあり、再生を跨ぐと同じ入力から別の世界が出てしまう
    int (*LoadPersist)(void* engine, int slot);

    // ---- v17 (M71a): 現在のシーンの識別 ----
    // GetSceneName: 今ロードされているシーンの sceneName (シーン JSON の "sceneName")。
    //   戻り値は NUL を除く実バイト数で、**cap が足りなくても実長を返す** (PersistGet と
    //   同じ規約 = 呼び側が「切れた」ことを判定できる)。buf へは min(実長, cap-1) バイト
    //   + NUL を書く。buf == null または cap <= 0 なら何も書かずに長さだけ返す。
    //   ★**パスではなく名前**を返すのが要点。SourcePath() は assets ルート込みの絶対パス
    //     なので、これを sim の分岐に使うとチェックアウト先でワールドハッシュが割れる。
    //   ★名前は sim 状態 (SimSnapshot v15 で往復する) なので、記録/検証・タイムトラベル・
    //     ロールバックのいずれでも同じ tick で同じ値が返る = 登録フィールドへ書き戻してよい
    int32_t (*GetSceneName)(void* engine, char* buf, int32_t cap);

    // ---- v18: 開発中の実行か ----
    // IsDevelopmentRun: エディタ / --project 付きの Runtime なら 1、配布物 (--project 無しの Runtime) なら 0。
    //   ゲームがデバッグ操作 (照明の切り替え・検証用の自動操作など) を配布物で閉じるための口。
    //   ★ビルド構成のマクロで分けないのは、GameLogic.dll がエディタと配布物で同じ 1 本で、
    //     構成マクロでのロジック分岐は決定論の規則 1 で禁止されているため
    //   ★値はプロセスの起動方法で決まる定数で、**sim 状態ではない** (.rep にも SimSnapshot にも載らない)。
    //     記録と検証は同じ起動方法で走らせること — 配布物で記録した .rep (crash.rep など) を --project 付きで
    //     検証すると、配布物では無視されたデバッグ入力が効いてその tick で割れる
    int32_t (*IsDevelopmentRun)(void* engine);

    // ---- v19: ウィンドウの表示モード ----
    // SetWindowMode: MYE_WINDOW_MODE_WINDOWED (0) / MYE_WINDOW_MODE_BORDERLESS (1)。それ以外の値は無視。
    //   **出力レーン** — SetCursorMode と同じく要求を書くだけで、実際の切り替えはフレーム末にエンジンが行い、
    //   変わったら <saveDir>\display.json へ書く (次の起動は最初からそのモードで開く)。
    //   ★窓が動くのは Runtime の通常起動だけ。エディタ・record/verify・--frames / --screenshot・ネット対戦・
    //     プローブ実行では要求を覚えるだけ (Game ビューはパネル / 人の座っていない実行で画面を奪わない)
    void (*SetWindowMode)(void* engine, int32_t mode);
    // GetWindowMode: 今の要求値。起動直後は実際のモード (Runtime なら display.json か
    //   project_settings.json の window.defaultMode、窓が動かない実行ではウィンドウ)。
    //   ★**起動方法と前回の選択で決まる値で、sim 状態ではない** — 設定画面の表示にだけ使い、
    //     登録フィールドへ書き戻さない (書くと前回の選択が違うだけで記録と検証のワールドハッシュが割れる)
    int32_t (*GetWindowMode)(void* engine);

    // ---- v20: 汎用タグ (TagComponent) ----
    // タグは番号 0..63 のビット集合で、名前はプロジェクト設定 (project_settings.json の "tags")。
    // 判定の規則は Engine/Engine/Scene/Tags.h の 1 本きり (エディタ・描画と同じ関数を見る)。
    // TagIndex: 名前 → 番号。空文字列 / 未登録は -1。大文字小文字は区別する。
    //   ★名前の表はプロジェクトの資産なので、同じ資産で走る記録と検証では同じ番号が返る。
    //     毎フレーム引かずに Start で番号へ解決して持つのが安い
    int32_t (*TagIndex)(void* engine, const char* name);
    // HasTag: id が**自分で**そのタグを持つなら 1 (祖先のタグは見ない — Unity の Tag と同じ)。
    //   無効な id / 範囲外の番号は 0。★SetTag で付けた直後に Tag コンポーネントが新しく
    //   足された場合は AddComponentByName と同じく tick 末に適用される = 同じ tick では 0
    int (*HasTag)(void* engine, MyeEntityId id, int32_t tagIndex);
    // SetTag: on != 0 で付ける / 0 で外す。Tag コンポーネントが無ければ付けるときだけ足す
    //   (外すときに足しはしない)。成功 (id が有効かつ番号が範囲内) で 1
    int (*SetTag)(void* engine, MyeEntityId id, int32_t tagIndex, int on);
    // FindEntitiesWithTag: そのタグを自分で持つ生存エンティティを **EntityID の index 昇順** で out へ。
    //   戻り値は切り捨て前の総数 (FindPartsByTag と同じ規約。out=null / cap<=0 は数えるだけ)
    int32_t (*FindEntitiesWithTag)(void* engine, int32_t tagIndex, MyeEntityId* out, int32_t cap);

    // ---- v21 (M78e): Compute ABI ----
    // C ABI 経由で GPU コンピュートバッファを確保・Dispatch する。
    // 生 D3D 型は Shared に出さない。メモリ解放はエンジン側 (ReleaseComputeBuffer / Shutdown)。
    // ★C# レーンは record/verify 中に走らないため、Compute 結果を ECS/WorldHash に書き戻す
    //   用法は禁止 (spec §4.1)。視覚効果・スクリプト内の一時利用に留めること。

    // CreateComputeBuffer: 構造化バッファを確保して不透明ハンドルを返す。
    //   count * stride バイトの GPU バッファ。flags: MYE_COMPUTE_BUFFER_* の論理和。
    //   失敗 (デバイス未接続 / 上限超過) は 0
    uint64_t (*CreateComputeBuffer)(void* engine, uint32_t count, uint32_t stride, uint32_t flags);

    // ReleaseComputeBuffer: バッファを解放する。無効 ID・二重解放は no-op で落ちない
    void (*ReleaseComputeBuffer)(void* engine, uint64_t bufferId);

    // SetComputeBuffer: shaderUtf8 の CS に bufferNameUtf8 の名前で bufferId をバインド予約する。
    //   無効なバッファ ID は 0 戻り。シェーダの読込・バインドは DispatchCompute まで遅延
    int (*SetComputeBuffer)(void* engine, const char* shaderUtf8,
                            const char* bufferNameUtf8, uint64_t bufferId);

    // SetComputeFloat / SetComputeFloat4: Properties／cbuffer の変数に値を設定する。
    //   シェーダを読んで名前を照合し、未知の変数名は 0 (落ちない)
    int (*SetComputeFloat)(void* engine, const char* shaderUtf8,
                           const char* propNameUtf8, float value);
    int (*SetComputeFloat4)(void* engine, const char* shaderUtf8,
                            const char* propNameUtf8,
                            float x, float y, float z, float w);

    // SetComputeTextureFromAsset: テクスチャを名前でバインド予約する。
    //   assetId は AssetID.value、または組み込み名キー HashStr("white") / HashStr("builtin://white")。
    //   assetId == 0・未解決・シェーダに無い名前は 0 (落ちない)
    int (*SetComputeTextureFromAsset)(void* engine, const char* shaderUtf8,
                                      const char* textureNameUtf8, uint64_t assetId);

    // DispatchCompute: shaderUtf8 を (必要なら) LoadCompute し、
    //   バインド済み状態を適用してから (gx, gy, gz) グループで Dispatch する。
    //   シェーダ無効・デバイス未接続は 0 (クラッシュしない)
    int (*DispatchCompute)(void* engine, const char* shaderUtf8,
                           uint32_t gx, uint32_t gy, uint32_t gz);

    // ---- v22 (M80l): 破壊への損傷 ----
    // ApplyFractureDamage: entity (Destructible のルートか、その破片のどちらでもよい) の
    //   Destructible 配下で、原点が point から radius 以内の破片へ amount*(1-d/radius) を
    //   加算する (radius<=0 は最寄りの 1 破片へ amount をそのまま)。呼んだ瞬間に damage
    //   フィールドへ書く (即時・同期)。対象が Destructible/FracturePiece のどちらでも
    //   ないエンティティ・破断済みで資産が解決できない場合は何もしない
    void (*ApplyFractureDamage)(void* engine, MyeEntityId entity, MyeVec3 point, float radius,
                                float amount);

    // ---- v23 (M81f): 専用サーバのレーン状態と参加・離脱 ----
    // ★v13 の Net* (表示専用・機種依存) とは別物。ここの 5 本は**確定入力 (システム入力) から導いた
    //   sim 状態** (Scene が持つ SessionLanes、WorldHash 対象) の読み取りで、全員が同じ tick に
    //   同じ値を読む。Update から読んで sim 状態へ書いてよい。
    // ★非サーバ構成 (オフライン / P2P) の既定値: レーン [0, playerCount) が Connected、
    //   playerId は全て 0、イベントは 0 件。
    // ★自分がサーバかクライアントかを知る口は**無い** (意図的)。役割は機種依存で、sim から
    //   読めると分岐してハッシュが割れるため。
    // NetLaneMask: Connected のレーンのビット (bit i = レーン i)
    uint32_t (*NetLaneMask)(void* engine);
    // NetLaneState: 0 Empty / 1 Connected / 2 Reserved (切断後の予約中)。範囲外レーンは 0
    uint32_t (*NetLaneState)(void* engine, uint32_t lane);
    // NetLanePlayerId: そのレーンの playerId。Empty・範囲外・非サーバ構成は 0
    uint64_t (*NetLanePlayerId)(void* engine, uint32_t lane);
    // NetSystemEventCount: この tick の頭に適用された参加・離脱イベントの数 (最大 8)
    uint32_t (*NetSystemEventCount)(void* engine);
    // NetGetSystemEvent: index 番目 (eventSeq 昇順) を out に書いて 1。範囲外は out を 0 埋めして 0
    int (*NetGetSystemEvent)(void* engine, uint32_t index, MyeNetSystemEvent* out);

    // ---- v24 (M82i): NavMesh ----
    // ★クエリ系は agentTypeId が合う最初の Surface (エンティティキー順、Agent の割り当てと同じ規則) を使い、
    //   通れるエリアは areaMask と、その Surface の areaCosts (Agent と同じ filter)。
    //   Surface が未ベイク / 未読み込み (シーンを読んだ最初の tick のスクリプトは、まだ NavSystem の読み込み前) なら
    //   全部 0 を返すだけで何も書かない。クエリは NavSystem の更新 (スクリプトの後) より前の状態を見る。
    // ★座標は全てワールド。全部 sim 状態を変えないクエリで、NavSetDestination / NavStop / NavCompleteLink だけが
    //   NavMeshAgent のフィールドを書く (Get/SetComponentField と同じ。tick の頭の NavSystem が拾う)。

    // NavSetDestination: Agent の目的地を書いて歩かせる (destination + hasDestination)。NavMeshAgent 非所持は 0。
    //   同じ値の再設定は経路を引き直さない (Arrived からは目的地が変わるまで復帰しない。Stuck は押し続け、前進が戻れば Moving、渋滞なら Arrived になる)
    int (*NavSetDestination)(void* engine, MyeEntityId entity, MyeVec3 destination);
    // NavStop: 目的地を外して止める (hasDestination = 0)。NavMeshAgent 非所持は 0
    int (*NavStop)(void* engine, MyeEntityId entity);
    // NavGetAgentState: 実行状態を out へ。NavMeshAgent 非所持は 0 (out は触らない)
    int (*NavGetAgentState)(void* engine, MyeEntityId entity, MyeNavAgentState* out);
    // NavFindPath: from から to への経路の角 (先頭は from を吸着した点) を outCorners へ最大 maxCorners 個書き、
    //   書いた数を返す (0 = 失敗: Surface なし / from か to の近く (1 m x 2 m) にナビメッシュがない / 経路なし)。
    //   届かない目的地は届く限りの最寄りまで返し、*outPartial = 1 (null 可)。maxCorners の上限は 256 (超えた分は切り捨て)
    //   ★Nav* が返す点の y は、ベイクした層から引いた歩行面の高さ (段差の天面・坂で 0.1 m 以内)。ポリゴンの平面の高さではない
    int32_t (*NavFindPath)(void* engine, int32_t agentTypeId, MyeVec3 from, MyeVec3 to, uint32_t areaMask,
                           MyeVec3* outCorners, int32_t maxCorners, int32_t* outPartial);
    // NavSamplePosition: pos の最寄りのナビメッシュ上の点 (extents は探す範囲の半径)。無ければ 0
    int (*NavSamplePosition)(void* engine, int32_t agentTypeId, MyeVec3 pos, MyeVec3 extents, uint32_t areaMask,
                             MyeVec3* out);
    // NavRaycast: from から to へナビメッシュの上を歩く線が壁で止まるか。成功 (from がナビメッシュに乗る) で 1、
    //   止まったかどうかは out->hit。from が乗らなければ 0 (out は触らない)
    int (*NavRaycast)(void* engine, int32_t agentTypeId, MyeVec3 from, MyeVec3 to, uint32_t areaMask,
                      MyeNavRaycastHit* out);
    // NavFindRandomPoint: center を中心に半径 radius の円の中から一様に選んだ点のうち、ナビメッシュの上に乗るものを out へ。
    //   ★乱数は World の RNG (RandomFloat01 と同じ列) を引く = 同じ入力から同じ点が出る。
    //     center の近傍 (1 m x 2 m) にナビメッシュが無い / radius が 0 以下 / Surface なしは、RNG を引かずに 0。
    //     円がほとんどナビメッシュの外で 16 回試して乗らなかった場合も 0 (このときは RNG を引いている)。
    //   ★center から**つながっている**とは限らない (孤島の点も返る)。要るなら NavFindPath の partial で確かめる
    int (*NavFindRandomPoint)(void* engine, int32_t agentTypeId, MyeVec3 center, float radius, uint32_t areaMask,
                              MyeVec3* out);
    // NavCompleteLink: Manual の Link で止まっている (入口へ近づく途中を含む) Agent に完了を通知し、出口へ渡らせる
    //   (NavMeshAgent.linkComplete = 1)。該当しなければ 0 (何も書かない)
    int (*NavCompleteLink)(void* engine, MyeEntityId entity);

    // ---- v25 (M83b): AI の知覚 ----
    // ★結果 (PerceptionGet) は知覚のフェーズ (スクリプトの後・ナビメッシュの前) が書いた値 = スクリプトの Update が
    //   読むのは前の tick の結果。報告 (ReportNoise / ReportDamage) は受け手の保留欄に溜まり、次の知覚のフェーズで
    //   消費される (LateUpdate や OnCollision から報告してもよい)。全部 sim 状態として決定的。

    // PerceptionReportNoise: pos で音を鳴らす (UE の ReportNoiseEvent)。hearingMode = Distance の AIPerception が、
    //   目との距離で減衰した音量 loudness * (1 - 距離 / range) が hearingRange 以内・hearingThreshold 以上なら聞く。
    //   instigator は鳴らした者 (陣営の判定と、知覚の相手になる。無効なハンドル可)。戻り値は聞こえた数。
    //   Acoustic モードの AIPerception には届かない (音響の波は AcousticEmitter で立てる)
    int (*PerceptionReportNoise)(void* engine, MyeVec3 pos, float loudness, float range, MyeEntityId instigator);
    // PerceptionReportDamage: victim が instigator から amount のダメージを受けたと知らせる (UE の ReportDamageEvent)。
    //   見えていなくても攻撃者 (の位置。分からなければ hitPos) を知覚する。victim に AIPerception が無ければ 0
    int (*PerceptionReportDamage)(void* engine, MyeEntityId victim, MyeEntityId instigator, float amount, MyeVec3 hitPos);
    // PerceptionGetCount: observer が知覚している相手の数 (0..8)。AIPerception 非所持は 0
    int32_t (*PerceptionGetCount)(void* engine, MyeEntityId observer);
    // PerceptionGet: index 番目 (相手のエンティティキー順) を out へ書いて 1。範囲外・非所持は 0 (out は触らない)
    int (*PerceptionGet)(void* engine, MyeEntityId observer, int32_t index, MyePercept* out);
    // PerceptionCanSee: observer から target が今見えるか (陣営・距離・視野角・視線。結果へは書かない)。
    //   target に AIStimulusSource が無ければ 0
    int (*PerceptionCanSee)(void* engine, MyeEntityId observer, MyeEntityId target);

    // ---- v26 (M84d2): NavMesh の続き ----
    // ★Agent の細かい制御 (isStopped / autoBraking / avoidancePriority / separationWeight / updatePosition /
    //   updateRotation、読み取り専用の desiredVelocity / nextPosition) は NavMeshAgent のフィールドなので
    //   Get/SetComponentField で読み書きする。
    // ★NavWarp / NavSetPath は呼んだその場で Agent と crowd を書き換える (tick のどこで呼んでも次の NavSystem の
    //   更新がその状態から進める)。全部 sim 状態として決定的。

    // NavWarp: pos の最寄りのナビメッシュ上の点 (水平 max(半径 x 2, 0.6) m・上下 1 m の範囲) へ Agent を瞬間移動させる
    //   (Unity の Warp)。Transform と CC の速度を書き、渡りの途中なら渡りを捨てる。目的地は保ち、次の更新が引き直す。
    //   NavMeshAgent / CC が無い、Surface が未読み込み、近くにナビメッシュが無いなら 0 (何も書かない)
    int (*NavWarp)(void* engine, MyeEntityId entity, MyeVec3 pos);
    // NavCalculatePath: Agent の今の位置から target への経路を、Agent の areaMask・navFilter で引いて out へ (歩かせない)。
    //   Agent が crowd に載っていない (Surface の読み込み前・ナビメッシュの外) / 渡りの途中 / target の近く (1 m x 2 m) に
    //   ナビメッシュが無い / 経路なしなら 0 (out->status = 0)
    int (*NavCalculatePath)(void* engine, MyeEntityId entity, MyeVec3 target, MyeNavPath* out);
    // NavSetPath: path をその Agent の経路にして歩かせる (Unity の SetPath)。目的地は path->end になり、経路は引き直さない。
    //   path の回廊に Agent の今のポリゴンが無い (引いた後に離れた) / タイルが作り直されてポリゴンが古い (Obstacle・Modifier) /
    //   種別が違う / 渡りの途中なら 0 (何も変えない)。そのときは NavSetDestination で引き直す
    int (*NavSetPath)(void* engine, MyeEntityId entity, const MyeNavPath* path);
    // ...Filtered: NavFindPath / NavSamplePosition / NavRaycast / NavFindRandomPoint に .navfilter.json の GUID
    //   (navFilter、0 = 無し) を足したもの。エリアのコストを上書きし、通れないエリアを足す。読み込まれていない GUID は無しと同じ
    int32_t (*NavFindPathFiltered)(void* engine, int32_t agentTypeId, MyeVec3 from, MyeVec3 to, uint32_t areaMask,
                                   uint64_t navFilter, MyeVec3* outCorners, int32_t maxCorners, int32_t* outPartial);
    int (*NavSamplePositionFiltered)(void* engine, int32_t agentTypeId, MyeVec3 pos, MyeVec3 extents, uint32_t areaMask,
                                     uint64_t navFilter, MyeVec3* out);
    int (*NavRaycastFiltered)(void* engine, int32_t agentTypeId, MyeVec3 from, MyeVec3 to, uint32_t areaMask,
                              uint64_t navFilter, MyeNavRaycastHit* out);
    int (*NavFindRandomPointFiltered)(void* engine, int32_t agentTypeId, MyeVec3 center, float radius, uint32_t areaMask,
                                      uint64_t navFilter, MyeVec3* out);
};

// スクリプトの各コールバックに渡されるコンテキスト (POD)
struct MyeUpdateContext {
    float dt = 0.0f;             // 固定 dt
    uint64_t tickIndex = 0;
    MyeEntityId self = {};       // このスクリプトが付いているエンティティ
    const MyeEngineApi* api = nullptr;
};
