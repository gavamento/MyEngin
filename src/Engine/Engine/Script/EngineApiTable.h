#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "Engine/Platform/Input.h"
#include "Shared/EngineAPI.h"

namespace mye {

class Scene;

// スクリプトが tick 内で積む再生イベント (M19)。ハッシュ後に AudioSystem が drain する。
struct ScriptAudioEvent {
    std::string key; // .wav アセットキー
    float volume = 1.0f;
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
};

// out に MyeEngineApi (engine = ctx) を構築する。ctx の生存は呼び出し側が管理する。
void BuildEngineApi(MyeEngineApi& out, ScriptApiContext* ctx);

} // namespace mye
