#include "Engine/Engine/DemoContent.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <functional>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include "Engine/Core/ComponentRegistry.h"
#include "Engine/Core/Components.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Engine/Animation.h"
#include "Engine/Engine/AnimatorController.h"
#include "Engine/Engine/Audio/AudioMixer.h"
#include "Engine/Engine/Audio/AudioSystem.h"
#include "Engine/Engine/Audio/SoundAsset.h"
#include "Engine/Engine/EngineLoop.h"
#include "Engine/Engine/FbxLoader.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/ModelLoader.h"
#include "Engine/Engine/Physics/PhysMatLibrary.h"
#include "Engine/Engine/Prefab.h"
#include "Engine/Engine/RagdollBuilder.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Engine/SceneSerializer.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/ShaderManager.h"

namespace mye {

void RegisterDemoContent(EngineContext& ctx)
{
    RenderResources& res = *ctx.resources;
    const AssetID shader = AssetID{ HashStr("forward_lit") };
    const AssetID white = res.textures.White();
    res.meshes.Cube();

    auto makeMat = [&](const char* name, float r, float g, float b) {
        Material m;
        m.shader = shader;
        m.texture = white;
        m.baseColor = { r, g, b, 1.0f };
        return res.materials.Register(name, m);
    };
    // 地面はファイルテクスチャ (assets\textures\test.png) — テクスチャホットリロードの実演用
    {
        Material m;
        m.shader = shader;
        const AssetID groundTex =
            res.textures.LoadFile(ctx.assetsRoot + L"\\textures\\test.png", true); // アルベド=sRGB (M38a)
        m.texture = groundTex.IsNull() ? white : groundTex;
        m.baseColor = groundTex.IsNull() ? DirectX::XMFLOAT4{ 0.45f, 0.47f, 0.50f, 1.0f }
                                         : DirectX::XMFLOAT4{ 1.0f, 1.0f, 1.0f, 1.0f };
        res.materials.Register("mat_ground", m);
    }
    makeMat("mat_arm", 0.85f, 0.55f, 0.20f);
    makeMat("mat_red", 0.85f, 0.30f, 0.28f);
    makeMat("mat_green", 0.35f, 0.75f, 0.40f);
    makeMat("mat_blue", 0.30f, 0.50f, 0.85f);
    makeMat("mat_yellow", 0.90f, 0.80f, 0.30f);
    // BoxTextured.glb の単発登録は M50a で削除 — RegisterAssetLibraries の起動走査が
    // 全モデルを RegisterAssets でヘッドレス登録するようになった (呼び出し側は常に対)
}

void BuildDemoScene(EngineContext& ctx, float perfRate, int perfMax)
{
    Scene& s = *ctx.scene;
    RenderResources& res = *ctx.resources;

    const AssetID cube = res.meshes.Cube();
    const AssetID matGround = AssetID{ HashStr("mat_ground") };
    const AssetID matArm = AssetID{ HashStr("mat_arm") };
    const AssetID palette[4] = {
        AssetID{ HashStr("mat_red") },
        AssetID{ HashStr("mat_green") },
        AssetID{ HashStr("mat_blue") },
        AssetID{ HashStr("mat_yellow") },
    };

    GameObject camera = s.CreateGameObject("Main Camera");
    camera.AddComponent<CameraComponent>();
    camera.SetLocalPosition(0.0f, 7.0f, -16.0f);
    camera.SetLocalRotationEuler(18.0f, 0.0f, 0.0f);

    GameObject sun = s.CreateGameObject("Sun");
    sun.AddComponent<LightComponent>();
    sun.SetLocalRotationEuler(50.0f, -30.0f, 0.0f);

    GameObject ground = s.CreateGameObject("Ground");
    ground.SetLocalPosition(0.0f, -1.0f, 0.0f);
    ground.SetLocalScale(40.0f, 0.5f, 40.0f);
    {
        auto* mr = ground.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = matGround;
    }

    GameObject spinner = s.CreateGameObject("Spinner");
    spinner.SetLocalPosition(0.0f, 1.5f, 0.0f);
    for (int a = 0; a < 20; ++a) {
        char name[32];
        snprintf(name, sizeof(name), "Arm_%02d", a);
        GameObject arm = s.CreateGameObject(name);
        arm.SetParent(spinner);
        arm.SetLocalRotationEuler(0.0f, a * (360.0f / 20.0f), 0.0f);
        arm.SetLocalScale(0.4f, 0.4f, 0.4f);
        {
            auto* mr = arm.AddComponent<MeshRendererComponent>();
            mr->mesh = cube;
            mr->material = matArm;
        }
        for (int j = 0; j < 25; ++j) {
            snprintf(name, sizeof(name), "Cube_%02d_%02d", a, j);
            GameObject leaf = s.CreateGameObject(name);
            leaf.SetParent(arm);
            const float dist = (2.0f + 0.9f * j) / 0.4f;
            const float wave = 1.0f + 0.8f * ((j % 5) - 2) * 0.3f;
            leaf.SetLocalPosition(dist, wave, 0.0f);
            leaf.SetLocalScale(0.75f, 0.75f, 0.75f);
            auto* mr = leaf.AddComponent<MeshRendererComponent>();
            mr->mesh = cube;
            mr->material = palette[(a + j) % 4];
        }
    }

    GameObject model = ModelLoader::Load(s, res, *ctx.shaders,
                                         ctx.assetsRoot + L"\\models\\BoxTextured.glb");
    if (model) {
        model.SetLocalPosition(0.0f, 1.5f, 0.0f);
        model.SetLocalScale(2.0f, 2.0f, 2.0f);
    }


    // ---- パーティクルエミッタ (M5 デモ: 中央の炎) ----
    GameObject fire = s.CreateGameObject("Fire");
    fire.SetLocalPosition(4.0f, 0.0f, 0.0f);
    auto* emitter = fire.AddComponent<ParticleEmitterComponent>(); // 既定値 = 上向きコーン炎
    if (perfRate > 0.0f) {
        emitter->rate = perfRate;
        emitter->maxParticles = (perfMax > 0) ? perfMax : 100000;
        emitter->lifetimeMin = 1.2f;
        emitter->lifetimeMax = 1.8f;
        emitter->speedMin = 3.0f;
        emitter->speedMax = 8.0f;
        emitter->coneAngleDeg = 60.0f;
    }

    // ---- GameLogic.dll のスクリプトをアタッチ (DLL 未ロードならスキップ) ----
    const ComponentTypeId rotator = ComponentRegistry::Get().FindByName("Rotator");
    if (rotator != kInvalidComponentType) {
        s.GetWorld().AddComponentRaw(spinner.Id(), rotator);
    }
    const ComponentTypeId player = ComponentRegistry::Get().FindByName("PlayerController");
    if (player != kInvalidComponentType && model) {
        s.GetWorld().AddComponentRaw(model.Id(), player);
        auto* col = model.AddComponent<ColliderComponent>();
        col->shape = 0;
        col->radius = 1.2f;
        // M7 以来の OnTrigger 用検知球。既定が bool 化でソリッドに変わったため明示
        col->isTrigger = true;
    }
    const ComponentTypeId spawner = ComponentRegistry::Get().FindByName("Spawner");
    if (spawner != kInvalidComponentType) {
        s.GetWorld().AddComponentRaw(fire.Id(), spawner);
    }

    // ---- 汎用フィールド ABI (v11、M50d) をリプレイ被覆に入れる ----
    // Health (assets\schemas 由来のスキーマ型) を Spinner に載せ、probe スクリプトが
    // 毎 tick GetComponentField → 減衰 → SetComponentField で書き戻す
    // (詳細は SchemaHealthDemo.cpp)。スキーマ未登録 (ヘッドレス selftest) や DLL 未ロード
    // では対ごとスキップ — Health 単独でも probe 単独でも被覆にならないため 2 つで 1 対
    const ComponentTypeId health = ComponentRegistry::Get().FindByName("Health");
    const ComponentTypeId healthDemo = ComponentRegistry::Get().FindByName("SchemaHealthDemo");
    if (health != kInvalidComponentType && healthDemo != kInvalidComponentType) {
        s.GetWorld().AddComponentRaw(spinner.Id(), health);
        s.GetWorld().AddComponentRaw(spinner.Id(), healthDemo);
    }
}

void RegisterRtShowcaseContent(EngineContext& ctx)
{
    RenderResources& res = *ctx.resources;
    const AssetID white = res.textures.White();
    const AssetID shader = AssetID{ HashStr("forward_lit") };
    res.meshes.Cube();
    res.meshes.Sphere();

    // マテリアルはこのシーン専用 (既定デモの mat_* とは名前を分ける)。
    // 二次ヒットのシェーディングはマテリアル定数のみ = テクスチャ非対応 (M46 v1 制限) なので、
    // ショーケースは意図的に全て無地にしてある — ラスタとレイトレで同じ色が出る
    auto makeMat = [&](const char* name, float r, float g, float b, float metallic,
                       float roughness, float emissive) {
        Material m;
        m.shader = shader;
        m.texture = white;
        m.baseColor = { r, g, b, 1.0f };
        m.metallic = metallic;
        m.roughness = roughness;
        m.emissiveIntensity = emissive;
        return res.materials.Register(name, m);
    };
    makeMat("rt_white", 0.80f, 0.79f, 0.76f, 0.0f, 0.85f, 0.0f);
    makeMat("rt_red", 0.75f, 0.15f, 0.12f, 0.0f, 0.85f, 0.0f);
    makeMat("rt_green", 0.18f, 0.65f, 0.20f, 0.0f, 0.85f, 0.0f);
    // 発光パネル。ラスタでは gbMaterial.b 経由でこの面自体が明るく描かれ、
    // レイトレでは RtMaterial.emissive としてバウンス先の光源になる (同じ強度から両方が出る)
    makeMat("rt_lamp", 1.00f, 0.95f, 0.85f, 0.0f, 1.0f, 6.0f);
    makeMat("rt_mirror", 0.95f, 0.93f, 0.88f, 1.0f, 0.05f, 0.0f);
    // 粗さは 0.18 にしてある: roughness 0.3〜0.6 の帯は 1spp の GGX ローブが
    // 小さな発光パネルを引く分散が大きく、v1 のデノイザではファイアフライが残るため (M46h v1 制限)
    makeMat("rt_brushed", 0.72f, 0.45f, 0.20f, 1.0f, 0.18f, 0.0f);
}

void BuildRtShowcaseScene(EngineContext& ctx)
{
    Scene& s = *ctx.scene;
    RenderResources& res = *ctx.resources;

    RegisterRtShowcaseContent(ctx); // 単独で呼ばれても実体が揃うようにしておく
    const AssetID cube = res.meshes.Cube();
    const AssetID sphere = res.meshes.Sphere();
    const AssetID matWhite = AssetID{ HashStr("rt_white") };
    const AssetID matRed = AssetID{ HashStr("rt_red") };
    const AssetID matGreen = AssetID{ HashStr("rt_green") };
    const AssetID matLamp = AssetID{ HashStr("rt_lamp") };
    const AssetID matMirror = AssetID{ HashStr("rt_mirror") };
    const AssetID matBrushed = AssetID{ HashStr("rt_brushed") };

    auto box = [&](const char* name, float px, float py, float pz, float sx, float sy, float sz,
                   AssetID mat, float yawDeg = 0.0f) {
        GameObject go = s.CreateGameObject(name);
        go.SetLocalPosition(px, py, pz);
        go.SetLocalScale(sx, sy, sz);
        if (yawDeg != 0.0f) {
            go.SetLocalRotationEuler(0.0f, yawDeg, 0.0f);
        }
        auto* mr = go.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = mat;
        return go;
    };
    auto ball = [&](const char* name, float px, float py, float pz, float diameter, AssetID mat) {
        GameObject go = s.CreateGameObject(name);
        go.SetLocalPosition(px, py, pz);
        go.SetLocalScale(diameter, diameter, diameter); // Sphere() は半径 0.5 = 直径 1
        auto* mr = go.AddComponent<MeshRendererComponent>();
        mr->mesh = sphere;
        mr->material = mat;
        return go;
    };

    // カメラは箱の内側に置く (前面の壁はカメラの背後なので視界を塞がない)。
    // 箱を閉じておくことで、拡散 GI のバウンスが外へ逃げずコーネル箱らしい色移りが出る
    GameObject camera = s.CreateGameObject("Main Camera");
    {
        auto* cam = camera.AddComponent<CameraComponent>();
        cam->fovYDeg = 55.0f; // 既定 60 はやや広角。天井の発光パネルが画に入る下限
    }
    camera.SetLocalPosition(0.0f, 4.2f, -4.3f);
    camera.SetLocalRotationEuler(4.0f, 0.0f, 0.0f); // わずかに見下ろして床を画に入れる

    // ---- 箱 (内寸 10 x 8 x 14、壁厚 0.4)。奥行きを取ってあるのは、
    //      被写体をカメラから離して画角に収めるため ----
    box("Floor", 0.0f, -0.2f, 2.0f, 10.8f, 0.4f, 14.4f, matWhite);
    box("Ceiling", 0.0f, 8.2f, 2.0f, 10.8f, 0.4f, 14.4f, matWhite);
    box("WallBack", 0.0f, 4.0f, 9.2f, 10.8f, 8.8f, 0.4f, matWhite);
    box("WallFront", 0.0f, 4.0f, -5.2f, 10.8f, 8.8f, 0.4f, matWhite); // カメラの背後
    box("WallLeft", -5.2f, 4.0f, 2.0f, 0.4f, 8.8f, 14.4f, matRed);
    box("WallRight", 5.2f, 4.0f, 2.0f, 0.4f, 8.8f, 14.4f, matGreen);

    // ---- 天井の面光源 (このシーン唯一の光源) ----
    box("LightPanel", 0.0f, 7.85f, 2.5f, 4.5f, 0.15f, 5.0f, matLamp);

    // ---- 被写体 ----
    ball("MirrorSphere", -2.1f, 1.5f, 4.2f, 3.0f, matMirror);  // 鏡面 = 箱が映り込む
    ball("BrushedSphere", 2.8f, 1.1f, 1.8f, 2.2f, matBrushed); // 金属 = 引き締まった映り込み
    box("TallBox", 2.2f, 2.2f, 6.2f, 2.2f, 4.4f, 2.2f, matWhite, 20.0f);
    box("ShortBox", -3.0f, 0.9f, 1.4f, 1.8f, 1.8f, 1.8f, matWhite, -18.0f);

    // 2 個目の発光体は **ファイルマテリアル** (.mat.json の "emissive" フィールド) から引く。
    // これで「JSON → MaterialLibrary → ラスタ + レイトレ」の経路がショーケースを起動するだけで
    // 通る (エディタで .mat.json を書き換えればホットリロードで光り方が変わるのも確認できる)。
    // ファイルが無ければ静かに省略する — このシーンの成立に必須ではない
    {
        const AssetID matFile = MaterialLibrary::HashForPath(
            ctx.assetsRoot + L"\\materials\\demo_emissive.mat.json");
        if (res.materials.Get(matFile) != nullptr) {
            box("EmissiveBar", -4.6f, 3.4f, 6.4f, 0.5f, 2.6f, 0.5f, matFile);
        }
    }
}

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
        col->shape = 1; // box
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
    {
        std::function<void(EntityID)> visit = [&](EntityID e) {
            if (!skinned.IsNull()) {
                return;
            }
            if (w.GetComponent<SkinnedMeshComponent>(e)) {
                skinned = e;
                return;
            }
            auto* h = w.GetComponent<HierarchyComponent>(e);
            for (EntityID c = h ? h->firstChild : kNullEntity; !c.IsNull();) {
                auto* ch = w.GetComponent<HierarchyComponent>(c);
                const EntityID next = ch ? ch->nextSibling : kNullEntity;
                visit(c);
                c = next;
            }
        };
        visit(actor.Id());
    }
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
        col->shape = 1;
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
        b->shape = 0;                      // 箱
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

void RegisterFlowShowcaseContent(EngineContext& ctx)
{
    RenderResources& res = *ctx.resources;
    const AssetID white = res.textures.White();
    const AssetID shader = AssetID{ HashStr("forward_lit") };
    res.meshes.Cube();
    res.meshes.Sphere();

    auto makeMat = [&](const char* name, float r, float g, float b) {
        Material m;
        m.shader = shader;
        m.texture = white;
        m.baseColor = { r, g, b, 1.0f };
        return res.materials.Register(name, m);
    };
    makeMat("flow_floor", 0.30f, 0.33f, 0.40f);
    makeMat("flow_wall", 0.20f, 0.22f, 0.28f);
    makeMat("flow_ball", 1.00f, 0.55f, 0.10f);
    makeMat("flow_paddle", 0.25f, 0.70f, 0.95f);
    makeMat("flow_deco", 0.60f, 0.35f, 0.85f);
}

namespace {

// flow シーン共通の骨格 (カメラ/太陽/床)。名前キーのマテリアルと builtin メッシュのみ =
// シーンファイルがチェックアウト非依存になる (モデル由来のサブアセット ID は使わない)
void BuildFlowStage(Scene& s, RenderResources& res, bool withColliders)
{
    const AssetID cube = res.meshes.Cube();

    GameObject camera = s.CreateGameObject("Main Camera");
    camera.AddComponent<CameraComponent>();
    camera.SetLocalPosition(0.0f, 6.0f, -14.0f);
    camera.SetLocalRotationEuler(20.0f, 0.0f, 0.0f);

    GameObject sun = s.CreateGameObject("Sun");
    sun.AddComponent<LightComponent>();
    sun.SetLocalRotationEuler(50.0f, -30.0f, 0.0f);

    GameObject floor = s.CreateGameObject("Floor");
    floor.SetLocalPosition(0.0f, -0.25f, 0.0f);
    floor.SetLocalScale(16.0f, 0.5f, 16.0f);
    {
        auto* mr = floor.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr("flow_floor") };
        if (withColliders) {
            auto* col = floor.AddComponent<ColliderComponent>();
            col->shape = 1; // box
            col->halfExtents = { 0.5f, 0.5f, 0.5f }; // ローカル半径 (スケールは Collider 側で掛かる)
            col->isTrigger = false;
        }
    }
}

// UI テキスト要素 (kind=1)。矩形左上がアンカー点 + オフセットに置かれる (M51e 意味論)
GameObject MakeUiText(Scene& s, const char* name, int anchor, float x, float y, float w, float h,
                      const char* text, float fontScale, int align)
{
    GameObject go = s.CreateGameObject(name);
    auto* ui = go.AddComponent<UIElementComponent>();
    ui->kind = 1;
    ui->anchor = anchor;
    ui->x = x;
    ui->y = y;
    ui->w = w;
    ui->h = h;
    ui->fontScale = fontScale;
    ui->align = align;
    std::snprintf(ui->text, sizeof(ui->text), "%s", text);
    return go;
}

void AttachScriptIfRegistered(World& w, EntityID e, const char* name)
{
    const ComponentTypeId t = ComponentRegistry::Get().FindByName(name);
    if (t != kInvalidComponentType) {
        w.AddComponentRaw(e, t);
    }
}

// タイトル/リザルト画面: 装飾 + UI + FlowTitleDriver (C++) + FlowMenu (C#、演出レーン)
void BuildFlowTitleScene(EngineContext& ctx)
{
    Scene& s = *ctx.scene;
    RenderResources& res = *ctx.resources;
    s.SetName("flow_title");
    BuildFlowStage(s, res, /*withColliders=*/false);
    const AssetID cube = res.meshes.Cube();

    // 回転する装飾キューブ (Rotator があれば回る — 無くてもシーンは成立する)
    for (int i = 0; i < 3; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "Deco_%d", i);
        GameObject deco = s.CreateGameObject(name);
        deco.SetLocalPosition(-3.0f + 3.0f * i, 1.2f, 2.0f);
        deco.SetLocalRotationEuler(0.0f, 20.0f * i, 0.0f);
        deco.SetLocalScale(1.4f, 1.4f, 1.4f);
        auto* mr = deco.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr("flow_deco") };
        AttachScriptIfRegistered(s.GetWorld(), deco.Id(), "Rotator");
    }

    MakeUiText(s, "TitleText", 1, -400.0f, 120.0f, 800.0f, 80.0f, "MyEngine FLOW DEMO", 3.0f, 4);
    GameObject hint = MakeUiText(s, "TitleHint", 4, -300.0f, 120.0f, 600.0f, 40.0f,
                                 "Space / Pad A : START   (auto start in 90 ticks)", 1.2f, 4);
    AttachScriptIfRegistered(s.GetWorld(), hint.Id(), "FlowMenu"); // C# 点滅 (別レーン)
    MakeUiText(s, "TitleBest", 7, -300.0f, -140.0f, 600.0f, 40.0f, "BEST 0   LAST 0   RUNS 0",
               1.4f, 4);

    GameObject director = s.CreateGameObject("FlowDirector");
    AttachScriptIfRegistered(s.GetWorld(), director.Id(), "FlowTitleDriver");
    s.GetWorld().ApplyStructuralChanges();
}

