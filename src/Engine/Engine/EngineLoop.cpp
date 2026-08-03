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
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Engine/SceneSerializer.h"
#include "Engine/Engine/Script/ManagedHost.h"
#include "Engine/Engine/EffectSystem.h"
#include "Engine/Engine/Script/ScriptHost.h"
#include "Engine/Engine/SkinningSystem.h"
#include "Engine/Engine/TransformSystem.h"
#include "Engine/Engine/UI/UIRenderer.h"
#include "Engine/Engine/Vfx/VfxRenderer.h"
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

// スクリプトが積んだオーディオイベント 1 件を実際の発音へ流す (M45g)。
// **必ずワールドハッシュの後に呼ぶこと** — ここから先は決定論レーンの外で、
// 記録/検証中は AudioSystem が suspend されているので個々の API が no-op になる。
// rng は AudioSystem 側の専用ストリーム。**world.Rng() は絶対に使わない** (hash 対象)。
void ApplyScriptAudioEvent(const ScriptAudioEvent& e, World& world, AudioSystem& audio,
                           const SoundLibrary& sounds, AudioSourceSystem& sources, Pcg32& rng)
{
    switch (e.op) {
    case ScriptAudioOp::PlayOneShot:
    case ScriptAudioOp::PlayAtPoint: {
        const bool at = e.op == ScriptAudioOp::PlayAtPoint;
        const ResolvedSound rs = ResolveSoundKey(audio, sounds, e.key);
        if (!rs.Valid()) {
            // ★黙って無音にしない — 綴り間違いは「壊れている」と見分けが付かないため
            MYE_LOG_WARN("[audio] unknown sound key (hash %016llx) from script",
                         static_cast<unsigned long long>(e.key));
            break;
        }
        PlayDesc d;
        AudioSpatial spatial;
        bool spatialize = false;
        if (rs.asset != nullptr) {
            if (rs.asset->stream) {
                // stream = BGM。ワンショットのレーンには載らないので BGM として鳴らす
                // (エディタ試聴 PreviewSound と同じ扱い。返したハンドルは無効のまま)
                SoundAsset a = *rs.asset;
                a.volume = std::clamp(a.volume * e.a, 0.0f, 1.0f);
                PlayMusicSound(audio, a, kMusicDefaultFadeSeconds);
                break;
            }
            const int index = PickVariationIndex(*rs.asset, rng.NextU32());
            if (index < 0) {
                break; // クリップが 1 つも割り当たっていないアセット
            }
            d = MakePlayDesc(*rs.asset, index, rng.Range(-1.0f, 1.0f), rng.Range(-1.0f, 1.0f),
                             audio);
            // スクリプト引数はアセット既定への**乗算**(コンポーネント上書きと同じ規約)
            d.volume = std::clamp(d.volume * e.a, 0.0f, 1.0f);
            d.pitch = std::clamp(d.pitch * e.b, 1.0f / AudioSystem::kMaxFreqRatio,
                                 AudioSystem::kMaxFreqRatio);
            spatial.spatialBlend = rs.asset->spatialBlend;
            spatial.minDistance = rs.asset->minDistance;
            spatial.maxDistance = rs.asset->maxDistance;
            spatial.rolloff = static_cast<int>(rs.asset->rolloff);
            spatial.dopplerScale = rs.asset->dopplerScale;
            spatial.reverbSend = rs.asset->reverbSend;
        } else {
            // 生クリップ (M19 からの PlaySound("beep") 経路)。アセットが無いので既定値
            d.clip = rs.clip;
            d.bus = audio.DefaultBus();
            d.volume = std::clamp(e.a, 0.0f, 1.0f);
            d.pitch = e.b;
            d.loop = e.i0 != 0;
        }
        if (at) {
            // ★PlaySoundAt は「その座標で鳴らせ」という明示指定なので、2D 設定の音でも
            //   3D に載せる。ここで落とすと座標を渡したのに定位しない = 一番分かりにくい
            spatial.position = AudioVec3{ e.pos.x, e.pos.y, e.pos.z };
            spatial.velocity = {}; // 置き音なので静止 (ドップラーは掛からない)
            if (spatial.spatialBlend <= 0.0f) {
                spatial.spatialBlend = 1.0f;
            }
            spatial.pitch = d.pitch;
            spatialize = true;
        }
        d.tag = e.handle; // 後の tick から StopVoice / SetVoiceVolume で引けるようにする
        d.spatial = spatialize ? &spatial : nullptr;
        audio.Play(d);
        break;
    }
    case ScriptAudioOp::StopVoice:
        audio.Stop(audio.FindByTag(e.handle), e.a);
        break;
    case ScriptAudioOp::SetVoiceVolume:
        audio.SetVoiceVolume(audio.FindByTag(e.handle), e.a);
        break;
    case ScriptAudioOp::SetVoicePitch:
        audio.SetVoicePitch(audio.FindByTag(e.handle), e.b);
        break;
    case ScriptAudioOp::PlaySource:
        sources.PlayEntity(world, audio, sounds, { e.entity.index, e.entity.generation });
        break;
    case ScriptAudioOp::StopSource:
        sources.StopEntity(world, audio, sounds, { e.entity.index, e.entity.generation }, e.a);
        break;
    case ScriptAudioOp::SetBusVolume: {
        const int bus = audio.FindBusHashed(e.key);
        if (bus >= 0) {
            audio.SetBusVolume(bus, e.a); // 未知のバス名は黙って無視 (既存の音量を壊さない)
        }
        break;
    }
    case ScriptAudioOp::PlayMusic: {
        const ResolvedSound rs = ResolveSoundKey(audio, sounds, e.key);
        if (rs.asset != nullptr) {
            SoundAsset a = *rs.asset;
            a.loop = e.i0 != 0;
            PlayMusicSound(audio, a, e.a);
        } else if (!rs.clip.IsNull()) {
            // .sound.json を作らずに素の .wav/.ogg を BGM 指定した場合。
            // GUID から実ファイルを引いて既定パラメータでストリーミングする
            MusicDesc d;
            d.path = assetguid::ResolvePath(rs.clip.value);
            d.key = rs.clip.value;
            const int bus = audio.FindBus("BGM");
            d.bus = bus >= 0 ? bus : audio.DefaultBus();
            d.fadeSeconds = e.a;
            d.loop = e.i0 != 0;
            if (!d.path.empty()) {
                audio.PlayMusic(d);
            }
        }
        break;
    }
    case ScriptAudioOp::StopMusic:
        audio.StopMusic(e.a);
        break;
    case ScriptAudioOp::SetListener:
        sources.SetListenerOverride({ e.entity.index, e.entity.generation });
        break;
    }
}

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
    MeshColliderLibrary meshColliders; // 静的メッシュコライダーの BVH キャッシュ (M41)
    std::vector<SolidContact> solidContacts; // 物理→衝突イベントの tick 内受け渡し (M28c)
    PrefabLibrary prefabLibrary;
    AnimationLibrary animLibrary;
    AnimationSystem animationSystem;
    ControllerLibrary controllerLibrary;        // Animator Controller (.controller.json、M22)
    AnimatorControllerSystem controllerSystem;  // ステートマシン評価 + ブレンド
    AssetDatabase assetDatabase;                // GUID/.meta サイドカー DB (M23)
    SkinningSystem skinningSystem; // スケルタルアニメの時刻進行 (M18)
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
    uiRenderer.Init(device, shaderManager, assetsRoot); // M21: 失敗してもエンジンは継続 (UI が出ないだけ)
    vfxRenderer.Init(device, shaderManager, &uiRenderer); // M29c: 同上 (VFX が出ないだけ)
    reloadHub.Init(&shaderManager, &resources, &scene, &prefabLibrary, &animLibrary, &soundLibrary,
                   &mixerLibrary, &audioSystem, assetsRoot);
    particleSystem.Init(device, shaderManager, assetsRoot);

    // ポストプロセス設定を config から反映 (M16)。全ビューポート共通の renderSystem に載る
    renderSystem.enablePostFx = config.postFx;
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
                                 &audioHandleSeq);
    managedHost.SetSharedServices(&audioQueue, &pendingScene, &effectQueue, &debugLines,
                                  &audioHandleSeq);

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
    // M36b 描画補間: 前 tick 末のワールド行列 (tick 頭に採取)。record/verify 中は不使用
    PrevWorldStore prevWorld;
    bool lastTickSimulated = false;
    const auto capturePrevWorld = [&prevWorld](World& w) {
        const ComponentTypeId req[] = { WorldMatrixComponent::sTypeId };
        w.ForEachArchetype(req, [&](Archetype& arch) {
            const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                const EntityID e = arch.EntityAt(row);
                if (e.index >= prevWorld.world.size()) {
                    prevWorld.world.resize(e.index + 1);
                    prevWorld.generation.resize(e.index + 1, 0);
                }
                prevWorld.world[e.index] =
                    static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row))->value;
                prevWorld.generation[e.index] = e.generation + 1;
            }
        });
    };
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
        // ---- オーディオのゲート (M45): 記録/検証中はサスペンドする。
        // drain だけでなく **オーディオ更新フレーム全体** を止めるのが要点 — 検証中は
        // 1 フレームで最大 64 tick 回るので、ゲートが drain だけだと 3D 計算 (M45e) や
        // playOnAwake がその頻度で走ってしまう。サスペンドの立ち上がりで一度だけ全停止し、
        // サスペンド中も voice 回収 (Update) は動き続ける (止めるとスロットが枯れる) ----
        audioSystem.SetSuspended(recorder.IsActive() || verifying);
        // 検証モードは実時間と切り離して最速で回す (spec 11.3 の CLI 実行)
        const int maxTicksThisFrame = verifying ? 64 : kMaxTicksPerFrame;
        while (ticks < maxTicksThisFrame
               && (verifying ? player.HasTick(ctx.tickIndex) : accumulator >= kFixedDt)) {
            if (verifying) {
                ctx.input = player.InputForTick(ctx.tickIndex); // フェーズ 1 の入力を置換
            }
            // M36b: tick 頭のワールド行列を補間用に採取 (record/verify 中は補間しないので省く)
            if (!recorder.IsActive() && !player.IsActive()) {
                capturePrevWorld(scene.GetWorld());
            }
            // v7 DebugDrawLine (M37): 前 tick の線を捨てて今 tick 分を積み直す。
            // 0 tick フレームでは最後の tick の線が描かれ続ける (意図どおり)
            debugLines.clear();
            app.OnTick(ctx); // エディタ更新 + simulateScripts の決定
            lastTickSimulated = ctx.simulateScripts; // M36b: 編集中 (非 Play) は補間を切る
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
                // 合成エフェクト (M32e): 子エミッタの停止/再開・duration+linger 後の自動破棄。
                // EffectComponent 非存在シーンでは完全 no-op = 既存シーンのリプレイ不変
                effectSystem.Update(scene.GetWorld());
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
                // トレイル点列の蓄積 (M29c)。WorldMatrix 確定後の tick 側で 1 回だけ —
                // Render 側だと SceneView/GameView の多重描画で多重サンプルされる
                vfxRenderer.UpdateTrails(scene.GetWorld(), ctx.tickIndex);
            }
            // ---- フェーズ 5: スクリプト層 LateUpdate ----
            if (runScripts) {
                scriptHost.RunLateUpdate();
            }
            if (runManaged) {
                managedHost.RunLateUpdate();
            }
            // ---- エフェクト spawn を drain (M32f): Prefab::Instantiate は内部で構造変更を確定する
            // ため tick 末のここで。verify でもゲートしない = sim 状態として同 tick のハッシュに含まれる。
            // C++ スクリプトは verify 中も走り同一キューを積む → 同一 fileId/EntityID 列で再現される。
            if (!effectQueue.empty()) {
                for (const EffectSpawnRequest& req : effectQueue) {
                    std::wstring full = Utf8ToWide(req.prefabKey);
                    if (full.size() < 12
                        || full.compare(full.size() - 12, 12, L".prefab.json") != 0) {
                        full += L".prefab.json";
                    }
                    if (full.find(L':') == std::wstring::npos) {
                        full = assetsRoot + L"\\" + full; // assets ルート相対を絶対化
                    }
                    const uint64_t hash = PrefabLibrary::HashForPath(full);
                    if (!prefabLibrary.Contains(hash)) {
                        prefabLibrary.LoadFromFile(full);
                    }
                    const bool hasParent = (req.parent.index != 0u || req.parent.generation != 0u);
                    const uint64_t parentFid =
                        hasParent ? scene.EnsureFileId(
                                        EntityID{ req.parent.index, req.parent.generation })
                                  : 0;
                    const uint64_t rootFid = Prefab::Instantiate(
                        scene, prefabLibrary, hash, parentFid, req.reservedRootFid);
                    if (rootFid != 0) {
                        const EntityID root = scene.FindByFileId(rootFid).Id();
                        if (auto* t = scene.GetWorld().GetComponent<LocalTransform>(root)) {
                            t->position = { req.pos.x, req.pos.y, req.pos.z };
                        }
                    } else {
                        MYE_LOG_WARN("PlayEffect: prefab not found: %s",
                                     WideToUtf8(full).c_str());
                    }
                }
                effectQueue.clear();
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

            // ---- オーディオ drain (M19/M45): ハッシュ後に再生する (voice 状態は絶対に
            // hashed state へ戻さない)。記録/検証中は AudioSystem 自体が suspend されており
            // Play が no-op になる。**キューの clear だけはゲートの外**で毎 tick 行う ----
            for (const ScriptAudioEvent& e : audioQueue) {
                ApplyScriptAudioEvent(e, scene.GetWorld(), audioSystem, soundLibrary, audioSources,
                                      audioScriptRng);
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
                    vfxRenderer.Reset(); // M29c: トレイル点列も新シーンでリセット
                    scriptHost.ClearStarted();
                    managedHost.OnSceneReloaded();
                    // M45: 旧シーンの SE を断ち、ハンドル採番も 0 から振り直す。
                    // 採番列は「スクリプトの呼出順」だけで決まる必要があるので、
                    // 記録/検証の別なく **必ず** リセットする (サスペンド中でも進む値のため)。
                    // ★M45f: StopAll はボイスプール (SE) だけを止め、**BGM は止めない** —
                    //   シーンをまたいで曲が途切れないのが BGM の要件。新シーンが別の曲を
                    //   指定すれば PlayMusic 側でクロスフェードし、同じ曲なら鳴り続ける
                    audioSystem.StopAll();
                    audioSources.Reset(); // 旧シーンの音源キャッシュ (速度推定 / 再生済みフラグ)
                    audioHandleSeq = 0;
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
    MYE_LOG_INFO("Engine loop finished (%llu frames, %llu ticks)",
                 static_cast<unsigned long long>(ctx.frameIndex),
                 static_cast<unsigned long long>(ctx.tickIndex));
    return exitCode;
}

} // namespace mye
