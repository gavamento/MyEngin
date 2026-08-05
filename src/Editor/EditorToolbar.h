#pragma once
#include "Editor/PlayModeController.h"

namespace mye {

struct EngineContext;
struct Selection;
class UndoStack;
class SceneViewWindow;
class LayoutManager;

// メニューバー直下の独立ツールバー (M27c、Unity のメインツールバー相当)。
// 左 = ギズモ操作 (SceneView の状態をアクセサ経由で共有)、中央 = Play/Pause/Step/Stop、
// 右 = Render Path + レイアウト切替。Play 中は背景をオレンジ系に着色し、全周に枠を描く。
// BeginViewportSideBar (Up) で WorkArea を確保するため DockSpaceOverViewport より前に呼ぶこと。
// 戻り値: レイアウトの「リセット (既定)」が選ばれたら true (呼び出し側が既定ドックを再構築)
// ミニシーン編集モード (M48k) のパンくず。name が非 null の間だけ表示され、
// **Play 系ボタンは無効化される** — 編集中の ctx.scene はアセット専用のミニシーンなので、
// Play (Save+Load でシーンを作り直す) を走らせると編集中の内容が壊れる
struct ActorEditBar {
    const char* name = nullptr; // 編集中のアセット名 (null = 通常モード)
    bool dirty = false;
    bool saveRequested = false; // 出力: 保存ボタンが押された
    bool exitRequested = false; // 出力: 「シーンへ戻る」が押された
};

class EditorToolbar {
public:
    bool OnImGui(EngineContext& ctx, PlayModeController& playMode, Selection& selection,
                 UndoStack& undo, SceneViewWindow& sceneView, LayoutManager& layouts,
                 ActorEditBar* actorEdit = nullptr);
};

} // namespace mye
