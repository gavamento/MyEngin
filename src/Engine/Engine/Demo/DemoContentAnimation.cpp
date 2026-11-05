// 骨アニメ系ショーケース (--parts-demo / --anim-demo)。
// DemoContent.cpp から領域ごとの翻訳単位へ分けたもの (M93g)。関数の契約は DemoContent.h、
// ショーケースを跨いで使うヘルパーは DemoContentInternal.h。
#include "Engine/Engine/Demo/DemoContent.h"
#include "Engine/Engine/Demo/DemoContentInternal.h"

#include <cstdio>

#include "Engine/Core/Ecs/ComponentRegistry.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/HierarchyWalk.h"
#include "Engine/Core/Util/Hash.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Engine/Loop/EngineLoop.h"
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/Asset/ModelLoader.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/Device/GpuResources.h"
#include "Engine/Renderer/Shader/ShaderManager.h"

namespace mye {

void RegisterPartsShowcaseContent(EngineContext& ctx)
{
    RenderResources& res = *ctx.resources;
    const AssetID white = res.textures.White();
    const AssetID shader = AssetID{ HashStr("forward_lit") };
    res.meshes.Cube();

    auto makeMat = [&](const char* name, float r, float g, float b) {
        Material m;
        m.shader = shader;
        m.texture = white;
        m.baseColor = { r, g, b, 1.0f };
        return res.materials.Register(name, m);
    };
    makeMat("parts_held", 1.00f, 0.55f, 0.10f);  // 部位に付いた立方体 (追従が目で分かる色)
    makeMat("parts_floor", 0.55f, 0.55f, 0.60f);
    makeMat("parts_drop", 0.20f, 0.70f, 1.00f);
}

void BuildPartsShowcaseScene(EngineContext& ctx)
{
    Scene& s = *ctx.scene;
    RenderResources& res = *ctx.resources;

    RegisterPartsShowcaseContent(ctx); // 単独で呼ばれても実体が揃うようにしておく
    const AssetID cube = res.meshes.Cube();

    GameObject camera = s.CreateGameObject("Main Camera");
    camera.AddComponent<CameraComponent>();
    camera.SetLocalPosition(0.0f, 1.2f, -3.2f);

    GameObject sun = s.CreateGameObject("Sun");
    sun.AddComponent<LightComponent>();
    sun.SetLocalRotationEuler(50.0f, -30.0f, 0.0f);

    GameObject floor = s.CreateGameObject("Floor");
    floor.SetLocalPosition(0.0f, -0.25f, 0.0f);
    floor.SetLocalScale(12.0f, 0.5f, 12.0f);
    {
        auto* mr = floor.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr("parts_floor") };
        auto* col = floor.AddComponent<ColliderComponent>();
        col->shape = collidershape::kBox;
        col->halfExtents = { 0.5f, 0.5f, 0.5f }; // ローカル半径 (スケールは Collider 側で掛かる)
        col->isTrigger = false;
    }

    // ---- スキンメッシュ本体 ----
    GameObject actor = ModelLoader::Load(s, res, *ctx.shaders,
                                         ctx.assetsRoot + L"\\models\\CesiumMan.glb");
    if (!actor) {
        MYE_LOG_ERROR("[parts] CesiumMan.glb could not be loaded — showcase scene is incomplete");
        return;
    }
    actor.SetLocalPosition(0.0f, 0.0f, 0.0f);

    // SkinnedMesh を持つエンティティを DFS 最初の 1 個だけ拾う (CesiumMan は skin 1 本)
    World& w = s.GetWorld();
    w.ApplyStructuralChanges();
    EntityID skinned = kNullEntity;
    ForEachInSubtree(w, actor.Id(), [&](EntityID e, uint32_t) {
        if (w.GetComponent<SkinnedMeshComponent>(e)) {
            skinned = e;
            return WalkStep::Stop;
        }
        return WalkStep::Continue;
    });
    if (skinned.IsNull()) {
        MYE_LOG_ERROR("[parts] no SkinnedMesh in CesiumMan.glb — showcase scene is incomplete");
        return;
    }

