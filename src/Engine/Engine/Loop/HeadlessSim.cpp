//====================================================================================
//                          HeadlessSim.cpp
//  MyEngin/ 秋田蓮音                                                     10/02/2026
//                                          GPU・窓・オーディオ無しで RunOneTick を回す sim ホスト
//====================================================================================
#include "Engine/Engine/Loop/HeadlessSim.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <vector>

#include <Windows.h>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Jobs/JobSystem.h"
#include "Engine/Core/Util/Random.h"
#include "Engine/Engine/Acoustic/AcousticField.h"
#include "Engine/Engine/Acoustic/AgentSystem.h"
#include "Engine/Engine/Animation/Animation.h"
#include "Engine/Engine/Animation/AnimatorController.h"
#include "Engine/Engine/Animation/PartFollowSystem.h"
#include "Engine/Engine/Animation/SkinningSystem.h"
#include "Engine/Engine/Asset/AssetDatabase.h"
#include "Engine/Engine/Audio/Playback/AudioMixer.h"
#include "Engine/Engine/Audio/Playback/AudioSourceSystem.h"
#include "Engine/Engine/Audio/Playback/AudioSystem.h"
#include "Engine/Engine/Audio/Playback/SoundAsset.h"
#include "Engine/Engine/HotReload/DllReloader.h"
#include "Engine/Engine/Loop/SimInit.h"
#include "Engine/Engine/Loop/TickInputs.h"
#include "Engine/Engine/Loop/TickRunner.h"
#include "Engine/Engine/Particles/ParticleSystem.h"
#include "Engine/Engine/Physics/Collider/ConvexColliderLibrary.h"
#include "Engine/Engine/Physics/Collider/MeshColliderLibrary.h"
#include "Engine/Engine/Physics/Collider/TerrainColliderLibrary.h"
#include "Engine/Engine/Physics/Fracture/FractureLibrary.h"
#include "Engine/Engine/Physics/Fracture/FractureSystem.h"
#include "Engine/Engine/Physics/Rigid/CollisionSystem.h"
#include "Engine/Engine/Physics/Rigid/PhysMatLibrary.h"
#include "Engine/Engine/Physics/Rigid/PhysicsSystem.h"
#include "Engine/Engine/Physics/Xpbd/XpbdBackend.h"
#include "Engine/Engine/Replay/Replay.h"
#include "Engine/Engine/Replay/SimSnapshot.h"
#include "Engine/Engine/Net/NetRuntime.h"
#include "Engine/Engine/Net/NetSession.h" // kNetProtoVersion (出自の protocolVersion)
#include "Engine/Engine/Scene/Prefab.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Engine/Scene/SceneSerializer.h"
#include "Engine/Engine/Scene/TransformSystem.h"
#include "Engine/Engine/Script/EngineApiTable.h"
#include "Engine/Engine/Script/ManagedHost.h"
#include "Engine/Engine/Script/ScriptHost.h"
#include "Engine/Engine/Vfx/EffectSystem.h"
#include "Engine/Engine/Vfx/VfxRenderer.h"
#include "Engine/Platform/InputActions.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/Compute/ComputeAbiRunner.h"
#include "Engine/Renderer/Device/GpuResources.h"
#include "Engine/Renderer/Shader/ShaderManager.h"

namespace mye {
namespace {

constexpr double kFixedDt = 1.0 / 60.0;

// ジョブシステムはプロセスに 1 つ。最初のインスタンスが起こし、最後のインスタンスが止める
std::atomic<int> g_jobUsers{ 0 };
// シャドウコピーの置き場をインスタンスごとに分ける連番。DllReloader は棚の掃除で「自分の PID の棚」を
// 消すので、同じ置き場を 2 インスタンスで共有すると片方がもう片方のロード済み DLL の棚を壊す
std::atomic<int> g_instanceSerial{ 0 };

} // namespace

// 状態の宣言順は EngineLoop::Run のローカルに揃えてある (破棄は逆順)。
// 描画系 (SwapChain / RenderSystem / ForwardPath / UIRenderer 等) は実体を持たない —
// TickServices が参照を要求するもの (resources / vfxRenderer / particleSystem / audio 系) だけ
// 「作って使わない」形で持つ
struct HeadlessSim::Impl : IEngineApp {
    EngineConfig config;
    StartSceneOptions sceneOptions;
    bool initialized = false;
    bool jobsStarted = false;
    std::wstring assetsRoot;
    std::wstring saveDir;
    std::wstring shadowCopyDir;
    std::wstring scenePath;

