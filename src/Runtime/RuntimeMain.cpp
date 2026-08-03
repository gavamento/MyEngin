#include <cstdio>
#include <filesystem>
#include <string>

#include <Windows.h>
#include <shellapi.h>

#include "Engine/Core/Log.h"
#include "Engine/Engine/DemoContent.h"
#include "Engine/Engine/EngineLoop.h"
#include "Engine/Engine/Project.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Engine/SceneSerializer.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/ShaderManager.h"

namespace {

// コンソールから起動された場合に標準出力を繋ぐ (CLI/リプレイ検証用)。EditorMain と同じ方針
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

// エディタ UI 無しの薄いランタイム (engine_spec.md 1.4 / M15)。
// リソース + シーンを読み、常時シミュレートしてバックバッファへ直接描画する。
// エンジン (EngineLoop) と GameLogic.dll は Editor と完全に共有 —
// よって同一シーンのシミュレーションは Editor とビット単位で一致する (リプレイ検証で実証)。
class RuntimeApp : public mye::IEngineApp {
public:
    std::wstring scenePath;
    bool startDeferred = false;

    void OnStart(mye::EngineContext& ctx) override
    {
        ctx.shaders->Load("forward_lit");
        mye::RegisterDemoContent(ctx);   // Editor と同じ実体登録 (AssetID 解決)
        mye::RegisterAssetLibraries(ctx); // .prefab / .anim を登録
        if (scenePath.empty()) {
            scenePath = ctx.assetsRoot + L"\\scenes\\main.scene.json";
            mye::ProjectManifest manifest; // ブートシーンはマニフェスト優先 (M26)
            if (!ctx.projectRoot.empty() && mye::LoadProjectManifest(ctx.projectRoot, manifest)) {
                scenePath = mye::ProjectBootScenePath(ctx.projectRoot, manifest);
            }
        }
        if (std::filesystem::exists(scenePath)) {
            mye::SceneSerializer::LoadFromFile(*ctx.scene, scenePath);
        } else {
            mye::BuildDemoScene(ctx); // ブートシーンが無ければデモを構築
        }
        // ランタイムは即 Play 相当。Editor の PlayModeController::Play と同じ Save+Load リロードで
        // EntityID を正規化する — これにより Editor が録った .rep と決定論的に一致する (M8 規約)。
        {
            const nlohmann::json snap = mye::SceneSerializer::SaveToJson(*ctx.scene);
            mye::SceneSerializer::LoadFromJson(*ctx.scene, snap);
        }
        if (startDeferred) {
            ctx.renderPath = ctx.renderPathDeferred;
        }
        MYE_LOG_INFO("RuntimeApp started (%u entities, scene=%s)",
                     ctx.scene->GetWorld().AliveCount(), mye::WideToUtf8(scenePath).c_str());
    }

    // ランタイムは常時シミュレート (Editor の Play 相当)
    void OnTick(mye::EngineContext& ctx) override { ctx.simulateScripts = true; }
};

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    AttachParentConsole();

    mye::EngineConfig config;
    config.title = L"MyEngine Runtime";
    config.renderSceneToBackbuffer = true; // シーンをバックバッファへ直接描画
    config.enableImGui = false;            // エディタ UI 無し

    RuntimeApp app;

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
            } else if (arg == L"--shot-every" && i + 1 < argc) {
                config.screenshotEvery = _wtoi64(argv[++i]);
            } else if (arg == L"--scene" && i + 1 < argc) {
                app.scenePath = argv[++i];
            } else if (arg == L"--replay-record" && i + 1 < argc) {
                config.replayRecordPath = argv[++i];
                config.vsync = false;
            } else if (arg == L"--replay-verify" && i + 1 < argc) {
                config.replayVerifyPath = argv[++i];
                config.vsync = false;
            } else if (arg == L"--replay-ticks" && i + 1 < argc) {
                config.replayTicks = _wtoi64(argv[++i]);
            } else if (arg == L"--deferred") {
                app.startDeferred = true;
            } else if (arg == L"--postfx-mode" && i + 1 < argc) {
                config.postFxTonemap = _wtoi(argv[++i]);
            } else if (arg == L"--no-postfx") {
                config.postFx = false;
            } else if (arg == L"--no-audio") {
                config.audio = false; // M45: XAudio2 を初期化しない (端末の無い CI / 撮影専用実行)
            } else if (arg == L"--exposure" && i + 1 < argc) {
                config.postFxExposure = static_cast<float>(_wtof(argv[++i]));
            } else if (arg == L"--no-bloom") {
                config.postFxBloom = false;
            } else if (arg == L"--bloom-threshold" && i + 1 < argc) {
                config.postFxBloomThreshold = static_cast<float>(_wtof(argv[++i]));
            } else if (arg == L"--bloom-intensity" && i + 1 < argc) {
                config.postFxBloomIntensity = static_cast<float>(_wtof(argv[++i]));
            } else if (arg == L"--no-fxaa") {
                config.postFxFxaa = false;
            } else if (arg == L"--no-jobs") {
                config.useJobs = false; // M25: 並列を直列化 (決定論ゲート / 計測比較)
            } else if (arg == L"--rt-debug" && i + 1 < argc) {
                config.rtDebugMode = _wtoi(argv[++i]); // M46b (Deferred のみ)
            } else if (arg == L"--rt-no-temporal") {
                config.rtTemporal = false; // M46d: 1spp 生のまま (A/B 計測用)
            } else if (arg == L"--rt-freeze-seed") {
                config.rtFreezeSeed = true; // M46d: 乱数列を進めない (決定的スクショ)
            } else if (arg == L"--rt-anim-seed") {
                config.rtAnimSeed = true; // M46d: スクショ時の自動 freeze を解除
            } else if (arg == L"--rt-no-svgf") {
                config.rtSvgf = false; // M46e: 空間フィルタ off (蓄積のみ = A/B 計測用)
            } else if (arg == L"--rt-gi") {
                config.rtGi = true; // M46f: GI を最終画像へ合成 (Deferred のみ)
            } else if (arg == L"--project" && i + 1 < argc) {
                // M26: プロジェクト指定。dist 配布物は従来どおり exe 隣の assets を自動発見する
                config.projectRoot = std::filesystem::absolute(argv[++i]).wstring();
            }
        }
        LocalFree(argv);
    }

    mye::EngineLoop loop;
    return loop.Run(config, app);
}
