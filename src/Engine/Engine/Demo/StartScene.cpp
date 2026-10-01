//====================================================================================
//                          StartScene.cpp
//  MyEngin/ 秋田蓮音                                                     10/02/2026
//                                          起動シーンの用意 (Runtime / ヘッドレス共通)
//====================================================================================
#include "Engine/Engine/Demo/StartScene.h"

#include <filesystem>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Engine/App/Project.h"
#include "Engine/Engine/Demo/DemoContent.h"
#include "Engine/Engine/Loop/EngineLoop.h"
#include "Engine/Engine/Physics/Fracture/FractureSystem.h" // PreloadFractureAssets
#include "Engine/Engine/Scene/Prefab.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Engine/Scene/SceneSerializer.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/Shader/ShaderManager.h"

namespace mye {

std::wstring PrepareStartScene(EngineContext& ctx, const StartSceneOptions& options)
{
    std::wstring scenePath = options.scenePath;
    const ShowcaseDef* showcase = options.showcase;

    ctx.shaders->Load("forward_lit");
    RegisterDemoContent(ctx);    // Editor と同じ実体登録 (AssetID 解決)
    RegisterAssetLibraries(ctx); // .prefab / .anim を登録
    if (scenePath.empty() && showcase != nullptr) {
        // ショーケースは表の保存先 (ShowcaseScenes.cpp)。cache\ の行はコードから毎回組む —
        // shot_verify はこの経路で撮るので、保存済みが残っていると exists() 側へ落ちて
        // golden が静かに変わる。bat 側で撮影前に消している
        scenePath = ShowcaseScenePath(*showcase, ctx.assetsRoot);
    } else if (scenePath.empty()) {
        scenePath = ctx.assetsRoot + L"\\scenes\\main.scene.json";
        ProjectManifest manifest; // ブートシーンはマニフェスト優先 (M26)
        if (!ctx.projectRoot.empty() && LoadProjectManifest(ctx.projectRoot, manifest)) {
            scenePath = ProjectBootScenePath(ctx.projectRoot, manifest);
        }
    }
    // ショーケース材質は無条件で登録する (M50a)。--scene で保存済みショーケースを
    // 直接開く経路でも実体が揃う。フラグでゲートすると、使わない側の材質が常に欠落する
    RegisterRtShowcaseContent(ctx);
    RegisterPartsShowcaseContent(ctx);
    RegisterFlowShowcaseContent(ctx); // M51j: flow_* 材質 (配布ブートシーンにも使う)
    RegisterLocalPlayersContent(ctx);  // M52g: mp_* 材質
    RegisterNetDuelContent(ctx);       // M52i: duel_* 材質
    RegisterRenderShowcaseContent(ctx); // M54a: rdemo_* 材質
    RegisterTerrainShowcaseContent(ctx); // M58c: tdemo_* 材質
    RegisterPhysicsShowcaseContent(ctx); // M59d: pdemo_* 材質
    RegisterJointShowcaseContent(ctx);   // M60i: jdemo_* 材質 + 車輪メッシュ
    RegisterFogShowcaseContent(ctx);     // M57追補: fdemo_* 材質
    RegisterParticleShowcaseContent(ctx); // M63a: vdemo_* 材質 + 手続きテクスチャ
    RegisterAcousticShowcaseContent(ctx); // M65b: adem_* 材質
    RegisterModalShowcaseContent(ctx);    // M76f: mdemo_* 材質
    if (options.runShowcasePrepare && showcase != nullptr && showcase->prepare != nullptr) {
        showcase->prepare(ctx);
    }
    if (std::filesystem::exists(scenePath)) {
        SceneSerializer::LoadFromFile(*ctx.scene, scenePath);
        // Editor と同じ「ロード直後 1 回」(M48e)。ここを揃えないと Editor で録った .rep と
        // 初期状態が食い違う
        Prefab::RefreshNonOverridden(*ctx.scene, *ctx.prefabs);
        // 最初の物理 tick より前に破片資産を先読みしておく (Editor 側と同じ)
        PreloadFractureAssets(ctx.scene->GetWorld());
    } else if (showcase != nullptr && showcase->build != nullptr) {
        showcase->build(ctx, options.showcaseOptions);
    } else {
        BuildDemoScene(ctx); // ブートシーンが無ければデモを構築
    }
    // Editor の PlayModeController::Play と同じ Save+Load リロードで EntityID を正規化する。
    // これにより Editor が録った .rep と決定論的に一致する (M8 規約)
    {
        const nlohmann::json snap = SceneSerializer::SaveToJson(*ctx.scene);
        SceneSerializer::LoadFromJson(*ctx.scene, snap);
    }
    return scenePath;
}

} // namespace mye
