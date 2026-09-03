#pragma once
#include <string>

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

    // 共有される設定 (assets\project_settings.json / assets\input\actions.json) に
    // 未保存の編集があるか (M66d、spec §4.1 の S6)。
    // ★対象外にしているもの:
    //     * レンダリングパスのラジオ … そもそも永続化されない (spec が明示的に除外)
    //     * editor_settings.json      … <project>\.mye\ = 個人設定でチームと共有しない
    //   窓を一度も開いていなければ nullptr / 未 Load = false (この窓でしか編集できない)
    bool HasUnsavedChanges() const;

private:
    void UpdateKeyCapture(EngineContext& ctx);
    void DrawInputSection(EngineContext& ctx);

    // キー捕捉 (M51d)。1 = アクションへキー追加 / 2 = 軸 posKey / 3 = 軸 negKey。
    // エンジンのスナップショット差分で新規押下 VK を拾う (ImGui のフォーカスと無関係)
    int captureKind_ = 0;
    int captureIndex_ = -1;
    // 未保存判定に使う参照 (寿命は EngineContext と同じ)
    InputActions* inputActions_ = nullptr;
    std::wstring assetsRoot_;
    InputSnapshot lastInput_ = {}; // 前フレームのスナップショット (押下エッジ検出用)
};

} // namespace mye
