#include "Engine/Engine/EngineLoop.h"

#include "Engine/Core/Check.h"
#include "Engine/Core/JobSystem.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Profiler.h"
#include "Engine/Engine/Animation.h"
#include "Engine/Engine/AnimatorController.h"
#include "Engine/Engine/AssetDatabase.h"
#include "Engine/Engine/CollisionSystem.h"
#include "Engine/Engine/HotReload/DllReloader.h"
#include "Engine/Engine/HotReload/ReloadHub.h"
#include "Engine/Engine/Audio/AudioSystem.h"
#include "Engine/Engine/Particles/ParticleSystem.h"
#include "Engine/Engine/Physics/PhysicsSystem.h"
#include "Engine/Engine/Prefab.h"
#include "Engine/Engine/Project.h"
#include "Engine/Engine/RenderSystem.h"
#include "Engine/Engine/Replay/Replay.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Engine/SceneSerializer.h"
#include "Engine/Engine/Script/ManagedHost.h"
#include "Engine/Engine/Script/ScriptHost.h"
#include "Engine/Engine/SkinningSystem.h"
#include "Engine/Engine/TransformSystem.h"
#include "Engine/Engine/UI/UIRenderer.h"
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
    ManagedHost managedHost;
    ParticleSystem particleSystem;
    CollisionSystem collisionSystem;
    PhysicsSystem physicsSystem; // 剛体積分 + 衝突解決 (M20、ステートレス)
    std::vector<SolidContact> solidContacts; // 物理→衝突イベントの tick 内受け渡し (M28c)
    PrefabLibrary prefabLibrary;
    AnimationLibrary animLibrary;
    AnimationSystem animationSystem;
    ControllerLibrary controllerLibrary;        // Animator Controller (.controller.json、M22)
    AnimatorControllerSystem controllerSystem;  // ステートマシン評価 + ブレンド
    AssetDatabase assetDatabase;                // GUID/.meta サイドカー DB (M23)
    SkinningSystem skinningSystem; // スケルタルアニメの時刻進行 (M18)
    UIRenderer uiRenderer;         // ゲーム内 UI (M21、backbuffer/GameView への重ね描画)
    AudioSystem audioSystem;       // XAudio2 (M19、決定論レーン外の出力 sink)
    std::vector<ScriptAudioEvent> audioQueue; // スクリプトの再生イベント (tick 内で積む)
    std::wstring pendingScene;                // LoadScene の遅延ロード先 (tick 末に消費)
    IRenderPath* activePath = &forwardPath;

    // ---- プロジェクト/アセットルート解決 (M26) ----
    // --project 指定時は <root>\assets を使い、状態ファイル (.mye\) をプロジェクト側へ置く。
    // 未指定はレガシー動作 (exe から上へ assets を探索、imgui.ini は CWD 相対)
    std::wstring assetsRoot;
    std::wstring imguiIniPath = L"imgui.ini";
    if (!config.projectRoot.empty()) {
        assetsRoot = config.projectRoot + L"\\assets";
        std::error_code fsec;
        if (!std::filesystem::exists(assetsRoot, fsec)) {
            MYE_LOG_ERROR("project assets not found: %s", WideToUtf8(assetsRoot).c_str());
            return 1;
        }
        const std::wstring localDir = config.projectRoot + L"\\" + kProjectLocalDir;
        std::filesystem::create_directories(localDir, fsec);
        imguiIniPath = localDir + L"\\imgui.ini";
    } else {
        assetsRoot = FindAssetsRoot();
    }

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
    ImGuiInitOptions imguiOpts;
    if (!config.projectRoot.empty()) {
        imguiOpts.iniPath = imguiIniPath; // 空 = ImGui 既定 (CWD の imgui.ini)
    }
    if (config.enableImGui && !imgui.Init(window, device, imguiOpts)) {
        return 1;
    }
    // ImGui のハンドラ登録より後に登録する (ImGui が先にメッセージを見る)
    window.AddMsgHandler([&input](void* hwnd, uint32_t msg, uint64_t wp, int64_t lp, int64_t& result) {
        return input.HandleMessage(hwnd, msg, wp, lp, result);
    });

    shaderManager.Init(device, assetsRoot + L"\\shaders");
    resources.Init(device);
    if (!forwardPath.Init(device, shaderManager)) {
        return 1;
    }
    if (!deferredPath.Init(device, shaderManager)) {
        return 1;
    }
    uiRenderer.Init(device, shaderManager); // M21: 失敗してもエンジンは継続 (UI が出ないだけ)
    reloadHub.Init(&shaderManager, &resources, &scene, &prefabLibrary, &animLibrary, assetsRoot);
    particleSystem.Init(device, shaderManager, assetsRoot);

    // ポストプロセス設定を config から反映 (M16)。全ビューポート共通の renderSystem に載る
    renderSystem.enablePostFx = config.postFx;
    renderSystem.postFxSettings.tonemap = config.postFxTonemap;
    renderSystem.postFxSettings.exposure = config.postFxExposure;
    renderSystem.postFxSettings.bloom = config.postFxBloom;
    renderSystem.postFxSettings.bloomThreshold = config.postFxBloomThreshold;
    renderSystem.postFxSettings.bloomIntensity = config.postFxBloomIntensity;
    renderSystem.postFxSettings.fxaa = config.postFxFxaa;

    // GameLogic.dll (スクリプト層)。エンジンの exe と同じ構成のビルド出力を監視する
    scriptHost.Init(&scene);
    {
        const std::wstring cacheHot =
            (std::filesystem::path(assetsRoot).parent_path() / L"cache" / L"hot").wstring();
        dllReloader.Init(&scriptHost, GetExecutableDir() + L"\\GameLogic.dll", cacheHot);
        dllReloader.LoadInitial();
    }

    // C# スクリプトホスト (CoreCLR)。未導入でも失敗ログのみでエンジンは継続する
    if (managedHost.Init(GetExecutableDir(), &scene)) {
        managedHost.CompileScripts(assetsRoot + L"\\scripts"); // 起動時に既存 C# を一括コンパイル
    }
    // シーン保存/復元時に C# コンポーネントのフィールドを永続化する hook を登録
    SceneSerializer::SetManagedHost(&managedHost);

    // オーディオ (M19)。XAudio2 初期化 + デモ .wav ロード + 両ホストへ共有バッファ接続。
    // 初期化失敗 (ヘッドレス等) でもエンジンは継続する (PlaySound が no-op になるだけ)
    audioSystem.Init();
    audioSystem.LoadWav("beep", assetsRoot + L"\\audio\\beep.wav");
    scriptHost.SetSharedServices(&audioQueue, &pendingScene);
    managedHost.SetSharedServices(&audioQueue, &pendingScene);

    clock.Init();

    EngineContext ctx;
    ctx.window = &window;
    ctx.device = &device;
    ctx.swapChain = &swapChain;
    ctx.scene = &scene;
    ctx.shaders = &shaderManager;
    ctx.resources = &resources;
    ctx.renderSystem = &renderSystem;
    ctx.uiRenderer = &uiRenderer;
    ctx.renderPath = activePath;
    ctx.renderPathForward = &forwardPath;
    ctx.renderPathDeferred = &deferredPath;
    ctx.reloadHub = &reloadHub;
    ctx.scriptHost = &scriptHost;
    ctx.dllReloader = &dllReloader;
    ctx.managedHost = &managedHost;
    ctx.particles = &particleSystem;
    ctx.prefabs = &prefabLibrary;
    ctx.anims = &animLibrary;
    ctx.controllers = &controllerLibrary;
    ctx.assetDb = &assetDatabase;
    ctx.assetsRoot = assetsRoot;
    ctx.projectRoot = config.projectRoot;
    ctx.imguiIniPath = imguiIniPath;

    // M23: assets\ を走査して .meta サイドカー (GUID) を生成/同期する。
    // アセット登録 (app.OnStart → RegisterAssetLibraries) の前に済ませ、パス⇄GUID 解決を利用可能にする。
    assetDatabase.ScanAndSync(assetsRoot);

    // M25: ジョブシステム起動 (min(16, cores-2) ワーカー)。--no-jobs で直列化。
    jobs::System().Init();
    jobs::System().SetEnabled(config.useJobs);
    MYE_LOG_INFO("[jobs] %s (%d workers)", config.useJobs ? "enabled" : "disabled (serial)",
                 jobs::System().WorkerCount());
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
        prof::BeginFrame();

        // ---- フェーズ 2: ホットリロード適用セーフポイント ----
        const double tReload = clock.Now();
        reloadHub.Update();
        dllReloader.Update();
        resources.textures.PollAsyncLoads(); // M23: 非同期デコード完了分を GPU 公開 (セーフポイント)
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
            // C# スクリプト層 (別レーン): 記録/検証中は走らせない → 純 C++ 決定論を保持
            const bool runManaged = ctx.simulateScripts && managedHost.IsReady()
                && !recorder.IsActive() && !player.IsActive();
            if (runManaged) {
                managedHost.SetTickContext(ctx.input, ctx.tickIndex, ctx.fixedDt);
                managedHost.RunStartAndUpdate();
            }
            // ---- アニメーション (フェーズ 3.5): スクリプト後・Transform 前に LocalTransform を確定 ----
            // Play 中のみ進行 (編集時は Animation 窓が明示サンプリングする)。
            // AnimatorComponent 非存在シーンでは完全 no-op = 既存シーンのリプレイ不変
            if (ctx.simulateScripts) {
                MYE_PROFILE_SCOPE("animation");
                animationSystem.Update(scene.GetWorld(), animLibrary);
                // Animator Controller (M22): ステートマシンでクリップを切替・ブレンド。
                // LocalTransform を駆動するので hash 対象、決定論 (整数 tick・整数比ブレンド)
                controllerSystem.Update(scene.GetWorld(), controllerLibrary, animLibrary);
                // スケルタルアニメの時刻を進める (M18)。ポーズは非ハッシュなのでリプレイ不変
                skinningSystem.Update(scene.GetWorld(), resources);
            }
            // ---- 物理 (フェーズ 3.6): スクリプト/アニメ後・Transform 前に剛体を積分 ----
            // LocalTransform.position を書き換えるので TransformSystem 前に走らせ、確定した
            // ワールド位置でコライダ判定させる。Rigidbody 非存在シーンでは完全 no-op (opt-in)
            if (ctx.simulateScripts) {
                MYE_PROFILE_SCOPE("physics");
                // ソリッド接触ペアを受け取り CollisionSystem へ渡す (M28c OnCollision 配信)
                physicsSystem.Update(scene.GetWorld(), ctx.fixedDt, &solidContacts);
            }
            // ---- フェーズ 4: システム層 ----
            // Transform を先に確定 (エミッタ/コライダのワールド位置は tick 決定論の一部)
            {
                MYE_PROFILE_SCOPE("transform");
                transformSystem.Update(scene.GetWorld());
            }
            if (ctx.simulateScripts) {
                {
                    MYE_PROFILE_SCOPE("collision");
                    // C# にもトリガー配信 (別レーン: 記録/検証中は managed=null で純 C++)
                    collisionSystem.Update(scene.GetWorld(), &scriptHost,
                                           runManaged ? &managedHost : nullptr, &solidContacts);
                }
                {
                    MYE_PROFILE_SCOPE("particles");
                    particleSystem.Update(scene.GetWorld(), ctx.fixedDt);
                }
            }
            // ---- フェーズ 5: スクリプト層 LateUpdate ----
            if (runScripts) {
                scriptHost.RunLateUpdate();
            }
            if (runManaged) {
                managedHost.RunLateUpdate();
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

            // ---- オーディオ drain (M19): ハッシュ後に再生する (voice 状態は絶対に hashed state
            // へ戻さない)。記録/検証中はゲート (実音を出さない)。キューは毎 tick クリア ----
            if (!recorder.IsActive() && !verifying) {
                for (const ScriptAudioEvent& e : audioQueue) {
                    audioSystem.Play(e.key, e.volume);
                }
            }
            audioQueue.clear();

            // ---- シーン遷移 (M19.4): pendingScene が積まれていれば tick 末にロードする ----
            // スクリプトが決定論的に LoadScene → 記録/検証とも同一 tick に再現される。
            // world.Clear (LoadFromFile 内) + carry-state リセット + RNG 決定論的再シードで
            // 新シーンが決定論的に始まる。mid-iteration の world 破棄を避けるため必ず tick 末。
            if (!pendingScene.empty()) {
                std::wstring full = pendingScene;
                pendingScene.clear();
                if (full.find(L':') == std::wstring::npos) {
                    full = assetsRoot + L"\\" + full; // assets ルート相対を絶対化
                }
                if (SceneSerializer::LoadFromFile(scene, full)) {
                    scene.GetWorld().Rng().Seed(0x4D794531ull); // 決定論的再シード (World 既定値)
                    collisionSystem.Reset();
                    particleSystem.ResetParticles();
                    scriptHost.ClearStarted();
                    managedHost.OnSceneReloaded();
                    MYE_LOG_INFO("[scene] loaded: %s", WideToUtf8(full).c_str());
                } else {
                    MYE_LOG_WARN("[scene] load failed: %s", WideToUtf8(full).c_str());
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
        audioSystem.Update(); // 再生し終えた source voice を掃除 (M19、フレーム毎)
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
                // M21: ゲーム内 UI を backbuffer に重ねる (Runtime 経路)。マウスは hover 表示用
                uiRenderer.Render(scene.GetWorld(), device, shaderManager, resources, target.rtv,
                                  target.width, target.height, ctx.input.mouseX, ctx.input.mouseY,
                                  ctx.input.MouseDown(0));
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
    uiRenderer.Shutdown();  // M21
    audioSystem.Shutdown(); // M19: source voice + XAudio2 を破棄 (host より先でも後でも可)
    jobs::System().Shutdown(); // M25: ワーカー join
    particleSystem.Shutdown();
    scriptHost.Shutdown();
    managedHost.Shutdown();
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
