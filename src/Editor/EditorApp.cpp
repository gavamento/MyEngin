#include "Editor/EditorApp.h"

#include <algorithm>
#include <filesystem>
#include <vector>

#include <Windows.h>
#include <commdlg.h>

#include "Editor/CreateMenu.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Engine/DemoContent.h"
#include "Engine/Engine/HotReload/DllReloader.h"
#include "Engine/Engine/HotReload/ReloadHub.h"
#include "Engine/Engine/ModelLoader.h"
#include "Engine/Engine/Prefab.h"
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
    settings_.Load(ctx.assetsRoot);
    scenePath_ = sceneOverride.empty() ? (ctx.assetsRoot + L"\\scenes\\main.scene.json")
                                       : sceneOverride;
    ctx.reloadHub->SetActiveScenePath(scenePath_);
    rebuildDockLayout_ = !std::filesystem::exists("imgui.ini");

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
}

void EditorApp::OnImGui(EngineContext& ctx)
{
    const ImGuiID dockspaceId =
        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_None);
    if (rebuildDockLayout_) {
        SetupDockLayout(dockspaceId);
        rebuildDockLayout_ = false;
    }
    ImGuizmo::BeginFrame(); // ImGui NewFrame 後・ギズモ使用前に 1 回

    DrawMainMenuBar(ctx);
    hierarchy_.OnImGui(ctx, selection_, undo_);
    inspector_.OnImGui(ctx, selection_, undo_);
    console_.OnImGui(settings_.externalEditorCmd);
    sceneView_.OnImGui(ctx, selection_, undo_, settings_);
    gameView_.OnImGui(ctx);
    particleSettings_.OnImGui(ctx);
    profiler_.OnImGui(ctx);
    assetBrowser_.OnImGui(ctx, selection_, undo_, settings_.externalEditorCmd);
    animation_.OnImGui(ctx, selection_, undo_);
    animatorController_.OnImGui(ctx, selection_);
    search_.OnImGui(ctx, selection_);
    projectSettings_.OnImGui(ctx, settings_, shortcuts_);
    buildSettings_.OnImGui(ctx);

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
}

void EditorApp::DrawMainMenuBar(EngineContext& ctx)
{
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New Scene")) {
            selection_.Clear();
            undo_.ClearAll();
            ctx.scene->Clear();
        }
        if (ImGui::MenuItem("Open Scene...")) {
            OpenScene(ctx);
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
            ctx.requestExit = true;
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
        ImGui::Separator();
        ImGui::MenuItem("Project Settings", nullptr, &projectSettings_.open);
        ImGui::MenuItem("Build Settings", nullptr, &buildSettings_.open);
        ImGui::EndMenu();
    }

    // ---- Play / Pause / Step (メニューバー中央) ----
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() * 0.5f - 80.0f);
    const PlayState state = playMode_.State();
    if (state == PlayState::Editing) {
        if (ImGui::Button("Play")) {
            selection_.Clear(); // 復元で EntityID が変わるため選択解除
            undo_.BeginPlaySession();
            playMode_.Play(*ctx.scene);
        }
    } else {
        if (ImGui::Button("Stop")) {
            selection_.Clear();
            playMode_.Stop(*ctx.scene);
            undo_.EndPlaySession(); // Play 中に積まれた Undo エントリを破棄
        }
        ImGui::SameLine();
        if (ImGui::Button(state == PlayState::Paused ? "Resume" : "Pause")) {
            playMode_.TogglePause();
        }
        if (state == PlayState::Paused) {
            ImGui::SameLine();
            if (ImGui::Button("Step")) {
                playMode_.Step();
            }
        }
    }
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

void EditorApp::OpenScene(EngineContext& ctx)
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
    if (GetOpenFileNameW(&ofn)) {
        selection_.Clear();
        undo_.ClearAll();
        if (SceneSerializer::LoadFromFile(*ctx.scene, path)) {
            scenePath_ = path;
            ctx.reloadHub->SetActiveScenePath(scenePath_);
            settings_.lastScenePath = WideToUtf8(scenePath_);
            settings_.Save();
        }
    }
}


} // namespace mye