    Scene scene;
    ShaderManager shaderManager; // Init しない = Load は ID 予約だけ (ヘッドレス規約)
    RenderResources resources;   // InitHeadless: CPU 側の positions / indices / skinnedModels だけ
    TransformSystem transformSystem;
    ScriptHost scriptHost;
    DllReloader dllReloader;
    ManagedHost managedHost; // Init しない = C# レーンは走らない
    ComputeAbiRunner computeAbi;
    ParticleSystem particleSystem;
    CollisionSystem collisionSystem;
    PhysicsSystem physicsSystem;
    FractureSystem fractureSystem;
    std::vector<ShapeImpulse> fractureShapeImpulses;
    MeshColliderLibrary meshColliders;
    ConvexColliderLibrary convexColliders;
    FractureLibrary fractureAssets;
    PhysMatLibrary physMatLibrary;
    TerrainColliderLibrary terrainColliders;
    XpbdBackend xpbd;
    AcousticField acoustic;
    AgentSystem agentSystem;
    std::vector<SolidContact> solidContacts;
    PrefabLibrary prefabLibrary;
    AnimationLibrary animLibrary;
    AnimationSystem animationSystem;
    ControllerLibrary controllerLibrary;
    AnimatorControllerSystem controllerSystem;
    AssetDatabase assetDatabase;
    SkinningSystem skinningSystem;
    PartFollowSystem partFollowSystem;
    EffectSystem effectSystem;
    VfxRenderer vfxRenderer;
    AudioSystem audioSystem; // Init しない = 再生は no-op (--no-audio と同じ)
    AudioSourceSystem audioSources;
    SoundLibrary soundLibrary;
    MixerLibrary mixerLibrary;
    std::vector<ScriptAudioEvent> audioQueue;
    uint64_t audioHandleSeq = 0;
    Pcg32 audioScriptRng;
    std::wstring pendingScene;
    int pendingSaveSlot = -1;
    int pendingLoadSlot = -1;
    int pendingLoadPersistSlot = -1;
    PadVibrationState padVibration;
    CursorLockState cursorLock;
    WindowModeState windowMode;
    std::vector<EffectSpawnRequest> effectQueue;
    std::vector<DebugLineCmd> debugLines;
    InputActions inputActions;
    NetRuntimeInfo netInfo;
    SimProvenance provenance = {};

    EngineContext ctx;
    InputSnapshot prevTickInput[kMaxPlayers] = {};
    SimRefs simRefs;
    ReplayPlayer player;
    TickServices tickServices;
    bool lastTickSimulated = false;
    int exitCode = 0;

    // IEngineApp: 起動シーンは Runtime と共有の PrepareStartScene。ヘッドレスは常時シミュレート
    void OnStart(EngineContext& c) override
    {
        scenePath = PrepareStartScene(c, sceneOptions);
    }
    void OnTick(EngineContext& c) override { c.simulateScripts = true; }

