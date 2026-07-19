#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <Windows.h>
#include <shellapi.h>

#include <DirectXMath.h>

#include "Engine/Core/EcsSelfTest.h"
#include "Engine/Core/Log.h"
#include "Engine/Engine/EngineLoop.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/ModelLoader.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/ShaderManager.h"

#include "imgui.h"

using namespace DirectX;

namespace {

class EditorApp : public mye::IEngineApp {
public:
    void OnStart(mye::EngineContext& ctx) override
    {
        ctx.shaders->Load("forward_lit");
        BuildTestScene(ctx);
        MYE_LOG_INFO("EditorApp started (%u entities)", ctx.scene->GetWorld().AliveCount());
    }

    void OnTick(mye::EngineContext& ctx) override
    {
        // ---- 回転デモ (M1 完了確認: ルート回転に 3 階層が追従) ----
        if (spinner_) {
            spinYaw_ += 20.0f * ctx.fixedDt; // deg/s
            spinner_.SetLocalRotationEuler(0.0f, spinYaw_, 0.0f);
        }
        UpdateFlyCamera(ctx);
    }

    void OnImGui(mye::EngineContext& ctx) override
    {
        // M2 で dockspace + 各ウィンドウに置き換える
        if (ImGui::Begin("Engine Stats")) {
            const ImGuiIO& io = ImGui::GetIO();
            ImGui::Text("FPS: %.1f (%.3f ms)", io.Framerate, 1000.0f / io.Framerate);
            ImGui::Text("Frame: %llu / Tick: %llu",
                        static_cast<unsigned long long>(ctx.frameIndex),
                        static_cast<unsigned long long>(ctx.tickIndex));
            ImGui::Text("Entities: %u", ctx.scene->GetWorld().AliveCount());
            ImGui::Separator();
            ImGui::TextUnformatted("RMB drag: look / WASD+QE: move / Shift: fast");
        }
        ImGui::End();
    }

private:
    void BuildTestScene(mye::EngineContext& ctx)
    {
        using namespace mye;
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

        // カメラ
        camera_ = s.CreateGameObject("Main Camera");
        camera_.AddComponent<CameraComponent>();
        camera_.SetLocalPosition(0.0f, 7.0f, -16.0f);
        camPitch_ = 18.0f;
        camYaw_ = 0.0f;
        camera_.SetLocalRotationEuler(camPitch_, camYaw_, 0.0f);

        // 太陽光
        GameObject sun = s.CreateGameObject("Sun");
        sun.AddComponent<LightComponent>();
        sun.SetLocalRotationEuler(50.0f, -30.0f, 0.0f);

        // 地面
        GameObject ground = s.CreateGameObject("Ground");
        ground.SetLocalPosition(0.0f, -1.0f, 0.0f);
        ground.SetLocalScale(40.0f, 0.5f, 40.0f);
        {
            auto* mr = ground.AddComponent<MeshRendererComponent>();
            mr->mesh = cube;
            mr->material = matGround;
        }

        // 3 階層のスピナー: root → 20 arms → 各 25 cubes (= 500 leaf cubes)
        spinner_ = s.CreateGameObject("Spinner");
        spinner_.SetLocalPosition(0.0f, 1.5f, 0.0f);
        for (int a = 0; a < 20; ++a) {
            char name[32];
            snprintf(name, sizeof(name), "Arm_%02d", a);
            GameObject arm = s.CreateGameObject(name);
            arm.SetParent(spinner_);
            arm.SetLocalRotationEuler(0.0f, a * (360.0f / 20.0f), 0.0f);
            {
                auto* mr = arm.AddComponent<MeshRendererComponent>();
                mr->mesh = cube;
                mr->material = matArm;
                arm.SetLocalScale(0.4f, 0.4f, 0.4f);
            }
            for (int j = 0; j < 25; ++j) {
                snprintf(name, sizeof(name), "Cube_%02d_%02d", a, j);
                GameObject leaf = s.CreateGameObject(name);
                leaf.SetParent(arm);
                // arm はスケール 0.4 なので、子はローカルで大きめに置く
                const float dist = (2.0f + 0.9f * j) / 0.4f;
                const float wave = 1.0f + 0.8f * ((j % 5) - 2) * 0.3f;
                leaf.SetLocalPosition(dist, wave, 0.0f);
                leaf.SetLocalScale(0.75f, 0.75f, 0.75f);
                auto* mr = leaf.AddComponent<MeshRendererComponent>();
                mr->mesh = cube;
                mr->material = palette[(a + j) % 4];
            }
        }

        // glTF モデル (assets\models\BoxTextured.glb — 無ければスキップ)
        GameObject model = ModelLoader::Load(s, res, *ctx.shaders,
                                             ctx.assetsRoot + L"\\models\\BoxTextured.glb");
        if (model) {
            model.SetLocalPosition(0.0f, 1.5f, 0.0f);
            model.SetLocalScale(2.0f, 2.0f, 2.0f);
        }
    }

