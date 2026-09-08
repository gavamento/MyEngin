// 実際のスクリプトを API スタブで実行する。独立実行なので Engine -> GameLogic 依存を作らない。
#include <cmath>
#include <cstdio>
#include <cstring>
#include "Shared/ScriptAPI.h"

namespace camera_test {
#include "../src/GameLogic/Scripts/WatcherFpsCamera.cpp"
}
namespace light_test {
#include "../src/GameLogic/Scripts/WatcherLightTool.cpp"
}

struct Harness {
    float x = 0.0f, y = 0.0f, lookX = 0.0f, lookY = 0.0f;
    bool run = false, crouch = false, light = false;
    MyeVec3 move = {};
    float gain = 0.0f, intensity = 0.0f;
    int breaths = 0;
    bool blocked = false;
    float safeRadius = 0.0f;
    MyeVec3 positions[8] = { { 0.0f, 1.1f, 0.0f }, { 0.0f, 0.0f, 1.2f } };
};

int main()
{
    Harness h;
    MyeEngineApi api = {};
    api.engine = &h;
    api.GetMouseDelta = [](void*, int32_t* x, int32_t* y) { *x = 0; *y = 0; };
    api.KeyDown = [](void*, uint8_t) { return 0; };
    api.GetAxisValue = [](void* p, uint64_t name) {
        const auto& s = *static_cast<Harness*>(p);
        if (name == MyeNameHash("MoveX")) return s.x;
        if (name == MyeNameHash("MoveY")) return s.y;
        if (name == MyeNameHash("WatcherLookX")) return s.lookX;
        if (name == MyeNameHash("WatcherLookY")) return s.lookY;
        return 0.0f;
    };
    api.GetActionState = [](void* p, uint64_t name) -> uint32_t {
        const auto& s = *static_cast<Harness*>(p);
        if (name == MyeNameHash("WatcherRun")) return s.run ? 1u : 0u;
        if (name == MyeNameHash("WatcherCrouch")) return s.crouch ? 1u : 0u;
        if (name == MyeNameHash("WatcherLight")) return s.light ? 1u : 0u;
        return 0u;
    };
    api.GetLocalPosition = [](void* p, MyeEntityId e, MyeVec3* out) {
        *out = static_cast<Harness*>(p)->positions[e.index]; return 1;
    };
    api.SetLocalPosition = [](void* p, MyeEntityId e, MyeVec3 v) {
        static_cast<Harness*>(p)->positions[e.index] = v; return 1;
    };
    api.GetLocalRotation = [](void*, MyeEntityId, MyeQuat* out) {
        *out = { 0.0f, 0.0f, 0.0f, 1.0f }; return 1;
    };
    api.SetLocalRotation = [](void*, MyeEntityId, MyeQuat) { return 1; };
    api.CharacterMove = [](void* p, MyeEntityId, MyeVec3 v) {
        static_cast<Harness*>(p)->move = v; return 1;
    };
    api.IsAlive = [](void*, MyeEntityId e) { return e.generation != 0 ? 1 : 0; };
    api.Raycast = [](void* p, MyeVec3, MyeVec3, float, MyeRaycastHit* hit) {
        hit->entity = { 7, 1 };
        return static_cast<Harness*>(p)->blocked ? 1 : 0;
    };
    api.SetComponentField = [](void* p, MyeEntityId, uint64_t, uint64_t field,
                                const void* value, int32_t size) -> int32_t {
        auto& s = *static_cast<Harness*>(p);
        float v = 0.0f;
        if (size == sizeof(v)) std::memcpy(&v, value, sizeof(v));
        if (field == MyeNameHash("footstepGain")) s.gain = v;
        if (field == MyeNameHash("intensity")) s.intensity = v;
        if (field == MyeNameHash("safeRadius")) s.safeRadius = v;
        if (field == MyeNameHash("pendingLoudness") && v > 0.0f) ++s.breaths;
        return 1;
    };
    api.Log = [](void*, int, const char*) {};
    MyeUpdateContext ctx = {};
    ctx.api = &api;
    ctx.dt = 1.0f / 60.0f;
    ctx.self = { 0, 1 };
    int failed = 0;
    int checks = 0;
    auto check = [&](bool ok, const char* name) {
        ++checks;
        if (!ok) { ++failed; std::printf("FAIL: %s\n", name); }
    };
    auto speed = [&]() { return std::sqrt(h.move.x * h.move.x + h.move.z * h.move.z); };
    camera_test::WatcherFpsCamera camera;
    h.y = 1.0f;
    camera.Update(ctx);
    const float straight = speed();
    const float walkGain = h.gain;
    h.x = 1.0f;
    camera.Update(ctx);
    check(std::abs(speed() - straight) < 0.00001f, "diagonal speed equals straight speed");
    h.x = 0.0f;
    h.y = 0.5f;
    camera.Update(ctx);
    check(std::abs(speed() - straight * 0.5f) < 0.00001f && h.gain < walkGain,
          "partial stick reduces both speed and acoustic gain");
    h.y = 1.0f;
    h.run = true;
    camera.Update(ctx);
    check(speed() > straight && h.gain > walkGain, "run is faster and louder than walk");
    h.crouch = true;
    camera.Update(ctx);
    check(speed() < straight && h.gain < walkGain, "crouch takes precedence over run");
    h.run = false;
    h.crouch = false;
    camera.pitchDeg = 75.0f;
    camera.Update(ctx);
    check(std::abs(speed() - straight) < 0.00001f, "looking up keeps horizontal speed");
    h.y = 0.0f;
    h.lookX = 1.0f;
    h.lookY = 1.0f;
    camera.pitchDeg = 0.0f;
    camera.Update(ctx);
    check(camera.yawDeg > 0.0f && camera.pitchDeg < 0.0f, "right stick controls both look axes");
    h.lookX = h.lookY = 0.0f;
    camera.breathPhase = 0;
    h.breaths = 0;
    h.light = true;
    for (int i = 0; i < 192; ++i) camera.Update(ctx);
    check(h.breaths == 2, "stationary breathing continues while light action is held");

    light_test::WatcherLightTool light;
    light.lamp0 = { 1, 1 };
    light.state0 = light_test::kLampPlaced;
    light.retrieveTicks = 3;
    light.Update(ctx);
    check(light.mode == 2 && h.intensity == light.lightIntensity,
          "retrieval starts without dimming the light");
    light.Update(ctx);
    check(light.state0 == light_test::kLampPlaced && h.intensity == light.lightIntensity,
          "placed light remains fully lit until retrieval finishes");
    h.x = 1.0f;
    light.Update(ctx);
    check(light.mode == 0 && light.state0 == light_test::kLampPlaced
              && h.intensity == light.lightIntensity, "movement cancels retrieval without losing light");
    h.x = 0.0f;
    for (int i = 0; i < 3; ++i) light.Update(ctx);
    check(light.state0 == light_test::kLampCarried && h.intensity == 0.0f,
          "completed retrieval stows the light");
    light.placeTicks = 3;
    light.Update(ctx);
    h.x = 1.0f;
    light.Update(ctx);
    check(light.mode == 0 && light.state0 == light_test::kLampCarried && h.intensity == 0.0f,
          "interrupted placement does not consume a carried light");
    h.x = h.y = 0.0f;
    light = {};
    light.lamp0 = { 1, 1 };
    light.lamp1 = { 2, 1 };
    light.lamp2 = { 3, 1 };
    light.placeTicks = 1;
    auto place = [&](float x) {
        h.positions[0] = { x, 1.1f, 0.0f };
        h.light = true;
        light.Update(ctx);
        h.light = false;
        light.Update(ctx);
    };
    place(10.0f);
    check(light.order0 == 1 && h.safeRadius == light.safeRadius,
          "only completed placement registers a checkpoint and safe radius");
    place(20.0f);
    place(30.0f);
    check(light.order0 == 3 && light.order1 == 2 && light.order2 == 1,
          "placement history follows completion order rather than entity slot");
    light.agent0 = { 4, 1 };
    h.positions[4] = h.positions[0];
    light.Update(ctx);
    check(light.caughtGrace == 0 && light.state2 == light_test::kLampPlaced,
          "completed light protects a player even with an enemy already inside");
    h.positions[0] = { 60.0f, 1.1f, 0.0f };
    h.positions[4] = h.positions[0];
    h.positions[4].x += 1.0f;
    h.blocked = true;
    light.Update(ctx);
    check(h.positions[0].x == 60.0f && light.caughtGrace == 0,
          "wall blocks capture inside the catch radius");
    h.blocked = false;
    light.Update(ctx);
    check(h.positions[0].x == 30.0f && light.state2 == light_test::kLampLost
              && light.state0 == light_test::kLampPlaced && light.state1 == light_test::kLampPlaced,
          "capture consumes only the latest remaining checkpoint and uses its safe footing");
    check(h.safeRadius == 0.0f && light.caughtGrace == 120 && speed() == 0.0f,
          "respawn removes the consumed sanctuary, stops movement and grants grace");
    h.positions[4] = h.positions[0];
    light.Update(ctx);
    check(light.caughtGrace == 119 && light.state1 == light_test::kLampPlaced,
          "grace prevents immediate repeat capture at the respawn point");
    check(light.ConsumeRespawnLight(ctx).x == 20.0f
              && light.ConsumeRespawnLight(ctx).x == 10.0f,
          "successive deaths select the next newest remaining light");
    light.state0 = light_test::kLampCarried;
    check(light.ConsumeRespawnLight(ctx).x == 10.0f && light.CarriedCount() == 1,
          "without a checkpoint fallback preserves all carried lights");

    // 最新を回収して置き直すと、古い2本の相対順も保持しなければならない。
    light = {};
    light.lamp0 = { 1, 1 }; light.lamp1 = { 2, 1 }; light.lamp2 = { 3, 1 };
    light.placeTicks = light.retrieveTicks = 1;
    place(10.0f); place(20.0f); place(30.0f);
    h.light = true;
    light.Update(ctx); // 最新の光2を回収
    check(light.state2 == light_test::kLampCarried && light.order2 == 0,
          "retrieval unregisters its checkpoint");
    place(40.0f);
    check(light.order0 == 3 && light.order1 == 2 && light.order2 == 1,
          "replacement compacts ranks without ties among older checkpoints");
    const auto saved = light; // 登録フィールドのみを使う復元も検査
    light_test::WatcherLightTool restored;
    for (const MyeScriptField& field : light_test::WatcherLightTool_mye_fields) {
        const auto next = [&]() -> size_t {
            if (field.type == MYE_FIELD_FLOAT3) return sizeof(MyeVec3);
            if (field.type == MYE_FIELD_ENTITYREF) return sizeof(MyeEntityId);
            if (field.type == MYE_FIELD_BOOL) return sizeof(bool);
            return sizeof(int32_t);
        };
        std::memcpy(reinterpret_cast<char*>(&restored) + field.offset,
                    reinterpret_cast<const char*>(&saved) + field.offset, next());
    }
    check(restored.ConsumeRespawnLight(ctx).x == 40.0f
              && restored.ConsumeRespawnLight(ctx).x == 20.0f,
          "reflected field restore retains checkpoint order and positions");
    h.blocked = true;
    check(!light.IsProtected(ctx, { 10.0f, 1.1f, 0.0f }), "light protection does not pass a wall");
    h.blocked = false;
    check(!light.IsProtected(ctx, { 10.0f, 10.0f, 0.0f }), "light protection does not cross floors");
    std::printf("Watcher rules: %d checks, %d failures\n", checks, failed);
    return failed == 0 ? 0 : 1;
}
