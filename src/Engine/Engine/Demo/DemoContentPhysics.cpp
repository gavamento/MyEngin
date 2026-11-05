// 物理系ショーケース (--physics-demo / --joint-demo / --fracture-demo)。
// DemoContent.cpp から領域ごとの翻訳単位へ分けたもの (M93g)。関数の契約は DemoContent.h、
// ショーケースを跨いで使うヘルパーは DemoContentInternal.h。
#include "Engine/Engine/Demo/DemoContent.h"
#include "Engine/Engine/Demo/DemoContentInternal.h"

#include <cmath>
#include <cstdio>
#include <vector>

#include "Engine/Core/Ecs/ComponentRegistry.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/HierarchyWalk.h"
#include "Engine/Core/Util/Hash.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Engine/Loop/EngineLoop.h"
#include "Engine/Engine/Physics/Fracture/FractureBuilder.h" // M80f: 破片エンティティの事前生成
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/Asset/ModelLoader.h"
#include "Engine/Engine/Physics/Fracture/FractureBake.h"    // M80f: --fracture-demo の焼き
#include "Engine/Engine/Physics/Fracture/FractureLibrary.h" // M80f: メモリ上焼きの登録口
#include "Engine/Engine/Physics/Fracture/FractureSkinBake.h" // M80j: スキン破壊の骨割り当て
#include "Engine/Engine/Physics/Ragdoll/RagdollBuilder.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Renderer/Device/GpuResources.h"
#include "Engine/Renderer/Shader/ShaderManager.h"

namespace mye {

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
        col->shape = collidershape::kBox;
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
        col->shape = collidershape::kBox;
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
        col->shape = collidershape::kSphere;
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
        col->shape = collidershape::kBox;
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
        col->shape = collidershape::kSphere;
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
        col->shape = collidershape::kSphere;
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
        col->shape = collidershape::kBox;
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
        col->shape = collidershape::kSphere;
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
        col->shape = collidershape::kBox;
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
        col->shape = collidershape::kBox;
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
        col->shape = collidershape::kSphere;
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
        col->shape = collidershape::kBox;
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
        col->shape = collidershape::kSphere;
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
        auto* rj = addJoint(rod, jointtype::kBall, kNullEntity);
        rj->anchor = { 0.0f, 0.5f, 0.0f };            // ローカル → スケールが掛かって +1.5m
        rj->connectedAnchor = { -24.0f, 8.0f, 0.0f }; // 相手が null のときだけワールド

