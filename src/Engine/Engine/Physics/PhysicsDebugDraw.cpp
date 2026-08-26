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

// ---- 可動域 (M60c の申し送り 9 を M60g2 で消化) ----
// ★ラグドールは「どこまで曲がるか」が見えないとデバッグが成立しない。M60c の時点では
//   曲げる被写体がヒンジ 1 本だったので後回しにしていた。
constexpr uint32_t kJointLimitColor = 0xC080FFFFu; // 可動域の円錐 / 円弧 (紫)
constexpr float kJointLimitLen = 0.40f;            // 円錐の母線 / 円弧の半径 [m]
constexpr float kJointTwistRatio = 0.45f;          // ツイスト円弧の半径 (母線に対する比)
constexpr int kJointConeSpokes = 8;                // 円錐の母線の本数
constexpr int kJointArcSegs = 16;                  // 円弧・リムの分割数
constexpr float kDeg2Rad = 3.14159265358979323846f / 180.0f;

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

// 単位クォータニオンでベクトルを回転 (表示専用。ソルバの QuatRotate と同じ式)
void QuatRotateV(float qx, float qy, float qz, float qw, float vx, float vy, float vz, float& ox,
                 float& oy, float& oz)
{
    const float tx = 2.0f * (qy * vz - qz * vy);
    const float ty = 2.0f * (qz * vx - qx * vz);
    const float tz = 2.0f * (qx * vy - qy * vx);
    ox = vx + qw * tx + (qy * tz - qz * ty);
    oy = vy + qw * ty + (qz * tx - qx * tz);
    oz = vz + qw * tz + (qx * ty - qy * tx);
}

// 正規化。長さが取れなければ false (縮退した軸で円錐を描かない)
bool Normalize3(float& x, float& y, float& z)
{
    const float l2 = x * x + y * y + z * z;
    if (l2 < 1e-12f) {
        return false;
    }
    const float inv = 1.0f / std::sqrt(l2);
    x *= inv;
    y *= inv;
    z *= inv;
    return true;
}

// a に垂直な単位ベクトルを 1 本作る。**最小成分の座標軸との外積**を採るので、
// a がどの向きでも縮退しない (円錐の「0 度」の基準を決めるためだけの向き)
void PerpOf(const float a[3], float out[3])
{
    const float ax = std::fabs(a[0]), ay = std::fabs(a[1]), az = std::fabs(a[2]);
    float rx = 0.0f, ry = 0.0f, rz = 0.0f;
    if (ax <= ay && ax <= az) {
        rx = 1.0f;
    } else if (ay <= az) {
        ry = 1.0f;
    } else {
        rz = 1.0f;
    }
    out[0] = a[1] * rz - a[2] * ry;
    out[1] = a[2] * rx - a[0] * rz;
    out[2] = a[0] * ry - a[1] * rx;
    Normalize3(out[0], out[1], out[2]);
}

