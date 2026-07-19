#include "Engine/Renderer/RenderTexture.h"

#include "Engine/Core/Log.h"
#include "Engine/Renderer/GraphicsDevice.h"

namespace mye {

bool RenderTexture::Create(GraphicsDevice& device, int width, int height, DXGI_FORMAT format,
                           bool withDepth)
{
    if (width <= 0 || height <= 0) {
        return false;
    }
    Release();

    ID3D11Device* dev = device.Device();

    D3D11_TEXTURE2D_DESC cd = {};
    cd.Width = static_cast<UINT>(width);
    cd.Height = static_cast<UINT>(height);
    cd.MipLevels = 1;
    cd.ArraySize = 1;
    cd.Format = format;
    cd.SampleDesc = { 1, 0 };
    cd.Usage = D3D11_USAGE_DEFAULT;
    cd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&cd, nullptr, color_.GetAddressOf()))
        || FAILED(dev->CreateRenderTargetView(color_.Get(), nullptr, rtv_.GetAddressOf()))
        || FAILED(dev->CreateShaderResourceView(color_.Get(), nullptr, srv_.GetAddressOf()))) {
        MYE_LOG_ERROR("RenderTexture: color creation failed (%dx%d)", width, height);
        Release();
        return false;
    }

    if (withDepth) {
        D3D11_TEXTURE2D_DESC dd = cd;
        dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        if (FAILED(dev->CreateTexture2D(&dd, nullptr, depth_.GetAddressOf()))
            || FAILED(dev->CreateDepthStencilView(depth_.Get(), nullptr, dsv_.GetAddressOf()))) {
            MYE_LOG_ERROR("RenderTexture: depth creation failed (%dx%d)", width, height);
            Release();
            return false;
        }
    }

    width_ = width;
    height_ = height;
    format_ = format;
    withDepth_ = withDepth;
    return true;
}

void RenderTexture::Resize(GraphicsDevice& device, int width, int height, DXGI_FORMAT format,
                           bool withDepth)
{
    if (width == width_ && height == height_ && format == format_ && withDepth == withDepth_) {
        return;
    }
    Create(device, width, height, format, withDepth);
}

void RenderTexture::Release()
{
    srv_.Reset();
    dsv_.Reset();
    rtv_.Reset();
    depth_.Reset();
    color_.Reset();
    width_ = 0;
    height_ = 0;
    format_ = DXGI_FORMAT_R8G8B8A8_UNORM;
    withDepth_ = true;
}

} // namespace mye
