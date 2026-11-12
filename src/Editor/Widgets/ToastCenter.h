#pragma once
#include <string>
#include <vector>

#include "Engine/Core/Diagnostics/Log.h"

namespace mye {

// トースト通知 (M27b)。呼び出し側が渡す矩形の右下へ積む。
//   - 明示発火: Notify() — シーン保存 / ホットリロード完了などの操作フィードバック
//   - 自動収集: ログの Warn/Error を独立 cursor で拾う。同一フレームに大量発生した場合は
//     「N 件」に集約する (Empty テンプレのアセット欠落 WARN 洪水対策)
// 描画は OnImGui() を全ウィンドウの後 (最前面) に呼ぶ。
// アンカーは「重ねてよい領域」の右下 (エディタは中央ドック = Scene / Game)。作業領域の右下に
// 固定すると、そこへドックした窓 (Stats / Profiler など) の文字へ半透明のまま重なって読めなくなる
class ToastCenter {
public:
    void Notify(LogLevel level, std::string text, float seconds = 4.0f);
    void OnImGui(float anchorRight, float anchorBottom);

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
