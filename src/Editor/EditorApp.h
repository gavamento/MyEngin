#pragma once
#include <memory>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

#include "Editor/AssetPreviewCache.h"
#include "Editor/EditorSettings.h"
#include "Editor/EditorToolbar.h"
#include "Editor/LayoutManager.h"
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
#include "Editor/Windows/AudioMixerWindow.h"
#include "Editor/Windows/SoundGenWindow.h"
#include "Engine/Engine/EngineLoop.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Engine/TransformSystem.h"

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
    bool rtShowcase = false;       // --rt-demo (M46i: コーネル箱のショーケースを構築)
    bool partsShowcase = false;    // --parts-demo (M48g: 部位追従のリプレイ被覆シーン)
    // --edit-actor PATH (M48k): 起動直後にミニシーン編集モードで開く。
    // 編集モードの入口はダブルクリックだけで自動検証できないため、既存の検証フラグ
    // (--select / --pick-test / --parts-demo) と同じ流儀で口を開けてある
    std::wstring editActorPath;

private:
    // 未保存変更ガード (M27b): dirty なら確認モーダルを経由して実行する
    // OpenSceneAsset = AssetBrowser ダブルクリック (pendingOpenScenePath_ をダイアログなしでロード)
    // ExitActorEdit = ミニシーン編集の終了 (M48k)。未保存なら同じ確認モーダルを通す
    enum class PendingAction { None, NewScene, OpenScene, OpenSceneAsset, Exit, ExitActorEdit };

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
    bool LoadSceneFromPath(EngineContext& ctx, const std::wstring& path); // ダイアログなし共通経路
    void ProcessPendingFileDrops(EngineContext& ctx); // エクスプローラー D&D (OnImGui 冒頭で消費)

    // ---- ミニシーン編集モード (M48k) ----
    // 構成アセット (.actor.json / .prefab.json) を**専用の Scene** に展開して編集する。
    // Hierarchy / Inspector / SceneView / Undo / Selection はすべて引数駆動なので、
    // 描画の間だけ `ctx.scene` をミニシーンへ差し替えるだけで丸ごと使い回せる。
    // ScriptHost / ManagedHost / ReloadHub が Init 時に captured した `Scene*` と、
    // EngineLoop がローカルに持つ Scene には**一切触れない** = tick 経路は無傷
    struct ActorEdit {
        Scene scene; // アセットの実体 (fileId == localId)
        std::wstring path;
        std::string name;
        bool actorFormat = true; // 読み込んだ宣言キーを維持する (.prefab.json を勝手に移行しない)
        // ---- 外側 (通常シーン) の編集状態の退避 ----
        // ★編集中は selection_ / undo_ / savedStateSerial_ の**中身をアセット側に入れ替える**。
        //   こうすると EditorApp 内に 49 箇所ある selection_/undo_ 参照を 1 つも書き換えずに
        //   「今開いている文書」を丸ごと切り替えられる (ショートカット・複製・削除・
        //   ダーティ判定・タイトルバーが全部そのまま効く)
        Selection outerSelection;
        UndoStack outerUndo;
        uint64_t outerSavedSerial = 0;
    };
    void OpenActorEdit(EngineContext& ctx, const std::wstring& path);
    void SaveActorEdit(EngineContext& ctx);
    void CloseActorEdit(EngineContext& ctx);
    bool InActorEdit() const { return actorEdit_ != nullptr; }

    std::unique_ptr<ActorEdit> actorEdit_; // 非 null = 編集モード中
    TransformSystem actorTransform_;       // ミニシーンの WorldMatrix はエンジン tick が回さない

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
    SoundGenWindow soundGen_;
    AudioMixerWindow audioMixer_;
    AssetPreviewCache preview_; // AssetBrowser のメッシュ/プレハブサムネイル (M27d)

    nlohmann::json clipboard_; // コピー/カットしたサブツリー群 (SubtreeToJson 形式の配列)
    std::wstring scenePath_;
    bool rebuildDockLayout_ = false;
    bool showStats_ = true;
    LayoutManager layouts_; // 名前付きレイアウト (ツールバー右端のドロップダウン)

    // ---- フィードバック層 (M27b) / ツールバー (M27c) ----
    EditorToolbar toolbar_;
    StatusBar statusBar_;
    ToastCenter toasts_;
    std::string projectName_;              // マニフェストの name (レガシー起動時は空)
    uint64_t savedStateSerial_ = 0;        // 最後に保存/ロードした時点の UndoStack::StateSerial
    PendingAction pendingAction_ = PendingAction::None;
    std::wstring pendingOpenScenePath_;    // OpenSceneAsset の対象 (AssetBrowser ダブルクリック)
    bool openSaveConfirm_ = false;         // 確認モーダルを開くリクエスト
    bool closeRequested_ = false;          // ウィンドウ × ボタン (WM_CLOSE を横取り)
    std::wstring baseTitle_;               // タイトルバー原文 (dirty で " *" を付ける)
    bool titleDirtyShown_ = false;
    uint32_t lastDllVersion_ = 0;          // GameLogic ホットリロードのトースト検知用
    uint64_t lastReloadCount_ = 0;         // アセットホットリロードのトースト検知用

    // ---- エクスプローラー D&D インポート ----
    struct PendingFileDrop {
        std::vector<std::wstring> paths;
        float clientX = 0.0f; // ドロップ位置 (クライアント座標)
        float clientY = 0.0f;
        bool inClientArea = false; // DragQueryPoint の戻り値 (false = タイトルバー等)
    };
    std::vector<PendingFileDrop> pendingFileDrops_; // WM_DROPFILES → 次の OnImGui で消費
};

} // namespace mye
