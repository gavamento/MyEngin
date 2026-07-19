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
class ParticleSystem;
class PrefabLibrary;
class AnimationLibrary;
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

    // ---- リプレイ一貫性検証 (engine_spec.md 11.3) ----
    std::wstring replayRecordPath; // 空でなければ記録モード (replayTicks 分記録して終了)
    std::wstring replayVerifyPath; // 空でなければ検証モード (全 tick 照合、exit code 0/1)
    int64_t replayTicks = 600;     // 記録する tick 数 (60Hz で 10 秒)
};

// フレーム計測 (Profiler ウィンドウ表示用)。EngineLoop が毎フレーム更新する
struct FrameTimings {
    float frameMs = 0.0f;   // フレーム全体
    float reloadMs = 0.0f;  // フェーズ 2 (ホットリロード)
    float tickMs = 0.0f;    // 全 tick (フェーズ 3-5,7)
    float renderMs = 0.0f;  // フェーズ 6 (シーン描画 + OnRenderViews)
    float presentMs = 0.0f; // フェーズ 8 (ImGui + Present)
    int ticksThisFrame = 0;
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
    IRenderPath* renderPath = nullptr;         // 現在アクティブなパス (書き換えると切替)
    IRenderPath* renderPathForward = nullptr;  // 選択肢: Forward
    IRenderPath* renderPathDeferred = nullptr; // 選択肢: Deferred
    ReloadHub* reloadHub = nullptr;
    ScriptHost* scriptHost = nullptr;
    DllReloader* dllReloader = nullptr;
    ParticleSystem* particles = nullptr;
    PrefabLibrary* prefabs = nullptr;   // 登録済みプレハブ (.prefab.json) — Editor / ReloadHub が使う
    AnimationLibrary* anims = nullptr;  // 登録済み AnimationClip (.anim.json)
    std::wstring assetsRoot;            // assets\ の絶対パス
    InputSnapshot input = {}; // 現フレームのスナップショット (tick 中も同一)
    // この tick でスクリプト層 (フェーズ 3/5) を実行するか。
    // エディタは OnTick で Play 状態に応じて設定する (Runtime は常に true)
    bool simulateScripts = true;
    uint64_t frameIndex = 0;  // 描画フレーム数
    uint64_t tickIndex = 0;   // 累計固定 tick 数 (シミュレーション時間 = tickIndex * fixedDt)
    float fixedDt = 1.0f / 60.0f;
    FrameTimings timings;     // 前フレームの計測値
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
