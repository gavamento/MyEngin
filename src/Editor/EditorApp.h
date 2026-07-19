#pragma once
#include <string>

#include "Editor/PlayModeController.h"
#include "Editor/Selection.h"
#include "Editor/Windows/ConsoleWindow.h"
#include "Editor/Windows/GameViewWindow.h"
#include "Editor/Windows/HierarchyWindow.h"
#include "Editor/Windows/InspectorWindow.h"
#include "Editor/Windows/ParticleSettingsWindow.h"
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
    bool autoPlay = false;         // --autoplay (起動直後に Play。スクリプト検証用)
    float perfRate = 0.0f;         // --perf-rate N (>0 でデモエミッタの放出数を上書き — 性能計測用)
    int perfMax = 0;               // --perf-max N

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
    ParticleSettingsWindow particleSettings_;

    std::wstring scenePath_;
    bool rebuildDockLayout_ = false;
    bool showStats_ = true;
};

} // namespace mye
