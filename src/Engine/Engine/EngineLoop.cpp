#include "Engine/Engine/EngineLoop.h"

#include "Engine/Core/Check.h"
#include "Engine/Core/Log.h"
#include "Engine/Engine/CollisionSystem.h"
#include "Engine/Engine/HotReload/DllReloader.h"
#include "Engine/Engine/HotReload/ReloadHub.h"
#include "Engine/Engine/Particles/ParticleSystem.h"
#include "Engine/Engine/RenderSystem.h"
#include "Engine/Engine/Replay/Replay.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Engine/Script/ScriptHost.h"
#include "Engine/Engine/TransformSystem.h"
#include "Engine/Platform/Clock.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Platform/Win32Window.h"
#include "Engine/Renderer/DeferredPath.h"
#include "Engine/Renderer/ForwardPath.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/GraphicsDevice.h"
#include "Engine/Renderer/ImGuiRenderer.h"
#include "Engine/Renderer/ShaderManager.h"
#include "Engine/Renderer/SwapChain.h"

#include <filesystem>

#include <Windows.h>

namespace mye {
namespace {

constexpr double kFixedDt = 1.0 / 60.0;
constexpr int kMaxTicksPerFrame = 5;   // スパイラルオブデス防止
constexpr double kMaxFrameDt = 0.25;   // ブレークポイント等の巨大 dt をクランプ

} // namespace

int EngineLoop::Run(const EngineConfig& config, IEngineApp& app)
{
    Win32Window window;
    GraphicsDevice device;
    SwapChain swapChain;
    ImGuiRenderer imgui;
    Input input;
    Clock clock;
    Scene scene;
    ShaderManager shaderManager;
    RenderResources resources;
    TransformSystem transformSystem;
    RenderSystem renderSystem;
    ForwardPath forwardPath;
    DeferredPath deferredPath;
    ReloadHub reloadHub;
    ScriptHost scriptHost;
    DllReloader dllReloader;
    ParticleSystem particleSystem;
    CollisionSystem collisionSystem;
    IRenderPath* activePath = &forwardPath;

    // ---- 起動 ----
    WindowDesc wd;
    wd.title = config.title.c_str();
    wd.width = config.width;
    wd.height = config.height;
    if (!window.Create(wd)) {
        return 1;
    }
    if (!device.Init()) {
        return 1;
    }
    if (!swapChain.Init(device, window.Hwnd(), window.Width(), window.Height())) {
        return 1;
    }
    if (config.enableImGui && !imgui.Init(window, device)) {
        return 1;
    }
    // ImGui のハンドラ登録より後に登録する (ImGui が先にメッセージを見る)
    window.AddMsgHandler([&input](void* hwnd, uint32_t msg, uint64_t wp, int64_t lp, int64_t& result) {
        return input.HandleMessage(hwnd, msg, wp, lp, result);
    });

    const std::wstring assetsRoot = FindAssetsRoot();
    shaderManager.Init(device, assetsRoot + L"\\shaders");
    resources.Init(device);
    if (!forwardPath.Init(device, shaderManager)) {
        return 1;
    }
    if (!deferredPath.Init(device, shaderManager)) {
        return 1;
    }
    reloadHub.Init(&shaderManager, &resources, &scene, assetsRoot);
    particleSystem.Init(device, shaderManager, assetsRoot);

    // GameLogic.dll (スクリプト層)。エンジンの exe と同じ構成のビルド出力を監視する
    scriptHost.Init(&scene);
    {
        const std::wstring cacheHot =
            (std::filesystem::path(assetsRoot).parent_path() / L"cache" / L"hot").wstring();
        dllReloader.Init(&scriptHost, GetExecutableDir() + L"\\GameLogic.dll", cacheHot);
        dllReloader.LoadInitial();
    }

    clock.Init();

    EngineContext ctx;
    ctx.window = &window;
    ctx.device = &device;
    ctx.swapChain = &swapChain;
    ctx.scene = &scene;
    ctx.shaders = &shaderManager;
    ctx.resources = &resources;
    ctx.renderSystem = &renderSystem;
    ctx.renderPath = activePath;
    ctx.renderPathForward = &forwardPath;
    ctx.renderPathDeferred = &deferredPath;
    ctx.reloadHub = &reloadHub;
    ctx.scriptHost = &scriptHost;
    ctx.dllReloader = &dllReloader;
    ctx.particles = &particleSystem;
    ctx.assetsRoot = assetsRoot;
    ctx.fixedDt = static_cast<float>(kFixedDt);

    app.OnStart(ctx);
    // OnStart で積まれた構造変更 (SetParent 等) を最初の描画前に反映する
    scene.GetWorld().ApplyStructuralChanges();
    MYE_LOG_INFO("Engine loop started (fixed dt = %.4f s, assets = %s)",
                 kFixedDt, WideToUtf8(assetsRoot).c_str());

    // ---- リプレイ記録/検証の準備 (spec 11.3) ----
    ReplayRecorder recorder;
    ReplayPlayer player;
    int exitCode = 0;
    if (!config.replayVerifyPath.empty()) {
        if (!player.Load(config.replayVerifyPath)) {
            return 1;
        }
        // 記録開始時の RNG 状態を復元して同一 tick 列を再現する
        scene.GetWorld().Rng().Restore(player.RngState(), player.RngInc());
    } else if (!config.replayRecordPath.empty()) {
        recorder.Start(config.replayRecordPath, scene.GetWorld().Rng().State(),
                       scene.GetWorld().Rng().Inc(), scene.GetWorld().AliveCount());
    }

    // ---- メインループ (フェーズ構成は engine_spec.md 5.3 / ADR-005) ----
    double accumulator = 0.0;
    bool running = true;
    while (running) {
        // ---- フェーズ 1: 時間更新 / 入力取得 ----
        if (!window.PumpMessages()) {
            break;
        }
        if (window.ConsumeResize()) {
            swapChain.Resize(window.Width(), window.Height());
        }
        double dt = clock.BeginFrame();
        if (dt > kMaxFrameDt) {
            dt = kMaxFrameDt;
        }
        ctx.input = input.CaptureSnapshot();
        logging::SetCurrentFrame(ctx.frameIndex);

        // ---- フェーズ 2: ホットリロード適用セーフポイント ----
        const double tReload = clock.Now();
        reloadHub.Update();
        dllReloader.Update();
        FrameTimings timings;
        timings.reloadMs = static_cast<float>((clock.Now() - tReload) * 1000.0);
        const double tTicks = clock.Now();

        // ---- 固定 tick: フェーズ 3(Script) / 4(Systems) / 5(Late) / 7(構造変更) ----
        accumulator += dt;
        int ticks = 0;
        const bool verifying = player.IsActive();
        // 検証モードは実時間と切り離して最速で回す (spec 11.3 の CLI 実行)
        const int maxTicksThisFrame = verifying ? 64 : kMaxTicksPerFrame;
        while (ticks < maxTicksThisFrame
               && (verifying ? player.HasTick(ctx.tickIndex) : accumulator >= kFixedDt)) {
            if (verifying) {
                ctx.input = player.InputForTick(ctx.tickIndex); // フェーズ 1 の入力を置換
            }
            app.OnTick(ctx); // エディタ更新 + simulateScripts の決定
            // ---- フェーズ 3: スクリプト層 Start → Update ----
            const bool runScripts = ctx.simulateScripts && scriptHost.IsLoaded();
            if (runScripts) {
                scriptHost.SetTickContext(ctx.input, ctx.tickIndex, ctx.fixedDt);
                scriptHost.RunStartAndUpdate();
            }
            // ---- フェーズ 4: システム層 ----
            // Transform を先に確定 (エミッタ/コライダのワールド位置は tick 決定論の一部)
            transformSystem.Update(scene.GetWorld());
            if (ctx.simulateScripts) {
                collisionSystem.Update(scene.GetWorld(), &scriptHost); // トリガーイベント配信
                particleSystem.Update(scene.GetWorld(), ctx.fixedDt);
            }
            // ---- フェーズ 5: スクリプト層 LateUpdate ----
            if (runScripts) {
                scriptHost.RunLateUpdate();
            }
            scene.GetWorld().ApplyStructuralChanges(); // フェーズ 7 (tick 末適用 = ADR-005)

            // ---- リプレイ: tick 末の状態ハッシュ (spec 11.3) ----
            if (recorder.IsActive()) {
                recorder.RecordTick(ctx.input, HashWorld(scene.GetWorld(), &particleSystem.Cpu()));
                if (recorder.TickCount() >= static_cast<uint64_t>(config.replayTicks)) {
                    recorder.Finish();
                    ctx.requestExit = true;
                }
            } else if (verifying) {
                const uint64_t actual = HashWorld(scene.GetWorld(), &particleSystem.Cpu());
                const uint64_t expected = player.ExpectedHash(ctx.tickIndex);
                if (actual != expected) {
                    // 乖離: 初回の tick とエンティティ別サブハッシュを報告して失敗終了
                    player.failed = true;
                    player.firstMismatchTick = ctx.tickIndex;
                    MYE_LOG_ERROR("[replay] HASH MISMATCH at tick %llu",
                                  static_cast<unsigned long long>(ctx.tickIndex));
                    MYE_LOG_ERROR("[replay]   expected %016llX / actual %016llX",
                                  static_cast<unsigned long long>(expected),
                                  static_cast<unsigned long long>(actual));
                    std::vector<EntityHash> detail;
                    uint64_t total = 0;
                    HashWorldDetailed(scene.GetWorld(), &particleSystem.Cpu(), detail, total);
                    MYE_LOG_ERROR("[replay]   entities=%zu rng=%016llX", detail.size(),
                                  static_cast<unsigned long long>(scene.GetWorld().Rng().State()));
                    for (size_t i = 0; i < detail.size() && i < 8; ++i) {
                        MYE_LOG_ERROR("[replay]   entity %u:%u hash=%016llX (%s)",
                                      detail[i].entity.index, detail[i].entity.generation,
                                      static_cast<unsigned long long>(detail[i].hash),
                                      scene.GetWorld().GetName(detail[i].entity));
                    }
                    exitCode = 1;
                    ctx.requestExit = true;
                } else {
                    ++player.verifiedTicks;
                }
            }

            ++ctx.tickIndex;
            ++ticks;
            if (!verifying) {
                accumulator -= kFixedDt;
            }
            if (ctx.requestExit) {
                break;
            }
        }
        if (!verifying && ticks == kMaxTicksPerFrame && accumulator > kFixedDt) {
            // 追いつけない分は捨てる (スローモーション化を許容し、tick 爆発を防ぐ)
            accumulator = kFixedDt;
        }
        if (verifying && !player.failed && !player.HasTick(ctx.tickIndex)) {
            MYE_LOG_INFO("[replay] VERIFY PASS: %llu ticks hash-identical",
                         static_cast<unsigned long long>(player.verifiedTicks));
            ctx.requestExit = true;
        }
        timings.tickMs = static_cast<float>((clock.Now() - tTicks) * 1000.0);
        timings.ticksThisFrame = ticks;
        const double tRender = clock.Now();

        // エディタ (または将来の設定) によるレンダリングパス切替を反映
        if (ctx.renderPath != nullptr && ctx.renderPath != activePath) {
            activePath = ctx.renderPath;
            MYE_LOG_INFO("[render] path switched to %s", activePath->Name());
        }

        if (!window.IsMinimized()) {
            // ---- フェーズ 6: シーン描画 ----
            // ワールド行列は描画直前に一括更新 (LocalTransform の純関数なので sim 状態に影響しない)
            transformSystem.Update(scene.GetWorld());

            app.OnRenderViews(ctx); // エディタの SceneView / GameView (独自 RT)

            FrameTarget target;
            target.rtv = swapChain.BackbufferRTV();
            target.dsv = swapChain.DepthDSV();
            target.width = swapChain.Width();
            target.height = swapChain.Height();
            memcpy(target.clearColor, config.clearColor, sizeof(target.clearColor));
            if (config.renderSceneToBackbuffer) {
                renderSystem.Render(scene.GetWorld(), device, *activePath, shaderManager, resources,
                                    target, nullptr, &particleSystem);
            } else {
                // ImGui 描画の下地としてクリアのみ
                ID3D11DeviceContext* dc = device.Context();
                dc->OMSetRenderTargets(1, &target.rtv, target.dsv);
                D3D11_VIEWPORT vp = {};
                vp.Width = static_cast<float>(target.width);
                vp.Height = static_cast<float>(target.height);
                vp.MaxDepth = 1.0f;
                dc->RSSetViewports(1, &vp);
                dc->ClearRenderTargetView(target.rtv, target.clearColor);
                dc->ClearDepthStencilView(target.dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
            }

            timings.renderMs = static_cast<float>((clock.Now() - tRender) * 1000.0);
            const double tPresent = clock.Now();

            // ---- フェーズ 8: ImGui 描画 / Present ----
            if (config.enableImGui) {
                imgui.BeginFrame();
                app.OnImGui(ctx);
                imgui.EndFrame();
            }
            timings.presentMs = static_cast<float>((clock.Now() - tPresent) * 1000.0);
            if (!config.screenshotPath.empty()) {
                if (config.screenshotEvery > 0) {
                    if (ctx.frameIndex > 0
                        && ctx.frameIndex % static_cast<uint64_t>(config.screenshotEvery) == 0) {
                        wchar_t numbered[1024];
                        swprintf_s(numbered, L"%s.%05llu.png", config.screenshotPath.c_str(),
                                   static_cast<unsigned long long>(ctx.frameIndex));
                        swapChain.SaveBackbufferPng(numbered);
                    }
                } else if (ctx.frameIndex == static_cast<uint64_t>(config.screenshotFrame)) {
                    swapChain.SaveBackbufferPng(config.screenshotPath);
                }
            }
            swapChain.Present(config.vsync);
        } else {
            Sleep(10); // 最小化中はスピンしない
        }

        device.PumpDebugMessages(); // D3D 検証メッセージをログへ (Debug のみ)

        timings.frameMs = static_cast<float>(dt * 1000.0);
        ctx.timings = timings;

        ++ctx.frameIndex;
        if (config.maxFrames > 0 && ctx.frameIndex >= static_cast<uint64_t>(config.maxFrames)) {
            MYE_LOG_INFO("maxFrames (%lld) reached, exiting", static_cast<long long>(config.maxFrames));
            running = false;
        }
        if (ctx.requestExit) {
            running = false;
        }
    }

    // ---- 終了 (起動の逆順) ----
    app.OnShutdown(ctx);
    particleSystem.Shutdown();
    scriptHost.Shutdown();
    reloadHub.Shutdown();
    deferredPath.Shutdown();
    forwardPath.Shutdown();
    imgui.Shutdown();
    swapChain.Shutdown();
    device.Shutdown();
    window.Destroy();
    if (recorder.IsActive()) {
        recorder.Finish(); // maxFrames 等で先に抜けた場合も書き出す
    }
    MYE_LOG_INFO("Engine loop finished (%llu frames, %llu ticks)",
                 static_cast<unsigned long long>(ctx.frameIndex),
                 static_cast<unsigned long long>(ctx.tickIndex));
    return exitCode;
}

} // namespace mye
