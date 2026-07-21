#pragma once
#include <string>

#include "nlohmann/json.hpp"

#include "Editor/AssetPreviewCache.h"
#include "Editor/EditorSettings.h"
#include "Editor/EditorToolbar.h"
#include "Editor/PlayModeController.h"
#include "Editor/Selection.h"
#include "Editor/ShortcutHub.h"
#include "Editor/StatusBar.h"
#include "Editor/ToastCenter.h"
#include "Editor/Undo/UndoStack.h"
#include "Editor/Windows/AnimationWindow.h"
#include "Editor/Windows/AnimatorControllerWindow.h"
#include "Editor/Windows/AssetBrowserWindow.h"
#include "Editor/Windows/BuildSettingsWindow.h"
#include "Editor/Windows/ConsoleWindow.h"
#include "Editor/Windows/GameViewWindow.h"
#include "Editor/Windows/HierarchyWindow.h"
#include "Editor/Windows/InspectorWindow.h"
#include "Editor/Windows/ParticleSettingsWindow.h"
#include "Editor/Windows/ProfilerWindow.h"
#include "Editor/Windows/ProjectSettingsWindow.h"
#include "Editor/Windows/SceneViewWindow.h"
#include "Editor/Windows/SearchWindow.h"
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
    // 未保存変更ガード (M27b): dirty なら確認モーダルを経由して実行する
    enum class PendingAction { None, NewScene, OpenScene, Exit };

    void DrawMainMenuBar(EngineContext& ctx);
    void HandleShortcuts(EngineContext& ctx);
    void SaveCurrentScene(EngineContext& ctx);
    bool IsSceneDirty() const { return undo_.StateSerial() != savedStateSerial_; }
    void RequestGuardedAction(EngineContext& ctx, PendingAction action);
    void ExecuteAction(EngineContext& ctx, PendingAction action);
    void DrawSaveConfirmModal(EngineContext& ctx);
    void UpdateWindowTitle(EngineContext& ctx);
    void PollReloadToasts(EngineContext& ctx);
    // 選択エンティティ操作 (グローバルショートカット。全て Undo 統合)
    nlohmann::json GatherSelectionSubtrees(EngineContext& ctx); // 選択のサブツリー群 (fileId 重複除去)
    void DuplicateSelection(EngineContext& ctx);
    void CopySelection(EngineContext& ctx);
    void CutSelection(EngineContext& ctx);
    void PasteClipboard(EngineContext& ctx);
    void DeleteSelection(EngineContext& ctx);
    void SetupDockLayout(unsigned int dockspaceId);
    void SaveSceneAs(EngineContext& ctx);
    bool OpenScene(EngineContext& ctx); // true = 実際にロードした (キャンセル時 false)

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
    AnimatorControllerWindow animatorController_;
    SearchWindow search_;
    ProjectSettingsWindow projectSettings_;
    BuildSettingsWindow buildSettings_;
    AssetPreviewCache preview_; // AssetBrowser のメッシュ/プレハブサムネイル (M27d)

    nlohmann::json clipboard_; // コピー/カットしたサブツリー群 (SubtreeToJson 形式の配列)
    std::wstring scenePath_;
    bool rebuildDockLayout_ = false;
    bool showStats_ = true;

    // ---- フィードバック層 (M27b) / ツールバー (M27c) ----
    EditorToolbar toolbar_;
    StatusBar statusBar_;
    ToastCenter toasts_;
    std::string projectName_;              // マニフェストの name (レガシー起動時は空)
    uint64_t savedStateSerial_ = 0;        // 最後に保存/ロードした時点の UndoStack::StateSerial
    PendingAction pendingAction_ = PendingAction::None;
    bool openSaveConfirm_ = false;         // 確認モーダルを開くリクエスト
    bool closeRequested_ = false;          // ウィンドウ × ボタン (WM_CLOSE を横取り)
    std::wstring baseTitle_;               // タイトルバー原文 (dirty で " *" を付ける)
    bool titleDirtyShown_ = false;
    uint32_t lastDllVersion_ = 0;          // GameLogic ホットリロードのトースト検知用
    uint64_t lastReloadCount_ = 0;         // アセットホットリロードのトースト検知用
};

} // namespace mye
