#include "Editor/EditorApp.h"

#include <algorithm>
#include <filesystem>
#include <vector>

#include <Windows.h>
#include <commdlg.h>
#include <shellapi.h>

#include "Editor/AssetOps.h"
#include "Editor/CreateMenu.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Engine/DemoContent.h"
#include "Engine/Engine/HotReload/DllReloader.h"
#include "Engine/Engine/HotReload/ReloadHub.h"
#include "Engine/Engine/ModelLoader.h"
#include "Engine/Engine/Prefab.h"
#include "Engine/Engine/Project.h"
#include "Engine/Engine/Script/ScriptHost.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Platform/Win32Window.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Engine/SceneSerializer.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/ShaderManager.h"

#include "imgui.h"
#include "imgui_internal.h" // DockBuilder API
#include "ImGuizmo/ImGuizmo.h"

namespace mye {

void EditorApp::OnStart(EngineContext& ctx)
{
    ctx.shaders->Load("forward_lit");
    // 設定の置き場 (M26): プロジェクト起動時は <project>\.mye\、レガシー時は従来の assets\ 直下
    const std::wstring settingsDir = ctx.projectRoot.empty()
        ? ctx.assetsRoot
        : ctx.projectRoot + L"\\" + kProjectLocalDir;
    settings_.Load(settingsDir);
    // 名前付きレイアウト: ImGui ini + パネル開閉フラグを <settingsDir>\layouts に保存/復元
    layouts_.Init(settingsDir + L"\\layouts",
                  { { "Hierarchy", &hierarchy_.open },
                    { "Inspector", &inspector_.open },
                    { "Console", &console_.open },
                    { "Scene", &sceneView_.open },
                    { "Game", &gameView_.open },
                    { "Assets", &assetBrowser_.open },
                    { "Animation", &animation_.open },
                    { "Animator", &animatorController_.open },
                    { "Search", &search_.open },
                    { "Profiler", &profiler_.open },
                    { "Particle Settings", &particleSettings_.open },
                    { "Sound Generator", &soundGen_.open },
                    { "Audio Mixer", &audioMixer_.open },
                    { "Project Settings", &projectSettings_.open },
                    { "Build Settings", &buildSettings_.open },
                    { "Stats", &showStats_ } });
    if (!sceneOverride.empty()) {
        scenePath_ = sceneOverride;
    } else {
        scenePath_ = ctx.assetsRoot + L"\\scenes\\main.scene.json";
        ProjectManifest manifest; // ブートシーンはマニフェスト優先 (M26)
        if (!ctx.projectRoot.empty() && LoadProjectManifest(ctx.projectRoot, manifest)) {
            scenePath_ = ProjectBootScenePath(ctx.projectRoot, manifest);
        }
    }
    ctx.reloadHub->SetActiveScenePath(scenePath_);
    rebuildDockLayout_ = !std::filesystem::exists(ctx.imguiIniPath);

    // リソース (メッシュ/マテリアル/モデル) は毎回登録する。
    // シーンファイルは AssetID しか持たないため、実体の登録は起動側の責務
    // (パスベースの完全なアセット解決は AssetManager の将来拡張)
    RegisterDemoContent(ctx);
    RegisterAssetLibraries(ctx); // シーンロード前に .prefab/.anim を登録 (参照解決のため)
    if (std::filesystem::exists(scenePath_)) {
        SceneSerializer::LoadFromFile(*ctx.scene, scenePath_);
    } else {
        BuildDemoScene(ctx, perfRate, perfMax);
    }
    if (saveSceneOnStart) {
        std::filesystem::create_directories(std::filesystem::path(scenePath_).parent_path());
        SceneSerializer::SaveToFile(*ctx.scene, scenePath_);
    }
    if (!selectName.empty()) {
        ctx.scene->GetWorld().ApplyStructuralChanges();
        if (GameObject g = ctx.scene->Find(selectName)) {
            selection_.SelectOnly(ctx.scene->EnsureFileId(g.Id()));
            MYE_LOG_INFO("selected '%s' (fileId %llu)", selectName.c_str(),
                         static_cast<unsigned long long>(selection_.primary));
        }
    }
    if (autoPlay) {
        playMode_.Play(*ctx.scene);
    }
    if (startDeferred) {
        ctx.renderPath = ctx.renderPathDeferred;
    }

    // ---- フィードバック層の初期化 (M27b) ----
    {
        ProjectManifest manifest;
        if (!ctx.projectRoot.empty() && LoadProjectManifest(ctx.projectRoot, manifest)) {
            projectName_ = manifest.name;
        }
    }
    savedStateSerial_ = undo_.StateSerial(); // ロード直後 = clean
    {
        wchar_t title[256] = {};
        GetWindowTextW(static_cast<HWND>(ctx.window->Hwnd()), title, 256);
        baseTitle_ = title;
    }
    lastDllVersion_ = ctx.dllReloader->Version();
    lastReloadCount_ = ctx.reloadHub->ReloadCount();
    // エクスプローラーからのファイルドロップを受理する (エディタ専用機能なのでここで有効化。
    // ゲーム実行系は DragAcceptFiles を呼ばないため対象外のまま)
    DragAcceptFiles(static_cast<HWND>(ctx.window->Hwnd()), TRUE);
    // 管理者権限で起動されたエディタ (管理者 VS からの F5 等) は UIPI が中権限エクスプローラー
    // からの WM_DROPFILES を遮断し、ドラッグが禁止カーソルになる。シェル D&D に必要な
    // 3 メッセージ (WM_DROPFILES / WM_COPYDATA / WM_COPYGLOBALDATA) のみ下位権限から許可する。
    // 非昇格時は実質 no-op。エディタ限定の緩和で、Runtime.exe には適用しない
    {
        const HWND hwnd = static_cast<HWND>(ctx.window->Hwnd());
        constexpr UINT kCopyGlobalData = 0x0049; // WM_COPYGLOBALDATA (ヘッダ未定義の内部メッセージ)
        ChangeWindowMessageFilterEx(hwnd, WM_DROPFILES, MSGFLT_ALLOW, nullptr);
        ChangeWindowMessageFilterEx(hwnd, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
        ChangeWindowMessageFilterEx(hwnd, kCopyGlobalData, MSGFLT_ALLOW, nullptr);
    }
    // × ボタン (WM_CLOSE) の横取り + WM_DROPFILES の受信。ハンドラは DefWindowProc より先に走る
    ctx.window->AddMsgHandler([this](void*, uint32_t msg, uint64_t wparam, int64_t, int64_t& result) {
        if (msg == WM_CLOSE) {
            closeRequested_ = true;
            result = 0;
            return true;
        }
        if (msg == WM_DROPFILES) {
            HDROP drop = reinterpret_cast<HDROP>(wparam);
            PendingFileDrop pd;
            POINT pt = {};
            pd.inClientArea = (DragQueryPoint(drop, &pt) != FALSE); // クライアント座標
            pd.clientX = static_cast<float>(pt.x);
            pd.clientY = static_cast<float>(pt.y);
            const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n; ++i) {
                const UINT len = DragQueryFileW(drop, i, nullptr, 0); // 終端を除く必要長
                std::vector<wchar_t> buf(len + 1);
                DragQueryFileW(drop, i, buf.data(), len + 1);
                pd.paths.emplace_back(buf.data(), len);
            }
            DragFinish(drop); // 分岐より先に必ず解放 (シェル側の HDROP リーク防止)
            pendingFileDrops_.push_back(std::move(pd));
            result = 0;
            return true;
        }
        return false;
    });

    MYE_LOG_INFO("EditorApp started (%u entities)", ctx.scene->GetWorld().AliveCount());
}

void EditorApp::OnTick(EngineContext& ctx)
{
    // ゲームロジック (GameLogic.dll のスクリプト) は Play 中のみ実行される
    ctx.simulateScripts = playMode_.ConsumeSimulateTick();
}

void EditorApp::OnRenderViews(EngineContext& ctx)
{
    sceneView_.OnRenderViews(ctx, selection_);
    gameView_.OnRenderViews(ctx);
    preview_.OnRenderViews(ctx); // アセットサムネイル生成 (D3D 描画はこのフェーズのみ)
}

void EditorApp::OnImGui(EngineContext& ctx)
{
    // レイアウトロードは **どの ImGui::Begin よりも前** に実行する — LoadIniSettingsFromDisk が
    // 既存ウィンドウの DockId を差し替え、同フレームの Begin で新ドックに再バインドされる
    layouts_.ApplyPendingLoad();

    // エクスプローラー D&D の消費はグリッド描画 (assetBrowser_) より先に行い、
    // インポートしたファイルが同フレームで表示されるようにする
    ProcessPendingFileDrops(ctx);

    // サイドバー (メニュー → ツールバー → ステータスバー) が WorkArea を先に確保し、
    // 残りに DockSpace を敷く — この順序を同一フレーム内で守ること (逆順だと 1 フレームちらつく)
    DrawMainMenuBar(ctx);
    if (toolbar_.OnImGui(ctx, playMode_, selection_, undo_, sceneView_, layouts_)) {
        rebuildDockLayout_ = true; // レイアウトの「リセット (既定)」
    }
    statusBar_.OnImGui(ctx, projectName_, scenePath_, IsSceneDirty(), playMode_.State(),
                       &console_.open);

    const ImGuiID dockspaceId =
        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_None);
    if (rebuildDockLayout_) {
        SetupDockLayout(dockspaceId);
        rebuildDockLayout_ = false;
    }
    ImGuizmo::BeginFrame(); // ImGui NewFrame 後・ギズモ使用前に 1 回

