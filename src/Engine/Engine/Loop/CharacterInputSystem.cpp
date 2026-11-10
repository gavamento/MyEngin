#include "Engine/Engine/Loop/CharacterInputSystem.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Core/Util/Hash.h"
#include "Engine/Platform/Input.h"
#include "Engine/Platform/InputActions.h"

using namespace DirectX;

namespace mye {

namespace {

constexpr int kMaxHierarchyDepth = 64;       // 親子の連鎖をこの深さで打ち切る (壊れた循環で止まらないため)
constexpr float kPi = 3.14159265358979f;
constexpr float kMinBasisLength2 = 1.0e-8f;  // カメラの前方の水平成分がこれ未満なら「真上 / 真下を向いている」とみなす
constexpr float kMinTurnSpeed = 0.05f;       // [m/s] これ以下の移動では向きを変えない (止まり際に向きが暴れない)
constexpr size_t kNameCapacity = 64;         // CharacterInputComponent の名前欄 (String64) の大きさ

// LocalTransform の親子連鎖から world 行列を組む (行ベクトル規約: world = local * parentWorld)。
// SpringFollowSystem の同名関数と同じ合成。WorldMatrix は描画時にも更新される派生値なので sim からは読まない
XMMATRIX WorldFromLocals(World& world, EntityID e)
{
    XMMATRIX m = XMMatrixIdentity();
    EntityID cur = e;
    for (int depth = 0; depth < kMaxHierarchyDepth && !cur.IsNull(); ++depth) {
        const auto* lt = world.GetComponent<LocalTransform>(cur);
        if (lt == nullptr) {
            break;
        }
        const XMMATRIX local = XMMatrixAffineTransformation(XMLoadFloat3(&lt->scale), XMVectorZero(),
                                                            XMLoadFloat4(&lt->rotation), XMLoadFloat3(&lt->position));
        m = m * local;
        const auto* h = world.GetComponent<HierarchyComponent>(cur);
        cur = (h != nullptr) ? h->parent : kNullEntity;
    }
    return m;
}

// 水平面 (xz) の単位ベクトル。前進がこの向きになる
struct GroundBasis {
    float forwardX = 0.0f;
    float forwardZ = 1.0f;
};

// エンティティの向きから水平の前方を取る。真上 / 真下を向いていて前方が潰れるときは上方向
// (画面の上) を使う。どちらも取れなければ false
bool GroundBasisOf(World& world, EntityID e, GroundBasis& out)
{
    const XMMATRIX m = WorldFromLocals(world, e);
    const XMVECTOR axes[2] = { XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f) };
    for (const XMVECTOR& axis : axes) {
        XMFLOAT3 d = {};
        XMStoreFloat3(&d, XMVector3TransformNormal(axis, m));
        const float length2 = d.x * d.x + d.z * d.z;
        if (std::isfinite(length2) && length2 > kMinBasisLength2) {
            const float inv = 1.0f / std::sqrt(length2);
            out.forwardX = d.x * inv;
            out.forwardZ = d.z * inv;
            return true;
        }
    }
    return false;
}

// シーンの主カメラ。選び方は RenderSystem / BuildSimWorldContext (UILayout.cpp) と同じ規則
// (走査順で最初のカメラ、isPrimary が出たら確定) = 画面に出ているカメラと同じものを選ぶ
EntityID FindPrimaryCamera(World& world)
{
    bool found = false;
    EntityID camera = kNullEntity;
    const ComponentTypeId req[] = { CameraComponent::sTypeId, LocalTransform::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int ci = arch.FindTypeIndex(CameraComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const auto* c = static_cast<const CameraComponent*>(arch.GetPtr(ci, row));
            if (!found || c->isPrimary) {
                camera = arch.EntityAt(row);
                found = true;
                if (c->isPrimary) {
                    return;
                }
            }
        }
    });
    return camera;
}