// ゲーム画面: 弾む球 (Rigidbody + FlowGameDriver) + パドル + HUD
void BuildFlowGameScene(EngineContext& ctx)
{
    Scene& s = *ctx.scene;
    RenderResources& res = *ctx.resources;
    s.SetName("flow_game");
    BuildFlowStage(s, res, /*withColliders=*/true);
    const AssetID cube = res.meshes.Cube();
    const AssetID sphere = res.meshes.Sphere();

    // 球を囲う壁 (静的コライダー)。転がり出てもデモが破綻しないように
    auto wall = [&](const char* name, float px, float pz, float sx, float sz) {
        GameObject go = s.CreateGameObject(name);
        go.SetLocalPosition(px, 1.0f, pz);
        go.SetLocalScale(sx, 2.0f, sz);
        auto* mr = go.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr("flow_wall") };
        auto* col = go.AddComponent<ColliderComponent>();
        col->shape = 1;
        col->halfExtents = { 0.5f, 0.5f, 0.5f };
        col->isTrigger = false;
    };
    wall("WallN", 0.0f, 8.25f, 16.0f, 0.5f);
    wall("WallS", 0.0f, -8.25f, 16.0f, 0.5f);
    wall("WallE", 8.25f, 0.0f, 0.5f, 16.0f);
    wall("WallW", -8.25f, 0.0f, 0.5f, 16.0f);

    // パドル (MoveX 軸で動かせる静的コライダー。リプレイでは軸 0 = 不動)
    GameObject paddle = s.CreateGameObject("Player");
    paddle.SetLocalPosition(0.0f, 0.45f, 0.0f);
    paddle.SetLocalScale(2.4f, 0.4f, 2.4f);
    {
        auto* mr = paddle.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr("flow_paddle") };
        auto* col = paddle.AddComponent<ColliderComponent>();
        col->shape = 1;
        col->halfExtents = { 0.5f, 0.5f, 0.5f };
        col->isTrigger = false;
    }

    // 弾む球 = このシーンの sim 本体。FlowGameDriver がフロー全体を駆動する
    GameObject ball = s.CreateGameObject("Ball");
    ball.SetLocalPosition(1.3f, 5.0f, 0.7f);
    {
        auto* mr = ball.AddComponent<MeshRendererComponent>();
        mr->mesh = sphere;
        mr->material = AssetID{ HashStr("flow_ball") };
        auto* col = ball.AddComponent<ColliderComponent>();
        col->shape = 0; // sphere
        col->radius = 0.5f;
        col->isTrigger = false;
        auto* rb = ball.AddComponent<RigidbodyComponent>();
        rb->restitution = 0.82f;
    }
    AttachScriptIfRegistered(s.GetWorld(), ball.Id(), "FlowGameDriver");

    MakeUiText(s, "GameScore", 0, 40.0f, 30.0f, 400.0f, 50.0f, "SCORE 0", 2.0f, 0);
    {
        GameObject bar = s.CreateGameObject("GameScoreBar");
        auto* ui = bar.AddComponent<UIElementComponent>();
        ui->kind = 0;
        ui->anchor = 0;
        ui->x = 40.0f;
        ui->y = 95.0f;
        ui->w = 400.0f;
        ui->h = 22.0f;
        ui->color = { 0.30f, 0.85f, 0.45f, 0.9f };
        ui->fillMode = 1; // 水平バー (FlowGameDriver が SetUIFill で書く)
        ui->fillAmount = 0.0f;
    }
    // PAUSED 表示はアルファ 0 で常設し、ドライバが SetUIColor で出し入れする
    GameObject paused = MakeUiText(s, "GamePause", 4, -300.0f, -40.0f, 600.0f, 80.0f, "PAUSED",
                                   4.0f, 4);
    if (auto* ui = paused.GetComponent<UIElementComponent>()) {
        ui->color = { 1.0f, 0.85f, 0.30f, 0.0f };
    }
    MakeUiText(s, "GameHint", 6, 40.0f, -60.0f, 900.0f, 40.0f,
               "A/D : MOVE   Esc/Start : PAUSE   J : LOAD SAVE", 1.2f, 0);
    s.GetWorld().ApplyStructuralChanges();
}

} // namespace

void EnsureFlowShowcaseScenes(EngineContext& ctx)
{
    RegisterFlowShowcaseContent(ctx);
    const std::wstring dir = ctx.assetsRoot + L"\\scenes";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    // ctx.scene に組んで保存 → Clear、を欠けているファイルの分だけ繰り返す。
    // 呼び出し時点の ctx.scene は空 (OnStart のシーンロード前) が前提
    auto ensure = [&](const wchar_t* file, void (*build)(EngineContext&)) {
        const std::wstring path = dir + L"\\" + file;
        if (std::filesystem::exists(path, ec)) {
            return;
        }
        build(ctx);
        if (SceneSerializer::SaveToFile(*ctx.scene, path)) {
            MYE_LOG_INFO("[flow] scene generated: %s", WideToUtf8(path).c_str());
        } else {
            MYE_LOG_ERROR("[flow] scene write failed: %s", WideToUtf8(path).c_str());
        }
        ctx.scene->Clear();
        ctx.scene->GetWorld().ApplyStructuralChanges();
    };
    ensure(L"flow_game.scene.json", &BuildFlowGameScene);
    ensure(L"flow_title.scene.json", &BuildFlowTitleScene);
}

void BuildNetDuelScene(EngineContext& ctx)
{
    Scene& s = *ctx.scene;
    RenderResources& res = *ctx.resources;
    s.SetName("net_duel");
    const AssetID cube = res.meshes.Cube();

    GameObject camera = s.CreateGameObject("Main Camera");
    camera.AddComponent<CameraComponent>();
    camera.SetLocalPosition(0.0f, 9.0f, -12.0f);
    camera.SetLocalRotationEuler(34.0f, 0.0f, 0.0f);

    GameObject sun = s.CreateGameObject("Sun");
    sun.AddComponent<LightComponent>();
    sun.SetLocalRotationEuler(52.0f, -28.0f, 0.0f);

    GameObject floor = s.CreateGameObject("Floor");
    floor.SetLocalPosition(0.0f, -0.25f, 0.0f);
    floor.SetLocalScale(20.0f, 0.5f, 14.0f);
    {
        auto* mr = floor.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr("duel_floor") };
    }
    // 得点リング。**当たり判定は持たない** — 判定はスクリプト側の距離計算だけで完結させる
    // (物理を挟むとデモの読み筋が「ネットの話」から離れる)。ここは目印
    GameObject ring = s.CreateGameObject("ScoreRing");
    ring.SetLocalPosition(0.0f, 0.02f, 0.0f);
    ring.SetLocalScale(6.4f, 0.04f, 6.4f);
    {
        auto* mr = ring.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr("duel_ring") };
    }

    static const char* kMats[] = { "duel_p0", "duel_p1" };
    for (uint32_t i = 0; i < 2; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "Duelist_%u", i + 1);
        GameObject go = s.CreateGameObject(name);
        go.SetLocalPosition(i == 0 ? -6.0f : 6.0f, 0.5f, 0.0f);
        auto* mr = go.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr(kMats[i]) };
        // レーンの結び付けは PlayerInput のミラー (M52g) をそのまま使う。
        // スクリプトは playerIndex を読んで v13 の GetAxisForPlayer を引く
        auto* pi = go.AddComponent<PlayerInputComponent>();
        pi->playerIndex = static_cast<int32_t>(i);
        AttachScriptIfRegistered(s.GetWorld(), go.Id(), "NetDuelDemo");
    }

    // HUD。**ここだけがネット状態 (機種依存の値) を書いてよい場所**
    GameObject hud = MakeUiText(s, "NetHud", 0, 16.0f, 16.0f, 640.0f, 28.0f, "net", 2.0f, 0);
    AttachScriptIfRegistered(s.GetWorld(), hud.Id(), "NetHudDemo");
}

void RegisterNetDuelContent(EngineContext& ctx)
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
    makeMat("duel_floor", 0.24f, 0.26f, 0.30f);
    makeMat("duel_ring", 0.55f, 0.50f, 0.20f);
    makeMat("duel_p0", 0.95f, 0.35f, 0.25f); // P1 = 赤
    makeMat("duel_p1", 0.25f, 0.65f, 0.95f); // P2 = 青
}

void RegisterLocalPlayersContent(EngineContext& ctx)
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
    makeMat("mp_floor", 0.26f, 0.28f, 0.34f);
    makeMat("mp_p0", 0.95f, 0.35f, 0.25f); // P1 = 赤
    makeMat("mp_p1", 0.25f, 0.65f, 0.95f); // P2 = 青
    makeMat("mp_p2", 0.35f, 0.85f, 0.40f); // P3 = 緑
    makeMat("mp_p3", 0.90f, 0.80f, 0.25f); // P4 = 黄
}

void BuildLocalPlayersScene(EngineContext& ctx)
{
    Scene& s = *ctx.scene;
    RenderResources& res = *ctx.resources;
    s.SetName("local_players");
    const AssetID cube = res.meshes.Cube();

    GameObject camera = s.CreateGameObject("Main Camera");
    camera.AddComponent<CameraComponent>();
    camera.SetLocalPosition(0.0f, 7.0f, -13.0f);
    camera.SetLocalRotationEuler(24.0f, 0.0f, 0.0f);

    GameObject sun = s.CreateGameObject("Sun");
    sun.AddComponent<LightComponent>();
    sun.SetLocalRotationEuler(52.0f, -28.0f, 0.0f);

    GameObject floor = s.CreateGameObject("Floor");
    floor.SetLocalPosition(0.0f, -0.25f, 0.0f);
    floor.SetLocalScale(18.0f, 0.5f, 12.0f);
    {
        auto* mr = floor.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr("mp_floor") };
    }

    // ★**kMaxPlayers 体すべて置く**。--local-players 2 で走らせると 3 体目以降の
    //   PlayerInput ミラーは全ゼロのまま = 「playerCount を超えたレーンは恒常ゼロ」が
    //   ワールドハッシュの上で固定される (動かない 2 体も立派な検査対象)
    static const char* kMats[] = { "mp_p0", "mp_p1", "mp_p2", "mp_p3" };
    for (uint32_t i = 0; i < kMaxPlayers; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "Player_%u", i + 1);
        GameObject go = s.CreateGameObject(name);
        go.SetLocalPosition(-6.0f + 4.0f * static_cast<float>(i), 0.5f, 0.0f);
        auto* mr = go.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr(kMats[i]) };
        auto* pi = go.AddComponent<PlayerInputComponent>();
        pi->playerIndex = static_cast<int32_t>(i);
        // 動きは C++ スクリプト側。**エンジンの ABI は 1 本も足していない** —
        // ミラーは v11 の GetComponentField (名前ハッシュの汎用スロット) で読む
        AttachScriptIfRegistered(s.GetWorld(), go.Id(), "LocalPlayerDemo");
    }
}

void RegisterPhysicsShowcaseContent(EngineContext& ctx)
{
    RenderResources& res = *ctx.resources;
    const AssetID white = res.textures.White();
    const AssetID shader = AssetID{ HashStr("forward_lit") };
    res.meshes.Cube();
    res.meshes.Sphere();

    auto makeMat = [&](const char* name, float r, float g, float b) {
        Material m;
        m.shader = shader;
        m.texture = white;
        m.baseColor = { r, g, b, 1.0f };
        return res.materials.Register(name, m);
    };
    makeMat("pdemo_floor", 0.30f, 0.32f, 0.36f);
    makeMat("pdemo_feather", 0.92f, 0.90f, 0.80f);
    makeMat("pdemo_steel", 0.62f, 0.64f, 0.68f);
    makeMat("pdemo_plane", 0.90f, 0.88f, 0.72f);
    makeMat("pdemo_ball", 0.90f, 0.35f, 0.30f);
    makeMat("pdemo_buoy", 0.95f, 0.72f, 0.20f);
    makeMat("pdemo_rubber", 0.25f, 0.75f, 0.45f);
}

// 名前で .physmat を引く (プリセットは起動走査で登録済み)。
// **絶対パスのハッシュを直接組まない** — チェックアウト先で値が変わるうえ、
// 同伴 .meta の GUID が優先されるので一致しない (M59a1 の申し送り 1)
AssetID FindPhysMat(const char* name)
{
    PhysMatLibrary* lib = physmat::Library();
    if (!lib) {
        return AssetID{};
    }
    for (const PhysMatEntry& e : lib->Enumerate()) { // 名前昇順 = 決定論
        if (e.name == name) {
            return AssetID{ e.hash };
        }
    }
    return AssetID{}; // 未登録なら未割当 = 既存フィールドで動く (デモは壊れない)
}

void BuildPhysicsShowcaseScene(EngineContext& ctx)
{
    Scene& s = *ctx.scene;
    RenderResources& res = *ctx.resources;
    s.SetName("physics_showcase");
    const AssetID cube = res.meshes.Cube();
    const AssetID sphere = res.meshes.Sphere();
    const AssetID matSteel = FindPhysMat("steel");
    const AssetID matRubber = FindPhysMat("rubber");

    GameObject camera = s.CreateGameObject("Main Camera");
    camera.AddComponent<CameraComponent>();
    camera.SetLocalPosition(0.0f, 8.0f, -22.0f);
    camera.SetLocalRotationEuler(16.0f, 0.0f, 0.0f);

    GameObject sun = s.CreateGameObject("Sun");
    sun.AddComponent<LightComponent>();
    sun.SetLocalRotationEuler(50.0f, -30.0f, 0.0f);

    // ---- 物理環境 (シーンに 1 個。entity.index 最小の active なものが使われる) ----
    // 風を入れておくと羽根が流れる = 風の経路もリプレイ被覆に載る
    {
        GameObject envGo = s.CreateGameObject("Environment");
        auto* env = envGo.AddComponent<PhysicsEnvironmentComponent>();
        env->gravity = { 0.0f, -9.81f, 0.0f };
        env->airDensity = 1.225f;
        env->windVelocity = { 1.5f, 0.0f, 0.0f };
        env->waterPlaneY = 0.0f;
        env->waterDensity = 1000.0f;
    }

    // ---- 床 (静的コライダー + メッシュ)。**x = 4 で切ってある** — その先が水面 ----
    {
        GameObject floor = s.CreateGameObject("Floor");
        floor.SetLocalPosition(-4.0f, -0.5f, 0.0f);
        floor.SetLocalScale(16.0f, 1.0f, 14.0f);
        auto* mr = floor.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr("pdemo_floor") };
        auto* col = floor.AddComponent<ColliderComponent>();
        col->shape = 1;
        col->isTrigger = false;
        col->halfExtents = { 0.5f, 0.5f, 0.5f }; // ワールドスケールが効く
        col->physMaterial = matSteel;            // 鋼の床 (e=0.6 を主張できるのが M59a2 の新能力)
    }

    // ---- 1. 羽根と鉄球: 空気があると同時に落ちない ----
    {
        GameObject feather = s.CreateGameObject("Feather");
        feather.SetLocalPosition(-10.0f, 9.0f, -3.0f);
        feather.SetLocalScale(0.9f, 0.03f, 0.9f);
        auto* mr = feather.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr("pdemo_feather") };
        auto* col = feather.AddComponent<ColliderComponent>();
        col->shape = 1;
        col->halfExtents = { 0.5f, 0.5f, 0.5f };
        auto* rb = feather.AddComponent<RigidbodyComponent>();
        rb->mass = 0.03f;
        rb->angularDamping = 0.0f; // 空力に任せる
        auto* aero = feather.AddComponent<AeroComponent>();
        aero->surfaceModel = true; // 向きを見る = ひらひら落ちる
    }
    {
        GameObject ball = s.CreateGameObject("SteelBall");
        ball.SetLocalPosition(-7.5f, 9.0f, -3.0f);
        ball.SetLocalScale(0.5f, 0.5f, 0.5f);
        auto* mr = ball.AddComponent<MeshRendererComponent>();
        mr->mesh = sphere;
        mr->material = AssetID{ HashStr("pdemo_steel") };
        auto* col = ball.AddComponent<ColliderComponent>();
        col->shape = 0;
        col->radius = 0.5f;
        col->physMaterial = matSteel;
        auto* rb = ball.AddComponent<RigidbodyComponent>();
        rb->useDensity = true; // 質量 = 鋼の密度 x 体積 (約 32kg)
        auto* aero = ball.AddComponent<AeroComponent>();
        aero->enableMagnus = false; // 抗力だけ (重いのでほとんど効かない = 対比になる)
    }

    // ---- 2. 紙飛行機: 主翼 + 尾翼を**子エンティティ**に置いて風見安定を作る ----
    {
        GameObject plane = s.CreateGameObject("PaperPlane");
        plane.SetLocalPosition(-3.0f, 6.0f, -9.0f);
        plane.SetLocalScale(0.35f, 0.08f, 1.2f);
        auto* mr = plane.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr("pdemo_plane") };
        auto* col = plane.AddComponent<ColliderComponent>();
        col->shape = 1;
        col->halfExtents = { 0.5f, 0.5f, 0.5f };
        auto* rb = plane.AddComponent<RigidbodyComponent>();
        rb->mass = 0.08f;
        rb->angularDamping = 0.02f;
        rb->velocity = { 0.0f, 0.0f, 11.0f }; // +Z へ射出
        // 主翼 (重心のわずかに前)
        GameObject wing = s.CreateGameObject("Wing");
        wing.SetParent(plane);
        wing.SetLocalPosition(0.0f, 0.0f, 0.15f);
        auto* ws = wing.AddComponent<AeroSurfaceComponent>();
        ws->normal = { 0.0f, 1.0f, 0.0f };
        ws->area = 0.30f;
        ws->stallAngleDeg = 14.0f;
        // 尾翼 (重心の後ろ = 復元モーメントの源)
        GameObject tail = s.CreateGameObject("Tail");
        tail.SetParent(plane);
        tail.SetLocalPosition(0.0f, 0.0f, -0.9f);
        auto* ts = tail.AddComponent<AeroSurfaceComponent>();
        ts->normal = { 0.0f, 1.0f, 0.0f };
        ts->area = 0.10f;
        ts->stallAngleDeg = 14.0f;
    }

    // ---- 3. カーブボール: 回転する球がマグヌスで曲がる ----
    {
        GameObject ball = s.CreateGameObject("CurveBall");
        ball.SetLocalPosition(2.0f, 5.0f, -9.0f);
        ball.SetLocalScale(0.4f, 0.4f, 0.4f);
        auto* mr = ball.AddComponent<MeshRendererComponent>();
        mr->mesh = sphere;
        mr->material = AssetID{ HashStr("pdemo_ball") };
        auto* col = ball.AddComponent<ColliderComponent>();
        col->shape = 0;
        col->radius = 0.5f;
        col->physMaterial = matRubber;
        auto* rb = ball.AddComponent<RigidbodyComponent>();
        rb->mass = 0.15f;
        rb->angularDamping = 0.0f;
        rb->velocity = { 0.0f, 1.0f, 16.0f };
        rb->angularVelocity = { 0.0f, 40.0f, 0.0f }; // +Y 軸回転 → -X 側へ曲がる
        auto* aero = ball.AddComponent<AeroComponent>();
        aero->enableMagnus = true;
        aero->magnusCoefficient = 3.0f; // 見て分かる曲がりに誇張
    }

    // ---- 4. 浮き: 床が切れた先 (x > 4) の水面に浮かぶ ----
    {
        GameObject buoy = s.CreateGameObject("Buoy");
        buoy.SetLocalPosition(8.0f, 4.0f, 0.0f);
        buoy.SetLocalScale(0.8f, 0.8f, 0.8f);
        auto* mr = buoy.AddComponent<MeshRendererComponent>();
        mr->mesh = sphere;
        mr->material = AssetID{ HashStr("pdemo_buoy") };
        auto* col = buoy.AddComponent<ColliderComponent>();
        col->shape = 0;
        col->radius = 0.5f;
        auto* rb = buoy.AddComponent<RigidbodyComponent>();
        // 半径 0.4 の球 = 0.268 m^3。その半分の水を押しのける重さ = 半没で釣り合う
        rb->mass = 134.0f;
        // M59f1: 重心を球心より下げる = 復原モーメントが立つ (傾けても起き上がる浮き)
        rb->centerOfMass = { 0.0f, -0.25f, 0.0f };
        auto* b = buoy.AddComponent<BuoyancyComponent>();
        b->linearDrag = 3.0f;
    }

    // ---- 4b. ジャイロ: 中間軸で回す平たい箱 (テニスラケット定理が replay に載る) ----
    // 無重力にはできないので落下しながら回る。**ジャイロ項は opt-in** なので、
    // このシーンに 1 体だけ置いて Debug/Release のビット一致を .rep で担保する
    {
        GameObject t = s.CreateGameObject("Tumbler");
        t.SetLocalPosition(8.0f, 9.0f, 5.0f);
        t.SetLocalScale(0.3f, 1.0f, 2.0f);
        auto* mr = t.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr("pdemo_rubber") };
        auto* col = t.AddComponent<ColliderComponent>();
        col->shape = 1;
        col->halfExtents = { 0.5f, 0.5f, 0.5f };
        auto* rb = t.AddComponent<RigidbodyComponent>();
        rb->mass = 1.0f;
        rb->gyroscopic = true;
        rb->angularDamping = 0.0f;
        rb->angularVelocity = { 0.05f, 12.0f, 0.0f }; // 中間軸 (ローカル Y) + 擾乱
    }

    // ---- 5. 材料プリセット: ゴム球が鋼の床で弾む (静的側が e を主張する新能力) ----
    {
        GameObject ball = s.CreateGameObject("RubberBall");
        ball.SetLocalPosition(-1.0f, 7.0f, 4.0f);
        ball.SetLocalScale(0.6f, 0.6f, 0.6f);
        auto* mr = ball.AddComponent<MeshRendererComponent>();
        mr->mesh = sphere;
        mr->material = AssetID{ HashStr("pdemo_rubber") };
        auto* col = ball.AddComponent<ColliderComponent>();
        col->shape = 0;
        col->radius = 0.5f;
        col->physMaterial = matRubber;
        auto* rb = ball.AddComponent<RigidbodyComponent>();
        rb->useDensity = true; // ゴムの密度から質量を導く
    }

    // ---- 6. 転がる箱の山: 既存ソルバ (接触・摩擦) もこのシーンで被覆する ----
    for (int i = 0; i < 4; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "Crate_%d", i);
        GameObject go = s.CreateGameObject(name);
        go.SetLocalPosition(-6.0f + 0.05f * static_cast<float>(i), 1.0f + 1.1f * static_cast<float>(i),
                            5.0f);
        auto* mr = go.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr("pdemo_floor") };
        auto* col = go.AddComponent<ColliderComponent>();
        col->shape = 1;
        col->halfExtents = { 0.5f, 0.5f, 0.5f };
        col->physMaterial = (i % 2 == 0) ? matSteel : matRubber;
        auto* rb = go.AddComponent<RigidbodyComponent>();
        rb->mass = 1.0f;
    }

    // ---- 7. CCD: 高速弾が厚さ 0.1m の壁で止まる (M59j) ----
    // ★1 サブステップ (env の substeps=4 なので dt/4) の移動量が 0.5m = 壁厚の 5 倍。
    //   CCD が無ければ確実に抜ける速度で、掃引経路が **必ず** replay 被覆に載るようにしてある
    {
        GameObject wall = s.CreateGameObject("ThinWall");
        wall.SetLocalPosition(-8.0f, 1.5f, 2.0f);
        wall.SetLocalScale(0.1f, 2.0f, 2.0f);
        auto* mr = wall.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr("pdemo_steel") };
        auto* col = wall.AddComponent<ColliderComponent>();
        col->shape = 1;
        col->halfExtents = { 0.5f, 0.5f, 0.5f }; // ワールドスケールが効く = 厚さ 0.1m
        col->physMaterial = matSteel;
    }
    {
        GameObject bullet = s.CreateGameObject("Bullet");
        bullet.SetLocalPosition(-11.5f, 1.5f, 2.0f);
        bullet.SetLocalScale(0.16f, 0.16f, 0.16f);
        auto* mr = bullet.AddComponent<MeshRendererComponent>();
        mr->mesh = sphere;
        mr->material = AssetID{ HashStr("pdemo_ball") };
        auto* col = bullet.AddComponent<ColliderComponent>();
        col->shape = 0;
        col->radius = 0.5f; // ワールド半径 0.08m
        // ★材料を**割り当てない** = e は Rigidbody の 0。鋼 (e=0.6) を付けると
        //   CCD の一発インパルスが 1.6 倍の跳ね返りを作り、弾が 72 m/s で後ろへ飛んで
        //   床の端 (x=-12) を越え、以降 600 tick ずっと落下し続ける (実測)。
        //   ショーケースとしても replay の被写体としても、壁に食い込んで止まる方が良い
        auto* rb = bullet.AddComponent<RigidbodyComponent>();
        rb->mass = 0.05f;
        rb->velocity = { 120.0f, 0.0f, 0.0f };
        rb->ccd = true;
    }
}

