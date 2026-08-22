#include "Engine/Engine/DemoContent.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <iterator>
#include <string>
#include <system_error>

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
#include "Engine/Engine/Prefab.h"
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

    // ---- デカール 2 枚 (M56a) ----
    // ★**golden に載せるために置いている。** 1 枚も置かないと「デカールが壊れても
    //   demo_render_deferred は緑のまま」= 回帰の被覆がゼロになる (M54a でこのシーンを
    //   作った理由そのもの)。置いた代償として demo_render_deferred / demo_render_taa の
    //   2 枚が M56a で動く (demo_render_forward が**動かない**ことが v1 の Forward 非対応の証明)。
    // ★テクスチャは付けない (null = 白 = color がそのまま出る)。AssetRef はシーン JSON へ
    //   64bit の AssetID をそのまま書く仕様で、テクスチャの AssetID は正規化絶対パスの
    //   ハッシュ = チェックアウト依存になる (M51j で踏んだ穴)。色だけでも
    //   「面に沿って貼り付く」ことは絵に出る。
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
    decal("DecalGround", -8.0f, 1.0f, -6.0f, 90.0f, 0.0f, 8.0f, 6.0f, 4.0f,
          0.95f, 0.42f, 0.12f, 0.85f, 90.0f, 0);
    // 2 枚目: 柱の側面へ水平投影 (回転なし = ローカル +Z が世界の +Z = カメラの奥向き)。
    // Pillar_03 (x=4, z=-6, 幅 1.8, 高さ 3.0) の手前の面だけを覆う箱で、床 (y=0) は
    // 箱の下端 y=0.5 の外なので掛からない = 「箱の外は捨てる」が角度フェードとは
    // 独立に効いていることの目印になる
    decal("DecalPillar", 4.0f, 1.5f, -5.0f, 0.0f, 0.0f, 2.5f, 2.0f, 4.0f,
          0.20f, 0.75f, 0.95f, 0.90f, 70.0f, 1);

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
