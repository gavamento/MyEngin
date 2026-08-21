#include "Engine/Engine/EngineLoop.h"

#include <algorithm>

#include "Engine/Core/AssetGuidResolver.h" // v8 PlayMusic の生クリップ経路 (GUID → 実パス)
#include "Engine/Core/Check.h"
#include "Engine/Core/JobSystem.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Profiler.h"
#include "Engine/Core/Random.h"
#include "Engine/Engine/Animation.h"
#include "Engine/Engine/AnimatorController.h"
#include "Engine/Engine/Asset/CookedCache.h"
#include "Engine/Engine/AssetDatabase.h"
#include "Engine/Engine/CollisionSystem.h"
#include "Engine/Engine/HotReload/DllReloader.h"
#include "Engine/Engine/HotReload/ReloadHub.h"
#include "Engine/Engine/Audio/AudioMixer.h"
#include "Engine/Engine/Audio/AudioSourceSystem.h"
#include "Engine/Engine/Audio/AudioSystem.h"
#include "Engine/Engine/Audio/SoundAsset.h"
#include "Engine/Engine/Particles/ParticleSystem.h"
#include "Engine/Engine/Physics/MeshColliderLibrary.h"
#include "Engine/Engine/Physics/PhysicsSystem.h"
#include "Engine/Engine/Prefab.h"
#include "Engine/Engine/Project.h"
#include "Engine/Engine/RenderSystem.h"
#include "Engine/Engine/Replay/Replay.h"
#include "Engine/Engine/Replay/SimSnapshot.h"
#include "Engine/Engine/Replay/TimeTravel.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/SaveGame.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Engine/SceneSerializer.h"
#include "Engine/Engine/Script/ManagedHost.h"
#include "Engine/Engine/EffectSystem.h"
#include "Engine/Engine/Script/ScriptHost.h"
#include "Engine/Engine/SchemaCodegen.h"
#include "Engine/Engine/SchemaComponents.h"
#include "Engine/Engine/PartFollowSystem.h"
#include "Engine/Engine/SkinningSystem.h"
#include "Engine/Engine/TickRunner.h"
#include "Engine/Engine/TransformSystem.h"
#include "Engine/Engine/UI/UIRenderer.h"
#include "Engine/Engine/Vfx/VfxRenderer.h"
#include "Engine/Platform/Clock.h"
#include "Engine/Platform/InputActions.h"
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
#include <fstream>

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
    InputActions inputActions;
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
    MeshColliderLibrary meshColliders; // 静的メッシュコライダーの BVH キャッシュ (M41)
    std::vector<SolidContact> solidContacts; // 物理→衝突イベントの tick 内受け渡し (M28c)
    PrefabLibrary prefabLibrary;
    AnimationLibrary animLibrary;
    AnimationSystem animationSystem;
    ControllerLibrary controllerLibrary;        // Animator Controller (.controller.json、M22)
    AnimatorControllerSystem controllerSystem;  // ステートマシン評価 + ブレンド
    AssetDatabase assetDatabase;                // GUID/.meta サイドカー DB (M23)
    SkinningSystem skinningSystem; // スケルタルアニメの時刻進行 (M18)
    PartFollowSystem partFollowSystem; // 部位のボーン追従 (M48g)
    EffectSystem effectSystem;     // 合成エフェクトのライフサイクル (M32e)
    UIRenderer uiRenderer;         // ゲーム内 UI (M21、backbuffer/GameView への重ね描画)
    VfxRenderer vfxRenderer;       // Sprite/Trail/TextMesh (M29c、メッシュ後・パーティクル前)
    AudioSystem audioSystem;       // XAudio2 (M19/M45、決定論レーン外の出力 sink)
    AudioSourceSystem audioSources; // AudioSource/AudioListener → X3DAudio (M45e)
    SoundLibrary soundLibrary;     // .sound.json (M45c)。PCM 実体は AudioSystem 側
    MixerLibrary mixerLibrary;     // .mixer.json (M45d)。バスグラフの実体は AudioSystem 側
    std::vector<ScriptAudioEvent> audioQueue; // スクリプトの再生イベント (tick 内で積む)
    // 再生ハンドルの予約カウンタ (M45)。**採番は push 側 = ゲートされない経路**で行う。
    // AudioSystem 側に置くと記録/検証中 (drain がゲートされる) だけ番号が進まず、
    // スクリプトが受け取る値がリプレイで食い違う。v7 Instantiate の fileId 予約と同型。
    uint64_t audioHandleSeq = 0;
    // スクリプト再生のバリエーション抽選 / 揺らぎ用 (M45g)。**world.Rng() とは別系統** —
    // RNG state はワールドハッシュ対象なので、オーディオが引くと sim が壊れる
    Pcg32 audioScriptRng;
    std::wstring pendingScene;                // LoadScene の遅延ロード先 (tick 末に消費)
    // M51g: SaveGame/LoadGame のスロット要求 (-1 = なし)。積み手はスクリプト API (v12)。
    // Save は tick 末ハッシュ後の出力レーンで書出、Load は pendingScene と同じ
    // セーフポイントで消費し record/verify 中は no-op + WARN (決定台帳 5)
    int pendingSaveSlot = -1;
    int pendingLoadSlot = -1;
    // M51h: SetPadVibration の目標値。スロットは書くだけで、適用はフレーム末の出力レーン
    // (record/verify 中とフォーカス喪失中は 0 に落とし、終了時も 0 リセット)
    PadVibrationState padVibration;
    std::vector<EffectSpawnRequest> effectQueue; // PlayEffect の spawn 要求 (tick 末に消費、M32f)
    std::vector<DebugLineCmd> debugLines; // DebugDrawLine (v7)。tick 頭クリア → 描画で消費
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
    if (!device.Init(config.forceWarp)) {
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

    // シェーダは 2 ルート解決: <assets>\shaders を先に見て、無ければエンジン組込みへ落ちる。
    // プロジェクトに同名を置けば上書きできる。レガシー起動では両者が同一なので 1 本に畳む。
    // 配布済み Runtime はリポジトリが無いので dist\assets\shaders のみ (DoPackage が同梱する)
    {
        std::vector<std::wstring> shaderDirs{ assetsRoot + L"\\shaders" };
        const std::wstring engineShaders = FindEngineShaderDir();
        if (!engineShaders.empty()
            && NormalizePathKey(engineShaders) != NormalizePathKey(shaderDirs.front())) {
            shaderDirs.push_back(engineShaders);
        }
        shaderManager.Init(device, std::move(shaderDirs));
    }
    resources.Init(device);
    // M41: 静的メッシュコライダー (Collider.shape=3)。pose 構築サイトが meshcol::Resolve で
    // AssetID → BVH 付きコライダーデータを引けるように接続する
    meshColliders.Init(&resources);
    meshcol::Install(&meshColliders);
    if (!forwardPath.Init(device, shaderManager)) {
        return 1;
    }
    if (!deferredPath.Init(device, shaderManager)) {
        return 1;
    }
    // M21: 失敗してもエンジンは継続 (UI が出ないだけ)。M52c: --font-embedded でフォント固定
    uiRenderer.Init(device, shaderManager, assetsRoot, config.fontEmbedded);
    vfxRenderer.Init(device, shaderManager, &uiRenderer); // M29c: 同上 (VFX が出ないだけ)
    reloadHub.Init(&shaderManager, &resources, &scene, &prefabLibrary, &animLibrary, &soundLibrary,
                   &mixerLibrary, &audioSystem, assetsRoot);
    particleSystem.Init(device, shaderManager, assetsRoot);

    // ポストプロセス設定を config から反映 (M16)。全ビューポート共通の renderSystem に載る
    renderSystem.enablePostFx = config.postFx;
    renderSystem.rtDebugMode = config.rtDebugMode; // M46b (--rt-debug N、Deferred のみ)
    renderSystem.rtTemporal = config.rtTemporal;   // M46d (--rt-no-temporal / --rt-freeze-seed)
    renderSystem.rtFreezeSeed = config.rtFreezeSeed;
    renderSystem.rtSvgf = config.rtSvgf;   // M46e (--rt-no-svgf)
    renderSystem.enableRtGi = config.rtGi;             // M46f (--rt-gi、Deferred のみ)
    renderSystem.enableRtShadow = config.rtShadow;     // M46g (--rt-shadow、Deferred のみ)
    renderSystem.enableRtRefl = config.rtRefl;         // M46h (--rt-refl、Deferred のみ)
    renderSystem.postFxSettings.tonemap = config.postFxTonemap;
    renderSystem.postFxSettings.exposure = config.postFxExposure;
    renderSystem.postFxSettings.bloom = config.postFxBloom;
    renderSystem.postFxSettings.bloomThreshold = config.postFxBloomThreshold;
    renderSystem.postFxSettings.bloomIntensity = config.postFxBloomIntensity;
    renderSystem.postFxSettings.fxaa = config.postFxFxaa;
    // M44b: リプレイ記録/検証・スクショの実行では露出適応を 1 フレーム収束にして
    // 決定的スクショを成立させる (aeInstant は Merge が base 維持する Settings 専用フィールド)
    renderSystem.postFxSettings.aeInstant = !config.replayVerifyPath.empty()
        || !config.replayRecordPath.empty() || !config.screenshotPath.empty();
    // M46c: 同じ理由でレイトレのサンプル列もフレームで進めない (毎フレーム同じノイズ =
    // スクリーンショットが決定的になる)。GPU 上で完結し読み戻さないので sim には無関係。
    // M46d: ただし freeze 中はテンポラル蓄積が「同じ 1 サンプルを積む」だけになり
    // デノイズ効果がスクショに写らない。--rt-anim-seed で自動 freeze を明示解除できる
    renderSystem.rtFreezeSeed =
        (config.rtFreezeSeed || renderSystem.postFxSettings.aeInstant) && !config.rtAnimSeed;

    // ---- スキーマ由来の動的コンポーネント (M48j) ----
    // ★呼ぶ位置がそのまま決定論の契約: 組込み型 (World の生成時に RegisterBuiltinComponents で
    //   済んでいる) の後、**GameLogic.dll のスクリプト型より前**。この 1 箇所に固定しておくと
    //   スキーマ型は組込み群とスクリプト群の間の連続ブロックになり、スクリプト型の TypeId は
    //   一様にずれるだけ = エンティティ内の相対順が変わらない = 既存シーンのハッシュ不変
    schema::RegisterSchemaComponents(assetsRoot);

    // GameLogic.dll (スクリプト層)。監視先は起動形態で 2 通りに分かれる:
    //   レガシー起動 (--project なし) = エンジンの exe と同じ構成のビルド出力。
    //     build\GameLogic.vcxproj が作るもので、replay_verify / selftest はこちらを使う
    //   プロジェクト起動 = <project>\cache\GameLogic.dll。
    //     エディタの Rebuild Scripts が cl.exe で直接ビルドする (vcxproj を介さない)
    // 分岐は assetsRoot ではなく projectRoot の有無で行う — assetsRoot 由来にすると
    // レガシー時に <repo>\cache\GameLogic.dll を見に行って既存の検証経路が壊れる
    scriptHost.Init(&scene);
    {
        const std::wstring cacheHot =
            (std::filesystem::path(assetsRoot).parent_path() / L"cache" / L"hot").wstring();
        const std::wstring dllPath = config.projectRoot.empty()
            ? GetExecutableDir() + L"\\GameLogic.dll"
            : config.projectRoot + L"\\cache\\GameLogic.dll";
        dllReloader.Init(&scriptHost, dllPath, cacheHot);
        dllReloader.LoadInitial();
    }

    // C# スクリプトホスト (CoreCLR)。未導入でも失敗ログのみでエンジンは継続する
    if (managedHost.Init(GetExecutableDir(), &scene)) {
        // スキーマ定数/アクセサを生成してからコンパイル (M50d)。Compile は
        // assets\scripts を再帰収集するので Generated\ は追加設定ゼロで混ざる
        schema::WriteCSharpBindings(assetsRoot);
        managedHost.CompileScripts(assetsRoot + L"\\scripts"); // 起動時に既存 C# を一括コンパイル
    }
    // シーン保存/復元時に C# コンポーネントのフィールドを永続化する hook を登録
    SceneSerializer::SetManagedHost(&managedHost);

    // オーディオ (M19/M45)。XAudio2 初期化 + 両ホストへ共有バッファ接続。
    // 初期化失敗 (ヘッドレス / --no-audio) でもエンジンは継続する (再生が no-op になるだけ)。
    // クリップの実ロードは M45c から RegisterAssetLibraries (assets\**\*.wav|*.ogg の走査) が
    // 担う — 単一ファイルのハードコードはここには置かない
    audioSystem.Init(config.audio);
    audioScriptRng.Seed(0x4D796541536372ull); // "MyeAScr" — world.Rng() とは別ストリーム
    scriptHost.SetSharedServices(&audioQueue, &pendingScene, &effectQueue, &debugLines,
                                 &audioHandleSeq, &inputActions, &pendingSaveSlot,
                                 &pendingLoadSlot, &padVibration);
    managedHost.SetSharedServices(&audioQueue, &pendingScene, &effectQueue, &debugLines,
                                  &audioHandleSeq, &inputActions, &pendingSaveSlot,
                                  &pendingLoadSlot, &padVibration);

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
    ctx.vfx = &vfxRenderer;
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
    ctx.audio = &audioSystem;
    ctx.sounds = &soundLibrary;
    ctx.mixers = &mixerLibrary;
    ctx.assetsRoot = assetsRoot;
    ctx.projectRoot = config.projectRoot;
    ctx.imguiIniPath = imguiIniPath;
    // M51d: 入力アクションマップ (assets\input\actions.json)。不在 = 空マップ = no-op
    inputActions.Load(assetsRoot);
    ctx.inputActions = &inputActions;

    // M23: assets\ を走査して .meta サイドカー (GUID) を生成/同期する。
    // アセット登録 (app.OnStart → RegisterAssetLibraries) の前に済ませ、パス⇄GUID 解決を利用可能にする。
    assetDatabase.ScanAndSync(assetsRoot);
    // M30c: 以後の path→AssetID キー計算 (IdForFile/HashForPath) を GUID 解決経由にする。
    // 未移動アセットは GUID == path-hash なので既存シーン/リプレイはビット不変
    assetDatabase.InstallAsKeyResolver();

    // M25: ジョブシステム起動 (min(16, cores-2) ワーカー)。--no-jobs で直列化。
    jobs::System().Init();
    jobs::System().SetEnabled(config.useJobs);
    MYE_LOG_INFO("[jobs] %s (%d workers)", config.useJobs ? "enabled" : "disabled (serial)",
                 jobs::System().WorkerCount());
    // M51a: sim 索引 (World クエリキャッシュ / Scene fileId 索引)。--no-sim-cache で素通し
    World::SetSimCacheEnabled(config.useSimCache);
    MYE_LOG_INFO("[simcache] %s", config.useSimCache ? "enabled" : "disabled (linear)");
    // M51b: アセットクックキャッシュ (モデル + .ogg PCM)。--no-cook-cache で毎回フルパース。
    // ディレクトリの二経路は GameLogic.dll と同じ規則 — 分岐は必ず projectRoot で判定する。
    // RegisterAssetLibraries (app.OnStart) より前に設定しておくこと
    {
        const std::wstring cookedDir =
            (config.projectRoot.empty() ? GetExecutableDir() : config.projectRoot)
            + L"\\cache\\cooked";
        CookedCache::Configure(cookedDir, config.useCookCache);
        MYE_LOG_INFO("[cook] %s (%s)",
                     config.useCookCache ? "enabled" : "disabled (parse every launch)",
                     WideToUtf8(cookedDir).c_str());
    }
    // M51g: セーブディレクトリ (SaveGame/LoadGame)。二経路は cache\cooked と同じ規則
    const std::wstring saveDir =
        (config.projectRoot.empty() ? GetExecutableDir() : config.projectRoot) + L"\\save";
    ctx.fixedDt = static_cast<float>(kFixedDt);

    app.OnStart(ctx);
    // OnStart で積まれた構造変更 (SetParent 等) を最初の描画前に反映する
    scene.GetWorld().ApplyStructuralChanges();
    MYE_LOG_INFO("Engine loop started (fixed dt = %.4f s, assets = %s)",
                 kFixedDt, WideToUtf8(assetsRoot).c_str());

    // M51d: 前 tick の入力 (アクションの pressed/released 判定用)。tick 0 はゼロ値 (決定台帳 4)
    InputSnapshot prevTickInput = {};

    // ---- sim スナップショットの参照束 (M52d) ----
    // 撮影対象は record/verify がハッシュを撮っている範囲と同一 (決定台帳 1)。
    // --snapshot-stress と、後続サブ (タイムトラベル / クラッシュ / ロールバック) が共有する
    SimRefs simRefs;
    simRefs.scene = &scene;
    simRefs.particles = &particleSystem.Cpu();
    simRefs.collision = &collisionSystem;
    simRefs.scripts = &scriptHost;
    simRefs.prevTickInput = &prevTickInput;
    // M52e: 音の再生ハンドル採番も sim へ戻る値なのでリングに載せる (SimSnapshot.h 参照)
    simRefs.audioHandleSeq = &audioHandleSeq;
    simRefs.tickIndex = &ctx.tickIndex;

    // ---- タイムトラベルのリング (M52e) ----
    // 有効化はエディタ (Play/Stop) か CLI プローブが行う。ここでは器を持つだけ
    TimeTravel timeTravel;
    ctx.timeTravel = &timeTravel;
    if (config.timeTravelProbeTicks > 0) {
        timeTravel.SetEnabled(true);
    }

    // ---- リプレイ記録/検証の準備 (spec 11.3) ----
    ReplayRecorder recorder;
    ReplayPlayer player;
    int exitCode = 0;
    if (!config.replayVerifyPath.empty()) {
        if (!player.Load(config.replayVerifyPath)) {
            return 1;
        }
        if (!player.Snapshot().empty()) {
            // v4 の埋め込み初期状態 (M52d)。**シーンの中身に依存せず**記録開始時点へ丸ごと
            // 戻せるので、配布ビルドで落ちた .rep をどのシーンからでも再生できる (M52f が本命)。
            // RNG も prevTickInput も blob に入っているので個別復元は不要
            if (!RestoreSimSnapshot(simRefs, player.Snapshot().data(), player.Snapshot().size())) {
                MYE_LOG_ERROR("[replay] embedded snapshot could not be restored");
                return 1;
            }
            MYE_LOG_INFO("[replay] restored embedded snapshot (%zu bytes)",
                         player.Snapshot().size());
        } else {
            // 記録開始時の RNG 状態を復元して同一 tick 列を再現する (v3 以来の従来経路)
            scene.GetWorld().Rng().Restore(player.RngState(), player.RngInc());
        }
    } else if (!config.replayRecordPath.empty()) {
        // --rep-snapshot: 記録開始時点の sim 状態を .rep の先頭へ埋め込む。
        // ここは app.OnStart + ApplyStructuralChanges の直後 = 構造変更が空の撮影点
        std::vector<std::byte> startSnapshot;
        if (config.replayEmbedSnapshot && !CaptureSimSnapshot(simRefs, startSnapshot)) {
            MYE_LOG_ERROR("[replay] could not capture the embedded snapshot");
            return 1;
        }
        recorder.Start(config.replayRecordPath, scene.GetWorld().Rng().State(),
                       scene.GetWorld().Rng().Inc(), scene.GetWorld().AliveCount(),
                       startSnapshot.data(), startSnapshot.size());
    }

    // ---- 決定的スクショ (M52c) ----
    // 「同じコマンドを 2 回叩いたら同じ PNG が出る」を成立させるモード。連番撮影
    // (--shot-every) は実時間で回したいライブ検証用なので対象外、--shot-realtime で明示解除。
    // ★frame 番号 == tick 番号になるので --shot-frame N は「N tick 目の絵」を指す
    const bool deterministicShot =
        !config.screenshotPath.empty() && config.screenshotEvery == 0 && !config.shotRealtime;
    if (deterministicShot) {
        MYE_LOG_INFO("[shot] deterministic capture: fixed dt + async texture drain "
                     "(frame index == tick index)");
    }

    // ---- メインループ (フェーズ構成は engine_spec.md 5.3 / ADR-005) ----
    double accumulator = 0.0;
    bool running = true;
    // M36b 描画補間: 前 tick 末のワールド行列 (tick 頭に採取)。record/verify 中は不使用
    PrevWorldStore prevWorld;
    bool lastTickSimulated = false;

    // --snapshot-stress の作業領域と実測 (M52d)。撮り直し用にバッファを 2 本持つのは、
    // 「撮る → 戻す → 撮り直してバイト比較」で非対称な復元をその場で捕まえるため
    std::vector<std::byte> stressBlob;
    std::vector<std::byte> stressBlobAgain;
    uint64_t stressCount = 0;
    size_t stressBytes = 0;
    double stressCaptureMs = 0.0;
    double stressRestoreMs = 0.0;

    // ---- tick 本体へ渡す参照束 (M52d) ----
    // 中身はすべてこのスコープのローカルなので、ループ前に 1 回組んで使い回す。
    // 「tick が何を消費するか」をこの 1 構造体に閉じたことで、後続サブ (タイムトラベル /
    // ロールバック) は別の入力源で同じ RunOneTick を回せる
    TickServices tickServices;
    tickServices.ctx = &ctx;
    tickServices.config = &config;
    tickServices.scene = &scene;
    tickServices.app = &app;
    tickServices.inputActions = &inputActions;
    tickServices.prevTickInput = &prevTickInput;
    tickServices.scriptHost = &scriptHost;
    tickServices.managedHost = &managedHost;
    tickServices.animationSystem = &animationSystem;
    tickServices.animLibrary = &animLibrary;
    tickServices.controllerSystem = &controllerSystem;
    tickServices.controllerLibrary = &controllerLibrary;
    tickServices.skinningSystem = &skinningSystem;
    tickServices.partFollowSystem = &partFollowSystem;
    tickServices.effectSystem = &effectSystem;
    tickServices.physicsSystem = &physicsSystem;
    tickServices.transformSystem = &transformSystem;
    tickServices.collisionSystem = &collisionSystem;
    tickServices.particleSystem = &particleSystem;
    tickServices.vfxRenderer = &vfxRenderer;
    tickServices.resources = &resources;
    tickServices.solidContacts = &solidContacts;
    tickServices.effectQueue = &effectQueue;
    tickServices.debugLines = &debugLines;
    tickServices.audioQueue = &audioQueue;
    tickServices.audioSystem = &audioSystem;
    tickServices.audioSources = &audioSources;
    tickServices.soundLibrary = &soundLibrary;
    tickServices.audioScriptRng = &audioScriptRng;
    tickServices.audioHandleSeq = &audioHandleSeq;
    tickServices.pendingScene = &pendingScene;
    tickServices.pendingSaveSlot = &pendingSaveSlot;
    tickServices.pendingLoadSlot = &pendingLoadSlot;
    tickServices.prefabLibrary = &prefabLibrary;
    tickServices.assetsRoot = &assetsRoot;
    tickServices.saveDir = &saveDir;
    tickServices.recorder = &recorder;
    tickServices.player = &player;
    tickServices.prevWorld = &prevWorld;
    tickServices.lastTickSimulated = &lastTickSimulated;
    tickServices.exitCode = &exitCode;

    // ---- タイムトラベルのシーク本体 (M52e) ----
    // 「target 以下の最寄りスナップショットへ Restore → 記録入力で target まで描画なし再シム」。
    // ★再シムは**通常 tick と同じ RunOneTick** を通す (決定台帳 2)。ここで tick を書き直すと
    //   「巻き戻したときだけ挙動が違う」種類のバグが必ず入る。
    // ★呼べるのは tick 境界だけ (構造変更が空)。フレーム頭から呼ぶので、直前フレームの
    //   ImGui が積んだ編集要求を先に捌いてから戻す (捌かないと破棄済み世界のコマンドが残る)
    const auto SeekTo = [&](uint64_t target) {
        SeekReport rep;
        rep.target = target;
        const double t0 = clock.Now();
        scene.GetWorld().ApplyStructuralChanges();
        if (target < ctx.tickIndex) {
            uint64_t snapTick = 0;
            const std::vector<std::byte>* blob = timeTravel.SnapshotAtOrBefore(target, snapTick);
            if (blob == nullptr
                || !RestoreSimSnapshot(simRefs, blob->data(), blob->size())) {
                MYE_LOG_ERROR("[timetravel] no restorable snapshot at or before tick %llu",
                              static_cast<unsigned long long>(target));
                rep.outcome = SeekOutcome::Failed;
                rep.ms = (clock.Now() - t0) * 1000.0;
                timeTravel.ReportSeek(rep);
                return rep;
            }
            rep.fromSnapshot = snapTick;
            // 非 sim レーンの見せ方は消費者の責務 (SimSnapshot.h、M52d 申し送り 6)。
            // タイムトラベルは「未来の残像」を消したいので、トレイルと GPU パーティクルを
            // 落とす。**CPU パーティクルの池は blob 側で戻っている**ので触ってはいけない
            vfxRenderer.Reset();
            particleSystem.Gpu().Reset();
        } else {
            // 前進シークは現在地からそのまま再シムする (戻す必要が無い)。
            // 「T-K へ戻ってから記録入力で T まで進めると元の T と一致する」という
            // プローブの主検査はこの経路を通る
            rep.fromSnapshot = ctx.tickIndex;
        }
        // ---- 記録入力で target まで再シム (描画なし) ----
        const bool savedSimulate = ctx.simulateScripts;
        const InputSnapshot savedInput = ctx.input;
        audioSystem.SetSuspended(true); // 立ち上がりで全停止 = 捨てた未来の音を断つ
        tickServices.app = nullptr;      // エディタ更新は回さない
        tickServices.recorder = nullptr; // 記録も照合もしない
        tickServices.player = nullptr;
        tickServices.prevWorld = nullptr;
        tickServices.resim = true;
        while (ctx.tickIndex < target) {
            const TimeTravelEntry* e = timeTravel.Entry(ctx.tickIndex);
            if (e == nullptr) {
                MYE_LOG_ERROR("[timetravel] missing input for tick %llu",
                              static_cast<unsigned long long>(ctx.tickIndex));
                rep.outcome = SeekOutcome::Failed;
                break;
            }
            ctx.input = e->input;
            // ★ポーズ中の tick も「進めない tick」として忠実になぞる —
            //   飛ばすと prevTickInput が食い違ってアクションの pressed/released が割れる
            ctx.simulateScripts = e->simulated;
            RunOneTick(tickServices);
            ++rep.resimTicks;
        }
        tickServices.app = &app;
        tickServices.recorder = &recorder;
        tickServices.player = &player;
        tickServices.prevWorld = &prevWorld;
        tickServices.resim = false;
        ctx.simulateScripts = savedSimulate;
        ctx.input = savedInput;
        audioSystem.SetSuspended(recorder.IsActive() || player.IsActive());
        // M36b の補間参照を捨てる: 過去へ飛んだ直後のフレームが「シーク前の行列」と
        // 混ざって 1 フレームだけ幽霊が出るのを防ぐ (Get() が null を返す = 補間しない)
        prevWorld.world.clear();
        prevWorld.generation.clear();
        if (rep.outcome != SeekOutcome::Failed) {
            // ★シークは毎回**自己検証する**。戻して同じ入力で回した結果が記録と
            //   ビット一致しなければ、決定論の外 (C# レーン等) が混ざっている証拠
            rep.expectedHash = timeTravel.HashAtTick(target);
            rep.actualHash = HashWorld(scene.GetWorld(), &particleSystem.Cpu(), &scene.Time(),
                                       &scene.Persist());
            rep.outcome = (rep.expectedHash == rep.actualHash) ? SeekOutcome::Ok
                                                              : SeekOutcome::HashMismatch;
        }
        rep.ms = (clock.Now() - t0) * 1000.0;
        timeTravel.ReportSeek(rep);
        MYE_LOG_INFO("[timetravel] seek to tick %llu from snapshot %llu (%llu ticks re-simulated, "
                     "%.2f ms) -> %s",
                     static_cast<unsigned long long>(rep.target),
                     static_cast<unsigned long long>(rep.fromSnapshot),
                     static_cast<unsigned long long>(rep.resimTicks), rep.ms,
                     rep.outcome == SeekOutcome::Ok
                         ? "hash OK"
                         : (rep.outcome == SeekOutcome::HashMismatch ? "HASH MISMATCH" : "FAILED"));
        return rep;
    };
    // --timetravel-selftest の進行状態 (M52e)。
    // 0 = 走行中 / 1 = シーク往復を検査済みでスクラブ静止の確認待ち /
    // 2 = 再開後の分岐の確認待ち / 3 = 終了
    int ttProbeStage = 0;
    int ttProbeFails = 0;
    int ttProbeFramesLeft = 0;
    uint64_t ttScrubTarget = 0;
    uint64_t ttPreScrubEnd = 0;

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
        const bool ttProbeRunning = config.timeTravelProbeTicks > 0 && ttProbeStage < 3;
        if (deterministicShot || ttProbeRunning) {
            // M52c: accumulator は毎フレームちょうど kFixedDt 増えて 1 tick 消費し 0 に戻る
            // (同じ double を足して引くので誤差ゼロ) = フレームと tick が 1:1 で固定される。
            // M52e のプローブも同じ扱い — 実時間で回すと 400 tick に 6.7 秒かかる
            dt = kFixedDt;
        }
        ctx.input = input.CaptureSnapshot();
        logging::SetCurrentFrame(ctx.frameIndex);
        prof::BeginFrame();

        // ---- フェーズ 2: ホットリロード適用セーフポイント ----
        const double tReload = clock.Now();
        reloadHub.Update();
        dllReloader.Update();
        resources.textures.PollAsyncLoads(); // M23: 非同期デコード完了分を GPU 公開 (セーフポイント)
        if (deterministicShot) {
            // M52c: 「間に合ったテクスチャだけが写る」を潰す。撮影経路だけの追加待ちなので
            // 通常のフレームレートには一切影響しない
            resources.textures.WaitForAsyncLoads();
        }
        FrameTimings timings;
        timings.reloadMs = static_cast<float>((clock.Now() - tReload) * 1000.0);
        const double tTicks = clock.Now();

        // ---- 固定 tick: フェーズ 3(Script) / 4(Systems) / 5(Late) / 7(構造変更) ----
        accumulator += dt;
        int ticks = 0;
        const bool verifying = player.IsActive();
        // ---- オーディオのゲート (M45): 記録/検証中はサスペンドする。
        // drain だけでなく **オーディオ更新フレーム全体** を止めるのが要点 — 検証中は
        // 1 フレームで最大 64 tick 回るので、ゲートが drain だけだと 3D 計算 (M45e) や
        // playOnAwake がその頻度で走ってしまう。サスペンドの立ち上がりで一度だけ全停止し、
        // サスペンド中も voice 回収 (Update) は動き続ける (止めるとスロットが枯れる) ----
        audioSystem.SetSuspended(recorder.IsActive() || verifying);

        // ---- タイムトラベル (M52e): リングの開始とシーク要求を tick 境界で捌く ----
        // ★どちらもフレーム頭 (= 前フレームの tick が終わった直後) でしか行わない。
        //   ImGui の途中で世界を差し替えると、その後のウィンドウが破棄済み EntityID を掴む
        // 記録/検証中は .rep がタイムラインの役なので、リングは起こさない
        // (起こすと 1 枚撮って entry が 1 つも積まれない空リングが残る)
        if (timeTravel.BeginPending() && !recorder.IsActive() && !verifying) {
            scene.GetWorld().ApplyStructuralChanges(); // 撮影点の前提 (構造変更が空)
            timeTravel.Begin(simRefs, ctx.tickIndex);
        }
        if (timeTravel.HasPendingSeek()) {
            const uint64_t target = timeTravel.PendingSeek();
            timeTravel.ClearPendingSeek();
            SeekTo(target);
        }
        // スクラブ中は tick を 1 本も進めない。
        // ★ここを止めないと、シーク直後のポーズ tick が「分岐」としてリングの未来を
        //   消してしまい、行ったり来たりのスクラブが成立しない (実装中に踏んだ罠)。
        //   ポーズ tick は sim を進めないが prevTickInput と tickIndex は動かすので、
        //   「ポーズしているから無害」ではない
        const bool scrubbing = timeTravel.Scrubbing();
        if (scrubbing) {
            accumulator = 0.0; // 再開時に溜まった分が一気に流れないように
        }
        // 検証モードは実時間と切り離して最速で回す (spec 11.3 の CLI 実行)
        const int maxTicksThisFrame = verifying ? 64 : kMaxTicksPerFrame;
        while (!scrubbing && ticks < maxTicksThisFrame
               && (verifying ? player.HasTick(ctx.tickIndex) : accumulator >= kFixedDt)) {
            if (verifying) {
                ctx.input = player.InputForTick(ctx.tickIndex); // フェーズ 1 の入力を置換
            }
            // ---- 固定 tick 本体 (M52d、決定台帳 2) ----
            // 通常 tick / タイムトラベル再シム / ロールバック再シムが通る唯一の実装。
            // ここでの仕事は「この tick が消費する入力を確定させて呼ぶ」だけ
            const uint64_t ranTick = ctx.tickIndex;
            RunOneTick(tickServices);
            // ---- タイムトラベルのリングへ記録 (M52e) ----
            // 記録/検証中は .rep がその役なので載せない。simulateScripts は
            // RunOneTick の中で app が決めた**その tick の実効値**を読む
            if (timeTravel.Enabled() && !recorder.IsActive() && !verifying) {
                timeTravel.OnTickEnd(simRefs, ranTick, ctx.input, ctx.simulateScripts);
            }
            // ---- スナップショット往復ストレス (M52d、--snapshot-stress N) ----
            // tick 境界 (= 構造変更が空でハッシュを撮ったのと同じ状態) で「撮る → 戻す →
            // 撮り直す」。sim の意味論は変わらないので、これを挟んでも .rep の期待ハッシュは
            // 全 tick 一致するはず。撮り直しのバイト比較まで見るのは、非対称な復元
            // (撮れているのに戻していない項目) をその場で捕まえるため
            if (config.snapshotStress > 0
                && ctx.tickIndex % static_cast<uint64_t>(config.snapshotStress) == 0) {
                const double tCap = clock.Now();
                bool ok = CaptureSimSnapshot(simRefs, stressBlob);
                const double tRes = clock.Now();
                ok = ok && RestoreSimSnapshot(simRefs, stressBlob.data(), stressBlob.size());
                const double tEnd = clock.Now();
                ok = ok && CaptureSimSnapshot(simRefs, stressBlobAgain)
                    && stressBlobAgain == stressBlob;
                if (!ok) {
                    MYE_LOG_ERROR("[snapshot] round-trip failed at tick %llu",
                                  static_cast<unsigned long long>(ctx.tickIndex));
                    exitCode = 1;
                    ctx.requestExit = true;
                }
                ++stressCount;
                stressBytes = stressBlob.size();
                stressCaptureMs += (tRes - tCap) * 1000.0;
                stressRestoreMs += (tEnd - tRes) * 1000.0;
            }
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
        // ---- タイムトラベルの自動プローブ (M52e、--timetravel-selftest N) ----
        // 「T まで進める → T-K へ戻す → 記録入力で T まで再シム → 元の T とハッシュ一致」を
        // 複数の K で実走する。エディタ GUI を開かずに巻き戻しの正しさを機械判定する唯一の口
        if (config.timeTravelProbeTicks > 0 && ttProbeStage == 0
            && ctx.tickIndex >= static_cast<uint64_t>(config.timeTravelProbeTicks)) {
            const uint64_t here = ctx.tickIndex;
            MYE_LOG_INFO("==== time travel probe: tick %llu, ring [%llu, %llu), %zu snapshots "
                         "(%zu KB) ====",
                         static_cast<unsigned long long>(here),
                         static_cast<unsigned long long>(timeTravel.FirstTick()),
                         static_cast<unsigned long long>(timeTravel.EndTick()),
                         timeTravel.SnapshotCount(), timeTravel.SnapshotBytes() / 1024);
            // K は「スナップショット間隔より短い / ちょうど / 跨ぐ / 大きく跨ぐ」を混ぜる。
            // 短い K は再シム 0 tick の縮退経路 (スナップショットそのものへ戻る) も踏む
            const uint64_t kList[] = { 1, 7, 30, 61, 120 };
            for (uint64_t k : kList) {
                if (here < k || here - k < timeTravel.FirstTick()) {
                    MYE_LOG_INFO("[timetravel]   K=%llu: skipped (outside the ring)",
                                 static_cast<unsigned long long>(k));
                    continue;
                }
                const SeekReport back = SeekTo(here - k);
                const SeekReport fwd = SeekTo(here);
                const bool ok = back.outcome == SeekOutcome::Ok && fwd.outcome == SeekOutcome::Ok;
                if (!ok) {
                    ++ttProbeFails;
                }
                MYE_LOG_INFO("[timetravel]   K=%llu: %s (back %llu ticks / forward %llu ticks)",
                             static_cast<unsigned long long>(k), ok ? "PASS" : "FAIL",
                             static_cast<unsigned long long>(back.resimTicks),
                             static_cast<unsigned long long>(fwd.resimTicks));
            }
            // ---- ここから先はリング操作の「生のループ上での」検査 ----
            // SeekTo を直接叩く上の検査では、スクラブ中に tick が止まることと、
            // 再開したときに未来が捨てられることが確かめられない。この 2 つは
            // 「ポーズ tick が黙ってリングの未来を食う」という一番痛い壊れ方の防波堤なので、
            // 実際に RequestSeek を出してフレームを回して確認する
            ttPreScrubEnd = timeTravel.EndTick();
            ttScrubTarget = (here >= timeTravel.FirstTick() + 60) ? here - 60
                                                                  : timeTravel.FirstTick();
            timeTravel.RequestSeek(ttScrubTarget);
            ttProbeFramesLeft = 3;
            ttProbeStage = 1;
        } else if (ttProbeStage == 1 && --ttProbeFramesLeft <= 0) {
            // 3 フレーム回した後: tick は 1 つも進んでおらず、記録済みの未来も残っているはず
            const bool still = ctx.tickIndex == ttScrubTarget;
            const bool futureKept = timeTravel.EndTick() == ttPreScrubEnd;
            if (!still || !futureKept || !timeTravel.Scrubbing()) {
                ++ttProbeFails;
            }
            MYE_LOG_INFO("[timetravel]   scrub hold: %s (tick %llu, ring end %llu)",
                         (still && futureKept) ? "PASS" : "FAIL",
                         static_cast<unsigned long long>(ctx.tickIndex),
                         static_cast<unsigned long long>(timeTravel.EndTick()));
            timeTravel.EndScrub(); // 再生を再開した = ここから分岐する
            ttProbeStage = 2;
        } else if (ttProbeStage == 2 && ctx.tickIndex > ttScrubTarget) {
            const bool branched = timeTravel.EndTick() == ctx.tickIndex
                && timeTravel.EndTick() < ttPreScrubEnd;
            if (!branched) {
                ++ttProbeFails;
            }
            MYE_LOG_INFO("[timetravel]   branch: %s (ring end %llu, was %llu)",
                         branched ? "PASS" : "FAIL",
                         static_cast<unsigned long long>(timeTravel.EndTick()),
                         static_cast<unsigned long long>(ttPreScrubEnd));
            if (ttProbeFails == 0) {
                MYE_LOG_INFO("==== time travel probe: ALL PASS ====");
            } else {
                MYE_LOG_ERROR("==== time travel probe: %d FAILED ====", ttProbeFails);
                exitCode = 1;
            }
            ttProbeStage = 3;
            ctx.requestExit = true;
        }
        if (verifying && !player.failed && !player.HasTick(ctx.tickIndex)) {
            MYE_LOG_INFO("[replay] VERIFY PASS: %llu ticks hash-identical",
                         static_cast<unsigned long long>(player.verifiedTicks));
            ctx.requestExit = true;
        }
        timings.tickMs = static_cast<float>((clock.Now() - tTicks) * 1000.0);
        timings.ticksThisFrame = ticks;
        // オーディオのフレーム更新 (M19: voice 回収 / M45e: 3D 定位)。
        // ★この位置は意図的で、動かしてはいけない: フレーム末の transformSystem.Update()
        //   (下の「ワールド行列は描画直前に一括更新」) より **前** なので、ここで読める
        //   WorldMatrix は tick 内 (フェーズ 4) で確定した「直前 tick の値」そのものになる。
        //   Runtime は vsync 無効で数千 fps 回るため、フレーム差分で速度を出すと値がノイズに
        //   なる — M45e のドップラーは tick 差分で速度を取る前提でここに置いている。
        //   M45e: AudioSource/AudioListener を先に処理して定位を確定させ、その後に
        //   voice 回収を回す (回収でスロットが空くのは次フレームからで良い)
        audioSources.Update(scene.GetWorld(), audioSystem, soundLibrary, ctx.tickIndex,
                            ctx.fixedDt, ctx.simulateScripts);
        // dt は**実時間**を渡す (M45f の BGM クロスフェードは絶対経過時間で進むため。
        // 固定 dt を渡すと 6500fps ではフェードが 100 倍速で終わる)
        audioSystem.Update(static_cast<float>(dt));
        // ---- パッド振動の適用 (M51h、出力レーン)。record/verify 中は 0 に落とす
        // (オーディオ suspend と同型)。フォーカス喪失中も 0 (裏で振動し続けない)。
        // 実際の XInputSetState は Input 側が値の変化時だけ発行する ----
        {
            // M52e: スクラブ中も 0 (止まった世界の裏で振動し続けない = 出力レーンの抑止)
            const bool vibSuspend = recorder.IsActive() || player.IsActive()
                || !window.HasFocus() || timeTravel.Scrubbing();
            input.ApplyVibration(vibSuspend ? 0.0f : padVibration.left,
                                 vibSuspend ? 0.0f : padVibration.right);
        }
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

            // M36b: 補間係数 (accumulator の残り比)。Play 中のみ有効 —
            // 編集中 / record / verify は 1.0 固定 = 従来描画 (リプレイ透過の保証)
            const bool interpOk =
                lastTickSimulated && !recorder.IsActive() && !player.IsActive();
            renderSystem.interpAlpha = interpOk
                ? std::clamp(static_cast<float>(accumulator / kFixedDt), 0.0f, 1.0f)
                : 1.0f;
            renderSystem.prevWorld = &prevWorld;
            renderSystem.debugLines = &debugLines; // v7 DebugDrawLine (M37)

            app.OnRenderViews(ctx); // エディタの SceneView / GameView (独自 RT)

            FrameTarget target;
            target.rtv = swapChain.BackbufferRTV();
            target.dsv = swapChain.DepthDSV();
            target.width = swapChain.Width();
            target.height = swapChain.Height();
            memcpy(target.clearColor, config.clearColor, sizeof(target.clearColor));
            target.depthSRV = swapChain.DepthSRV();          // M42a
            target.dsvReadOnly = swapChain.DepthDSVReadOnly();
            target.viewKey = 1; // runtime バックバッファ
            if (config.renderSceneToBackbuffer) {
                renderSystem.Render(scene.GetWorld(), device, *activePath, shaderManager, resources,
                                    target, nullptr, &particleSystem, &vfxRenderer);
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
    input.ApplyVibration(0.0f, 0.0f); // M51h: 終了後に振動を残さない
    app.OnShutdown(ctx);
    meshcol::Install(nullptr); // M41 (meshColliders 破棄前に必ず外す)
    AssetDatabase::UninstallKeyResolver(); // M30c (assetDatabase 破棄前に必ず外す)
    vfxRenderer.Shutdown(); // M29c
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
    if (config.rtDebugMode != 0 || config.rtGi || config.rtShadow || config.rtRefl) {
        // M46b: BVH の規模とソフトウェアトラバーサルの実測値 (性能ゲートの一次データ)
        MYE_LOG_INFO("[rt] mode %d: %d instances / %d triangles / build %.3f ms (CPU) / "
                     "trace %.3f ms / gi %.3f ms / temporal %.3f ms / svgf %.3f ms / "
                     "shadow %.3f ms (+ filter %.3f ms) / refl %.3f ms (+ denoise %.3f ms) "
                     "(GPU, last frame)",
                     config.rtDebugMode, renderSystem.RtInstanceCount(),
                     renderSystem.RtTriangleCount(), renderSystem.RtBuildCpuMs(),
                     renderSystem.RtDebugGpuMs(), renderSystem.RtGiGpuMs(),
                     renderSystem.RtTemporalGpuMs(), renderSystem.RtSvgfGpuMs(),
                     renderSystem.RtShadowGpuMs(), renderSystem.RtShadowFilterGpuMs(),
                     renderSystem.RtReflGpuMs(), renderSystem.RtReflDenoiseGpuMs());
    }
    if (stressCount > 0) {
        // 1 枚のバイト数と往復の実測 (M52e のリング枚数 / M52i のロールバック予算の一次データ)
        MYE_LOG_INFO("[snapshot] %llu round-trips: %zu bytes/snapshot, "
                     "capture %.3f ms avg / restore %.3f ms avg",
                     static_cast<unsigned long long>(stressCount), stressBytes,
                     stressCaptureMs / static_cast<double>(stressCount),
                     stressRestoreMs / static_cast<double>(stressCount));
    }
    MYE_LOG_INFO("Engine loop finished (%llu frames, %llu ticks)",
                 static_cast<unsigned long long>(ctx.frameIndex),
                 static_cast<unsigned long long>(ctx.tickIndex));
    return exitCode;
}

} // namespace mye