    // ---- 部位: 右腕の先端ジョイントに追従させる ----
    // **source の直子に置く** (M48g の v1 規約)。ジョイント名は CesiumMan.glb のノード名
    GameObject hand = s.CreateGameObject("HandSocket");
    hand.SetParent(GameObject(&w, skinned));
    {
        auto* p = hand.AddComponent<PartComponent>();
        p->tag = HashStr("HandR");
        std::snprintf(p->joint, sizeof(p->joint), "%s", "Skeleton_arm_joint_R__3_");
    }
    // 部位の子: 追従が子孫まで伝わることを絵と TransformSystem の両方で被覆する
    GameObject held = s.CreateGameObject("HeldCube");
    held.SetParent(hand);
    held.SetLocalScale(0.12f, 0.12f, 0.12f);
    {
        auto* mr = held.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr("parts_held") };
    }

    // ---- 物理との順序被覆: 部位の近くに剛体を落とす ----
    // PartFollowSystem → PhysicsSystem → TransformSystem の順序が崩れると軌道が変わる
    GameObject drop = s.CreateGameObject("Drop");
    drop.SetLocalPosition(0.35f, 2.4f, 0.0f);
    drop.SetLocalScale(0.25f, 0.25f, 0.25f);
    {
        auto* mr = drop.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr("parts_drop") };
        auto* col = drop.AddComponent<ColliderComponent>();
        col->shape = collidershape::kBox;
        col->halfExtents = { 0.5f, 0.5f, 0.5f };
        col->isTrigger = false;
        drop.AddComponent<RigidbodyComponent>();
    }

    // ---- 範囲部位 (M49): ボーン無しの静的オブジェクトの「一部分」を部位にする ----
    // 的 (Target) の上半分だけを WeakPoint にする。PartBounds が demo シーンに載ることで
    // シリアライズ + WorldHash + (M49 の PartRaycastDemo で) RaycastParts がリプレイ被覆に入る
    GameObject target = s.CreateGameObject("Target");
    target.SetLocalPosition(-0.9f, 0.5f, 0.4f);
    {
        auto* mr = target.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr("parts_drop") };
    }
    GameObject weak = s.CreateGameObject("WeakPoint");
    weak.SetParent(target);
    {
        auto* p = weak.AddComponent<PartComponent>();
        p->tag = HashStr("WeakPoint"); // joint 空 = 静的ソケット
        auto* b = weak.AddComponent<PartBoundsComponent>();
        b->shape = partboundsshape::kBox;
        b->center = { 0.0f, 0.25f, 0.0f }; // 単位キューブの上半分だけが弱点
        b->halfExtents = { 0.5f, 0.25f, 0.5f };
    }

    // ---- 部位 API (v9、M48h) をリプレイ被覆に入れる ----
    // スクリプトが自分のサブツリーから "HandR" タグの部位を引いて飾りを取り付ける。
    // これで「C ABI の FindPartsByTag → SetParent」が Debug/Release のハッシュ照合を
    // 通ることまで機械検証される (DLL 未ロードならスキップ = 既定デモと同じ流儀)
    const ComponentTypeId attachDemo = ComponentRegistry::Get().FindByName("PartAttachDemo");
    if (attachDemo != kInvalidComponentType) {
        w.AddComponentRaw(actor.Id(), attachDemo);
    }
    // 範囲部位レイキャスト (v10、M49) の恒久 probe。Target に付け、毎 tick RaycastParts を
    // 実走して結果を sim 状態に書き戻す (詳細は PartRaycastDemo.cpp)
    const ComponentTypeId rayDemo = ComponentRegistry::Get().FindByName("PartRaycastDemo");
    if (rayDemo != kInvalidComponentType) {
        w.AddComponentRaw(target.Id(), rayDemo);
    }
    w.ApplyStructuralChanges();
}

void BuildAnimShowcaseScene(EngineContext& ctx)
{
    Scene& s = *ctx.scene;
    RenderResources& res = *ctx.resources;
    s.SetName("anim_showcase");

    // .meta の GUID (assets\anims\anim_test.controller.json.meta)
    constexpr uint64_t kAnimTestControllerGuid = 0xb44c659f992d4a83ull;

    Material floorMat;
    floorMat.shader = AssetID{ HashStr("forward_lit") };
    floorMat.texture = res.textures.White();
    floorMat.baseColor = { 0.30f, 0.32f, 0.36f, 1.0f };
    const AssetID floorMatId = res.materials.Register("animdemo_floor", floorMat);

    // モデルの正面は glTF の +Z = エンジンの -Z (ローダが Z を反転する)。カメラは -Z 側から正面を見る
    GameObject camera = s.CreateGameObject("Main Camera");
    camera.AddComponent<CameraComponent>();
    camera.SetLocalPosition(0.0f, 1.6f, -5.0f);
    camera.SetLocalRotationEuler(8.0f, 0.0f, 0.0f);

    GameObject sun = s.CreateGameObject("Sun");
    sun.AddComponent<LightComponent>();
    sun.SetLocalRotationEuler(50.0f, -30.0f, 0.0f);

    GameObject floor = s.CreateGameObject("Floor");
    floor.SetLocalPosition(0.0f, -0.05f, 0.0f);
    floor.SetLocalScale(10.0f, 0.1f, 10.0f);
    {
        auto* mr = floor.AddComponent<MeshRendererComponent>();
        mr->mesh = res.meshes.Cube();
        mr->material = floorMatId;
    }

    // 同じコントローラで Y-up と Z-up の 2 体を回す (骨クリップを名前で引くので、どちらのモデルでも同じ)
    struct ActorSpec {
        const wchar_t* file;
        float x;
    };
    const ActorSpec actors[] = {
        { L"\\models\\anim_test.glb", -1.0f },
        { L"\\models\\anim_test_zup.glb", 1.0f },
    };
    for (const ActorSpec& a : actors) {
        GameObject actor = ModelLoader::Load(s, res, *ctx.shaders, ctx.assetsRoot + a.file);
        if (!actor) {
            MYE_LOG_ERROR("[anim-demo] %s could not be loaded (run tools\\gen_anim_test_gltf.ps1)",
                          WideToUtf8(a.file).c_str());
            continue;
        }
        actor.SetLocalPosition(a.x, 0.0f, 0.0f);
        actor.AddComponent<AnimatorControllerComponent>()->controller = AssetID{ kAnimTestControllerGuid };
    }
}

} // namespace mye
