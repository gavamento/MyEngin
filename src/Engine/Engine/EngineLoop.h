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
class UIRenderer;
class VfxRenderer;
class IRenderPath;
class ReloadHub;
class ScriptHost;
class DllReloader;
class ManagedHost;
class ParticleSystem;
class PrefabLibrary;
class AnimationLibrary;
class ControllerLibrary;
class AssetDatabase;
class AudioSystem;
class SoundLibrary;
class MixerLibrary;
class InputActions;
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

    // ---- ポストプロセス (M16) ----
    bool postFx = true;         // false で HDR 配管をバイパス (従来の直描き)
    int postFxTonemap = 1;      // 0=passthrough(配管検証) 1=ACES 2=Reinhard
    float postFxExposure = 1.0f;
    bool postFxBloom = true;
    float postFxBloomThreshold = 1.0f;
    float postFxBloomIntensity = 0.6f;
    bool postFxFxaa = true;

    // ---- ジョブシステム (M25) ----
    bool useJobs = true; // false で全並列を直列化 (決定論ゲート / 計測比較用)

    // ---- sim 索引 (M51a) ----
    // false で World クエリキャッシュ / Scene fileId 索引を素通しして線形経路に落とす
    // (決定論ゲート / 障害切り分け用)。結果はキャッシュ有無でビット同一
    bool useSimCache = true;

    // ---- アセットクックキャッシュ (M51b) ----
    // false で cache\cooked\ を読み書きせず毎回フルパース (障害切り分け / A-B 計測用)。
    // 登録される内容はクック有無でビット同一 (CookedCacheSelfTest + replay_verify が保証)
    bool useCookCache = true;

    // ---- レイトレのデバッグ表示 (M46b、--rt-debug N) ----
    // 0=off 1=BVH ヒートマップ 2=ヒット法線 3=インスタンス ID
    // 4=生 GI (1spp) 5=蓄積 GI 6=履歴長 (M46c/M46d) 7=SVGF 後 8=推定分散 (M46e)。
    // Deferred パスのみ効く。
    // 終了時に BVH の規模とトラバーサルの GPU 時間をログに出す (性能実測用)
    int rtDebugMode = 0;
    // M46d: テンポラル蓄積 (--rt-no-temporal で off = 1spp 生のまま。A/B 計測用)。
    // 乱数列の freeze は既定でスクショ/リプレイ時に自動 on (M46c)。
    // --rt-freeze-seed で常時 on、--rt-anim-seed でその自動 on を解除する
    // (蓄積のデノイズ効果をスクリーンショットに写すには後者が要る)
    bool rtTemporal = true;
    bool rtFreezeSeed = false;
    bool rtAnimSeed = false;
    // M46e: SVGF 空間フィルタ (--rt-no-svgf で off)。rtTemporal=false では元から動かない
    bool rtSvgf = true;
    // M46f: レイトレ GI を最終画像へ合成する (--rt-gi)。Deferred パスのみ。
    // off なら BVH の構築も転送も走らないので既定の描画経路は一切変わらない
    bool rtGi = false;
    // M46g: 平行光の影をレイトレで作る (--rt-shadow)。Deferred パスのみ。同上
    bool rtShadow = false;
    // M46h: スペキュラ環境項をレイトレ反射で置き換える (--rt-refl)。Deferred パスのみ。同上
    bool rtRefl = false;

    // ---- オーディオ (M45) ----
    // false (--no-audio) で XAudio2 を一切初期化しない。オーディオ端末の無い CI や
    // スクリーンショット専用実行で、デバイス確保とストリーミングスレッドを避けるため
    bool audio = true;

    // ---- プロジェクト (M26) ----
    // 空 = 従来動作 (FindAssetsRoot で単一リポジトリレイアウトを探索)。
    // 非空 = <projectRoot>\assets をアセットルートにする (--project で注入)
    std::wstring projectRoot;

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
    UIRenderer* uiRenderer = nullptr;          // ゲーム内 UI (M21)。GameView / Runtime が重ね描画
    VfxRenderer* vfx = nullptr;                // Sprite/Trail/TextMesh (M29c)。RenderSystem に渡す
    IRenderPath* renderPath = nullptr;         // 現在アクティブなパス (書き換えると切替)
    IRenderPath* renderPathForward = nullptr;  // 選択肢: Forward
    IRenderPath* renderPathDeferred = nullptr; // 選択肢: Deferred
    ReloadHub* reloadHub = nullptr;
    ScriptHost* scriptHost = nullptr;
    DllReloader* dllReloader = nullptr;
    ManagedHost* managedHost = nullptr; // C# スクリプトホスト (CoreCLR)。未導入時は null/未 ready
    ParticleSystem* particles = nullptr;
    PrefabLibrary* prefabs = nullptr;   // 登録済みプレハブ (.prefab.json) — Editor / ReloadHub が使う
    AnimationLibrary* anims = nullptr;  // 登録済み AnimationClip (.anim.json)
    ControllerLibrary* controllers = nullptr; // 登録済み Animator Controller (.controller.json、M22)
    AssetDatabase* assetDb = nullptr;   // GUID/.meta サイドカー DB (M23)。パス⇄GUID 解決
    // オーディオ (M45)。**決定論レーン外の出力 sink** — sim から再生位置や再生中判定を
    // 読み戻してはいけない (読んだ瞬間にリプレイが壊れる)。エディタの試聴/ミキサー用に公開する
    AudioSystem* audio = nullptr;
    SoundLibrary* sounds = nullptr;     // 登録済みサウンドアセット (.sound.json、M45c)
    MixerLibrary* mixers = nullptr;     // 登録済みミキサー (.mixer.json、M45d)。アクティブは 1 本
    std::wstring assetsRoot;            // assets\ の絶対パス
    std::wstring projectRoot;           // プロジェクトルート (M26)。レガシー起動時は空
    std::wstring imguiIniPath;          // imgui.ini の解決済みパス (レガシー時は L"imgui.ini")
    InputSnapshot input = {}; // 現フレームのスナップショット (tick 中も同一)
    // アクションマップ (M51d)。EngineLoop が所有し tick 頭に評価済み。
    // assets\input\actions.json が無ければ空マップ (ActionState/AxisValue は常に 0)
    InputActions* inputActions = nullptr;
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
