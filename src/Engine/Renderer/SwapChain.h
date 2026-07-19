#pragma once
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

namespace mye {

class GraphicsDevice;

// flip-model スワップチェーン + バックバッファ RTV。
// フォーマットは R8G8B8A8_UNORM (sRGB 変換はレンダリングパス側で扱う方針。M6.5 で両パス統一)
class SwapChain {
public:
    bool Init(GraphicsDevice& device, void* hwnd, int width, int height);
    void Shutdown();

    void Resize(int width, int height);
    void Present(bool vsync);

    ID3D11RenderTargetView* BackbufferRTV() const { return rtv_.Get(); }
    int Width() const { return width_; }
    int Height() const { return height_; }

private:
    bool CreateRTV();

    GraphicsDevice* device_ = nullptr;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;
    int width_ = 0;
    int height_ = 0;
};

} // namespace mye