// 名前欄 (終端が無くても 64 バイトで打ち切る) のハッシュ。空なら 0 = 「使わない」
uint64_t NameHashOf(const char* name)
{
    const size_t length = strnlen(name, kNameCapacity);
    return (length == 0) ? 0 : HashStr(std::string_view(name, length));
}

// 直接キーが押されているか。割り当てなし (範囲外の番号) は常に偽
bool KeyHeld(const InputSnapshot* lane, int32_t vk)
{
    return lane != nullptr && vk > 0 && vk <= 0xFF && lane->KeyDown(static_cast<uint8_t>(vk));
}

float Clamp11(float v)
{
    return (std::max)(-1.0f, (std::min)(1.0f, v));
}

float YawOf(const XMFLOAT4& q)
{
    const float fx = 2.0f * (q.x * q.z + q.w * q.y);
    const float fz = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
    if (fx == 0.0f && fz == 0.0f) {
        return 0.0f;
    }
    return std::atan2(fx, fz);
}

float WrapPi(float a)
{
    while (a > kPi) {
        a -= 2.0f * kPi;
    }
    while (a < -kPi) {
        a += 2.0f * kPi;
    }
    return a;
}

// 進む方 (vx, vz) へ yaw だけ寄せる。NavSystem の TurnToward と同じ式
void TurnToward(LocalTransform& transform, float turnSpeedDeg, float vx, float vz, float dt)
{
    const float current = YawOf(transform.rotation);
    const float target = std::atan2(vx, vz);
    const float maxStep = turnSpeedDeg * (kPi / 180.0f) * dt;
    const float delta = (std::max)(-maxStep, (std::min)(maxStep, WrapPi(target - current)));
    const float yaw = current + delta;
    transform.rotation = { 0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f) };
}

} // namespace

