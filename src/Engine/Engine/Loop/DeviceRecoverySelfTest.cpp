//====================================================================================
//                          DeviceRecoverySelfTest.cpp
//  MyEngine/ 秋田蓮音                                                      10/07/2026
//                                          デバイス復旧 (連続消失の制限・参照数ゲート) の回帰テスト
//====================================================================================
#include "Engine/Engine/Loop/DeviceRecoverySelfTest.h"

#include <d3d11.h>
#include <wrl/client.h>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Engine/Loop/DeviceRecovery.h"
#include "Engine/Renderer/Device/GraphicsDevice.h"

namespace mye {

namespace {

// 旧デバイスの子オブジェクトとして握らせるための小さな定数バッファ
bool CreateProbeBuffer(GraphicsDevice& device, Microsoft::WRL::ComPtr<ID3D11Buffer>& out)
{
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 16;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    return SUCCEEDED(device.Device()->CreateBuffer(&bd, nullptr, out.GetAddressOf()));
}

} // namespace

bool RunDeviceRecoverySelfTest()
{
    MYE_LOG_INFO("==== DeviceRecovery self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    // ---- 連続消失の窓判定: 2 回までは復旧し、窓内の 3 回目で諦める。窓の外へ出れば数え直す ----
    {
        DeviceLostLimiter limiter;
        check(limiter.RecordLoss(0.0), "1st loss is recoverable");
        check(limiter.RecordLoss(10.0), "2nd loss within the window is recoverable");
        check(!limiter.RecordLoss(20.0), "3rd loss within the window is refused");
        check(limiter.RecordLoss(20.0 + kDeviceLostWindowSec + 1.0),
              "losses older than the window are forgotten");
    }

    // ---- 旧デバイスの子オブジェクトを握ったままだとゲート不合格 (復旧せず旧デバイスを残す) ----
    GraphicsDevice device;
    if (!device.Init(true)) {
        check(false, "WARP device creation");
        return false;
    }
    {
        Microsoft::WRL::ComPtr<ID3D11Buffer> leaked;
        check(CreateProbeBuffer(device, leaked), "probe buffer creation");
        ID3D11Device* oldDevice = device.Device();
        const DeviceRecycleResult r = RecycleDevice(device, 1, 0);
        check(r.status == DeviceRecycleStatus::StaleDeviceRefs && r.staleRefs > kExpectedExternalDeviceRefs,
              "a leaked child object fails the gate");
        check(device.Device() == oldDevice, "the old device is kept when the gate fails");
        leaked.Reset();
    }

    // ---- 誰も握っていなければ同種 (WARP) の新デバイスへ作り直せる ----
    {
        const DeviceRecycleResult r = RecycleDevice(device, 3, 0);
        check(r.status == DeviceRecycleStatus::Ok && r.attempts == 1, "clean recycle succeeds on the first attempt");
        check(device.Device() != nullptr && device.IsWarp(), "the recreated device keeps the WARP kind");
        Microsoft::WRL::ComPtr<ID3D11Buffer> fresh;
        check(CreateProbeBuffer(device, fresh), "the recreated device is usable");
    }
    device.Shutdown();

    MYE_LOG_INFO("DeviceRecovery self test: %s", failCount == 0 ? "ALL PASS" : "FAILED");
    return failCount == 0;
}

} // namespace mye
