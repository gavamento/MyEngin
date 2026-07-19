#pragma once
#include <vector>

#include "Engine/Core/Log.h"

namespace mye {

// ログ / シェーダエラー / ホットリロード通知の表示 (engine_spec.md 9 章)
class ConsoleWindow {
public:
    void OnImGui();

private:
    std::vector<LogEntry> entries_;
    uint64_t cursor_ = 0;
    bool showTrace_ = false;
    bool showInfo_ = true;
    bool showWarn_ = true;
    bool showError_ = true;
    bool autoScroll_ = true;
};

} // namespace mye