uint32_t UpdateCharacterInput(World& world, const InputActions& actions, const InputSnapshot* inputs,
                              InputBufferState& inputBuffer, uint32_t playerCount, float dt)
{
    std::vector<EntityID> entities;
    const ComponentTypeId req[] = { CharacterInputComponent::sTypeId, CharacterControllerComponent::sTypeId,
                                    LocalTransform::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            entities.push_back(arch.EntityAt(row));
        }
    });
    if (entities.empty()) {
        return 0;
    }
    std::sort(entities.begin(), entities.end(), [](EntityID a, EntityID b) { return a.index < b.index; });

    // 主カメラは必要になったときに 1 回だけ探す (kCameraRelative3D で cameraEntity が無効なものが居るときだけ)
    bool primaryCameraSearched = false;
    EntityID primaryCamera = kNullEntity;

    uint32_t driven = 0;
    for (EntityID entity : entities) {
        if (!IsEntityActive(world, entity)) {
            continue;
        }
        auto* ci = world.GetComponent<CharacterInputComponent>(entity);
        auto* cc = world.GetComponent<CharacterControllerComponent>(entity);
        auto* lt = world.GetComponent<LocalTransform>(entity);
        if (ci == nullptr || cc == nullptr || lt == nullptr) {
            continue;
        }

        // 範囲外のレーンは無入力にする (0 へ丸めない。理由は PlayerInputSystem.cpp と同じ)
        const bool laneValid = ci->playerIndex >= 0 && static_cast<uint32_t>(ci->playerIndex) < playerCount;
        const uint32_t lane = laneValid ? static_cast<uint32_t>(ci->playerIndex) : 0;
        const InputSnapshot* laneInput = (laneValid && inputs != nullptr) ? &inputs[lane] : nullptr;
        const uint64_t moveXHash = laneValid ? NameHashOf(ci->moveXAxis) : 0;
        const uint64_t moveYHash = laneValid ? NameHashOf(ci->moveYAxis) : 0;
        const uint64_t jumpHash = laneValid ? NameHashOf(ci->jumpAction) : 0;

        // ---- 移動: 直接キー (±1) と actions.json の軸を足して [-1, 1] に収める ----
        float ax = (KeyHeld(laneInput, ci->keyRight) ? 1.0f : 0.0f) - (KeyHeld(laneInput, ci->keyLeft) ? 1.0f : 0.0f);
        float ay = (KeyHeld(laneInput, ci->keyForward) ? 1.0f : 0.0f) - (KeyHeld(laneInput, ci->keyBack) ? 1.0f : 0.0f);
        if (moveXHash != 0) {
            ax += actions.AxisValue(moveXHash, lane);
        }
        if (moveYHash != 0) {
            ay += actions.AxisValue(moveYHash, lane);
        }
        ax = Clamp11(ax);
        ay = Clamp11(ay);
        const float speed = (std::isfinite(ci->moveSpeed) && ci->moveSpeed > 0.0f) ? ci->moveSpeed : 0.0f;

        float vx = 0.0f;
        float vz = 0.0f;
        if (ci->mode == charinputmode::kSideScroll2D) {
            vx = ax * speed;
        } else {
            // 斜め入力で速くならないよう、長さが 1 を超えるぶんだけ縮める (小さいスティック入力は拡大しない)
            const float length = std::sqrt(ax * ax + ay * ay);
            if (length > 1.0f) {
                ax /= length;
                ay /= length;
            }
            GroundBasis basis; // 既定はワールド軸 (前進 = +Z)。カメラが取れないときもこのまま
            if (ci->mode == charinputmode::kCameraRelative3D) {
                EntityID camera = ci->cameraEntity;
                if (camera.IsNull() || !world.IsAlive(camera)) {
                    if (!primaryCameraSearched) {
                        primaryCamera = FindPrimaryCamera(world);
                        primaryCameraSearched = true;
                    }
                    camera = primaryCamera;
                }
                if (!camera.IsNull() && !GroundBasisOf(world, camera, basis)) {
                    basis = GroundBasis{};
                }
            }
            vx = (basis.forwardZ * ax + basis.forwardX * ay) * speed;
            vz = (-basis.forwardX * ax + basis.forwardZ * ay) * speed;
        }
        cc->moveInput = { vx, 0.0f, vz };

        // 親付きの向きは親の回転と合成されるので触らない (NavSystem の TurnToward と同じ扱い)
        if (ci->mode == charinputmode::kCameraRelative3D && std::isfinite(ci->turnSpeedDeg) && ci->turnSpeedDeg > 0.0f
            && world.GetParent(entity).IsNull() && (vx * vx + vz * vz) > kMinTurnSpeed * kMinTurnSpeed) {
            TurnToward(*lt, ci->turnSpeedDeg, vx, vz, dt);
        }

        // ---- ジャンプ: 直接キーを押した瞬間、またはアクションの Pressed / 先行受付の窓の中 ----
        const bool jumpKeyDown = KeyHeld(laneInput, ci->keyJump);
        const bool jumpKeyPressed = jumpKeyDown && ci->jumpKeyWasDown == 0;
        ci->jumpKeyWasDown = jumpKeyDown ? 1u : 0u;
        const uint32_t jumpState = (jumpHash != 0) ? actions.ActionStateBuffered(jumpHash, lane, inputBuffer) : 0u;
        const bool wantsJump = jumpKeyPressed || (jumpState & (kActionPressed | kActionBuffered)) != 0;
        // isGrounded は前 tick の物理の結果。空中では書かない (書くと物理が消費するだけだが、
        // 先行受付の窓を無駄に消してしまう)
        if (wantsJump && cc->isGrounded && std::isfinite(ci->jumpSpeed) && ci->jumpSpeed > 0.0f) {
            cc->jumpSpeed = ci->jumpSpeed;
            if (jumpHash != 0) {
                actions.ConsumeBuffered(jumpHash, lane, inputBuffer); // 同じ押下で 2 回跳ばない
            }
        }
        ++driven;
    }
    return driven;
}

} // namespace mye