void RegisterRenderShowcaseContent(EngineContext& ctx)
{
    RenderResources& res = *ctx.resources;
    const AssetID white = res.textures.White();
    const AssetID shader = AssetID{ HashStr("forward_lit") };
    res.meshes.Cube();
    res.meshes.Sphere();

    // 名前は **rdemo_ 接頭辞**。既定デモ (mat_*) / RT (rt_*) / parts / flow / mp_ / duel_ の
    // どれとも衝突させない — 材質は全ショーケース分が無条件登録されるので、名前が被ると
    // 「後勝ちで別のシーンの色に化ける」が静かに起きる
    auto makeMat = [&](const char* name, float r, float g, float b, float metallic,
                       float roughness, float emissive) {
        Material m;
        m.shader = shader;
        m.texture = white;
        m.baseColor = { r, g, b, 1.0f };
        m.metallic = metallic;
        m.roughness = roughness;
        m.emissiveIntensity = emissive;
        return res.materials.Register(name, m);
    };
    makeMat("rdemo_ground", 0.34f, 0.35f, 0.38f, 0.0f, 0.90f, 0.0f);
    makeMat("rdemo_pillar_a", 0.72f, 0.70f, 0.66f, 0.0f, 0.75f, 0.0f);
    makeMat("rdemo_pillar_b", 0.60f, 0.34f, 0.28f, 0.0f, 0.65f, 0.0f);
    makeMat("rdemo_pillar_c", 0.28f, 0.42f, 0.56f, 0.0f, 0.55f, 0.0f);
    // 反射床パッチ。M56d (SSR) と M56f (反射プローブ) の被写体そのものなので、
    // **粗さは 0.1 以下**にしておく (SSR は粗さでフェードするため、粗いと画に出ない)
    makeMat("rdemo_mirror", 0.85f, 0.87f, 0.90f, 0.90f, 0.10f, 0.0f);
    makeMat("rdemo_spin", 0.95f, 0.58f, 0.14f, 0.0f, 0.45f, 0.0f);
    makeMat("rdemo_far", 0.42f, 0.45f, 0.52f, 0.0f, 0.85f, 0.0f);
    // ライト位置の目印。**発光だけ**でライティングには寄与しない (エンジンに面光源は無い) —
    // 「どこにスポット/点光源があるか」が絵の上で分かると M54c/M54d の A/B が読みやすい
    makeMat("rdemo_lamp_warm", 1.00f, 0.82f, 0.55f, 0.0f, 1.0f, 5.0f);
    makeMat("rdemo_lamp_cool", 0.70f, 0.85f, 1.00f, 0.0f, 1.0f, 5.0f);
}

void BuildRenderShowcaseScene(EngineContext& ctx)
{
    Scene& s = *ctx.scene;
    RenderResources& res = *ctx.resources;

    RegisterRenderShowcaseContent(ctx); // 単独で呼ばれても実体が揃うようにしておく
    s.SetName("render_showcase");
    const AssetID cube = res.meshes.Cube();
    const AssetID sphere = res.meshes.Sphere();
    const AssetID matGround = AssetID{ HashStr("rdemo_ground") };
    const AssetID matMirror = AssetID{ HashStr("rdemo_mirror") };
    const AssetID matSpin = AssetID{ HashStr("rdemo_spin") };
    const AssetID matFar = AssetID{ HashStr("rdemo_far") };
    const AssetID pillarMats[3] = {
        AssetID{ HashStr("rdemo_pillar_a") },
        AssetID{ HashStr("rdemo_pillar_b") },
        AssetID{ HashStr("rdemo_pillar_c") },
    };

    auto box = [&](const char* name, float px, float py, float pz, float sx, float sy, float sz,
                   AssetID mat) {
        GameObject go = s.CreateGameObject(name);
        go.SetLocalPosition(px, py, pz);
        go.SetLocalScale(sx, sy, sz);
        auto* mr = go.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = mat;
        return go;
    };
    auto ball = [&](const char* name, float px, float py, float pz, float diameter, AssetID mat) {
        GameObject go = s.CreateGameObject(name);
        go.SetLocalPosition(px, py, pz);
        go.SetLocalScale(diameter, diameter, diameter); // Sphere() は半径 0.5 = 直径 1
        auto* mr = go.AddComponent<MeshRendererComponent>();
        mr->mesh = sphere;
        mr->material = mat;
        return go;
    };

    // ---- カメラ ----
    // shot_verify は 960x540 (16:9) で撮る。この位置/画角で「柱の群れ + 反射パッチ +
    // 地平線 + 遠景」が 1 枚に収まる。俯角 14° は「床を広く見せつつ空も残す」下限
    GameObject camera = s.CreateGameObject("Main Camera");
    {
        auto* cam = camera.AddComponent<CameraComponent>();
        cam->fovYDeg = 55.0f;
    }
    camera.SetLocalPosition(0.0f, 8.0f, -22.0f);
    camera.SetLocalRotationEuler(13.0f, 0.0f, 0.0f);

    // ---- 太陽 (平行光) ----
    // **必ず残す** — CSM (M54c 以降が拡張する既存のシャドウパス) の被覆がここでしか取れない。
    // 強度は既定の 1.0 から落としてある: 平行光が支配的だと局所ライトの寄与が絵に出ず、
    // M54c/M54d の A/B が「差が見えない」で終わってしまう。アンビエントも同じ理由で低め
    // (アンビエントは **最初のライト**のものが全体に使われるので、Sun を最初に作る)
    GameObject sun = s.CreateGameObject("Sun");
    {
        auto* l = sun.AddComponent<LightComponent>();
        l->intensity = 0.85f;
        l->color = { 1.00f, 0.96f, 0.88f };
        l->ambient = { 0.13f, 0.14f, 0.17f };
    }
    sun.SetLocalRotationEuler(46.0f, -34.0f, 0.0f);

    // ---- 床 200x200 ----
    // 大スケールにしてあるのは CSM のカスケード分割 (カメラフィット、kShadowMaxDist=60) を
    // 実際に働かせるため。M58 の地形もこのスケール感になる
    box("Ground", 0.0f, -0.5f, 0.0f, 200.0f, 1.0f, 200.0f, matGround);

    // ---- 反射床パッチ (M56d SSR / M56f 反射プローブの被写体) ----
    // 床とほぼ同じ高さに薄く敷く。柱がこの上に立つので「柱が映り込む」絵になる
    box("MirrorPatch", 0.0f, 0.05f, 8.0f, 18.0f, 0.1f, 12.0f, matMirror);

    // ---- 柱 20 本 ----
    // ★座標は**固定表**。rand() はもちろん、trig を使った「それらしいばらつき」も入れない
    //   (規則 2)。並びの意図は 3 つ:
    //     ・スポット 2 本の光錐に入る柱を作る            (M54c の透視シャドウが画に出る)
    //     ・点光源 2 個を囲む柱を作る                    (M54d のキューブ 6 面の影が交差する)
    //     ・反射パッチの上/縁に立つ柱を作る              (M56d の SSR が画に出る)
    struct Pillar {
        float x;
        float z;
        float height;
        float width;
        int mat;
    };
    static const Pillar kPillars[] = {
        { -16.0f, -4.0f, 3.5f, 1.6f, 0 }, { -9.0f, -6.0f, 2.4f, 1.2f, 1 },
        { -3.0f, -5.0f, 4.5f, 1.4f, 2 },  { 4.0f, -6.0f, 3.0f, 1.8f, 0 },
        { 11.0f, -4.0f, 5.0f, 1.4f, 1 },  { 18.0f, -2.0f, 2.8f, 1.6f, 2 },
        { -19.0f, 4.0f, 4.2f, 1.5f, 1 },  { -12.0f, 3.0f, 6.0f, 1.3f, 2 },
        { -6.0f, 6.0f, 3.2f, 1.7f, 0 },   { 0.0f, 3.0f, 2.2f, 2.2f, 1 },
        { 7.0f, 5.0f, 5.5f, 1.4f, 2 },    { 14.0f, 7.0f, 3.6f, 1.6f, 0 },
        { -15.0f, 12.0f, 5.2f, 1.5f, 2 }, { -8.0f, 14.0f, 3.0f, 1.3f, 0 },
        { -1.0f, 12.0f, 6.5f, 1.5f, 1 },  { 6.0f, 15.0f, 4.0f, 1.8f, 2 },
        { 13.0f, 13.0f, 2.6f, 1.4f, 1 },  { -11.0f, 22.0f, 7.0f, 1.6f, 0 },
        { 2.0f, 24.0f, 5.0f, 1.5f, 2 },   { 10.0f, 21.0f, 6.2f, 1.4f, 1 },
    };
    for (int i = 0; i < static_cast<int>(std::size(kPillars)); ++i) {
        const Pillar& p = kPillars[i];
        char name[32];
        std::snprintf(name, sizeof(name), "Pillar_%02d", i);
        box(name, p.x, p.height * 0.5f, p.z, p.width, p.height, p.width, pillarMats[p.mat]);
    }

    // ---- スポットライト 2 本 (M54c: 透視 1 面のシャドウ) ----
    // 光の向きはエンティティのローカル +Z (RenderSystem がワールド行列の第 3 行を使う)。
    // **柱を斜めから照らす**向きに置いてある — 真上から当てると影が柱の足元に潰れて、
    // シャドウが入ったかどうかが絵で判別できない
    auto spot = [&](const char* name, float px, float py, float pz, float pitchDeg, float yawDeg,
                    float r, float g, float b, float intensity, AssetID markerMat) {
        GameObject go = s.CreateGameObject(name);
        go.SetLocalPosition(px, py, pz);
        go.SetLocalRotationEuler(pitchDeg, yawDeg, 0.0f);
        auto* l = go.AddComponent<LightComponent>();
        l->type = 2;
        l->color = { r, g, b };
        l->intensity = intensity;
        l->range = 42.0f;
        l->spotInnerDeg = 18.0f;
        l->spotOuterDeg = 30.0f;
        // M54e: ここで初めて局所影を立てる。M54b〜M54d の間は既定 0 のままにしてあった
        // ので、golden 8 枚は「機能を足しても絵が動かない」でビット一致し続けていた。
        // このコミットから demo_render_* の 2 枚だけが動く (それ以外が動いたらバグ)
        l->castShadow = 1;
        // 目印 (発光する小球)。ライトエンティティの子にすると位置が自動で追従する
        GameObject marker = s.CreateGameObject((std::string(name) + "_Marker").c_str());
        marker.SetParent(go);
        marker.SetLocalScale(0.5f, 0.5f, 0.5f);
        auto* mr = marker.AddComponent<MeshRendererComponent>();
        mr->mesh = sphere;
        mr->material = markerMat;
        return go;
    };
    spot("SpotLeft", -12.0f, 10.0f, 4.0f, 44.0f, 34.0f, 1.00f, 0.88f, 0.70f, 9.0f,
         AssetID{ HashStr("rdemo_lamp_warm") });
    spot("SpotRight", 12.0f, 10.0f, 6.0f, 44.0f, -34.0f, 0.72f, 0.86f, 1.00f, 9.0f,
         AssetID{ HashStr("rdemo_lamp_cool") });

    // ---- 点光源 2 個 (M54d: キューブ 6 面のシャドウ) ----
    // **柱の間**に低く置く。全方位に影を落とすので、周囲の柱の影が床の上で交差する =
    // 6 面ぶんのアトラスが正しく張られたかが 1 枚の絵で分かる
    auto point = [&](const char* name, float px, float py, float pz, float r, float g, float b,
                     float intensity, float range, AssetID markerMat) {
        GameObject go = s.CreateGameObject(name);
        go.SetLocalPosition(px, py, pz);
        auto* l = go.AddComponent<LightComponent>();
        l->type = 1;
        l->color = { r, g, b };
        l->intensity = intensity;
        l->range = range;
        l->castShadow = 1; // M54e: 点光源はキューブ 6 面 (M54d) をアトラスへ焼く
        GameObject marker = s.CreateGameObject((std::string(name) + "_Marker").c_str());
        marker.SetParent(go);
        marker.SetLocalScale(0.4f, 0.4f, 0.4f);
        auto* mr = marker.AddComponent<MeshRendererComponent>();
        mr->mesh = sphere;
        mr->material = markerMat;
        return go;
    };
    point("PointWarm", -5.0f, 2.6f, 6.0f, 1.00f, 0.55f, 0.22f, 6.0f, 18.0f,
          AssetID{ HashStr("rdemo_lamp_warm") });
    point("PointCool", 5.5f, 2.6f, 13.0f, 0.35f, 0.65f, 1.00f, 6.0f, 18.0f,
          AssetID{ HashStr("rdemo_lamp_cool") });

    // ---- デカール 2 枚 (M56a / M56b) ----
    // ★**golden に載せるために置いている。** 1 枚も置かないと「デカールが壊れても
    //   demo_render_deferred は緑のまま」= 回帰の被覆がゼロになる (M54a でこのシーンを
    //   作った理由そのもの)。置いた代償として demo_render_deferred / demo_render_taa の
    //   2 枚が M56a で動く (demo_render_forward が**動かない**ことが v1 の Forward 非対応の証明)。
    // ★albedo テクスチャは付けない (null = 白 = color がそのまま出る)。AssetRef はシーン
    //   JSON へ 64bit の AssetID をそのまま書く仕様で、テクスチャの AssetID は正規化絶対
    //   パスのハッシュ = チェックアウト依存になる (M51j で踏んだ穴)。**このシーンは
    //   コードから毎回組み直す生成物 (cache\render_showcase.scene.json は gitignore) なので
    //   それでも構わない** — 実際 M56b では法線マップをファイルから読んでいる。
    // 向きは **ローカル +Z が投影方向** (LightComponent と同じ規約)。
    auto decal = [&](const char* name, float px, float py, float pz, float pitchDeg, float yawDeg,
                     float sx, float sy, float sz, float r, float g, float b, float a,
                     float angleFadeDeg, int32_t sortOrder) {
        GameObject go = s.CreateGameObject(name);
        go.SetLocalPosition(px, py, pz);
        go.SetLocalRotationEuler(pitchDeg, yawDeg, 0.0f);
        go.SetLocalScale(sx, sy, sz);
        auto* d = go.AddComponent<DecalComponent>();
        d->color = { r, g, b, a };
        d->angleFadeDeg = angleFadeDeg;
        d->sortOrder = sortOrder;
        return go;
    };
    // 1 枚目: 床へ真下投影 (pitch 90 = ローカル +Z が世界の -Y)。箱は
    // x ∈ [-12,-4] / z ∈ [-9,-3] / y ∈ [-1,3] で、Pillar_01 (x=-9 z=-6 高さ 2.4) を
    // すっぽり含む位置にしてある。**床と柱の天面には乗り、柱の側面には乗らない**
    // (側面は法線が投影方向と直角 = 角度フェード 0) — スクリーンスペースの貼り絵ではなく
    // 面へ投影していることが 1 枚の絵で分かる
    GameObject decalGround = decal("DecalGround", -8.0f, 1.0f, -6.0f, 90.0f, 0.0f, 8.0f, 6.0f,
                                   4.0f, 0.95f, 0.42f, 0.12f, 0.85f, 90.0f, 0);
    // ---- M56b: この 1 枚だけ法線 / roughness も上描きする ----
    // ★**2 枚のうち 1 枚だけ**にしてあるのが要点。DecalPillar は強度 0 のまま =
    //   「M56b を足しても albedo だけのデカールは 1 ビットも動かない」が同じ 1 枚の絵で
    //   検証できる (img-diff の差分が DecalGround の footprint に閉じるはず)。
    // ★法線マップは既にリポジトリに居る assets\textures\demo_normal.png (256^2、
    //   平坦 128,128,255 に凹凸)。**リニアで読む** (法線マップに sRGB を掛けると凹凸が歪む)。
    //   平坦な床と柱の天面に貼るので、法線を書き換えないと絵が 1 画素も変わらない
    //   (投影方向 = -Y = 受け面の法線そのものなので、法線マップ無しの「平坦なデカール」は
    //   定義上なにもしない) — つまり**このテクスチャが無いと M56b の被覆が作れない**。
    //   assetsRoot にファイルが無い環境では null が返り、平坦 = 恒等に落ちる。
    if (auto* dg = decalGround.GetComponent<DecalComponent>()) {
        dg->normalTex = res.textures.LoadFile(ctx.assetsRoot + L"\\textures\\demo_normal.png",
                                              /*srgb=*/false);
        dg->normalStrength = 1.0f;
        // 濡れた路面のように鏡面を立てる。床 (rdemo_ground) の粗さは 0.90 なので
        // 0.15 との差はスポット 2 本と点光源 2 個のハイライトとしてはっきり出る
        dg->roughness = 0.15f;
        dg->roughnessStrength = 1.0f;
    }
    // 2 枚目: 柱の側面へ水平投影 (回転なし = ローカル +Z が世界の +Z = カメラの奥向き)。
    // Pillar_03 (x=4, z=-6, 幅 1.8, 高さ 3.0) の手前の面だけを覆う箱で、床 (y=0) は
    // 箱の下端 y=0.5 の外なので掛からない = 「箱の外は捨てる」が角度フェードとは
    // 独立に効いていることの目印になる
    decal("DecalPillar", 4.0f, 1.5f, -5.0f, 0.0f, 0.0f, 2.5f, 2.0f, 4.0f,
          0.20f, 0.75f, 0.95f, 0.90f, 70.0f, 1);

    // ---- 反射プローブ 1 個 (M56f) ----
    // ★**置いても絵は 1 ビットも変わらない** — ベイクは常に明示指示 (`--probe-bake-all` /
    //   `View > Rendering > すべての反射プローブをベイク`) なので、golden はプローブを
    //   足しても不変のまま。デカール (M56a) が golden 2 枚を動かしたのとはここが違う。
    // 箱は反射パッチ MirrorPatch (中心 (0,0.05,8) / 18 x 12) をちょうど覆う大きさで、
    // 撮影点は床から 3 の高さ = 柱が水平 4 面にしっかり写る位置。**視差補正 (ボックス投影)
    // が効いているかは、床の映り込みが箱の壁で切り替わるかで読める**
    {
        GameObject probe = s.CreateGameObject("ReflectionProbe");
        probe.SetLocalPosition(0.0f, 3.0f, 8.0f);
        auto* rp = probe.AddComponent<ReflectionProbeComponent>();
        rp->extents = { 11.0f, 6.0f, 8.0f };
        rp->blendDistance = 1.5f;
    }

    // ---- 回転する物体 (M55c の velocity / M55e のモーションブラー) ----
    // 既定デモの Spinner (DemoContent.cpp の Rotator) と同じ流儀。腕を付けてあるのは
    // 「回っていること」が静止画 1 枚でも分かるようにするため
    GameObject spinner = s.CreateGameObject("Spinner");
    spinner.SetLocalPosition(0.0f, 3.4f, -4.0f);
    for (int a = 0; a < 3; ++a) {
        char name[32];
        std::snprintf(name, sizeof(name), "SpinArm_%d", a);
        GameObject arm = s.CreateGameObject(name);
        arm.SetParent(spinner);
        arm.SetLocalRotationEuler(0.0f, a * 120.0f, 0.0f);
        GameObject blade = s.CreateGameObject((std::string(name) + "_Blade").c_str());
        blade.SetParent(arm);
        blade.SetLocalPosition(0.0f, 0.0f, 2.4f);
        blade.SetLocalScale(0.5f, 0.5f, 3.2f);
        auto* mr = blade.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = matSpin;
    }
    AttachScriptIfRegistered(s.GetWorld(), spinner.Id(), "Rotator");

    // ---- 遠景 (DoF / フォグ / M58e の LOD) ----
    // CSM の届く範囲 (kShadowMaxDist=60) の外に置いてある。影が付かない距離帯が
    // 画にあること自体が M54 の「どこまで影が出るか」の目視材料になる
    box("Far_0", -30.0f, 6.0f, 60.0f, 12.0f, 12.0f, 8.0f, matFar);
    box("Far_1", 25.0f, 8.0f, 72.0f, 16.0f, 16.0f, 10.0f, matFar);
    box("Far_2", 0.0f, 10.0f, 88.0f, 20.0f, 20.0f, 14.0f, matFar);
    box("Far_3", -55.0f, 7.0f, 78.0f, 14.0f, 14.0f, 12.0f, matFar);
    box("Far_4", 48.0f, 5.0f, 55.0f, 10.0f, 10.0f, 7.0f, matFar);
    ball("FarDome", -38.0f, 3.0f, 44.0f, 6.0f, matFar);

    // ---- 高さフォグ (M57d のフロクセルが置き換える対象) ----
    // 色は既定の clearColor (0.08,0.09,0.11) 近傍にしてある — 背景にはフォグが掛からない
    // (ApplyFog はマテリアルシェーダの中) ので、離れた色を選ぶと地平線に段が出る
    GameObject fog = s.CreateGameObject("Fog");
    {
        auto* f = fog.AddComponent<FogComponent>();
        f->mode = 2; // Exp2
        f->color = { 0.11f, 0.13f, 0.17f, 1.0f };
        f->density = 0.011f;
        f->heightFalloff = 0.035f; // 高いところほど薄い = 柱の頭が抜けて見える
        f->baseHeight = 0.0f;
        f->inscatterIntensity = 0.30f; // 太陽方向にだけ僅かに明るい
        f->inscatterPower = 10.0f;
    }

    s.GetWorld().ApplyStructuralChanges();
}

