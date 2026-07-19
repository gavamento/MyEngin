#include "Editor/EditorApp.h"

#include <filesystem>

#include <Windows.h>
#include <commdlg.h>

#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Engine/HotReload/DllReloader.h"
#include "Engine/Engine/HotReload/ReloadHub.h"
#include "Engine/Engine/ModelLoader.h"
#include "Engine/Engine/Script/ScriptHost.h"
#include "Engine/Platform/Win32Window.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Engine/SceneSerializer.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/ShaderManager.h"

#include "imgui.h"
#include "imgui_internal.h" // DockBuilder API

namespace mye {

void EditorApp::OnStart(EngineContext& ctx)
{
    ctx.shaders->Load("forward_lit");
    scenePath_ = ctx.assetsRoot + L"\\scenes\\main.scene.json";
    ctx.reloadHub->SetActiveScenePath(scenePath_);
    rebuildDockLayout_ = !std::filesystem::exists("imgui.ini");

    // リソース (メッシュ/マテリアル/モデル) は毎回登録する。
    // シーンファイルは AssetID しか持たないため、実体の登録は起動側の責務
    // (パスベースの完全なアセット解決は AssetManager の将来拡張)
    RegisterDemoResources(ctx);
    if (std::filesystem::exists(scenePath_)) {
        SceneSerializer::LoadFromFile(*ctx.scene, scenePath_);
    } else {
        BuildDemoEntities(ctx);
    }
    if (saveSceneOnStart) {
        std::filesystem::create_directories(std::filesystem::path(scenePath_).parent_path());
        SceneSerializer::SaveToFile(*ctx.scene, scenePath_);
    }
    if (autoPlay) {
        playMode_.Play(*ctx.scene);
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
    sceneView_.OnRenderViews(ctx);
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

    DrawMainMenuBar(ctx);
    hierarchy_.OnImGui(ctx, selection_);
    inspector_.OnImGui(ctx, selection_);
    console_.OnImGui();
    sceneView_.OnImGui(ctx);
    gameView_.OnImGui(ctx);
    particleSettings_.OnImGui(ctx);

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
            selection_.entity = kNullEntity;
            ctx.scene->Clear();
        }
        if (ImGui::MenuItem("Open Scene...")) {
            OpenScene(ctx);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Save Scene", "Ctrl+S")) {
            std::filesystem::create_directories(std::filesystem::path(scenePath_).parent_path());
            SceneSerializer::SaveToFile(*ctx.scene, scenePath_);
        }
        if (ImGui::MenuItem("Save Scene As...")) {
            SaveSceneAs(ctx);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) {
            ctx.requestExit = true;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Stats", nullptr, &showStats_);
        if (ImGui::MenuItem("Reset Layout")) {
            rebuildDockLayout_ = true;
        }
        ImGui::EndMenu();
    }

    // ---- Play / Pause / Step (メニューバー中央) ----
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() * 0.5f - 80.0f);
    const PlayState state = playMode_.State();
    if (state == PlayState::Editing) {
        if (ImGui::Button("Play")) {
            selection_.entity = kNullEntity; // 復元で EntityID が変わるため選択解除
            playMode_.Play(*ctx.scene);
        }
    } else {
        if (ImGui::Button("Stop")) {
            selection_.entity = kNullEntity;
            playMode_.Stop(*ctx.scene);
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

    // Ctrl+S ショートカット
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S)) {
        std::filesystem::create_directories(std::filesystem::path(scenePath_).parent_path());
        SceneSerializer::SaveToFile(*ctx.scene, scenePath_);
    }
}

void EditorApp::SetupDockLayout(unsigned int dockspaceId)
{
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->Size);

    ImGuiID center = dockspaceId;
    const ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.18f, nullptr, &center);
    const ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.26f, nullptr, &center);
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.28f, nullptr, &center);
    ImGuiID rightBottom = right;
    const ImGuiID rightTop = ImGui::DockBuilderSplitNode(rightBottom, ImGuiDir_Up, 0.7f, nullptr, &rightBottom);

    ImGui::DockBuilderDockWindow("Hierarchy", left);
    ImGui::DockBuilderDockWindow("Inspector", rightTop);
    ImGui::DockBuilderDockWindow("Stats", rightBottom);
    ImGui::DockBuilderDockWindow("Particle Settings", rightBottom);
    ImGui::DockBuilderDockWindow("Console", bottom);
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
        SceneSerializer::SaveToFile(*ctx.scene, scenePath_);
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
        selection_.entity = kNullEntity;
        if (SceneSerializer::LoadFromFile(*ctx.scene, path)) {
            scenePath_ = path;
            ctx.reloadHub->SetActiveScenePath(scenePath_);
        }
    }
}

