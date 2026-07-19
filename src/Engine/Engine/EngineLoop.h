#pragma once
#include <cstdint>
#include <string>

#include "Engine/Platform/Input.h"

namespace mye {

class Win32Window;
class GraphicsDevice;
class SwapChain;
class Scene;
class ShaderManager;
class RenderSystem;
class IRenderPath;
class ReloadHub;
class ScriptHost;
class DllReloader;
struct RenderResources;

struct EngineConfig {
    std::wstring title = L"MyEngine";
    int width = 1600;
    int height = 900;
    int64_t maxFrames = -1;  // >0 でそのフレーム数後に自動終了 (スモークテスト / CI 用)
    bool enableImGui = true; // false = エディタ UI 無し (将来の Runtime.exe 用の余地)
    bool vsync = true;
    float clearColor[4] = { 0.08f, 0.09f, 0.11f, 1.0f };
    std::wstring screenshotPath; // 空でなければ screenshotFrame で PNG 保存 (検証用)
    int64_t screenshotFrame = 60;
    int64_t screenshotEvery = 0; // >0 で N フレーム毎に連番保存 (ライブ検証用)
    // true: シーンをバックバッファへ直接描画 (Runtime / M1 デモ)。
    // false: 描画は app の OnRenderViews に委ねる (エディタは SceneView/GameView の RT へ描く)
    bool renderSceneToBackbuffer = true;
};

// アプリ側 (Editor / Runtime) がサブシステムへアクセスするための窓口
struct EngineContext {
    Win32Window* window = nullptr;
    GraphicsDevice* device = nullptr;
    SwapChain* swapChain = nullptr;
    Scene* scene = nullptr;             // アクティブシーン (EngineLoop が所有)
    ShaderManager* shaders = nullptr;
    RenderResources* resources = nullptr;
    RenderSystem* renderSystem = nullptr;
    IRenderPath* renderPath = nullptr;  // 現在アクティブなパス (M6.5 で切替)
    ReloadHub* reloadHub = nullptr;
    ScriptHost* scriptHost = nullptr;
    DllReloader* dllReloader = nullptr;
    std::wstring assetsRoot;            // assets\ の絶対パス
    InputSnapshot input = {}; // 現フレームのスナップショット (tick 中も同一)
    // この tick でスクリプト層 (フェーズ 3/5) を実行するか。
    // エディタは OnTick で Play 状態に応じて設定する (Runtime は常に true)
    bool simulateScripts = true;
    uint64_t frameIndex = 0;  // 描画フレーム数
    uint64_t tickIndex = 0;   // 累計固定 tick 数 (シミュレーション時間 = tickIndex * fixedDt)
    float fixedDt = 1.0f / 60.0f;
    bool requestExit = false;
};

class IEngineApp {
public:
    virtual ~IEngineApp() = default;
    virtual void OnStart(EngineContext&) {}
    virtual void OnTick(EngineContext&) {}        // 固定 tick 毎 (spec 5.3 フェーズ 3 スロット)
    virtual void OnRenderViews(EngineContext&) {} // フェーズ 6: 独自 RT への描画 (エディタの SceneView 等)
    virtual void OnImGui(EngineContext&) {}       // 描画フレーム毎 (spec 5.3 フェーズ 8)
    virtual void OnShutdown(EngineContext&) {}
};

// メインループ (engine_spec.md 5.3)。
//
// 決定論のための設計判断 (ADR 候補):
//   シミュレーションは 60Hz 固定 tick で進み、構造変更の適用 (spec フェーズ 7) は
//   「フレーム末」ではなく「tick 末」に行う。1 フレームに複数 tick が走る場合でも
//   tick 列としての挙動が フレームレートに依存しなくなり、リプレイ再現 (spec 11.3) が
//   フレーム分割と無関係に成立する。フレーム構造:
//     1. 時間更新 / 入力スナップショット確定
//     2. ホットリロード適用 (セーフポイント)
//     [tick × N] 3. スクリプト Update → 4. システム → 5. LateUpdate → 7. 構造変更適用
//     6. シーン描画
//     8. ImGui 描画 / Present
class EngineLoop {
public:
    int Run(const EngineConfig& config, IEngineApp& app);
};

} // namespace mye
