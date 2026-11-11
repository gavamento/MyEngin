//====================================================================================
//                          CameraSelectSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                       11/11/2026
//                                          描画するカメラの選択規則 (CameraOwner、M101) の回帰テストの実装
//====================================================================================
#include "Engine/Engine/Rendering/CameraSelectSelfTest.h"

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/ComponentRegistry.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Rendering/CameraSelect.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/Scene/Scene.h"

namespace mye {

bool RunCameraSelectSelfTest()
{
    MYE_LOG_INFO("==== CameraSelect self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };
    RegisterBuiltinComponents();

    // ---- (1) 登録: TypeId 101、NoHash ----
    {
        const ComponentTypeId id = CameraOwnerComponent::sTypeId;
        check(id == 101 && ComponentRegistry::Get().FindByName("CameraOwner") == id,
              "component: registered as CameraOwner with TypeId 101");
        check((ComponentRegistry::Get().Desc(id).flags & kComponentNoHash) != 0, "component: CameraOwner is kComponentNoHash");
    }

    // 選ばれたカメラのエンティティ
    auto pick = [](World& w, uint32_t lane) {
        EntityID got = kNullEntity;
        camselect::ForEachCameraPick(w, lane, [&](Archetype& arch, uint32_t row) { got = arch.EntityAt(row); });
        return got;
    };

    Scene scene;
    World& w = scene.GetWorld();
    auto addCamera = [&](const char* name, bool primary) {
        GameObject go = scene.CreateGameObjectTracked(name);
        w.ApplyStructuralChanges();
        w.AddComponent<CameraComponent>(go.Id());
        if (!w.GetComponent<WorldMatrixComponent>(go.Id())) {
            w.AddComponent<WorldMatrixComponent>(go.Id());
        }
        w.ApplyStructuralChanges();
        auto* c = w.GetComponent<CameraComponent>(go.Id());
        if (c) {
            c->isPrimary = primary;
        }
        return go.Id();
    };

    // ---- (2) CameraOwner が無いシーンは従来どおり (どのレーンでも同じ primary) ----
    const EntityID shared = addCamera("Shared", false);
    const EntityID fp0 = addCamera("Fp0", true);
    check(pick(w, 0) == fp0 && pick(w, 1) == fp0, "no owner: the primary camera is chosen on every lane");

    // ---- (3) 持ち主付き: そのレーンのマシンでだけ候補になる。付けてもハッシュは動かない ----
    auto own = [&](EntityID e, int32_t lane) {
        w.AddComponent<CameraOwnerComponent>(e);
        w.ApplyStructuralChanges();
        w.GetComponent<CameraOwnerComponent>(e)->lane = lane;
    };
    own(fp0, 0);
    const EntityID fp1 = addCamera("Fp1", true);
    own(fp1, 1);
    check(pick(w, 0) == fp0, "owner: lane 0 sees its own primary camera");
    check(pick(w, 1) == fp1, "owner: lane 1 sees its own primary camera");
    check(pick(w, 2) == shared, "owner: a lane with no camera of its own falls back to the shared camera");

    // 一人称 → 三人称の切り替え (isPrimary) はそのレーンの絵だけを変える
    const EntityID tp1 = addCamera("Tp1", false);
    own(tp1, 1);
    w.GetComponent<CameraComponent>(fp1)->isPrimary = false;
    w.GetComponent<CameraComponent>(tp1)->isPrimary = true;
    check(pick(w, 1) == tp1 && pick(w, 0) == fp0, "owner: switching lane 1 to third person leaves lane 0 alone");

    // 負のレーンは誰にも出ない
    w.GetComponent<CameraOwnerComponent>(tp1)->lane = -1;
    check(pick(w, 1) != tp1, "owner: a negative lane is never chosen");

    // ハッシュは CameraOwner の有無・値で動かない (カメラの増減は動くので、同じ構成で比べる)
    Scene a;
    Scene b;
    for (Scene* s : { &a, &b }) {
        GameObject go = s->CreateGameObjectTracked("Cam");
        s->GetWorld().ApplyStructuralChanges();
        s->GetWorld().AddComponent<CameraComponent>(go.Id());
        if (s == &b) {
            s->GetWorld().AddComponent<CameraOwnerComponent>(go.Id());
        }
        s->GetWorld().ApplyStructuralChanges();
        if (s == &b) {
            s->GetWorld().GetComponent<CameraOwnerComponent>(go.Id())->lane = 3;
        }
    }
    check(HashWorld(a.GetWorld()) == HashWorld(b.GetWorld()), "hash: adding CameraOwner never moves the world hash");

    MYE_LOG_INFO("==== CameraSelect self test: %s (%d failures) ====", failCount == 0 ? "PASS" : "FAIL", failCount);
    return failCount == 0;
}

} // namespace mye