    hierarchy_.OnImGui(ctx, selection_, undo_);
    inspector_.OnImGui(ctx, selection_, undo_);
    console_.OnImGui(settings_.externalEditorCmd);
    sceneView_.OnImGui(ctx, selection_, undo_, settings_);
    gameView_.OnImGui(ctx);
    particleSettings_.OnImGui(ctx);
    profiler_.OnImGui(ctx);
    assetBrowser_.OnImGui(ctx, selection_, undo_, settings_.externalEditorCmd, preview_);
    // AssetBrowser で .scene.json がダブルクリックされたら未保存変更ガード経由で開く
    if (std::wstring p = assetBrowser_.TakePendingOpenScene(); !p.empty()) {
        pendingOpenScenePath_ = std::move(p);
        RequestGuardedAction(ctx, PendingAction::OpenSceneAsset);
    }
    animation_.OnImGui(ctx, selection_, undo_);
    animatorController_.OnImGui(ctx, selection_);
    search_.OnImGui(ctx, selection_);
    projectSettings_.OnImGui(ctx, settings_, shortcuts_);
    buildSettings_.OnImGui(ctx);
    // 書き出し先は AssetBrowser の表示中フォルダ (未初期化なら窓側が <assets>\audio に落とす)
    soundGen_.OnImGui(ctx, assetBrowser_.CurrentDir());
    // Asset Browser で .mixer.json がダブルクリックされたら Audio Mixer を開く (M45d)
    if (assetBrowser_.TakeOpenMixerRequest()) {
        audioMixer_.FocusOnActive();
    }
    audioMixer_.OnImGui(ctx);