        GameObject bob = addSphere("PendulumBob", -24.0f, 4.4f, 0.0f, 0.8f, "jdemo_swing");
        auto* brb = addBody(bob, 6.0f);
        brb->velocity = { 3.0f, 0.0f, 0.0f };
        brb->angularDamping = 0.0f;
        auto* bj = addJoint(bob, jointtype::kBall, rod.Id());
        bj->anchor = { 0.0f, 0.75f, 0.0f };          // +0.6m (球の外 = 吊り点)
        bj->connectedAnchor = { 0.0f, -0.5f, 0.0f }; // 棒の下端 (相手のローカル)
    }

    // ---- 2. ロープ (Ball 10 連鎖 + 錘) ----
    // ★**コライダーは見た目どおり** (halfExtents 0.5)。関節で繋がった相手と食い込むと永久に
    //   押し合うので、`Joint.disableCollision` で**繋がったペアだけ候補から落とす** (縮めて逃げない)。
    //   ★これが `disableCollision` の replay / golden 被覆でもある。切れるのは直接繋がった 1 ペアだけなので、1 つ飛ばしの鎖どうしは
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
            auto* j = addJoint(link, jointtype::kBall, prev);
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
        auto* j = addJoint(weight, jointtype::kBall, prev);
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
        auto* j = addJoint(door, jointtype::kHinge, kNullEntity);
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
        auto* j = addJoint(arm, jointtype::kHinge, kNullEntity);
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
        auto* j = addJoint(plat, jointtype::kSlider, kNullEntity);
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
        addJoint(bracket, jointtype::kFixed, kNullEntity)->connectedAnchor = { 0.0f, 3.2f, 0.0f };

        GameObject arm = makeBox("WeldArm", 1.2f, 3.2f, 0.0f, 1.8f, 0.3f, 0.3f, "jdemo_weld");
        // ★見た目どおりの 0.5 で置き、繋がったペアだけ `disableCollision` で落とす
        //   (溶接で密着している相手と食い込むと接触ソルバが毎 tick 押し返して静止しない)
        addBoxCollider(arm, 0.5f, 0.5f, 0.5f, matSteel);
        addBody(arm, 3.0f);
        auto* aj = addJoint(arm, jointtype::kFixed, bracket.Id());
        aj->disableCollision = true; // ブラケットと腕 (M60j)
        aj->anchor = { -0.5f, 0.0f, 0.0f };         // 腕の -X 端 (-0.9m)
        aj->connectedAnchor = { 0.5f, 0.0f, 0.0f }; // ブラケットの +X 面 (+0.3m)

        GameObject weight = addSphere("WeldWeight", 1.92f, 2.41f, 0.0f, 0.8f, "jdemo_crate");
        addBody(weight, 15.0f);
        auto* wj = addJoint(weight, jointtype::kBall, arm.Id());
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
        auto* j = addJoint(deck, jointtype::kHinge, kNullEntity);
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

    // ---- 9. 凸多面体の山 (collidershape::kConvex) ----
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
            col->shape = collidershape::kConvex;
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
            ForEachInSubtree(w, model.Id(), [&](EntityID e, uint32_t) {
                if (auto* mr = w.GetComponent<MeshRendererComponent>(e)) {
                    meshEntity = e;
                    meshId = mr->mesh;
                    return WalkStep::Stop;
                }
                return WalkStep::Continue;
            });
            if (!meshEntity.IsNull() && !meshId.IsNull()) {
                model.SetLocalPosition(16.6f, 5.6f, 0.2f);
                GameObject body(&w, meshEntity);
                auto* col = body.AddComponent<ColliderComponent>();
                col->shape = collidershape::kConvex;
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
    //   restRotation / 短すぎる骨の足切り) が 2 箇所に散る。生成器が Engine 層にあるのは
    //   このため (Runtime も同じシーンを組める)
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
            ForEachInSubtree(w, actor.Id(), [&](EntityID e, uint32_t) {
                if (w.GetComponent<SkinnedMeshComponent>(e)) {
                    skinned = e;
                    return WalkStep::Stop;
                }
                return WalkStep::Continue;
            });
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
        ccol->shape = collidershape::kBox;
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

namespace {

// resources.meshes の CPU 頂点/インデックスを、分割コア (FractureBake.h) が使う Renderer
// 非依存の頂点形式へ詰め替える (FractureLibrary.cpp の ToMeshVertex とは逆向き)
FractureMesh ToFractureMesh(const Mesh& mesh)
{
    FractureMesh out;
    out.verts.resize(mesh.positions.size());
    for (size_t i = 0; i < mesh.positions.size(); ++i) {
        out.verts[i].position = mesh.positions[i];
        out.verts[i].normal = i < mesh.normals.size() ? mesh.normals[i] : DirectX::XMFLOAT3{ 0, 1, 0 };
        out.verts[i].uv = i < mesh.uvs.size() ? mesh.uvs[i] : DirectX::XMFLOAT2{ 0, 0 };
    }
    out.indices.assign(mesh.indices.begin(), mesh.indices.end());
    return out;
}

// builtin キューブを Voronoi 分割し、ファイルを作らずメモリ上へ登録する (--fracture-demo 用、
// 接頭辞 "fracture://demo_..."）。Debug と Release が同じ入力から独立に焼いて replay が
// 一致すること自体が、分割コアの構成間一致 (sub-02 の契約) を実行経路で証明する
const FractureAssetHandle* BakeDemoFracture(RenderResources& res, const char* namePrefix, uint32_t seed,
                                            int32_t pieceCount)
{
    Mesh* cubeMesh = res.meshes.Get(res.meshes.Cube());
    if (cubeMesh == nullptr) {
        return nullptr;
    }
    FractureBakeInput in;
    in.sourceMesh = ToFractureMesh(*cubeMesh);
    in.seed = seed;
    in.pieceCount = pieceCount;
    FractureBakeResult bake;
    if (!BakeFracture(in, bake) || !bake.success) {
        MYE_LOG_ERROR("[fracture-demo] bake failed (%s): %s", namePrefix, bake.failReason.c_str());
        return nullptr;
    }
    FractureLibrary* lib = fracturelib::Library();
    if (lib == nullptr) {
        return nullptr;
    }
    return lib->RegisterBaked(namePrefix, bake, HashStr("builtin://cube"), seed, pieceCount, 0, 32);
}

// ---- スキン破壊 (M80j): 2 骨の「腕」を手続き生成する ----
// FractureSkinSelfTest.cpp と同じ最小構成 (下半分=Bone0 / 上半分=Bone1、CCW 外向きの箱) を
// このファイルにも複製する (非公開ヘルパはファイルごとに持つ既存の流儀)。バインドポーズのまま
// 描く (アニメさせない — このデモの目的は「割れる前/後」の絵の切り替えで、追従アニメ自体の
// 検証は FractureSkinSelfTest が既に持つ)
struct DemoSkinArm {
    std::vector<MeshVertex> verts;
    std::vector<uint32_t> indices;
    std::vector<FractureSkinVertex> skinVerts;
    SkinnedModel model;
};

DemoSkinArm MakeDemoSkinArm(float hx, float hy, float hz)
{
    DemoSkinArm arm;
    // 面ごとに 4 頂点を独立させ、面法線をそのまま持たせる (FractureSelfTest.cpp の MakeBox は
    // 8 頂点共有・対角方向の法線で、ヘッドレスの物理検証専用 — 実際にライトを当てて描くと
    // 面が平らなのに法線が滑らかに変化して「ねじれた」ように見える。破片も元メッシュの法線を
    // そのまま引き継ぐので、割れる前・後の両方でここを直しておく)
    struct FaceVert {
        float x, y, z;
    };
    auto addFace = [&](const FaceVert& a, const FaceVert& b, const FaceVert& c, const FaceVert& d,
                       float nx, float ny, float nz) {
        const uint32_t base = static_cast<uint32_t>(arm.verts.size());
        const FaceVert corners[4] = { a, b, c, d };
        for (const FaceVert& p : corners) {
            MeshVertex mv;
            mv.position = { p.x, p.y, p.z };
            mv.normal = { nx, ny, nz };
            mv.uv = { 0.0f, 0.0f };
            mv.boneIndices[0] = (p.y > 0.0f) ? 1 : 0;
            mv.boneWeights = { 1.0f, 0.0f, 0.0f, 0.0f };
            arm.verts.push_back(mv);
            FractureSkinVertex sv;
            sv.position = mv.position;
            sv.boneIndices[0] = mv.boneIndices[0];
            sv.boneWeights = mv.boneWeights;
            arm.skinVerts.push_back(sv);
        }
        arm.indices.push_back(base + 0);
        arm.indices.push_back(base + 1);
        arm.indices.push_back(base + 2);
        arm.indices.push_back(base + 0);
        arm.indices.push_back(base + 2);
        arm.indices.push_back(base + 3);
    };
    const FaceVert p000{ -hx, -hy, -hz }, p100{ hx, -hy, -hz }, p110{ hx, hy, -hz }, p010{ -hx, hy, -hz };
    const FaceVert p001{ -hx, -hy, hz }, p101{ hx, -hy, hz }, p111{ hx, hy, hz }, p011{ -hx, hy, hz };
    addFace(p000, p010, p110, p100, 0, 0, -1);  // -Z
    addFace(p001, p101, p111, p011, 0, 0, 1);   // +Z
    addFace(p000, p100, p101, p001, 0, -1, 0);  // -Y
    addFace(p010, p011, p111, p110, 0, 1, 0);   // +Y
    addFace(p000, p001, p011, p010, -1, 0, 0);  // -X
    addFace(p100, p110, p111, p101, 1, 0, 0);   // +X

    arm.model.joints.resize(2);
    arm.model.joints[0].name = "Bone0";
    arm.model.joints[0].parent = -1;
    arm.model.joints[0].bindT = { 0.0f, -hy, 0.0f };
    arm.model.joints[0].bindR = { 0.0f, 0.0f, 0.0f, 1.0f };
    arm.model.joints[0].bindS = { 1.0f, 1.0f, 1.0f };
    arm.model.joints[0].inverseBind = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, hy, 0, 1 };
    arm.model.joints[1].name = "Bone1";
    arm.model.joints[1].parent = 0;
    arm.model.joints[1].bindT = { 0.0f, 2.0f * hy, 0.0f };
    arm.model.joints[1].bindR = { 0.0f, 0.0f, 0.0f, 1.0f };
    arm.model.joints[1].bindS = { 1.0f, 1.0f, 1.0f };
    arm.model.joints[1].inverseBind = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -hy, 0, 1 };
    return arm;
}