void RegisterTerrainShowcaseContent(EngineContext& ctx)
{
    RenderResources& res = *ctx.resources;
    const AssetID white = res.textures.White();
    const AssetID shader = AssetID{ HashStr("forward_lit") };
    res.meshes.Cube();

    // 名前は **tdemo_ 接頭辞** (rdemo_ / mat_ / rt_ / parts / flow / mp_ / duel_ と衝突させない —
    // 材質は全ショーケース分が無条件登録されるので、被ると後勝ちで別の色に化ける)
    Material m;
    m.shader = shader;
    m.texture = white;
    m.baseColor = { 0.86f, 0.36f, 0.20f, 1.0f };
    m.metallic = 0.0f;
    m.roughness = 0.55f;
    // 参照用の柱。**地形の起伏に対する縦のスケール**を絵の中に置くためだけの存在で、
    // 「地形と通常メッシュが同じ深度バッファを共有しているか」を 1 枚の絵で見分ける材料になる
    // (地形だけだと前後関係のバグが画に出ない)
    res.materials.Register("tdemo_marker", m);
}

void BuildTerrainShowcaseScene(EngineContext& ctx, float lodDistance, float skirtDepth)
{
    Scene& s = *ctx.scene;
    RenderResources& res = *ctx.resources;

    RegisterTerrainShowcaseContent(ctx); // 単独で呼ばれても実体が揃うようにしておく
    s.SetName("terrain_showcase");
    const AssetID cube = res.meshes.Cube();
    const AssetID matMarker = AssetID{ HashStr("tdemo_marker") };

    // ---- カメラ ----
    // 地形は 256x256 m / 高さ -4..+32 m (assets\terrain\demo.terrain.json)。
    // 960x540 (16:9) の shot_verify で「起伏の陰影 + 地平線 + 空」が入る位置に置く。
    //
    // ★**手前の縁 (z=-128) を画面の下に追い出す**位置合わせが要る。地形は「面」であって
    //   「塊」ではないので、視線が縁の高さより下を通ると**裏面カリングで地形が消え、
    //   画面の下端に空色の帯が出る** (最初の試写で実際に出た。バグにしか見えない)。
    //   条件は atan((camY - 地形の最大高さ) / 縁までの距離) > 俯角 + 縦画角の半分:
    //   camY=50 / 縁まで 14 m / 最大高さ 32 m → 52° > 17 + 27.5 = 44.5° (余裕 7.5°)。
    //   カメラを引くほどこの余裕が減る — 引きたくなったら高さも一緒に上げること
    GameObject camera = s.CreateGameObject("Main Camera");
    {
        auto* cam = camera.AddComponent<CameraComponent>();
        cam->fovYDeg = 55.0f;
        cam->farZ = 800.0f; // 奥の縁 (約 270 m 先) まで確実に入る
    }
    camera.SetLocalPosition(0.0f, 50.0f, -142.0f);
    camera.SetLocalRotationEuler(17.0f, 0.0f, 0.0f);

    // ---- 太陽 (平行光) ----
    // 斜めから当てて尾根と谷にコントラストを付ける。真上だと起伏が飛んで
    // 「法線が正しいか」が絵で判別できなくなる (M58b の継ぎ目検査の目視版)
    GameObject sun = s.CreateGameObject("Sun");
    {
        auto* l = sun.AddComponent<LightComponent>();
        l->intensity = 1.15f;
        l->color = { 1.00f, 0.95f, 0.86f };
        l->ambient = { 0.16f, 0.18f, 0.22f };
    }
    sun.SetLocalRotationEuler(38.0f, -28.0f, 0.0f);

    // ---- 空 (グラデーション) ----
    // 地形の輪郭が空との境界で出る = 「チャンクが 1 枚欠けた」が絵で分かる
    GameObject sky = s.CreateGameObject("Sky");
    {
        auto* sk = sky.AddComponent<SkyboxComponent>();
        sk->topColor = { 0.20f, 0.38f, 0.72f, 1.0f };
        sk->horizonColor = { 0.72f, 0.80f, 0.88f, 1.0f };
        sk->bottomColor = { 0.30f, 0.28f, 0.24f, 1.0f };
    }

    // ---- 地形 ----
    // source は **assets\ 相対**で持つ (M58b: AssetID にすると Inspector の参照ピッカーが
    // 引くべきランタイムライブラリが無い。相対パスならシーン JSON がチェックアウト先に
    // 依存しない = M51j の絶対パスハッシュ問題も踏まない)
    GameObject terrain = s.CreateGameObject("Terrain");
    {
        auto* t = terrain.AddComponent<TerrainComponent>();
        std::snprintf(t->source, sizeof(t->source), "terrain/demo.terrain.json");
        t->chunkTiles = 32; // 128 タイル / 32 = 4x4 = 16 チャンク
        // M58e: 既定 0 = LOD 無効。--terrain-lod のときだけ点く。
        // ★このカメラ (y=50 / z=-142 / 俯角 17°) だとチャンク行の viewZ はおおよそ
        //   57 / 118 / 179 / 241 m。lodDistance=80 で 3 段すべてが 1 枚の絵に入る =
        //   LOD 境界が 2 本できるので、クラックが出るなら必ず映る
        t->lodDistance = lodDistance;
        t->skirtDepth = skirtDepth;
    }

    // ---- 参照用の柱 ----
    // 手前側に等間隔で並べる。y は地形の高さを問い合わせずに固定値で置いてある —
    // 高さ問い合わせ API は M59 (地形コリジョン) の話で、今は ECS にも ABI にも無い。
    // 柱の足元が地面に埋まるか浮くかは golden の主張ではない (縦の目盛りとしてだけ使う)
    static const float kMarkerX[] = { -60.0f, -20.0f, 20.0f, 60.0f };
    for (int i = 0; i < static_cast<int>(std::size(kMarkerX)); ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "Marker_%d", i);
        GameObject go = s.CreateGameObject(name);
        go.SetLocalPosition(kMarkerX[i], 16.0f, -70.0f);
        go.SetLocalScale(3.0f, 32.0f, 3.0f);
        auto* mr = go.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = matMarker;
    }

    s.GetWorld().ApplyStructuralChanges();
}

// ---- M60i: 関節と機構 (M60) のショーケース ----

// 車輪メッシュ (M60i): **軸が X 向き**の多角柱。builtin の Cylinder は軸が Y なので
// そのままでは使えず、車輪エンティティを回して寝かせることも**できない** —
// 回すとサスのレイ方向 (ローカル -Y) と前方向 (ローカル +Z) まで一緒に回ってしまう
// (`PhysicsSystem.cpp` の車輪の配線が正本)。
// ★半径と幅をメッシュへ焼き込んであるのは **エンティティのスケールを 1 に保つため**。
//   描画側の回転 (RenderSystem) は「エンティティのローカル空間 = スケールが掛かる前」に
//   効くので、非一様スケールの車輪は回すと歪む。
// ★側面は分割ごとにフラット法線。丸く均すと軸対称になり、**転がっていることが絵から
//   消える** (回転角を読んでいるかどうかが目で確かめられなくなる)
AssetID RegisterWheelMesh(RenderResources& res, const char* name, float radius, float halfWidth)
{
    constexpr int kSeg = 16;
    constexpr float kTwoPi = 6.28318530718f;
    std::vector<MeshVertex> verts;
    std::vector<uint32_t> indices;
    verts.reserve(static_cast<size_t>(kSeg) * 10);
    indices.reserve(static_cast<size_t>(kSeg) * 12);

    for (int i = 0; i < kSeg; ++i) {
        const float a0 = kTwoPi * static_cast<float>(i) / static_cast<float>(kSeg);
        const float a1 = kTwoPi * static_cast<float>(i + 1) / static_cast<float>(kSeg);
        const float am = 0.5f * (a0 + a1);
        const float c0 = std::cos(a0), s0 = std::sin(a0);
        const float c1 = std::cos(a1), s1 = std::sin(a1);
        const float y0 = radius * c0, z0 = radius * s0;
        const float y1 = radius * c1, z1 = radius * s1;
        const float u0 = static_cast<float>(i) / static_cast<float>(kSeg);
        const float u1 = static_cast<float>(i + 1) / static_cast<float>(kSeg);
        // 接地面 (側面)。巻き順の規約は builtin と同じ = cross(v1-v0, v2-v0) が外向き
        const DirectX::XMFLOAT3 nSide{ 0.0f, std::cos(am), std::sin(am) };
        const uint32_t b = static_cast<uint32_t>(verts.size());
        verts.push_back({ { -halfWidth, y0, z0 }, nSide, { u0, 1.0f } });
        verts.push_back({ { halfWidth, y0, z0 }, nSide, { u0, 0.0f } });
        verts.push_back({ { halfWidth, y1, z1 }, nSide, { u1, 0.0f } });
        verts.push_back({ { -halfWidth, y1, z1 }, nSide, { u1, 1.0f } });
        indices.insert(indices.end(), { b, b + 3, b + 2, b, b + 2, b + 1 });
        // 側板 (+X)
        const uint32_t cp = static_cast<uint32_t>(verts.size());
        const DirectX::XMFLOAT3 nPos{ 1.0f, 0.0f, 0.0f };
        verts.push_back({ { halfWidth, 0.0f, 0.0f }, nPos, { 0.5f, 0.5f } });
        verts.push_back({ { halfWidth, y0, z0 }, nPos, { 0.5f + 0.5f * c0, 0.5f + 0.5f * s0 } });
        verts.push_back({ { halfWidth, y1, z1 }, nPos, { 0.5f + 0.5f * c1, 0.5f + 0.5f * s1 } });
        indices.insert(indices.end(), { cp, cp + 1, cp + 2 });
        // 側板 (-X)。法線が逆なので頂点順も逆に回す
        const uint32_t cm = static_cast<uint32_t>(verts.size());
        const DirectX::XMFLOAT3 nNeg{ -1.0f, 0.0f, 0.0f };
        verts.push_back({ { -halfWidth, 0.0f, 0.0f }, nNeg, { 0.5f, 0.5f } });
        verts.push_back({ { -halfWidth, y1, z1 }, nNeg, { 0.5f + 0.5f * c1, 0.5f + 0.5f * s1 } });
        verts.push_back({ { -halfWidth, y0, z0 }, nNeg, { 0.5f + 0.5f * c0, 0.5f + 0.5f * s0 } });
        indices.insert(indices.end(), { cm, cm + 1, cm + 2 });
    }
    return res.meshes.Register(name, verts, indices);
}

void RegisterJointShowcaseContent(EngineContext& ctx)
{
    RenderResources& res = *ctx.resources;
    const AssetID white = res.textures.White();
    const AssetID shader = AssetID{ HashStr("forward_lit") };
    res.meshes.Cube();
    res.meshes.Sphere();

    auto makeMat = [&](const char* name, float r, float g, float b) {
        Material m;
        m.shader = shader;
        m.texture = white;
        m.baseColor = { r, g, b, 1.0f };
        return res.materials.Register(name, m);
    };
    // 接頭辞は **jdemo_**。材質は全ショーケース分が無条件登録されるので、他と名前が
    // 被ると先に登録したほうが黙って上書きされる (M54a の申し送りと同じ配慮)
    makeMat("jdemo_ground", 0.28f, 0.30f, 0.33f);
    makeMat("jdemo_frame", 0.45f, 0.46f, 0.50f); // 柱・塔 (静的な受け手)
    makeMat("jdemo_swing", 0.85f, 0.72f, 0.25f); // 振り子・ロープ
    makeMat("jdemo_door", 0.72f, 0.36f, 0.28f);  // ドア
    makeMat("jdemo_motor", 0.30f, 0.62f, 0.78f); // モータで駆動されるもの
    makeMat("jdemo_weld", 0.55f, 0.55f, 0.62f);  // 固定関節
    makeMat("jdemo_plank", 0.66f, 0.50f, 0.32f); // 桟橋の甲板
    makeMat("jdemo_crate", 0.78f, 0.30f, 0.30f); // 落とす荷物
    makeMat("jdemo_hull", 0.40f, 0.70f, 0.45f);  // 凸包
    makeMat("jdemo_car", 0.24f, 0.34f, 0.68f);   // 車体
    makeMat("jdemo_glue", 0.86f, 0.66f, 0.20f);  // 粘着 (糊) の天井
    // ★真っ黒にしない。面ごとのフラット法線が拾う陰影が潰れて、**転がっているか
    //   どうかが絵から消える** (回転角を描画側が読んでいることの目視確認ができなくなる)
    makeMat("jdemo_tire", 0.26f, 0.26f, 0.29f); // タイヤ
    // 車輪は Wheel コンポーネントの既定寸法 (半径 0.35 / 幅 0.25) に合わせて焼く
    RegisterWheelMesh(res, "jdemo_wheel", 0.35f, 0.125f);
}

