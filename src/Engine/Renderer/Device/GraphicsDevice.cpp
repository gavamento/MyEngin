#include "Engine/Renderer/Device/GraphicsDevice.h"

#include <iterator>
#include <vector>

#include <dxgi.h>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {

bool GraphicsDevice::Init(bool forceWarp)
{
    return CreateDevice(/*tryHardware=*/!forceWarp, /*tryWarp=*/true);
}

bool GraphicsDevice::Recreate()
{
    // 消失前と同じ種類だけを試す。WARP へ黙って落とすと「動いているが原因が分からない」状態になる
    return CreateDevice(/*tryHardware=*/!warp_, /*tryWarp=*/warp_);
}

bool GraphicsDevice::CreateDevice(bool tryHardware, bool tryWarp)
{
    debugMsgCursor_ = 0;
    const UINT baseFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    UINT debugFlag = 0;

    // デバッグレイヤは診断出力のみで挙動 (状態) に影響しないため、
    // Debug 構成限定でも一貫性ポリシー (spec 11.2 規則 1) に抵触しない
#ifdef _DEBUG
    debugFlag = D3D11_CREATE_DEVICE_DEBUG;
#endif

    const D3D_FEATURE_LEVEL requested = D3D_FEATURE_LEVEL_11_0;
    D3D_FEATURE_LEVEL obtained = {};

    auto create = [&](D3D_DRIVER_TYPE driver, UINT flags) {
        device_.Reset();
        context_.Reset();
        return D3D11CreateDevice(
            nullptr, driver, nullptr, flags,
            &requested, 1, D3D11_SDK_VERSION,
            device_.GetAddressOf(), &obtained, context_.GetAddressOf());
    };

    // 試行順 (M52b)。既定は HARDWARE → WARP: GPU の無い CI runner でも
    // デバイス生成で止まらず、同じ検証一式 (selftest / replay_verify) が回る。
    // sim は CPU 専用なのでリプレイのハッシュは採用ドライバに依らない。
    // forceWarp (--warp) はハードウェアを試さない — 「CI と同じ絵で撮る」明示指定なので、
    // 黙って GPU へ戻ると golden スクショが撮影機ごとに変わってしまう
    const D3D_DRIVER_TYPE candidates[] = { D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP };

    HRESULT hr = E_FAIL;
    for (int i = 0; i < static_cast<int>(std::size(candidates)); ++i) {
        const bool isWarp = (candidates[i] == D3D_DRIVER_TYPE_WARP);
        if (isWarp ? !tryWarp : !tryHardware) {
            continue;
        }
        UINT flags = baseFlags | debugFlag;
        hr = create(candidates[i], flags);
        if (FAILED(hr) && debugFlag != 0) {
            // Graphics Tools 未インストール環境ではデバッグレイヤ付き生成が失敗する
            MYE_LOG_WARN("D3D11 debug layer unavailable, falling back (hr=0x%08lX)", hr);
            flags = baseFlags;
            hr = create(candidates[i], flags);
        }
        if (SUCCEEDED(hr)) {
            debugLayer_ = (flags & D3D11_CREATE_DEVICE_DEBUG) != 0;
            warp_ = isWarp;
            break;
        }
        MYE_LOG_WARN("D3D11CreateDevice(%s) failed (hr=0x%08lX)",
                     isWarp ? "WARP" : "HARDWARE", hr);
    }

    if (FAILED(hr)) {
        MYE_LOG_ERROR("D3D11CreateDevice failed on all driver types (hr=0x%08lX)", hr);
        device_.Reset();
        context_.Reset();
        return false;
    }

#ifdef _DEBUG
    if (debugLayer_) {
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

    // 採用アダプタ名をログに残す (CI が本当に WARP を掴んだかを run ログで確認できる)
    adapterName_ = warp_ ? "WARP" : "unknown";
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    if (SUCCEEDED(device_.As(&dxgiDevice))) {
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        if (SUCCEEDED(dxgiDevice->GetAdapter(adapter.GetAddressOf()))) {
            DXGI_ADAPTER_DESC desc = {};
            if (SUCCEEDED(adapter->GetDesc(&desc))) {
                adapterName_ = WideToUtf8(desc.Description);
            }
        }
    }

    MYE_LOG_INFO("D3D11 device created: %s (%s, FL 11_0, debug layer: %s)",
                 adapterName_.c_str(), warp_ ? "WARP" : "hardware",
                 debugLayer_ ? "on" : "off");
    return true;
}

long GraphicsDevice::DeviceRemovedReason() const
{
    return device_ ? static_cast<long>(device_->GetDeviceRemovedReason()) : 0;
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

int GraphicsDevice::CountExternalDeviceRefs() const
{
    if (!device_) {
        return 0;
    }
    // AddRef の戻り値 = 現在の参照数。自分の device_ と今の AddRef の 2 本を引いたものが他者の保持数
    const ULONG withProbe = device_->AddRef();
    device_->Release();
    return static_cast<int>(withProbe) - 2;
}

void GraphicsDevice::ReportLiveObjectsDetail()
{
#ifdef _DEBUG
    if (!debugLayer_ || !device_) {
        return;
    }
    Microsoft::WRL::ComPtr<ID3D11Debug> debug;
    if (SUCCEEDED(device_.As(&debug))) {
        debug->ReportLiveDeviceObjects(D3D11_RLDO_DETAIL | D3D11_RLDO_IGNORE_INTERNAL);
    }
    PumpDebugMessages(); // レポートは InfoQueue へも積まれる = ログから読める
#endif
}

void GraphicsDevice::ReleaseContext()
{
    if (context_) {
        context_->ClearState();
        context_->Flush();
    }
    context_.Reset();
}

void GraphicsDevice::ReleaseDevice()
{
    device_.Reset();
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
