#pragma once
// スクリプトモジュールの記述子 (Engine と GameLogic.dll の両方が読む純粋な型定義)。
// engine_spec.md 5.2 / 8.4: 状態はエンジン側 ECS に置かれ、DLL はロジック関数のみ持つ。

#include <stdint.h>

#include "Shared/EngineAPI.h"

// Engine/Core/Reflection.h の FieldType と同値 (エンジン側で変換・検証される)
enum MyeFieldType {
    MYE_FIELD_FLOAT = 0,
    MYE_FIELD_INT32 = 1,
    MYE_FIELD_UINT32 = 2,
    MYE_FIELD_UINT64 = 3,
    MYE_FIELD_BOOL = 4,
    MYE_FIELD_FLOAT2 = 5,
    MYE_FIELD_FLOAT3 = 6,
    MYE_FIELD_FLOAT4 = 7,
    MYE_FIELD_QUAT = 8,
    MYE_FIELD_COLOR = 9,
    MYE_FIELD_ENTITYREF = 10,
};

struct MyeScriptField {
    const char* name;  // DLL 内静的文字列 (エンジンはロード時にコピーする)
    int32_t type;      // MyeFieldType
    uint32_t offset;
};

// スクリプト型 1 つ分。関数ポインタは DLL 内を指す — リロード時に必ず再バインドされる
struct MyeScriptDesc {
    const char* name;
    uint32_t stateSize;
    uint32_t stateAlign;
    uint64_t layoutHash; // (name,type,offset) 列の FNV-1a。一致すれば移行不要
    const MyeScriptField* fields;
    uint32_t fieldCount;
    void (*construct)(void* dst);                            // デフォルト値の書き込み
    void (*start)(void* state, MyeUpdateContext* ctx);       // null 可
    void (*update)(void* state, MyeUpdateContext* ctx);      // null 可
    void (*lateUpdate)(void* state, MyeUpdateContext* ctx);  // null 可
};

struct MyeScriptModule {
    uint32_t apiVersion; // MYE_API_VERSION と一致しなければロード拒否
    uint32_t scriptCount;
    const MyeScriptDesc* scripts;
};

// GameLogic.dll がエクスポートするエントリポイントの型
// extern "C" const MyeScriptModule* GameLogic_GetModule(const MyeEngineApi* api);
typedef const MyeScriptModule* (*MyeGetModuleFn)(const MyeEngineApi* api);
