#pragma once
#include "Engine/Engine/EngineLoop.h"

namespace mye {

struct EditorSettings;

// パーティクル設定 (engine_spec.md 9 章 / 7.4)。
// バックエンド切替ラジオボタン、比較モード起動、SIMD トグル、更新時間表示。
//
// ★M66h: この窓のトグルは **project_settings.json を書かない**。
//   比較モード / SIMD / 比較オフセットは個人設定なので editor_settings.json (.mye\) へ、
//   バックエンドは**セッション上書きのみ** (プロジェクト既定にするのは Project Settings 窓)。
//   `--particle-backend` / `--particle-compare` が「書き戻さない」のと同じ扱いに揃えてある
class ParticleSettingsWindow {
public:
    bool open = true; // 閉じる / 再表示 (タブ [x] と Window メニューに連動)
    void OnImGui(EngineContext& ctx, EditorSettings& settings);
};

} // namespace mye
