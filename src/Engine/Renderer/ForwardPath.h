#pragma once
#include <wrl/client.h>

#include "Engine/Renderer/RenderPath.h"

namespace mye {

// Forward レンダリング (engine_spec.md 6.1 Option A)。
// opaque を手前順 → transparent を奥順で 1 パス描画する
class ForwardPath : public IRenderPath {
public:
    const char* Name() const override { return "Forward"; }
    bool Init(GraphicsDevice& device, ShaderManager& shaders) override;
    void Shutdown() override;
    void Render(GraphicsDevice& device, const RenderView& view, const RenderQueue& queue,
                const SceneLightData& lights, RenderResources& resources,
                ShaderManager& shaders) override;

private:
    void DrawItems(GraphicsDevice& device, const std::vector<RenderItem>& items,
                   const RenderView& view, RenderResources& resources, ShaderManager& shaders);

    Microsoft::WRL::ComPtr<ID3D11Buffer> perFrameCB_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> perObjectCB_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> materialCB_; // PBR パラメータ (metallic/roughness)
    Microsoft::WRL::ComPtr<ID3D11Buffer> boneCB_;     // ボーンパレット (b3、スキニング、M18)
    AssetID skinnedShader_ = {};                      // forward_skinned (スキンメッシュ用に差替)
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> shadowSampler_; // 比較サンプラ (PCF)
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthOpaque_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthTransparent_; // 書き込みなし
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendOpaque_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendAlpha_;
};

} // namespace mye
