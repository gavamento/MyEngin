#include "Engine/Engine/DemoContent.h"

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <filesystem>
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

    // glTF のメッシュ/マテリアル/テクスチャを登録 (エンティティは作らない)。
    // 保存済みシーンをロードする場合でも AssetID の実体が揃うようにする
    ModelLoader::ReloadMeshes(res, *ctx.shaders, ctx.assetsRoot + L"\\models\\BoxTextured.glb");
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
    }
    const ComponentTypeId spawner = ComponentRegistry::Get().FindByName("Spawner");
    if (spawner != kInvalidComponentType) {
        s.GetWorld().AddComponentRaw(fire.Id(), spawner);
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
        col->isTrigger = 0;
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
        col->isTrigger = 0;
        drop.AddComponent<RigidbodyComponent>();
    }
    w.ApplyStructuralChanges();
}

void RegisterAssetLibraries(EngineContext& ctx)
{
    std::error_code ec;
    if (!std::filesystem::is_directory(ctx.assetsRoot, ec)) {
        return;
    }
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
            } else if (ctx.resources != nullptr
                       && (ext == L".glb" || ext == L".gltf" || ext == L".fbx")) {
                // M48g: **スケルトンだけ**を登録する (エンティティも GPU バッファも作らない)。
                // 保存済みシーンをロードする経路は ModelLoader::Load を通らないため、
                // SkinnedMesh.model の AssetID が誰にも登録されずポーズ評価が丸ごと
                // 落ちていた (骨追従も描画のボーンパレットも)。Editor / Runtime 共通の穴
                const size_t n = (ext == L".fbx") ? FbxLoader::RegisterSkinnedModels(*ctx.resources, p)
                                                  : ModelLoader::RegisterSkinnedModels(*ctx.resources, p);
                if (n > 0) {
                    MYE_LOG_INFO("[assets] %zu skinned model(s) registered: %s", n,
                                 WideToUtf8(p).c_str());
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
}

} // namespace mye
