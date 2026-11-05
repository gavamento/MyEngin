//====================================================================================
//                          SelfTestSuites.cpp
//  MyEngin/ 秋田蓮音                                                       11/04/2026
//                                          selftest のスイート登録表と選択・実行の実装
//====================================================================================
#include "Editor/SelfTest/SelfTestSuites.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

#include "Editor/Asset/AssetOpsSelfTest.h"
#include "Editor/Scene/CameraPilotSelfTest.h"
#include "Editor/SelfTest/AnimatorControllerEditSelfTest.h"
#include "Editor/SelfTest/BehaviorTreeEditorSelfTest.h"
#include "Editor/SelfTest/ComputeInspectorSelfTest.h"
#include "Editor/SelfTest/DecalSelfTest.h"
#include "Editor/SelfTest/DeviceLostRescueSelfTest.h"
#include "Editor/SelfTest/GameFlowSelfTest.h"
#include "Editor/SelfTest/HzbSelfTest.h"
#include "Editor/SelfTest/LightSelectionSelfTest.h"
#include "Editor/SelfTest/PartSelfTest.h"
#include "Editor/SelfTest/ProbeBakerSelfTest.h"
#include "Editor/SelfTest/RagdollBuildSelfTest.h"
#include "Editor/SelfTest/SpriteEditorSelfTest.h"
#include "Editor/SelfTest/SsrSelfTest.h"
#include "Editor/SelfTest/StartupSplashSelfTest.h"
#include "Editor/SelfTest/TerrainSelfTest.h"
#include "Editor/SelfTest/TilePaletteSelfTest.h"
#include "Editor/SourceControl/SourceControlSelfTest.h"
#include "Editor/Tools/EngineIndexDumpSelfTest.h"
#include "Editor/Tools/FractureEditorSelfTest.h"
#include "Editor/Tools/NavEditorSelfTest.h"
#include "Editor/Tools/PatrolRouteEditSelfTest.h"
#include "Editor/Undo/UndoSelfTest.h"
#include "Engine/Core/Ecs/EcsSelfTest.h"
#include "Engine/Core/Jobs/JobSystemSelfTest.h"
#include "Engine/Core/Localization/LocalizationSelfTest.h"
#include "Engine/Engine/Feel/FeelLibrarySelfTest.h"
#include "Engine/Engine/Feel/FeelMathSelfTest.h"
#include "Engine/Engine/Feel/FeelSystemSelfTest.h"
#include "Engine/Engine/Feel/SpringFollowSelfTest.h"
#include "Engine/Engine/AI/BehaviorTreeSelfTest.h"
#include "Engine/Engine/Acoustic/AcousticSelfTest.h"
#include "Engine/Engine/Animation/AnimatorControllerSelfTest.h"
#include "Engine/Engine/Animation/SkeletonSelfTest.h"
#include "Engine/Engine/App/EngineCliSelfTest.h"
#include "Engine/Engine/App/ProjectSettingsFileSelfTest.h"
#include "Engine/Engine/Asset/AssetDatabaseSelfTest.h"
#include "Engine/Engine/Asset/CookedCacheSelfTest.h"
#include "Engine/Engine/Asset/MeshLodSelfTest.h"
#include "Engine/Engine/Asset/SubAssetKeySelfTest.h"
#include "Engine/Engine/Audio/Playback/AudioSelfTest.h"
#include "Engine/Engine/Audio/Spatial/AcousticAudioSelfTest.h"
#include "Engine/Engine/Audio/Synth/ImpactSynthSelfTest.h"
#include "Engine/Engine/Audio/Synth/ModalAudioSelfTest.h"
#include "Engine/Engine/Audio/Synth/ModalSynthSelfTest.h"
#include "Engine/Engine/Compute/ComputeManagedSelfTest.h"
#include "Engine/Engine/Compute/ComputeScriptSelfTest.h"
#include "Engine/Engine/Compute/ComputeServiceSelfTest.h"
#include "Engine/Engine/Compute/ComputeSettingsSelfTest.h"
#include "Engine/Engine/Compute/ComputeStateSelfTest.h"
#include "Engine/Engine/Compute/KernelLintSelfTest.h"
#include "Engine/Engine/Compute/KernelSelfTest.h"
#include "Engine/Engine/Demo/TwoDDemoSelfTest.h"
#include "Engine/Engine/HotReload/DllReloaderSelfTest.h"
#include "Engine/Engine/HotReload/ReloadHubSelfTest.h"
#include "Engine/Engine/Loop/DeviceRecoverySelfTest.h"
#include "Engine/Engine/Loop/SimParallelSelfTest.h"
#include "Engine/Engine/Modal/ModalSelfTest.h"
#include "Engine/Engine/Navigation/GridNav2DSelfTest.h"
#include "Engine/Engine/Navigation/NavAgentSelfTest.h"
#include "Engine/Engine/Navigation/NavDeterminismSelfTest.h"
#include "Engine/Engine/Navigation/NavSurfaceSelfTest.h"
#include "Engine/Engine/Net/NetSelfTest.h"
#include "Engine/Engine/Net/ServerNetSelfTest.h"
#include "Engine/Engine/Online/OnlineSelfTest.h"
#include "Engine/Engine/Particles/ParticleSelfTest.h"
#include "Engine/Engine/Perception/PerceptionSelfTest.h"
#include "Engine/Engine/Physics/Collider/ConvexSelfTest.h"
#include "Engine/Engine/Physics/Fracture/FractureSelfTest.h"
#include "Engine/Engine/Physics/Fracture/FractureSkinSelfTest.h"
#include "Engine/Engine/Physics/Ragdoll/RagdollSelfTest.h"
#include "Engine/Engine/Physics/Rigid/PhysMatSelfTest.h"
#include "Engine/Engine/Physics/Rigid/PhysicsSelfTest.h"
#include "Engine/Engine/Physics/Xpbd/XpbdSelfTest.h"
#include "Engine/Engine/RayTracing/RtSelfTest.h"
#include "Engine/Engine/Rendering/Camera2DFollowSelfTest.h"
#include "Engine/Engine/Rendering/RenderStatsSelfTest.h"
#include "Engine/Engine/Rendering/ShadowCullSelfTest.h"
#include "Engine/Engine/Rendering/SkinBoundsSelfTest.h"
#include "Engine/Engine/Rendering/SkinPaletteCacheSelfTest.h"
#include "Engine/Engine/Rendering/Sprite2D/Line2DSelfTest.h"
#include "Engine/Engine/Rendering/Sprite2D/Sprite2DAssetSelfTest.h"
#include "Engine/Engine/Rendering/Sprite2D/SpriteShapeSelfTest.h"
#include "Engine/Engine/Rendering/WaterWaveSelfTest.h"
#include "Engine/Engine/Replay/CrashRingSelfTest.h"
#include "Engine/Engine/Replay/SimSnapshotSelfTest.h"
#include "Engine/Engine/Replay/TimeTravelSelfTest.h"
#include "Engine/Engine/Replay/WorldHasherSelfTest.h"
#include "Engine/Engine/Scene/SceneSelfTest.h"
#include "Engine/Engine/Scene/TagSelfTest.h"
#include "Engine/Engine/Schema/SchemaSelfTest.h"
#include "Engine/Engine/Script/TwoDAbiSelfTest.h"
#include "Engine/Engine/Session/SessionSelfTest.h"
#include "Engine/Engine/Tilemap/TilemapSelfTest.h"
#include "Engine/Engine/UI/FontSelfTest.h"
#include "Engine/Engine/UI/UISelfTest.h"
#include "Engine/Engine/Vfx/VfxSelfTest.h"
#include "Engine/Platform/InputActionsSelfTest.h"
#include "Engine/Renderer/Compute/ComputeAbiSelfTest.h"
#include "Engine/Renderer/Compute/ProjectComputeRunnerSelfTest.h"
#include "Engine/Renderer/Passes/OcclusionSelfTest.h"
#include "Engine/Renderer/Passes/WaterSurfaceSelfTest.h"
#include "Engine/Renderer/Pipeline/PixelPerfect2DSelfTest.h"
#include "Engine/Renderer/Pipeline/RenderSelfTest.h"
#include "Engine/Renderer/PostFx/FxStackSelfTest.h"
#include "Engine/Renderer/PostFx/ProjectEffectRunnerSelfTest.h"
#include "Engine/Renderer/Shader/ProjectShaderPropertiesSelfTest.h"
#include "Engine/Renderer/Shader/ShaderManagerProjectIndexSelfTest.h"
#include "Engine/Renderer/Shader/SurfaceDeferredSelfTest.h"
#include "Engine/Renderer/Shader/SurfaceMaterialSelfTest.h"
#include "Engine/Renderer/Shader/SurfaceShaderSelfTest.h"
#include "Engine/Renderer/Texture/ImageDiffSelfTest.h"
#include "Engine/Renderer/Texture/TextureCookSelfTest.h"

