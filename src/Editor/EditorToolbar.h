#pragma once
#include "Editor/PlayModeController.h"

namespace mye {

struct EngineContext;
struct Selection;
class UndoStack;
class SceneViewWindow;

// メニューバー直下の独立ツールバー (M27c、Unity のメインツールバー相当)。
// 左 = ギズモ操作 (SceneView の状態をアクセサ経由で共有)、中央 = Play/Pause/Step/Stop、
// 右 = Render Path。Play 中は背景をオレンジ系に着色し、ビューポート全周に枠を描く。
// BeginViewportSideBar (Up) で WorkArea を確保するため DockSpaceOverViewport より前に呼ぶこと
class EditorToolbar {
public:
    void OnImGui(EngineContext& ctx, PlayModeController& playMode, Selection& selection,
                 UndoStack& undo, SceneViewWindow& sceneView);
};

} // namespace mye
