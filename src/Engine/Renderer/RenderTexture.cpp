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
        // M42a: TYPELESS 化して DSV (D24S8) と SRV (R24X8) を同一テクスチャから作る。
        // 深度のビット素性は従来の D24S8 のまま (ShadowPass.cpp の R32_TYPELESS と同型パターン)
        D3D11_TEXTURE2D_DESC dd = cd;
        dd.Format = DXGI_FORMAT_R24G8_TYPELESS;
        dd.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;

        D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
        dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; // TYPELESS なので明示必須
        dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;

        // read-only DSV: 深度テストは効くが書込み不可 -> 深度 SRV との同時バインドが合法
        D3D11_DEPTH_STENCIL_VIEW_DESC roDesc = dsvDesc;
        roDesc.Flags = D3D11_DSV_READ_ONLY_DEPTH | D3D11_DSV_READ_ONLY_STENCIL;

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; // .r = 深度 [0,1]
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;

        if (FAILED(dev->CreateTexture2D(&dd, nullptr, depth_.GetAddressOf()))
            || FAILED(dev->CreateDepthStencilView(depth_.Get(), &dsvDesc, dsv_.GetAddressOf()))
            || FAILED(dev->CreateDepthStencilView(depth_.Get(), &roDesc,
                                                  dsvReadOnly_.GetAddressOf()))
            || FAILED(dev->CreateShaderResourceView(depth_.Get(), &srvDesc,
                                                    depthSrv_.GetAddressOf()))) {
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
    depthSrv_.Reset();
    srv_.Reset();
    dsvReadOnly_.Reset();
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