namespace mye {

namespace {

// 全走の順。Full は Debug の所要時間が長い上位 10 本 (sub-00b の計測表: plans/m95-action-combat/sub-00b.md)
// (M95 で足す場合は末尾の M95 マーカーの下へ)
const SelfTestSuite kSuites[] = {
    { "ecs", mye::RunEcsSelfTest, SelfTestTier::Fast, "" },
    { "sceneserializer", mye::RunSceneSerializerSelfTest, SelfTestTier::Fast, "" },
    { "undo", mye::RunUndoSelfTest, SelfTestTier::Fast, "" },
    { "render", mye::RunRenderSelfTest, SelfTestTier::Fast, "" },
    { "physics", mye::RunPhysicsSelfTest, SelfTestTier::Full, "" },
    { "ui", mye::RunUISelfTest, SelfTestTier::Fast, "" },
    { "animatorcontroller", mye::RunAnimatorControllerSelfTest, SelfTestTier::Fast, "" },
    { "assetdatabase", mye::RunAssetDatabaseSelfTest, SelfTestTier::Fast, "" },
    { "texturecook", mye::RunTextureCookSelfTest, SelfTestTier::Fast, "" },
    { "jobsystem", mye::RunJobSystemSelfTest, SelfTestTier::Fast, "" },
    { "vfx", mye::RunVfxSelfTest, SelfTestTier::Fast, "" },
    { "particle", mye::RunParticleSelfTest, SelfTestTier::Fast, "" },
    { "simparallel", mye::RunSimParallelSelfTest, SelfTestTier::Fast, "M90g: sim の並列化 (jobs あり / なし一致)" },
    { "assetops", mye::RunAssetOpsSelfTest, SelfTestTier::Fast, "" },
    { "font", mye::RunFontSelfTest, SelfTestTier::Fast, "" },
    { "audio", mye::RunAudioSelfTest, SelfTestTier::Fast, "" },
    { "rt", mye::RunRtSelfTest, SelfTestTier::Fast, "" },
    { "localization", mye::RunLocalizationSelfTest, SelfTestTier::Fast, "" },
    { "skeleton", mye::RunSkeletonSelfTest, SelfTestTier::Fast, "" },
    { "part", mye::RunPartSelfTest, SelfTestTier::Fast, "" },
    { "schema", mye::RunSchemaSelfTest, SelfTestTier::Fast, "" },
    { "cookedcache", mye::RunCookedCacheSelfTest, SelfTestTier::Fast, "" },
    { "inputactions", mye::RunInputActionsSelfTest, SelfTestTier::Fast, "" },
    { "gameflow", mye::RunGameFlowSelfTest, SelfTestTier::Fast, "" },
    { "worldhasher", mye::RunWorldHasherSelfTest, SelfTestTier::Fast, "" },
    { "simsnapshot", mye::RunSimSnapshotSelfTest, SelfTestTier::Fast, "" },
    { "timetravel", mye::RunTimeTravelSelfTest, SelfTestTier::Fast, "" },
    { "crashring", mye::RunCrashRingSelfTest, SelfTestTier::Fast, "" },
    { "imagediff", mye::RunImageDiffSelfTest, SelfTestTier::Fast, "" },
    { "net", mye::RunNetSelfTest, SelfTestTier::Fast, "" },
    // ---- 末尾 append (統合契約の予約 7) ----
    { "lightselection", mye::RunLightSelectionSelfTest, SelfTestTier::Fast, "M54b" },
    { "terrain", mye::RunTerrainSelfTest, SelfTestTier::Fast, "M58b" },
    { "decal", mye::RunDecalSelfTest, SelfTestTier::Fast, "M56a" },
    { "hzb", mye::RunHzbSelfTest, SelfTestTier::Fast, "M56c" },
    { "ssr", mye::RunSsrSelfTest, SelfTestTier::Fast, "M56d" },
    { "probebaker", mye::RunProbeBakerSelfTest, SelfTestTier::Fast, "M56e" },
    { "physmat", mye::RunPhysMatSelfTest, SelfTestTier::Fast, "M59a1" },
    { "convex", mye::RunConvexSelfTest, SelfTestTier::Fast, "M60f" },
    { "ragdoll", mye::RunRagdollSelfTest, SelfTestTier::Fast, "M60g1" },
    { "ragdollbuild", mye::RunRagdollBuildSelfTest, SelfTestTier::Fast, "M60g2" },
    { "camerapilot", mye::RunCameraPilotSelfTest, SelfTestTier::Fast, "カメラ操縦 (視錐台 UI 追補)" },
    { "dllreloader", mye::RunDllReloaderSelfTest, SelfTestTier::Fast, "DLL 書き込み完了プローブ (M52h 追補)" },
    { "xpbd", mye::RunXpbdSelfTest, SelfTestTier::Fast, "M60'b" },
    { "acoustic", mye::RunAcousticSelfTest, SelfTestTier::Fast, "M65a" },
    { "sourcecontrol", mye::RunSourceControlSelfTest, SelfTestTier::Fast, "M66a" },
    { "acousticaudio", mye::RunAcousticAudioSelfTest, SelfTestTier::Fast, "M68a" },
    { "subassetkey", mye::RunSubAssetKeySelfTest, SelfTestTier::Fast, "M74a / M74b" },
    { "impactsynth", mye::RunImpactSynthSelfTest, SelfTestTier::Fast, "ImpactSynth (計画 ImpactSoundDesign)" },
    { "reloadhub", mye::RunReloadHubSelfTest, SelfTestTier::Fast, "ホットリロードの資産の種類表" },
    { "modalsynth", mye::RunModalSynthSelfTest, SelfTestTier::Fast, "M76a: Deep-Modal のモード合成器" },
    { "modal", mye::RunModalSelfTest, SelfTestTier::Fast, "M76b: Deep-Modal のボクセライザ" },
    { "modalaudio", mye::RunModalAudioSelfTest, SelfTestTier::Fast, "M76f: 衝突 -> モーダル一発再生の橋渡し" },
    { "tag", mye::RunTagSelfTest, SelfTestTier::Fast, "汎用タグ / RT のタグフィルタ / シェーダキャッシュ" },
    { "waterwave", mye::RunWaterWaveSelfTest, SelfTestTier::Fast, "三角関数 (Gerstner波) による水面波" },
    { "enginecli", mye::RunEngineCliSelfTest, SelfTestTier::Fast, "両 Main 共通の CLI フラグ表" },
    { "projectsettingsfile", mye::RunProjectSettingsFileSelfTest, SelfTestTier::Fast, "project_settings.json: 他キーの保持 / 壊れたファイルは上書きしない" },
    { "projectshaderproperties", mye::RunProjectShaderPropertiesSelfTest, SelfTestTier::Fast, "M78a: Properties DSL パース/パック" },
    { "projecteffectrunner", mye::RunProjectEffectRunnerSelfTest, SelfTestTier::Fast, "M78b: ポスト挿入点・ソート" },
    { "fxstack", mye::RunFxStackSelfTest, SelfTestTier::Fast, "M78c: fxstack ロード/保存" },
    { "projectcomputerunner", mye::RunProjectComputeRunnerSelfTest, SelfTestTier::Fast, "M78d: CS dispatch 基盤" },
    { "shadermanagerprojectindex", mye::RunShaderManagerProjectIndexSelfTest, SelfTestTier::Fast, "M78: assets 全域索引" },
    { "computeabi", mye::RunComputeAbiSelfTest, SelfTestTier::Fast, "M78e: Compute ABI v21" },
    { "surfaceshader", mye::RunSurfaceShaderSelfTest, SelfTestTier::Fast, "M79a: サーフェス規約・生成エントリ" },
    { "surfacematerial", mye::RunSurfaceMaterialSelfTest, SelfTestTier::Full, "M79b: Forward 描画とマテリアル Properties" },
    { "surfacedeferred", mye::RunSurfaceDeferredSelfTest, SelfTestTier::Full, "M79c: Deferred サーフェス段・CSM 影" },
    { "watersurface", mye::RunWaterSurfaceSelfTest, SelfTestTier::Fast, "M79e: 水面サーフェス経路・MyEngineWater" },
    { "fracture", mye::RunFractureSelfTest, SelfTestTier::Full, "M80a: 破壊分割コア (閉じ判定・平面切断)" },
    { "fractureeditor", mye::RunFractureEditorSelfTest, SelfTestTier::Full, "M80i: 破壊物 Inspector の焼き回り" },
    { "fractureskin", mye::RunFractureSkinSelfTest, SelfTestTier::Fast, "M80j: スキンメッシュの破壊" },
    { "session", mye::RunSessionSelfTest, SelfTestTier::Fast, "M81b: システム入力 / SessionLanes / .rep v9" },
    { "servernet", mye::RunServerNetSelfTest, SelfTestTier::Full, "M81d: 入力確定型サーバ/クライアント (1 プロセス内の偽トランスポート)" },
    { "online", mye::RunOnlineSelfTest, SelfTestTier::Fast, "M93: 参加コード / P2P 中継の分割 / local 実装のロビー" },
    { "navdeterminism", mye::RunNavDeterminismSelfTest, SelfTestTier::Fast, "M82a: Recast のビット一致と状態の保存・復元" },
    { "navsurface", mye::RunNavSurfaceSelfTest, SelfTestTier::Fast, "M82b: Surface のベイク入力・.mnav・NavSystem" },
    { "naveditor", mye::RunNavEditorSelfTest, SelfTestTier::Fast, "M82b: Create / Bake / Clear の皮" },
    { "navagent", mye::RunNavAgentSelfTest, SelfTestTier::Full, "M82c: Agent・dtCrowd・SimSnapshot の Nav 節" },
    { "perception", mye::RunPerceptionSelfTest, SelfTestTier::Fast, "M83: AI の知覚 (視覚・聴覚・ダメージ・接触・予測)" },
    { "behaviortree", mye::RunBehaviorTreeSelfTest, SelfTestTier::Full, "M85: ビヘイビアツリー (アセット・Composite・BT 節)" },
    { "patrolrouteedit", mye::RunPatrolRouteEditSelfTest, SelfTestTier::Fast, "M85g: 巡回ルートの点の編集と Undo" },
    { "engineindexdump", mye::RunEngineIndexDumpSelfTest, SelfTestTier::Fast, "M87a: 索引ダンプの件数・決定性" },
    { "behaviortreeeditor", mye::RunBehaviorTreeEditorSelfTest, SelfTestTier::Fast, "M85h: BT 窓のモデル層 (追加・接続・削除・保存)" },
    { "animatorcontrolleredit", mye::RunAnimatorControllerEditSelfTest, SelfTestTier::Fast, "M89g: コントローラ窓の骨の駆動の編集" },
    { "devicelostrescue", mye::RunDeviceLostRescueSelfTest, SelfTestTier::Fast, "M88a: デバイス消失時の退避保存" },
    { "devicerecovery", mye::RunDeviceRecoverySelfTest, SelfTestTier::Fast, "M88b: 復旧の連続消失制限と旧デバイス参照数ゲート" },
    { "renderstats", mye::RunRenderStatsSelfTest, SelfTestTier::Fast, "M90a: 描画統計のビュー別集計と JSON ダンプ" },
    { "occlusion", mye::RunOcclusionSelfTest, SelfTestTier::Full, "M90b: GPU オクルージョン (max-Z HZB・保守的判定・ON/OFF 画素一致)" },
    { "shadowcull", mye::RunShadowCullSelfTest, SelfTestTier::Fast, "M90d: CSM のカスケード別判定 (近平面を除く 5 面・run 数)" },
    { "skinbounds", mye::RunSkinBoundsSelfTest, SelfTestTier::Fast, "M90d: スキンの保守的 AABB (全クリップの全フレームを包む)" },
    { "meshlod", mye::RunMeshLodSelfTest, SelfTestTier::Fast, "M90e: メッシュ LOD (生成・クック・.meta・選択)" },
    { "skinpalettecache", mye::RunSkinPaletteCacheSelfTest, SelfTestTier::Fast, "M90f: パレットのキャッシュ・URO の更新 tick・LOD 履歴の破棄" },
    { "kernel", mye::RunKernelSelfTest, SelfTestTier::Fast, "M91a: *.kernel.hlsl の GPU/CPU 単一ソース・CPU 実行器・ハッシュ" },
    { "kernellint", mye::RunKernelLintSelfTest, SelfTestTier::Fast, "M91b: カーネル検査器と C# 変換器" },
    { "computeservice", mye::RunComputeServiceSelfTest, SelfTestTier::Fast, "M91c: ForEach のエンジン核 (ストア・判断・Fx GPU)" },
    { "computestate", mye::RunComputeStateSelfTest, SelfTestTier::Fast, "M91d: State バッファのスナップショット・ハッシュ・寿命" },
    { "computescript", mye::RunComputeScriptSelfTest, SelfTestTier::Fast, "M91e: GameLogic のカーネル表・COMPUTE_KERNELS・ForEach の ABI" },
    { "computemanaged", mye::RunComputeManagedSelfTest, SelfTestTier::Fast, "M91g: C# の ForEach・C# の CPU 版 (C++ 版とのバイト一致)" },
    { "computesettings", mye::RunComputeSettingsSelfTest, SelfTestTier::Fast, "M91h: compute 節・Compute 欄の行・カーネルのホットリロード" },
    { "computeinspector", mye::RunComputeInspectorSelfTest, SelfTestTier::Fast, "M91h: Compute 欄の文字列 (日英)" },
    { "startupsplash", mye::RunStartupSplashSelfTest, SelfTestTier::Fast, "起動スプラッシュの進捗とメビウス幾何" },
    { "sprite2dasset", mye::RunSprite2DAssetSelfTest, SelfTestTier::Fast, "M94c: スプライト資産 (.meta の sprites) とソート層" },
    { "tilemap", mye::RunTilemapSelfTest, SelfTestTier::Fast, "M94j: タイルパレット / タイルマップ資産と座標変換" },
    { "spriteeditor", mye::RunSpriteEditorSelfTest, SelfTestTier::Fast, "M94e: スプライトエディタ (スライス純関数 / Undo / 保存)" },
    { "tilepalette", mye::RunTilePaletteSelfTest, SelfTestTier::Fast, "M94l: タイルパレット窓の塗り (塗り潰し / 矩形 / Undo / ピック)" },
    { "twodabi", mye::RunTwoDAbiSelfTest, SelfTestTier::Fast, "M94n: ABI v31 (箱のクエリ / SpriteByName / Tilemap)" },
    { "line2d", mye::RunLine2DSelfTest, SelfTestTier::Fast, "M94r: Line2D の帯・塗りの頂点生成と描画リスト" },
    { "spriteshape", mye::RunSpriteShapeSelfTest, SelfTestTier::Fast, "M94s: SpriteShape2D の輪郭・縁・塗り・辺の箱・丘を転がる球" },
    { "gridnav2d", mye::RunGridNav2DSelfTest, SelfTestTier::Fast, "M94u: GridAgent2D のグリッド A* (段差 / 穴 / NoPath / 並走一致)" },
    { "camera2dfollow", mye::RunCamera2DFollowSelfTest, SelfTestTier::Fast, "M94v: Camera2DFollow (デッドゾーン / damping / 境界 / 並走一致)" },
    { "pixelperfect2d", mye::RunPixelPerfect2DSelfTest, SelfTestTier::Fast, "M94v: pixelperfect2d::Compute の表" },
    { "twoddemo", mye::RunTwoDDemoSelfTest, SelfTestTier::Full, "M94w: --2d-demo の 3000 tick 不変量と焼き込みハッシュ" },
    { "feelmath", mye::RunFeelMathSelfTest, SelfTestTier::Fast, "M94a (手触り): イージング / カーブ / ばね / 値ノイズ" },
    { "feelsystem", mye::RunFeelSystemSelfTest, SelfTestTier::Fast, "M94c (手触り): 再生中の手触り (経過・置換・評価・内容ゲート)" },
    { "feellibrary", mye::RunFeelLibrarySelfTest, SelfTestTier::Fast, "M94d (手触り): .feel.json の往復・名前の解決・FeelTrigger" },
    { "springfollow", mye::RunSpringFollowSelfTest, SelfTestTier::Fast, "M94g (手触り): ばね追従 (瞬間移動・収束・空間変換・回転)" },
};

const SelfTestAlias kAliases[] = {
    { "twod", "twoddemo" },
    { "m94", "physics,fracture,tilemap,ui,sprite2dasset,spriteeditor,tilepalette,animatorcontroller,animatorcontrolleredit,assetdatabase,reloadhub,assetops,twodabi,part,surfaceshader,surfacematerial,surfacedeferred,line2d,spriteshape,gridnav2d,camera2dfollow,pixelperfect2d,perception,behaviortree,twoddemo" },
    { "m95", "physics,animatorcontroller,skeleton,part,twodabi,simsnapshot,worldhasher" },
};

constexpr const char* kServerNetPrefix = "servernet";

std::string Lower(std::string s)
{
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

std::vector<std::string> SplitList(const std::string& text)
{
    std::vector<std::string> items;
    size_t begin = 0;
    while (begin <= text.size()) {
        size_t end = text.find(',', begin);
        if (end == std::string::npos) end = text.size();
        std::string item = text.substr(begin, end - begin);
        const size_t first = item.find_first_not_of(" \t");
        const size_t last = item.find_last_not_of(" \t");
        if (first != std::string::npos) items.push_back(Lower(item.substr(first, last - first + 1)));
        begin = end + 1;
    }
    return items;
}

// 名前 1 個 (スイート名 / servernet-* / 別名) を表の添字へ展開する。解決できなければ false
bool ExpandName(const std::string& name, std::vector<size_t>& out)
{
    const std::span<const SelfTestSuite> suites = GetSelfTestSuites();
    // servernet-v1 など: 絞り込みは ServerNetSelfTest.cpp が環境変数から自分で読むので、スイートは servernet 1 本
    const std::string lookup = name.rfind(kServerNetPrefix, 0) == 0 ? std::string(kServerNetPrefix) : name;
    for (size_t i = 0; i < suites.size(); ++i) {
        if (lookup == suites[i].name) {
            out.push_back(i);
            return true;
        }
    }
    for (const SelfTestAlias& alias : GetSelfTestAliases()) {
        if (name == alias.name) {
            for (const std::string& member : SplitList(alias.members)) {
                if (!ExpandName(member, out)) return false;
            }
            return true;
        }
    }
    return false;
}

const char* TierName(SelfTestTier tier)
{
    return tier == SelfTestTier::Fast ? "fast" : "full";
}

const char* ModeName(SelfTestMode mode)
{
    switch (mode) {
    case SelfTestMode::Full: return "full";
    case SelfTestMode::Only: return "only";
    default: return "fast";
    }
}

} // namespace

std::span<const SelfTestSuite> GetSelfTestSuites()
{
    return std::span<const SelfTestSuite>(kSuites);
}

std::span<const SelfTestAlias> GetSelfTestAliases()
{
    return std::span<const SelfTestAlias>(kAliases);
}

bool ResolveSelfTestSelection(const std::string& onlyValue, bool full, SelfTestSelection& out)
{
    out = SelfTestSelection{};
    const std::span<const SelfTestSuite> suites = GetSelfTestSuites();
    const std::vector<std::string> items = SplitList(onlyValue);
    if (items.empty()) {
        out.mode = full ? SelfTestMode::Full : SelfTestMode::Fast;
        for (size_t i = 0; i < suites.size(); ++i) {
            if (full || suites[i].tier == SelfTestTier::Fast) out.indices.push_back(i);
        }
        return true;
    }

    out.mode = SelfTestMode::Only;
    std::vector<size_t> expanded;
    for (const std::string& item : items) {
        if (!ExpandName(item, expanded)) {
            out.unknownName = item;
            return false;
        }
    }
    // 別名 1 個だけの指定は別名の並びを守る (m94 など、従来の並びで回すため)。重複は先勝ちで落とす
    const bool keepAliasOrder = items.size() == 1 && items[0].rfind(kServerNetPrefix, 0) != 0
        && std::any_of(GetSelfTestAliases().begin(), GetSelfTestAliases().end(),
                       [&](const SelfTestAlias& a) { return items[0] == a.name; });
    if (!keepAliasOrder) std::sort(expanded.begin(), expanded.end());
    for (size_t index : expanded) {
        if (std::find(out.indices.begin(), out.indices.end(), index) == out.indices.end()) out.indices.push_back(index);
    }
    return true;
}

std::vector<std::string> VerifySelfTestSelectionRules()
{
    std::vector<std::string> failures;
    const std::span<const SelfTestSuite> suites = GetSelfTestSuites();
    auto names = [&](const SelfTestSelection& sel) {
        std::vector<std::string> list;
        for (size_t index : sel.indices) list.push_back(suites[index].name);
        return list;
    };
    auto expect = [&](bool cond, const char* what) {
        if (!cond) failures.push_back(what);
    };
    using Names = std::vector<std::string>;

    SelfTestSelection sel;
    expect(ResolveSelfTestSelection("", false, sel) && sel.mode == SelfTestMode::Fast, "fast: 未指定は fast モード");
    size_t fastCount = 0;
    for (const SelfTestSuite& suite : suites) fastCount += suite.tier == SelfTestTier::Fast ? 1 : 0;
    expect(sel.indices.size() == fastCount, "fast: tier が Fast のスイートだけ");
    expect(std::is_sorted(sel.indices.begin(), sel.indices.end()), "fast: 表の順");

    expect(ResolveSelfTestSelection("", true, sel) && sel.mode == SelfTestMode::Full, "full: full モード");
    expect(sel.indices.size() == suites.size(), "full: 全スイート");
    expect(std::is_sorted(sel.indices.begin(), sel.indices.end()), "full: 表の順");

    // only は tier を問わず、書いた順ではなく表の順 (重複と大文字・空白は吸収)
    expect(ResolveSelfTestSelection(" UNDO , ecs,undo", false, sel) && sel.mode == SelfTestMode::Only
               && names(sel) == Names{ "ecs", "undo" }, "only: 表の順・重複除去・小文字化・空白除去");
    expect(ResolveSelfTestSelection("ecs", true, sel) && sel.mode == SelfTestMode::Only
               && names(sel) == Names{ "ecs" }, "only: full より優先");

    // 別名 1 個だけは別名の並び。m95 は表では worldhasher が simsnapshot より前
    expect(ResolveSelfTestSelection("m95", false, sel)
               && names(sel) == Names{ "physics", "animatorcontroller", "skeleton", "part", "twodabi", "simsnapshot", "worldhasher" },
           "alias: 別名 1 個は別名の並び");
    expect(ResolveSelfTestSelection("m95,ecs", false, sel) && !names(sel).empty() && names(sel).front() == "ecs"
               && sel.indices.size() == 8 && std::is_sorted(sel.indices.begin(), sel.indices.end()),
           "alias: 別名と他の項目の混在は表の順");

    expect(ResolveSelfTestSelection("servernet-v1", false, sel) && names(sel) == Names{ "servernet" }, "servernet-*: 前方一致で servernet 1 本");

    expect(!ResolveSelfTestSelection("ecs,nosuchsuite", false, sel) && sel.unknownName == "nosuchsuite", "unknown: 未知名は false で名前を返す");
    return failures;
}

void PrintSelfTestList()
{
    for (const SelfTestSuite& suite : GetSelfTestSuites()) {
        std::printf("%s\t%s\n", suite.name, TierName(suite.tier));
    }
    for (const SelfTestAlias& alias : GetSelfTestAliases()) {
        std::printf("%s\t= %s\n", alias.name, alias.members);
    }
    std::fflush(stdout);
}

bool RunSelfTestSelection(const SelfTestSelection& selection)
{
    using Clock = std::chrono::steady_clock;
    const std::span<const SelfTestSuite> suites = GetSelfTestSuites();
    bool allOk = true;
    int failed = 0;
    const Clock::time_point totalStart = Clock::now();
    for (size_t index : selection.indices) {
        const SelfTestSuite& suite = suites[index];
        const Clock::time_point start = Clock::now();
        // &= を使わず結果を受けるだけにして、先行失敗があっても次を必ず回す
        const bool ok = suite.run();
        const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
        std::printf("[selftest] %s %s (%lld ms, %s)\n", suite.name, ok ? "PASS" : "FAIL", ms, TierName(suite.tier));
        if (!ok) {
            allOk = false;
            ++failed;
        }
    }
    const long long totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - totalStart).count();
    std::printf("[selftest] total %zu suites, %d failed, %lld ms (mode: %s)\n",
                selection.indices.size(), failed, totalMs, ModeName(selection.mode));
    std::fflush(stdout);
    return allOk;
}

} // namespace mye
