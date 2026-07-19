#include "Engine/Renderer/GraphicsDevice.h"

#include <vector>

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
        // ピッキングの R32_UINT ターゲットでブレンド不可を誤検知する既知の偽陽性
        // (ID 376: RENDERTARGET_DOESNOTSUPPORT_BLENDING) を抑制する。
        // UINT RT はそもそもブレンドしないので実害は無い
        Microsoft::WRL::ComPtr<ID3D11InfoQueue> infoQueue;
        if (SUCCEEDED(device_.As(&infoQueue))) {
            D3D11_MESSAGE_ID hide[] = { static_cast<D3D11_MESSAGE_ID>(376) };
            D3D11_INFO_QUEUE_FILTER filter = {};
            filter.DenyList.NumIDs = 1;
            filter.DenyList.pIDList = hide;
            infoQueue->AddStorageFilterEntries(&filter);
        }
    }
#endif

    if (FAILED(hr)) {
        MYE_LOG_ERROR("D3D11CreateDevice failed (hr=0x%08lX)", hr);
        return false;
    }

    MYE_LOG_INFO("D3D11 device created (FL 11_0, debug layer: %s)", debugLayer_ ? "on" : "off");
    return true;
}

void GraphicsDevice::PumpDebugMessages()
{
#ifdef _DEBUG
    if (!debugLayer_ || !device_) {
        return;
    }
    Microsoft::WRL::ComPtr<ID3D11InfoQueue> queue;
    if (FAILED(device_.As(&queue))) {
        return;
    }
    const UINT64 count = queue->GetNumStoredMessages();
    for (UINT64 i = debugMsgCursor_; i < count; ++i) {
        SIZE_T len = 0;
        queue->GetMessage(i, nullptr, &len);
        if (len == 0) {
            continue;
        }
        std::vector<uint8_t> storage(len);
        auto* msg = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
        if (SUCCEEDED(queue->GetMessage(i, msg, &len))) {
            if (msg->Severity == D3D11_MESSAGE_SEVERITY_ERROR
                || msg->Severity == D3D11_MESSAGE_SEVERITY_CORRUPTION) {
                MYE_LOG_ERROR("[d3d] %.*s", static_cast<int>(msg->DescriptionByteLength),
                              msg->pDescription);
            } else if (msg->Severity == D3D11_MESSAGE_SEVERITY_WARNING) {
                MYE_LOG_WARN("[d3d] %.*s", static_cast<int>(msg->DescriptionByteLength),
                             msg->pDescription);
            }
        }
    }
    debugMsgCursor_ = count;
#endif
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
