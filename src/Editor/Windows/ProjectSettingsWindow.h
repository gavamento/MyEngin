#pragma once
#include "Engine/Engine/EngineLoop.h"

namespace mye {

struct EditorSettings;
class ShortcutHub;

// Project / Editor 設定ウィンドウ (engine_spec.md 9 章、M15)。
// editor_settings.json の編集 (外部エディタコマンド / スナップ量 / グリッド)、
// レンダリングパス既定、ショートカット一覧表示。
class ProjectSettingsWindow {
public:
    bool open = false;
    void OnImGui(EngineContext& ctx, EditorSettings& settings, ShortcutHub& shortcuts);
};

} // namespace mye
