//====================================================================================
//                          SimInit.cpp
//  MyEngin/ 秋田蓮音                                                     10/02/2026
//                                          sim 側の起動手順 (EngineLoop / HeadlessSim 共通)
//====================================================================================
#include "Engine/Engine/Loop/SimInit.h"

#include <filesystem>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Engine/App/Project.h"
#include "Engine/Engine/Asset/AssetDatabase.h"
#include "Engine/Engine/Asset/CookedCache.h"
#include "Engine/Engine/HotReload/DllReloader.h"
#include "Engine/Engine/Loop/EngineLoop.h"
#include "Engine/Engine/Physics/Collider/ConvexColliderLibrary.h"
#include "Engine/Engine/Physics/Collider/MeshColliderLibrary.h"
#include "Engine/Engine/Physics/Collider/TerrainColliderLibrary.h"
#include "Engine/Engine/Physics/Fracture/FractureLibrary.h"
#include "Engine/Engine/Physics/Rigid/PhysMatLibrary.h"
#include "Engine/Engine/Schema/SchemaComponents.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Engine/Scene/TagNames.h"
#include "Engine/Engine/Script/EngineApiTable.h"
#include "Engine/Engine/Script/ManagedHost.h"
#include "Engine/Engine/Script/ScriptHost.h"
#include "Engine/Engine/Session/Provenance.h"
#include "Engine/Engine/UI/UILayout.h"
#include "Engine/Engine/UI/UIProjectSettings.h"
#include "Engine/Engine/UI/UITextMetrics.h"
#include "Engine/Platform/InputActions.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/Device/GpuResources.h"

