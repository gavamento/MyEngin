#pragma once
#include <string>

#include "Editor/PlayModeController.h"
#include "Editor/Selection.h"
#include "Editor/Windows/ConsoleWindow.h"
#include "Editor/Windows/GameViewWindow.h"
#include "Editor/Windows/HierarchyWindow.h"
#include "Editor/Windows/InspectorWindow.h"
#include "Editor/Windows/SceneViewWindow.h"
#include "Engine/Engine/EngineLoop.h"
#include "Engine/Engine/GameObject.h"

namespace mye {

// エディタ本体 (engine_spec.md 9 章)。dockspace + 各ウィンドウ + Play 制御
class EditorApp : public IEngineApp {
public:
    void OnStart(EngineContext& ctx) override;
    void OnTick(EngineContext& ctx) override;
    void OnRenderViews(EngineContext& ctx) override;
    void OnImGui(EngineContext& ctx) override;

    bool saveSceneOnStart = false; // --save-scene-on-start (シーンリロード検証用)

private:
    void RegisterDemoResources(EngineContext& ctx); // メッシュ/マテリアル/モデル登録 (毎回)
    void BuildDemoEntities(EngineContext& ctx);     // エンティティ構築 (シーンファイルが無い時のみ)
    void DrawMainMenuBar(EngineContext& ctx);
    void SetupDockLayout(unsigned int dockspaceId);
    void SaveSceneAs(EngineContext& ctx);
    void OpenScene(EngineContext& ctx);

    Selection selection_;
    PlayModeController playMode_;
    HierarchyWindow hierarchy_;
    InspectorWindow inspector_;
    ConsoleWindow console_;
    SceneViewWindow sceneView_;
    GameViewWindow gameView_;

    GameObject spinner_; // デモ用 (Play 中のみ回転)
    float spinYaw_ = 0.0f;
    std::wstring scenePath_;
    bool rebuildDockLayout_ = false;
    bool showStats_ = true;
};

} // namespace mye
