#pragma once
#include <string>
#include <vector>

#include "Engine/Core/Diagnostics/Log.h"

namespace mye {

// ログ / シェーダエラー / ホットリロード通知の表示 (engine_spec.md 9 章)。
// M11: 検索 / Warn・Error カウント / 重複 Collapse / ダブルクリックで file:line ジャンプ
class ConsoleWindow {
public:
    bool open = true; // 閉じる / 再表示 (タブ [x] と Window メニューに連動)
    void OnImGui();

    // ダブルクリックされたログ行のソース位置を取り出す (false = 要求なし)。
    // EditorApp が OpenScriptInIde で開く (外部エディタの起動待ちを持つのが EditorApp のため)
    bool TakeOpenSourceRequest(std::wstring& file, int& line)
    {
        if (pendingSourceFile_.empty()) {
            return false;
        }
        file.swap(pendingSourceFile_);
        pendingSourceFile_.clear();
        line = pendingSourceLine_;
        return true;
    }

private:
    std::wstring pendingSourceFile_;
    int pendingSourceLine_ = 0;

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
