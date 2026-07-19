#pragma once
#include <wrl/client.h>

#include "Engine/Renderer/RenderPath.h"
#include "Engine/Renderer/RenderTexture.h"

namespace mye {

// Deferred レンダリング (engine_spec.md 6.1 Option B / M6.5)。
//   1. ジオメトリパス: opaque → GBuffer (albedo RGBA8 + 法線 R10G10B10A2 + 共有深度)
//   2. ライティングパス: フルスクリーン解決 (Forward と同じ common.hlsli の関数)
//   3. 透明後段: transparent はマテリアルの Forward シェーダで上描き
//      (パーティクルはさらにその後、RenderSystem が共通の Forward 後段として描く)
// ライトは Forward と同じ LightList データを使うため、切替で見た目が一致する
class DeferredPath : public IRenderPath {
public:
    const char* Name() const override { return "Deferred"; }
    bool Init(GraphicsDevice& device, ShaderManager& shaders) override;
    void Shutdown() override;
    void Render(GraphicsDevice& device, const RenderView& view, const RenderQueue& queue,
                const DirectionalLightData& light, RenderResources& resources,
                ShaderManager& shaders) override;

private:
    RenderTexture gbAlbedo_; // a=1 でジオメトリ有りマーク
    RenderTexture gbNormal_;

    Microsoft::WRL::ComPtr<ID3D11Buffer> perFrameCB_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> perObjectCB_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> lightCB_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthOpaque_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthDisabled_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthTransparent_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendOpaque_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendAlpha_;
    AssetID gbufferShader_ = {};
    AssetID lightShader_ = {};
};

} // namespace mye