    // ピッキング自動テスト (--pick-test): 指定フレームでビュー中心を選択できるか検証
    if (pickTestFrame >= 0 && static_cast<int64_t>(ctx.frameIndex) == pickTestFrame) {
        if (sceneView_.PickAtCenter(ctx, selection_)) {
            GameObject g = ctx.scene->FindByFileId(selection_.primary);
            MYE_LOG_INFO("PICK TEST: PASS -- hit '%s' (fileId %llu)", g ? g.Name() : "?",
                         static_cast<unsigned long long>(selection_.primary));
        } else {
            MYE_LOG_ERROR("PICK TEST: FAIL -- no entity at view center");
        }
    }

    if (showStats_) {
        if (ImGui::Begin("Stats", &showStats_)) {
            const ImGuiIO& io = ImGui::GetIO();
            ImGui::Text("FPS: %.1f (%.3f ms)", io.Framerate, 1000.0f / io.Framerate);
            ImGui::Text("Frame: %llu / Tick: %llu",
                        static_cast<unsigned long long>(ctx.frameIndex),
                        static_cast<unsigned long long>(ctx.tickIndex));
            ImGui::Text("Entities: %u", ctx.scene->GetWorld().AliveCount());
            const char* stateName = "Editing";
            if (playMode_.State() == PlayState::Playing) { stateName = "Playing"; }
            if (playMode_.State() == PlayState::Paused) { stateName = "Paused"; }
            ImGui::Text("Play state: %s", stateName);
            ImGui::Text("GameLogic: %s (v%u, %u scripts)",
                        ctx.scriptHost->IsLoaded() ? "loaded" : "not loaded",
                        ctx.dllReloader->Version(), ctx.scriptHost->ScriptTypeCount());
        }
        ImGui::End();
    }

