#pragma once
#include "Engine/Engine/EngineLoop.h"

namespace mye {

// フレームタイム / フェーズ別時間 / パーティクル更新時間の表示 (engine_spec.md 9 章)
class ProfilerWindow {
public:
    bool open = true; // 閉じる / 再表示 (タブ [x] と Window メニューに連動)
    void OnImGui(EngineContext& ctx);

private:
    static constexpr int kHistory = 240;
    float frameHistory_[kHistory] = {};
    int cursor_ = 0;
};

} // namespace mye
