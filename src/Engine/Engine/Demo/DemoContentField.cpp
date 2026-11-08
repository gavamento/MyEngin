//====================================================================================
//                          DemoContentField.cpp
//  MyEngin/ 秋田蓮音                                                       11/08/2026
//                                          かき乱しフィールド (M96) のショーケース (--field-demo、霧の広場)
//====================================================================================
#include "Engine/Engine/Demo/DemoContent.h"
#include "Engine/Engine/Demo/DemoContentInternal.h"

#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Util/Hash.h"
#include "Engine/Engine/Asset/ModelLoader.h"
#include "Engine/Engine/Loop/EngineLoop.h"
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Renderer/Device/GpuResources.h"
#include "Engine/Renderer/Shader/ShaderManager.h"

namespace mye {

namespace {

// 霧の広場の寸法。場は広場の手前から奥へ伸びる 32 m 四方 (既定の 128 x 128 マス、0.25 m)
constexpr float kFieldCenterZ = 8.0f;

void AddCamera(Scene& s)
{
    GameObject camera = s.CreateGameObject("Main Camera");
    camera.AddComponent<CameraComponent>();
    // 層 (高さ 0〜1 m) を斜めに見下ろすカメラ。球の通り道の筋が、地面の暗さと霧の白さの対比で読める
    camera.SetLocalPosition(0.0f, 7.0f, -12.0f);
    camera.SetLocalRotationEuler(36.0f, 0.0f, 0.0f);
}

void AddLights(Scene& s)
{
    // 平行光 (既定の種類)。地面の霧に当たる太陽光 (fogSunLight) の出どころ
    GameObject sun = s.CreateGameObject("Sun");
    sun.AddComponent<LightComponent>();
    sun.SetLocalRotationEuler(50.0f, -30.0f, 0.0f);
    // 霧の中で光が滲むかを見るための点光源 1 つ (霧の縁に近い奥)
    GameObject lamp = s.CreateGameObject("Lamp");
    lamp.SetLocalPosition(0.0f, 1.6f, 12.0f);
    auto* light = lamp.AddComponent<LightComponent>();
    light->type = lighttype::kPoint;
    light->color = { 1.0f, 0.82f, 0.6f };
    light->intensity = 3.0f;
    light->range = 9.0f;
}

void AddPhysicsEnvironment(Scene& s)
{
    GameObject envGo = s.CreateGameObject("Environment");
    auto* env = envGo.AddComponent<PhysicsEnvironmentComponent>();
    env->gravity = { 0.0f, -9.81f, 0.0f };
    env->windVelocity = { 0.4f, 0.0f, 0.0f }; // 弱い風。晴れた筋が風下 (+x) へゆっくり流される
}

void AddGround(Scene& s, const AssetID cube)
{
    GameObject floor = s.CreateGameObject("Floor");
    floor.SetLocalPosition(0.0f, -0.5f, 10.0f);
    floor.SetLocalScale(34.0f, 1.0f, 52.0f);
    auto* mr = floor.AddComponent<MeshRendererComponent>();
    mr->mesh = cube;
    mr->material = AssetID{ HashStr("pdemo_floor") };
    auto* col = floor.AddComponent<ColliderComponent>();
    col->shape = collidershape::kBox;
    col->halfExtents = { 0.5f, 0.5f, 0.5f };
    col->friction = 0.0f; // 球が止まらず滑る (霧を横切る見た目が目的で、転がりの再現ではない)
}

// 場: 地面を這う厚さ 1 m の層。濃さ + 跡。エンティティの位置が格子の中心と層の下端 (y) になる
void AddField(Scene& s)
{
    GameObject field = s.CreateGameObject("DisturbanceField");
    field.SetLocalPosition(0.0f, 0.0f, kFieldCenterZ);
    auto* f = field.AddComponent<DisturbanceFieldComponent>();
    f->shape = 0;
    f->channels = kDisturbanceChannelDensity | kDisturbanceChannelTrail;
    f->densityKeep = 0.995f; // 筋が 10 秒近くかけて戻る
    f->fogDensity = 1.6f;    // 既定 (2.0) だと層が不透明で、晴れた筋が読めない
    f->fogSunLight = true;   // 太陽の向きで霧の明るさが変わる (影なし)
}

// 霧を割って転がる球。物理の初速だけで動く (スクリプトなし = 決定的)。源は速度を剛体から自動で取る
void AddRollingBall(Scene& s, const AssetID sphere, const char* name, float x, float z, float speed)
{
    GameObject ball = s.CreateGameObject(name);
    ball.SetLocalPosition(x, 0.45f, z);
    ball.SetLocalScale(0.8f, 0.8f, 0.8f);
    auto* mr = ball.AddComponent<MeshRendererComponent>();
    mr->mesh = sphere;
    mr->material = AssetID{ HashStr("pdemo_ball") };
    auto* col = ball.AddComponent<ColliderComponent>();
    col->shape = collidershape::kSphere;
    col->radius = 0.5f;
    col->friction = 0.0f;
    auto* rb = ball.AddComponent<RigidbodyComponent>();
    rb->useDensity = true;
    rb->velocity = { 0.0f, 0.0f, speed };
    auto* src = ball.AddComponent<DisturbanceSourceComponent>();
    src->mode = 0;
    src->radius = 1.5f;
    src->strength = 0.5f;
    src->fullSpeed = 3.0f;
    src->densityDelta = -0.3f;
    src->trailDepth = 0.5f;
}

// 霧の中を歩く人 (M96d)。ZZFieldDemoDriver が決まった経路で歩き、斬撃 (Cone) と爆発 (Radial) を明示のインパルスで積み、
// 足元の濃さを FieldSample で読む。歩いた筋は Moving の源が残す。GameLogic.dll が無い構成では動かない
void AddFieldDemoPerson(Scene& s, const AssetID capsule)
{
    GameObject person = s.CreateGameObject("FieldWalker");
    person.SetLocalPosition(5.0f, 0.9f, -6.0f); // CC の中心は足元から 0.9 m 上 (既定の高さ 1.8)
    person.AddComponent<CharacterControllerComponent>();
    auto* src = person.AddComponent<DisturbanceSourceComponent>();
    src->mode = 0;
    src->radius = 1.0f;
    src->strength = 0.5f;
    src->fullSpeed = 3.0f;
    src->densityDelta = -0.3f;
    src->trailDepth = 0.5f;
    AttachScriptIfRegistered(s.GetWorld(), person.Id(), "ZZFieldDemoDriver");
    // 見た目は子へ逃がす (CC のカプセルは本体の scale を拾うので本体は無スケール)
    GameObject body = s.CreateGameObject("Body");
    body.SetParent(person);
    body.SetLocalScale(0.6f, 0.9f, 0.6f);
    auto* mr = body.AddComponent<MeshRendererComponent>();
    mr->mesh = capsule;
    mr->material = AssetID{ HashStr("pdemo_ball") };
}

// M96o: 霧の中のランタン 3 個 (点光源)。場の fogLocalLights を true にして、地面の霧が光ることを見せる
void AddFieldDemoLanterns(Scene& s)
{
    struct Lantern {
        const char* name;
        float x;
        float z;
    };
    const Lantern lanterns[] = { { "Lantern_A", -5.0f, 5.0f }, { "Lantern_B", 4.0f, 8.0f }, { "Lantern_C", -1.0f, 13.0f } };
    for (const Lantern& l : lanterns) {
        GameObject go = s.CreateGameObject(l.name);
        go.SetLocalPosition(l.x, 0.7f, l.z);
        auto* light = go.AddComponent<LightComponent>();
        light->type = lighttype::kPoint;
        light->color = { 1.0f, 0.7f, 0.35f };
        light->intensity = 80.0f;
        light->range = 6.0f;
    }
    GameObject field = s.Find("DisturbanceField");
    if (field) {
        field.GetComponent<DisturbanceFieldComponent>()->fogLocalLights = true;
    }
}

// 浅い池 (M96 sub-12): 水面 (Gerstner を弱めた浅い池) と、波紋チャネルだけの 2D 層を同じ場所に置く。
// 人 (DisturbanceSource.ripple 付きの球) が池を横切ると波紋が広がる。層の下端 (y) = 水面の高さ
void AddFieldDemoPond(Scene& s, const AssetID sphere)
{
    constexpr float kPondX = 5.0f;
    constexpr float kPondZ = -2.0f;
    constexpr float kPondSurfaceY = 0.12f;
    constexpr float kPondSize = 8.0f; // 水面メッシュ (50 m 四方) を縮めた一辺 [m]。層 (32 マス x 0.25 m) と同じ広さ

    GameObject water = s.CreateGameObject("PondWater");
    water.SetLocalPosition(kPondX, 0.0f, kPondZ);
    water.SetLocalScale(kPondSize / 50.0f, 1.0f, kPondSize / 50.0f);
    auto* wave = water.AddComponent<WaterWaveComponent>();
    wave->baseHeight = kPondSurfaceY;
    wave->overallScale = 0.15f; // 浅い池: 解析波は弱くして、波紋を主役にする
    wave->waveCount = 2;
    wave->foamStrength = 0.0f;
    wave->affectBuoyancy = false;

    GameObject field = s.CreateGameObject("PondRippleField");
    field.SetLocalPosition(kPondX, kPondSurfaceY, kPondZ);
    auto* f = field.AddComponent<DisturbanceFieldComponent>();
    f->shape = 0;
    f->cellsX = 32;
    f->cellsZ = 32;
    f->channels = kDisturbanceChannelRipple;
    f->rippleKeep = 0.995f;

    // 人 2 人が池を +x へ横切る (物理の初速だけ = 決定的)。広場の霧が水面を隠すので、通り道の霧も晴らす
    const float zs[2] = { -3.5f, -0.5f };
    const float speeds[2] = { 2.0f, 2.5f };
    const char* names[2] = { "PondWalker_A", "PondWalker_B" };
    for (int i = 0; i < 2; ++i) {
        GameObject walker = s.CreateGameObject(names[i]);
        walker.SetLocalPosition(0.5f, 0.45f, zs[i]);
        walker.SetLocalScale(0.8f, 0.8f, 0.8f);
        auto* mr = walker.AddComponent<MeshRendererComponent>();
        mr->mesh = sphere;
        mr->material = AssetID{ HashStr("pdemo_ball") };
        auto* col = walker.AddComponent<ColliderComponent>();
        col->shape = collidershape::kSphere;
        col->radius = 0.5f;
        col->friction = 0.0f;
        auto* rb = walker.AddComponent<RigidbodyComponent>();
        rb->useDensity = true;
        rb->velocity = { speeds[i], 0.0f, 0.0f };
        auto* src = walker.AddComponent<DisturbanceSourceComponent>();
        src->mode = 0;
        src->radius = 1.6f;
        src->strength = 0.0f;
        src->densityDelta = -1.0f; // 池の上の霧 (広場の層) を人の通り道だけ晴らして、水面を見せる
        src->ripple = 0.3f;
        src->fullSpeed = 3.0f;
    }
}

// 煙の柱 (M96g): Volume 32 x 16 x 32 (0.5 m) と、下から上向きに煙を吹き出す Constant の源。浮力で昇る。
// 広場の爆発 (ZZFieldDemoDriver の Radial、(-7, 0.3, 1) 半径 4 m) の真上に噴き出し口を置き、爆発の瞬間に煙の根元が千切れて吹き飛ぶ。
// Volume のフロクセルの霧はカメラの froxelOn が要る (自動で回すのは M96h)。froxelDensity = 0 で場の霧だけを描く
void AddFieldDemoSmoke(Scene& s)
{
    constexpr float kVolumeX = -7.0f;
    constexpr float kVolumeY = 4.0f; // 格子は y = 0〜8 m
    constexpr float kVolumeZ = 2.0f;

    GameObject volume = s.CreateGameObject("SmokeVolume");
    volume.SetLocalPosition(kVolumeX, kVolumeY, kVolumeZ);
    auto* f = volume.AddComponent<DisturbanceFieldComponent>();
    f->shape = 1;
    f->cellsX = 32;
    f->cellsY = 16;
    f->cellsZ = 32;
    f->cellSize = 0.5f;
    f->channels = kDisturbanceChannelDensity;
    f->restDensity = 0.0f;   // 何も無い空気。源が足した分だけ煙になる
    f->densityKeep = 0.997f; // 煙が十数秒かけて薄れる
    f->velocityKeep = 0.97f;
    f->buoyancy = 8.0f;
    f->fogDensity = 3.0f;
    f->fogColor = { 0.3f, 0.3f, 0.34f }; // 白い地面の霧に対して読める暗い煙
    f->fogBrightness = 0.5f;

    GameObject vent = s.CreateGameObject("SmokeVent");
    vent.SetLocalPosition(kVolumeX, 0.5f, 1.0f);
    auto* src = vent.AddComponent<DisturbanceSourceComponent>();
    src->mode = 1;
    src->radius = 1.3f;
    src->strength = 0.4f;
    src->densityDelta = 0.5f;
    src->constantVelocity = { 0.0f, 2.0f, 0.0f };

    GameObject camera = s.Find("Main Camera");
    if (camera) {
        auto* fx = camera.GetComponent<CameraPostFxComponent>();
        if (fx == nullptr) {
            fx = camera.AddComponent<CameraPostFxComponent>();
        }
        fx->froxelOn = true;
        fx->froxelDensity = 0.0f;
    }
}

// M96f: 骨アニメの人が剣を振る瞬間に、アニメイベント disturb で霧を斬る (field_swing.controller.json の
// Attack の 22 tick、右手から前方の円錐)。人は +X を向く = イベントの dir (モデルの前方 -Z) がエンティティの回転で回る
void AddFieldDemoSwing(EngineContext& ctx)
{
    // .meta の GUID (assets\anims\field_swing.controller.json.meta)
    constexpr uint64_t kFieldSwingControllerGuid = 0xc96f5a2e8d1b4073ull;
    GameObject actor = ModelLoader::Load(*ctx.scene, *ctx.resources, *ctx.shaders, ctx.assetsRoot + L"\\models\\anim_test.glb");
    if (!actor) {
        MYE_LOG_ERROR("[field-demo] anim_test.glb could not be loaded (run tools\\gen_anim_test_gltf.ps1)");
        return;
    }
    actor.SetLocalPosition(-4.0f, 0.0f, -3.0f);
    actor.SetLocalRotationEuler(0.0f, -90.0f, 0.0f);
    actor.AddComponent<AnimatorControllerComponent>()->controller = AssetID{ kFieldSwingControllerGuid };
}

// 草束 (M96 sub-14): 霧の広場の中ほどに細い箱の草を格子状に並べる。ボールが通った所の草が流れの向きへ倒れ、
// 流れが引くと戻る (GrassField.surface。倒れは VS が場を読んで決める)。ずらしは整数の混ぜだけ = 決定的
void AddFieldDemoGrass(EngineContext& ctx, Scene& s, const AssetID cube)
{
    constexpr int kBladesX = 21;
    constexpr int kBladesZ = 21;
    constexpr float kSpacing = 0.5f;
    constexpr float kOriginX = -5.0f;
    constexpr float kOriginZ = 3.0f;
    const AssetID grass = MaterialLibrary::HashForPath(ctx.assetsRoot + L"\\materials\\GrassField.mat.json");
    for (int iz = 0; iz < kBladesZ; ++iz) {
        for (int ix = 0; ix < kBladesX; ++ix) {
            const uint32_t h = static_cast<uint32_t>(ix) * 73856093u ^ static_cast<uint32_t>(iz) * 19349663u;
            const float jx = static_cast<float>(h & 0xFFu) / 255.0f - 0.5f;
            const float jz = static_cast<float>((h >> 8) & 0xFFu) / 255.0f - 0.5f;
            const float height = 0.45f + 0.25f * static_cast<float>((h >> 16) & 0xFFu) / 255.0f;
            GameObject blade = s.CreateGameObject("Grass");
            blade.SetLocalPosition(kOriginX + (static_cast<float>(ix) + 0.7f * jx) * kSpacing, height * 0.5f,
                                   kOriginZ + (static_cast<float>(iz) + 0.7f * jz) * kSpacing);
            blade.SetLocalScale(0.08f, height, 0.08f);
            auto* mr = blade.AddComponent<MeshRendererComponent>();
            mr->mesh = cube;
            mr->material = grass;
        }
    }
}

// 追従する層 (M96i): 固定の霧 (z -8〜24) の奥 (z 24〜) へ転がる球に、小さな 2D 層 (12 m 四方) が付いて動く。
// 固定の霧の外でも球の周りだけは霧と足跡が出る。球は物理の初速だけで動く (決定的)。場は最後に作る (4 個目 = 上限ちょうど)
void AddFieldDemoFollow(Scene& s, const AssetID sphere)
{
    constexpr float kStartZ = 18.0f;

    GameObject ball = s.CreateGameObject("FollowBall");
    ball.SetLocalPosition(0.0f, 0.45f, kStartZ);
    ball.SetLocalScale(0.8f, 0.8f, 0.8f);
    auto* mr = ball.AddComponent<MeshRendererComponent>();
    mr->mesh = sphere;
    mr->material = AssetID{ HashStr("pdemo_ball") };
    auto* col = ball.AddComponent<ColliderComponent>();
    col->shape = collidershape::kSphere;
    col->radius = 0.5f;
    col->friction = 0.0f;
    auto* rb = ball.AddComponent<RigidbodyComponent>();
    rb->useDensity = true;
    rb->velocity = { 0.0f, 0.0f, 2.5f };
    auto* src = ball.AddComponent<DisturbanceSourceComponent>();
    src->mode = 0;
    src->radius = 1.2f;
    src->strength = 0.5f;
    src->fullSpeed = 3.0f;
    src->densityDelta = -0.4f;
    src->trailDepth = 0.6f;

    GameObject field = s.CreateGameObject("FollowField");
    field.SetLocalPosition(0.0f, 0.0f, kStartZ);
    auto* f = field.AddComponent<DisturbanceFieldComponent>();
    f->shape = 0;
    f->cellsX = 48;
    f->cellsZ = 48;
    f->channels = kDisturbanceChannelDensity | kDisturbanceChannelTrail;
    f->densityKeep = 0.995f;
    f->fogDensity = 1.6f;
    f->followTarget = ball.Id();
    f->followMargin = 4;
}

} // namespace

// M96b: --field-demo。霧の広場: 平らな地面に 2D 層の場を 1 個置き、DisturbanceSource 付きの球 3 個を霧の中へ転がす。
// 通った筋の霧が晴れ、時間で戻る。要素は関数ごとに分けてあり、後のサブで足すもの (人・爆発・3D 煙など) は
// この関数の末尾へ足す (CPU 粒子の乱数ストリームは生成順に依存するので、前へ挿さない)
void BuildFieldShowcaseScene(EngineContext& ctx)
{
    Scene& s = *ctx.scene;
    s.SetName("field_showcase");
    const AssetID cube = ctx.resources->meshes.Cube();
    const AssetID sphere = ctx.resources->meshes.Sphere();

    AddCamera(s);
    AddLights(s);
    AddPhysicsEnvironment(s);
    AddGround(s, cube);
    AddField(s);
    AddRollingBall(s, sphere, "Ball_A", -3.0f, -10.5f, 4.0f);
    AddRollingBall(s, sphere, "Ball_B", 0.0f, -10.0f, 5.0f);
    AddRollingBall(s, sphere, "Ball_C", 3.0f, -9.5f, 3.5f);
    AddFieldDemoPerson(s, ctx.resources->meshes.Capsule());
    AddFieldDemoLanterns(s);
    AddFieldDemoPond(s, sphere);
    AddFieldDemoSmoke(s);
    AddFieldDemoSwing(ctx);
    AddFieldDemoGrass(ctx, s, cube);
    AddFieldDemoFollow(s, sphere);
    s.GetWorld().ApplyStructuralChanges();
}

} // namespace mye