    // ---- フィードバック層 (M27b): 最前面に描く ----
    if (closeRequested_) {
        closeRequested_ = false;
        RequestGuardedAction(ctx, PendingAction::Exit);
    }
    DrawSaveConfirmModal(ctx);
    PollReloadToasts(ctx);
    UpdateWindowTitle(ctx);
    toasts_.OnImGui();
}

void EditorApp::DrawMainMenuBar(EngineContext& ctx)
{
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }
    if (ImGui::BeginMenu("File")) {
        // New/Open/Exit は未保存変更ガード経由 (M27b。dirty なら確認モーダル)
        if (ImGui::MenuItem("New Scene")) {
            RequestGuardedAction(ctx, PendingAction::NewScene);
        }
        if (ImGui::MenuItem("Open Scene...")) {
            RequestGuardedAction(ctx, PendingAction::OpenScene);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Save Scene", shortcuts_.Label(Shortcut::Save))) {
            SaveCurrentScene(ctx);
        }
        if (ImGui::MenuItem("Save Scene As...")) {
            SaveSceneAs(ctx);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Build Settings...")) {
            buildSettings_.open = true;
        }
        if (ImGui::MenuItem("Project Settings...")) {
            projectSettings_.open = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) {
            RequestGuardedAction(ctx, PendingAction::Exit);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Stats", nullptr, &showStats_);
        if (ImGui::BeginMenu("Render Path")) {
            // 実行時切替 (M6.5)。描画のみの変更なのでリプレイ一貫性には影響しない
            const bool isForward = (ctx.renderPath == ctx.renderPathForward);
            if (ImGui::MenuItem("Forward", nullptr, isForward)) {
                ctx.renderPath = ctx.renderPathForward;
            }
            if (ImGui::MenuItem("Deferred", nullptr, !isForward)) {
                ctx.renderPath = ctx.renderPathDeferred;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Rendering")) {
            // 描画専用トグル (M40d)。sim/hash 非影響
            ImGui::MenuItem("Shadows", nullptr, &ctx.renderSystem->enableShadows);
            ImGui::MenuItem("SSAO (Deferred)", nullptr, &ctx.renderSystem->enableSsao);
            ImGui::MenuItem("GPU Instancing", nullptr, &ctx.renderSystem->enableInstancing);
            ImGui::MenuItem("Post FX", nullptr, &ctx.renderSystem->enablePostFx);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("RT Debug (Deferred)")) {
            // M46b: BVH 検証用の可視化。off なら BVH の構築も GPU 転送も走らない
            int& mode = ctx.renderSystem->rtDebugMode;
            if (ImGui::MenuItem("Off", nullptr, mode == 0)) {
                mode = 0;
            }
            if (ImGui::MenuItem("BVH Heatmap", nullptr, mode == 1)) {
                mode = 1;
            }
            if (ImGui::MenuItem("Hit Normals", nullptr, mode == 2)) {
                mode = 2;
            }
            if (ImGui::MenuItem("Instance ID", nullptr, mode == 3)) {
                mode = 3;
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Reset Layout")) {
            rebuildDockLayout_ = true;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        const bool editing = playMode_.State() == PlayState::Editing;
        if (ImGui::MenuItem("Undo", shortcuts_.Label(Shortcut::Undo), false,
                            editing && undo_.CanUndo())) {
            undo_.Undo(*ctx.scene, selection_);
        }
        if (ImGui::MenuItem("Redo", shortcuts_.Label(Shortcut::Redo), false,
                            editing && undo_.CanRedo())) {
            undo_.Redo(*ctx.scene, selection_);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("GameObject")) {
        DrawCreateMenuItems(ctx, selection_, undo_); // parent 省略 = ルート生成
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Window")) {
        // 各パネルの表示トグル (閉じたパネルはここから再表示)
        ImGui::MenuItem("Hierarchy", nullptr, &hierarchy_.open);
        ImGui::MenuItem("Inspector", nullptr, &inspector_.open);
        ImGui::MenuItem("Console", nullptr, &console_.open);
        ImGui::MenuItem("Scene", nullptr, &sceneView_.open);
        ImGui::MenuItem("Game", nullptr, &gameView_.open);
        ImGui::MenuItem("Assets", nullptr, &assetBrowser_.open);
        ImGui::MenuItem("Animation", nullptr, &animation_.open);
        ImGui::MenuItem("Animator", nullptr, &animatorController_.open);
        ImGui::MenuItem("Search", nullptr, &search_.open);
        ImGui::MenuItem("Profiler", nullptr, &profiler_.open);
        ImGui::MenuItem("Particle Settings", nullptr, &particleSettings_.open);
        ImGui::MenuItem("Sound Generator", nullptr, &soundGen_.open);
        ImGui::MenuItem("Audio Mixer", nullptr, &audioMixer_.open);
        ImGui::Separator();
        ImGui::MenuItem("Project Settings", nullptr, &projectSettings_.open);
        ImGui::MenuItem("Build Settings", nullptr, &buildSettings_.open);
        ImGui::EndMenu();
    }

    // Play/Pause/Step は M27c でツールバー (EditorToolbar) へ移設
    ImGui::EndMainMenuBar();

    HandleShortcuts(ctx);
}

void EditorApp::HandleShortcuts(EngineContext& ctx)
{
    // テキスト入力中はエディタショートカットを無効化 (ImGui のテキスト編集/Undo を優先)
    if (ImGui::GetIO().WantTextInput) {
        return;
    }
    if (shortcuts_.Pressed(Shortcut::Save)) {
        SaveCurrentScene(ctx);
    }
    // Undo/Redo・編集操作は編集モードのみ (Play 中の変更は Stop で破棄されるため)
    if (playMode_.State() == PlayState::Editing) {
        if (shortcuts_.Pressed(Shortcut::Undo)) {
            undo_.Undo(*ctx.scene, selection_);
        }
        if (shortcuts_.Pressed(Shortcut::Redo)
            || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z)) {
            undo_.Redo(*ctx.scene, selection_);
        }
        if (shortcuts_.Pressed(Shortcut::Duplicate)) {
            DuplicateSelection(ctx);
        }
        if (shortcuts_.Pressed(Shortcut::Copy)) {
            CopySelection(ctx);
        }
        if (shortcuts_.Pressed(Shortcut::Cut)) {
            CutSelection(ctx);
        }
        if (shortcuts_.Pressed(Shortcut::Paste)) {
            PasteClipboard(ctx);
        }
        if (shortcuts_.Pressed(Shortcut::Delete)) {
            DeleteSelection(ctx);
        }
    }
}

void EditorApp::SaveCurrentScene(EngineContext& ctx)
{
    std::filesystem::create_directories(std::filesystem::path(scenePath_).parent_path());
    if (SceneSerializer::SaveToFile(*ctx.scene, scenePath_)) {
        settings_.lastScenePath = WideToUtf8(scenePath_);
        settings_.Save();
        savedStateSerial_ = undo_.StateSerial(); // この状態が保存済み基準になる (M27b)
        toasts_.Notify(LogLevel::Info,
                       "シーンを保存しました: "
                           + WideToUtf8(std::filesystem::path(scenePath_).filename().wstring()));
    } else {
        toasts_.Notify(LogLevel::Error, "シーンの保存に失敗しました");
    }
}

nlohmann::json EditorApp::GatherSelectionSubtrees(EngineContext& ctx)
{
    nlohmann::json out = nlohmann::json::array();
    std::vector<uint64_t> seen;
    for (uint64_t fid : selection_.ids) {
        GameObject g = ctx.scene->FindByFileId(fid);
        if (!g) {
            continue;
        }
        nlohmann::json sub = SceneSerializer::SubtreeToJson(*ctx.scene, g.Id());
        for (auto& item : sub) {
            const uint64_t f = item.value("fileId", 0ull);
            if (f != 0 && std::find(seen.begin(), seen.end(), f) != seen.end()) {
                continue; // 親と子を同時選択した場合の重複を除く
            }
            if (f != 0) {
                seen.push_back(f);
            }
            out.push_back(std::move(item));
        }
    }
    return out;
}

void EditorApp::DuplicateSelection(EngineContext& ctx)
{
    if (selection_.Empty()) {
        return;
    }
    const nlohmann::json subtrees = GatherSelectionSubtrees(ctx);
    if (subtrees.empty()) {
        return;
    }
    undo_.BeginRecord("Duplicate", selection_);
    const std::vector<uint64_t> newRoots = SceneSerializer::CloneSubtree(*ctx.scene, subtrees);
    ctx.scene->GetWorld().ApplyStructuralChanges();
    selection_.Clear();
    for (uint64_t f : newRoots) {
        selection_.Add(f);
        undo_.CaptureAfter(*ctx.scene, f);
    }
    undo_.EndRecord(selection_);
}

void EditorApp::CopySelection(EngineContext& ctx)
{
    clipboard_ = GatherSelectionSubtrees(ctx);
}

void EditorApp::CutSelection(EngineContext& ctx)
{
    CopySelection(ctx);
    DeleteSelection(ctx);
}

void EditorApp::PasteClipboard(EngineContext& ctx)
{
    if (!clipboard_.is_array() || clipboard_.empty()) {
        return;
    }
    undo_.BeginRecord("Paste", selection_);
    const std::vector<uint64_t> newRoots = SceneSerializer::CloneSubtree(*ctx.scene, clipboard_);
    ctx.scene->GetWorld().ApplyStructuralChanges();
    selection_.Clear();
    for (uint64_t f : newRoots) {
        selection_.Add(f);
        undo_.CaptureAfter(*ctx.scene, f);
    }
    undo_.EndRecord(selection_);
}

void EditorApp::DeleteSelection(EngineContext& ctx)
{
    if (selection_.Empty()) {
        return;
    }
    undo_.BeginRecord("Delete", selection_);
    for (uint64_t fid : selection_.ids) {
        GameObject g = ctx.scene->FindByFileId(fid);
        if (g) {
            undo_.CaptureBefore(*ctx.scene, fid);
            ctx.scene->GetWorld().DestroyEntity(g.Id());
        }
    }
    ctx.scene->GetWorld().ApplyStructuralChanges();
    selection_.Clear();
    undo_.EndRecord(selection_); // CaptureAfter 無し → destroyed 扱い
}

void EditorApp::SetupDockLayout(unsigned int dockspaceId)
{
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->Size);

    ImGuiID center = dockspaceId;
    const ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.18f, nullptr, &center);
    const ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.26f, nullptr, &center);
    ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.28f, nullptr, &center);
    const ImGuiID bottomRight = ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Right, 0.4f, nullptr, &bottom);
    ImGuiID rightBottom = right;
    const ImGuiID rightTop = ImGui::DockBuilderSplitNode(rightBottom, ImGuiDir_Up, 0.7f, nullptr, &rightBottom);

    ImGui::DockBuilderDockWindow("Hierarchy", left);
    ImGui::DockBuilderDockWindow("Search", left);
    ImGui::DockBuilderDockWindow("Inspector", rightTop);
    ImGui::DockBuilderDockWindow("Stats", rightBottom);
    ImGui::DockBuilderDockWindow("Particle Settings", rightBottom);
    ImGui::DockBuilderDockWindow("Sound Generator", rightBottom);
    ImGui::DockBuilderDockWindow("Audio Mixer", bottom);
    ImGui::DockBuilderDockWindow("Profiler", rightBottom);
    ImGui::DockBuilderDockWindow("Console", bottom);
    ImGui::DockBuilderDockWindow("Assets", bottomRight);
    ImGui::DockBuilderDockWindow("Animation", bottom);
    ImGui::DockBuilderDockWindow("Animator", bottom);
    ImGui::DockBuilderDockWindow("Scene", center);
    ImGui::DockBuilderDockWindow("Game", center);
    ImGui::DockBuilderFinish(dockspaceId);
}

