// ナビ・知覚・ビヘイビアツリーのショーケース (--nav-demo / --perception-demo / --bt-demo)。
// DemoContent.cpp から領域ごとの翻訳単位へ分けたもの (M93g)。関数の契約は DemoContent.h、
// ショーケースを跨いで使うヘルパーは DemoContentInternal.h。
#include "Engine/Engine/Demo/DemoContent.h"
#include "Engine/Engine/Demo/DemoContentInternal.h"

#include <cmath>

#include "Engine/Core/Ecs/ComponentRegistry.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Util/Hash.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Engine/Loop/EngineLoop.h"
#include "Engine/Engine/Navigation/NavBake.h"      // M82c: --nav-demo のベイク
#include "Engine/Engine/Navigation/NavMeshAsset.h" // M82c: メモリ上の登録
#include "Engine/Engine/Scene/TransformSystem.h"
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Renderer/Device/GpuResources.h"

namespace mye {

// M82c: --nav-demo。段差・坂・台・孤島・壁のある庭に Agent を 6 体置く。
// ★ナビメッシュはファイルを作らずシーン構築時にメモリ上で焼いて登録する (`nav://demo`)。
//   コミットした .mnav だとベイク方式の版 (kNavBakeVersion) を上げたときに古くなるが、
//   これなら Debug / Release / Server が独立に焼いて replay が一致すること自体が、
//   ベイクの構成間一致 (sub-02 の契約) を実行経路で証明する
void BuildNavShowcaseScene(EngineContext& ctx)
{
    Scene& s = *ctx.scene;
    World& w = s.GetWorld();
    RenderResources& res = *ctx.resources;
    s.SetName("nav_showcase");

    auto makeMat = [&](const char* name, float r, float g, float b) {
        Material m;
        m.shader = AssetID{ HashStr("forward_lit") };
        m.texture = res.textures.White();
        m.baseColor = { r, g, b, 1.0f };
        return res.materials.Register(name, m);
    };
    makeMat("navdemo_floor", 0.30f, 0.32f, 0.36f);
    makeMat("navdemo_block", 0.62f, 0.60f, 0.55f);
    makeMat("navdemo_wall", 0.45f, 0.30f, 0.28f);
    const AssetID agentColors[] = {
        makeMat("navdemo_agent0", 0.90f, 0.35f, 0.30f), makeMat("navdemo_agent1", 0.95f, 0.70f, 0.25f),
        makeMat("navdemo_agent2", 0.45f, 0.80f, 0.35f), makeMat("navdemo_agent3", 0.30f, 0.65f, 0.90f),
        makeMat("navdemo_agent4", 0.65f, 0.45f, 0.90f), makeMat("navdemo_agent5", 0.90f, 0.50f, 0.75f),
    };
    const AssetID cube = res.meshes.Cube();

    GameObject camera = s.CreateGameObject("Main Camera");
    camera.AddComponent<CameraComponent>();
    camera.SetLocalPosition(0.0f, 17.0f, -19.0f);
    camera.SetLocalRotationEuler(42.0f, 0.0f, 0.0f);

    GameObject sun = s.CreateGameObject("Sun");
    sun.AddComponent<LightComponent>();
    sun.SetLocalRotationEuler(50.0f, -30.0f, 0.0f);

    {
        GameObject envGo = s.CreateGameObject("Environment");
        auto* env = envGo.AddComponent<PhysicsEnvironmentComponent>();
        env->gravity = { 0.0f, -9.81f, 0.0f };
    }

    // 静的な箱 (見た目 = cube を拡大、当たり = 単位箱。Transform の scale が当たりにも掛かる)
    auto addBlock = [&](const char* name, DirectX::XMFLOAT3 center, DirectX::XMFLOAT3 half, const char* material,
                        const DirectX::XMFLOAT4* rotation = nullptr) {
        GameObject go = s.CreateGameObject(name);
        go.SetLocalPosition(center.x, center.y, center.z);
        go.SetLocalScale(half.x * 2.0f, half.y * 2.0f, half.z * 2.0f);
        if (rotation != nullptr) {
            go.GetComponent<LocalTransform>()->rotation = *rotation;
        }
        auto* mr = go.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr(material) };
        auto* col = go.AddComponent<ColliderComponent>();
        col->shape = collidershape::kBox;
        col->halfExtents = { 0.5f, 0.5f, 0.5f };
        return go;
    };

