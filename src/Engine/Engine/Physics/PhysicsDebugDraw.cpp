#include "Engine/Engine/Physics/PhysicsDebugDraw.h"

#include <cmath>

#include "Engine/Core/Components.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/DebugDraw.h"
#include "Engine/Engine/Physics/PhysicsSystem.h"

namespace mye {
namespace {

// EditorLinePass::Unpack と同じ 0xRRGGBBAA
constexpr uint32_t kContactColor = 0xFF5050FFu; // 接触点の十字 (赤)
constexpr uint32_t kNormalColor = 0xFFC040FFu;  // 接触法線 (橙)
constexpr uint32_t kVelocityColor = 0x40D0FFFFu; // 速度ベクトル (水色)

constexpr float kCrossArm = 0.06f;      // 接触点の十字の腕の長さ [m]
constexpr float kNormalLen = 0.30f;     // 法線の基本長 [m]
// インパルス表示の縮尺。1 N*s あたり何 m 伸ばすか。表示のためだけの値なので
// 「よくある接触 (質量 1kg が 60Hz で受け止められる程度 = 0.16 N*s 前後) で
//  基本長と同じくらいになる」を目安に置いてある
constexpr float kImpulseScale = 2.0f;
constexpr float kImpulseMaxLen = 3.0f; // 伸びすぎて画面を埋めないための頭打ち
constexpr float kVelocityScale = 0.1f; // 1 m/s あたり 0.1 m
constexpr float kVelocityMinSpeed = 0.05f; // これ未満は描かない (静止物のノイズ除去)

void PushLine(std::vector<DebugLineCmd>& out, float ax, float ay, float az, float bx, float by,
              float bz, uint32_t rgba)
{
    DebugLineCmd c;
    c.ax = ax;
    c.ay = ay;
    c.az = az;
    c.bx = bx;
    c.by = by;
    c.bz = bz;
    c.rgba = rgba;
    out.push_back(c);
}

} // namespace

PhysicsDebugFlags& GetPhysicsDebugFlags()
{
    static PhysicsDebugFlags flags;
    return flags;
}

void BuildPhysicsDebugLines(World& world, const std::vector<SolidContact>& contacts,
                            const PhysicsDebugFlags& flags, std::vector<DebugLineCmd>& out)
{
    if (flags.contacts) {
        // contacts は key 昇順 (PhysicsSystem の出力規約) なので、そのまま舐めれば固定順
        for (const SolidContact& c : contacts) {
            // 接触点の十字 (軸平行 3 本。向きを持たせないのは「点である」ことを示すため)
            PushLine(out, c.px - kCrossArm, c.py, c.pz, c.px + kCrossArm, c.py, c.pz,
                     kContactColor);
            PushLine(out, c.px, c.py - kCrossArm, c.pz, c.px, c.py + kCrossArm, c.pz,
                     kContactColor);
            PushLine(out, c.px, c.py, c.pz - kCrossArm, c.px, c.py, c.pz + kCrossArm,
                     kContactColor);
            // 法線 (大 index 側 → 小 index 側)。impulses ON なら強さを長さに乗せる
            float len = kNormalLen;
            if (flags.impulses) {
                len = c.impulse * kImpulseScale;
                if (len < 0.0f) {
                    len = 0.0f;
                }
                if (len > kImpulseMaxLen) {
                    len = kImpulseMaxLen;
                }
            }
            PushLine(out, c.px, c.py, c.pz, c.px + c.nx * len, c.py + c.ny * len,
                     c.pz + c.nz * len, kNormalColor);
        }
    }

    if (flags.velocities) {
        // 根元は当 tick のワールド行列 (TransformSystem の後に呼ばれる前提)
        const ComponentTypeId req[] = { RigidbodyComponent::sTypeId,
                                        WorldMatrixComponent::sTypeId };
        world.ForEachArchetype(req, [&](Archetype& arch) {
            const int ri = arch.FindTypeIndex(RigidbodyComponent::sTypeId);
            const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                if (!IsEntityActive(world, arch.EntityAt(row))) {
                    continue;
                }
                const auto* rb = static_cast<const RigidbodyComponent*>(arch.GetPtr(ri, row));
                const float vx = rb->velocity.x, vy = rb->velocity.y, vz = rb->velocity.z;
                const float sp = std::sqrt(vx * vx + vy * vy + vz * vz);
                if (sp < kVelocityMinSpeed) {
                    continue;
                }
                const auto* wm = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row));
                const float ox = wm->value._41, oy = wm->value._42, oz = wm->value._43;
                PushLine(out, ox, oy, oz, ox + vx * kVelocityScale, oy + vy * kVelocityScale,
                         oz + vz * kVelocityScale, kVelocityColor);
            }
        });
    }
}

} // namespace mye
