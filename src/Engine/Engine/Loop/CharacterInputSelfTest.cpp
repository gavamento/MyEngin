#include "Engine/Engine/Loop/CharacterInputSelfTest.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/ComponentRegistry.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Loop/CharacterInputSystem.h"
#include "Engine/Engine/Physics/Rigid/PhysicsSystem.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Platform/Input.h"
#include "Engine/Platform/InputActions.h"

namespace mye {

namespace {

constexpr float kDt = 1.0f / 60.0f;
constexpr uint8_t kVkSpace = 0x20;
constexpr uint8_t kVkA = 0x41;
constexpr uint8_t kVkD = 0x44;
constexpr uint8_t kVkJ = 0x4A;
constexpr uint8_t kVkL = 0x4C;
constexpr uint8_t kVkW = 0x57;
constexpr int kSettleTicks = 120;      // y=2 から落として着地が落ち着くまで
constexpr float kTakeoffSpeed = 4.0f;  // [m/s] 上向きの速度がこれを超えた tick を「跳んだ」と数える (jumpSpeed は 5)

// 既定の名前 (MoveX / MoveY / Jump) に、先行受付つきの JumpBuffered を足した入力マップ
constexpr const char* kActionsJson = R"({
  "actions": [
    { "name": "Jump", "keys": ["Space"] },
    { "name": "JumpBuffered", "keys": ["J"], "bufferTicks": 30 }
  ],
  "axes": [
    { "name": "MoveX", "posKey": "D", "negKey": "A", "padAxis": "LX" },
    { "name": "MoveY", "posKey": "W", "negKey": "S", "padAxis": "LY" }
  ]
})";

void SetKey(InputSnapshot& s, uint8_t vk, bool down)
{
    const uint8_t bit = static_cast<uint8_t>(1u << (vk & 7));
    if (down) {
        s.keys[vk >> 3] |= bit;
    } else {
        s.keys[vk >> 3] &= static_cast<uint8_t>(~bit);
    }
}

bool Near(float a, float b, float eps)
{
    return std::fabs(a - b) <= eps;
}

// 床 1 枚とキャラクター 1 体のシーンを、TickRunner と同じ順 (入力の評価 → 先行入力 → CharacterInput → 物理) で回す
struct Rig {
    Scene scene;
    PhysicsSystem physics;
    InputActions actions;
    InputSnapshot cur[kMaxPlayers] = {};
    InputSnapshot prev[kMaxPlayers] = {};
    uint32_t playerCount = 1;
    EntityID character = kNullEntity;

    Rig()
    {
        actions.LoadFromJsonText(kActionsJson);
        GameObject ground = scene.CreateGameObjectTracked("Ground");
        ground.SetLocalPosition(0.0f, -0.5f, 0.0f);
        auto* col = ground.AddComponent<ColliderComponent>();
        col->shape = collidershape::kBox;
        col->isTrigger = false;
        col->halfExtents = { 40.0f, 0.5f, 40.0f };
        GameObject ch = scene.CreateGameObjectTracked("Character");
        ch.SetLocalPosition(0.0f, 2.0f, 0.0f);
        ch.AddComponent<CharacterControllerComponent>();
        ch.AddComponent<CharacterInputComponent>();
        character = ch.Id();
        scene.GetWorld().ApplyStructuralChanges();
    }

    World& W() { return scene.GetWorld(); }
    CharacterInputComponent* Input() { return W().GetComponent<CharacterInputComponent>(character); }
    CharacterControllerComponent* Controller() { return W().GetComponent<CharacterControllerComponent>(character); }
    LocalTransform* Transform() { return W().GetComponent<LocalTransform>(character); }

    // stepSim = false は TimeControl のポーズ相当 (入力の評価だけ進み、CharacterInput と物理は走らない)
    uint32_t Tick(bool stepSim = true)
    {
        actions.Evaluate(cur, prev, playerCount);
        actions.UpdateBuffers(scene.InputBuffer());
        for (uint32_t p = 0; p < kMaxPlayers; ++p) {
            prev[p] = cur[p];
        }
        uint32_t driven = 0;
        if (stepSim) {
            driven = UpdateCharacterInput(W(), actions, cur, scene.InputBuffer(), playerCount, kDt);
            physics.Update(W(), kDt);
        }
        return driven;
    }