void EditorApp::SaveSceneAs(EngineContext& ctx)
{
    wchar_t path[MAX_PATH] = L"main.scene.json";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = static_cast<HWND>(ctx.window->Hwnd());
    ofn.lpstrFilter = L"Scene (*.scene.json)\0*.scene.json\0All (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    const std::wstring initialDir = ctx.assetsRoot + L"\\scenes";
    ofn.lpstrInitialDir = initialDir.c_str();
    ofn.Flags = OFN_OVERWRITEPROMPT;
    if (GetSaveFileNameW(&ofn)) {
        scenePath_ = path;
        ctx.reloadHub->SetActiveScenePath(scenePath_);
        SaveCurrentScene(ctx);
    }
}

bool EditorApp::OpenScene(EngineContext& ctx)
{
    wchar_t path[MAX_PATH] = L"";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = static_cast<HWND>(ctx.window->Hwnd());
    ofn.lpstrFilter = L"Scene (*.scene.json)\0*.scene.json\0All (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    const std::wstring initialDir = ctx.assetsRoot + L"\\scenes";
    ofn.lpstrInitialDir = initialDir.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) {
        return false; // キャンセル (シーンは無変更なので dirty 状態も保持)
    }
    return LoadSceneFromPath(ctx, path);
}

// ファイルダイアログを経ない共通ロード経路 (OpenScene と AssetBrowser ダブルクリックが使う)
bool EditorApp::LoadSceneFromPath(EngineContext& ctx, const std::wstring& path)
{
    selection_.Clear();
    undo_.ClearAll();
    if (!SceneSerializer::LoadFromFile(*ctx.scene, path)) {
        toasts_.Notify(LogLevel::Error,
                       "シーンを開けませんでした: "
                           + WideToUtf8(std::filesystem::path(path).filename().wstring()));
        return false;
    }
    scenePath_ = path;
    ctx.reloadHub->SetActiveScenePath(scenePath_);
    settings_.lastScenePath = WideToUtf8(scenePath_);
    settings_.Save();
    return true;
}