    // 庭の形状。NavAgentSelfTest の庭と同じ構成。段差は Surface の maxClimb ちょうど (0.3 m)、坂は 30 度
    constexpr float kPi = 3.14159265f;
    constexpr float kStepHeight = 0.3f;
    constexpr float kRampDeg = 30.0f;
    addBlock("Floor", { 0.0f, -0.5f, 0.0f }, { 12.0f, 0.5f, 12.0f }, "navdemo_floor");
    addBlock("Step", { -4.0f, kStepHeight * 0.5f, 0.0f }, { 1.5f, kStepHeight * 0.5f, 12.0f }, "navdemo_block");
    const float rampAngle = kRampDeg * kPi / 180.0f;
    const float sn = std::sin(rampAngle);
    const float cs = std::cos(rampAngle);
    const DirectX::XMFLOAT4 rampRotation = { 0.0f, 0.0f, std::sin(rampAngle * 0.5f), std::cos(rampAngle * 0.5f) };
    addBlock("Ramp", { 4.0f + 2.0f * cs + 0.1f * sn, 2.0f * sn - 0.1f * cs, 0.0f }, { 2.0f, 0.1f, 3.0f },
             "navdemo_block", &rampRotation);
    const float platformTop = 4.0f * sn;
    addBlock("Platform", { 9.0f, platformTop * 0.5f, 0.0f }, { 1.8f, platformTop * 0.5f, 3.0f }, "navdemo_block");
    addBlock("Island", { 0.0f, 1.5f, 8.0f }, { 2.0f, 1.5f, 2.0f }, "navdemo_wall"); // 登れない高さ
    addBlock("WallA", { 1.0f, 1.0f, -6.0f }, { 0.4f, 1.0f, 3.0f }, "navdemo_wall");
    addBlock("WallB", { 6.0f, 1.0f, 8.0f }, { 3.0f, 1.0f, 0.4f }, "navdemo_wall");

    GameObject surfaceGo = s.CreateGameObject("NavMesh Surface");
    {
        auto* sf = surfaceGo.AddComponent<NavMeshSurfaceComponent>();
        sf->center = { 0.0f, 3.0f, 0.0f };
        sf->size = { 26.0f, 10.0f, 26.0f };
    }

    // Agent。左端に並べ、東の台 / 孤島 / 北東 / 南東 / 反対側の隅へ向かう。
    // NavDemoDriver (GameLogic) が 300 tick で出発点へ戻す (GameLogic.dll 未ロード = ヘッドレス selftest 等では戻らない)
    struct AgentSpec {
        float startZ;
        DirectX::XMFLOAT3 destination;
        int quality;
    };
    const AgentSpec specs[] = {
        { -7.0f, { 9.4f, platformTop, -1.0f }, 2 }, { -4.0f, { 9.4f, platformTop, 1.0f }, 3 },
        { -1.0f, { 0.0f, 3.0f, 8.0f }, 2 },         { 2.0f, { 10.0f, 0.0f, -9.0f }, 1 },
        { 5.0f, { 10.0f, 0.0f, 9.0f }, 2 },         { 8.5f, { -9.0f, 0.0f, -9.0f }, 3 },
    };
    const ComponentTypeId driver = ComponentRegistry::Get().FindByName("NavDemoDriver");
    int colorIndex = 0;
    for (const AgentSpec& spec : specs) {
        GameObject agent = s.CreateGameObject("NavAgent");
        agent.SetLocalPosition(-9.0f, 0.9f, spec.startZ);
        agent.AddComponent<CharacterControllerComponent>();
        auto* na = agent.AddComponent<NavMeshAgentComponent>();
        na->destination = spec.destination;
        na->hasDestination = true;
        na->avoidanceQuality = spec.quality;
        if (driver != kInvalidComponentType) {
            w.AddComponentRaw(agent.Id(), driver);
        }
        // 見た目は子へ逃がす (CC のカプセルは本体の scale を拾うので本体は無スケール)
        GameObject body = s.CreateGameObject("Body");
        body.SetParent(agent);
        body.SetLocalScale(0.6f, 0.9f, 0.6f);
        auto* mr = body.AddComponent<MeshRendererComponent>();
        mr->mesh = res.meshes.Capsule();
        mr->material = agentColors[colorIndex++ % 6];
    }

