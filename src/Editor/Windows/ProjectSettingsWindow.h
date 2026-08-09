#pragma once
#include "Engine/Engine/EngineLoop.h"

namespace mye {

struct EditorSettings;
class ShortcutHub;

// Project / Editor 設定ウィンドウ (engine_spec.md 9 章、M15)。
// editor_settings.json の編集 (外部エディタコマンド / スナップ量 / グリッド)、
// レンダリングパス既定、入力アクションマップ (M51d)、ショートカット一覧表示。
class ProjectSettingsWindow {
public:
    bool open = false;
    void OnImGui(EngineContext& ctx, EditorSettings& settings, ShortcutHub& shortcuts);

private:
    void UpdateKeyCapture(EngineContext& ctx);
    void DrawInputSection(EngineContext& ctx);

    // キー捕捉 (M51d)。1 = アクションへキー追加 / 2 = 軸 posKey / 3 = 軸 negKey。
    // エンジンのスナップショット差分で新規押下 VK を拾う (ImGui のフォーカスと無関係)
    int captureKind_ = 0;
    int captureIndex_ = -1;
    InputSnapshot lastInput_ = {}; // 前フレームのスナップショット (押下エッジ検出用)
};

} // namespace mye
