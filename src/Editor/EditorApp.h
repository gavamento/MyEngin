#pragma once
#include <string>

#include "nlohmann/json.hpp"

#include "Editor/EditorSettings.h"
#include "Editor/PlayModeController.h"
#include "Editor/Selection.h"
#include "Editor/ShortcutHub.h"
#include "Editor/Undo/UndoStack.h"
#include "Editor/Windows/AnimationWindow.h"
#include "Editor/Windows/AssetBrowserWindow.h"
#include "Editor/Windows/ConsoleWindow.h"
#include "Editor/Windows/GameViewWindow.h"
#include "Editor/Windows/HierarchyWindow.h"
#include "Editor/Windows/InspectorWindow.h"
#include "Editor/Windows/ParticleSettingsWindow.h"
#include "Editor/Windows/ProfilerWindow.h"
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
    bool startDeferred = false;    // --deferred (起動時から Deferred パス — 検証用)
    std::string selectName;        // --select NAME (起動時に名前でエンティティを選択 — ギズモ検証用)
    int pickTestFrame = -1;        // --pick-test (このフレームで中心をピッキングし PASS/FAIL ログ)
    std::wstring sceneOverride;    // --scene PATH (既定の main.scene.json の代わりに読むシーン)

private:
    void DrawMainMenuBar(EngineContext& ctx);
    void HandleShortcuts(EngineContext& ctx);
    void SaveCurrentScene(EngineContext& ctx);
    // 選択エンティティ操作 (グローバルショートカット。全て Undo 統合)
    nlohmann::json GatherSelectionSubtrees(EngineContext& ctx); // 選択のサブツリー群 (fileId 重複除去)
    void DuplicateSelection(EngineContext& ctx);
    void CopySelection(EngineContext& ctx);
    void CutSelection(EngineContext& ctx);
    void PasteClipboard(EngineContext& ctx);
    void DeleteSelection(EngineContext& ctx);
    void SetupDockLayout(unsigned int dockspaceId);
    void SaveSceneAs(EngineContext& ctx);
    void OpenScene(EngineContext& ctx);

    Selection selection_;
    UndoStack undo_;
    EditorSettings settings_;
    ShortcutHub shortcuts_;
    PlayModeController playMode_;
    HierarchyWindow hierarchy_;
    InspectorWindow inspector_;
    ConsoleWindow console_;
    SceneViewWindow sceneView_;
    GameViewWindow gameView_;
    ParticleSettingsWindow particleSettings_;
    ProfilerWindow profiler_;
    AssetBrowserWindow assetBrowser_;
    AnimationWindow animation_;

    nlohmann::json clipboard_; // コピー/カットしたサブツリー群 (SubtreeToJson 形式の配列)
    std::wstring scenePath_;
    bool rebuildDockLayout_ = false;
    bool showStats_ = true;
};

} // namespace mye
