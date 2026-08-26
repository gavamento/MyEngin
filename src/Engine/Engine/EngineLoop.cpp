#include "Engine/Engine/EngineLoop.h"

#include <algorithm>
#include <cstring>
#include <memory>

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
#include "Engine/Engine/Physics/ConvexColliderLibrary.h"
#include "Engine/Engine/Physics/MeshColliderLibrary.h"
#include "Engine/Engine/Physics/PhysMatLibrary.h"
#include "Engine/Engine/Physics/XpbdBackend.h" // M60'b: 変形体の粒子池
#include "Engine/Engine/Physics/TerrainColliderLibrary.h"
#include "Engine/Engine/Physics/PhysicsSystem.h"
#include "Engine/Engine/Prefab.h"
#include "Engine/Engine/ProbeBaker.h"
#include "Engine/Engine/Project.h"
#include "Engine/Engine/RenderSystem.h"
#include "Engine/Engine/Net/NetRollback.h"
#include "Engine/Engine/Net/NetRuntime.h"
#include "Engine/Engine/Net/NetSession.h"
#include "Engine/Engine/Replay/CrashRing.h"
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
#include "Engine/Engine/UI/UILayout.h" // ワールド追従 UI の射影コンテキスト
#include "Engine/Engine/UI/UIRenderer.h"
#include "Engine/Engine/Vfx/VfxRenderer.h"
#include "Engine/Platform/Clock.h"
#include "Engine/Platform/CrashHandler.h"
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
#include "Shared/EngineAPI.h" // MYE_API_VERSION (ネットのハンドシェイクで照合する)

#include <filesystem>
#include <fstream>

#include <Windows.h>

namespace mye {
namespace {

constexpr double kFixedDt = 1.0 / 60.0;
constexpr int kMaxTicksPerFrame = 5;   // スパイラルオブデス防止
constexpr double kMaxFrameDt = 0.25;   // ブレークポイント等の巨大 dt をクランプ
// ネットの入力待ち (M52h)。届いていない tick をこの時間だけ受信を回して待つ。
// 長くするとフレームが詰まり、短くすると 1 フレーム空振りして描画が 1 回無駄になる —
// どちらも sim には影響しない (待つのは「いつ回すか」だけ) ので体感で決めてよい値
constexpr uint32_t kNetStallWaitMs = 6;

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
    ConvexColliderLibrary convexColliders;   // 凸包コライダー + .mcvx クック (M60f)
    PhysMatLibrary physMatLibrary;     // .physmat.json (M59a1)。sim の消費は M59a2 から
    TerrainColliderLibrary terrainColliders; // 地形コライダー (M59i)。**描画側とは別キャッシュ**
    // XPBD 変形体の粒子池 (M60'b)。ECS 外 sim 状態の 2 例目 — ハッシュ節 (SimSources) と
    // snapshot 節 (SimRefs) の両方へ必ず配線する (3 点セット契約)
    XpbdBackend xpbd;
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
    // 高 DPI で UI を実寸へ (テーマ第 3 世代)。--screenshot 中は 1.0 固定 — 撮影サイズは
    // 論理ピクセル指定なので、撮った機械の DPI で golden が変わってはならない
    imguiOpts.dpiScale = config.screenshotPath.empty() ? window.DpiScale() : 1.0f;
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
    // M60f: 凸包コライダー (Collider.shape=5)。meshcol と同じ AssetID→形状の解決だが
    // クック (.mcvx) が乗るので CookedCache::Configure より後で使われること (Get は lazy)
    convexColliders.Init(&resources);
    convexcol::Install(&convexColliders);
    // M59a1: 物理マテリアル (.physmat.json)。起動走査 (RegisterAssetLibraries) と ReloadHub が
    // physmat::Library() 経由で読み込むので、走査より前に注入しておくこと
    physmat::Install(&physMatLibrary);
    // M59i: 地形コライダー (Collider.shape=4)。描画の TerrainSystem とは別に sim 用の
    // 地形データを持つ — 描画のキャッシュを読むと「絵を出したかどうか」で sim が変わる
    terraincol::Install(&terrainColliders);
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
    renderSystem.assetsRoot = assetsRoot; // M58c: TerrainComponent.source の解決基点
    renderSystem.enablePostFx = config.postFx;
    renderSystem.rtDebugMode = config.rtDebugMode; // M46b (--rt-debug N、Deferred のみ)
    renderSystem.velocityDebugMode = config.velocityDebug; // M55c (--velocity-debug)
    renderSystem.hzbDebugMip = config.hzbDebug;            // M56c (--hzb-debug N)
    renderSystem.enableSsr = config.ssr;                   // M56d (--ssr、Deferred のみ)
    renderSystem.rtTemporal = config.rtTemporal;   // M46d (--rt-no-temporal / --rt-freeze-seed)
    renderSystem.rtFreezeSeed = config.rtFreezeSeed;
    renderSystem.rtSvgf = config.rtSvgf;   // M46e (--rt-no-svgf)
    renderSystem.enableRtGi = config.rtGi;             // M46f (--rt-gi、Deferred のみ)
    renderSystem.enableRtShadow = config.rtShadow;     // M46g (--rt-shadow、Deferred のみ)
    renderSystem.enableRtRefl = config.rtRefl;         // M46h (--rt-refl、Deferred のみ)
    renderSystem.enableFroxel = config.froxel;         // M57b (--froxel。まだ絵は変わらない)
    renderSystem.froxelSettings.temporal = config.froxelTemporal; // M57c (--froxel-no-temporal)
    renderSystem.froxelDumpFrame = config.froxelDumpFrame; // M57b/M57c (--froxel-dump N)
    renderSystem.postFxSettings.tonemap = config.postFxTonemap;
    renderSystem.postFxSettings.exposure = config.postFxExposure;
    renderSystem.postFxSettings.bloom = config.postFxBloom;
    renderSystem.postFxSettings.bloomThreshold = config.postFxBloomThreshold;
    renderSystem.postFxSettings.bloomIntensity = config.postFxBloomIntensity;
    renderSystem.postFxSettings.fxaa = config.postFxFxaa;
    renderSystem.postFxSettings.taaOn = config.postFxTaa ? 1 : 0; // M55d (--taa、Deferred のみ)
    renderSystem.postFxSettings.motionBlurIntensity = config.postFxMotionBlur; // M55e
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
        if (!dllReloader.LoadInitial()) {
            // ★黙って続けない。C++ スクリプトが 1 本も無い世界は「動くけれど別物」で、
            //   リプレイもネットも全く違う結果になる (M52h でシャドウコピーの衝突により
            //   実際に踏んだ)。エンジンは継続できるので停止まではしないが、
            //   ログ上で必ず目立たせる
            MYE_LOG_ERROR("[dll] GameLogic.dll was not loaded - NO C++ scripts are registered "
                          "(the world will not match a normal run)");
        }
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
    // v13 (M52i): ネットセッションの状態 POD。**宣言はここ** (スクリプトへ配線する
    // 時点で生きていること = Run のスコープ)。中身を書くのはフレーム末の 1 か所だけ
    NetRuntimeInfo netInfo;
    scriptHost.SetSharedServices(&audioQueue, &pendingScene, &effectQueue, &debugLines,
                                 &audioHandleSeq, &inputActions, &pendingSaveSlot,
                                 &pendingLoadSlot, &padVibration, &netInfo);
    managedHost.SetSharedServices(&audioQueue, &pendingScene, &effectQueue, &debugLines,
                                  &audioHandleSeq, &inputActions, &pendingSaveSlot,
                                  &pendingLoadSlot, &padVibration, &netInfo);

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