void EditorApp::RegisterDemoResources(EngineContext& ctx)
{
    RenderResources& res = *ctx.resources;
    const AssetID shader = AssetID{ HashStr("forward_lit") };
    const AssetID white = res.textures.White();
    res.meshes.Cube();

    auto makeMat = [&](const char* name, float r, float g, float b) {
        Material m;
        m.shader = shader;
        m.texture = white;
        m.baseColor = { r, g, b, 1.0f };
        return res.materials.Register(name, m);
    };
    // 地面はファイルテクスチャ (assets\textures\test.png) — テクスチャホットリロードの実演用
    {
        Material m;
        m.shader = shader;
        const AssetID groundTex = res.textures.LoadFile(ctx.assetsRoot + L"\\textures\\test.png");
        m.texture = groundTex.IsNull() ? white : groundTex;
        m.baseColor = groundTex.IsNull() ? DirectX::XMFLOAT4{ 0.45f, 0.47f, 0.50f, 1.0f }
                                         : DirectX::XMFLOAT4{ 1.0f, 1.0f, 1.0f, 1.0f };
        res.materials.Register("mat_ground", m);
    }
    makeMat("mat_arm", 0.85f, 0.55f, 0.20f);
    makeMat("mat_red", 0.85f, 0.30f, 0.28f);
    makeMat("mat_green", 0.35f, 0.75f, 0.40f);
    makeMat("mat_blue", 0.30f, 0.50f, 0.85f);
    makeMat("mat_yellow", 0.90f, 0.80f, 0.30f);

    // glTF のメッシュ/マテリアル/テクスチャを登録 (エンティティは作らない)。
    // 保存済みシーンをロードする場合でも AssetID の実体が揃うようにする
    ModelLoader::ReloadMeshes(res, *ctx.shaders, ctx.assetsRoot + L"\\models\\BoxTextured.glb");
}

void EditorApp::BuildDemoEntities(EngineContext& ctx)
{
    Scene& s = *ctx.scene;
    RenderResources& res = *ctx.resources;

    const AssetID cube = res.meshes.Cube();
    const AssetID matGround = AssetID{ HashStr("mat_ground") };
    const AssetID matArm = AssetID{ HashStr("mat_arm") };
    const AssetID palette[4] = {
        AssetID{ HashStr("mat_red") },
        AssetID{ HashStr("mat_green") },
        AssetID{ HashStr("mat_blue") },
        AssetID{ HashStr("mat_yellow") },
    };

    GameObject camera = s.CreateGameObject("Main Camera");
    camera.AddComponent<CameraComponent>();
    camera.SetLocalPosition(0.0f, 7.0f, -16.0f);
    camera.SetLocalRotationEuler(18.0f, 0.0f, 0.0f);

    GameObject sun = s.CreateGameObject("Sun");
    sun.AddComponent<LightComponent>();
    sun.SetLocalRotationEuler(50.0f, -30.0f, 0.0f);

    GameObject ground = s.CreateGameObject("Ground");
    ground.SetLocalPosition(0.0f, -1.0f, 0.0f);
    ground.SetLocalScale(40.0f, 0.5f, 40.0f);
    {
        auto* mr = ground.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = matGround;
    }

    GameObject spinner = s.CreateGameObject("Spinner");
    spinner.SetLocalPosition(0.0f, 1.5f, 0.0f);
    for (int a = 0; a < 20; ++a) {
        char name[32];
        snprintf(name, sizeof(name), "Arm_%02d", a);
        GameObject arm = s.CreateGameObject(name);
        arm.SetParent(spinner);
        arm.SetLocalRotationEuler(0.0f, a * (360.0f / 20.0f), 0.0f);
        arm.SetLocalScale(0.4f, 0.4f, 0.4f);
        {
            auto* mr = arm.AddComponent<MeshRendererComponent>();
            mr->mesh = cube;
            mr->material = matArm;
        }
        for (int j = 0; j < 25; ++j) {
            snprintf(name, sizeof(name), "Cube_%02d_%02d", a, j);
            GameObject leaf = s.CreateGameObject(name);
            leaf.SetParent(arm);
            const float dist = (2.0f + 0.9f * j) / 0.4f;
            const float wave = 1.0f + 0.8f * ((j % 5) - 2) * 0.3f;
            leaf.SetLocalPosition(dist, wave, 0.0f);
            leaf.SetLocalScale(0.75f, 0.75f, 0.75f);
            auto* mr = leaf.AddComponent<MeshRendererComponent>();
            mr->mesh = cube;
            mr->material = palette[(a + j) % 4];
        }
    }

    GameObject model = ModelLoader::Load(s, res, *ctx.shaders,
                                         ctx.assetsRoot + L"\\models\\BoxTextured.glb");
    if (model) {
        model.SetLocalPosition(0.0f, 1.5f, 0.0f);
        model.SetLocalScale(2.0f, 2.0f, 2.0f);
    }

    // ---- パーティクルエミッタ (M5 デモ: 中央の炎) ----
    GameObject fire = s.CreateGameObject("Fire");
    fire.SetLocalPosition(4.0f, 0.0f, 0.0f);
    auto* emitter = fire.AddComponent<ParticleEmitterComponent>(); // 既定値 = 上向きコーン炎
    if (perfRate > 0.0f) {
        emitter->rate = perfRate; // 性能計測モード
        emitter->maxParticles = (perfMax > 0) ? perfMax : 100000;
        emitter->lifetimeMin = 1.2f;
        emitter->lifetimeMax = 1.8f;
        emitter->speedMin = 3.0f;
        emitter->speedMax = 8.0f;
        emitter->coneAngleDeg = 60.0f;
    }

    // ---- GameLogic.dll のスクリプトをアタッチ (DLL 未ロードならスキップ) ----
    const ComponentTypeId rotator = ComponentRegistry::Get().FindByName("Rotator");
    if (rotator != kInvalidComponentType) {
        s.GetWorld().AddComponentRaw(spinner.Id(), rotator);
    }
    const ComponentTypeId player = ComponentRegistry::Get().FindByName("PlayerController");
    if (player != kInvalidComponentType && model) {
        s.GetWorld().AddComponentRaw(model.Id(), player);
    }
    // 生成/破棄を続けるスクリプト (リプレイ検証に構造変更を含める)
    const ComponentTypeId spawner = ComponentRegistry::Get().FindByName("Spawner");
    if (spawner != kInvalidComponentType) {
        s.GetWorld().AddComponentRaw(fire.Id(), spawner);
    }
}

} // namespace mye