// 手続き生成した腕を Voronoi 分割し、骨へ割り当ててメモリ上へ登録する (--fracture-demo 用)
const FractureAssetHandle* BakeDemoSkinFracture(const char* namePrefix, const DemoSkinArm& arm, float hy)
{
    FractureBakeInput in;
    in.sourceMesh.verts.resize(arm.verts.size());
    for (size_t i = 0; i < arm.verts.size(); ++i) {
        in.sourceMesh.verts[i].position = arm.verts[i].position;
        in.sourceMesh.verts[i].normal = arm.verts[i].normal;
        in.sourceMesh.verts[i].uv = arm.verts[i].uv;
    }
    in.sourceMesh.indices.assign(arm.indices.begin(), arm.indices.end());
    // 下半分/上半分がそれぞれ 1 破片になるよう、明示シードで確実な 2 分割にする
    // (PlaceSeeds のランダム配置に頼らない — 骨割り当ての検算はこの形状依存)
    FractureBakeResult bake;
    const bool baked
        = BakeFractureWithSeeds(in.sourceMesh, { { 0, -hy * 0.5f, 0 }, { 0, hy * 0.5f, 0 } }, 0.1f, bake);
    if (!baked || !bake.success) {
        MYE_LOG_ERROR("[fracture-demo] skin arm bake failed (%s): %s", namePrefix, bake.failReason.c_str());
        return nullptr;
    }
    std::vector<FractureSkinJoint> joints;
    for (const SkeletonJoint& j : arm.model.joints) {
        joints.push_back({ j.name, j.inverseBind });
    }
    const std::vector<std::string> boneNames = AssignFractureBonesAndTransform(bake, arm.skinVerts, joints);
    FractureLibrary* lib = fracturelib::Library();
    if (lib == nullptr) {
        return nullptr;
    }
    return lib->RegisterBaked(namePrefix, bake, HashStr("demo://skin_arm"), 1, 2, 0, 32, boneNames);
}

} // namespace

