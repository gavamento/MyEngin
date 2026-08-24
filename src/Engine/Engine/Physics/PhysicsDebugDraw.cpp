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

// ---- 関節 (M60a) ----
constexpr uint32_t kJointAnchorA = 0x40FF90FFu;   // owner 側アンカー (緑)
constexpr uint32_t kJointAnchorB = 0xFFD040FFu;   // 相手側アンカー (黄)
constexpr uint32_t kJointErrColor = 0xFF3030FFu;  // 2 点のずれ (赤。長いほど拘束が破れている)
constexpr uint32_t kJointAxisColor = 0x60A0FFFFu; // 軸 (青)
constexpr float kJointCrossArm = 0.10f; // アンカーの十字の腕 [m] (接触点より一回り大きく)
constexpr float kJointAxisLen = 0.50f;  // 軸の描画長 [m]

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

// 軸平行の十字 (点であることを示すために向きを持たせない。接触点と同じ流儀)
void PushCross(std::vector<DebugLineCmd>& out, float x, float y, float z, float arm, uint32_t rgba)
{
    PushLine(out, x - arm, y, z, x + arm, y, z, rgba);
    PushLine(out, x, y - arm, z, x, y + arm, z, rgba);
    PushLine(out, x, y, z - arm, x, y, z + arm, rgba);
}

// 行優先 (translation が _41.._43) のワールド行列で点を変換する
void XformPoint(const DirectX::XMFLOAT4X4& m, const DirectX::XMFLOAT3& v, float& ox, float& oy,
                float& oz)
{
    ox = v.x * m._11 + v.y * m._21 + v.z * m._31 + m._41;
    oy = v.x * m._12 + v.y * m._22 + v.z * m._32 + m._42;
    oz = v.x * m._13 + v.y * m._23 + v.z * m._33 + m._43;
}

// 同じ行列で方向を変換する (平行移動を足さない)
void XformDir(const DirectX::XMFLOAT4X4& m, const DirectX::XMFLOAT3& v, float& ox, float& oy,
              float& oz)
{
    ox = v.x * m._11 + v.y * m._21 + v.z * m._31;
    oy = v.x * m._12 + v.y * m._22 + v.z * m._32;
    oz = v.x * m._13 + v.y * m._23 + v.z * m._33;
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

    if (flags.joints) {
        // 走査順はアーキタイプ順 (= World の内部順) だが、**出力は線を積むだけで
        // sim に触れないので順序は結果に効かない**。ここを決定論キーで並べ替えないのは
        // 接触の可視化 (contacts) が SolidContact の key 昇順に乗っているのと同じ理由で、
        // 「読むだけの出力レーン」に決定論の面積を広げたくないから
        const ComponentTypeId req[] = { JointComponent::sTypeId, WorldMatrixComponent::sTypeId };
        world.ForEachArchetype(req, [&](Archetype& arch) {
            const int ji = arch.FindTypeIndex(JointComponent::sTypeId);
            const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                if (!IsEntityActive(world, arch.EntityAt(row))) {
                    continue;
                }
                const auto* jc = static_cast<const JointComponent*>(arch.GetPtr(ji, row));
                const auto* wm = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row));
                float ax, ay, az;
                XformPoint(wm->value, jc->anchor, ax, ay, az);
                float bx, by, bz;
                if (jc->connectedEntity.IsNull()) {
                    // 相手が居ないときだけ connectedAnchor はワールド座標 (ソルバと同規約)
                    bx = jc->connectedAnchor.x;
                    by = jc->connectedAnchor.y;
                    bz = jc->connectedAnchor.z;
                } else {
                    const auto* owm =
                        world.GetComponent<WorldMatrixComponent>(jc->connectedEntity);
                    if (!owm) {
                        continue; // 相手の行列が無い = まだ Transform が回っていない
                    }
                    XformPoint(owm->value, jc->connectedAnchor, bx, by, bz);
                }
                PushCross(out, ax, ay, az, kJointCrossArm, kJointAnchorA);
                PushCross(out, bx, by, bz, kJointCrossArm, kJointAnchorB);
                // ★2 点を結ぶ線が**拘束誤差そのもの**。満たされていれば長さ 0 で消える —
                //   「赤い線が見えたら関節が破れている」が一目で分かるのが可視化の主目的
                PushLine(out, ax, ay, az, bx, by, bz, kJointErrColor);
                // 軸を使う型 (Hinge / Slider / Cone) だけ軸を描く
                if (jc->type == 1 || jc->type == 3 || jc->type == 4) {
                    float dx, dy, dz;
                    XformDir(wm->value, jc->axis, dx, dy, dz);
                    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
                    if (len > 1e-6f) {
                        const float s = kJointAxisLen / len;
                        PushLine(out, ax - dx * s, ay - dy * s, az - dz * s, ax + dx * s,
                                 ay + dy * s, az + dz * s, kJointAxisColor);
                    }
                }
            }
        });
    }
}

} // namespace mye
