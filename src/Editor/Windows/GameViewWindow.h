#pragma once
#include "Engine/Engine/EngineLoop.h"
#include "Engine/Renderer/RenderTexture.h"

namespace mye {

// ゲームカメラ (シーン内の CameraComponent) 視点の表示 (engine_spec.md 9 章)
class GameViewWindow {
public:
    void OnRenderViews(EngineContext& ctx);
    void OnImGui(EngineContext& ctx);

private:
    RenderTexture rt_;
    int desiredW_ = 0;
    int desiredH_ = 0;
    bool hasCamera_ = false;
};

} // namespace mye
