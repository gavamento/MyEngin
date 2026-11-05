// 手触りのショーケース (--feel-demo、M94i、ADR-035)。関数の契約は DemoContent.h、
// ショーケースを跨いで使うヘルパーは DemoContentInternal.h。
#include "Engine/Engine/Demo/DemoContent.h"
#include "Engine/Engine/Demo/DemoContentInternal.h"

#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Util/Hash.h"
#include "Engine/Engine/Feel/FeelLibrary.h"
#include "Engine/Engine/Loop/EngineLoop.h"
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Renderer/Device/GpuResources.h"

namespace mye {

namespace {

AssetID FindFeelPreset(const char* name)
{
    const FeelPreset* p = feellib::ResolveName(HashStr(name));
    return p != nullptr ? AssetID{ p->hash } : AssetID{}; // 未登録なら未割当 = 鳴らないだけ (デモは壊れない)
}

} // namespace

// M94i: --feel-demo。手触りの 3 つの口を 1 枚に並べる:
//   リグ "FeelRig" (FeelDemo スクリプトが左右へ往復させ、台本の tick でヒットストップ / スロー / 揺れ / プリセットを鳴らす)
//   カメラ "FeelCamera" (SpringFollow でリグを遅れて追う = 追従のばね)
//   ボール "FeelBall" (床で跳ねるたびに FeelTrigger がプリセット "Bump" を鳴らす = コード無しの経路)
// プリセットは assets\feel\*.feel.json (起動走査で登録済み)。GameLogic.dll が無い構成ではリグは動かない
void BuildFeelShowcaseScene(EngineContext& ctx)
{
    Scene& s = *ctx.scene;
    World& w = s.GetWorld();
    RenderResources& res = *ctx.resources;
    s.SetName("feel_showcase");
    const AssetID cube = res.meshes.Cube();
    const AssetID sphere = res.meshes.Sphere();

    GameObject sun = s.CreateGameObject("Sun");
    sun.AddComponent<LightComponent>();
    sun.SetLocalRotationEuler(50.0f, -30.0f, 0.0f);

    {
        GameObject envGo = s.CreateGameObject("Environment");
        auto* env = envGo.AddComponent<PhysicsEnvironmentComponent>();
        env->gravity = { 0.0f, -9.81f, 0.0f };
    }
    {
        GameObject floor = s.CreateGameObject("Floor");
        floor.SetLocalPosition(0.0f, -0.5f, 0.0f);
        floor.SetLocalScale(20.0f, 1.0f, 12.0f);
        auto* mr = floor.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr("pdemo_floor") };
        auto* col = floor.AddComponent<ColliderComponent>();
        col->shape = collidershape::kBox;
        col->halfExtents = { 0.5f, 0.5f, 0.5f };
        col->physMaterial = FindPhysMat("steel");
    }

    GameObject rig = s.CreateGameObject("FeelRig");
    {
        auto* mr = rig.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = AssetID{ HashStr("pdemo_buoy") };
        rig.SetLocalScale(0.6f, 0.6f, 0.6f);
        rig.SetLocalPosition(0.0f, 0.3f, 0.0f);
        AttachScriptIfRegistered(w, rig.Id(), "FeelDemo");
    }
    {
        GameObject camera = s.CreateGameObject("FeelCamera");
        camera.AddComponent<CameraComponent>();
        camera.SetLocalPosition(0.0f, 4.0f, -10.0f);
        camera.SetLocalRotationEuler(18.0f, 0.0f, 0.0f);
        auto* follow = camera.AddComponent<SpringFollowComponent>();
        follow->target = rig.Id();
        follow->offset = { 0.0f, 4.0f, -10.0f };
        follow->frequencyHz = 1.5f;
        follow->dampingRatio = 0.8f;    // わずかに行き過ぎて戻る (遅れの手触り)
        follow->rotationFrequencyHz = 0.0f; // 向きはカメラ自身のまま (リグは回らない)
    }
    {
        GameObject ball = s.CreateGameObject("FeelBall");
        ball.SetLocalPosition(2.0f, 6.0f, 2.0f);
        ball.SetLocalScale(0.6f, 0.6f, 0.6f);
        auto* mr = ball.AddComponent<MeshRendererComponent>();
        mr->mesh = sphere;
        mr->material = AssetID{ HashStr("pdemo_rubber") };
        auto* col = ball.AddComponent<ColliderComponent>();
        col->shape = collidershape::kSphere;
        col->radius = 0.5f;
        col->physMaterial = FindPhysMat("rubber");
        auto* rb = ball.AddComponent<RigidbodyComponent>();
        rb->useDensity = true;
        auto* trig = ball.AddComponent<FeelTriggerComponent>();
        trig->feel = FindFeelPreset("Bump");
        trig->minImpulse = 1.0f;
        trig->fullImpulse = 20.0f;
        trig->cooldownTicks = 10;
    }
    w.ApplyStructuralChanges();
}

} // namespace mye
