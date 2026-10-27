// Compute.ForEach のショーケース (--compute-demo)。
// DemoContent.cpp から領域ごとの翻訳単位へ分けたもの (M93g)。関数の契約は DemoContent.h、
// ショーケースを跨いで使うヘルパーは DemoContentInternal.h。
#include "Engine/Engine/Demo/DemoContent.h"
#include "Engine/Engine/Demo/DemoContentInternal.h"

#include <cstdio>

#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Util/Hash.h"
#include "Engine/Engine/Loop/EngineLoop.h"
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Renderer/Device/GpuResources.h"

namespace mye {

// M91i: --compute-demo。Compute.ForEach の 3 つの使い方を 1 枚に並べる。
//   中央の黄色い群れ 512 体  = C++ の State (ComputeDemoSwarm、CPU。結果が LocalTransform に入る = リプレイの照合に入る)
//   左で噴き出す火花 4096 粒 = C++ の Fx (ComputeDemoSparks、GPU 優先。DebugDrawLine にだけ描く = 照合に入らない)
//   右の青い群れ 48 体      = C# の State (ComputeCsSwarm、C# に変換した CPU 版。C# レーンは記録・検証中は止まる)
// 体の名前は DemoBoid_%03u / DemoCsBoid_%02u。スクリプトが名前で引いて位置を書く。
// GameLogic.dll / C# が無い構成 (ヘッドレスの selftest など) では体は動かない。生成順は他のデモと独立
void BuildComputeShowcaseScene(EngineContext& ctx)
{
    Scene& s = *ctx.scene;
    World& w = s.GetWorld();
    RenderResources& res = *ctx.resources;
    s.SetName("compute_showcase");

    constexpr uint32_t kBoidCount = 512; // ComputeDemoSwarm.cpp の kBoidCount と同じ
    constexpr uint32_t kCsBoidCount = 48; // ComputeCsSwarm.cs の Count と同じ

    auto makeMat = [&](const char* name, float r, float g, float b) {
        Material m;
        m.shader = AssetID{ HashStr("forward_lit") };
        m.texture = res.textures.White();
        m.baseColor = { r, g, b, 1.0f };
        return res.materials.Register(name, m);
    };
    const AssetID floorMat = makeMat("computedemo_floor", 0.22f, 0.24f, 0.28f);
    const AssetID boidMat = makeMat("computedemo_boid", 0.95f, 0.78f, 0.25f);
    const AssetID csBoidMat = makeMat("computedemo_csboid", 0.35f, 0.60f, 0.95f);
    const AssetID cube = res.meshes.Cube();

    GameObject camera = s.CreateGameObject("Main Camera");
    camera.AddComponent<CameraComponent>();
    camera.SetLocalPosition(0.0f, 4.0f, -15.0f);
    camera.SetLocalRotationEuler(6.0f, 0.0f, 0.0f);

    GameObject sun = s.CreateGameObject("Sun");
    sun.AddComponent<LightComponent>();
    sun.SetLocalRotationEuler(50.0f, -30.0f, 0.0f);

    GameObject floor = s.CreateGameObject("Floor");
    floor.SetLocalPosition(0.0f, -0.05f, 4.0f);
    floor.SetLocalScale(40.0f, 0.1f, 30.0f);
    {
        auto* mr = floor.AddComponent<MeshRendererComponent>();
        mr->mesh = cube;
        mr->material = floorMat;
    }

    auto addBoids = [&](const char* format, uint32_t count, AssetID material, float size) {
        char name[32];
        for (uint32_t i = 0; i < count; ++i) {
            std::snprintf(name, sizeof(name), format, i);
            GameObject boid = s.CreateGameObject(name);
            boid.SetLocalScale(size, size, size);
            auto* mr = boid.AddComponent<MeshRendererComponent>();
            mr->mesh = cube;
            mr->material = material;
        }
    };
    addBoids("DemoBoid_%03u", kBoidCount, boidMat, 0.2f);
    addBoids("DemoCsBoid_%02u", kCsBoidCount, csBoidMat, 0.25f);

    // スクリプトの持ち主。C++ のスクリプトは GameLogic.dll、C# のスクリプトは起動時のコンパイルで登録された後に付く
    GameObject swarm = s.CreateGameObject("Compute Swarm (C++ State)");
    AttachScriptIfRegistered(w, swarm.Id(), "ComputeDemoSwarm");
    GameObject sparks = s.CreateGameObject("Compute Sparks (C++ Fx)");
    AttachScriptIfRegistered(w, sparks.Id(), "ComputeDemoSparks");
    GameObject csSwarm = s.CreateGameObject("Compute Swarm (C# State)");
    AttachScriptIfRegistered(w, csSwarm.Id(), "ComputeCsSwarm");
    w.ApplyStructuralChanges();
}

} // namespace mye
