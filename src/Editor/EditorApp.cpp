#include "Editor/EditorApp.h"

#include <filesystem>

#include <Windows.h>
#include <commdlg.h>

#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Engine/ModelLoader.h"
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
    rebuildDockLayout_ = !std::filesystem::exists("imgui.ini");
    BuildTestScene(ctx);
    MYE_LOG_INFO("EditorApp started (%u entities)", ctx.scene->GetWorld().AliveCount());
}

void EditorApp::OnTick(EngineContext& ctx)
{
    if (!playMode_.ConsumeSimulateTick()) {
        return; // 編集中はシミュレーションを進めない
    }
    // ---- デモ用ゲームロジック (M4 で GameLogic.dll のスクリプトに移行する) ----
    if (!spinner_) {
        spinner_ = ctx.scene->Find("Spinner");
    }
    if (spinner_) {
        spinYaw_ += 20.0f * ctx.fixedDt;
        spinner_.SetLocalRotationEuler(0.0f, spinYaw_, 0.0f);
    }
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
            spinner_ = {};
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
            spinner_ = {};
            playMode_.Play(*ctx.scene);
        }
    } else {
        if (ImGui::Button("Stop")) {
            selection_.entity = kNullEntity;
            spinner_ = {};
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
        spinner_ = {};
        if (SceneSerializer::LoadFromFile(*ctx.scene, path)) {
            scenePath_ = path;
        }
    }
}

void EditorApp::BuildTestScene(EngineContext& ctx)
{
    Scene& s = *ctx.scene;
    RenderResources& res = *ctx.resources;

    const AssetID cube = res.meshes.Cube();
    const AssetID shader = AssetID{ HashStr("forward_lit") };
    const AssetID white = res.textures.White();

    auto makeMat = [&](const char* name, float r, float g, float b) {
        Material m;
        m.shader = shader;
        m.texture = white;
        m.baseColor = { r, g, b, 1.0f };
        return res.materials.Register(name, m);
    };
    const AssetID matGround = makeMat("mat_ground", 0.45f, 0.47f, 0.50f);
    const AssetID matArm = makeMat("mat_arm", 0.85f, 0.55f, 0.20f);
    const AssetID palette[4] = {
        makeMat("mat_red", 0.85f, 0.30f, 0.28f),
        makeMat("mat_green", 0.35f, 0.75f, 0.40f),
        makeMat("mat_blue", 0.30f, 0.50f, 0.85f),
        makeMat("mat_yellow", 0.90f, 0.80f, 0.30f),
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
}

} // namespace mye