    void UpdateFlyCamera(mye::EngineContext& ctx)
    {
        using namespace mye;
        if (!camera_) {
            return;
        }
        const InputSnapshot& in = ctx.input;

        // RMB ドラッグで視点回転 (tick 間のマウス差分)
        if (haveMousePrev_ && in.MouseDown(1)) {
            camYaw_ += static_cast<float>(in.mouseX - prevMouseX_) * 0.25f;
            camPitch_ += static_cast<float>(in.mouseY - prevMouseY_) * 0.25f;
            camPitch_ = std::clamp(camPitch_, -89.0f, 89.0f);
        }
        prevMouseX_ = in.mouseX;
        prevMouseY_ = in.mouseY;
        haveMousePrev_ = true;
        camera_.SetLocalRotationEuler(camPitch_, camYaw_, 0.0f);

        // WASD + QE 移動
        auto* t = camera_.GetComponent<LocalTransform>();
        const XMVECTOR rot = XMLoadFloat4(&t->rotation);
        const XMVECTOR forward = XMVector3Rotate(XMVectorSet(0, 0, 1, 0), rot);
        const XMVECTOR right = XMVector3Rotate(XMVectorSet(1, 0, 0, 0), rot);
        const XMVECTOR up = XMVectorSet(0, 1, 0, 0);

        XMVECTOR move = XMVectorZero();
        if (in.KeyDown('W')) { move = XMVectorAdd(move, forward); }
        if (in.KeyDown('S')) { move = XMVectorSubtract(move, forward); }
        if (in.KeyDown('D')) { move = XMVectorAdd(move, right); }
        if (in.KeyDown('A')) { move = XMVectorSubtract(move, right); }
        if (in.KeyDown('E')) { move = XMVectorAdd(move, up); }
        if (in.KeyDown('Q')) { move = XMVectorSubtract(move, up); }

        if (XMVectorGetX(XMVector3LengthSq(move)) > 0.0001f) {
            const float speed = in.KeyDown(VK_SHIFT) ? 20.0f : 6.0f;
            move = XMVectorScale(XMVector3Normalize(move), speed * ctx.fixedDt);
            XMVECTOR pos = XMLoadFloat3(&t->position);
            XMStoreFloat3(&t->position, XMVectorAdd(pos, move));
        }
    }

    mye::GameObject spinner_;
    mye::GameObject camera_;
    float spinYaw_ = 0.0f;
    float camYaw_ = 0.0f;
    float camPitch_ = 0.0f;
    int32_t prevMouseX_ = 0;
    int32_t prevMouseY_ = 0;
    bool haveMousePrev_ = false;
};

// コンソールから起動された場合に標準出力をそのコンソールへ繋ぐ
// (CLI モード --replay-verify (M6) や スモークテストのログ確認用)。
// 既にリダイレクトされている場合 (パイプ/ファイル) は CRT が起動時に束縛済みなので触らない
// — ここで CONOUT$ を開くとリダイレクトを上書きしてしまう。
void AttachParentConsole()
{
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    const bool redirected = (out != nullptr && out != INVALID_HANDLE_VALUE);
    if (!redirected && AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    AttachParentConsole();

    mye::EngineConfig config;
    config.title = L"MyEngine Editor";
    bool selftest = false;

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            const std::wstring arg = argv[i];
            if (arg == L"--frames" && i + 1 < argc) {
                config.maxFrames = _wtoi64(argv[++i]);
            } else if (arg == L"--width" && i + 1 < argc) {
                config.width = _wtoi(argv[++i]);
            } else if (arg == L"--height" && i + 1 < argc) {
                config.height = _wtoi(argv[++i]);
            } else if (arg == L"--no-vsync") {
                config.vsync = false;
            } else if (arg == L"--screenshot" && i + 1 < argc) {
                config.screenshotPath = argv[++i];
            } else if (arg == L"--shot-frame" && i + 1 < argc) {
                config.screenshotFrame = _wtoi64(argv[++i]);
            } else if (arg == L"--selftest") {
                selftest = true;
            }
        }
        LocalFree(argv);
    }

    if (selftest) {
        return mye::RunEcsSelfTest() ? 0 : 1; // ウィンドウ/D3D 不要のヘッドレス実行
    }

    EditorApp app;
    mye::EngineLoop loop;
    return loop.Run(config, app);
}
