#include "Engine/Renderer/SwapChain.h"

#include "Engine/Core/Log.h"
#include "Engine/Renderer/GraphicsDevice.h"
#include "Engine/Renderer/ImageWrite.h"

namespace mye {

using Microsoft::WRL::ComPtr;

bool SwapChain::Init(GraphicsDevice& device, void* hwnd, int width, int height)
{
    device_ = &device;
    width_ = width;
    height_ = height;

    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(device.Device()->QueryInterface(IID_PPV_ARGS(dxgiDevice.GetAddressOf())))) {
        MYE_LOG_ERROR("SwapChain: QueryInterface(IDXGIDevice) failed");
        return false;
    }
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(adapter.GetAddressOf()))) {
        MYE_LOG_ERROR("SwapChain: GetAdapter failed");
        return false;
    }
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf())))) {
        MYE_LOG_ERROR("SwapChain: GetParent(IDXGIFactory2) failed");
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc = { 1, 0 };
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Scaling = DXGI_SCALING_STRETCH;

    HRESULT hr = factory->CreateSwapChainForHwnd(
        device.Device(), static_cast<HWND>(hwnd), &desc, nullptr, nullptr,
        swapChain_.GetAddressOf());
    if (FAILED(hr)) {
        MYE_LOG_ERROR("CreateSwapChainForHwnd failed (hr=0x%08lX)", hr);
        return false;
    }

    // Alt+Enter の排他フルスクリーン遷移は無効化 (ボーダーレスで扱う方針)
    factory->MakeWindowAssociation(static_cast<HWND>(hwnd), DXGI_MWA_NO_ALT_ENTER);

    return CreateRTV() && CreateDepth();
}

void SwapChain::Shutdown()
{
    depthSrv_.Reset();
    dsvReadOnly_.Reset();
    dsv_.Reset();
    depthTex_.Reset();
    rtv_.Reset();
    swapChain_.Reset();
    device_ = nullptr;
}

bool SwapChain::CreateDepth()
{
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = static_cast<UINT>(width_);
    td.Height = static_cast<UINT>(height_);
    td.MipLevels = 1;
    td.ArraySize = 1;
    // M42a: TYPELESS 化 (RenderTexture.cpp と同型。深度ビット素性は D24S8 のまま)
    td.Format = DXGI_FORMAT_R24G8_TYPELESS;
    td.SampleDesc = { 1, 0 };
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->Device()->CreateTexture2D(&td, nullptr, depthTex_.ReleaseAndGetAddressOf()))) {
        MYE_LOG_ERROR("SwapChain: depth texture creation failed");
        return false;
    }

    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; // TYPELESS なので明示必須
    dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    D3D11_DEPTH_STENCIL_VIEW_DESC roDesc = dsvDesc;
    roDesc.Flags = D3D11_DSV_READ_ONLY_DEPTH | D3D11_DSV_READ_ONLY_STENCIL;
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; // .r = 深度 [0,1]
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    if (FAILED(device_->Device()->CreateDepthStencilView(depthTex_.Get(), &dsvDesc,
                                                         dsv_.ReleaseAndGetAddressOf()))
        || FAILED(device_->Device()->CreateDepthStencilView(
            depthTex_.Get(), &roDesc, dsvReadOnly_.ReleaseAndGetAddressOf()))
        || FAILED(device_->Device()->CreateShaderResourceView(
            depthTex_.Get(), &srvDesc, depthSrv_.ReleaseAndGetAddressOf()))) {
        MYE_LOG_ERROR("SwapChain: depth view creation failed");
        return false;
    }
    return true;
}

bool SwapChain::CreateRTV()
{
    ComPtr<ID3D11Texture2D> backbuffer;
    if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(backbuffer.GetAddressOf())))) {
        MYE_LOG_ERROR("SwapChain: GetBuffer failed");
        return false;
    }
    if (FAILED(device_->Device()->CreateRenderTargetView(backbuffer.Get(), nullptr, rtv_.ReleaseAndGetAddressOf()))) {
        MYE_LOG_ERROR("SwapChain: CreateRenderTargetView failed");
        return false;
    }
    return true;
}

void SwapChain::Resize(int width, int height)
{
    if (!swapChain_ || width <= 0 || height <= 0) {
        return;
    }
    if (width == width_ && height == height_) {
        return;
    }

    // バックバッファ参照を全て外してから ResizeBuffers (flip model の必須要件)
    rtv_.Reset();
    dsv_.Reset();
    depthTex_.Reset();
    device_->Context()->OMSetRenderTargets(0, nullptr, nullptr);
    device_->Context()->Flush();

    const HRESULT hr = swapChain_->ResizeBuffers(0, static_cast<UINT>(width), static_cast<UINT>(height),
                                                 DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        MYE_LOG_ERROR("ResizeBuffers failed (hr=0x%08lX)", hr);
        return;
    }
    width_ = width;
    height_ = height;
    CreateRTV();
    CreateDepth();
}

bool SwapChain::SaveBackbufferPng(const std::wstring& path)
{
    ComPtr<ID3D11Texture2D> backbuffer;
    if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(backbuffer.GetAddressOf())))) {
        return false;
    }
    D3D11_TEXTURE2D_DESC desc = {};
    backbuffer->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device_->Device()->CreateTexture2D(&desc, nullptr, staging.GetAddressOf()))) {
        return false;
    }
    ID3D11DeviceContext* dc = device_->Context();
    dc->CopyResource(staging.Get(), backbuffer.Get());

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(dc->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return false;
    }
    const bool ok = WritePngRGBA(path, static_cast<const uint8_t*>(mapped.pData),
                                 static_cast<int>(desc.Width), static_cast<int>(desc.Height),
                                 static_cast<int>(mapped.RowPitch));
    dc->Unmap(staging.Get(), 0);
    if (ok) {
        MYE_LOG_INFO("screenshot saved: %dx%d", desc.Width, desc.Height);
    } else {
        MYE_LOG_ERROR("screenshot write failed");
    }
    return ok;
}

void SwapChain::Present(bool vsync)
{
    const HRESULT hr = swapChain_->Present(vsync ? 1 : 0, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        MYE_LOG_ERROR("Present: device removed/reset (hr=0x%08lX, reason=0x%08lX)", hr,
                      device_ ? device_->Device()->GetDeviceRemovedReason() : 0);
    }
}

} // namespace mye