    void Settle()
    {
        W().ApplyStructuralChanges();
        for (int i = 0; i < kSettleTicks; ++i) {
            Tick();
        }
    }

    // ticks 回すあいだに「跳んだ」回数 (上向きの速度が閾値を下から超えた回数)
    int CountTakeoffs(int ticks)
    {
        int takeoffs = 0;
        bool rising = Controller()->velocity.y > kTakeoffSpeed;
        for (int i = 0; i < ticks; ++i) {
            Tick();
            const bool nowRising = Controller()->velocity.y > kTakeoffSpeed;
            if (nowRising && !rising) {
                ++takeoffs;
            }
            rising = nowRising;
        }
        return takeoffs;
    }
};

float YawDegOf(const DirectX::XMFLOAT4& q)
{
    return 2.0f * std::atan2(q.y, q.w) * (180.0f / 3.14159265358979f);
}

// y 軸まわりに yawDeg 回したクォータニオン (前方 +Z が (sin, 0, cos) を向く)
DirectX::XMFLOAT4 YawQuat(float yawDeg)
{
    const float half = yawDeg * (3.14159265358979f / 180.0f) * 0.5f;
    return { 0.0f, std::sin(half), 0.0f, std::cos(half) };
}

} // namespace

bool RunCharacterInputSelfTest()
{
    MYE_LOG_INFO("==== CharacterInput self test ====");
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

    // ---- (1) 登録と既定値 ----
    {
        CharacterInputComponent d;
        check(CharacterInputComponent::sTypeId == 99
                  && ComponentRegistry::Get().FindByName("CharacterInput") == CharacterInputComponent::sTypeId,
              "registered as CharacterInput with TypeId 99");
        check(d.mode == charinputmode::kWorldAxes3D && d.keyForward == 0x57 && d.keyBack == 0x53 && d.keyLeft == 0x41
                  && d.keyRight == 0x44 && d.keyJump == 0x20 && std::strcmp(d.moveXAxis, "MoveX") == 0
                  && std::strcmp(d.moveYAxis, "MoveY") == 0 && std::strcmp(d.jumpAction, "Jump") == 0,
              "defaults: world axes, WASD + Space, MoveX / MoveY / Jump");
    }

    // ---- (2) ワールド軸: D で +X、W で +Z、斜めでも速さは moveSpeed ----
    {
        Rig r;
        r.Settle();
        check(r.Controller()->isGrounded, "world axes: the character lands before the test starts");
        const float speed = r.Input()->moveSpeed;
        SetKey(r.cur[0], kVkD, true);
        const uint32_t driven = r.Tick();
        for (int i = 1; i < 60; ++i) {
            r.Tick();
        }
        check(driven == 1 && Near(r.Transform()->position.x, speed, 0.1f) && Near(r.Transform()->position.z, 0.0f, 0.02f),
              "world axes: D walks moveSpeed metres along +X in one second");
        SetKey(r.cur[0], kVkD, false);
        SetKey(r.cur[0], kVkW, true);
        const float z0 = r.Transform()->position.z;
        for (int i = 0; i < 60; ++i) {
            r.Tick();
        }
        check(Near(r.Transform()->position.z - z0, speed, 0.1f), "world axes: W walks along +Z");
        SetKey(r.cur[0], kVkD, true);
        r.Tick();
        const DirectX::XMFLOAT3 diagonal = r.Controller()->moveInput;
        check(Near(std::sqrt(diagonal.x * diagonal.x + diagonal.z * diagonal.z), speed, 1.0e-3f) && diagonal.x > 0.0f
                  && diagonal.z > 0.0f,
              "world axes: a diagonal is not faster than moveSpeed");
        SetKey(r.cur[0], kVkD, false);
        SetKey(r.cur[0], kVkW, false);
        r.Tick();
        check(r.Controller()->moveInput.x == 0.0f && r.Controller()->moveInput.z == 0.0f,
              "world axes: releasing the keys stops the character");
    }

    // ---- (3) 直接キーの変更と、actions.json の軸との合成 ----
    {
        Rig r;
        r.Settle();
        const float speed = r.Input()->moveSpeed;
        // 右キーを L に変えただけでは、MoveX の軸 (D) がまだ効く
        r.Input()->keyRight = kVkL;
        SetKey(r.cur[0], kVkD, true);
        r.Tick();
        check(Near(r.Controller()->moveInput.x, speed, 1.0e-4f), "remap: the MoveX axis still answers to D");
        // 軸名を空にすると直接キーだけになる
        r.Input()->moveXAxis[0] = '\0';
        r.Tick();
        check(r.Controller()->moveInput.x == 0.0f, "remap: with the axis name cleared, D no longer moves");
        SetKey(r.cur[0], kVkD, false);
        SetKey(r.cur[0], kVkL, true);
        r.Tick();
        check(Near(r.Controller()->moveInput.x, speed, 1.0e-4f), "remap: the new key L moves right");
        SetKey(r.cur[0], kVkL, false);
        // 直接キーを全部外すと軸だけで動く
        CharacterInputComponent axisOnly;
        axisOnly.keyForward = axisOnly.keyBack = axisOnly.keyLeft = axisOnly.keyRight = axisOnly.keyJump = charinputkey::kNone;
        *r.Input() = axisOnly;
        SetKey(r.cur[0], kVkA, true);
        r.Tick();
        check(Near(r.Controller()->moveInput.x, -speed, 1.0e-4f), "remap: with no direct keys, the axis alone moves left");
        // どちらにも無い名前 / キーは無入力
        std::snprintf(r.Input()->moveXAxis, sizeof(r.Input()->moveXAxis), "%s", "NoSuchAxis");
        r.Tick();
        check(r.Controller()->moveInput.x == 0.0f, "remap: an undefined axis name gives no input");
    }

    // ---- (4) カメラ相対: カメラの向きが前、進む方へ体が向く ----
    {
        Rig r;
        GameObject camera = r.scene.CreateGameObjectTracked("Camera");
        camera.SetLocalPosition(0.0f, 5.0f, -5.0f);
        camera.AddComponent<CameraComponent>();
        GameObject marker = r.scene.CreateGameObjectTracked("Marker");
        marker.SetLocalPosition(0.0f, 0.0f, 0.0f);
        r.Settle();
        const float speed = r.Input()->moveSpeed;
        r.W().GetComponent<LocalTransform>(camera.Id())->rotation = YawQuat(90.0f);    // 前方 = +X
        r.W().GetComponent<LocalTransform>(marker.Id())->rotation = YawQuat(180.0f);   // 前方 = -Z
        r.Input()->mode = charinputmode::kCameraRelative3D;
        r.Input()->turnSpeedDeg = 90.0f;
        SetKey(r.cur[0], kVkW, true);
        r.Tick();
        check(Near(r.Controller()->moveInput.x, speed, 1.0e-3f) && Near(r.Controller()->moveInput.z, 0.0f, 1.0e-3f),
              "camera relative: forward follows the primary camera's facing");
        check(Near(YawDegOf(r.Transform()->rotation), 90.0f * kDt, 0.01f),
              "camera relative: the body turns by at most turnSpeedDeg * dt per tick");
        for (int i = 0; i < 90; ++i) {
            r.Tick();
        }
        check(Near(YawDegOf(r.Transform()->rotation), 90.0f, 0.05f), "camera relative: the body ends up facing the movement");
        r.Input()->turnSpeedDeg = 0.0f;
        r.Input()->cameraEntity = marker.Id();
        r.Tick();
        check(Near(r.Controller()->moveInput.z, -speed, 1.0e-3f) && Near(r.Controller()->moveInput.x, 0.0f, 1.0e-3f),
              "camera relative: cameraEntity overrides the primary camera");
        check(Near(YawDegOf(r.Transform()->rotation), 90.0f, 0.05f), "camera relative: turnSpeedDeg 0 leaves the facing alone");
    }
    {
        Rig r; // カメラの無いシーン
        r.Settle();
        r.Input()->mode = charinputmode::kCameraRelative3D;
        SetKey(r.cur[0], kVkW, true);
        r.Tick();
        check(Near(r.Controller()->moveInput.z, r.Input()->moveSpeed, 1.0e-4f) && r.Controller()->moveInput.x == 0.0f,
              "camera relative: without a camera it falls back to world axes");
    }

    // ---- (5) 2D 横スクロール: 左右だけ ----
    {
        Rig r;
        r.Controller()->plane2D = true;
        r.Input()->mode = charinputmode::kSideScroll2D;
        r.Settle();
        SetKey(r.cur[0], kVkD, true);
        SetKey(r.cur[0], kVkW, true);
        for (int i = 0; i < 30; ++i) {
            r.Tick();
        }
        check(Near(r.Controller()->moveInput.x, r.Input()->moveSpeed, 1.0e-4f) && r.Controller()->moveInput.z == 0.0f
                  && r.Transform()->position.x > 1.0f && Near(r.Transform()->position.z, 0.0f, 1.0e-4f),
              "side scroll: left/right only, forward/back is ignored");
    }

    // ---- (6) ジャンプ: 押した瞬間に 1 回、押しっぱなしや空中では跳ばない ----
    {
        Rig r;
        r.Settle();
        SetKey(r.cur[0], kVkSpace, true);
        r.Tick();
        check(r.Controller()->velocity.y > kTakeoffSpeed, "jump: Space takes off on the tick it is pressed");
        // 押したまま着地しても、もう一度は跳ばない (直接キーもアクションも「押した瞬間」だけ)
        check(r.CountTakeoffs(180) == 0, "jump: holding the key does not jump again after landing");
        SetKey(r.cur[0], kVkSpace, false);
        r.Tick();
        SetKey(r.cur[0], kVkSpace, true);
        r.Tick();
        check(r.Controller()->velocity.y > kTakeoffSpeed, "jump: a fresh press jumps again");
        SetKey(r.cur[0], kVkSpace, false);
        for (int i = 0; i < 10; ++i) {
            r.Tick();
        }
        const bool airborne = !r.Controller()->isGrounded;
        const float vyBefore = r.Controller()->velocity.y;
        SetKey(r.cur[0], kVkSpace, true);
        r.Tick();
        check(airborne && r.Controller()->velocity.y < vyBefore && r.Controller()->jumpSpeed == 0.0f,
              "jump: pressing in the air does nothing and leaves no pending jump");
        SetKey(r.cur[0], kVkSpace, false);
        check(r.CountTakeoffs(180) == 0, "jump: the air press does not fire after landing");
        r.Input()->jumpSpeed = 0.0f;
        SetKey(r.cur[0], kVkSpace, true);
        r.Tick();
        check(r.Controller()->velocity.y < kTakeoffSpeed && r.Controller()->jumpSpeed == 0.0f, "jump: jumpSpeed 0 disables jumping");
    }

    // ---- (7) 先行入力: 着地の少し前に押したジャンプが、着地した tick の次で 1 回だけ出る ----
    {
        Rig r;
        r.Settle();
        r.Input()->keyJump = charinputkey::kNone;
        std::snprintf(r.Input()->jumpAction, sizeof(r.Input()->jumpAction), "%s", "JumpBuffered");
        SetKey(r.cur[0], kVkJ, true);
        r.Tick();
        SetKey(r.cur[0], kVkJ, false);
        check(r.Controller()->velocity.y > kTakeoffSpeed, "buffered jump: the action alone jumps");
        for (int i = 0; i < 44; ++i) { // 滞空は約 61 tick。着地の 15 tick ほど前まで進める
            r.Tick();
        }
        const bool airborne = !r.Controller()->isGrounded;
        SetKey(r.cur[0], kVkJ, true);
        r.Tick();
        SetKey(r.cur[0], kVkJ, false);
        check(airborne && r.CountTakeoffs(200) == 1, "buffered jump: a press shortly before landing fires exactly once");
    }

    // ---- (8) 入力レーン ----
    {
        Rig r;
        r.playerCount = 2;
        r.Settle();
        const float speed = r.Input()->moveSpeed;
        r.Input()->playerIndex = 1;
        SetKey(r.cur[0], kVkD, true);
        r.Tick();
        check(r.Controller()->moveInput.x == 0.0f, "lanes: lane 0 input does not move a lane 1 character");
        SetKey(r.cur[0], kVkD, false);
        SetKey(r.cur[1], kVkD, true);
        r.Tick();
        check(Near(r.Controller()->moveInput.x, speed, 1.0e-4f), "lanes: lane 1 input moves a lane 1 character");
        r.Input()->playerIndex = 2; // playerCount 以上
        r.Tick();
        const bool beyondCount = r.Controller()->moveInput.x == 0.0f;
        r.Input()->playerIndex = -1;
        SetKey(r.cur[0], kVkD, true);
        r.Tick();
        check(beyondCount && r.Controller()->moveInput.x == 0.0f, "lanes: an out-of-range lane gives no input (not lane 0)");
    }

    // ---- (9) 何もしない場合 ----
    {
        Scene scene;
        InputActions actions;
        actions.LoadFromJsonText(kActionsJson);
        InputSnapshot cur[kMaxPlayers] = {};
        InputSnapshot prev[kMaxPlayers] = {};
        SetKey(cur[0], kVkD, true);
        actions.Evaluate(cur, prev, 1);
        check(UpdateCharacterInput(scene.GetWorld(), actions, cur, scene.InputBuffer(), 1, kDt) == 0,
              "no-op: a world without CharacterInput");
        GameObject lone = scene.CreateGameObjectTracked("NoController");
        lone.SetLocalPosition(1.0f, 2.0f, 3.0f);
        lone.AddComponent<CharacterInputComponent>();
        scene.GetWorld().ApplyStructuralChanges();
        const uint32_t driven = UpdateCharacterInput(scene.GetWorld(), actions, cur, scene.InputBuffer(), 1, kDt);
        const LocalTransform* lt = scene.GetWorld().GetComponent<LocalTransform>(lone.Id());
        check(driven == 0 && lt != nullptr && lt->position.x == 1.0f && lt->position.y == 2.0f && lt->position.z == 3.0f,
              "no-op: CharacterInput without a CharacterController");
    }
    {
        Rig r;
        r.Settle();
        SetKey(r.cur[0], kVkD, true);
        check(UpdateCharacterInput(r.W(), r.actions, nullptr, r.scene.InputBuffer(), 1, kDt) == 1
                  && Near(r.Controller()->moveInput.x, 0.0f, 1.0e-6f),
              "no-op: null lane inputs give no direct-key input (the axis has not been evaluated yet)");
    }

    // ---- (10) 止めた tick に押したジャンプは、再開した tick で暴発しない ----
    {
        Rig r;
        r.Settle();
        SetKey(r.cur[0], kVkSpace, true);
        for (int i = 0; i < 3; ++i) {
            r.Tick(false);
        }
        SetKey(r.cur[0], kVkSpace, false);
        r.Tick(false);
        check(r.Controller()->jumpSpeed == 0.0f && r.CountTakeoffs(60) == 0,
              "pause: a jump pressed and released while paused does not fire on resume");
    }

    // ---- (11) 決定論: 同じ入力列を 2 回流してワールドハッシュが毎 tick 一致する ----
    {
        Rig a;
        Rig b;
        a.Input()->mode = charinputmode::kCameraRelative3D;
        b.Input()->mode = charinputmode::kCameraRelative3D;
        for (Rig* r : { &a, &b }) {
            GameObject camera = r->scene.CreateGameObjectTracked("Camera");
            camera.SetLocalPosition(0.0f, 5.0f, -5.0f);
            camera.AddComponent<CameraComponent>();
            r->Settle();
            r->W().GetComponent<LocalTransform>(camera.Id())->rotation = YawQuat(37.0f);
        }
        bool identical = true;
        for (int tick = 0; tick < 240; ++tick) {
            for (Rig* r : { &a, &b }) {
                SetKey(r->cur[0], kVkD, tick < 120);
                SetKey(r->cur[0], kVkW, tick >= 60 && tick < 200);
                SetKey(r->cur[0], kVkA, tick >= 180);
                SetKey(r->cur[0], kVkSpace, tick == 30 || tick == 150);
                r->Tick();
            }
            if (HashWorld(a.W()) != HashWorld(b.W())) {
                identical = false;
            }
        }
        const LocalTransform* moved = a.Transform();
        check(identical && (std::fabs(moved->position.x) > 1.0f || std::fabs(moved->position.z) > 1.0f),
              "determinism: two runs of the same input hash identically for 240 ticks");
    }

    if (failCount == 0) {
        MYE_LOG_INFO("==== CharacterInput self test: ALL PASS ====");
    } else {
        MYE_LOG_ERROR("==== CharacterInput self test: %d FAILED ====", failCount);
    }
    return failCount == 0;
}

} // namespace mye