    // M82f: 実行時に NavMesh を切り抜く障害物。NavObstacleDriver (GameLogic) が tick で撤去・移動・再設置する。
    // 出発点の近くに置き、Agent の経路を迂回させる。コライダーを持たないのでベイクには入らない
    {
        GameObject obstacle = s.CreateGameObject("NavObstacle");
        obstacle.SetLocalPosition(-7.0f, 1.0f, -1.0f);
        auto* ob = obstacle.AddComponent<NavMeshObstacleComponent>();
        ob->size = { 1.0f, 2.0f, 4.0f };
        const ComponentTypeId obstacleDriver = ComponentRegistry::Get().FindByName("NavObstacleDriver");
        if (obstacleDriver != kInvalidComponentType) {
            w.AddComponentRaw(obstacle.Id(), obstacleDriver);
        }
        GameObject body = s.CreateGameObject("Body");
        body.SetParent(obstacle);
        body.SetLocalScale(ob->size.x, ob->size.y, ob->size.z);
        auto* mr = body.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr("navdemo_wall") };
    }

    // M82g: エリア 3 (高コスト) の帯。NavModifierDriver (GameLogic) が tick で動かし・歩行不可にし・戻す。
    // 左の Agent 群の進路にかぶせ、迂回させる。Obstacle と同様コライダーを持たないのでベイクには入らない
    {
        GameObject zone = s.CreateGameObject("NavModifier");
        zone.SetLocalPosition(-1.0f, 1.0f, 2.0f);
        auto* mod = zone.AddComponent<NavMeshModifierComponent>();
        mod->size = { 3.0f, 2.0f, 6.0f };
        mod->area = 3;
        surfaceGo.GetComponent<NavMeshSurfaceComponent>()->areaCosts[3] = 8.0f;
        const ComponentTypeId modifierDriver = ComponentRegistry::Get().FindByName("NavModifierDriver");
        if (modifierDriver != kInvalidComponentType) {
            w.AddComponentRaw(zone.Id(), modifierDriver);
        }
    }

    // M82h: 床から孤島 (登れない高さ 3 m) の上へ跳ぶ Jump の Link。双方向なので、300 tick の NavDemoDriver の
    // 目的地の戻しで島の上の Agent が跳び降りる。ベイクには入らず、NavSystem が tick の頭にタイルへ差し込む
    {
        GameObject link = s.CreateGameObject("NavLink");
        auto* nl = link.AddComponent<NavMeshLinkComponent>();
        nl->start = { 1.5f, 0.0f, 5.0f };
        nl->end = { 1.5f, 3.0f, 6.8f };
        nl->traversal = navlinktraversal::kJump;
        nl->jumpHeight = 1.0f;
    }

    // ベイクして登録する。入力収集はワールド行列を読むので Transform を一度確定させる
    w.ApplyStructuralChanges();
    TransformSystem transforms;
    transforms.Update(w);
    NavBakeInputs inputs;
    constexpr uint64_t kNavDemoGuid = HashStr("nav://demo");
    if (!NavPrepareBakeInputs(w, surfaceGo.Id(), inputs)) {
        MYE_LOG_ERROR("[nav-demo] cannot collect the bake input");
        return;
    }
    NavBakeOutput baked = NavBakeAsset(inputs.config, inputs.soup, inputs.clipBoxes, nullptr);
    if (baked.status != NavBakeStatus::Ok) {
        MYE_LOG_ERROR("[nav-demo] bake failed: %s", baked.message.c_str());
        return;
    }
    NavMeshAsset::RegisterInMemory(kNavDemoGuid, std::move(baked.data));
    w.GetComponent<NavMeshSurfaceComponent>(surfaceGo.Id())->navAsset = AssetID{ kNavDemoGuid };
}

