#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "Engine/Engine/DebugDraw.h"
#include "Engine/Platform/Input.h"
#include "Shared/EngineAPI.h"

namespace mye {

class Scene;

// スクリプトが tick 内で積む再生イベント (M19)。ハッシュ後に AudioSystem が drain する。
struct ScriptAudioEvent {
    std::string key; // .wav アセットキー
    float volume = 1.0f;
};

// スクリプトが tick 内で積むエフェクト/汎用生成要求 (M32f、v7 Instantiate も共用)。
// tick 末 (ハッシュ前) に EngineLoop がプレハブをインスタンス化する。
// sim 状態なので record/verify とも実行される。
struct EffectSpawnRequest {
    std::string prefabKey; // .prefab.json、assets 相対 (省略サフィックス可)
    MyeVec3 pos = {};      // ルートの LocalTransform.position (parent 有効ならローカル)
    MyeEntityId parent = {};
    // v7 Instantiate (M37): 呼出時に予約したルート fileId (0 = PlayEffect = 予約なし)。
    // 予約は Scene::NextFileId() の消費 = 決定論点で行われ record/verify で同一列になる
    uint64_t reservedRootFid = 0;
};

// MyeEngineApi の engine 不透明ポインタが指すコンテキスト。
// C++ の ScriptHost と C# の ManagedHost が同じ C ABI テーブルを共有するために使う
// (Transform / Log / Input / Random は両ホストで同一実装)。
struct ScriptApiContext {
    Scene* scene = nullptr;
    InputSnapshot input = {};
    uint64_t tickIndex = 0;
    float dt = 1.0f / 60.0f;
    // v3 (M19) のスロット。EngineLoop が毎 tick セットする。null 時は該当 API が no-op。
    std::vector<ScriptAudioEvent>* audioQueue = nullptr; // PlaySound の積み先
    std::wstring* pendingScene = nullptr;                // LoadScene の書き先 (tick 末に消費)
    void* physics = nullptr;                             // Raycast 用 (M20 で PhysicsWorld*)
    std::vector<EffectSpawnRequest>* effectQueue = nullptr; // PlayEffect の積み先 (M32f、tick 末消費)
    std::vector<DebugLineCmd>* debugLines = nullptr; // DebugDrawLine の積み先 (v7。tick 頭クリア)
};

// out に MyeEngineApi (engine = ctx) を構築する。ctx の生存は呼び出し側が管理する。
void BuildEngineApi(MyeEngineApi& out, ScriptApiContext* ctx);

} // namespace mye