void BuildJointShowcaseScene(EngineContext& ctx)
{
    Scene& s = *ctx.scene;
    World& w = s.GetWorld();
    RenderResources& res = *ctx.resources;
    s.SetName("joint_showcase");
    const AssetID cube = res.meshes.Cube();
    const AssetID sphere = res.meshes.Sphere();
    const AssetID wheelMesh = AssetID{ HashStr("jdemo_wheel") };
    const AssetID matSteel = FindPhysMat("steel");
    const AssetID matWood = FindPhysMat("wood");

    // 部品を組む定型。**halfExtents は 0.5 固定でワールドスケールに効かせる** —
    // 物理ショーケース (M59d) と同じ流儀
    auto makeBox = [&](const char* name, float x, float y, float z, float sx, float sy, float sz,
                       const char* mat) {
        GameObject go = s.CreateGameObject(name);
        go.SetLocalPosition(x, y, z);
        go.SetLocalScale(sx, sy, sz);
        auto* mr = go.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr(mat) };
        return go;
    };
    auto addBoxCollider = [&](GameObject go, float hx, float hy, float hz, AssetID physMat) {
        auto* col = go.AddComponent<ColliderComponent>();
        col->shape = 1;
        col->isTrigger = false;
        col->halfExtents = { hx, hy, hz };
        col->physMaterial = physMat;
        return col;
    };
    auto addSphere = [&](const char* name, float x, float y, float z, float sc, const char* mat) {
        GameObject go = s.CreateGameObject(name);
        go.SetLocalPosition(x, y, z);
        go.SetLocalScale(sc, sc, sc);
        auto* mr = go.AddComponent<MeshRendererComponent>();
        mr->mesh = sphere;
        mr->material = AssetID{ HashStr(mat) };
        auto* col = go.AddComponent<ColliderComponent>();
        col->shape = 0;
        col->isTrigger = false;
        col->radius = 0.5f;
        col->physMaterial = matSteel;
        return go;
    };
    auto addBody = [&](GameObject go, float mass) {
        auto* rb = go.AddComponent<RigidbodyComponent>();
        rb->mass = mass;
        return rb;
    };
    auto addJoint = [&](GameObject go, int32_t type, EntityID other) {
        auto* j = go.AddComponent<JointComponent>();
        j->type = type;
        j->connectedEntity = other;
        return j;
    };

    GameObject camera = s.CreateGameObject("Main Camera");
    camera.AddComponent<CameraComponent>();
    // 展示 11 種 (x = -24〜28) と車のレーン (z = -16) が 1 枚に入る画角。
    // fovY 60 度 / 16:9 なら水平画角は約 91 度 = 距離 30m で幅 61m。
    // ★右端の粘着展示 (x = 28) は画面端から 1 割ほど内側に収まる — golden スクショの
    //   被写体なので、これ以上右へ展示を足すときは撮り直して framing を確かめること
    camera.SetLocalPosition(0.0f, 9.5f, -30.0f);
    camera.SetLocalRotationEuler(11.0f, 0.0f, 0.0f);

    GameObject sun = s.CreateGameObject("Sun");
    sun.AddComponent<LightComponent>();
    sun.SetLocalRotationEuler(52.0f, -28.0f, 0.0f);

    // ---- 物理環境 ----
    // ★**substeps は 16 (上限)**。ラグドールが要求する — 4 でも 8 でも「床に触れながら
    //   関節に吊られている」骨が微振動を続け、島は全員が静まるまで誰も眠らないので
    //   ラグドール全体が一生眠らない (M60g2 の申し送り 2 の実測)。車両が推奨する 8 も
    //   これで満たす。「剛性はサブステップで買える」(M59g2-7) の一番大きな請求書
    {
        GameObject envGo = s.CreateGameObject("Environment");
        auto* env = envGo.AddComponent<PhysicsEnvironmentComponent>();
        env->gravity = { 0.0f, -9.81f, 0.0f };
        env->substeps = 16;
    }

    // ---- 地面 ----
    // 車が 10 秒走っても場外へ出ない広さ (M60h2 の申し送り 7: 80m 角では全開加速で
    // 端から落ちた)。摩擦は乾いた舗装を明示する — 既定 0.5 のままだとタイヤが滑る
    {
        GameObject ground =
            makeBox("Ground", 0.0f, -0.5f, 0.0f, 220.0f, 1.0f, 220.0f, "jdemo_ground");
        addBoxCollider(ground, 0.5f, 0.5f, 0.5f, AssetID{})->friction = 1.0f;
    }

    // ---- 1. 二重振り子 (Ball × 2) ----
    // 関節が「点で繋ぐ」ことだけを主張する被写体。★軌道が初期値に鋭敏なので、
    // **Debug と Release が 600 tick ビット一致することの主張が一番強く出る**
    {
        makeBox("PendulumPost", -24.0f, 4.0f, 0.0f, 0.3f, 8.0f, 0.3f, "jdemo_frame");
        GameObject rod =
            makeBox("PendulumRod", -24.0f, 6.5f, 0.0f, 0.16f, 3.0f, 0.16f, "jdemo_swing");
        addBoxCollider(rod, 0.5f, 0.5f, 0.5f, matSteel);
        auto* rrb = addBody(rod, 2.0f);
        rrb->velocity = { 3.0f, 0.0f, 0.0f }; // 吊った直後に横へ蹴る
        rrb->angularDamping = 0.0f;
        auto* rj = addJoint(rod, 0, kNullEntity);
        rj->anchor = { 0.0f, 0.5f, 0.0f };            // ローカル → スケールが掛かって +1.5m
        rj->connectedAnchor = { -24.0f, 8.0f, 0.0f }; // 相手が null のときだけワールド

        GameObject bob = addSphere("PendulumBob", -24.0f, 4.4f, 0.0f, 0.8f, "jdemo_swing");
        auto* brb = addBody(bob, 6.0f);
        brb->velocity = { 3.0f, 0.0f, 0.0f };
        brb->angularDamping = 0.0f;
        auto* bj = addJoint(bob, 0, rod.Id());
        bj->anchor = { 0.0f, 0.75f, 0.0f };          // +0.6m (球の外 = 吊り点)
        bj->connectedAnchor = { 0.0f, -0.5f, 0.0f }; // 棒の下端 (相手のローカル)
    }

    // ---- 2. ロープ (Ball 10 連鎖 + 錘) ----
    // ★**コライダーは見た目どおり** (halfExtents 0.5)。M60i では「関節で繋がった相手と
    //   食い込むと永久に押し合う」のを 0.4 へ縮めて幾何で逃げていたが、M60j の
    //   `Joint.disableCollision` が入ったので**繋がったペアだけ候補から落とす**形へ移した。
    //   ★これが `disableCollision` の replay / golden 被覆でもある (M60k まで selftest しか
    //     踏んでいなかった)。切れるのは直接繋がった 1 ペアだけなので、1 つ飛ばしの鎖どうしは
    //     当たったまま = 「伝播しない」ことも 600 tick のハッシュに載る
    {
        constexpr int kLinks = 10;
        makeBox("RopePost", -19.0f, 4.0f, 0.0f, 0.3f, 8.0f, 0.3f, "jdemo_frame");
        EntityID prev = kNullEntity;
        for (int i = 0; i < kLinks; ++i) {
            char name[32];
            std::snprintf(name, sizeof(name), "RopeLink_%d", i);
            const float y = 7.75f - 0.5f * static_cast<float>(i);
            GameObject link = makeBox(name, -19.0f, y, 0.0f, 0.18f, 0.5f, 0.18f, "jdemo_swing");
            addBoxCollider(link, 0.5f, 0.5f, 0.5f, matWood);
            auto* rb = addBody(link, 0.5f);
            rb->velocity = { 2.5f, 0.0f, 0.0f }; // 鎖ごと横へ振り出す
            auto* j = addJoint(link, 0, prev);
            j->disableCollision = true; // 1 つ上の鎖 (先頭は天井) との接触を切る (M60j)
            j->anchor = { 0.0f, 0.5f, 0.0f };
            if (prev.IsNull()) {
                j->connectedAnchor = { -19.0f, 8.0f, 0.0f }; // 天井へ
            } else {
                j->connectedAnchor = { 0.0f, -0.5f, 0.0f }; // 1 つ上の鎖の下端
            }
            prev = link.Id();
        }
        GameObject weight = addSphere("RopeWeight", -19.0f, 2.4f, 0.0f, 0.9f, "jdemo_crate");
        auto* rb = addBody(weight, 12.0f); // 鎖を張らせる錘 (張力が関節に効く)
        rb->velocity = { 2.5f, 0.0f, 0.0f };
        auto* j = addJoint(weight, 0, prev);
        j->disableCollision = true; // 錘と最下段の鎖 (M60j)
        j->anchor = { 0.0f, 0.6667f, 0.0f };
        j->connectedAnchor = { 0.0f, -0.5f, 0.0f };
    }

    // ---- 3. ドア (Hinge + 角度リミット) ----
    // 蝶番は板の -X 辺。開く向きへ蹴り出し、-100 度で止まって戻る =
    // **リミットが片側不等式であること**が絵にも .rep にも出る
    {
        makeBox("DoorPost", -15.0f, 1.7f, 0.0f, 0.25f, 3.4f, 0.25f, "jdemo_frame");
        GameObject door = makeBox("Door", -14.0f, 1.7f, 0.0f, 2.0f, 3.0f, 0.16f, "jdemo_door");
        addBoxCollider(door, 0.5f, 0.5f, 0.5f, matWood);
        addBody(door, 12.0f)->angularVelocity = { 0.0f, -3.0f, 0.0f };
        auto* j = addJoint(door, 1, kNullEntity);
        j->axis = { 0.0f, 1.0f, 0.0f };
        j->anchor = { -0.5f, 0.0f, 0.0f }; // 板の -X 辺 (スケールが掛かって -1.0m)
        j->connectedAnchor = { -15.0f, 1.7f, 0.0f };
        j->useLimit = true;
        j->limitMin = -100.0f;
        j->limitMax = 4.0f;
    }

    // ---- 4. モータ駆動のクランク (Hinge + モータ) ----
    // 端で吊った腕を回し続ける。**重力が 1 周ごとに符号を変える負荷**なので、
    // モータの上限が「力ではなく力積 (maxForce·h)」として効いているかが挙動に出る
    {
        makeBox("CrankPost", -9.2f, 1.6f, 0.0f, 0.3f, 3.2f, 0.3f, "jdemo_frame");
        GameObject arm = makeBox("CrankArm", -8.0f, 3.2f, 0.0f, 2.4f, 0.25f, 0.25f, "jdemo_motor");
        addBoxCollider(arm, 0.5f, 0.5f, 0.5f, matSteel);
        addBody(arm, 3.0f)->angularDamping = 0.0f;
        auto* j = addJoint(arm, 1, kNullEntity);
        j->axis = { 0.0f, 0.0f, 1.0f }; // XY 平面で回る = カメラから回転が見える
        j->anchor = { -0.5f, 0.0f, 0.0f };
        j->connectedAnchor = { -9.2f, 3.2f, 0.0f };
        j->motorTargetVelocity = 2.5f; // rad/s
        j->motorMaxForce = 250.0f;     // N·m (自重の 35 N·m を十分に超える)
    }

    // ---- 5. エレベータ (Slider + 変位リミット + 線形モータ) ----
    // 荷物を載せた台がレールを上がり、上限に貼り付いて保持する
    {
        makeBox("ElevatorRailL", -5.1f, 2.5f, 0.0f, 0.15f, 5.0f, 0.15f, "jdemo_frame");
        makeBox("ElevatorRailR", -2.9f, 2.5f, 0.0f, 0.15f, 5.0f, 0.15f, "jdemo_frame");
        GameObject plat =
            makeBox("ElevatorPlate", -4.0f, 1.0f, 0.0f, 1.8f, 0.25f, 1.8f, "jdemo_motor");
        addBoxCollider(plat, 0.5f, 0.5f, 0.5f, matSteel);
        addBody(plat, 20.0f);
        auto* j = addJoint(plat, 3, kNullEntity);
        j->axis = { 0.0f, 1.0f, 0.0f };
        j->connectedAnchor = { -4.0f, 1.0f, 0.0f }; // ここが変位 0
        j->useLimit = true;
        j->limitMin = 0.0f;
        j->limitMax = 4.0f;
        j->motorTargetVelocity = 1.2f; // m/s
        j->motorMaxForce = 4000.0f;    // N (台 + 荷物の 255 N を持ち上げる)

        GameObject cargo =
            makeBox("ElevatorCargo", -4.0f, 1.65f, 0.0f, 0.9f, 0.9f, 0.9f, "jdemo_crate");
        addBoxCollider(cargo, 0.5f, 0.5f, 0.5f, matWood);
        addBody(cargo, 6.0f); // 拘束されていない = 摩擦だけで乗っている
    }

    // ---- 6. 固定 (Fixed) の片持ち ----
    // ワールドへ溶接したブラケット + そこへ溶接した腕 + 腕の先に吊った錘。
    // **固定関節が曲げモーメントを受け止めていること**が「腕が垂れない」で分かる
    {
        GameObject bracket =
            makeBox("WeldBracket", 0.0f, 3.2f, 0.0f, 0.6f, 0.6f, 0.6f, "jdemo_weld");
        addBoxCollider(bracket, 0.5f, 0.5f, 0.5f, matSteel);
        addBody(bracket, 2.0f);
        addJoint(bracket, 2, kNullEntity)->connectedAnchor = { 0.0f, 3.2f, 0.0f };

        GameObject arm = makeBox("WeldArm", 1.2f, 3.2f, 0.0f, 1.8f, 0.3f, 0.3f, "jdemo_weld");
        // ★M60i では 0.45 へ縮めて幾何で逃げていた (溶接で密着している相手と食い込むと
        //   接触ソルバが毎 tick 押し返して静止しない)。M60j 以降は見た目どおりの 0.5 で
        //   置き、繋がったペアだけ `disableCollision` で落とす
        addBoxCollider(arm, 0.5f, 0.5f, 0.5f, matSteel);
        addBody(arm, 3.0f);
        auto* aj = addJoint(arm, 2, bracket.Id());
        aj->disableCollision = true; // ブラケットと腕 (M60j)
        aj->anchor = { -0.5f, 0.0f, 0.0f };         // 腕の -X 端 (-0.9m)
        aj->connectedAnchor = { 0.5f, 0.0f, 0.0f }; // ブラケットの +X 面 (+0.3m)

        GameObject weight = addSphere("WeldWeight", 1.92f, 2.41f, 0.0f, 0.8f, "jdemo_crate");
        addBody(weight, 15.0f);
        auto* wj = addJoint(weight, 0, arm.Id());
        wj->disableCollision = true; // 腕と錘 (M60j)
        wj->anchor = { 0.0f, 0.8f, 0.0f };
        wj->connectedAnchor = { 0.4f, -0.5f, 0.0f };
    }

    // ---- 7. 破断する桟橋 (Hinge + ほぼ剛のリミット + breakTorque) ----
    // 塔から片持ちで張り出した甲板を、狭いリミットで水平に保つ。そこへ荷物を落とすと
    // 蝶番が受け止める反力が閾値を超えて**関節が折れ、甲板ごと落ちる**。
    // ★数えるのは等式行とリミット行 (= 反力) だけで、モータ行は数えない (M60d-3)
    {
        GameObject tower = makeBox("PierTower", 5.0f, 1.6f, 0.0f, 0.9f, 3.2f, 2.4f, "jdemo_frame");
        addBoxCollider(tower, 0.5f, 0.5f, 0.5f, matSteel);

        GameObject deck = makeBox("PierDeck", 8.0f, 3.4f, 0.0f, 5.0f, 0.3f, 2.2f, "jdemo_plank");
        addBoxCollider(deck, 0.5f, 0.5f, 0.5f, matWood);
        addBody(deck, 25.0f);
        auto* j = addJoint(deck, 1, kNullEntity);
        j->axis = { 0.0f, 0.0f, 1.0f };
        j->anchor = { -0.5f, 0.0f, 0.0f }; // 甲板の -X 端 (-2.5m) = 塔の側
        j->connectedAnchor = { 5.5f, 3.4f, 0.0f };
        j->useLimit = true;
        j->limitMin = -1.5f; // ほぼ剛。自重の 613 N·m はこのリミット行が受ける
        j->limitMax = 1.5f;
        j->breakForce = 1500.0f;  // N   (自重 245 N)
        j->breakTorque = 1000.0f; // N·m (自重 613 N·m / 荷物が乗ると 2800 N·m)

        GameObject crate = makeBox("PierCrate", 9.6f, 9.0f, 0.0f, 1.3f, 1.3f, 1.3f, "jdemo_crate");
        addBoxCollider(crate, 0.5f, 0.5f, 0.5f, matSteel);
        addBody(crate, 70.0f);
    }

    // ---- 8. 複合コライダー (L 字) ----
    // 親は形状を持たず、**子形状だけで形になっているボディ** (M60e で `ownShape` を切る
    // 原因になった経路)。慣性が 3x3 フルテンソルで合成されるので、非対称な L 字を
    // 放り投げると軸が寝る (対角近似では出ない挙動)
    {
        GameObject root = s.CreateGameObject("CompoundL");
        root.SetLocalPosition(13.0f, 4.5f, 0.0f);
        auto* rb = root.AddComponent<RigidbodyComponent>();
        rb->mass = 8.0f;
        rb->compoundColliders = true;
        rb->angularVelocity = { 1.5f, 0.0f, 2.0f };
        rb->angularDamping = 0.0f;
        GameObject a = makeBox("CompoundL_A", 0.0f, 0.0f, 0.0f, 2.0f, 0.4f, 0.8f, "jdemo_weld");
        a.SetParent(root);
        a.SetLocalPosition(0.0f, 0.0f, 0.0f);
        addBoxCollider(a, 0.5f, 0.5f, 0.5f, matSteel);
        GameObject b = makeBox("CompoundL_B", 0.0f, 0.0f, 0.0f, 0.4f, 2.0f, 0.8f, "jdemo_weld");
        b.SetParent(root);
        b.SetLocalPosition(0.8f, 0.8f, 0.0f);
        addBoxCollider(b, 0.5f, 0.5f, 0.5f, matSteel);
    }

    // ---- 9. 凸多面体の山 (Collider.shape = 5) ----
    // ★**見た目のメッシュと凸包の元メッシュを同じ AssetID にしてある** — 絵と当たりが
    //   ずれないので「凸包が壊れた」が目で分かる。
    // ★1 個だけモデル由来 (.glb) を混ぜてあるのは **`.mcvx` クックを replay に載せるため**。
    //   builtin メッシュは登録名に '#' を持たないのでその場生成になり、クック経路が一度も
    //   踏まれない。モデル由来なら「1 回目で焼いて 2 回目以降は読む」が 600 tick の
    //   ハッシュ照合そのものになる (= キャッシュの有無でワールドハッシュが変わらない証明。
    //   Debug と Release は cooked ディレクトリが別なので、両者が独立に焼いて一致する)
    {
        auto makeHull = [&](const char* name, AssetID mesh, float x, float y, float z, float sc,
                            float mass) {
            GameObject go = s.CreateGameObject(name);
            go.SetLocalPosition(x, y, z);
            go.SetLocalScale(sc, sc, sc);
            auto* mr = go.AddComponent<MeshRendererComponent>();
            mr->mesh = mesh;
            mr->material = AssetID{ HashStr("jdemo_hull") };
            auto* col = go.AddComponent<ColliderComponent>();
            col->shape = 5;
            col->isTrigger = false;
            col->meshAsset = mesh; // 凸包の素材 = 見た目と同じメッシュ
            col->physMaterial = matWood;
            addBody(go, mass);
            return go;
        };
        makeHull("Hull_Cube0", cube, 17.0f, 1.2f, -0.6f, 0.9f, 3.0f);
        makeHull("Hull_Cube1", cube, 18.4f, 1.2f, 0.4f, 0.9f, 3.0f);
        makeHull("Hull_Cube2", cube, 17.6f, 2.6f, -0.1f, 0.9f, 3.0f);
        makeHull("Hull_Ball0", sphere, 19.2f, 1.4f, -0.8f, 1.0f, 2.0f);
        makeHull("Hull_Ball1", sphere, 18.0f, 4.0f, 0.6f, 1.0f, 2.0f);
        // モデル由来 (クック経路)。ロードに失敗したらこの 1 個が欠けるだけ
        GameObject model =
            ModelLoader::Load(s, res, *ctx.shaders, ctx.assetsRoot + L"\\models\\BoxTextured.glb");
        if (model) {
            w.ApplyStructuralChanges();
            EntityID meshEntity = kNullEntity;
            AssetID meshId{};
            std::function<void(EntityID)> visit = [&](EntityID e) {
                if (!meshEntity.IsNull()) {
                    return;
                }
                if (auto* mr = w.GetComponent<MeshRendererComponent>(e)) {
                    meshEntity = e;
                    meshId = mr->mesh;
                    return;
                }
                auto* h = w.GetComponent<HierarchyComponent>(e);
                for (EntityID c = h ? h->firstChild : kNullEntity;;) {
                    if (c.IsNull()) {
                        break;
                    }
                    auto* ch = w.GetComponent<HierarchyComponent>(c);
                    const EntityID next = ch ? ch->nextSibling : kNullEntity;
                    visit(c);
                    c = next;
                }
            };
            visit(model.Id());
            if (!meshEntity.IsNull() && !meshId.IsNull()) {
                model.SetLocalPosition(16.6f, 5.6f, 0.2f);
                GameObject body(&w, meshEntity);
                auto* col = body.AddComponent<ColliderComponent>();
                col->shape = 5;
                col->isTrigger = false;
                col->meshAsset = meshId;
                col->physMaterial = matWood;
                addBody(body, 4.0f);
            } else {
                MYE_LOG_WARN("[jdemo] BoxTextured.glb has no MeshRenderer - the cooked convex "
                             "hull path is not covered by this scene");
            }
        }
    }

    // ---- 10. ラグドール (M60g1 の逆駆動 + M60g2 の生成器) ----
    // ★**生成器をそのまま呼ぶ** — 手で組むと g2 が積み上げた寸法の決め方 (2 段カプセル /
    //   restRotation / 短すぎる骨の足切り) が 2 箇所に散る。生成器を Engine 層へ移したのは
    //   このため (M60i で `src\Editor\` から移動。Runtime も同じシーンを組めるようになる)
    {
        GameObject actor =
            ModelLoader::Load(s, res, *ctx.shaders, ctx.assetsRoot + L"\\models\\CesiumMan.glb");
        if (!actor) {
            MYE_LOG_ERROR("[jdemo] CesiumMan.glb could not be loaded - no ragdoll in the scene");
        } else {
            actor.SetLocalPosition(22.0f, 1.4f, 0.0f);
            actor.SetLocalRotationEuler(0.0f, 0.0f, 25.0f); // 傾けて置く = 落ちて転ぶ
            w.ApplyStructuralChanges();
            EntityID skinned = kNullEntity;
            std::function<void(EntityID)> visit = [&](EntityID e) {
                if (!skinned.IsNull()) {
                    return;
                }
                if (w.GetComponent<SkinnedMeshComponent>(e)) {
                    skinned = e;
                    return;
                }
                auto* h = w.GetComponent<HierarchyComponent>(e);
                for (EntityID c = h ? h->firstChild : kNullEntity;;) {
                    if (c.IsNull()) {
                        break;
                    }
                    auto* ch = w.GetComponent<HierarchyComponent>(c);
                    const EntityID next = ch ? ch->nextSibling : kNullEntity;
                    visit(c);
                    c = next;
                }
            };
            visit(actor.Id());
            const SkinnedMeshComponent* sm =
                skinned.IsNull() ? nullptr : w.GetComponent<SkinnedMeshComponent>(skinned);
            const SkinnedModel* model = sm ? res.skinnedModels.Get(sm->model) : nullptr;
            if (!model || ragdoll_build::Build(s, skinned, *model) <= 0) {
                MYE_LOG_ERROR("[jdemo] ragdoll could not be built from CesiumMan.glb");
            } else {
                w.ApplyStructuralChanges();
                if (auto* rag = w.GetComponent<RagdollComponent>(skinned)) {
                    rag->active = true; // 最初から物理駆動 (アニメは骨を触らない)
                }
            }
        }
    }

    // ---- 11. 粘着 (M60d の adhesion) ----
    // ★**同じ糊の天井から、軽い箱はぶら下がったまま / 重い箱は落ちる**。粘着は
    //   「このペアが支えられる引っ張り力 [N]」なので (面積ではなく力)、質量を変えるだけで
    //   保持と落下が分かれる — それが絵に出るように 2 個並べてある。
    //   glue.physmat.json は adhesion 60 N。4kg = 39.2 N は保ち、14kg = 137.3 N は落ちる。
    // ★**天井と箱の両方に糊を割り当てる**。結合則は min なので、片方が未割当 (= 0) だと
    //   ペアの粘着は 0 になる。
    // ★箱は天井へ 10mm 食い込ませて置く — 粘着は「接触があるとき法線インパルスの下限を
    //   負へ開く」だけなので、離れて置くと接触が生まれず、ただ落ちる。
    // ★これが `.physmat` の adhesion の replay / golden 被覆でもある (M60k まで selftest しか
    //   踏んでいなかった。版管理された .physmat を 1 種足したのはこのため)
    {
        const AssetID matGlue = FindPhysMat("glue");
        makeBox("GluePost", 24.9f, 2.4f, 0.0f, 0.3f, 4.8f, 0.3f, "jdemo_frame");
        GameObject ceiling =
            makeBox("GlueCeiling", 26.4f, 4.65f, 0.0f, 3.2f, 0.3f, 1.4f, "jdemo_glue");
        addBoxCollider(ceiling, 0.5f, 0.5f, 0.5f, matGlue); // 静的 (Rigidbody を付けない)

        // 天井の下面は y = 4.5。箱の上面をそこから 10mm 食い込ませる
        auto glued = [&](const char* name, float x, float size, float mass) {
            const float half = 0.5f * size;
            GameObject go = makeBox(name, x, 4.5f - half + 0.01f, 0.0f, size, size, size,
                                    "jdemo_crate");
            addBoxCollider(go, 0.5f, 0.5f, 0.5f, matGlue);
            addBody(go, mass);
            return go;
        };
        glued("GlueBoxLight", 25.6f, 0.8f, 4.0f);  //  39.2 N < 60 N → ぶら下がったまま
        glued("GlueBoxHeavy", 27.4f, 1.0f, 14.0f); // 137.3 N > 60 N → 剥がれて落ちる
    }

    // ---- 12. 車 (Vehicle + Wheel 4 本 + C++ スクリプトの運転入力) ----
    // ★**車体は無スケール**。子の LocalPosition には親のスケールが掛かるので、車体に
    //   見た目のスケールを入れると車輪の取り付け位置まで伸びる。見た目は子の箱に持たせる。
    // ★車輪エンティティも**無回転・スケール 1** — サスのレイ方向 (ローカル -Y) と
    //   前方向 (ローカル +Z) がそのまま姿勢から読まれるため。回転角・切れ角・サスの
    //   伸縮の見た目は描画側 (RenderSystem) が出力フィールドから作る =
    //   **ハッシュ対象を 1 バイトも増やさない**
    {
        constexpr float kHalfY = 0.25f;
        constexpr float kRadius = 0.35f;
        constexpr float kRest = 0.4f;
        GameObject chassis = s.CreateGameObject("Car");
        // ★手前のレーン (z = -16) を左から右へ走らせる。S 字の第 1 旋回は +Z 側へ
        //   膨らむので (実測)、展示の列 (z = 0) との間に 8m の余裕を取ってある
        chassis.SetLocalPosition(-14.0f, kRadius + kRest + kHalfY, -16.0f);
        chassis.SetLocalRotationEuler(0.0f, 90.0f, 0.0f); // 前 (+Z) を +X へ向ける
        auto* ccol = chassis.AddComponent<ColliderComponent>();
        ccol->shape = 1;
        ccol->isTrigger = false;
        ccol->halfExtents = { 0.9f, kHalfY, 1.8f };
        ccol->friction = 1.0f;
        // 重心を床側へ下げる。レイキャストサスは横転を止める仕組みを持たないので、
        // 舵を切ったまま加速すると素の箱では簡単にひっくり返る
        addBody(chassis, 1200.0f)->centerOfMass = { 0.0f, -0.15f, 0.0f };
        auto* veh = chassis.AddComponent<VehicleComponent>();
        veh->motorForce = 3000.0f;
        veh->brakeForce = 6000.0f;
        veh->maxSteerAngleDeg = 30.0f;
        // 運転入力は sim 状態フィールド。スクリプトが SetComponentField で書く
        // (**車両のために ABI スロットを 1 本も足していない** = M60j を廃止した根拠)
        AttachScriptIfRegistered(w, chassis.Id(), "VehicleDemoDriver");

        GameObject shell = makeBox("CarBody", 0.0f, 0.0f, 0.0f, 1.8f, 0.5f, 3.6f, "jdemo_car");
        shell.SetParent(chassis);
        shell.SetLocalPosition(0.0f, 0.0f, 0.0f);
        GameObject cabin = makeBox("CarCabin", 0.0f, 0.0f, 0.0f, 1.5f, 0.6f, 1.6f, "jdemo_car");
        cabin.SetParent(chassis);
        cabin.SetLocalPosition(0.0f, 0.5f, -0.2f);

        static const float kWx[4] = { -0.8f, 0.8f, -0.8f, 0.8f };
        static const float kWz[4] = { 1.4f, 1.4f, -1.4f, -1.4f }; // 前 2 / 後 2
        for (int i = 0; i < 4; ++i) {
            char name[32];
            std::snprintf(name, sizeof(name), "CarWheel_%d", i);
            GameObject wheel = s.CreateGameObject(name);
            wheel.SetParent(chassis);
            wheel.SetLocalPosition(kWx[i], -kHalfY, kWz[i]);
            auto* mr = wheel.AddComponent<MeshRendererComponent>();
            mr->mesh = wheelMesh;
            mr->material = AssetID{ HashStr("jdemo_tire") };
            auto* wc = wheel.AddComponent<WheelComponent>();
            wc->restLength = kRest;
            wc->radius = kRadius;
            wc->stiffness = 60000.0f; // 1 輪 300kg → 沈み込み 0.049m
            wc->damping = 8000.0f;
            wc->steerFactor = (kWz[i] > 0.0f) ? 1.0f : 0.0f; // 前輪操舵
            wc->driveFactor = (kWz[i] > 0.0f) ? 0.0f : 1.0f; // 後輪駆動
            wc->brakeFactor = 1.0f;
        }
    }

    w.ApplyStructuralChanges();
}