void BuildFractureShowcaseScene(EngineContext& ctx)
{
    Scene& s = *ctx.scene;
    World& w = s.GetWorld();
    RenderResources& res = *ctx.resources;
    s.SetName("fracture_showcase");
    const AssetID cube = res.meshes.Cube();
    const AssetID matSteel = FindPhysMat("steel");

    // 固定の壁。床から浮かせた寸法をここでまとめ、壁本体と射出弾の狙いの
    // 両方から参照する (狙いがずれないように)
    constexpr float kWallX = 3.0f;
    constexpr float kWallWidth = 2.5f, kWallHeight = 2.5f, kWallThickness = 0.6f;
    constexpr float kWallBottomAboveFloor = 1.5f; // 床との間に落下ぶんの隙間を空ける
    constexpr float kWallCenterY = kWallBottomAboveFloor + kWallHeight * 0.5f;

    auto makeMat = [&](const char* name, float r, float g, float b) {
        Material m;
        m.shader = AssetID{ HashStr("forward_lit") };
        m.texture = res.textures.White();
        m.baseColor = { r, g, b, 1.0f };
        return res.materials.Register(name, m);
    };
    makeMat("frdemo_floor", 0.30f, 0.32f, 0.36f);
    makeMat("frdemo_box", 0.75f, 0.35f, 0.22f);
    makeMat("frdemo_wall", 0.55f, 0.56f, 0.60f);
    makeMat("frdemo_inner", 0.86f, 0.80f, 0.62f);

    GameObject camera = s.CreateGameObject("Main Camera");
    camera.AddComponent<CameraComponent>();
    camera.SetLocalPosition(0.0f, 5.0f, -12.0f);
    camera.SetLocalRotationEuler(14.0f, 0.0f, 0.0f);

    GameObject sun = s.CreateGameObject("Sun");
    sun.AddComponent<LightComponent>();
    sun.SetLocalRotationEuler(50.0f, -30.0f, 0.0f);

    // ---- 物理環境 ----
    {
        GameObject envGo = s.CreateGameObject("Environment");
        auto* env = envGo.AddComponent<PhysicsEnvironmentComponent>();
        env->gravity = { 0.0f, -9.81f, 0.0f };
    }

    // ---- 床 ----
    {
        GameObject floor = s.CreateGameObject("Floor");
        floor.SetLocalPosition(0.0f, -0.5f, 0.0f);
        floor.SetLocalScale(24.0f, 1.0f, 24.0f);
        auto* mr = floor.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr("frdemo_floor") };
        auto* col = floor.AddComponent<ColliderComponent>();
        col->shape = collidershape::kBox;
        col->halfExtents = { 0.5f, 0.5f, 0.5f };
        col->physMaterial = matSteel;
        col->friction = 0.8f;
    }

    // ---- 落下して転がる破壊物の箱 (動的ルート)。壊れる前は 1 剛体として動くだけを見せ、
    //      着地した後に球をぶつけると割れる (FractureSystem、M80g) ----
    {
        const char* prefix = "fracture://demo_box";
        const FractureAssetHandle* baked = BakeDemoFracture(res, prefix, 1, 8);
        GameObject box = s.CreateGameObject("FractureBox");
        box.SetLocalPosition(-2.5f, 4.0f, 0.0f);
        box.SetLocalRotationEuler(22.0f, 17.0f, 0.0f); // 転がる初期姿勢
        auto* mr = box.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr("frdemo_box") };
        auto* rb = box.AddComponent<RigidbodyComponent>();
        rb->mass = 8.0f;
        auto* d = box.AddComponent<DestructibleComponent>();
        d->innerMaterial = AssetID{ HashStr("frdemo_inner") };
        // strength は既定値のまま (実測した既定 70N で足りる。20kg・30m/s の球衝突は
        // 桁違いに大きい)
        // FractureSystem が実行時にこの Destructible の資産を再解決するための参照
        // (メモリ登録なので guid ではなく登録名の hash と同じ値、ResolveFractureAsset 参照)
        d->fractureAsset = AssetID{ HashStr(prefix) };
        if (baked != nullptr) {
            BuildFracturePieces(w, box.Id(), *baked);
        }
    }

    // ---- 固定の壁 (kinematic ルート)。床から浮かせて置き、下寄りを撃つ。
    //      壁の残りは kinematic のまま宙に固定される。下寄りの破片は、外れると
    //      真下が壁の外 (床までの隙間) になるため重力で落ちて穴が見える。上寄りを
    //      撃った場合は、外れても真下に壁の残りが支えとして残り続けるため落ちない
    //      (実測で確認済み。壁の上端を撃つ案は採らなかった) ----
    {
        const char* prefix = "fracture://demo_wall";
        const FractureAssetHandle* baked = BakeDemoFracture(res, prefix, 2, 12);
        GameObject wall = s.CreateGameObject("FractureWall");
        wall.SetLocalScale(kWallWidth, kWallHeight, kWallThickness);
        wall.SetLocalPosition(kWallX, kWallCenterY, 0.0f);
        auto* mr = wall.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr("frdemo_wall") };
        auto* rb = wall.AddComponent<RigidbodyComponent>();
        rb->isKinematic = true;
        // 分かれた破片の質量は「ルートの全質量 × 体積比」で決まる (spec §4.1 破断 5)。壁らしい
        // 重さを与えて、外れた破片が床に落ちたときの見た目を安定させる
        rb->mass = 100.0f;
        auto* d = wall.AddComponent<DestructibleComponent>();
        d->innerMaterial = AssetID{ HashStr("frdemo_inner") };
        // strength は既定値のまま (実測した既定 70N で足りる)
        d->fractureAsset = AssetID{ HashStr(prefix) };
        if (baked != nullptr) {
            BuildFracturePieces(w, wall.Id(), *baked);
        }
        // ABI v22 probe (M80l): 着弾より先にスクリプトから割り、onBreak を受け取る
        // (GameLogic.dll 未ロード = ヘッドレス selftest 等ではスキップ)
        const ComponentTypeId damageProbe = ComponentRegistry::Get().FindByName("FractureDamageProbe");
        if (damageProbe != kInvalidComponentType) {
            w.AddComponentRaw(wall.Id(), damageProbe);
        }
    }

    // ---- スキンメッシュの破壊 (M80j)。2 骨の腕を kinematic ルートとして置き、球を当てると
    //      Bone1 側 (上半分) が骨追従から剛体化して落ち、Bone0 側は骨に追従したまま残る。
    //      バインドポーズのまま (アニメさせない — 追従アニメ自体の検証は FractureSkinSelfTest) ----
    constexpr float kArmHx = 0.4f, kArmHy = 1.0f, kArmHz = 0.4f;
    {
        const char* prefix = "fracture://demo_skin_arm";
        const DemoSkinArm arm = MakeDemoSkinArm(kArmHx, kArmHy, kArmHz);
        const FractureAssetHandle* baked = BakeDemoSkinFracture(prefix, arm, kArmHy);
        const AssetID armMesh = res.meshes.Register("frdemo_skin_arm_mesh", arm.verts, arm.indices);
        const AssetID armSkin = res.skinnedModels.Register("frdemo_skin_arm_model", arm.model);
        GameObject skinArm = s.CreateGameObject("FractureSkinArm");
        skinArm.SetLocalPosition(8.0f, 3.0f, 0.0f);
        auto* mr = skinArm.AddComponent<MeshRendererComponent>();
        mr->mesh = armMesh;
        mr->material = AssetID{ HashStr("frdemo_box") };
        auto* sm = skinArm.AddComponent<SkinnedMeshComponent>();
        sm->model = armSkin;
        sm->clip = -1; // クリップ無し = 常にバインドポーズ
        sm->playing = false;
        auto* d = skinArm.AddComponent<DestructibleComponent>();
        d->innerMaterial = AssetID{ HashStr("frdemo_inner") };
        // strength は既定値のまま (箱と同じ理由、上のコメント参照)
        d->fractureAsset = AssetID{ HashStr(prefix) };
        if (baked != nullptr) {
            BuildFracturePieces(w, skinArm.Id(), *baked);
        }
    }

    // ---- 撃ち出す弾 (M80g)。決定的な初速の球を tick 0 から飛ばす (重力なしの直進 — 他の
    //      デモの射出弾と同じ流儀だが、着地後の箱と kinematic な壁の両方へ確実に当てるため
    //      重力を切って軌道を単純にした)。箱は着地 (概ね tick 50 前後) してから当たるよう
    //      距離を離して遅らせ、壁は静止しているので早く当てる ----
    auto makeCannonball = [&](const char* name, DirectX::XMFLOAT3 pos, DirectX::XMFLOAT3 vel) {
        GameObject ball = s.CreateGameObject(name);
        ball.SetLocalPosition(pos.x, pos.y, pos.z);
        auto* mr = ball.AddComponent<MeshRendererComponent>();
        mr->mesh = res.meshes.Sphere();
        mr->material = AssetID{ HashStr("frdemo_wall") };
        auto* col = ball.AddComponent<ColliderComponent>();
        col->shape = collidershape::kSphere;
        col->radius = 0.5f;
        auto* rb = ball.AddComponent<RigidbodyComponent>();
        rb->mass = 20.0f;
        rb->gravityScale = 0.0f;
        rb->velocity = vel;
    };
    makeCannonball("FractureBallBox", { -2.5f, 0.55f, -20.0f }, { 0.0f, 0.0f, 30.0f });
    // 壁の下寄りを狙う (上のコメント参照)
    makeCannonball("FractureBallWall", { kWallX, kWallCenterY - kWallHeight * 0.35f, -15.0f },
                   { 0.0f, 0.0f, 30.0f });
    // Bone1 (上半分) の中心あたり (root + (0, hy/2, 0)) を狙う。バインドポーズ固定なので
    // 静的に計算できる (FractureSkinSelfTest の狙い方と同じ式)
    makeCannonball("FractureBallSkinArm", { 8.0f, 3.0f + kArmHy * 0.5f, -15.0f },
                   { 0.0f, 0.0f, 30.0f });
}

} // namespace mye
