#pragma once
#include <string>
#include <vector>

#include "Engine/Core/Log.h"

namespace mye {

// 右下のトースト通知 (M27b)。
//   - 明示発火: Notify() — シーン保存 / ホットリロード完了などの操作フィードバック
//   - 自動収集: ログの Warn/Error を独立 cursor で拾う。同一フレームに大量発生した場合は
//     「N 件」に集約する (Empty テンプレのアセット欠落 WARN 洪水対策)
// 描画は OnImGui() を全ウィンドウの後 (最前面) に呼ぶ
class ToastCenter {
public:
    void Notify(LogLevel level, std::string text, float seconds = 4.0f);
    void OnImGui();

private:
    struct Toast {
        LogLevel level;
        std::string text;
        float remaining;
        float height = 40.0f; // 前フレームの実測ウィンドウ高 (積み上げ位置の計算用)
    };

    void CollectLogs();

    std::vector<Toast> toasts_;
    uint64_t logCursor_ = 0;
};

} // namespace mye