void RegisterFogShowcaseContent(EngineContext& ctx)
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
    // 接頭辞は **fdemo_**。材質は全ショーケース分が無条件登録されるので、他と名前が
    // 被ると先に登録したほうが黙って上書きされる (M54a の申し送りと同じ配慮)
    // ★明るめに焼いてある。霧は「元の色をフォグ色へ寄せる」効果なので、素の色が暗いと
    //   掛かっているのかどうかが絵から読めない (最初に暗い材質で組んで失敗した)
    makeMat("fdemo_ground", 0.42f, 0.46f, 0.40f);
    makeMat("fdemo_pillar", 0.78f, 0.79f, 0.82f); // 距離帯の物差し (奥ほど霧に沈む)
}

// ---- M63a: パーティクル表現ショーケース (--particle-demo) ----
// 材質 2 つ + **手続き生成のテクスチャ 2 枚**。テクスチャをコードで焼くのは
// RegisterJointShowcaseContent の車輪メッシュと同じ流儀 (checkout 先に依存しない生成物)。
//
// ★テクスチャが要るのは被覆のため。既定の procedural ソフト円は**点対称**なので、
//   回転させても絵が 1 画素も変わらない = golden が C1 の回帰を検出できない。
//   ストレッチ (C2) も同じ理由で、向きの分かる絵でないと「伸びたか」しか読めない。
void RegisterParticleShowcaseContent(EngineContext& ctx)
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
    // 接頭辞は **vdemo_** (VFX)。★pdemo_ は --physics-demo が使用済み — 材質は全ショーケース
    // 分が無条件登録されるので、被ると先に登録したほうが黙って上書きされる (M54a の申し送り)
    makeMat("vdemo_ground", 0.34f, 0.36f, 0.40f);
    makeMat("vdemo_block", 0.72f, 0.70f, 0.66f); // 影を落とす箱 (C4 の被写体)

    // (1) 向きの分かるスプライト: +X を指す矢羽根。64x64 RGBA8。
    //     ★点対称にしないことだけが要件。回転角がそのまま絵の向きになるので、
    //       C1 が壊れると golden が必ず動く
    {
        constexpr int kN = 64;
        std::vector<uint8_t> px(static_cast<size_t>(kN) * kN * 4, 0);
        for (int y = 0; y < kN; ++y) {
            for (int x = 0; x < kN; ++x) {
                // ローカル座標 [-1,1]。+X が「前」、|v| が幅
                const float u = (static_cast<float>(x) + 0.5f) / kN * 2.0f - 1.0f;
                const float v = (static_cast<float>(y) + 0.5f) / kN * 2.0f - 1.0f;
                // 先端 (u=+1) で幅 0、尾 (u=-1) で最大幅の三角形。尾を少しえぐって矢羽根に
                const float halfWidth = 0.42f * (1.0f - u) * 0.5f;
                const float notch = 0.30f * (-u - 0.55f); // u<-0.55 でくびれる
                float a = 0.0f;
                if (u <= 1.0f && std::fabs(v) <= halfWidth && std::fabs(v) >= std::max(0.0f, notch)) {
                    // 縁を 1 画素ぶんだけ滑らかに (WARP でもエイリアスが暴れないように)
                    const float edge = (halfWidth - std::fabs(v)) * static_cast<float>(kN) * 0.5f;
                    a = std::clamp(edge, 0.0f, 1.0f);
                }
                const size_t o = (static_cast<size_t>(y) * kN + x) * 4;
                const uint8_t c = static_cast<uint8_t>(std::lround(a * 255.0f));
                px[o + 0] = c;
                px[o + 1] = c;
                px[o + 2] = c;
                px[o + 3] = c;
            }
        }
        res.textures.CreateFromRgba8("vdemo_arrow", px.data(), kN, kN, false, true);
    }

    // (2) フリップブックアトラス 4x4 (256x256)。コマ番号が絵から読めるように、
    //     リングの半径をコマごとに変え、切り欠きの角度もコマごとに回す。
    //     ★「隣り合うコマが十分に違う」ことが C3 (フレーム間ブレンド / ランダム開始) の
    //       被覆条件 — 似たコマだと補間しても開始をずらしても絵が変わらない
    {
        constexpr int kTiles = 4;
        constexpr int kTile = 64;
        constexpr int kN = kTiles * kTile;
        std::vector<uint8_t> px(static_cast<size_t>(kN) * kN * 4, 0);
        for (int ty = 0; ty < kTiles; ++ty) {
            for (int tx = 0; tx < kTiles; ++tx) {
                const int frame = ty * kTiles + tx; // 0..15
                const float t = static_cast<float>(frame) / 15.0f;
                const float radius = 0.25f + 0.65f * t;      // 広がるリング
                const float thick = 0.30f - 0.18f * t;       // 薄くなる
                const float notchAngle = t * 6.28318531f;    // 切り欠きが 1 周する
                for (int y = 0; y < kTile; ++y) {
                    for (int x = 0; x < kTile; ++x) {
                        const float u = (static_cast<float>(x) + 0.5f) / kTile * 2.0f - 1.0f;
                        const float v = (static_cast<float>(y) + 0.5f) / kTile * 2.0f - 1.0f;
                        const float r = std::sqrt(u * u + v * v);
                        float a = std::clamp(1.0f - std::fabs(r - radius) / thick, 0.0f, 1.0f);
                        // 切り欠き (コマごとに位置が回る = 隣接コマが必ず違う絵になる)
                        const float ang = std::atan2(v, u);
                        float d = std::fabs(ang - (notchAngle - 3.14159265f));
                        d = std::min(d, 6.28318531f - d);
                        if (d < 0.45f) {
                            a = 0.0f;
                        }
                        const size_t o =
                            (static_cast<size_t>(ty * kTile + y) * kN + (tx * kTile + x)) * 4;
                        const uint8_t c = static_cast<uint8_t>(std::lround(a * 255.0f));
                        px[o + 0] = c;
                        px[o + 1] = c;
                        px[o + 2] = c;
                        px[o + 3] = c;
                    }
                }
            }
        }
        // ★mips は作らない。4x4 アトラスは縮小時に隣のコマが滲み込む (タイル境界を
        //   跨ぐ mip はコマ同士を混ぜる) ので、フリップブックでは常に off が正しい
        res.textures.CreateFromRgba8("vdemo_flipbook", px.data(), kN, kN, false, false);
    }
}

// M63a: パーティクル表現ショーケース (--particle-demo)。golden 16/17 枚目の被写体。
//
// ★存在理由は被覆の穴埋め。M63 の 5 機能 (回転 / 速度ストレッチ / フリップブック /
//   ライティング / 深度衝突) は**全部既定 off** なので、絵に出さないと回帰検出がゼロになる。
//   既存デモに足すのでは駄目で、既定デモ (demo_forward/deferred = **CI 対象**) を動かすと
//   無関係な変更のレビューで毎回赤くなり、--fog-demo に足すと「フロクセル 2 分岐 ×
//   中間キー × VFX」の密な被覆を潰す (DemoContent.cpp の fog 節に明文の警告がある)。
// ★**CPU と GPU の 2 枚を撮る**のが要点。C1〜C3 は CPU インスタンス経路と GPU VS 経路の
//   2 実装を持つのに、既存 15 枚には両者を同じ被写体で突き合わせる golden が 1 枚も無い。
//   2 枚あれば「片方だけ直した」が必ず赤くなる。
void BuildParticleShowcaseScene(EngineContext& ctx)
{
    Scene& s = *ctx.scene;
    RenderResources& res = *ctx.resources;
    s.SetName("particle_showcase");
    const AssetID cube = res.meshes.Cube();
    // ★手続き生成テクスチャの ID は **HashStr(生成名)** (CreateFromRgba8 と同じ式)。
    //   IdForFile は NormalizePathKey を通すので**別の値**になる — 取り違えると
    //   テクスチャ未解決で procedural 円へ落ち、回転が絵に出ないまま golden が焼ける
    const AssetID arrowTex{ HashStr("vdemo_arrow") };
    const AssetID flipTex{ HashStr("vdemo_flipbook") };

    auto box = [&](const char* name, float x, float y, float z, float sx, float sy, float sz,
                   const char* mat) {
        GameObject go = s.CreateGameObject(name);
        go.SetLocalPosition(x, y, z);
        go.SetLocalScale(sx, sy, sz);
        auto* mr = go.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr(mat) };
        return go;
    };

    GameObject camera = s.CreateGameObject("Main Camera");
    camera.AddComponent<CameraComponent>();
    // 6 本のエミッタ (x = -16..+10) を 1 枚に収める。少し見下ろして床との衝突が読めるように。
    // M63d で左端に mode=1 のライティングエミッタが増えたが、**既存 5 本は 1 つも動かして
    // いない** (元から x_px 230 より左は空白だった) — 動かすと C1〜C3/C5 の被覆が
    // 「位置が変わっただけ」で全部赤くなり、レビューで本物の回帰が埋もれる
    camera.SetLocalPosition(0.5f, 3.6f, -15.0f);
    camera.SetLocalRotationEuler(8.0f, 0.0f, 0.0f);

    // 平行光 (C4 の CSM 影の光源)。仰角を寝かせて箱の影を長く伸ばす —
    // 「影の中を煙が通る」が絵で読めるのが M63d の見どころ
    GameObject sun = s.CreateGameObject("Sun");
    {
        auto* l = sun.AddComponent<LightComponent>();
        l->intensity = 1.6f;
        l->ambient = { 0.10f, 0.11f, 0.13f }; // 暗めのアンビエント = 受光の差が読める
        sun.SetLocalRotationEuler(28.0f, -25.0f, 0.0f);
    }

    box("Ground", 0.0f, -0.5f, 8.0f, 60.0f, 1.0f, 60.0f, "vdemo_ground");
    // C4 の影を作る箱。ライティングエミッタ (x=+4) の手前に置いて影を跨がせる
    box("ShadowCaster", 6.6f, 1.5f, 3.2f, 0.9f, 3.0f, 0.9f, "vdemo_block");
    // M63d: mode=1 側 (x=-16) にも**同じ相対位置**で箱を置く。emitter との差 (+2.0, z=3.2)
    // まで揃えてあるのは、2 つのライティングモードを**同じ幾何条件で並べる**ため —
    // 「粒子単位は粒子まるごと影に入る/出るのでパッと切り替わる」「画素単位は煙の中を
    // 影の角柱が通る」という違いだけが絵に残る
    box("ShadowCaster2", -14.0f, 1.5f, 3.2f, 0.9f, 3.0f, 0.9f, "vdemo_block");

    // 点光源 (C4)。**局所ライトが煙を照らす**のが「完全 unlit」を直したことの主張なので、
    // 平行光だけでは足りない。範囲は隣のエミッタへ漏れない程度に絞る
    GameObject lamp = s.CreateGameObject("Lamp");
    {
        auto* l = lamp.AddComponent<LightComponent>();
        l->type = 1; // Point
        l->color = { 1.0f, 0.62f, 0.28f };
        l->intensity = 6.0f;
        l->range = 5.0f;
        lamp.SetLocalPosition(4.6f, 1.3f, 5.0f);
    }

    // M63d: mode=1 側の点光源。**色も強度も範囲も Lamp と同一**にして、
    // 2 つのライティングモードの A/B が光源の違いに汚されないようにする
    // (エミッタとの相対位置 (同 x, +1.0 上, 1.0 手前) も揃えてある)
    GameObject lamp2 = s.CreateGameObject("Lamp2");
    {
        auto* l = lamp2.AddComponent<LightComponent>();
        l->type = 1; // Point
        l->color = { 1.0f, 0.62f, 0.28f };
        l->intensity = 6.0f;
        l->range = 5.0f;
        lamp2.SetLocalPosition(-16.0f, 1.3f, 5.0f);
    }

    // ---- (1) C1 回転: 矢羽根が粒子ごとに違う速さで回る ----
    // ★角速度を**非対称な範囲**にするのが要点。全部同じ速さだと KillDead の swap 漏れ
    //   (粒子が死ぬたびに隣へ他人の回転が飛び移る) が絵に出ない
    {
        GameObject go = s.CreateGameObject("P1_Rotation");
        go.SetLocalPosition(-9.5f, 0.3f, 6.0f);
        auto* e = go.AddComponent<ParticleEmitterComponent>();
        e->seed = 63001u;
        e->blendMode = 1; // alpha (向きが読めるように不透明寄り)
        e->rate = 15.0f;
        e->texture = arrowTex;
        e->shape = 2; // cone
        e->coneAngleDeg = 26.0f;
        e->speedMin = 1.1f;
        e->speedMax = 1.9f;
        e->lifetimeMin = 1.6f;
        e->lifetimeMax = 2.4f;
        e->sizeMin = 0.34f;
        e->sizeMax = 0.50f;
        e->gravity = { 0.0f, 0.35f, 0.0f };
        e->colorBegin = { 1.0f, 0.92f, 0.70f, 1.0f };
        e->colorEnd = { 0.9f, 0.45f, 0.20f, 0.0f };
        e->sizeEndScale = 0.9f;
        e->rotationMin = -3.14159f;   // 初期角はばらばら
        e->rotationMax = 3.14159f;
        e->rotationSpeedMin = -4.5f;  // 逆回転も混ぜる (符号規約の被覆)
        e->rotationSpeedMax = 6.0f;
    }

    // ---- (2) C2 速度ストレッチ: 火花が進行方向へ伸びる (実装は M63b) ----
    {
        GameObject go = s.CreateGameObject("P2_Stretch");
        go.SetLocalPosition(-4.8f, 0.3f, 6.0f);
        auto* e = go.AddComponent<ParticleEmitterComponent>();
        e->seed = 63002u;
        e->blendMode = 0; // additive (火花)
        e->rate = 90.0f;
        e->texture = arrowTex;
        e->shape = 2;
        e->coneAngleDeg = 42.0f;
        e->speedMin = 4.0f; // 速いほど伸びる = ストレッチが読める
        e->speedMax = 8.0f;
        e->lifetimeMin = 0.8f;
        e->lifetimeMax = 1.3f;
        e->sizeMin = 0.10f;
        e->sizeMax = 0.16f;
        e->gravity = { 0.0f, -5.0f, 0.0f }; // 放物線 = 速度の向きが刻々変わる
        e->colorBegin = { 1.0f, 0.85f, 0.45f, 1.0f };
        e->colorEnd = { 1.0f, 0.25f, 0.05f, 0.0f };
        e->stretchScale = 0.22f;
        e->stretchMax = 5.0f;
    }

    // ---- (3) C3 フリップブック: コマ送り + 補間 + 開始位相のばらつき (実装は M63c) ----
    {
        GameObject go = s.CreateGameObject("P3_Flipbook");
        go.SetLocalPosition(0.0f, 0.4f, 6.0f);
        auto* e = go.AddComponent<ParticleEmitterComponent>();
        e->seed = 63003u;
        e->blendMode = 1;
        e->rate = 14.0f; // 少なめ = 1 粒ずつのコマが読める
        e->texture = flipTex;
        e->flipTilesX = 4;
        e->flipTilesY = 4;
        e->flipCycles = 1.0f;
        e->flipFps = 11.0f;      // 寿命に依らない固定 fps
        e->flipBlend = 1;        // コマ間補間
        e->flipRandomStart = 1;  // ★同 tick 湧きが同じコマになる問題そのものの被覆
        e->shape = 1;            // sphere
        e->shapeRadius = 0.5f;
        e->speedMin = 0.4f;
        e->speedMax = 0.9f;
        e->lifetimeMin = 1.8f;
        e->lifetimeMax = 2.6f;
        e->sizeMin = 0.55f;
        e->sizeMax = 0.80f;
        e->gravity = { 0.0f, 0.5f, 0.0f };
        e->colorBegin = { 0.65f, 0.85f, 1.0f, 1.0f };
        e->colorEnd = { 0.30f, 0.55f, 0.95f, 0.0f };
        e->sizeEndScale = 1.2f;
    }

    // ---- (4) C4 ライティング: 点光源に照らされ、箱の影を通る煙 (実装は M63d) ----
    {
        GameObject go = s.CreateGameObject("P4_Lit");
        go.SetLocalPosition(4.6f, 0.3f, 6.0f);
        auto* e = go.AddComponent<ParticleEmitterComponent>();
        e->seed = 63004u;
        e->blendMode = 1; // alpha (加算だと受光の差が飽和して読めない)
        e->rate = 26.0f;
        e->shape = 1;
        e->shapeRadius = 0.45f;
        e->speedMin = 0.5f;
        e->speedMax = 1.1f;
        e->lifetimeMin = 2.2f;
        e->lifetimeMax = 3.0f;
        e->sizeMin = 0.55f;
        e->sizeMax = 0.85f;
        e->gravity = { -0.15f, 0.75f, 0.0f }; // 影の側へ流す
        // ★素の色は**白に近いグレー**。着色しておくと点光源の橙が乗ったか読めない
        e->colorBegin = { 0.82f, 0.84f, 0.86f, 0.60f };
        e->colorEnd = { 0.60f, 0.62f, 0.66f, 0.0f };
        e->sizeEndScale = 1.8f;
        e->lightingMode = 2; // 画素単位 (球面法線)
        e->lightWrap = 0.55f;
        e->lightIntensity = 1.0f;
        e->lightReceiveShadow = 1;
    }

    // ---- (4b) C4 ライティング mode=1: **粒子単位** (受光を VS で色へ畳む) ----
    // ★P4_Lit と **lightingMode 以外は 1 フィールドも違わない**。光源も箱も相対位置まで
    //   ミラーしてあるので、2 枚の絵の違いは「粒子単位か画素単位か」だけになる。
    // ★これが golden における mode=1 の**唯一の被覆**。VS ステージへの CB / CSM /
    //   比較サンプラのバインドを 1 本でも落とすと、CSM が未バインド SRV から 0 を返して
    //   dirShadow=0 になり**この 1 本だけ暗く沈む** — mode=2 側は無傷なので、
    //   並べてあることが検出そのものになっている
    {
        GameObject go = s.CreateGameObject("P4b_LitVertex");
        go.SetLocalPosition(-16.0f, 0.3f, 6.0f);
        auto* e = go.AddComponent<ParticleEmitterComponent>();
        e->seed = 63006u;
        e->blendMode = 1;
        e->rate = 26.0f;
        e->shape = 1;
        e->shapeRadius = 0.45f;
        e->speedMin = 0.5f;
        e->speedMax = 1.1f;
        e->lifetimeMin = 2.2f;
        e->lifetimeMax = 3.0f;
        e->sizeMin = 0.55f;
        e->sizeMax = 0.85f;
        e->gravity = { -0.15f, 0.75f, 0.0f };
        e->colorBegin = { 0.82f, 0.84f, 0.86f, 0.60f };
        e->colorEnd = { 0.60f, 0.62f, 0.66f, 0.0f };
        e->sizeEndScale = 1.8f;
        e->lightingMode = 1; // 粒子単位 (VS で畳む) ← ここだけが P4_Lit との差
        e->lightWrap = 0.55f;
        e->lightIntensity = 1.0f;
        e->lightReceiveShadow = 1;
    }

    // ---- (5) C5 深度衝突: 床で跳ねて滑る破片 (GPU 限定。実装は M63e) ----
    // ★CPU バックエンドでは衝突しない (spec 7.5 の例外) ので、**2 枚の golden が
    //   意図的に食い違う唯一の場所**。差が出ること自体が仕様の可視化になっている
    {
        GameObject go = s.CreateGameObject("P5_Collide");
        go.SetLocalPosition(10.0f, 3.4f, 6.0f);
        auto* e = go.AddComponent<ParticleEmitterComponent>();
        e->seed = 63005u;
        e->blendMode = 1;
        e->rate = 60.0f;
        e->shape = 3; // box
        e->boxExtents = { 0.9f, 0.05f, 0.9f };
        e->emitFrom = 1;
        e->speedMin = 0.2f;
        e->speedMax = 0.7f;
        e->lifetimeMin = 2.0f;
        e->lifetimeMax = 3.2f;
        e->sizeMin = 0.12f;
        e->sizeMax = 0.20f;
        e->gravity = { 0.0f, -7.0f, 0.0f }; // 床へ叩きつける
        e->colorBegin = { 0.55f, 0.95f, 0.75f, 0.95f };
        e->colorEnd = { 0.25f, 0.70f, 0.55f, 0.0f };
        e->depthCollision = 1;
        e->collisionBounce = 0.35f;
        e->collisionFriction = 0.4f;
        e->collisionLifeLoss = 0.12f;
        e->collisionFloor = 1;
        e->collisionFloorY = 0.0f;
    }
}

