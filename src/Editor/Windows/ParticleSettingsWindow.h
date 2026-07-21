#pragma once
#include "Engine/Engine/EngineLoop.h"

namespace mye {

// パーティクル設定 (engine_spec.md 9 章 / 7.4)。
// バックエンド切替ラジオボタン、比較モード起動、SIMD トグル、更新時間表示
class ParticleSettingsWindow {
public:
    bool open = true; // 閉じる / 再表示 (タブ [x] と Window メニューに連動)
    void OnImGui(EngineContext& ctx);
};

} // namespace mye
