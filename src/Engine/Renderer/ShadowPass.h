#pragma once
#include <DirectXMath.h>
#include <d3d11.h>
#include <wrl/client.h>

#include "Engine/Core/EntityID.h"

namespace mye {

class GraphicsDevice;
class ShaderManager;
class RenderQueue;
struct RenderResources;

// 平行光シャドウマップ (M17)。ライト視点から不透明ジオメトリの深度のみを描き、
// 深度を SRV として本描画のライティングへ渡す (PCF 比較)。描画専用でハッシュ非対象。
class ShadowPass {
public:
    bool Init(GraphicsDevice& device, ShaderManager& shaders, int resolution = 2048);
    bool IsReady() const { return ready_; }

    // 不透明キューを lightViewProj (非転置、行ベクトル規約 world*view*proj) でシャドウ深度へ描く。
    void Render(GraphicsDevice& device, ShaderManager& shaders, const RenderQueue& queue,
                RenderResources& resources, const DirectX::XMFLOAT4X4& lightViewProj);

    ID3D11ShaderResourceView* SRV() const { return srv_.Get(); }
    int Resolution() const { return resolution_; }

private:
    bool ready_ = false;
    int resolution_ = 0;
    AssetID depthShader_ = {};

    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> objectCB_; // 1 オブジェクトあたり transpose(world*lightVP)
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthState_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_; // 深度バイアス付き
};

} // namespace mye