// 原点 o・基準 u・従 v の平面で、角度 [aDeg, bDeg] の円弧を半径 r で描く。
// 端の 2 本は原点からの「腕」を出す (どこが下限でどこが上限かを見せるため)
void PushArc(std::vector<DebugLineCmd>& out, const float o[3], const float u[3], const float v[3],
             float r, float aDeg, float bDeg, uint32_t rgba)
{
    float px = 0.0f, py = 0.0f, pz = 0.0f;
    for (int i = 0; i <= kJointArcSegs; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kJointArcSegs);
        const float ang = (aDeg + (bDeg - aDeg) * t) * kDeg2Rad;
        const float c = std::cos(ang), s = std::sin(ang);
        const float x = o[0] + r * (u[0] * c + v[0] * s);
        const float y = o[1] + r * (u[1] * c + v[1] * s);
        const float z = o[2] + r * (u[2] * c + v[2] * s);
        if (i > 0) {
            PushLine(out, px, py, pz, x, y, z, rgba);
        } else {
            PushLine(out, o[0], o[1], o[2], x, y, z, rgba); // 下限の腕
        }
        if (i == kJointArcSegs) {
            PushLine(out, o[0], o[1], o[2], x, y, z, rgba); // 上限の腕
        }
        px = x;
        py = y;
        pz = z;
    }
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
                const WorldMatrixComponent* owm = nullptr;
                if (jc->connectedEntity.IsNull()) {
                    // 相手が居ないときだけ connectedAnchor はワールド座標 (ソルバと同規約)
                    bx = jc->connectedAnchor.x;
                    by = jc->connectedAnchor.y;
                    bz = jc->connectedAnchor.z;
                } else {
                    owm = world.GetComponent<WorldMatrixComponent>(jc->connectedEntity);
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
                    // ---- 可動域 (M60g2) ----
                    // ★`useLimit` が false なら行が 1 本も立たない (= 可動域は無い) ので
                    //   描かない。ソルバの分岐条件とここの分岐条件を揃えておかないと
                    //   「絵には枠があるのに素通りする」という一番たちの悪い嘘になる
                    if (jc->useLimit && Normalize3(dx, dy, dz)) {
                        const float o[3] = { ax, ay, az };
                        if (jc->type == 3) {
                            // スライダ: 許される変位の区間そのものを線分で出す
                            const float lox = o[0] + dx * jc->limitMin;
                            const float loy = o[1] + dy * jc->limitMin;
                            const float loz = o[2] + dz * jc->limitMin;
                            const float hix = o[0] + dx * jc->limitMax;
                            const float hiy = o[1] + dy * jc->limitMax;
                            const float hiz = o[2] + dz * jc->limitMax;
                            PushLine(out, lox, loy, loz, hix, hiy, hiz, kJointLimitColor);
                            PushCross(out, lox, loy, loz, kJointCrossArm * 0.5f, kJointLimitColor);
                            PushCross(out, hix, hiy, hiz, kJointCrossArm * 0.5f, kJointLimitColor);
                        } else if (jc->type == 1) {
                            // ヒンジ: 軸に垂直な面内の円弧。0 度の基準は表示専用の
                            // 「軸に垂直な適当な 1 本」なので、**絵の向きではなく開き角を見る**
                            const float axis[3] = { dx, dy, dz };
                            float u[3], v[3];
                            PerpOf(axis, u);
                            v[0] = axis[1] * u[2] - axis[2] * u[1];
                            v[1] = axis[2] * u[0] - axis[0] * u[2];
                            v[2] = axis[0] * u[1] - axis[1] * u[0];
                            PushArc(out, o, u, v, kJointLimitLen, jc->limitMin, jc->limitMax,
                                    kJointLimitColor);
                        } else {
                            // コーン: 円錐は**相手が担いでいる rest 軸**まわりに開く。
                            // owner 側の軸で描くと「傾いた結果」を枠として描くことになり、
                            // 逸脱しているかどうかが絵から読めなくなる (ソルバの
                            // JointConeAxisB と同じ量を出している)
                            float lx, ly, lz;
                            QuatRotateV(-jc->restRotation.x, -jc->restRotation.y,
                                        -jc->restRotation.z, jc->restRotation.w, jc->axis.x,
                                        jc->axis.y, jc->axis.z, lx, ly, lz);
                            float cx = lx, cy = ly, cz = lz;
                            if (owm) {
                                XformDir(owm->value, DirectX::XMFLOAT3{ lx, ly, lz }, cx, cy, cz);
                            }
                            if (Normalize3(cx, cy, cz)) {
                                const float axis[3] = { cx, cy, cz };
                                float u[3], v[3];
                                PerpOf(axis, u);
                                v[0] = axis[1] * u[2] - axis[2] * u[1];
                                v[1] = axis[2] * u[0] - axis[0] * u[2];
                                v[2] = axis[0] * u[1] - axis[1] * u[0];
                                // 円錐 (母線 + リム)
                                const float half = jc->swingLimitDeg * kDeg2Rad;
                                const float ch = std::cos(half) * kJointLimitLen;
                                const float sh = std::sin(half) * kJointLimitLen;
                                float px = 0.0f, py = 0.0f, pz = 0.0f;
                                for (int i = 0; i <= kJointArcSegs; ++i) {
                                    const float t = 6.28318530717958647692f
                                        * static_cast<float>(i) / static_cast<float>(kJointArcSegs);
                                    const float c = std::cos(t), sn = std::sin(t);
                                    const float x =
                                        o[0] + axis[0] * ch + sh * (u[0] * c + v[0] * sn);
                                    const float y =
                                        o[1] + axis[1] * ch + sh * (u[1] * c + v[1] * sn);
                                    const float z =
                                        o[2] + axis[2] * ch + sh * (u[2] * c + v[2] * sn);
                                    if (i > 0) {
                                        PushLine(out, px, py, pz, x, y, z, kJointLimitColor);
                                    }
                                    if (i % (kJointArcSegs / kJointConeSpokes) == 0) {
                                        PushLine(out, o[0], o[1], o[2], x, y, z, kJointLimitColor);
                                    }
                                    px = x;
                                    py = y;
                                    pz = z;
                                }
                                // ツイスト範囲は軸まわりの円弧 (円錐より一回り内側に)
                                PushArc(out, o, u, v, kJointLimitLen * kJointTwistRatio,
                                        jc->limitMin, jc->limitMax, kJointLimitColor);
                            }
                        }
                    }
                }
            }
        });
    }
}

} // namespace mye
