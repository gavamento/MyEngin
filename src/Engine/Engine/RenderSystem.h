#pragma once
#include <d3d11.h>

#include "Engine/Renderer/RenderTypes.h"

namespace mye {

class World;
class GraphicsDevice;
class IRenderPath;
class ShaderManager;
struct RenderResources;

// このフレームの描画先
struct FrameTarget {
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    int width = 0;
    int height = 0;
    float clearColor[4] = { 0.08f, 0.09f, 0.11f, 1.0f };
};

// ECS から描画アイテムを収集し、ソートして RenderPath に提出する (spec 5.1 システム層 / 6.3)。
// カメラ: isPrimary の CameraComponent (無ければ最初のカメラ)。
// ライト: 最初の LightComponent (向き = エンティティの +Z)
class RenderSystem {
public:
    void Render(World& world, GraphicsDevice& device, IRenderPath& path, ShaderManager& shaders,
                RenderResources& resources, const FrameTarget& target);

private:
    RenderQueue queue_; // フレーム毎に再利用 (アロケーション回避)
};

} // namespace mye
