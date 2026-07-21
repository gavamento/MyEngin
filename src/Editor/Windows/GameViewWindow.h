#pragma once
#include "Engine/Engine/EngineLoop.h"
#include "Engine/Renderer/RenderTexture.h"

namespace mye {

// ゲームカメラ (シーン内の CameraComponent) 視点の表示 (engine_spec.md 9 章)
class GameViewWindow {
public:
    bool open = true; // 閉じる / 再表示 (タブ [x] と Window メニューに連動)
    void OnRenderViews(EngineContext& ctx);
    void OnImGui(EngineContext& ctx);

private:
    RenderTexture rt_;
    int desiredW_ = 0;
    int desiredH_ = 0;
    bool hasCamera_ = false;
    int aspectMode_ = 0;    // 0=Free 1=16:9 2=4:3 3=1:1 (レターボックス)
    bool showStats_ = true; // 統計オーバーレイ (FPS/entities/tick)
};

} // namespace mye