// M83: --perception-demo。柱のある広場を侵入者が四角く歩き、見張り 3 体が知覚する。
//   Guard A: 正面に侵入者のルートがあり、柱の陰では見失う (視覚・遮蔽・見失う距離・予測)
//   Guard B: ルートの脇に背を向けて立ち、tick 240 ごろ横を通り過ぎる侵入者に触れられて気付く (接触)
//   Guard C: ルートに背を向けて立ち、足音 (ReportNoise) と tick 360 の攻撃 (ReportDamage) で気付く (聴覚・ダメージ)
// 侵入者の移動・音・攻撃は PerceptionDemoIntruder、見張りが知覚した方へ向くのは PerceptionDemoGuard (どちらも GameLogic)。
// GameLogic.dll が無い構成 (ヘッドレスの selftest など) では誰も動かず、Guard A が最初の位置の侵入者を見るだけ
void BuildPerceptionShowcaseScene(EngineContext& ctx)
{
    Scene& s = *ctx.scene;
    World& w = s.GetWorld();
    RenderResources& res = *ctx.resources;
    s.SetName("perception_showcase");

    auto makeMat = [&](const char* name, float r, float g, float b) {
        Material m;
        m.shader = AssetID{ HashStr("forward_lit") };
        m.texture = res.textures.White();
        m.baseColor = { r, g, b, 1.0f };
        return res.materials.Register(name, m);
    };
    makeMat("percdemo_floor", 0.30f, 0.32f, 0.36f);
    makeMat("percdemo_pillar", 0.55f, 0.52f, 0.48f);
    const AssetID intruderMat = makeMat("percdemo_intruder", 0.95f, 0.75f, 0.25f);
    const AssetID guardMat = makeMat("percdemo_guard", 0.35f, 0.55f, 0.90f);
    const AssetID noseMat = makeMat("percdemo_nose", 0.90f, 0.30f, 0.30f);
    const AssetID cube = res.meshes.Cube();

    GameObject camera = s.CreateGameObject("Main Camera");
    camera.AddComponent<CameraComponent>();
    camera.SetLocalPosition(0.0f, 22.0f, -20.0f);
    camera.SetLocalRotationEuler(50.0f, 0.0f, 0.0f);

    GameObject sun = s.CreateGameObject("Sun");
    sun.AddComponent<LightComponent>();
    sun.SetLocalRotationEuler(50.0f, -30.0f, 0.0f);

    {
        GameObject envGo = s.CreateGameObject("Environment");
        auto* env = envGo.AddComponent<PhysicsEnvironmentComponent>();
        env->gravity = { 0.0f, -9.81f, 0.0f };
    }

    auto addBlock = [&](const char* name, DirectX::XMFLOAT3 center, DirectX::XMFLOAT3 half, const char* material) {
        GameObject go = s.CreateGameObject(name);
        go.SetLocalPosition(center.x, center.y, center.z);
        go.SetLocalScale(half.x * 2.0f, half.y * 2.0f, half.z * 2.0f);
        auto* mr = go.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr(material) };
        auto* col = go.AddComponent<ColliderComponent>();
        col->shape = collidershape::kBox;
        col->halfExtents = { 0.5f, 0.5f, 0.5f };
        return go;
    };
    addBlock("Floor", { 0.0f, -0.5f, 0.0f }, { 15.0f, 0.5f, 15.0f }, "percdemo_floor");
    addBlock("Pillar", { 0.0f, 1.5f, -4.0f }, { 1.0f, 1.5f, 1.0f }, "percdemo_pillar");
    addBlock("Pillar 2", { -4.0f, 1.5f, 4.0f }, { 0.8f, 1.5f, 0.8f }, "percdemo_pillar");

    // CC の中心は足元から 0.9 m 上 (既定の高さ 1.8)。目と見られる点は中心からの高さで書く
    constexpr float kCcHalfHeight = 0.9f;
    auto addCapsuleBody = [&](GameObject owner, AssetID material, bool nose) {
        GameObject body = s.CreateGameObject("Body");
        body.SetParent(owner);
        body.SetLocalScale(0.6f, 0.9f, 0.6f);
        auto* mr = body.AddComponent<MeshRendererComponent>();
        mr->mesh = res.meshes.Capsule();
        mr->material = material;
        if (nose) {
            // 前 (+Z) を示す鼻。見張りがどちらを向いているかが絵で分かる
            GameObject tip = s.CreateGameObject("Nose");
            tip.SetParent(owner);
            tip.SetLocalPosition(0.0f, 0.7f, 0.35f);
            tip.SetLocalScale(0.15f, 0.15f, 0.3f);
            auto* nmr = tip.AddComponent<MeshRendererComponent>();
            nmr->mesh = cube;
            nmr->material = noseMat;
        }
    };

    // 侵入者 (プレイヤー役)。陣営 0
    {
        GameObject intruder = s.CreateGameObject("Intruder");
        intruder.SetLocalPosition(-8.0f, kCcHalfHeight, -8.0f);
        intruder.AddComponent<CharacterControllerComponent>();
        auto* src = intruder.AddComponent<AIStimulusSourceComponent>();
        src->faction = 0;
        src->targetHeight = 0.6f;
        const ComponentTypeId driver = ComponentRegistry::Get().FindByName("PerceptionDemoIntruder");
        if (driver != kInvalidComponentType) {
            w.AddComponentRaw(intruder.Id(), driver);
        }
        addCapsuleBody(intruder, intruderMat, false);
    }

    // 見張り。陣営 1 (仲間どうしは知覚しない)。yaw 0 = +Z を向く
    struct GuardSpec {
        const char* name;
        DirectX::XMFLOAT3 pos;
        float yawDeg;
    };
    const GuardSpec guards[] = {
        { "Guard A", { 0.0f, kCcHalfHeight, -12.0f }, 0.0f },
        { "Guard B", { 2.0f, kCcHalfHeight, -8.6f }, 180.0f }, // ルートの 0.6 m 脇でルートに背を向ける
        { "Guard C", { -12.0f, kCcHalfHeight, 0.0f }, -90.0f },
    };
    const ComponentTypeId guardDriver = ComponentRegistry::Get().FindByName("PerceptionDemoGuard");
    for (const GuardSpec& g : guards) {
        GameObject guard = s.CreateGameObject(g.name);
        guard.SetLocalPosition(g.pos.x, g.pos.y, g.pos.z);
        guard.SetLocalRotationEuler(0.0f, g.yawDeg, 0.0f);
        guard.AddComponent<CharacterControllerComponent>();
        auto* p = guard.AddComponent<AIPerceptionComponent>();
        p->eyeHeight = 0.7f;
        p->sightRadius = 14.0f;
        p->loseSightRadius = 17.0f;
        p->fovDeg = 90.0f;
        p->hearingRange = 22.0f;
        p->faction = 1;
        p->hostileMask = 1u << 0;
        p->forgetTicks = 300;
        p->predictionTicks = 90;
        auto* src = guard.AddComponent<AIStimulusSourceComponent>();
        src->faction = 1;
        src->targetHeight = 0.6f;
        if (guardDriver != kInvalidComponentType) {
            w.AddComponentRaw(guard.Id(), guardDriver);
        }
        addCapsuleBody(guard, guardMat, true);
    }
}