namespace mye {

bool ResolveAssetsRoot(const EngineConfig& config, std::wstring& assetsRoot)
{
    // --project 指定時は <root>\assets を使う。未指定はレガシー動作 (exe から上へ assets を探索)
    if (config.projectRoot.empty()) {
        assetsRoot = FindAssetsRoot();
        return true;
    }
    assetsRoot = config.projectRoot + L"\\assets";
    std::error_code ec;
    if (!std::filesystem::exists(assetsRoot, ec)) {
        MYE_LOG_ERROR("project assets not found: %s", WideToUtf8(assetsRoot).c_str());
        return false;
    }
    return true;
}

void InstallSimLibraries(const SimLibraries& libs)
{
    // M41: 静的メッシュコライダー (Collider.shape=3)。pose 構築サイトが meshcol::Resolve で
    // AssetID → BVH 付きコライダーデータを引けるように接続する
    libs.meshColliders->Init(libs.resources);
    meshcol::Install(libs.meshColliders);
    // M60f: 凸包コライダー (Collider.shape=5)。meshcol と同じ AssetID→形状の解決だが
    // クック (.mcvx) が乗るので CookedCache::Configure より後で使われること (Get は lazy)
    libs.convexColliders->Init(libs.resources);
    convexcol::Install(libs.convexColliders);
    // M80c: 破片資産 (.mfrac)。凸包は convexColliders へ委譲する
    libs.fractureAssets->Init(libs.resources, libs.convexColliders);
    fracturelib::Install(libs.fractureAssets);
    // M59a1: 物理マテリアル (.physmat.json)。起動走査 (RegisterAssetLibraries) より前に注入する
    physmat::Install(libs.physMat);
    // M59i: 地形コライダー (Collider.shape=4)。描画の TerrainSystem とは別に sim 用の
    // 地形データを持つ — 描画のキャッシュを読むと「絵を出したかどうか」で sim が変わる
    terraincol::Install(libs.terrainColliders);
}

void UninstallSimLibraries()
{
    meshcol::Install(nullptr);
    convexcol::Install(nullptr);
    fracturelib::Install(nullptr);
    physmat::Install(nullptr);
    terraincol::Install(nullptr);
}

void InitSimProjectState(const std::wstring& assetsRoot)
{
    // M75c: UI の既定キャンバスの基準解像度 (project_settings.json の ui 節)。**tick が回る前に
    // 1 回だけ**書く静的な値 — sim のヒットテストが読むので、途中で変えると同じ .rep の再生が割れる
    // (.rep には載せない。ネットは NetIdentity.referenceW/H で入口照合する)
    {
        const uilayout::ProjectUiSettings uiSettings = uilayout::LoadProjectUiSettings(assetsRoot);
        uilayout::SetDefaultCanvasReference(uiSettings.referenceW, uiSettings.referenceH);
        if (uiSettings.referenceW != uilayout::kCanvasRefW
            || uiSettings.referenceH != uilayout::kCanvasRefH) {
            MYE_LOG_INFO("[ui] default canvas reference %dx%d (project_settings.json)",
                         uiSettings.referenceW, uiSettings.referenceH);
        }
    }
    // M75d: フォント計測表 (assets\fonts\<描画フォント>.fontmetrics.json)。基準解像度と同じく
    // **tick が回る前に 1 回だけ**。Layout / Fitter が sim の中で読むので、途中で差し替えると
    // 再シムや .rep の検証が割れる。表が無いときは固定メトリクス (ロード側が必要なら WARN を出す)。
    // ★`--font-embedded` とは無関係に読む — 描画フラグで sim の入力が変わってはならない
    {
        uitext::SetActiveFontMetrics(uitext::LoadProjectFontMetrics(assetsRoot));
        const uitext::FontMetrics& fm = uitext::ActiveFontMetrics();
        if (!fm.Empty()) {
            MYE_LOG_INFO("[ui] font metrics: %s (%u glyphs, hash 0x%016llx)", fm.FontName().c_str(),
                         fm.GlyphCount(), static_cast<unsigned long long>(fm.Hash()));
        }
    }
    // 汎用タグ: 名前の表を tick より前に読んでおく (スクリプトの TagIndex が tick 中に表を
    // 書き換えないように)
    TagNames::Get().Load(assetsRoot);
}

bool LoadGameLogic(Scene& scene, ScriptHost& scriptHost, DllReloader& dllReloader,
                   const std::wstring& assetsRoot, const std::wstring& dllPath,
                   const std::wstring& shadowCopyDir)
{
    // ---- スキーマ由来の動的コンポーネント (M48j) ----
    // ★呼ぶ位置がそのまま決定論の契約: 組込み型 (World の生成時に RegisterBuiltinComponents で
    //   済んでいる) の後、**GameLogic.dll のスクリプト型より前**。この 1 箇所に固定しておくと
    //   スキーマ型は組込み群とスクリプト群の間の連続ブロックになり、スクリプト型の TypeId は
    //   一様にずれるだけ = エンティティ内の相対順が変わらない = 既存シーンのハッシュ不変
    schema::RegisterSchemaComponents(assetsRoot);

    scriptHost.Init(&scene);
    dllReloader.Init(&scriptHost, dllPath, shadowCopyDir);
    if (!dllReloader.LoadInitial()) {
        // ★黙って続けない。C++ スクリプトが 1 本も無い世界は「動くけれど別物」で、
        //   リプレイもネットも全く違う結果になる (M52h でシャドウコピーの衝突により
        //   実際に踏んだ)。エンジンは継続できるので停止まではしないが、
        //   ログ上で必ず目立たせる
        MYE_LOG_ERROR("[dll] GameLogic.dll was not loaded - NO C++ scripts are registered "
                      "(the world will not match a normal run)");
        return false;
    }
    return true;
}

void WireScriptServices(ScriptHost& scriptHost, ManagedHost& managedHost,
                        const SimSharedServices& s, bool developmentRun)
{
    scriptHost.SetSharedServices(s.audioQueue, s.pendingScene, s.effectQueue, s.debugLines,
                                 s.audioHandleSeq, s.inputActions, s.pendingSaveSlot,
                                 s.pendingLoadSlot, s.padVibration, s.netInfo, s.cursorLock,
                                 s.pendingLoadPersistSlot, s.windowMode);
    managedHost.SetSharedServices(s.audioQueue, s.pendingScene, s.effectQueue, s.debugLines,
                                  s.audioHandleSeq, s.inputActions, s.pendingSaveSlot,
                                  s.pendingLoadSlot, s.padVibration, s.netInfo, s.cursorLock,
                                  s.pendingLoadPersistSlot, s.windowMode);
    // v18: 開発中の実行か。プロセスの定数なので起動時に 1 回だけ渡す (sim 状態ではない = .rep に載らない)
    scriptHost.SetDevelopmentRun(developmentRun);
    managedHost.SetDevelopmentRun(developmentRun);
    scriptHost.SetNavSystem(s.nav);
    managedHost.SetNavSystem(s.nav);
}

void InitSimAssets(AssetDatabase& assetDatabase, InputActions& inputActions,
                   const std::wstring& assetsRoot)
{
    // M51d: 入力アクションマップ (assets\input\actions.json)。不在 = 空マップ = no-op
    inputActions.Load(assetsRoot);
    // M23: assets\ を走査して .meta サイドカー (GUID) を生成/同期する。
    // アセット登録 (シーン構築 → RegisterAssetLibraries) の前に済ませ、パス⇄GUID 解決を利用可能にする。
    assetDatabase.ScanAndSync(assetsRoot);
    // M30c: 以後の path→AssetID キー計算 (IdForFile/HashForPath) を GUID 解決経由にする。
    // 未移動アセットは GUID == path-hash なので既存シーン/リプレイはビット不変
    assetDatabase.InstallAsKeyResolver();
}

void ConfigureSimCaches(const EngineConfig& config, const std::wstring& cookedDir)
{
    // M51a: sim 索引 (World クエリキャッシュ / Scene fileId 索引)。--no-sim-cache で素通し
    World::SetSimCacheEnabled(config.useSimCache);
    MYE_LOG_INFO("[simcache] %s", config.useSimCache ? "enabled" : "disabled (linear)");
    // M51b: アセットクックキャッシュ (モデル + .ogg PCM)。--no-cook-cache で毎回フルパース。
    // RegisterAssetLibraries (シーン構築) より前に設定しておくこと
    CookedCache::Configure(cookedDir, config.useCookCache);
    MYE_LOG_INFO("[cook] %s (%s)",
                 config.useCookCache ? "enabled" : "disabled (parse every launch)",
                 WideToUtf8(cookedDir).c_str());
}

uint32_t BuildSessionConfigBits(const EngineConfig& config)
{
    return (config.synthInput ? kCfgSynthInput : 0u) | (config.useJobs ? kCfgJobs : 0u)
        | (config.useSimCache ? kCfgSimCache : 0u) | (config.useCookCache ? kCfgCookCache : 0u)
        | (config.allowGameMismatch ? kCfgAllowGameMismatch : 0u);
}

void FillSessionConfigFromProject(SessionConfig& cfg, const EngineConfig& config)
{
    cfg.tickRate = 60;
    cfg.configBits = BuildSessionConfigBits(config);
    cfg.referenceW = static_cast<uint32_t>(uilayout::DefaultCanvasDesc().referenceW);
    cfg.referenceH = static_cast<uint32_t>(uilayout::DefaultCanvasDesc().referenceH);
    cfg.fontMetricsHash = uitext::ActiveFontMetrics().Hash();
}

SimProvenance BuildRunProvenance(const DllReloader& dllReloader, const std::wstring& assetsRoot,
                                 uint32_t protocolVersion, bool withContentHash)
{
    const std::string_view git = EngineBuildGit();
    if (IsDirtyGit(git)) {
        // dirty 同士は中身が違っても同じ engineVersion になる (開発時の既知の穴)
        MYE_LOG_WARN("[provenance] engine build %s has uncommitted changes - two different dirty "
                     "builds compare as equal", std::string(git).c_str());
    }
    const SimProvenance p = MakeSimProvenance(
        dllReloader.LoadedGameVersion(), withContentHash ? ResolveContentHash(assetsRoot) : 0,
        protocolVersion);
    MYE_LOG_INFO("%s%s", FormatProvenance(p).c_str(),
                 withContentHash ? "" : " (content hash not computed: no net / recording)");
    return p;
}

} // namespace mye
