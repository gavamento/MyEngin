//====================================================================================
//                          DeviceRecovery.cpp
//  MyEngine/ 秋田蓮音                                                      10/07/2026
//                                          デバイス消失からの復旧手順 (連続消失の制限・旧デバイス参照数ゲート)
//====================================================================================
#include "Engine/Engine/Loop/DeviceRecovery.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Renderer/Device/GraphicsDevice.h"

namespace mye {

bool DeviceLostLimiter::RecordLoss(double nowSec)
{
    times_.erase(std::remove_if(times_.begin(), times_.end(),
                                [nowSec](double t) { return nowSec - t > kDeviceLostWindowSec; }),
                 times_.end());
    times_.push_back(nowSec);
    return static_cast<int>(times_.size()) < kDeviceLostMaxInWindow;
}

DeviceRecycleResult RecycleDevice(GraphicsDevice& device, int attempts, int retryMs)
{
    DeviceRecycleResult result;
    device.ReleaseContext();
    const int staleRefs = device.CountExternalDeviceRefs();
    MYE_LOG_INFO("[device] old device external references: %d (allowed %d)", staleRefs,
                 kExpectedExternalDeviceRefs);
    if (staleRefs > kExpectedExternalDeviceRefs) {
        // 古いリソースを掴んだまま新デバイスで描くと未定義動作になるので、復旧を打ち切る
        MYE_LOG_ERROR("[device] old device is still referenced by %d object(s) (expected %d)",
                      staleRefs, kExpectedExternalDeviceRefs);
        device.ReportLiveObjectsDetail();
        result.status = DeviceRecycleStatus::StaleDeviceRefs;
        result.staleRefs = staleRefs;
        return result;
    }
    device.ReleaseDevice();
    for (int i = 0; i < attempts; ++i) {
        if (i > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(retryMs));
        }
        result.attempts = i + 1;
        if (device.Recreate()) {
            return result;
        }
    }
    result.status = DeviceRecycleStatus::RecreateFailed;
    return result;
}

} // namespace mye
