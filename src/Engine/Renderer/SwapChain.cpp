#include "Engine/Renderer/SwapChain.h"

#include "Engine/Core/Log.h"
#include "Engine/Renderer/GraphicsDevice.h"

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

    return CreateRTV();
}

void SwapChain::Shutdown()
{
    rtv_.Reset();
    swapChain_.Reset();
    device_ = nullptr;
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