void BuildFogShowcaseScene(EngineContext& ctx)
{
    Scene& s = *ctx.scene;
    World& w = s.GetWorld();
    RenderResources& res = *ctx.resources;
    s.SetName("fog_showcase");
    const AssetID cube = res.meshes.Cube();

    auto box = [&](const char* name, float x, float y, float z, float sx, float sy, float sz,
                   const char* mat) {
        GameObject go = s.CreateGameObject(name);
        go.SetLocalPosition(x, y, z);
        go.SetLocalScale(sx, sy, sz);
        auto* mr = go.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr(mat) };
        return go;
    };

    GameObject camera = s.CreateGameObject("Main Camera");
    camera.AddComponent<CameraComponent>();
    // 手前 8m から奥 70m までを 1 枚に収める画角。**フロクセルのグリッド端 (既定 64m) が
    // 画の中に入っていること**が要点 — 受け持ちの切り替わりが絵に出ていないと、
    // 「解析フォグとフロクセルの分担」が壊れても golden が気づけない
    camera.SetLocalPosition(0.0f, 4.0f, -16.0f);
    camera.SetLocalRotationEuler(5.0f, 0.0f, 0.0f);

    GameObject sun = s.CreateGameObject("Sun");
    sun.AddComponent<LightComponent>();
    // ★太陽をカメラの正面側 (奥) へ振る — 太陽インスキャッタ (M43a) は「視線が太陽へ
    //   向くほどフォグ色が太陽色へ寄る」効果なので、背後から照らすと絵に 1 画素も出ない。
    //   VFX が ApplyFog へ寄ったこと (コミット②) を絵で見るための配置。
    //   仰角は地面が読める程度に上げてある (寝かせると全部逆光のシルエットになる)
    sun.SetLocalRotationEuler(42.0f, 6.0f, 0.0f);

    box("Ground", 0.0f, -0.5f, 25.0f, 120.0f, 1.0f, 200.0f, "fdemo_ground");

    // ---- 距離帯の物差し (グリッドの内と外を跨ぐ) ----
    // 10 / 25 / 45 はフロクセルのグリッド内、70 は外。**同じ形・同じ材質**を並べるので、
    // 霧の掛かり方の違いがそのまま距離の関数として読める
    const float pillarZ[] = { 10.0f, 25.0f, 45.0f, 70.0f };
    for (int i = 0; i < 4; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "Pillar_%d", i);
        const float x = (i % 2 == 0) ? -6.0f : 6.0f;
        box(name, x, 3.0f, pillarZ[i], 1.6f, 7.0f, 1.6f, "fdemo_pillar");
    }

    // ---- 霧 (--render-demo と同じ値) ----
    // 同じ設定にしてあるのは、あちらで確認済みの「メッシュがどう霧むか」を基準として
    // 使うため。粒子と VFX がこれと食い違っていないかが本ショーケースの見どころ
    {
        GameObject fog = s.CreateGameObject("Fog");
        auto* f = fog.AddComponent<FogComponent>();
        f->mode = 2; // Exp2
        f->color = { 0.11f, 0.13f, 0.17f, 1.0f };
        // ★density は --render-demo (0.011) より濃い 0.020。あちらは被写体が 44〜88m に
        //   あるが、こちらは粒子と VFX が 28〜38m の近距離に居るので、同じ濃度だと
        //   f が 0.12 程度にしかならず「霧が適用されているか」が絵から読めない。
        //   0.020 なら 32m で f≈0.34 / 61m で f≈0.78 / 86m で f≈0.95 と広い範囲に散る
        f->density = 0.020f;
        f->heightFalloff = 0.035f;
        f->baseHeight = 0.0f;
        f->inscatterIntensity = 0.30f;
        f->inscatterPower = 10.0f;
    }

    // ---- パーティクル: 加算と alpha を両方置く ----
    // ★フロクセル合成の 2 分岐 (加算は透過率だけ / alpha は scene·T + inscatter) を
    //   **両方とも絵に出す**のが目的。片方だけだと、取り違えても golden が気づけない
    {
        GameObject fire = s.CreateGameObject("Fire_Additive");
        fire.SetLocalPosition(-3.0f, 0.2f, 16.0f);
        auto* e = fire.AddComponent<ParticleEmitterComponent>();
        e->blendMode = 0; // additive
        e->rate = 120.0f;
        e->seed = 20250830u;
    }
    {
        GameObject smoke = s.CreateGameObject("Smoke_Alpha");
        smoke.SetLocalPosition(3.0f, 0.2f, 16.0f);
        auto* e = smoke.AddComponent<ParticleEmitterComponent>();
        e->blendMode = 1; // alpha
        e->rate = 90.0f;
        e->seed = 777u;
        e->colorBegin = { 0.75f, 0.78f, 0.82f, 0.85f };
        e->colorEnd = { 0.55f, 0.58f, 0.62f, 0.0f };
        e->sizeMin = 0.35f;
        e->sizeMax = 0.60f;
        e->speedMin = 1.0f;
        e->speedMax = 1.8f;
        // ★M42追補: **中間キーの CPU/GPU 一致をピクセルで担保するための被覆**。
        //   GPU バックエンドは長らく begin→end の 2 点線形しか持っておらず、中間キーを
        //   丸ごと無視していた。golden にこの 2 本が写っている限り、片方だけ直した瞬間に赤くなる。
        //   ★値は「begin→end の線形補間から**明確に外れる**」ように選ぶこと —
        //     線上に置くと中間キーを無視しても絵が変わらず、被覆にならない。
        //   立ち上がりで炎に照らされた暖色へ寄せ (alpha も線形より高い)、その後に冷えて散る絵。
        e->colorMid1 = { 0.92f, 0.60f, 0.34f, 0.72f };
        e->colorMidT1 = 0.25f;
        //   サイズは 1.0 → 1.4 → 0.25 (sizeEndScale の既定) の山なり。t=0.35 の 2 点線形は
        //   0.74 なので約 2 倍 = 中間キーを無視すれば一目で分かる差。★2.2 まで太らせると
        //   煙が TrailTip のリボンを半分隠し、M57追補 が作った VFX のピクセル被覆
        //   (シアン画素 151 → 83) を削ってしまう — 被覆を足すために別の被覆を潰さないこと
        e->sizeMidScale = 1.4f;
        e->sizeMidT = 0.35f;
    }

    // ---- VFX (Sprite / Trail / TextMesh) ----
    // ★**リポジトリで唯一この 3 種が写る被写体**。M57e まで golden に 1 枚も無かったので、
    //   VfxRenderer は壊れても 14 枚が全部緑のままだった
    // ★スプライト 2 枚は **「霧の量だけが違う 2 枚」になるように配置してある**:
    //   どちらもカメラの目線高さ (y=4) に置き、遠いほうは距離比 (68/28) だけ大きく焼く。
    //   こうすると 2 枚は画面上で同じ大きさ・左右対称の同じ高さに出るので、
    //   **見え方の差が霧の量そのもの**になる。片方だけ霧が抜けたら一目で分かる。
    //   ★寸法や z を動かすときは必ずこの比 (size ∝ 距離) を保つこと — 崩すと
    //     「大きさが違うから暗いのか、霧で暗いのか」が絵から切り分けられなくなる
    {
        GameObject spriteNear = s.CreateGameObject("Sprite_Near");
        spriteNear.SetLocalPosition(-10.0f, 4.0f, 12.0f); // カメラから 28m
        auto* sp = spriteNear.AddComponent<SpriteRendererComponent>();
        sp->color = { 1.0f, 0.55f, 0.25f, 1.0f };
        sp->size = { 2.0f, 2.0f };
    }
    {
        GameObject spriteFar = s.CreateGameObject("Sprite_Far");
        spriteFar.SetLocalPosition(24.29f, 4.0f, 52.0f); // カメラから 68m (28m の 2.43 倍)
        auto* sp = spriteFar.AddComponent<SpriteRendererComponent>();
        sp->color = { 1.0f, 0.55f, 0.25f, 1.0f };
        sp->size = { 4.857f, 4.857f }; // 2.0 * 2.43 = 画面上では近い板と同じ大きさ
    }
    {
        // ★Trail は「点が 2 つ以上溜まって初めて」リボンになる (TrailStore)。静止した
        //   エンティティでは 1 本も出ないので、Rotator (GameLogic.dll) で親を Y 軸回転させて
        //   子のワールド位置を動かす。**DLL が焼けていないとリボンが消えて golden が
        //   静かに変わる** (joints の VehicleDemoDriver と同じ依存)。
        //   Rotator は 30 deg/s なので、frame 120 (2.0 秒) までに 60 度ぶんの弧が溜まる
        GameObject spinner = s.CreateGameObject("Spinner");
        spinner.SetLocalPosition(0.0f, 3.2f, 20.0f);
        AttachScriptIfRegistered(w, spinner.Id(), "Rotator");
        GameObject tip = s.CreateGameObject("TrailTip");
        tip.SetParent(spinner);
        tip.SetLocalPosition(4.5f, 0.0f, 0.0f);
        auto* tr = tip.AddComponent<TrailRendererComponent>();
        tr->duration = 2.0f; // 撮影 frame (120 = 2.0 秒) と揃える = 弧が最長になる
        tr->width = 0.55f;
        tr->colorBegin = { 0.35f, 0.85f, 1.0f, 1.0f };
        tr->colorEnd = { 0.10f, 0.35f, 0.65f, 0.0f };
        tr->minVertexDistance = 0.05f;
    }
    {
        GameObject label = s.CreateGameObject("Label");
        label.SetLocalPosition(0.0f, 5.2f, 30.0f);
        auto* tm = label.AddComponent<TextMeshComponent>();
        std::snprintf(tm->text, sizeof(tm->text), "FOG");
        // fontScale 1.0 で行高 ≈ 0.3 ワールド単位。30m 先で読める大きさに留める —
        // 大きくしすぎると板が画面を覆って、奥の柱 (霧の物差し) が隠れる
        tm->fontScale = 6.0f;
        tm->color = { 0.95f, 0.95f, 0.80f, 1.0f };
    }

    w.ApplyStructuralChanges();
}

// ---- M65b: 音響ショーケース (--acoustic-demo) ----
// 接頭辞は **adem_**。材質は全ショーケース分が無条件登録されるので、他と名前が被ると
// 先に登録したほうが黙って上書きされる (M54a の申し送りと同じ配慮)。
// 既使用: rdemo_ / tdemo_ / pdemo_ / jdemo_ / fdemo_ / vdemo_
void RegisterAcousticShowcaseContent(EngineContext& ctx)
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
    // ★暗い。企画は「世界は真っ暗で、音の波だけが世界を描く」なので素の色は沈めてある。
    //   ただし**真っ黒にはしない** — M65e で golden を撮るときに全画素が黒だと
    //   機能が壊れて何も出なくても golden 一致で通る = 回帰検出がゼロになる
    //   (計画 M65e の最大の罠)。壁と床の輪郭が薄く読める程度に留めてある
    makeMat("adem_floor", 0.13f, 0.14f, 0.16f);
    makeMat("adem_wall", 0.20f, 0.20f, 0.23f);
    makeMat("adem_source", 0.85f, 0.72f, 0.30f); // 音源の目印 (黄)
    // ---- M65c: 床材タイル 6 種 ----
    // ★色は**音の大きさの順に明るく**してある。企画は「材質は事前には分からない」だが、
    //   デモは検証用なので「どのタイルで大きい波が出たか」が絵で読めることを優先する
    makeMat("adem_carpet", 0.26f, 0.15f, 0.14f);
    makeMat("adem_wood", 0.34f, 0.24f, 0.13f);
    makeMat("adem_gravel", 0.30f, 0.30f, 0.27f);
    makeMat("adem_water", 0.12f, 0.26f, 0.38f);
    makeMat("adem_metal", 0.46f, 0.48f, 0.52f);
    makeMat("adem_glass", 0.34f, 0.48f, 0.50f);
    makeMat("adem_drop", 0.62f, 0.30f, 0.28f); // 衝撃音の被写体 (落ちてくる箱)
    // ---- M65f: 敵 2 種 ----
    // ★色でセンサーが読めるようにしてある (赤 = 耳 / 緑 = 目)。同じ FSM を回していて
    //   違うのは載っているセンサーだけ、という設計が絵で確かめられる
    makeMat("adem_agent_ear", 0.58f, 0.20f, 0.22f);
    makeMat("adem_agent_eye", 0.22f, 0.52f, 0.26f);
    // ---- M65g: プレイヤーと道具 ----
    // ★設置光 (adem_lamp) だけ明るい。**スクリプトが強度を 0 から育てる**ので、
    //   球そのものが暗いと「置いている最中」が絵から読めない (企画 4-3 はゲージを
    //   出さないと決めているので、光の育ち方が唯一の進行表示になる)
    makeMat("adem_player", 0.30f, 0.44f, 0.58f);
    makeMat("adem_lamp", 0.95f, 0.86f, 0.62f);
    makeMat("adem_stone", 0.38f, 0.36f, 0.33f);
    makeMat("adem_bottle", 0.30f, 0.52f, 0.44f);
}

// ---- 音響ショーケースの間取り ----
// 12x12 タイル / 1 タイル = 2m → 24x24m。'#' = 壁 / '.' = 通れる。
// **L 字の廊下で 2 部屋を繋ぐ**のが唯一の要件 — 直線で見通せる配置にすると
// 「角を回り込んだ」ことが絵から読めない (シャドウマップでも同じ絵になってしまう)。
//   部屋 A (左上) → 横に伸びる廊下 (row 4) → 縦の廊下 (col 9) → 部屋 B (下)
// 音源は部屋 A に置く。部屋 B は**部屋 A から一直線には見えない**位置にある
static const char* const kAcousticMap[12] = {
    "############", // r0
    "#....#######", // r1  部屋 A (col 1..4)
    "#....#######", // r2
    "#....#######", // r3
    "#..........#", // r4  横の廊下 (col 1..10)
    "#########.##", // r5  縦の廊下 (col 9)
    "#########.##", // r6
    "##.........#", // r7  部屋 B (col 2..10)
    "##.........#", // r8
    "##.........#", // r9
    "##.........#", // r10
    "############", // r11
};
constexpr float kAcousticTile = 2.0f;
constexpr int kAcousticMapN = 12;

// マップ座標 -> ワールド。中心が原点に来るように寄せる
static float AcousticMapToWorld(int i)
{
    return (static_cast<float>(i) - (kAcousticMapN - 1) * 0.5f) * kAcousticTile;
}

