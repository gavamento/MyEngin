#pragma once
#include <DirectXMath.h>

#include "Engine/Engine/EngineLoop.h"
#include "Engine/Renderer/RenderTexture.h"

namespace mye {

// エディタカメラでシーンを描画するビュー (engine_spec.md 9 章)。
// カメラはエンティティではなくエディタ所有 (Play 状態と無関係に操作できる)
class SceneViewWindow {
public:
    void OnRenderViews(EngineContext& ctx); // フェーズ 6: RT へ描画
    void OnImGui(EngineContext& ctx);       // Image 表示 + カメラ操作

private:
    RenderTexture rt_;
    DirectX::XMFLOAT3 camPos_ = { 0.0f, 7.0f, -16.0f };
    float camYaw_ = 0.0f;
    float camPitch_ = 18.0f;
    int desiredW_ = 0;
    int desiredH_ = 0;
};

} // namespace mye
