#pragma once
#include <string>
#include <vector>

#include "Engine/Core/Log.h"

namespace mye {

// ログ / シェーダエラー / ホットリロード通知の表示 (engine_spec.md 9 章)。
// M11: 検索 / Warn・Error カウント / 重複 Collapse / ダブルクリックで file:line ジャンプ
class ConsoleWindow {
public:
    bool open = true; // 閉じる / 再表示 (タブ [x] と Window メニューに連動)
    // externalEditorCmd: "code -g {file}:{line}" 形式 (ソースジャンプに使う)
    void OnImGui(const std::string& externalEditorCmd);

private:
    void JumpToSource(const LogEntry& e, const std::string& cmd);

    std::vector<LogEntry> entries_;
    uint64_t cursor_ = 0;
    char searchBuf_[128] = {};
    bool showTrace_ = false;
    bool showInfo_ = true;
    bool showWarn_ = true;
    bool showError_ = true;
    bool autoScroll_ = true;
    bool collapse_ = false;
};

} // namespace mye