// M65b: 音響ショーケース (--acoustic-demo)。
// ★**波が壁を貫通せず L 字を曲がる**ことを見せるためだけのシーン。
//   M65b 時点では絵は出ず、デバッグ線 (View > 音響) でしか見えない —
//   ライティングに差し込むのは M65e。それでも今サブで置くのは、replay 7 ペア目の
//   被写体 (= 波のハッシュ被覆) がここにしか無いため。
void BuildAcousticShowcaseScene(EngineContext& ctx)
{
    Scene& s = *ctx.scene;
    World& w = s.GetWorld();
    RenderResources& res = *ctx.resources;
    s.SetName("acoustic_showcase");
    const AssetID cube = res.meshes.Cube();

    auto box = [&](const char* name, float x, float y, float z, float sx, float sy, float sz,
                   const char* mat) {
        GameObject go = s.CreateGameObject(name);
        go.SetLocalPosition(x, y, z);
        go.SetLocalScale(sx, sy, sz);
        auto* mr = go.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr(mat) };
        auto* col = go.AddComponent<ColliderComponent>();
        col->shape = 1; // box。★これが占有ベイクの入力 = 音を遮る実体
        col->halfExtents = { 0.5f, 0.5f, 0.5f }; // スケールが掛かるので単位箱でよい
        return go;
    };

    GameObject camera = s.CreateGameObject("Main Camera");
    camera.AddComponent<CameraComponent>();
    // ★間取り全体が 1 枚に入る俯瞰。**L 字の 2 本の腕と 2 部屋が同時に見えること**が
    //   画角の唯一の要件 — 片方の腕が切れていると「回り込んだ」が絵から読めない
    // ★M65e で寄せた (26,-17 -> 20,-13)。golden をここで初めて撮るので、間取りが
    //   画面の中で小さいと**残光が壊れても差分画素が少なすぎて埋もれる**。
    //   2 本の腕と 2 部屋が入る限界まで寄せてある
    camera.SetLocalPosition(0.0f, 20.0f, -13.0f);
    camera.SetLocalRotationEuler(56.0f, 0.0f, 0.0f);

    // 弱い環境光。★企画は真っ暗だが、**壁の輪郭が読めない画は golden にならない**
    //   (何も出なくても一致してしまう)。輪郭だけが見える強さに落としてある
    GameObject sun = s.CreateGameObject("Sun");
    auto* sunLight = sun.AddComponent<LightComponent>();
    sunLight->intensity = 0.35f;
    sun.SetLocalRotationEuler(62.0f, 24.0f, 0.0f);

    box("Floor", 0.0f, -0.5f, 0.0f, 26.0f, 1.0f, 26.0f, "adem_floor");

    // ---- 壁 ----
    // 1 タイル = 1 箱。**連結して 1 枚の板にまとめない** — まとめると占有ベイクの
    // 入力が減って速くはなるが、間取りを変えたときに手で貼り直すことになる
    for (int r = 0; r < kAcousticMapN; ++r) {
        for (int c = 0; c < kAcousticMapN; ++c) {
            if (kAcousticMap[r][c] != '#') {
                continue;
            }
            char name[32];
            std::snprintf(name, sizeof(name), "Wall_%02d_%02d", r, c);
            box(name, AcousticMapToWorld(c), 1.5f, AcousticMapToWorld(r), kAcousticTile, 3.0f,
                kAcousticTile, "adem_wall");
        }
    }

    // ---- 音響ボリューム ----
    // 52x6x52 セル / 0.5m = 26x3x26m。間取り (24x24m) を水平に 1m ずつ包んでいる。
    // ★グリッド外は壁扱いなので、包みきれていないと端で波が消えて理由不明のバグに見える。
    // ★★**高さは壁より高くしてはいけない**。中心 1.75 / 6 セル = y[0.25, 3.25] は
    //   壁 (y[0,3]) が全 6 層を塞ぐ高さで、床 (y[-1,0]) はグリッドの外に落ちる。
    //   最初 8 セル (y[-0.5,3.5]) で組んだら**壁の上に 0.5m の隙間が開いて波が壁を
    //   飛び越え**、L 字を曲がらずに直接となりの部屋へ届いていた (実測: 到達セルが
    //   開セルのほぼ全部になり、企画の中核が絵から消えた)。
    //   高さ方向の包み込みは水平方向と逆で、**きつく取る**のが正しい
    {
        GameObject vol = s.CreateGameObject("Acoustic Volume");
        vol.SetLocalPosition(0.0f, 1.75f, 0.0f);
        auto* av = vol.AddComponent<AcousticVolumeComponent>();
        av->dimX = 52;
        av->dimY = 6;
        av->dimZ = 52;
        av->cellSize = 0.5f;
        av->navCellRatio = 2;
    }

    // ---- 音源 (部屋 A) ----
    // ★**生成順の末尾に置く**。デモへ物を足すときに前へ挿すと粒子 RNG のストリームが
    //   ずれる、という既知の罠と同じ理由で、末尾追加を習慣にしておく
    {
        GameObject src = s.CreateGameObject("Wave Source");
        src.SetLocalPosition(AcousticMapToWorld(2), 1.0f, AcousticMapToWorld(2));
        src.SetLocalScale(0.6f, 0.6f, 0.6f);
        auto* mr = src.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr("adem_source") };
        auto* em = src.AddComponent<AcousticEmitterComponent>();
        em->ticksPerRing = 2; // 0.5m / 2 tick = 15 m/s。部屋 A から部屋 B まで約 1.6 秒
        em->cooldownTicks = 30;
        // WavePinger (GameLogic.dll) が 150 tick ごとに pendingLoudness を書く。
        // ★DLL が焼けていないと**波が 1 本も出ない** — replay 7 ペア目のハッシュ被覆が
        //   丸ごと消えるので、joints の VehicleDemoDriver と同じ「DLL 依存の被写体」
        AttachScriptIfRegistered(w, src.Id(), "WavePinger");
    }

    // ---- 聴者 (部屋 B) ----
    // M65b では鏡が空のままだが、**デバッグ線の被写体**として先に置いてある。
    // M65f でここに「いつ・どこから聞こえたか」が入る
    {
        GameObject ear = s.CreateGameObject("Listener");
        // ★部屋 B の**入口寄り**に置く。奥に置くと L 字経路が 32m を超えて
        //   一度も届かず、M65f の聴覚 AI が無反応のまま「壊れていないのに動かない」になる
        ear.SetLocalPosition(AcousticMapToWorld(8), 1.0f, AcousticMapToWorld(8));
        ear.AddComponent<AcousticListenerComponent>();
    }

    // ---- M65c: 床材タイル (横の廊下 row 4 の col 2..7) ----
    // ★**厚み 0.6 / 中心 y=-0.25 = 天面 0.05**。数字の理由:
    //   (a) セル中心 (層 0 = y=0.5) には**届かない**こと — 届くと廊下の床が
    //       占有セルになり、M65b で測った「断面 24 セルの平面波」が変わる。
    //   (b) 床 (天面 y=0) との段差が **CC が登れる高さ**であること。
    //       ★★M65c は天面 0.45 で置いていたが、それは **M65f で敵が廊下へ入れない**
    //         原因だった。0.45m の段差は collide-and-slide では登れず、追跡中の敵が
    //         タイルの端で永久に足踏みする (probe で発見。歩行者はタイルの上で
    //         生まれるので一度も跨がず、絵にも出ていなかった)。5cm なら誰でも登れる。
    //   ★(a) の「天面が音響ボリュームの下端 (y=0.25) より上」は**タイルには要らない**。
    //     足音は歩行者の位置 (y=1.35) で鳴るので、タイルの高さは発音位置に無関係。
    //     この制約が効くのは接触点そのものが発音位置になる**衝撃の金属板だけ**で、
    //     あちらは天面 0.45 のまま据え置いてある。
    // ★6 枚を**隙間なく並べる**のも意図的 (端で段差を作らない)
    {
        static const char* const kTileMats[6] = { "carpet", "wood",  "gravel",
                                                  "water",  "metal", "glass" };
        static const char* const kTileVisuals[6] = { "adem_carpet", "adem_wood",  "adem_gravel",
                                                     "adem_water",  "adem_metal", "adem_glass" };
        for (int i = 0; i < 6; ++i) {
            const int col = 2 + i;
            char name[48];
            std::snprintf(name, sizeof(name), "Tile_%s", kTileMats[i]);
            GameObject t = s.CreateGameObject(name);
            t.SetLocalPosition(AcousticMapToWorld(col), -0.25f, AcousticMapToWorld(4));
            t.SetLocalScale(kAcousticTile, 0.6f, kAcousticTile);
            auto* mr = t.AddComponent<MeshRendererComponent>();
            mr->mesh = cube;
            mr->material = AssetID{ HashStr(kTileVisuals[i]) };
            auto* col2 = t.AddComponent<ColliderComponent>();
            col2->shape = 1;
            col2->halfExtents = { 0.5f, 0.5f, 0.5f };
            // ★materials 未登録なら AssetID{} = 未割当 = 無音に落ちるだけ (デモは壊れない)
            col2->physMaterial = FindPhysMat(kTileMats[i]);
        }
    }

    // ---- M65c: 歩行者 (足音の被写体) ----
    // ★CC 単体で組む (ソリッド Collider を併用しない) — 足元へ撃つレイが自分に当たると
    //   GroundMaterialUnder が無音を返す仕様なので、併用すると足音が 1 度も出なくなる
    {
        GameObject walker = s.CreateGameObject("Walker");
        // 天面 0.45 + カプセル半長 0.9 = 1.35。落として馴染ませるより初期値で載せる
        walker.SetLocalPosition(AcousticMapToWorld(2), 1.35f, AcousticMapToWorld(4));
        walker.SetLocalScale(0.6f, 1.0f, 0.6f);
        auto* mr = walker.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr("adem_source") };
        auto* cc = walker.AddComponent<CharacterControllerComponent>();
        cc->radius = 0.3f;
        cc->height = 1.8f;
        auto* em = walker.AddComponent<AcousticEmitterComponent>();
        em->autoFootstep = true;
        em->stepDistanceM = 0.9f; // 歩幅。2.0 m/s なら 27 tick に 1 歩
        em->ticksPerRing = 2;
        em->cooldownTicks = 12; // 金属の上で走っても波が詰まらない下限間隔
        AttachScriptIfRegistered(w, walker.Id(), "WaveWalker");
    }

    // ---- M65c: 衝撃音 (部屋 B の金属板へ箱を落とす) ----
    // ★歩行者の通り道から**外して**置く。廊下に置くと CC が剛体に阻まれて止まり、
    //   足音の被写体が丸ごと死ぬ (CC は剛体を押せない)
    {
        GameObject plate = s.CreateGameObject("Impact Plate");
        plate.SetLocalPosition(AcousticMapToWorld(5), 0.15f, AcousticMapToWorld(8));
        plate.SetLocalScale(kAcousticTile, 0.6f, kAcousticTile);
        auto* mr = plate.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr("adem_metal") };
        auto* pc = plate.AddComponent<ColliderComponent>();
        pc->shape = 1;
        pc->halfExtents = { 0.5f, 0.5f, 0.5f };
        pc->physMaterial = FindPhysMat("metal");

        GameObject drop = s.CreateGameObject("Impact Box");
        drop.SetLocalPosition(AcousticMapToWorld(5), 4.5f, AcousticMapToWorld(8));
        drop.SetLocalScale(0.7f, 0.7f, 0.7f);
        auto* dmr = drop.AddComponent<MeshRendererComponent>();
        dmr->mesh = cube;
        dmr->material = AssetID{ HashStr("adem_drop") };
        auto* dc = drop.AddComponent<ColliderComponent>();
        dc->shape = 1;
        dc->halfExtents = { 0.5f, 0.5f, 0.5f };
        auto* rb = drop.AddComponent<RigidbodyComponent>();
        rb->mass = 2.0f;
        // ★**反発は 0**。0.5 で組んだら、跳ね返りが減衰しきらずに
        //   「5 tick ごとに力積 1.6 N*s」の極限周期に入り、**900 tick で 151 回**
        //   衝撃音を出し続けた (実測)。反発 + 貫通押し出しが作る古典的なリミットサイクルで、
        //   速度は 0.5 m/s あるのでスリープ閾値にも掛からない。跳ねるたびに音が小さく
        //   なる様子は絵として魅力的だが、**永久音源はデモとして嘘**なので捨てた
        //   (減衰そのものは ImpactGain のセルフテストが固定している)
        rb->restitution = 0.0f;
    }

    // ---- M65e: 設置光 1 個 (企画 §4-3 の「持ち込んだ光」) ----
    // ★**golden を撮るために必須の 1 個**。M65e で初めてピクセルが動くが、企画どおり
    //   真っ暗な画にすると「機能が壊れて残光が 1 画素も出なくても golden と一致する」=
    //   回帰検出がゼロになる (計画 M65e の最大の罠)。弱い環境光 (Sun 0.35) で壁の輪郭を、
    //   この点光源で「音以外の光」を出しておくと、**音の帯が消えたときだけ絵が変わる**。
    // ★置き場所は部屋 A の隅。廊下と部屋 B には届かない範囲にしてあるので、
    //   L 字の向こう側で光っているものは残光しかない = 1 枚で両方を主張できる
    // ★**関数の末尾に足すこと** — 前に挿すと以降の全エンティティの index がずれ、
    //   波スロットの割り当て順まで動く (plans の「デモの生成順は RNG のストリーム」)
    {
        GameObject lamp = s.CreateGameObject("Placed Light");
        lamp.SetLocalPosition(AcousticMapToWorld(3), 1.6f, AcousticMapToWorld(3));
        auto* pl = lamp.AddComponent<LightComponent>();
        pl->type = 1; // 点光源
        pl->range = 6.0f;
        pl->intensity = 2.2f;
        pl->color = { 1.0f, 0.86f, 0.62f }; // 携行灯らしい暖色 (残光の寒色と対になる)
    }

    // ---- M65f: 敵 2 種 (同じ FSM / 違うセンサー) ----
    // ★**2 体置くことが企画の主張そのもの**。同じ AgentBrain を回していて、
    //   片方は AcousticListener だけ、もう片方は LightSeeker だけを持つ。
    //   絵の中で「赤は足音のほうへ、緑は設置光のほうへ」動けば、
    //   「1 つの FSM に差し替え可能なセンサーを挿す」という設計が成立している。
    // ★どちらも **CC 単体** (ソリッド Collider を付けない) — 足音のときと同じ理由で、
    //   同じエンティティにコライダを併用すると真下のレイが自分に当たる。
    //   さらに敵にコライダを付けると占有ベイクの署名が毎 tick 変わって全再ベイクが走る
    //   (動くものは遮蔽に入れない、という Sync 側の規約)。
    auto agent = [&](const char* name, int col, int row, const char* mat) {
        GameObject go = s.CreateGameObject(name);
        const float px = AcousticMapToWorld(col);
        const float pz = AcousticMapToWorld(row);
        go.SetLocalPosition(px, 1.35f, pz);
        go.SetLocalScale(0.6f, 1.6f, 0.6f);
        auto* mr = go.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr(mat) };
        auto* cc = go.AddComponent<CharacterControllerComponent>();
        cc->radius = 0.3f;
        cc->height = 1.6f;
        auto* br = go.AddComponent<AgentBrainComponent>();
        // ★home は明示的に焼く。AgentSystem は「原点なら未設定」で拾うが、
        //   それに頼るとデモの巡回先が「たまたま原点」になったときに壊れる
        br->home = { px, 1.35f, pz };
        br->target = br->home;
        return go;
    };
    {
        // 耳の敵: 部屋 B の奥。廊下を往復する歩行者の足音 (材質で 3m〜30m) を聞いて、
        // **音が回り込んできたのと同じ道**を辿って寄ってくる
        GameObject ear = agent("Agent Ear", 9, 9, "adem_agent_ear");
        auto* ls = ear.AddComponent<AcousticListenerComponent>();
        // ★閾値は**逆二乗のスケールで決める**こと。EnergyAt は minD = 1 セル (0.5m) の
        //   逆二乗なので、振幅 1.0 の音でも 10m 先では (0.5/10)^2 = 0.0025 しかない。
        //   0.015 のような「一見ちょうどよさそうな」値だと 3m 以内でしか聞こえず、
        //   デモでは 600 tick 走らせても 1 度も反応しなかった (probe で発見)。
        //   0.0015 = 廊下 (10m) の water / metal / glass は聞こえて gravel は聞こえない境
        ls->threshold = 0.0015f;
    }
    {
        // 目の敵: 部屋 B の入口寄り。**視線判定は持たない** (v1 の割り切り) ので、
        // 壁の向こうの設置光にも引かれる — 企画の「光に寄る」をそのまま素直に実装した形。
        // attractRadius を既定の 12 から広げてあるのは、部屋 B から部屋 A の光までが
        // 直線で 15m あり、既定のままだと 1 度も反応せず絵に出ないため
        GameObject eye = agent("Agent Eye", 4, 8, "adem_agent_eye");
        auto* seek = eye.AddComponent<LightSeekerComponent>();
        seek->attractRadius = 30.0f;
        seek->minIntensity = 0.5f; // 太陽 (0.35) には引かれない (平行光は元々見ない)
    }

    // ---- M65g: プレイヤー (企画のゲームループを通すための薄皮) ----
    // ★**エンジンには 1 行も足していない**。一人称視点 / 光の設置・回収 / 投擲は
    //   すべて GameLogic.dll の C++ スクリプト 3 本で、使っているのは既存 ABI (v15) だけ。
    // ★置き場所は部屋 A の隅。企画 4-2 の「決して消えない開始地点の光」= M65e で置いた
    //   Placed Light がすぐそこにあり、**捕まったときに押し戻される先**でもある。
    //   敵 2 体の経路 (廊下 → 部屋 A の光) から 1.7m (catchRadius) 以上離してあるので、
    //   無入力のスクショ実行で勝手に捕まって絵が動くことはない。
    // ★CC 単体 (ソリッド Collider なし) — 足音のレイが自分に当たると無音になる、という
    //   M65c からの規約。歩行者・敵とまったく同じ組み方。
    // ★**関数の末尾に足す** (以降のエンティティ index を動かさない = 波スロットの
    //   割り当て順と粒子 RNG のストリームを保つ)
    {
        GameObject player = s.CreateGameObject("Watcher");
        // ★y は敵 2 体と同じ 1.35。**CC の寸法にはスケールが掛かる** ので、
        //   height 1.6 x scale.y 1.6 = 全高 2.56 → 静止時の中心は床天面 + 1.28 になる。
        //   1.0 で置いたら床にめり込んだ状態から始まり、押し出しで浮き上がりながら
        //   歩くという分かりにくい壊れ方をした (probe で発見)
        player.SetLocalPosition(AcousticMapToWorld(1), 1.35f, AcousticMapToWorld(1));
        player.SetLocalScale(0.55f, 1.6f, 0.55f);
        auto* mr = player.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr("adem_player") };
        auto* cc = player.AddComponent<CharacterControllerComponent>();
        cc->radius = 0.3f;
        cc->height = 1.6f;
        auto* em = player.AddComponent<AcousticEmitterComponent>();
        // 足音は**床材が大きさを決める** (企画 3-4)。歩幅だけを WatcherFpsCamera が
        // 移動モード (しゃがみ / 歩き / 走り) で書き替える = 企画 3-2 の速度と危険度
        em->autoFootstep = true;
        em->stepDistanceM = 0.9f;
        em->ticksPerRing = 2;
        em->cooldownTicks = 12;
        // ★3 本とも AttachScriptIfRegistered。DLL が焼けていなくてもシーンは成立する
        //   (プレイヤーはただの箱として立っているだけになる)
        AttachScriptIfRegistered(w, player.Id(), "WatcherFpsCamera");
        AttachScriptIfRegistered(w, player.Id(), "WatcherLightTool");
        AttachScriptIfRegistered(w, player.Id(), "WatcherThrowTool");
    }

    // ---- M65g: 携行できる光 3 本 (企画 4-1 の上限そのもの) ----
    // ★★**スクリプトが実行時に生成するのではなく、シーンが用意して使い回す**。
    //   スクリプトから足したコンポーネントは tick 末まで存在しない (EngineAPI.h v14 の明文)
    //   ので、生成した同じ tick に LightComponent へ書いても 1 バイトも入らず、
    //   翌 tick までの 1 フレーム**既定値の平行光** (白 / intensity 1.0) が
    //   シーン全体を照らす = 暗闇のゲームで最悪の閃光になる (probe で実測して設計を変えた)。
    //   使い回しなら構造変更が 1 度も起きず、企画の「同時に持てるのは 3 本」とも一致する。
    // ★床下 (y=-4) に格納しておく。床は y[-1,0] を占めるので俯瞰のスクショには出ず、
    //   intensity 0 なのでライティングにも LightSeeker (minIntensity 0.5) にも寄与しない
    //   = golden は「プレイヤーの箱が増えたぶん」しか動かない
    for (int i = 0; i < 3; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "Watcher Lamp %d", i);
        GameObject lamp = s.CreateGameObject(name);
        lamp.SetLocalPosition(0.0f, -4.0f, 0.0f);
        lamp.SetLocalScale(0.3f, 0.3f, 0.3f);
        auto* lmr = lamp.AddComponent<MeshRendererComponent>();
        lmr->mesh = res.meshes.Sphere();
        lmr->material = AssetID{ HashStr("adem_lamp") };
        auto* pl = lamp.AddComponent<LightComponent>();
        pl->type = 1; // 点光源
        pl->range = 6.0f;
        pl->intensity = 0.0f; // 消灯 = 手札。設置中にスクリプトが 0 から育てる
        pl->color = { 1.0f, 0.86f, 0.62f }; // 携行灯らしい暖色 (残光の寒色と対になる)
    }
}

void RegisterAssetLibraries(EngineContext& ctx)
{
    std::error_code ec;
    if (!std::filesystem::is_directory(ctx.assetsRoot, ec)) {
        return;
    }
    // 起動時アセット走査の所要時間 (M51b のクック効果を含む診断値。sim には無関係)
    const auto scanStart = std::chrono::steady_clock::now();
    std::vector<std::wstring> audioFiles; // 走査後にまとめて判定する (M45f。下の注記参照)
    for (const auto& e : std::filesystem::recursive_directory_iterator(ctx.assetsRoot, ec)) {
        if (!e.is_regular_file()) {
            continue;
        }
        const std::wstring p = e.path().wstring();
        if (PrefabLibrary::IsComposePath(p)) { // .actor.json / .prefab.json (M48d)
            if (ctx.prefabs) {
                ctx.prefabs->LoadFromFile(p);
            }
        } else if (p.size() >= 10 && p.compare(p.size() - 10, 10, L".anim.json") == 0) {
            if (ctx.anims) {
                ctx.anims->LoadFromFile(p);
            }
        } else if (p.size() >= 9 && p.compare(p.size() - 9, 9, L".mat.json") == 0) {
            if (ctx.resources) {
                ctx.resources->materials.LoadFromFile(p, ctx.resources->textures, ctx.assetsRoot);
            }
        } else if (p.size() >= 16 && p.compare(p.size() - 16, 16, L".controller.json") == 0) {
            if (ctx.controllers) {
                ctx.controllers->LoadFromFile(p); // M22: Animator Controller
            }
        } else if (p.size() >= 11 && p.compare(p.size() - 11, 11, L".sound.json") == 0) {
            if (ctx.sounds) {
                ctx.sounds->LoadFromFile(p); // M45c: サウンドアセット
            }
        } else if (p.size() >= 11 && p.compare(p.size() - 11, 11, L".mixer.json") == 0) {
            if (ctx.mixers) {
                ctx.mixers->LoadFromFile(p); // M45d: ミキサー (適用は走査後にまとめて)
            }
        } else if (p.size() >= 13 && p.compare(p.size() - 13, 13, L".physmat.json") == 0) {
            // M59a1: 物理マテリアル。所有は EngineLoop、ここへは physmat:: で注入済み
            // (EngineContext を汚さない meshcol:: 流儀)
            if (PhysMatLibrary* pm = physmat::Library()) {
                pm->LoadFromFile(p);
            }
        } else {
            std::wstring ext = e.path().extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
            // 素の音声ファイルは走査中には展開せず、候補として貯めるだけにする。
            // **.sound.json より先に .wav を見ることがある**ので、「BGM 専用かどうか」は
            // 全部読み終えてからでないと判定できない (M45f)
            if (ext == L".wav" || ext == L".ogg") {
                audioFiles.push_back(p);
            } else if (ctx.resources != nullptr && ctx.shaders != nullptr
                       && (ext == L".glb" || ext == L".gltf" || ext == L".fbx")) {
                // M50a: メッシュ / マテリアル / スキンを丸ごとヘッドレス登録する。
                // M48g はスケルトンだけだったため、保存済みシーンをロードする経路
                // (ModelLoader::Load を通らない) では MeshRenderer.mesh / .material の
                // 実体が誰にも登録されず、モデルが描画されなかった。Editor / Runtime 共通
                const bool ok = (ext == L".fbx")
                    ? FbxLoader::RegisterAssets(*ctx.resources, *ctx.shaders, p, false)
                    : ModelLoader::RegisterAssets(*ctx.resources, *ctx.shaders, p, false);
                if (ok) {
                    MYE_LOG_INFO("[assets] model assets registered: %s", WideToUtf8(p).c_str());
                }
            }
        }
    }

    // M45c: 素の音声ファイルは PCM を AudioSystem へ展開しつつ、**ファイル名 stem を
    // 名前キーに登録**する (LoadWav = M19 互換シム)。既存スクリプトの
    // PlaySound("beep") がハードコードロード無しで引き続き解決できる経路がこれ。
    // --no-audio / デバイス無しでは LoadClipFile が早期 return するので何も起きない。
    // ★M45f: **stream の .sound.json からしか参照されていないファイルは展開しない** —
    //   数分の BGM を PCM 化すると数十 MB になり、ストリーミングの意味が無くなる。
    //   展開しない = 名前キーも張られないので、その素材は PlaySound(名前) では鳴らない
    //   (BGM を一発ものとして鳴らす経路は元々無いので、失うものは無い)
    for (const std::wstring& p : audioFiles) {
        if (ctx.audio == nullptr) {
            break;
        }
        if (ctx.sounds != nullptr &&
            ctx.sounds->UsageOfClip(AudioSystem::IdForFile(p).value) == ClipUsage::StreamOnly) {
            MYE_LOG_INFO("[audio] stream-only (not preloaded): %s", WideToUtf8(p).c_str());
            continue;
        }
        ctx.audio->LoadWav(WideToUtf8(std::filesystem::path(p).stem().wstring()), p);
    }

    // M45d: バスグラフはグローバルに 1 つなので、走査後に「どれを鳴らすか」を 1 本決める。
    // 反復順に依存しないよう PickStartupMixer が名前で選ぶ (default 優先 → 名前順の先頭)。
    // .mixer.json が 1 本も無ければ AudioSystem の既定構成 (Master/BGM/SE/UI) のまま
    if (ctx.mixers != nullptr && ctx.audio != nullptr) {
        const uint64_t hash = ctx.mixers->PickStartupMixer();
        if (const MixerAsset* m = ctx.mixers->Get(hash)) {
            ctx.mixers->SetActive(hash);
            ctx.audio->ApplyMixer(*m);
            MYE_LOG_INFO("[mixer] active mixer: %s (%d buses)", m->name.c_str(),
                         static_cast<int>(m->buses.size()));
        }
    }

    const double scanMs = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - scanStart)
                              .count();
    MYE_LOG_INFO("[assets] startup asset scan: %.1f ms", scanMs);
}

} // namespace mye
