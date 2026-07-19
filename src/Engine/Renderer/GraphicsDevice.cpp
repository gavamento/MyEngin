#include "Engine/Renderer/GraphicsDevice.h"

#include "Engine/Core/Log.h"

namespace mye {

bool GraphicsDevice::Init()
{
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    // デバッグレイヤは診断出力のみで挙動 (状態) に影響しないため、
    // Debug 構成限定でも一貫性ポリシー (spec 11.2 規則 1) に抵触しない
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    const D3D_FEATURE_LEVEL requested = D3D_FEATURE_LEVEL_11_0;
    D3D_FEATURE_LEVEL obtained = {};

    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        &requested, 1, D3D11_SDK_VERSION,
        device_.GetAddressOf(), &obtained, context_.GetAddressOf());

#ifdef _DEBUG
    if (FAILED(hr)) {
        // Graphics Tools 未インストール環境ではデバッグレイヤ付き生成が失敗する
        MYE_LOG_WARN("D3D11 debug layer unavailable, falling back (hr=0x%08lX)", hr);
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
            &requested, 1, D3D11_SDK_VERSION,
            device_.GetAddressOf(), &obtained, context_.GetAddressOf());
    } else {
        debugLayer_ = true;
    }
#endif

    if (FAILED(hr)) {
        MYE_LOG_ERROR("D3D11CreateDevice failed (hr=0x%08lX)", hr);
        return false;
    }

    MYE_LOG_INFO("D3D11 device created (FL 11_0, debug layer: %s)", debugLayer_ ? "on" : "off");
    return true;
}

void GraphicsDevice::Shutdown()
{
    if (context_) {
        context_->ClearState();
        context_->Flush();
    }
    context_.Reset();

#ifdef _DEBUG
    if (debugLayer_ && device_) {
        // 解放漏れの診断。device 自身が Summary に載るのは正常
        Microsoft::WRL::ComPtr<ID3D11Debug> debug;
        if (SUCCEEDED(device_.As(&debug))) {
            debug->ReportLiveDeviceObjects(D3D11_RLDO_SUMMARY | D3D11_RLDO_IGNORE_INTERNAL);
        }
    }
#endif

    device_.Reset();
}

} // namespace mye
