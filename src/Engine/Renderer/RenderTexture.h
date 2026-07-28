#pragma once
#include <d3d11.h>
#include <wrl/client.h>

namespace mye {

class GraphicsDevice;

// オフスクリーン描画先 (SceneView / GameView / GBuffer)。
// カラー RTV+SRV と (任意で) デプス DSV を持つ。サイズ変更は Resize (同サイズなら no-op)
class RenderTexture {
public:
    bool Create(GraphicsDevice& device, int width, int height,
                DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM, bool withDepth = true);
    void Resize(GraphicsDevice& device, int width, int height,
                DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM, bool withDepth = true);
    void Release();

    ID3D11RenderTargetView* RTV() const { return rtv_.Get(); }
    ID3D11DepthStencilView* DSV() const { return dsv_.Get(); }
    ID3D11ShaderResourceView* SRV() const { return srv_.Get(); }
    // M42a: 深度の SRV 読み (ソフトパーティクル等)。read-only DSV とセットで
    // 「深度テスト継続 + SRV 参照」の同時バインドが合法になる
    ID3D11ShaderResourceView* DepthSRV() const { return depthSrv_.Get(); }
    ID3D11DepthStencilView* DSVReadOnly() const { return dsvReadOnly_.Get(); }
    int Width() const { return width_; }
    int Height() const { return height_; }
    bool IsValid() const { return rtv_ != nullptr; }

private:
    Microsoft::WRL::ComPtr<ID3D11Texture2D> color_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> depth_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsvReadOnly_; // M42a
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> depthSrv_;  // M42a
    int width_ = 0;
    int height_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_R8G8B8A8_UNORM;
    bool withDepth_ = true;
};

} // namespace mye
