//====================================================================================
//                          DeviceRecovery.h
//  MyEngine/ 秋田蓮音                                                      10/07/2026
//                                          デバイス消失からの復旧手順 (連続消失の制限・旧デバイス参照数ゲート)
//====================================================================================
#pragma once
#include <vector>

namespace mye {

class GraphicsDevice;

// 新デバイス作成の再試行 (TDR 直後はドライバの再初期化中で作成が失敗しうる)
constexpr int kDeviceRecreateAttempts = 10;
constexpr int kDeviceRecreateRetryMs = 500;
// 直近 kDeviceLostWindowSec 秒以内にこの回数目の消失が起きたら復旧を諦める
// (復旧直後にまた落ちるドライバ / シェーダの無限ループを避ける)
constexpr int kDeviceLostMaxInWindow = 3;
constexpr double kDeviceLostWindowSec = 60.0;
// 全所有者が手放した後に、旧デバイスを他者が握っていてよい参照数。超えたら取りこぼしがある
constexpr int kExpectedExternalDeviceRefs = 0;

// 連続消失の窓判定。時刻は呼び出し側が渡す (描画専用の実時間。sim には持ち込まない)
class DeviceLostLimiter {
public:
    // 消失を 1 回記録する。復旧を試みてよければ true、窓内で kDeviceLostMaxInWindow 回目なら false
    bool RecordLoss(double nowSec);

private:
    std::vector<double> times_;
};

enum class DeviceRecycleStatus { Ok, StaleDeviceRefs, RecreateFailed };

struct DeviceRecycleResult {
    DeviceRecycleStatus status = DeviceRecycleStatus::Ok;
    int staleRefs = 0; // StaleDeviceRefs: 旧デバイスを握ったままの参照数
    int attempts = 0;  // 新デバイスの作成を試した回数
};

// 旧デバイスを手放して同種の新デバイスを作る。手順: コンテキスト解放 → 旧デバイス参照数ゲート
// → 旧デバイス解放 → 再作成 (retryMs 間隔で最大 attempts 回)。
// ゲート不合格のときは旧デバイスを残したまま StaleDeviceRefs を返す (新デバイスは作らない)。
// 呼ぶ前に、device を握る全所有者 (スワップチェーン・ImGui・パス・アセット等) が手放し済みであること
DeviceRecycleResult RecycleDevice(GraphicsDevice& device, int attempts = kDeviceRecreateAttempts,
                                  int retryMs = kDeviceRecreateRetryMs);

} // namespace mye
