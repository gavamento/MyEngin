#pragma once
// スクリプトモジュールの記述子 (Engine と GameLogic.dll の両方が読む純粋な型定義)。
// engine_spec.md 5.2 / 8.4: 状態はエンジン側 ECS に置かれ、DLL はロジック関数のみ持つ。

#include <stdint.h>

#include "Shared/EngineAPI.h"

// Engine/Core/Ecs/Reflection.h の FieldType と同値 (エンジン側で変換・検証される)
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
    // v11 (M50d) 末尾追加。★番号は Reflection.h の宣言順そのまま — String256 は
    // String64 の隣ではなく Float4x4 の後 (M34 で末尾追加された歴史をミラーする)
    MYE_FIELD_ASSETREF = 11,  // uint64 (AssetID)
    MYE_FIELD_STRING64 = 12,  // char[64] 固定長 (終端以降もハッシュ対象 — 尾部はゼロに)
    MYE_FIELD_FLOAT4X4 = 13,  // float[16]
    MYE_FIELD_STRING256 = 14, // char[256] 固定長
};

struct MyeScriptField {
    const char* name;  // DLL 内静的文字列 (エンジンはロード時にコピーする)
    int32_t type;      // MyeFieldType
    uint32_t offset;
    // ---- Inspector 用メタデータ (v16 でレイアウトに追加) ----
    // ★この構造体のレイアウトを変えるときは MYE_API_VERSION を同時に上げること — 上げずに
    //   足すと、apiVersion が一致したまま別レイアウトの GameLogic.dll が受理されて
    //   静かに壊れる (フィールド表が丸ごとずれる)。
    // displayName: Inspector の表示名 (null = name をそのまま出す)。engine 側の
    //   FieldDesc::displayName と同じ役割で、シリアライズキーである name には触らない。
    // rangeMin/rangeMax: スライダの範囲 (両方 0 = 範囲指定なし = 従来のドラッグ入力)。
    // ★layoutHash には**入れない** — 表示メタデータが変わっただけでスクリプト状態の
    //   移行を走らせる理由は無い (移行判定は (name,type,offset) が正本)
    const char* displayName;
    float rangeMin;
    float rangeMax;
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
    // トリガーイベント (v2、null 可) — CollisionSystem が配信する
    void (*onTriggerEnter)(void* state, MyeUpdateContext* ctx, MyeEntityId other);
    void (*onTriggerExit)(void* state, MyeUpdateContext* ctx, MyeEntityId other);
    // ソリッド衝突イベント (v4 で予約、M28c で配信)。normal は相手→自分方向 (ワールド)
    void (*onCollisionEnter)(void* state, MyeUpdateContext* ctx, MyeEntityId other, MyeVec3 normal);
    void (*onCollisionStay)(void* state, MyeUpdateContext* ctx, MyeEntityId other);
    void (*onCollisionExit)(void* state, MyeUpdateContext* ctx, MyeEntityId other);
    // 破壊イベント (v22、M80l、null 可)。Destructible のルートにあるスクリプトへ、
    // 分かれた塊のリーダー (piece)・その塊で荷重最大の破片の原点 (point)・その荷重 [N]
    // (impulse) を渡す。分かれた塊ごとに 1 回、ルート index → 新リーダー index 昇順で届く
    void (*onBreak)(void* state, MyeUpdateContext* ctx, MyeEntityId piece, MyeVec3 point,
                    float impulse);
};

// BT タスクの戻り値 (BtResult と同値)
enum MyeBtStatus {
    MYE_BT_RUNNING = 0,
    MYE_BT_SUCCESS = 1,
    MYE_BT_FAILURE = 2,
};

// BT タスクのコールバックに渡されるコンテキスト (POD)。状態 (T) は第 1 引数で渡る
struct MyeBtTaskContext {
    float dt;                    // 固定 dt
    uint64_t tickIndex;
    MyeEntityId self;            // この木の持ち主 (BehaviorTreeComponent を付けたエンティティ)
    const MyeEngineApi* api;
};

// BT の C++ タスク 1 種。状態 (stateSize バイト) は木のノードのインスタンスごとにエンジンの BT 表が持つ。
// 入るたびに construct で既定値にし、.bt.json のノードの "fields" の値を名前で上書きしてから onStart を呼ぶ。
// ★stateSize の上限は 112 バイト (超えたタスクは登録されず、そのノードは Failure)。
// ★状態のバイト列は BT 節とワールドハッシュに入るので、FIELDS に書かないメンバとパディングは呼ぶたびに 0 へ戻る
//   (Debug / Release でパディングの中身が違ってもハッシュが割れないように)。保ちたい値は全部 FIELDS に書く。
// ★layoutHash が変わった (ホットリロードでフィールドが変わった) 実行中の木は、OnAbort を呼ばずに状態を捨てて根からやり直す
struct MyeBtTaskDesc {
    const char* name;
    uint32_t stateSize;
    uint32_t stateAlign;
    uint64_t layoutHash;         // MyeScriptDesc と同じ (name,type,offset) 列の FNV-1a
    const MyeScriptField* fields;
    uint32_t fieldCount;
    void (*construct)(void* dst);
    // 入った tick に 1 回。null なら入った tick に onTick を呼ぶ。戻り値は MyeBtStatus
    int32_t (*onStart)(void* state, MyeBtTaskContext* ctx);
    // 入った次の tick から毎 tick。null なら Running のまま
    int32_t (*onTick)(void* state, MyeBtTaskContext* ctx);
    // Abort されたとき (Success / Failure で終わったときは呼ばない)。null 可
    void (*onAbort)(void* state, MyeBtTaskContext* ctx);
};

struct MyeScriptModule {
    uint32_t apiVersion; // MYE_API_VERSION と一致しなければロード拒否
    uint32_t scriptCount;
    const MyeScriptDesc* scripts;
    // v27 (M85k) 末尾追加
    uint32_t btTaskCount;
    const MyeBtTaskDesc* btTasks;
};

// GameLogic.dll がエクスポートするエントリポイントの型
// extern "C" const MyeScriptModule* GameLogic_GetModule(const MyeEngineApi* api);
typedef const MyeScriptModule* (*MyeGetModuleFn)(const MyeEngineApi* api);
