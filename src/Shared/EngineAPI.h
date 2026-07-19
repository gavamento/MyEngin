#pragma once
// エンジン → GameLogic.dll に渡す C 関数テーブル (engine_spec.md 8.4)。
// DLL 境界規則:
//   - extern "C" スタイルの関数ポインタのみ。C++ vtable / STL / 例外は越えない
//   - メモリの確保・解放は常にエンジン側 (この API 経由)。DLL 側 CRT ヒープに依存しない
//   - 文字列 (const char*) は呼び出しの間だけ有効。保持するならコピーすること

#include <stdint.h>

#include "Shared/MathPod.h"

// 互換性チェック用。テーブルや ScriptDesc のレイアウトを変えたら必ず上げること
#define MYE_API_VERSION 1u

// MYE_LOG レベル (Engine/Core/Log.h の LogLevel と同値)
enum MyeLogLevel {
    MYE_LOG_LEVEL_TRACE = 0,
    MYE_LOG_LEVEL_INFO = 1,
    MYE_LOG_LEVEL_WARN = 2,
    MYE_LOG_LEVEL_ERROR = 3,
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
};

// スクリプトの各コールバックに渡されるコンテキスト (POD)
struct MyeUpdateContext {
    float dt = 0.0f;             // 固定 dt
    uint64_t tickIndex = 0;
    MyeEntityId self = {};       // このスクリプトが付いているエンティティ
    const MyeEngineApi* api = nullptr;
};