    // ---- クラッシュバンドル (M52f) ----
    // ★設置は app.OnStart の**前**。シーンロードやスクリプト初期化で落ちるのは
    //   もっともありふれた壊れ方で、そこを取りこぼすとハンドラの価値が半分になる。
    //   この時点ではリングがまだ空 (crash.rep 無し) だが、minidump と crash.txt は残る。
    // リングとペイロードは Run のスコープに置く = ハンドラが掴むポインタはこの関数が
    // 生きている間だけ有効。解除は下の CrashHandlerScope (RAII) が受け持つ
    CrashRing crashRing;
    CrashPayload crashPayload;
    crashPayload.ring = &crashRing;
    const CrashTestKind crashTestKind = static_cast<CrashTestKind>(config.crashTest);
    if (config.crashHandler) {
        CrashHandlerConfig crashCfg;
        crashCfg.crashRoot =
            config.projectRoot.empty() ? GetExecutableDir() : config.projectRoot;
        crashCfg.appName = config.title;
        crashCfg.tickIndex = &ctx.tickIndex;
        crashCfg.frameIndex = &ctx.frameIndex;
        crashCfg.sceneLabel = crashPayload.sceneSource;
        crashCfg.payload = &WriteCrashPayload;
        crashCfg.payloadUser = &crashPayload;
        InstallCrashHandler(crashCfg);
    }
    // ★Install 以降には early return が何本かある (埋め込みスナップショットの復元失敗など)。
    //   そこで外し忘れると、破棄済みの ctx / crashRing を指したハンドラが Run を抜けた後も
    //   居座る。RAII に任せて経路を数えないで済ませる — この guard は crashRing /
    //   crashPayload / ctx より**後**に宣言してあるので、必ずそれらより先に走る
    struct CrashHandlerScope {
        ~CrashHandlerScope() { UninstallCrashHandler(); }
    } crashHandlerScope;

    app.OnStart(ctx);
    // OnStart で積まれた構造変更 (SetParent 等) を最初の描画前に反映する
    scene.GetWorld().ApplyStructuralChanges();
    MYE_LOG_INFO("Engine loop started (fixed dt = %.4f s, assets = %s)",
                 kFixedDt, WideToUtf8(assetsRoot).c_str());

    // M51d: 前 tick の入力 (アクションの pressed/released 判定用)。tick 0 はゼロ値 (決定台帳 4)。
    // M52g: レーンごとに持つ。**常に kMaxPlayers 本**確保する — 実効レーン数で伸縮させると
    // スナップショット blob のレイアウトが起動オプション依存になる (SimSnapshot.cpp 参照)
    InputSnapshot prevTickInput[kMaxPlayers] = {};

    // ---- sim スナップショットの参照束 (M52d) ----
    // 撮影対象は record/verify がハッシュを撮っている範囲と同一 (決定台帳 1)。
    // --snapshot-stress と、後続サブ (タイムトラベル / クラッシュ / ロールバック) が共有する
    SimRefs simRefs;
    simRefs.scene = &scene;
    simRefs.particles = &particleSystem.Cpu();
    simRefs.xpbd = &xpbd; // M60'b: ハッシュ (SimSources) と対で撮る
    simRefs.collision = &collisionSystem;
    simRefs.scripts = &scriptHost;
    simRefs.prevTickInput = prevTickInput;
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

    // ---- 入力レーン数の確定 (M52g) ----
    // 走行中は変えない。**検証時だけは .rep の playerCount が指定に勝つ** —
    // tick レコード長がファイル側で決まっているので、ここで食い違うと入力が
    // 1 レーンぶんずつずれて読まれる (無音で全 tick MISMATCH になる最悪の壊れ方)
    ctx.playerCount = 1;
    if (config.localPlayers > 1) {
        ctx.playerCount = (config.localPlayers >= static_cast<int>(kMaxPlayers))
            ? kMaxPlayers
            : static_cast<uint32_t>(config.localPlayers);
    }

    // ---- ネット対戦セッション (M52h、決定台帳 5) ----
    // ここでやることは 2 つだけ: 「同じものを走らせているか」を入口で照合することと、
    // 消費するレーン数を確定させること。以降、tick が何を消費するかは NetSession が
    // そろえた確定入力だけで決まる (= 2 台の .rep がバイト一致する根拠)
    NetSession net;
    InputSnapshot netLiveInput = {}; // フレーム頭のライブ入力 (自レーンぶん)
    // ---- 予測ロールバック (M52i) ----
    // netRb   … 投機記録 + 巻き戻し先スナップショットのリング
    // netInfo … 上位 (エディタの NetWindow / ABI v13 のスロット) へ見せる POD。
    //           **毎フレーム 1 回だけ EngineLoop が書く** (読み手は触らない)
    NetRollback netRb;
    bool netRollbackActive = false;
    bool netDesync = false;
    uint64_t netDesyncTick = 0;
    uint64_t netDesyncScan = 0; // 次に突き合わせる checkpoint tick (M52i)
    // ★接続できなかったときは **return せずにフレームループを 0 周で抜ける**。
    //   ここで早期 return すると下の Shutdown 群 (ジョブのワーカー join / CoreCLR /
    //   D3D) を素通りして、デストラクタ順で abort する = 「相手に断られた」だけなのに
    //   クラッシュハンドラがバンドルを吐く (実装中に実測: exit code 3 + crash\<stamp>\)。
    //   ネットは「繋がらない」が日常的に起きるレーンなので、そこを異常終了にしない
    bool netFailed = false;
    // ★--replay-verify はネットに勝つ。.rep には全レーンの入力が既に入っているので、
    //   そこへネット越しの入力を混ぜたら検証にならない
    const bool netEnabled = config.netRole != 0 && config.replayVerifyPath.empty();
    if (config.netRole != 0 && !config.replayVerifyPath.empty()) {
        MYE_LOG_WARN("[net] --replay-verify wins over the net session (the .rep already carries "
                     "every lane's input)");
    }
    // 自レーンの tick 入力。**「tick T の値は T と入力源だけで決まる」**ようにしてあるので、
    // 先出し (priming) でも通常 tick でも同じ関数で作れる。合成入力のときは
    // SynthLaneInput(T, 自レーン) なので、2 台合わせた列は
    // 「--local-players 2 --synth-input のローカル実行」と 1 バイトも変わらない
    // (net_verify.bat はそこまで含めて照合する)
    const auto NetLocalInput = [&](uint64_t targetTick) -> InputSnapshot {
        return config.synthInput ? SynthLaneInput(targetTick, net.LocalPlayerIndex())
                                 : netLiveInput;
    };
    if (netEnabled) {
        NetConfig ncfg;
        ncfg.role = static_cast<NetRole>(config.netRole);
        ncfg.port = static_cast<uint16_t>(config.netPort);
        ncfg.joinTarget = config.netJoinTarget;
        ncfg.playerCount = static_cast<uint32_t>(config.netPlayers);
        ncfg.inputDelay = static_cast<uint32_t>(config.netInputDelay);
        ncfg.lossPercent = static_cast<uint32_t>(config.netLossPercent);
        // ★M52 は 2 人 P2P に限定 (計画「見送り」)。3 人以上はメッシュ / リレー /
        //   NAT 越えが要るので、黙って動くふりをせずここで止める
        if (ncfg.playerCount != 2) {
            MYE_LOG_ERROR("[net] --net-players %u is not supported: this build does 2-player "
                          "peer-to-peer only", ncfg.playerCount);
            netFailed = true;
        }
        NetIdentity id;
        id.apiVersion = MYE_API_VERSION;
        id.repVersion = kReplayFileVersion;
        id.snapshotVersion = kSimSnapshotVersion;
        id.playerCount = ncfg.playerCount;
        id.inputDelay = ncfg.inputDelay;
        id.configBits = (config.synthInput ? kNetCfgSynthInput : 0u)
            | (config.useJobs ? kNetCfgJobs : 0u) | (config.useSimCache ? kNetCfgSimCache : 0u)
            | (config.useCookCache ? kNetCfgCookCache : 0u);
        // 開始点のワールドハッシュ。**tick 末にハッシュを撮るのと同じ点** (OnStart +
        // ApplyStructuralChanges の直後) で撮る = 「同じシーンから始めたか」の機械照合
        id.startWorldHash = HashWorld(scene.GetWorld(),
                                      {&particleSystem.Cpu(), &scene.Time(), &scene.Persist(), &xpbd});
        const bool ok = !netFailed && net.Start(ncfg, id, ctx.tickIndex)
            && net.WaitUntilReady([&window] { return window.PumpMessages(); });
        if (!ok) {
            MYE_LOG_ERROR("[net] could not establish the session - exiting");
            netFailed = true;
        }
        ctx.playerCount = ncfg.playerCount;
        // ---- 先出し (priming) ----
        // 入力遅延 N tick = 「tick T が消費するのは T-N フレームで確定した入力」。
        // 最初の N tick には対応する過去が無いので、ここで作って送っておく。
        // ★これをやらないと tick 0 の時点で自レーンが空 = 双方が永久に stall する
        for (uint32_t k = 0; k < ncfg.inputDelay && !netFailed; ++k) {
            const uint64_t target = ctx.tickIndex + k;
            net.SubmitLocalInput(target, NetLocalInput(target));
        }
        // ---- ロールバックのリング開始 (M52i) ----
        // ★撮影点は startWorldHash を撮ったのと**同じ点** (OnStart + 構造変更適用の直後)。
        //   ここが「tick startTick が走る前」の状態 = 最初の巻き戻し先になる
        if (!netFailed && config.netRollback) {
            netRollbackActive = netRb.Begin(simRefs, ctx.tickIndex);
        }
        if (!netFailed) {
            MYE_LOG_INFO("[net] lockstep ready: local lane %u of %u, input delay %u ticks, "
                         "rollback %s (max %u ticks ahead)",
                         net.LocalPlayerIndex(), ctx.playerCount, ncfg.inputDelay,
                         netRollbackActive ? "on" : "off", kNetMaxSpeculation);
        }
        netInfo.active = true;
        netInfo.rollbackEnabled = netRollbackActive;
        netInfo.role = config.netRole;
        ctx.net = &netInfo;
        // 最初の checkpoint (開始 tick 以上で kNetHashCheckpoint の倍数)
        netDesyncScan = ((ctx.tickIndex + kNetHashCheckpoint - 1) / kNetHashCheckpoint)
            * kNetHashCheckpoint;
    }

