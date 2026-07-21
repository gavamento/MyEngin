#pragma once
// エンジン → GameLogic.dll に渡す C 関数テーブル (engine_spec.md 8.4)。
// DLL 境界規則:
//   - extern "C" スタイルの関数ポインタのみ。C++ vtable / STL / 例外は越えない
//   - メモリの確保・解放は常にエンジン側 (この API 経由)。DLL 側 CRT ヒープに依存しない
//   - 文字列 (const char*) は呼び出しの間だけ有効。保持するならコピーすること

#include <stdint.h>

#include "Shared/MathPod.h"

// 互換性チェック用。テーブルや ScriptDesc のレイアウトを変えたら必ず上げること
// v3 (M19): gamepad / Raycast / PlaySound / StopSound / LoadScene をスロット予約で一括追加
// v4 (M28a): 剛体操作 (AddForce/AddImpulse/AddTorque/Get/SetVelocity) + 空間クエリ
//            (OverlapSphere/OverlapBox/SphereCast) + OnCollision コールバックを一括追加。
//            AddTorque は M28b、Overlap*/SphereCast と OnCollision 配信は M28c で実装
#define MYE_API_VERSION 4u

// MYE_LOG レベル (Engine/Core/Log.h の LogLevel と同値)
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

// Raycast のヒット結果 (M19 で予約、M20 の物理で実装)
struct MyeRaycastHit {
    MyeEntityId entity;
    MyeVec3 point;
    MyeVec3 normal;
    float distance;
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
    // Overlap 系: ヒットしたエンティティを outEntities に最大 maxCount 個 (index 昇順) 書き、
    // 戻り値は「切り捨て前の総ヒット数」。バッファは呼び出し側が確保する (DLL 境界規則)。
    int (*OverlapSphere)(void* engine, MyeVec3 center, float radius, MyeEntityId* outEntities,
                         int maxCount);
    int (*OverlapBox)(void* engine, MyeVec3 center, MyeVec3 halfExtents, MyeQuat rotation,
                      MyeEntityId* outEntities, int maxCount);
    // 半径 radius の球を dir 方向に掃引し最近ヒットを返す (Raycast の太い版)
    int (*SphereCast)(void* engine, MyeVec3 origin, MyeVec3 dir, float radius, float maxDist,
                      MyeRaycastHit* outHit);
};

// スクリプトの各コールバックに渡されるコンテキスト (POD)
struct MyeUpdateContext {
    float dt = 0.0f;             // 固定 dt
    uint64_t tickIndex = 0;
    MyeEntityId self = {};       // このスクリプトが付いているエンティティ
    const MyeEngineApi* api = nullptr;
};