// ---- 未保存変更ガード + フィードバック層 (M27b) ----

void EditorApp::RequestGuardedAction(EngineContext& ctx, PendingAction action)
{
    if (!IsSceneDirty()) {
        ExecuteAction(ctx, action);
        return;
    }
    pendingAction_ = action;
    openSaveConfirm_ = true;
}

void EditorApp::ExecuteAction(EngineContext& ctx, PendingAction action)
{
    switch (action) {
    case PendingAction::NewScene:
        selection_.Clear();
        undo_.ClearAll();
        ctx.scene->Clear();
        savedStateSerial_ = undo_.StateSerial(); // 空シーン = clean
        break;
    case PendingAction::OpenScene:
        if (OpenScene(ctx)) {
            savedStateSerial_ = undo_.StateSerial(); // ロード直後 = clean
        }
        break;
    case PendingAction::OpenSceneAsset:
        if (LoadSceneFromPath(ctx, pendingOpenScenePath_)) {
            savedStateSerial_ = undo_.StateSerial(); // ロード直後 = clean
        }
        pendingOpenScenePath_.clear();
        break;
    case PendingAction::Exit:
        ctx.requestExit = true;
        break;
    default:
        break;
    }
}

void EditorApp::DrawSaveConfirmModal(EngineContext& ctx)
{
    if (openSaveConfirm_) {
        openSaveConfirm_ = false;
        ImGui::OpenPopup("未保存の変更");
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
                                   vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("未保存の変更", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    ImGui::Text("シーンに未保存の変更があります。保存しますか？");
    ImGui::TextDisabled("%s", WideToUtf8(scenePath_).c_str());
    ImGui::Spacing();
    const PendingAction action = pendingAction_;
    if (ImGui::Button("保存する", ImVec2(110, 0))) {
        SaveCurrentScene(ctx);
        pendingAction_ = PendingAction::None;
        ImGui::CloseCurrentPopup();
        if (!IsSceneDirty()) { // 保存成功時のみ続行 (失敗はトーストで通知済み)
            ExecuteAction(ctx, action);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("保存しない", ImVec2(110, 0))) {
        pendingAction_ = PendingAction::None;
        ImGui::CloseCurrentPopup();
        ExecuteAction(ctx, action);
    }
    ImGui::SameLine();
    if (ImGui::Button("キャンセル", ImVec2(110, 0))) {
        pendingAction_ = PendingAction::None;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void EditorApp::UpdateWindowTitle(EngineContext& ctx)
{
    const bool dirty = IsSceneDirty();
    if (dirty == titleDirtyShown_ || baseTitle_.empty()) {
        return;
    }
    titleDirtyShown_ = dirty;
    const std::wstring title = dirty ? baseTitle_ + L" *" : baseTitle_;
    SetWindowTextW(static_cast<HWND>(ctx.window->Hwnd()), title.c_str());
}

void EditorApp::ProcessPendingFileDrops(EngineContext& ctx)
{
    if (pendingFileDrops_.empty()) {
        return;
    }
    for (PendingFileDrop& pd : pendingFileDrops_) {
        if (!pd.inClientArea || pd.paths.empty()) {
            continue; // タイトルバー等クライアント領域外へのドロップ
        }
        if (!assetBrowser_.open || assetBrowser_.CurrentDir().empty() ||
            !assetBrowser_.IsClientPosInPanel(pd.clientX, pd.clientY)) {
            // 黙って捨てると「無反応」に見えて原因調査が難しいため誘導トーストを出す
            toasts_.Notify(LogLevel::Warn, "インポートは Assets パネル上にドロップしてください");
            continue; // Assets パネル上のドロップのみインポートする (誤操作防止)
        }
        const ImportResult r = ImportExternalPaths(ctx, pd.paths, assetBrowser_.CurrentDir());
        if (r.imported > 0) {
            toasts_.Notify(LogLevel::Info,
                           std::to_string(r.imported) + " 件のアセットをインポートしました");
        }
        if (r.failed > 0) {
            toasts_.Notify(LogLevel::Error,
                           std::to_string(r.failed) + " 件のインポートに失敗しました");
        }
        if (r.imported == 0 && r.failed == 0 && r.skipped > 0) {
            toasts_.Notify(LogLevel::Warn,
                           "インポートをスキップしました (対象外またはドロップ元と同じフォルダ)");
        }
    }
    pendingFileDrops_.clear();
}

void EditorApp::PollReloadToasts(EngineContext& ctx)
{
    const uint32_t dllVersion = ctx.dllReloader->Version();
    if (dllVersion != lastDllVersion_) {
        lastDllVersion_ = dllVersion;
        toasts_.Notify(LogLevel::Info, "GameLogic をホットリロードしました (v"
                                           + std::to_string(dllVersion) + ")");
    }
    const uint64_t reloads = ctx.reloadHub->ReloadCount();
    if (reloads != lastReloadCount_) {
        lastReloadCount_ = reloads;
        toasts_.Notify(LogLevel::Info, "アセットをホットリロードしました");
    }
}

} // namespace mye