    // ---- リプレイ記録/検証の準備 (spec 11.3) ----
    ReplayRecorder recorder;
    ReplayPlayer player;
    int exitCode = netFailed ? 1 : 0;

    // ---- 反射プローブのベイカ (M56e、--probe-bake のときだけ実体を持つ) ----
    // 専用の RenderSystem を内側に抱えるので、使わない実行では確保もしない
    std::unique_ptr<ProbeBaker> probeBaker;
    bool probeBaked = false;
    // M56f: 焼き上がった束 (テクスチャの所有者)。renderSystem がここを指す
    ReflectionProbeArray probeArray;
    if (config.probeBake || config.probeBakeAll) {
        probeBaker = std::make_unique<ProbeBaker>();
        probeBaker->assetsRoot = ctx.assetsRoot; // 地形もキャプチャに写す
        memcpy(probeBaker->clearColor, config.clearColor, sizeof(probeBaker->clearColor));
    }
    if (!config.replayVerifyPath.empty()) {
        if (!player.Load(config.replayVerifyPath)) {
            return 1;
        }
        if (player.PlayerCount() != ctx.playerCount) {
            MYE_LOG_INFO("[replay] player lanes: %u (from the .rep; --local-players said %u)",
                         player.PlayerCount(), ctx.playerCount);
            ctx.playerCount = player.PlayerCount();
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
    } else if (!config.replayRecordPath.empty() && !netFailed) {
        // 接続できなかったときは記録も始めない (0 tick の .rep を残すと、後で
        // 「録れているのに中身が無い」という別の謎になる)。
        // --rep-snapshot: 記録開始時点の sim 状態を .rep の先頭へ埋め込む。
        // ここは app.OnStart + ApplyStructuralChanges の直後 = 構造変更が空の撮影点
        std::vector<std::byte> startSnapshot;
        if (config.replayEmbedSnapshot && !CaptureSimSnapshot(simRefs, startSnapshot)) {
            MYE_LOG_ERROR("[replay] could not capture the embedded snapshot");
            return 1;
        }
        recorder.Start(config.replayRecordPath, scene.GetWorld().Rng().State(),
                       scene.GetWorld().Rng().Inc(), scene.GetWorld().AliveCount(),
                       ctx.playerCount, startSnapshot.data(), startSnapshot.size());
    }

    // ---- クラッシュ .rep のリングを起こす (M52f) ----
    // ★ここが「構造変更が空」の最初の撮影点で、かつ埋め込みスナップショットの復元
    //   (verify) より**後**。先に起こすと復元で tick 番号が飛んでリングが取り直しになる。
    // 記録/検証中も動かす — そこで落ちた .rep も同じ価値がある
    if (config.crashHandler) {
        // ★レコード長を決めるので Begin より前に (M52g)。ここから先 playerCount は動かない
        CrashRingConfig crashCfg = crashRing.Config();
        crashCfg.playerCount = ctx.playerCount;
        crashRing.Configure(crashCfg);
        crashRing.SetEnabled(true);
        crashRing.Begin(simRefs, ctx.tickIndex);
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
    bool running = !netFailed; // 接続失敗時は 1 フレームも回さずに通常の後始末へ落ちる
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
    tickServices.prevTickInput = prevTickInput;
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
    tickServices.xpbd = &xpbd; // M60'b
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
    // M52h: セッションが立った時点で C# レーンは最後まで止める。途中で on/off すると
    // 「片方だけ C# が動いた tick」が生まれて必ず割れるので、走行中は変えない
    tickServices.netLockstep = netEnabled;
    // ★ロールバック中は .rep の記録を EngineLoop が引き取る (M52i)。
    //   RunOneTick の中で記録すると**予測で走った tick までファイルに載る**ので、
    //   巻き戻して走り直した tick が二重に並んだ .rep になる。記録してよいのは
    //   「確定入力で走り、もう覆らない」と分かった tick だけ = 確定した瞬間に書く。
    //   ロールバック無しの素のロックステップ (M52h) は全 tick が最初から確定なので
    //   従来どおり RunOneTick の中で記録する (経路を増やさない)
    if (netRollbackActive) {
        tickServices.recorder = nullptr;
    }

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
        InputSnapshot savedInputs[kMaxPlayers] = {};
        for (uint32_t p = 0; p < kMaxPlayers; ++p) {
            savedInputs[p] = ctx.inputs[p];
        }
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
            for (uint32_t p = 0; p < kMaxPlayers; ++p) {
                ctx.inputs[p] = e->inputs[p];
            }
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
        for (uint32_t p = 0; p < kMaxPlayers; ++p) {
            ctx.inputs[p] = savedInputs[p];
        }
        audioSystem.SetSuspended(recorder.IsActive() || player.IsActive());
        // M36b の補間参照を捨てる: 過去へ飛んだ直後のフレームが「シーク前の行列」と
        // 混ざって 1 フレームだけ幽霊が出るのを防ぐ (Get() が null を返す = 補間しない)
        prevWorld.world.clear();
        prevWorld.generation.clear();
        if (rep.outcome != SeekOutcome::Failed) {
            // ★シークは毎回**自己検証する**。戻して同じ入力で回した結果が記録と
            //   ビット一致しなければ、決定論の外 (C# レーン等) が混ざっている証拠
            rep.expectedHash = timeTravel.HashAtTick(target);
            rep.actualHash = HashWorld(scene.GetWorld(),
                                       {&particleSystem.Cpu(), &scene.Time(), &scene.Persist(), &xpbd});
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
    // ---- tick 末のワールドハッシュ (M52f 申し送り 6 の畳み込み) ----
    // クラッシュリング / タイムトラベル / ロールバックの 3 者が**同じ 1 個**を使う。
    // M52f までは消費者ごとに撮っていて、両方 on だと同じ tick で 2 回走っていた
    // (実測 約 0.2ms/回)。ロールバックが毎 tick ハッシュを要求するようになったので
    // ここで 1 本に畳んだ
    const auto TickEndHash = [&]() -> uint64_t {
        return HashWorld(scene.GetWorld(),
                         {&particleSystem.Cpu(), &scene.Time(), &scene.Persist(), &xpbd});
    };

    // ---- 予測ロールバック (M52i、決定台帳 2) ----
    // tick が消費する入力レーンを組む。**未着レーンは直近の確定値の繰り返しで埋める**。
    // 1 本でも埋めたら true (= 後で覆りうる投機 tick)
    const auto BuildNetInputs = [&](uint64_t tick, InputSnapshot* out) -> bool {
        bool predicted = false;
        for (uint32_t p = 0; p < ctx.playerCount; ++p) {
            if (const InputSnapshot* in = net.LaneInput(tick, p)) {
                out[p] = *in;
                continue;
            }
            const InputSnapshot* guess = net.PredictLane(tick, p);
            out[p] = (guess != nullptr) ? *guess : InputSnapshot{};
            predicted = true;
        }
        return predicted;
    };

    // from の**直前**まで巻き戻し、そこから現在 tick まで確定入力で走り直す。
    // ★通常 tick と同じ RunOneTick を通す (決定台帳 2)。タイムトラベルの SeekTo と
    //   まったく同じ形にしてあるのは、抑止する対象 (出力レーン / C# / 記録) が
    //   「過去をなぞっている」という一点で完全に同じだから。
    // ★ここで net.SubmitLocalInput を呼んではいけない — 自レーンの値は tick ごとに
    //   ちょうど 1 回しか確定させない (M52h 申し送り 6)。再シムで撃ち直すと、相手が
    //   先に消費した値と食い違って本物の desync になる
    const auto NetResimFrom = [&](uint64_t from) -> uint64_t {
        const uint64_t resume = ctx.tickIndex;
        if (from >= resume) {
            return 0;
        }
        scene.GetWorld().ApplyStructuralChanges();
        // ★クラッシュ .rep の tick 列も一緒に巻き戻す。これをしないと再シムの
        //   OnTickBegin が「tick が飛んだ」と見て毎回撮り直し、ネット対戦中は
        //   リングが常に数 tick しか持たない = crash.rep も desync バンドルも
        //   再現に使えない .rep になる (実装中に実測: tickCount が 1 だった)
        crashRing.Rewind(from);
        const std::vector<std::byte>* blob = netRb.SnapshotBefore(from);
        if (blob == nullptr || !RestoreSimSnapshot(simRefs, blob->data(), blob->size())) {
            MYE_LOG_ERROR("[net] rollback: no restorable snapshot for tick %llu",
                          static_cast<unsigned long long>(from));
            return 0;
        }
        // ★非 sim レーン (トレイル / GPU パーティクル) は**あえて落とさない**。
        //   巻き戻し幅は最大 8 tick = 133ms で、残像より「毎ロールバックで全消し」の
        //   ちらつきの方が実害が大きい。タイムトラベル (任意の過去へ飛ぶ) とは
        //   ここだけ方針が違う — 見せ方は消費者の責務 (SimSnapshot.h)
        const bool savedSimulate = ctx.simulateScripts;
        InputSnapshot savedInputs[kMaxPlayers] = {};
        for (uint32_t p = 0; p < kMaxPlayers; ++p) {
            savedInputs[p] = ctx.inputs[p];
        }
        IEngineApp* const savedApp = tickServices.app;
        PrevWorldStore* const savedPrev = tickServices.prevWorld;
        tickServices.app = nullptr;       // エディタ更新は回さない
        tickServices.prevWorld = nullptr; // 描画補間の採取も要らない
        tickServices.resim = true;
        audioSystem.SetSuspended(true); // 捨てた未来の音を断つ
        for (uint64_t t = from; t < resume; ++t) {
            const NetSpecTick* e = netRb.Entry(t);
            const bool predicted = BuildNetInputs(t, ctx.inputs);
            // ポーズ tick も忠実になぞる (飛ばすと prevTickInput が食い違う)
            ctx.simulateScripts = (e != nullptr) ? e->simulated : savedSimulate;
            crashRing.OnTickBegin(t, ctx.inputs, ctx.playerCount);
            RunOneTick(tickServices);
            const uint64_t h = TickEndHash();
            crashRing.OnTickEnd(simRefs, t, h);
            netRb.OnTickEnd(simRefs, t, ctx.inputs, ctx.playerCount, h, predicted,
                            ctx.simulateScripts);
        }
        tickServices.app = savedApp;
        tickServices.prevWorld = savedPrev;
        tickServices.resim = false;
        ctx.simulateScripts = savedSimulate;
        for (uint32_t p = 0; p < kMaxPlayers; ++p) {
            ctx.inputs[p] = savedInputs[p];
        }
        audioSystem.SetSuspended(recorder.IsActive() || player.IsActive());
        return resume - from;
    };

    // 確定した tick を .rep へ落とし、相手へ「ここまで確定した」と主張する。
    // ★.rep に載るのはここを通った tick だけ = **予測で走った tick は 1 本も載らない**。
    //   これが「ロールバック有りで録った .rep がロックステップのものとバイト一致する」
    //   ことの根拠で、net_verify.bat はそれを機械検証している
    const auto NetCommitConfirmed = [&]() {
        uint64_t confirmed = netRb.ConfirmedTick();
        uint64_t lastTick = 0;
        uint64_t lastHash = 0;
        bool any = false;
        while (confirmed < ctx.tickIndex && net.HasInputs(confirmed)) {
            const NetSpecTick* e = netRb.Entry(confirmed);
            if (e == nullptr || e->predicted) {
                break; // まだ覆りうる (次の Reconcile で片付く)
            }
            if (recorder.IsActive()) {
                recorder.RecordTick(e->inputs, ctx.playerCount, e->hashAfter);
                if (recorder.TickCount() >= static_cast<uint64_t>(config.replayTicks)) {
                    recorder.Finish();
                    ctx.requestExit = true;
                }
            }
            netRb.NoteCommitted(confirmed, e->hashAfter);
            // ★リングの保護下限を進めるのは**確定した tick まで**。投機 tick で進めると、
            //   後から届いた本物の入力が「消費済み」として捨てられ、予測が永久に
            //   直らないまま静かに走り続ける (実装中に最初に踏む罠)
            net.OnTickConsumed(confirmed);
            // 相手へ主張するのは checkpoint tick だけ (NetSession.h の kNetHashCheckpoint)
            if (confirmed % kNetHashCheckpoint == 0) {
                lastTick = confirmed;
                lastHash = e->hashAfter;
                any = true;
            }
            ++confirmed;
        }
        netRb.SetConfirmedTick(confirmed);
        if (any) {
            net.SetLocalConfirmed(lastTick, lastHash);
        }
    };

    // フレーム頭に 1 回。届いた確定入力と投機記録を突き合わせ、外れていたら巻き戻す
    const auto NetReconcile = [&]() {
        if (!netRollbackActive || !net.Running()) {
            return;
        }
        uint64_t bad = ~0ull;
        for (uint64_t t = netRb.ConfirmedTick(); t < ctx.tickIndex; ++t) {
            if (!net.HasInputs(t)) {
                break; // まだ確定していない = ここから先は判定できない
            }
            const NetSpecTick* e = netRb.Entry(t);
            if (e == nullptr) {
                break; // リングから溢れた (投機上限があるので通常は起きない)
            }
            if (!e->predicted) {
                continue; // 最初から確定入力で走った tick
            }
            if (netRb.InputsMatch(t, net.InputsFor(t), ctx.playerCount)) {
                netRb.MarkConfirmed(t); // 予測が当たった = 巻き戻す理由が無い
                continue;
            }
            bad = t;
            break;
        }
        if (bad != ~0ull) {
            const uint64_t depth = ctx.tickIndex - bad;
            const double t0 = clock.Now();
            if (NetResimFrom(bad) == 0) {
                MYE_LOG_ERROR("[net] rollback failed at tick %llu - cannot continue",
                              static_cast<unsigned long long>(bad));
                exitCode = 1;
                ctx.requestExit = true;
                return;
            }
            netRb.NoteRollback(depth);
            MYE_LOG_TRACE("[net] rolled back %llu tick(s) to %llu (%.2f ms)",
                          static_cast<unsigned long long>(depth),
                          static_cast<unsigned long long>(bad),
                          (clock.Now() - t0) * 1000.0);
        }
        NetCommitConfirmed();
    };

    // ---- desync 検出 (M52i) ----
    // 相手が主張する確定 (tick, hash) と自分の確定ハッシュを突き合わせる。
    // ★突き合わせてよいのは**確定 tick だけ**。予測で走った tick のハッシュを比べると、
    //   正常に働いているロールバックが desync として誤検出される
    const auto NetCheckDesync = [&]() {
        if (!netEnabled || !net.Running() || netDesync) {
            return;
        }
        // 確定済みの checkpoint を**古い順に**突き合わせる。
        // ★「相手が最後に主張した 1 個」だけを見る作りにすると、比較する tick が
        //   到着タイミングで決まって 2 台が別々の tick を報告する (実測: 60 と 68)。
        //   古い順に舐めれば、どちらも「最初に食い違った checkpoint」に落ち着く
        uint64_t peerTick = 0;
        uint64_t peerHash = 0;
        uint64_t mine = 0;
        while (netDesyncScan < netRb.ConfirmedTick()) {
            if (!net.PeerHashFor(netDesyncScan, peerHash)) {
                uint64_t newestTick = 0;
                uint64_t newestHash = 0;
                // 主張そのものが落ちた (ロス) なら、相手はとうに先へ行っている。
                // ここで待ち続けると検出が永久に止まるので、諦めて次の checkpoint へ
                if (net.PeerConfirmed(newestTick, newestHash)
                    && newestTick > netDesyncScan + kNetHashCheckpoint * 4) {
                    netDesyncScan += kNetHashCheckpoint;
                    continue;
                }
                return; // まだ届いていないだけ
            }
            if (netRb.CommittedHash(netDesyncScan, mine) && mine != peerHash) {
                break; // 見つけた
            }
            netDesyncScan += kNetHashCheckpoint;
        }
        if (netDesyncScan >= netRb.ConfirmedTick()) {
            return; // 食い違いなし
        }
        peerTick = netDesyncScan;
        netDesync = true;
        netDesyncTick = peerTick;
        MYE_LOG_ERROR("[net] DESYNC at tick %llu: local %016llX / peer %016llX",
                      static_cast<unsigned long long>(peerTick),
                      static_cast<unsigned long long>(mine),
                      static_cast<unsigned long long>(peerHash));
        NetDesyncReport report;
        report.tick = peerTick;
        report.nowTick = ctx.tickIndex;
        report.localHash = mine;
        report.peerHash = peerHash;
        report.localPlayer = net.LocalPlayerIndex();
        report.role = config.netRole;
        HashDump dump;
        HashWorldDump(scene.GetWorld(),
                      {&particleSystem.Cpu(), &scene.Time(), &scene.Persist(), &xpbd}, ctx.tickIndex, dump);
        std::wstring dir;
        const std::wstring crashRoot =
            config.projectRoot.empty() ? GetExecutableDir() : config.projectRoot;
        if (WriteNetDesyncBundle(crashRoot, report, crashRing, dump, dir)) {
            MYE_LOG_ERROR("[net]   bundle: %s", WideToUtf8(dir).c_str());
            MYE_LOG_ERROR("[net]   next: --rep-diff both local.rep, then --replay-verify "
                          "--hash-dump-tick %llu on each and --hash-diff (see desync.txt)",
                          static_cast<unsigned long long>(peerTick));
        }
        if (config.netHaltOnDesync) {
            // ★既定で止める。desync 後の世界は 2 台で別物であって「遊べているように
            //   見えるだけ」なので、黙って続けるのが一番たちが悪い
            exitCode = 4; // 4 = desync (1 = 通常の失敗 / 2 = 落とし損ね と区別する)
            ctx.requestExit = true;
        }
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
        // ---- 入力レーンの確定 (M52g) ----
        // レーン 0 = キーボード + マウス + パッド 0、レーン n>0 = パッド n (Input.h の規約)。
        // playerCount を超えるレーンには一度も書かない = 恒常ゼロ値
        // ★ネット中はローカルのデバイスを 1 レーンぶんしか読まない。レーン n>0 は
        //   相手の端末が持っている = XInput スロット n を撃つ意味が無い (M52g 申し送り 4 の
        //   「未接続スロットへの XInputGetState は重い」がそのまま効く)
        {
            const uint32_t captureLanes = netEnabled ? 1u : ctx.playerCount;
            for (uint32_t p = 0; p < captureLanes; ++p) {
                ctx.inputs[p] = input.CaptureSnapshot(p);
            }
            if (netEnabled) {
                // ★ライブ入力は tick ループへ入る前に退避する。ループ内で ctx.inputs は
                //   ネットの確定入力で丸ごと上書きされるので、そこから自レーンを読むと
                //   「相手から返ってきた自分の入力」を送り直す循環になる
                netLiveInput = ctx.inputs[0];
                net.Poll();
            }
        }
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

        // ---- クラッシュバンドルへ添える元シーンのパス (M52f) ----
        // ハンドラ内で std::wstring を触らない (再確保済みの領域を掴む事故) ために、
        // 毎フレーム固定バッファへ写しておく。LoadScene で変わりうるので毎フレーム
        {
            const std::wstring& src = scene.SourcePath();
            const size_t n = src.size() < 519 ? src.size() : 519;
            std::memcpy(crashPayload.sceneSource, src.c_str(), n * sizeof(wchar_t));
            crashPayload.sceneSource[n] = L'\0';
        }

        // ---- タイムトラベル (M52e): リングの開始とシーク要求を tick 境界で捌く ----
        // ★どちらもフレーム頭 (= 前フレームの tick が終わった直後) でしか行わない。
        //   ImGui の途中で世界を差し替えると、その後のウィンドウが破棄済み EntityID を掴む
        // 記録/検証中は .rep がタイムラインの役なので、リングは起こさない
        // (起こすと 1 枚撮って entry が 1 つも積まれない空リングが残る)
        // ★ネット中はタイムラインを起こさない (M52i)。相手の居る tick 列を勝手に
        //   巻き戻しても相手はついてこないので、スクラブは原理的に成立しない。
        //   同じ tick 境界を 2 種類の巻き戻し (シークとロールバック) が奪い合う状態も
        //   作らずに済む — この排他は「機能の削り」ではなく意味論の帰結
        if (timeTravel.BeginPending() && !recorder.IsActive() && !verifying && !netEnabled) {
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
        // ---- 遅延ロックステップのゲート (M52h) ----
        // ★全 peer の tick 入力がそろうまで **1 tick も進めない**。ここが「揃った入力で
        //   回る」の全部で、これ以上の同期機構は要らない (予測して先へ進むのは M52i)。
        //   届いていなければ数 ms だけ受信を回して待つ — フレームの見た目を滑らかに
        //   するためだけの待ちで、sim には一切影響しない
        bool netStalled = false;
        const auto NetReady = [&](uint64_t tick) {
            if (!netEnabled || !net.Running()) {
                return true;
            }
            if (net.HasInputs(tick)) {
                return true;
            }
            // ---- 予測ロールバック (M52i) ----
            // ★そろっていなくても**待たずに**進む。ここで数 ms 待ってから予測すると
            //   「遅延が小さいときだけ滑らか」という中途半端な挙動になり、
            //   ロールバックが本当に効いているのかも分からなくなる。
            //   上限まで先行したら、そこから先は M52h と同じく待つ
            if (netRollbackActive && tick - netRb.ConfirmedTick() < kNetMaxSpeculation) {
                return true;
            }
            const double t0 = clock.Now();
            const bool got = net.WaitForInputs(tick, kNetStallWaitMs);
            net.NoteStall((clock.Now() - t0) * 1000.0);
            netStalled = !got;
            return got;
        };
        // ---- 突き合わせ (M52i) ----
        // ★tick ループへ入る**直前**に置く。フレーム頭 (ImGui の直後) だと、前フレームの
        //   エディタ操作が積んだ構造変更を巻き込んだ状態で世界を差し替えることになる。
        //   ここはホットリロードのセーフポイントも通過済みで、確実に tick 境界
        NetReconcile();
        NetCheckDesync();
        while (!scrubbing && ticks < maxTicksThisFrame
               && (verifying ? player.HasTick(ctx.tickIndex)
                             : (accumulator >= kFixedDt && NetReady(ctx.tickIndex)))) {
            // M52i: この tick が未確定レーンを予測で埋めて走ったか (tick 末の投機記録へ)
            bool netTickPredicted = false;
            if (verifying) {
                // フェーズ 1 の入力をレーンごと置換する
                for (uint32_t p = 0; p < ctx.playerCount; ++p) {
                    ctx.inputs[p] = player.InputForTick(ctx.tickIndex, p);
                }
            } else if (netEnabled && net.Running()) {
                // ネットの確定入力で全レーンを置換する。**verify の置換と同じ場所**に
                // 置いてあるので、この tick が消費した列がそのまま .rep に載る
                // (= 2 台の .rep のバイト一致が「同じ tick 列を回した」証明になる)。
                // ★合成入力より先に見る: ネット中の合成入力は既に相手側のレーンにも
                //   載っていて、ここで上書きすると自分の値で相手のレーンを潰す
                if (netRollbackActive) {
                    // M52i: 未着レーンは予測で埋める (そろっていれば確定値がそのまま入る)
                    netTickPredicted = BuildNetInputs(ctx.tickIndex, ctx.inputs);
                } else {
                    const InputSnapshot* lanes = net.InputsFor(ctx.tickIndex);
                    for (uint32_t p = 0; p < ctx.playerCount; ++p) {
                        ctx.inputs[p] = lanes[p];
                    }
                }
            } else if (config.synthInput) {
                // 合成入力 (M52g、--synth-input)。**verify の置換と同じ場所**に置くのが要点 —
                // ここで入れた値がそのまま .rep へ記録され、検証では上の分岐で記録値として
                // 戻ってくる。つまり「合成入力で録った .rep」は普通の .rep と区別なく再生でき、
                // 検証側に --synth-input を渡す必要も無い
                for (uint32_t p = 0; p < ctx.playerCount; ++p) {
                    ctx.inputs[p] = SynthLaneInput(ctx.tickIndex, p);
                }
            }
            // ---- 固定 tick 本体 (M52d、決定台帳 2) ----
            // 通常 tick / タイムトラベル再シム / ロールバック再シムが通る唯一の実装。
            // ここでの仕事は「この tick が消費する入力を確定させて呼ぶ」だけ
            const uint64_t ranTick = ctx.tickIndex;
            // ---- 自レーンの未来入力を確定させる (M52h) ----
            // ★tick t を回す**直前**に t + delay を送る。ここに置くことで
            //   「1 tick につきちょうど 1 回」が構造的に保証される — フレーム頭に置くと
            //   tick が回らないフレームで同じ target を違う値で送り直してしまい、
            //   相手が先に消費した値と食い違って即 desync する
            if (netEnabled && net.Running()) {
                const uint64_t target = ranTick + net.InputDelay();
                net.SubmitLocalInput(target, NetLocalInput(target));
            }
            // ★クラッシュ .rep へは tick に**入る前**に入力を載せる (M52f)。
            //   落ちるのは RunOneTick の中なので、tick 末に載せる作りだと
            //   「まさに落ちた tick」が .rep に残らず、再生してもその tick へ入れない
            crashRing.OnTickBegin(ranTick, ctx.inputs, ctx.playerCount);
            if (crashTestKind != CrashTestKind::None
                && ranTick == static_cast<uint64_t>(config.crashTestTick)) {
                MYE_LOG_ERROR("[crash] --crash-test %s: crashing on purpose at tick %llu",
                              CrashTestKindName(crashTestKind),
                              static_cast<unsigned long long>(ranTick));
                TriggerTestCrash(crashTestKind);
                // ★ここへ戻ってきたら「落とすつもりが落ちなかった」= 検出器自身の故障。
                //   黙って走り続けると bat が無限に待つ (実装中に踏んだ: Debug CRT の
                //   不正パラメータがアサートダイアログで止まっていた)
                MYE_LOG_ERROR("[crash] --crash-test %s did NOT crash - the handler was not "
                              "exercised (this is a bug in the trigger)",
                              CrashTestKindName(crashTestKind));
                exitCode = 2;
                ctx.requestExit = true;
            }
            RunOneTick(tickServices);
            // ---- tick 末ハッシュは 1 回だけ (M52i、M52f 申し送り 6) ----
            // クラッシュリング / ロールバック / タイムトラベルの 3 者が同じ 1 個を使う。
            // 撮る点は「構造変更が空 = .rep が記録するのと同じ点」で M52f から不変
            const bool ttRing = timeTravel.Enabled() && !recorder.IsActive() && !verifying;
            const bool needTickHash = crashRing.Enabled() || netEnabled || ttRing;
            const uint64_t tickHash = needTickHash ? TickEndHash() : 0;
            if (crashRing.Enabled()) {
                // これがあるので、届いた crash.rep は「本当に同じ世界を再現したか」を
                // 受け取り側が tick 単位で機械判定できる
                crashRing.OnTickEnd(simRefs, ranTick, tickHash);
            }
            if (netEnabled && net.Running()) {
                if (netRollbackActive) {
                    // 投機記録 + 「次 tick が走る前」のスナップショット (M52i)。
                    // ★ここでは .rep へ書かない / 相手へハッシュを主張しない —
                    //   この tick はまだ覆りうる。確定は NetCommitConfirmed の仕事
                    netRb.OnTickEnd(simRefs, ranTick, ctx.inputs, ctx.playerCount, tickHash,
                                    netTickPredicted, ctx.simulateScripts);
                    if (!netRb.Active()) {
                        MYE_LOG_ERROR("[net] rollback ring failed at tick %llu - exiting",
                                      static_cast<unsigned long long>(ranTick));
                        exitCode = 1;
                        ctx.requestExit = true;
                    }
                } else {
                    // 素のロックステップ (M52h): 走った tick はその場で確定している。
                    // desync 検出はこちらの経路でも同じように効く
                    netRb.NoteCommitted(ranTick, tickHash);
                    netRb.SetConfirmedTick(ranTick + 1);
                    if (ranTick % kNetHashCheckpoint == 0) {
                        net.SetLocalConfirmed(ranTick, tickHash);
                    }
                    net.OnTickConsumed(ranTick); // 消費済み tick はリングの保護下限を進める
                }
            }
            // ---- タイムトラベルのリングへ記録 (M52e) ----
            // 記録/検証中は .rep がその役なので載せない。simulateScripts は
            // RunOneTick の中で app が決めた**その tick の実効値**を読む
            if (ttRing) {
                timeTravel.OnTickEnd(simRefs, ranTick, ctx.inputs, ctx.playerCount,
                                     ctx.simulateScripts, tickHash);
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
        // ★stall 中は実時間を溜めない。溜めると「相手を待っていた 2 秒」ぶんが
        //   復帰した瞬間に早送りとして流れ、遊べたものではなくなる。待たされたのは
        //   ゲームの都合ではないので、その時間は無かったことにする
        if (netStalled && accumulator > kFixedDt) {
            accumulator = kFixedDt;
        }
        if (netEnabled && net.State() == NetState::Failed) {
            MYE_LOG_ERROR("[net] session failed at tick %llu - exiting",
                          static_cast<unsigned long long>(ctx.tickIndex));
            exitCode = 1;
            ctx.requestExit = true;
        }
        // ---- 上位へ見せる状態を更新する (M52i) ----
        // ★書くのはここ 1 か所だけ。エディタの NetWindow も ABI v13 のスロットも
        //   この POD を読むので、複数箇所で書くと「窓とスクリプトで値が違う」になる
        if (netEnabled) {
            netInfo.connected = net.Running();
            netInfo.localPlayer = net.LocalPlayerIndex();
            netInfo.playerCount = net.PlayerCount();
            netInfo.inputDelay = net.InputDelay();
            netInfo.pingMs = net.PingMs();
            netInfo.confirmedTick = netRb.ConfirmedTick();
            netInfo.speculation = static_cast<uint32_t>(
                ctx.tickIndex > netRb.ConfirmedTick() ? ctx.tickIndex - netRb.ConfirmedTick() : 0);
            netInfo.predictedTicks = netRb.PredictedTicks();
            netInfo.rollbacks = netRb.RollbackCount();
            netInfo.rollbackTicks = netRb.RollbackTicks();
            netInfo.maxRollbackDepth = netRb.MaxRollbackDepth();
            uint64_t h = 0;
            if (netRb.ConfirmedTick() > 0 && netRb.CommittedHash(netRb.ConfirmedTick() - 1, h)) {
                netInfo.localHash = h;
            }
            net.PeerConfirmed(netInfo.peerTick, netInfo.peerHash);
            netInfo.desync = netDesync;
            netInfo.desyncTick = netDesyncTick;
            netInfo.packetsSent = net.PacketsSent();
            netInfo.packetsRecv = net.PacketsRecv();
            netInfo.packetsDropped = net.PacketsDropped();
            netInfo.stalls = net.StallCount();
            netInfo.stallMs = net.StallMs();
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
            // 未完了 tick (クラッシュ .rep の最後の 1 本) がある場合は必ず併記する。
            // 「600 tick 一致」と「599 tick 一致 + 1 tick 未照合」を同じ文で出さない
            MYE_LOG_INFO("[replay] VERIFY PASS: %llu ticks hash-identical%s",
                         static_cast<unsigned long long>(player.verifiedTicks),
                         player.unverifiedTicks > 0
                             ? " (plus in-flight tick(s) with no expected hash - crash bundle)"
                             : "");
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
                // M21: ゲーム内 UI を backbuffer に重ねる (Runtime 経路)。マウスは hover 表示用。
                // ワールド追従 UI は直近 Render のカメラ (補間済み・ジッタ無し) + prevWorld で
                // 3D パスと同じ絵の位置に射影する
                uilayout::UIWorldContext uiWc;
                uiWc.viewProj = renderSystem.lastViewProjNoJitter;
                uiWc.prevWorld = renderSystem.prevWorld;
                uiWc.alpha = renderSystem.interpAlpha;
                uiRenderer.Render(scene.GetWorld(), device, shaderManager, resources, target.rtv,
                                  target.width, target.height, ctx.Input().mouseX,
                                  ctx.Input().mouseY, ctx.Input().MouseDown(0),
                                  renderSystem.lastCamValid ? &uiWc : nullptr);
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
            // ---- 反射プローブのベイク (M56e、--probe-bake) ----
            // ★スクショ保存の**後**に置く。ベイクは RTV / ビューポート / ラスタライザを
            //   総取り替えするので、撮影より前でやるとその日の PNG が説明のつかない形で
            //   変わりうる。ここなら残るのは Present だけ = 撮影経路と完全に独立
            if (probeBaker && !probeBaked
                && ctx.frameIndex == static_cast<uint64_t>(config.probeBakeFrame)) {
                probeBaked = true; // 成否によらず 1 回で打ち切る (自動リトライは決定性の敵)
                // ---- M56f: シーンのプローブを全部焼いて描画へ載せる (--probe-bake-all) ----
                // ★焼いた束の**面の向き**は M56e と同じ継ぎ目比で機械判定する。ここを
                //   省くと「複数プローブを配列へ詰め替える」段が無検査になり、スライスの
                //   取り違え (プローブ A の絵が B の箱で出る) が絵からしか分からなくなる
                if (config.probeBakeAll) {
                    if (probeBaker->BakeAll(scene.GetWorld(), device, forwardPath, shaderManager,
                                            resources, probeArray)) {
                        renderSystem.reflectionProbes = &probeArray.set;
                        for (size_t pi = 0; pi < probeArray.probes.size(); ++pi) {
                            std::vector<float> faces;
                            int faceSize = 0;
                            ProbeSeamStats seam;
                            if (!ProbeReadFaces(device, probeArray.probes[pi], faces, faceSize)
                                || !ProbeSeamCheck(faces, faceSize, seam)) {
                                MYE_LOG_ERROR("[probe] readback failed (probe %zu)", pi);
                                exitCode = 5;
                                continue;
                            }
                            const bool seamOk = seam.seamRatio < kProbeSeamRatioLimit;
                            MYE_LOG_INFO("[probe] probe %zu at (%.2f, %.2f, %.2f): seam %s, "
                                         "ratio %.3f (limit %.2f)",
                                         pi,
                                         static_cast<double>(probeArray.probes[pi].position.x),
                                         static_cast<double>(probeArray.probes[pi].position.y),
                                         static_cast<double>(probeArray.probes[pi].position.z),
                                         seamOk ? "PASS" : "FAIL",
                                         static_cast<double>(seam.seamRatio),
                                         static_cast<double>(kProbeSeamRatioLimit));
                            if (!seamOk) {
                                exitCode = 5;
                            }
                            // PNG は先頭 1 個だけ (目視の口。残りは上の比が受け持つ)
                            if (pi == 0) {
                                ProbeWriteFacesPng(faces, faceSize,
                                                   config.probeBakePng.empty()
                                                       ? std::wstring(L"tests\\actual\\probe_faces.png")
                                                       : config.probeBakePng);
                            }
                        }
                    } else {
                        exitCode = 5;
                    }
                }
                BakedProbe probe;
                const DirectX::XMFLOAT3 pos = { config.probeBakePos[0], config.probeBakePos[1],
                                                config.probeBakePos[2] };
                // ★キャプチャは **Forward パス固定**。Deferred で撮ると共有 GBuffer 5 枚が
                //   128^2 へ縮んで次フレームに戻される (メイン描画の TAA / SSR 履歴を巻き添えに
                //   する) うえ、プローブの 128^2 に SSAO も SSR も意味が無い
                if (!config.probeBake) {
                    // --probe-bake-all だけ指定された = 位置指定の 1 個焼きは走らせない
                } else if (probeBaker->Bake(scene.GetWorld(), device, forwardPath, shaderManager,
                                            resources, pos, 0.1f, 500.0f, probe)) {
                    std::vector<float> faces;
                    int faceSize = 0;
                    ProbeSeamStats seam;
                    if (ProbeReadFaces(device, probe, faces, faceSize)
                        && ProbeSeamCheck(faces, faceSize, seam)) {
                        const std::wstring png = config.probeBakePng.empty()
                            ? std::wstring(L"tests\\actual\\probe_faces.png")
                            : config.probeBakePng;
                        ProbeWriteFacesPng(faces, faceSize, png);
                        // 「継ぎ目の段差 / 面の中の段差」が 1 前後なら面の向きは合っている。
                        // max は輪郭が継ぎ目をまたぐだけで跳ねるので参考値にとどめる
                        const bool seamOk = seam.seamRatio < kProbeSeamRatioLimit;
                        MYE_LOG_INFO("[probe] seam check %s: ratio %.3f (limit %.2f) = "
                                     "seam %.5f / interior %.5f, max seam %.4f, "
                                     "%d samples, mean luma %.4f",
                                     seamOk ? "PASS" : "FAIL",
                                     static_cast<double>(seam.seamRatio),
                                     static_cast<double>(kProbeSeamRatioLimit),
                                     static_cast<double>(seam.meanSeamDiff),
                                     static_cast<double>(seam.meanInteriorDiff),
                                     static_cast<double>(seam.maxSeamDiff), seam.samples,
                                     static_cast<double>(seam.meanLuma));
                        if (!seamOk) {
                            exitCode = 5; // 5 = プローブの面が合っていない (4 = desync と区別)
                        }
                    } else {
                        MYE_LOG_ERROR("[probe] readback failed");
                        exitCode = 5;
                    }
                } else {
                    exitCode = 5;
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
    convexcol::Install(nullptr); // M60f (convexColliders 破棄前に必ず外す)
    physmat::Install(nullptr); // M59a1 (physMatLibrary 破棄前に必ず外す)
    terraincol::Install(nullptr); // M59i (terrainColliders 破棄前に必ず外す)
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
    if (netEnabled) {
        // ★抜ける前に最後の入力を撃ち切る。相手はこちらの最後の数 tick をまだ受け取って
        //   いないかもしれず、黙って終了すると相手だけが stall タイムアウトで落ちる
        net.Finish();
        if (netRollbackActive) {
            // M52i: ロールバックの実測値。予測が当たっていれば rollbacks は 0 に近づき、
            // ロスや相手の遅れが増えるほど伸びる = 回線品質の一次データにもなる
            MYE_LOG_INFO("[net] rollback: %llu rollback(s) / %llu re-simulated tick(s) / "
                         "max depth %llu / %llu predicted tick(s) / snapshots %.1f MB / "
                         "confirmed up to tick %llu",
                         static_cast<unsigned long long>(netRb.RollbackCount()),
                         static_cast<unsigned long long>(netRb.RollbackTicks()),
                         static_cast<unsigned long long>(netRb.MaxRollbackDepth()),
                         static_cast<unsigned long long>(netRb.PredictedTicks()),
                         static_cast<double>(netRb.SnapshotBytes()) / (1024.0 * 1024.0),
                         static_cast<unsigned long long>(netRb.ConfirmedTick()));
        }
        if (netDesync) {
            MYE_LOG_ERROR("[net] session ended on a DESYNC at tick %llu",
                          static_cast<unsigned long long>(netDesyncTick));
        }
    }
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
    if (config.ssr) {
        // M56d: SSR の実測 (ヘッドレス撮影で数字を残す唯一の口。理由は下の [hzb] と同じ)。
        // HZB も一緒に出す — SSR の総コストは「ピラミッド構築 + トレース」の和で、
        // WARP でどちらが支配項かはこの 2 つを並べないと分からない
        MYE_LOG_INFO("[ssr] --ssr: trace %.3f ms + hzb %.3f ms (GPU, last frame)",
                     renderSystem.SsrGpuMs(), renderSystem.HzbGpuMs());
    }
    if (probeBaker) {
        // ★「--frames が baked フレームに届かず 1 度も焼かなかった」を黙って緑にしない。
        //   ベイクは 1 フレームの中で完結するので、走らなかった = 指定が噛み合っていない
        if (!probeBaked) {
            MYE_LOG_ERROR("[probe] --probe-bake never fired (frame %d was not reached; "
                          "%llu frames ran)",
                          config.probeBakeFrame, static_cast<unsigned long long>(ctx.frameIndex));
            exitCode = 5;
        } else {
            MYE_LOG_INFO("[probe] bake %.1f ms (CPU, one shot)", probeBaker->LastBakeCpuMs());
        }
        // M56f: 束を捨てる前に指している側を必ず外す (ぶら下がりポインタ)。
        // 描画はもう止まっているので順序が効くわけではないが、「所有者が死ぬ前に
        // 参照を切る」を 1 箇所で守っておかないと、後でここに描画が挟まったときに死ぬ
        renderSystem.reflectionProbes = nullptr;
        probeArray.Clear();
        probeBaker->Shutdown();
    }
    if (config.hzbDebug != 0) {
        // M56c: HZB の実測 (SSR (M56d) が「加速構造の元が取れるか」を判断する一次データ)。
        // ProfilerWindow は GUI がある経路にしか無いので、ヘッドレス撮影で数字を残す口が
        // ここしかない。値は最後に描いたフレームの全段ディスパッチ合計
        // 表示段は DeferredPath 側で段数に頭打ちされるので、ここでは指定値をそのまま出す
        // (「10 段しか無いのに 11 を指定した」が数字から読めるようにするため)
        MYE_LOG_INFO("[hzb] --hzb-debug %d: build %.3f ms (GPU, last frame)", config.hzbDebug,
                     renderSystem.HzbGpuMs());
    }
    if (stressCount > 0) {
        // 1 枚のバイト数と往復の実測 (M52e のリング枚数 / M52i のロールバック予算の一次データ)
        MYE_LOG_INFO("[snapshot] %llu round-trips: %zu bytes/snapshot, "
                     "capture %.3f ms avg / restore %.3f ms avg",
                     static_cast<unsigned long long>(stressCount), stressBytes,
                     stressCaptureMs / static_cast<double>(stressCount),
                     stressRestoreMs / static_cast<double>(stressCount));
    }
    if (crashRing.Enabled()) {
        // 1 本の .rep がどこまで小さく収まっているかの一次データ (バンドルの上限見積り)
        MYE_LOG_INFO("[crash] rep ring: %llu snapshots taken, last image %zu bytes "
                     "(snapshot %zu + %llu ticks)",
                     static_cast<unsigned long long>(crashRing.SnapshotCount()),
                     crashRing.ImageBytes(), crashRing.SnapshotBytes(),
                     static_cast<unsigned long long>(crashRing.RecordCount()));
    }
    MYE_LOG_INFO("Engine loop finished (%llu frames, %llu ticks)",
                 static_cast<unsigned long long>(ctx.frameIndex),
                 static_cast<unsigned long long>(ctx.tickIndex));
    // ハンドラを外すのは crashHandlerScope (RAII) の仕事 — ここでは何もしない
    return exitCode;
}

} // namespace mye
