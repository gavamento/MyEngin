//====================================================================================
//                          SpringFollowSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                       11/04/2026
//                                          ばね追従 (SpringFollow、M94g) の回帰テスト実装
//====================================================================================
#include "Engine/Engine/Feel/SpringFollowSelfTest.h"

#include <cmath>

#include <DirectXMath.h>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Feel/SpringFollowSystem.h"
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/Scene/Scene.h"

using namespace DirectX;

namespace mye {

bool RunSpringFollowSelfTest()
{
    MYE_LOG_INFO("==== SpringFollow self test ====");
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
    constexpr float kDt = 1.0f / 60.0f;

    {
        Scene scene;
        World& w = scene.GetWorld();
        GameObject target = scene.CreateGameObjectTracked("Target");
        target.AddComponent<LocalTransform>()->position = { 10.0f, 0.0f, 0.0f };
        GameObject follower = scene.CreateGameObjectTracked("Follower");
        follower.AddComponent<LocalTransform>();
        auto* sf = follower.AddComponent<SpringFollowComponent>();
        sf->target = target.Id();
        sf->frequencyHz = 3.0f;
        sf->dampingRatio = 1.0f;
        sf->rotationFrequencyHz = 0.0f;
        w.ApplyStructuralChanges();

        check(UpdateSpringFollow(w, kDt) == 1 && w.GetComponent<LocalTransform>(follower.Id())->position.x == 10.0f
                  && w.GetComponent<SpringFollowComponent>(follower.Id())->initialized == 1,
              "the first update snaps to the target");
        w.GetComponent<LocalTransform>(target.Id())->position.x = 20.0f;
        float maxX = 0.0f;
        for (int i = 0; i < 180; ++i) {
            UpdateSpringFollow(w, kDt);
            maxX = std::max(maxX, w.GetComponent<LocalTransform>(follower.Id())->position.x);
        }
        const float x = w.GetComponent<LocalTransform>(follower.Id())->position.x;
        check(maxX <= 20.0f + 1e-3f && std::fabs(x - 20.0f) < 1e-2f, "critical damping catches up without overshoot");

        // 相手の空間の offset: 相手が Y 軸まわりに 90 度回ると、前 (+z) のずらしは +x へ向く
        XMStoreFloat4(&w.GetComponent<LocalTransform>(target.Id())->rotation,
                      XMQuaternionRotationRollPitchYaw(0.0f, XM_PIDIV2, 0.0f));
        auto* sf2 = w.GetComponent<SpringFollowComponent>(follower.Id());
        sf2->offset = { 0.0f, 0.0f, 2.0f };
        sf2->initialized = 0;
        UpdateSpringFollow(w, kDt);
        const XMFLOAT3 p = w.GetComponent<LocalTransform>(follower.Id())->position;
        check(std::fabs(p.x - 22.0f) < 1e-4f && std::fabs(p.z) < 1e-4f, "the offset is expressed in the target's space");

        // 相手が居ない / 自分自身なら何もしない
        sf2 = w.GetComponent<SpringFollowComponent>(follower.Id());
        sf2->target = follower.Id();
        check(UpdateSpringFollow(w, kDt) == 0, "following itself does nothing");
        sf2 = w.GetComponent<SpringFollowComponent>(follower.Id());
        sf2->target = kNullEntity;
        check(UpdateSpringFollow(w, kDt) == 0, "no target does nothing");
    }

    // 親を持つ追従側: 目標は親の空間へ直して LocalTransform に書く
    {
        Scene scene;
        World& w = scene.GetWorld();
        GameObject target = scene.CreateGameObjectTracked("Target");
        target.AddComponent<LocalTransform>()->position = { 3.0f, 1.0f, 0.0f };
        GameObject parent = scene.CreateGameObjectTracked("Parent");
        parent.AddComponent<LocalTransform>()->position = { 5.0f, 0.0f, 0.0f };
        GameObject child = scene.CreateGameObjectTracked("Child");
        child.AddComponent<LocalTransform>();
        auto* sf = child.AddComponent<SpringFollowComponent>();
        sf->target = target.Id();
        sf->rotationFrequencyHz = 0.0f;
        w.ApplyStructuralChanges();
        w.SetParent(child.Id(), parent.Id());
        w.ApplyStructuralChanges();
        UpdateSpringFollow(w, kDt);
        const XMFLOAT3 p = w.GetComponent<LocalTransform>(child.Id())->position;
        check(std::fabs(p.x + 2.0f) < 1e-4f && std::fabs(p.y - 1.0f) < 1e-4f,
              "a follower with a parent writes the goal in its parent's space");
    }

    // 回転の追従: 0 Hz は追わない / 正の Hz は寄っていく
    {
        Scene scene;
        World& w = scene.GetWorld();
        GameObject target = scene.CreateGameObjectTracked("Target");
        XMStoreFloat4(&target.AddComponent<LocalTransform>()->rotation, XMQuaternionRotationRollPitchYaw(0.0f, 1.0f, 0.0f));
        GameObject follower = scene.CreateGameObjectTracked("Follower");
        follower.AddComponent<LocalTransform>();
        auto* sf = follower.AddComponent<SpringFollowComponent>();
        sf->target = target.Id();
        sf->rotationFrequencyHz = 2.0f;
        sf->initialized = 1; // 瞬間移動を飛ばして寄り方を見る
        w.ApplyStructuralChanges();
        const XMVECTOR goal = XMQuaternionRotationRollPitchYaw(0.0f, 1.0f, 0.0f);
        const auto angleTo = [&]() {
            const XMVECTOR q = XMLoadFloat4(&w.GetComponent<LocalTransform>(follower.Id())->rotation);
            return std::fabs(XMVectorGetX(XMQuaternionDot(q, goal)));
        };
        const float before = angleTo();
        UpdateSpringFollow(w, kDt);
        const float after1 = angleTo();
        for (int i = 0; i < 240; ++i) {
            UpdateSpringFollow(w, kDt);
        }
        check(after1 > before && angleTo() > 0.9999f, "rotation converges toward the target");
    }

    return failCount == 0;
}

} // namespace mye
