#include "Engine/Engine/EngineLoop.h"

#include "Engine/Core/Check.h"
#include "Engine/Core/Log.h"
#include "Engine/Platform/Clock.h"
#include "Engine/Platform/Win32Window.h"
#include "Engine/Renderer/GraphicsDevice.h"
#include "Engine/Renderer/ImGuiRenderer.h"
#include "Engine/Renderer/SwapChain.h"

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

    clock.Init();

    EngineContext ctx;
    ctx.window = &window;
    ctx.device = &device;
    ctx.swapChain = &swapChain;
    ctx.fixedDt = static_cast<float>(kFixedDt);

    app.OnStart(ctx);
    MYE_LOG_INFO("Engine loop started (fixed dt = %.4f s)", kFixedDt);

    // ---- メインループ ----
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

        // ---- フェーズ 2: ホットリロード適用セーフポイント (M3/M4 で実装) ----

        // ---- 固定 tick: フェーズ 3(Script) / 4(Systems) / 5(Late) / 7(構造変更) ----
        accumulator += dt;
        int ticks = 0;
        while (accumulator >= kFixedDt && ticks < kMaxTicksPerFrame) {
            app.OnTick(ctx); // フェーズ 3 スロット (M1 以降: システム更新/構造変更適用が後続する)
            ++ctx.tickIndex;
            accumulator -= kFixedDt;
            ++ticks;
        }
        if (ticks == kMaxTicksPerFrame && accumulator > kFixedDt) {
            // 追いつけない分は捨てる (スローモーション化を許容し、tick 爆発を防ぐ)
            accumulator = kFixedDt;
        }

        if (!window.IsMinimized()) {
            // ---- フェーズ 6: シーン描画 (M1 以降は RenderPath 経由) ----
            ID3D11RenderTargetView* rtv = swapChain.BackbufferRTV();
            ID3D11DeviceContext* dc = device.Context();
            dc->OMSetRenderTargets(1, &rtv, nullptr);
            D3D11_VIEWPORT vp = {};
            vp.Width = static_cast<float>(swapChain.Width());
            vp.Height = static_cast<float>(swapChain.Height());
            vp.MaxDepth = 1.0f;
            dc->RSSetViewports(1, &vp);
            dc->ClearRenderTargetView(rtv, config.clearColor);

            // ---- フェーズ 8: ImGui 描画 / Present ----
            if (config.enableImGui) {
                imgui.BeginFrame();
                app.OnImGui(ctx);
                imgui.EndFrame();
            }
            swapChain.Present(config.vsync);
        } else {
            Sleep(10); // 最小化中はスピンしない
        }

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
    imgui.Shutdown();
    swapChain.Shutdown();
    device.Shutdown();
    window.Destroy();
    MYE_LOG_INFO("Engine loop finished (%llu frames, %llu ticks)",
                 static_cast<unsigned long long>(ctx.frameIndex),
                 static_cast<unsigned long long>(ctx.tickIndex));
    return 0;
}

} // namespace mye