    void BuildTickServices();
};

// EngineLoop::Run の tickServices 組み立てと対応する (契約は TickRunner.h の TickServices)。
// メンバを足す・消すときは両方を見ること
void HeadlessSim::Impl::BuildTickServices()
{
    TickServices& ts = tickServices;
    ts.ctx = &ctx;
    ts.config = &config;
    ts.scene = &scene;
    ts.app = this;
    ts.inputActions = &inputActions;
    ts.prevTickInput = prevTickInput;
    ts.scriptHost = &scriptHost;
    ts.managedHost = &managedHost;
    ts.computeAbi = &computeAbi;
    ts.animationSystem = &animationSystem;
    ts.animLibrary = &animLibrary;
    ts.controllerSystem = &controllerSystem;
    ts.controllerLibrary = &controllerLibrary;
    ts.skinningSystem = &skinningSystem;
    ts.partFollowSystem = &partFollowSystem;
    ts.effectSystem = &effectSystem;
    ts.physicsSystem = &physicsSystem;
    ts.fractureSystem = &fractureSystem;
    ts.shapeImpulses = &fractureShapeImpulses;
    ts.xpbd = &xpbd;
    ts.acoustic = &acoustic;
    ts.agentSystem = &agentSystem;
    ts.transformSystem = &transformSystem;
    ts.collisionSystem = &collisionSystem;
    ts.particleSystem = &particleSystem;
    ts.vfxRenderer = &vfxRenderer;
    ts.resources = &resources;
    ts.solidContacts = &solidContacts;
    ts.effectQueue = &effectQueue;
    ts.debugLines = &debugLines;
    ts.audioQueue = &audioQueue;
    ts.audioSystem = &audioSystem;
    ts.audioSources = &audioSources;
    ts.soundLibrary = &soundLibrary;
    ts.audioScriptRng = &audioScriptRng;
    ts.audioHandleSeq = &audioHandleSeq;
    ts.pendingScene = &pendingScene;
    ts.pendingSaveSlot = &pendingSaveSlot;
    ts.pendingLoadSlot = &pendingLoadSlot;
    ts.pendingLoadPersistSlot = &pendingLoadPersistSlot;
    ts.prefabLibrary = &prefabLibrary;
    ts.assetsRoot = &assetsRoot;
    ts.saveDir = &saveDir;
    ts.recorder = nullptr; // 記録はしない (照合専用)
    ts.player = &player;
    ts.prevWorld = nullptr; // 描画補間の採取は要らない
    ts.lastTickSimulated = &lastTickSimulated;
    ts.exitCode = &exitCode;
}

HeadlessSim::HeadlessSim() : impl_(std::make_unique<Impl>()) {}

HeadlessSim::~HeadlessSim()
{
    Impl& m = *impl_;
    if (!m.initialized) {
        return;
    }
    // 所有者が死ぬ前に注入を外す (EngineLoop::Run の終了手順と同じ順)
    Activate();
    UninstallSimLibraries();
    AssetDatabase::UninstallKeyResolver();
    SceneSerializer::SetManagedHost(nullptr);
    m.scriptHost.Shutdown(); // GameLogic.dll を手放してからシャドウコピーを消す
    m.managedHost.Shutdown();
    m.computeAbi.Shutdown();
    // 自分の棚だけ消す。置き場 (i<N>) と親 (hot_server) は、他プロセスの棚が残っていれば
    // 空でないので remove が失敗して残る (並列実行の相手を巻き込まない)
    std::error_code ec;
    std::filesystem::remove_all(
        m.shadowCopyDir + L"\\p" + std::to_wstring(GetCurrentProcessId()), ec);
    std::filesystem::remove(m.shadowCopyDir, ec);
    std::filesystem::remove(std::filesystem::path(m.shadowCopyDir).parent_path(), ec);
    if (m.jobsStarted && g_jobUsers.fetch_sub(1) == 1) {
        jobs::System().Shutdown();
    }
}

void HeadlessSim::Activate()
{
    Impl& m = *impl_;
    InstallSimLibraries({ &m.resources, &m.meshColliders, &m.convexColliders, &m.fractureAssets,
                          &m.physMatLibrary, &m.terrainColliders });
    m.assetDatabase.InstallAsKeyResolver();
    SceneSerializer::SetManagedHost(&m.managedHost);
}

bool HeadlessSim::Init(const HeadlessSimSetup& setup)
{
    Impl& m = *impl_;
    m.config = setup.config;
    // クックキャッシュは使わない: device 無しでクックすると texture=0 の材質を .mmdl へ書き、
    // Editor / Runtime と共有の <exeDir>\cache\cooked を汚す。毎回フルパースでも登録内容は
    // クック有無でビット同一 (M51b)
    m.config.useCookCache = false;
    m.sceneOptions = setup.scene;
    m.sceneOptions.runShowcasePrepare = true; // Editor の OnStart と同じ手順 (flow-demo の 2 シーン)

    if (!ResolveAssetsRoot(m.config, m.assetsRoot)) {
        return false;
    }
    // 状態ファイルの置き場は Editor / Runtime と分ける (並列実行でも、片方の実行が相手の
    // シャドウコピー・セーブを触らない)。分岐は GameLogic.dll と同じく projectRoot の有無
    const std::wstring base = m.config.projectRoot.empty() ? GetExecutableDir() : m.config.projectRoot;
    m.saveDir = base + L"\\save_server";
    m.shadowCopyDir =
        base + L"\\cache\\hot_server\\i" + std::to_wstring(g_instanceSerial.fetch_add(1));

    // ---- GPU 系を除いた sim 側の起動手順 (EngineLoop::Run と同じ順、SimInit.h) ----
    m.resources.InitHeadless();
    InstallSimLibraries({ &m.resources, &m.meshColliders, &m.convexColliders, &m.fractureAssets,
                          &m.physMatLibrary, &m.terrainColliders });
    // ParticleSystem::Init の代わり: CPU / GPU バックエンドの選択だけ project_settings.json から決める
    m.particleSystem.LoadSettings(m.assetsRoot + L"\\project_settings.json");
    InitSimProjectState(m.assetsRoot);
    const std::wstring dllPath = m.config.projectRoot.empty()
        ? GetExecutableDir() + L"\\GameLogic.dll"
        : m.config.projectRoot + L"\\cache\\GameLogic.dll";
    LoadGameLogic(m.scene, m.scriptHost, m.dllReloader, m.assetsRoot, dllPath, m.shadowCopyDir);
    SceneSerializer::SetManagedHost(&m.managedHost);
    m.audioScriptRng.Seed(kAudioScriptRngSeed);
    {
        SimSharedServices shared;
        shared.audioQueue = &m.audioQueue;
        shared.pendingScene = &m.pendingScene;
        shared.effectQueue = &m.effectQueue;
        shared.debugLines = &m.debugLines;
        shared.audioHandleSeq = &m.audioHandleSeq;
        shared.inputActions = &m.inputActions;
        shared.pendingSaveSlot = &m.pendingSaveSlot;
        shared.pendingLoadSlot = &m.pendingLoadSlot;
        shared.padVibration = &m.padVibration;
        shared.netInfo = &m.netInfo;
        shared.cursorLock = &m.cursorLock;
        shared.pendingLoadPersistSlot = &m.pendingLoadPersistSlot;
        shared.windowMode = &m.windowMode;
        WireScriptServices(m.scriptHost, m.managedHost, shared, m.config.developmentRun);
    }
    // GPU デバイスは無い: コンピュート ABI のスロットは 0 / no-op を返す
    m.scriptHost.SetComputeAbi(&m.computeAbi, nullptr, &m.shaderManager, &m.resources.textures);
    m.managedHost.SetComputeAbi(&m.computeAbi, nullptr, &m.shaderManager, &m.resources.textures);

    EngineContext& ctx = m.ctx;
    ctx.scene = &m.scene;
    ctx.shaders = &m.shaderManager;
    ctx.resources = &m.resources;
    ctx.vfx = &m.vfxRenderer;
    ctx.scriptHost = &m.scriptHost;
    ctx.dllReloader = &m.dllReloader;
    ctx.managedHost = &m.managedHost;
    ctx.particles = &m.particleSystem;
    ctx.computeAbi = &m.computeAbi;
    ctx.prefabs = &m.prefabLibrary;
    ctx.anims = &m.animLibrary;
    ctx.controllers = &m.controllerLibrary;
    ctx.assetDb = &m.assetDatabase;
    ctx.audio = &m.audioSystem;
    ctx.sounds = &m.soundLibrary;
    ctx.audioSources = &m.audioSources;
    ctx.mixers = &m.mixerLibrary;
    ctx.assetsRoot = m.assetsRoot;
    ctx.projectRoot = m.config.projectRoot;
    InitSimAssets(m.assetDatabase, m.inputActions, m.assetsRoot);
    ctx.inputActions = &m.inputActions;

    if (g_jobUsers.fetch_add(1) == 0) {
        jobs::System().Init();
    }
    m.jobsStarted = true;
    jobs::System().SetEnabled(m.config.useJobs);
    MYE_LOG_INFO("[jobs] %s (%d workers)", m.config.useJobs ? "enabled" : "disabled (serial)",
                 jobs::System().WorkerCount());
    ConfigureSimCaches(m.config, base + L"\\cache\\cooked");
    ctx.fixedDt = static_cast<float>(kFixedDt);
    // 出自 (M81c)。EngineLoop と同じ位置 (InitSimAssets の後・シーン構築の前)
    m.provenance = BuildRunProvenance(m.dllReloader, m.assetsRoot, kNetProtoVersion,
                                      /*withContentHash=*/true); // サーバの出自は常に完全な形で持つ

    m.simRefs.scene = &m.scene;
    m.simRefs.particles = &m.particleSystem.Cpu();
    m.simRefs.xpbd = &m.xpbd;
    m.simRefs.acoustic = &m.acoustic;
    m.simRefs.collision = &m.collisionSystem;
    m.simRefs.scripts = &m.scriptHost;
    m.simRefs.prevTickInput = m.prevTickInput;
    m.simRefs.audioHandleSeq = &m.audioHandleSeq;
    m.simRefs.tickIndex = &ctx.tickIndex;
    m.BuildTickServices();
    m.initialized = true; // 以降の失敗でも破棄時に外すべき注入が入っている

    // ---- 起動シーン ----
    ctx.playerCount = 1;
    if (m.config.localPlayers > 1) {
        ctx.playerCount = (m.config.localPlayers >= static_cast<int>(kMaxPlayers))
            ? kMaxPlayers
            : static_cast<uint32_t>(m.config.localPlayers);
    }
    ctx.hasSystemInput = setup.systemInput;
    m.OnStart(ctx);
    // OnStart で積まれた構造変更 (SetParent 等) を確定する (EngineLoop と同じ点)
    m.scene.GetWorld().ApplyStructuralChanges();
    MYE_LOG_INFO("[headless] sim ready (%u entities, scene=%s, assets=%s)",
                 m.scene.GetWorld().AliveCount(), WideToUtf8(m.scenePath).c_str(),
                 WideToUtf8(m.assetsRoot).c_str());
    return true;
}

HeadlessVerifyResult HeadlessSim::VerifyReplay()
{
    Impl& m = *impl_;
    HeadlessVerifyResult r;
    if (!m.initialized || m.config.replayVerifyPath.empty()) {
        MYE_LOG_ERROR("[headless] VerifyReplay needs Init() and a --replay-verify path");
        return r;
    }
    Activate();
    EngineContext& ctx = m.ctx;
    if (!m.player.Load(m.config.replayVerifyPath)) {
        return r; // 理由は Load が ERROR で出している
    }
    r.totalTicks = m.player.TickCount();
    // .rep の playerCount が指定に勝つ (tick レコード長がファイル側で決まっている)
    if (m.player.PlayerCount() == 0 || m.player.PlayerCount() > kMaxPlayers) {
        MYE_LOG_ERROR("[headless] .rep has an invalid player count: %u", m.player.PlayerCount());
        return r;
    }
    if (m.player.PlayerCount() != ctx.playerCount) {
        MYE_LOG_INFO("[replay] player lanes: %u (from the .rep; --local-players said %u)",
                     m.player.PlayerCount(), ctx.playerCount);
        ctx.playerCount = m.player.PlayerCount();
    }
    // M81b: システム入力を持つ記録は tick ごとにそれも流す (EngineLoop の verify 経路と同じ)
    ctx.hasSystemInput = m.player.HasSystemInput();
    if (!m.player.Snapshot().empty()) {
        // v4 の埋め込み初期状態: シーンの中身に依存せず記録開始時点へ丸ごと戻せる
        if (!RestoreSimSnapshot(m.simRefs, m.player.Snapshot().data(), m.player.Snapshot().size())) {
            MYE_LOG_ERROR("[replay] embedded snapshot could not be restored");
            return r;
        }
        MYE_LOG_INFO("[replay] restored embedded snapshot (%zu bytes)", m.player.Snapshot().size());
    } else {
        // 記録開始時の RNG 状態を復元して同一 tick 列を再現する (v3 以来の従来経路)
        m.scene.GetWorld().Rng().Restore(m.player.RngState(), m.player.RngInc());
    }
    // 検証中は音を止める扱い (EngineLoop と同じ。デバイスは無いので実害は無いが門を揃える)
    m.audioSystem.SetSuspended(true);
    r.ran = true;

    const auto t0 = std::chrono::steady_clock::now();
    // EngineLoop の verify 経路と同じ順序: 入力の置換 → RunOneTick (ハッシュ照合は RunOneTick の中)。
    // 実時間は待たない。不一致なら TickRunner が requestExit を立てる
    while (!ctx.requestExit && m.player.HasTick(ctx.tickIndex)) {
        ApplyReplayInputs(ctx, m.player);
        RunOneTick(m.tickServices);
    }
    r.elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    r.verifiedTicks = m.player.verifiedTicks;
    r.unverifiedTicks = m.player.unverifiedTicks;
    r.firstMismatchTick = m.player.firstMismatchTick;
    r.passed = !m.player.failed && m.exitCode == 0 && m.player.HasTick(ctx.tickIndex) == false;
    return r;
}

uint64_t HeadlessSim::RunTick(const InputSnapshot* lanes, const SystemInputTick* sys, bool resim)
{
    Impl& m = *impl_;
    Activate();
    ApplyConfirmedInputs(m.ctx, lanes, sys);
    m.tickServices.resim = resim;
    RunOneTick(m.tickServices);
    m.tickServices.resim = false;
    return WorldHash();
}

uint64_t HeadlessSim::WorldHash()
{
    Impl& m = *impl_;
    return HashWorld(m.scene.GetWorld(), m.simRefs.HashSources());
}

const SimRefs& HeadlessSim::Refs() const { return impl_->simRefs; }
uint32_t HeadlessSim::PlayerCount() const { return impl_->ctx.playerCount; }
void HeadlessSim::SetPokeTick(int64_t tick) { impl_->config.netPokeTick = tick; }

uint64_t HeadlessSim::TickIndex() const { return impl_->ctx.tickIndex; }
const std::wstring& HeadlessSim::AssetsRoot() const { return impl_->assetsRoot; }
const std::wstring& HeadlessSim::ShadowCopyDir() const { return impl_->shadowCopyDir; }
const SimProvenance& HeadlessSim::Provenance() const { return impl_->provenance; }

} // namespace mye