// M85: --bt-demo。壁のある広場を見張り 2 体 (NavMeshAgent + AIPerception + BehaviorTree + AnimatorController) が巡回し、
// プレイヤー役が見張り A の視界へ入って見つかり、壁の向こうへ逃げて見失わせる。
//   巡回 -> 発見 (僚機 B へ GuardAlert を送る) -> 追跡 -> 見失う -> 捜索 -> 巡回へ戻る、が 1 回の実行に入る
// 木は assets\ai\guard.bt.json (+ guard.bb.json / guard_sense.bt.json)。A の巡回ルートは BehaviorTreeComponent の
// Entity キーの初期値で、僚機 B のルートだけ GameLogic の BtDemoDriver が BtSetBlackboard (ABI v27) で渡す。
// プレイヤー役の動きと段階のログは BtDemoDriver (GameLogic、固定 tick で決まる)。
// GameLogic.dll が無い構成 (ヘッドレスの selftest など) では誰も動かず、B は巡回しない。C# のタスクは使わない (C# レーンは replay の被覆外)
// ★ナビメッシュは --nav-demo と同じくメモリ上で焼いて登録する (`nav://bt-demo`)
void BuildBtShowcaseScene(EngineContext& ctx)
{
    Scene& s = *ctx.scene;
    World& w = s.GetWorld();
    RenderResources& res = *ctx.resources;
    s.SetName("bt_showcase");

    // .meta の GUID (assets\ai\guard.bt.json.meta / guard.controller.json.meta)
    constexpr uint64_t kGuardTreeGuid = 0x5d2e9a4c71b83f06ull;
    constexpr uint64_t kAnimatorControllerGuid = 0xc91d4f27e6a05b83ull;

    auto makeMat = [&](const char* name, float r, float g, float b) {
        Material m;
        m.shader = AssetID{ HashStr("forward_lit") };
        m.texture = res.textures.White();
        m.baseColor = { r, g, b, 1.0f };
        return res.materials.Register(name, m);
    };
    makeMat("btdemo_floor", 0.30f, 0.32f, 0.36f);
    makeMat("btdemo_wall", 0.45f, 0.30f, 0.28f);
    const AssetID playerMat = makeMat("btdemo_player", 0.95f, 0.75f, 0.25f);
    const AssetID guardMatA = makeMat("btdemo_guard_a", 0.35f, 0.55f, 0.90f);
    const AssetID guardMatB = makeMat("btdemo_guard_b", 0.45f, 0.85f, 0.55f);
    const AssetID noseMat = makeMat("btdemo_nose", 0.90f, 0.30f, 0.30f);
    const AssetID routeMat = makeMat("btdemo_route", 0.90f, 0.90f, 0.30f);
    const AssetID cube = res.meshes.Cube();

    GameObject camera = s.CreateGameObject("Main Camera");
    camera.AddComponent<CameraComponent>();
    camera.SetLocalPosition(0.0f, 26.0f, -17.0f);
    camera.SetLocalRotationEuler(58.0f, 0.0f, 0.0f);

    GameObject sun = s.CreateGameObject("Sun");
    sun.AddComponent<LightComponent>();
    sun.SetLocalRotationEuler(50.0f, -30.0f, 0.0f);

    {
        GameObject envGo = s.CreateGameObject("Environment");
        auto* env = envGo.AddComponent<PhysicsEnvironmentComponent>();
        env->gravity = { 0.0f, -9.81f, 0.0f };
    }

    auto addBlock = [&](const char* name, DirectX::XMFLOAT3 center, DirectX::XMFLOAT3 half, const char* material) {
        GameObject go = s.CreateGameObject(name);
        go.SetLocalPosition(center.x, center.y, center.z);
        go.SetLocalScale(half.x * 2.0f, half.y * 2.0f, half.z * 2.0f);
        auto* mr = go.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr(material) };
        auto* col = go.AddComponent<ColliderComponent>();
        col->shape = collidershape::kBox;
        col->halfExtents = { 0.5f, 0.5f, 0.5f };
        return go;
    };
    addBlock("Floor", { 0.0f, -0.5f, 0.0f }, { 14.0f, 0.5f, 14.0f }, "btdemo_floor");
    // 東西に長い壁。西端 (x = -4) の手前だけが通り道で、南側の見張りの視線を北側から切る
    addBlock("Wall", { 5.0f, 1.5f, 0.0f }, { 9.0f, 1.5f, 0.4f }, "btdemo_wall");

    GameObject surfaceGo = s.CreateGameObject("NavMesh Surface");
    {
        auto* sf = surfaceGo.AddComponent<NavMeshSurfaceComponent>();
        sf->center = { 0.0f, 3.0f, 0.0f };
        sf->size = { 30.0f, 10.0f, 30.0f };
    }

    // CC の中心は足元から 0.9 m 上 (既定の高さ 1.8)
    constexpr float kCcHalfHeight = 0.9f;
    auto addCapsuleBody = [&](GameObject owner, AssetID material, bool nose) {
        GameObject body = s.CreateGameObject("Body");
        body.SetParent(owner);
        body.SetLocalScale(0.6f, 0.9f, 0.6f);
        auto* mr = body.AddComponent<MeshRendererComponent>();
        mr->mesh = res.meshes.Capsule();
        mr->material = material;
        if (nose) {
            // 前 (+Z) を示す鼻。見張りがどちらを向いているかが絵で分かる
            GameObject tip = s.CreateGameObject("Nose");
            tip.SetParent(owner);
            tip.SetLocalPosition(0.0f, 0.7f, 0.35f);
            tip.SetLocalScale(0.15f, 0.15f, 0.3f);
            auto* nmr = tip.AddComponent<MeshRendererComponent>();
            nmr->mesh = cube;
            nmr->material = noseMat;
        }
    };

    // 巡回ルート。点はエンティティのローカル座標で、ルートは原点に置くのでワールドと同じ
    auto addRoute = [&](const char* name, std::initializer_list<DirectX::XMFLOAT3> points) {
        GameObject route = s.CreateGameObject(name);
        auto* pr = route.AddComponent<PatrolRouteComponent>();
        pr->mode = patrolmode::kLoop;
        pr->pointCount = static_cast<int32_t>(points.size());
        int index = 0;
        for (const DirectX::XMFLOAT3& p : points) {
            pr->points[index] = p;
            pr->waitTicks[index] = 20;
            // 点の目印 (見た目だけ)
            GameObject mark = s.CreateGameObject("Mark");
            mark.SetParent(route);
            mark.SetLocalPosition(p.x, 0.05f, p.z);
            mark.SetLocalScale(0.5f, 0.1f, 0.5f);
            auto* mmr = mark.AddComponent<MeshRendererComponent>();
            mmr->mesh = cube;
            mmr->material = routeMat;
            ++index;
        }
        return route;
    };
    GameObject routeA = addRoute("Route A", { { -11.0f, 0.0f, -10.0f }, { -3.0f, 0.0f, -10.0f }, { -3.0f, 0.0f, -4.0f },
                                              { -11.0f, 0.0f, -4.0f } });
    addRoute("Route B", { { 11.0f, 0.0f, -10.0f }, { 11.0f, 0.0f, -4.0f }, { 5.0f, 0.0f, -4.0f }, { 5.0f, 0.0f, -10.0f } });

    // 見張り。陣営 1、プレイヤー (陣営 0) だけが敵。B は視界を狭くして、A の知らせ (GuardAlert) で動く形にする
    struct GuardSpec {
        const char* name;
        DirectX::XMFLOAT3 pos;
        float sightRadius;
        float loseSightRadius;
        float fovDeg;
        AssetID material;
    };
    const GuardSpec guards[] = {
        { "Guard A", { -11.0f, kCcHalfHeight, -10.0f }, 12.0f, 14.0f, 120.0f, guardMatA },
        { "Guard B", { 11.0f, kCcHalfHeight, -10.0f }, 6.0f, 8.0f, 90.0f, guardMatB },
    };
    std::vector<GameObject> guardObjects;
    for (const GuardSpec& g : guards) {
        GameObject guard = s.CreateGameObject(g.name);
        guard.SetLocalPosition(g.pos.x, g.pos.y, g.pos.z);
        guard.AddComponent<CharacterControllerComponent>();
        auto* agent = guard.AddComponent<NavMeshAgentComponent>();
        agent->avoidanceQuality = 1;
        auto* p = guard.AddComponent<AIPerceptionComponent>();
        p->eyeHeight = 0.7f;
        p->sightRadius = g.sightRadius;
        p->loseSightRadius = g.loseSightRadius;
        p->fovDeg = g.fovDeg;
        p->faction = 1;
        p->hostileMask = 1u << 0;
        p->forgetTicks = 300;
        p->predictionTicks = 60;
        guard.AddComponent<AnimatorControllerComponent>()->controller = AssetID{ kAnimatorControllerGuid };
        auto* bt = guard.AddComponent<BehaviorTreeComponent>();
        bt->tree = AssetID{ kGuardTreeGuid };
        bt->drawDebug = guardObjects.empty(); // 画面確認用に A だけ SceneView へ実行中のタスクを出す
        addCapsuleBody(guard, g.material, true);
        guardObjects.push_back(guard);
    }
    // Entity キーの初期値: A は route と buddy (= B)、B は buddy (= A)。B の route は BtDemoDriver が ABI で渡す
    {
        auto* btA = guardObjects[0].GetComponent<BehaviorTreeComponent>();
        std::snprintf(btA->bbEntityKey[0], kBtEntityKeyBytes, "%s", "route");
        btA->bbEntityValue[0] = routeA.Id();
        std::snprintf(btA->bbEntityKey[1], kBtEntityKeyBytes, "%s", "buddy");
        btA->bbEntityValue[1] = guardObjects[1].Id();
        auto* btB = guardObjects[1].GetComponent<BehaviorTreeComponent>();
        std::snprintf(btB->bbEntityKey[0], kBtEntityKeyBytes, "%s", "buddy");
        btB->bbEntityValue[0] = guardObjects[0].Id();
    }

    // プレイヤー役。陣営 0。動きは BtDemoDriver
    {
        GameObject player = s.CreateGameObject("Player");
        player.SetLocalPosition(-11.0f, kCcHalfHeight, 6.0f);
        player.AddComponent<CharacterControllerComponent>();
        auto* src = player.AddComponent<AIStimulusSourceComponent>();
        src->faction = 0;
        src->targetHeight = 0.6f;
        const ComponentTypeId driver = ComponentRegistry::Get().FindByName("BtDemoDriver");
        if (driver != kInvalidComponentType) {
            w.AddComponentRaw(player.Id(), driver);
        }
        addCapsuleBody(player, playerMat, false);
    }

    // ベイクして登録する (入力収集はワールド行列を読むので Transform を一度確定させる)
    w.ApplyStructuralChanges();
    TransformSystem transforms;
    transforms.Update(w);
    NavBakeInputs inputs;
    constexpr uint64_t kBtDemoNavGuid = HashStr("nav://bt-demo");
    if (!NavPrepareBakeInputs(w, surfaceGo.Id(), inputs)) {
        MYE_LOG_ERROR("[bt-demo] cannot collect the bake input");
        return;
    }
    NavBakeOutput baked = NavBakeAsset(inputs.config, inputs.soup, inputs.clipBoxes, nullptr);
    if (baked.status != NavBakeStatus::Ok) {
        MYE_LOG_ERROR("[bt-demo] bake failed: %s", baked.message.c_str());
        return;
    }
    NavMeshAsset::RegisterInMemory(kBtDemoNavGuid, std::move(baked.data));
    w.GetComponent<NavMeshSurfaceComponent>(surfaceGo.Id())->navAsset = AssetID{ kBtDemoNavGuid };
}

} // namespace mye
