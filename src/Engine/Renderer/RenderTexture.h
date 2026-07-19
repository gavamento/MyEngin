#pragma once
#include <d3d11.h>
#include <wrl/client.h>

namespace mye {

class GraphicsDevice;

// オフスクリーン描画先 (SceneView / GameView / M6.5 GBuffer 補助)。
// カラー RTV+SRV とデプス DSV を持つ。サイズ変更は Resize (同サイズなら no-op)
class RenderTexture {
public:
    bool Create(GraphicsDevice& device, int width, int height);
    void Resize(GraphicsDevice& device, int width, int height);
    void Release();

    ID3D11RenderTargetView* RTV() const { return rtv_.Get(); }
    ID3D11DepthStencilView* DSV() const { return dsv_.Get(); }
    ID3D11ShaderResourceView* SRV() const { return srv_.Get(); }
    int Width() const { return width_; }
    int Height() const { return height_; }
    bool IsValid() const { return rtv_ != nullptr; }

private:
    Microsoft::WRL::ComPtr<ID3D11Texture2D> color_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> depth_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_;
    int width_ = 0;
    int height_ = 0;
};

} // namespace mye
