#include "Engine/Engine/Physics/AeroSampling.h"

#include <cmath>

#include "Engine/Engine/Physics/Shapes.h"

namespace mye {
namespace {

// 方位 8 分割の (cos, sin)。**表で持つのは決定論のため** — std::cos/std::sin は CRT 実装
// 依存でビットが動きうる。8 分割なら値は 0 / ±1 / ±sqrt(2)/2 の 3 種しか出てこない
constexpr float kSqrtHalf = 0.70710678f;
constexpr float kAeroAzimuth[kAeroCapsuleSegments][2] = {
    { 1.0f, 0.0f },        { kSqrtHalf, kSqrtHalf },   { 0.0f, 1.0f },
    { -kSqrtHalf, kSqrtHalf }, { -1.0f, 0.0f },        { -kSqrtHalf, -kSqrtHalf },
    { 0.0f, -1.0f },       { kSqrtHalf, -kSqrtHalf },
};

constexpr float kPi = 3.14159265f;

// pose のローカル方向 (lx,ly,lz) をワールドへ (基底 3 本の線形結合)
void ToWorldDir(const ShapePose& p, float lx, float ly, float lz, float& ox, float& oy, float& oz)
{
    ox = p.bx[0] * lx + p.by[0] * ly + p.bz[0] * lz;
    oy = p.bx[1] * lx + p.by[1] * ly + p.bz[1] * lz;
    oz = p.bx[2] * lx + p.by[2] * ly + p.bz[2] * lz;
}

// 中心からの相対ベクトル r と剛体の v/omega から、その点の速度を作って要素を仕上げる
void FinishElement(const ShapePose& p, float rx, float ry, float rz, float nx, float ny, float nz,
                   float area, float vx, float vy, float vz, float wx, float wy, float wz,
                   SurfaceElement& e)
{
    e.px = p.px + rx;
    e.py = p.py + ry;
    e.pz = p.pz + rz;
    e.nx = nx;
    e.ny = ny;
    e.nz = nz;
    e.area = area;
    // v + omega x r
    e.vx = vx + (wy * rz - wz * ry);
    e.vy = vy + (wz * rx - wx * rz);
    e.vz = vz + (wx * ry - wy * rx);
}

} // namespace

void AccumulateSurfaceElement(const SurfaceElement& e, const AeroCoeffs& c, float refX, float refY,
                              float refZ, AeroAccum& acc)
{
    if (e.area <= 0.0f) {
        return;
    }
    // u = 要素が空気に対して進む速度
    const float ux = e.vx - c.windX;
    const float uy = e.vy - c.windY;
    const float uz = e.vz - c.windZ;
    const float un = e.nx * ux + e.ny * uy + e.nz * uz;
    float fx = 0.0f, fy = 0.0f, fz = 0.0f;
    if (un > 0.0f) {
        // 風上面だけが圧力を受ける (風下面は剥離して寄与しないとみなす Newton 流の平板モデル)。
        // F = -Cn rho A un^2 n。sin^2(alpha) が (n.u)^2 として出るので三角関数を呼ばない。
        // 力が **-n 方向 (面に垂直)** なので、傾いた面では流れに垂直な成分 = 揚力が
        // 自動的に含まれる。全要素の和のモーメントが風見安定になる
        const float p = -c.normalCoeff * c.density * e.area * un * un;
        fx = e.nx * p;
        fy = e.ny * p;
        fz = e.nz * p;
    }
    if (c.tangentCoeff > 0.0f) {
        // 表面摩擦は風上/風下を問わず接線方向に効く
        const float tx = ux - un * e.nx;
        const float ty = uy - un * e.ny;
        const float tz = uz - un * e.nz;
        const float t2 = tx * tx + ty * ty + tz * tz;
        if (t2 > 0.0f) {
            const float q = -c.tangentCoeff * c.density * e.area * std::sqrt(t2);
            fx += tx * q;
            fy += ty * q;
            fz += tz * q;
        }
    }
    acc.fx += fx;
    acc.fy += fy;
    acc.fz += fz;
    const float rx = e.px - refX;
    const float ry = e.py - refY;
    const float rz = e.pz - refZ;
    acc.tx += ry * fz - rz * fy;
    acc.ty += rz * fx - rx * fz;
    acc.tz += rx * fy - ry * fx;
}

void AccumulateShapeAero(const ShapePose& pose, float vx, float vy, float vz, float wx, float wy,
                         float wz, const AeroCoeffs& c, AeroAccum& acc)
{
    SurfaceElement e;
    switch (pose.shape) {
    case 0: { // 球: 面積分の閉形式。等方なのでトルクは出ない (回転由来のマグヌスは
              // 等方空力 (M59b) 側の担当 — こちらは向きを見る抗力だけを受け持つ)
        const float R = pose.radius;
        if (R <= 0.0f) {
            return;
        }
        const float ux = vx - c.windX, uy = vy - c.windY, uz = vz - c.windZ;
        const float sp = std::sqrt(ux * ux + uy * uy + uz * uz);
        if (sp <= 0.0f) {
            return;
        }
        // 風上半球の圧力を積むと F = (pi/2) R^2 Cn rho u^2 = 1/2 rho Cn (pi R^2) u^2。
        // **平板と同じ Cn でも実効 Cd は半分**になる (AeroSampling.h の係数コメント)。
        // 細かく分割した球を同じカーネルへ流すとこの値へ収束することを selftest が固定する
        const float k = -0.5f * c.density * c.normalCoeff * kPi * R * R * sp;
        acc.fx += ux * k;
        acc.fy += uy * k;
        acc.fz += uz * k;
        return;
    }
    case 1: { // box: 基底順の 6 面 (+X, -X, +Y, -Y, +Z, -Z)
        const float halfs[3] = { pose.hx, pose.hy, pose.hz };
        // 面 k の面積は残り 2 辺の積 (全辺 = 2h)
        const float areas[3] = { 4.0f * pose.hy * pose.hz, 4.0f * pose.hx * pose.hz,
                                 4.0f * pose.hx * pose.hy };
        for (int axis = 0; axis < 3; ++axis) {
            for (int sign = 0; sign < 2; ++sign) {
                const float s = (sign == 0) ? 1.0f : -1.0f;
                float lx = 0.0f, ly = 0.0f, lz = 0.0f;
                if (axis == 0) { lx = s; } else if (axis == 1) { ly = s; } else { lz = s; }
                float nx, ny, nz;
                ToWorldDir(pose, lx, ly, lz, nx, ny, nz);
                const float h = halfs[axis];
                FinishElement(pose, nx * h, ny * h, nz * h, nx, ny, nz, areas[axis], vx, vy, vz,
                              wx, wy, wz, e);
                AccumulateSurfaceElement(e, c, pose.px, pose.py, pose.pz, acc);
            }
        }
        return;
    }
    case 2: { // capsule (ローカル Y 軸): 方位 8 分割の側面 → +Y 端 → -Y 端
        const float R = pose.radius;
        if (R <= 0.0f) {
            return;
        }
        const float seg = pose.halfSeg;
        // 側面: 高さ 2*seg の円筒を方位 8 分割。1 枚の面積 = (2 pi R / N) * (2 seg)
        if (seg > 0.0f) {
            const float stripArea = (2.0f * kPi * R / static_cast<float>(kAeroCapsuleSegments))
                                  * (2.0f * seg);
            for (int i = 0; i < kAeroCapsuleSegments; ++i) {
                const float cx = kAeroAzimuth[i][0];
                const float cz = kAeroAzimuth[i][1];
                float nx, ny, nz;
                ToWorldDir(pose, cx, 0.0f, cz, nx, ny, nz); // 半径方向 (ローカル XZ 面内)
                FinishElement(pose, nx * R, ny * R, nz * R, nx, ny, nz, stripArea, vx, vy, vz, wx,
                              wy, wz, e);
                AccumulateSurfaceElement(e, c, pose.px, pose.py, pose.pz, acc);
            }
        }
        // 端の半球は「面積 pi R^2 / 2 の円盤」に置き換える — 軸方向の流れに対する半球の
        // 圧力積分がちょうどこの円盤と一致するため (傾いた流れでの横力は落ちる近似)
        const float capArea = 0.5f * kPi * R * R;
        for (int sign = 0; sign < 2; ++sign) {
            const float s = (sign == 0) ? 1.0f : -1.0f;
            float nx, ny, nz;
            ToWorldDir(pose, 0.0f, s, 0.0f, nx, ny, nz);
            const float d = seg + R; // 端の中心までの距離
            FinishElement(pose, nx * d, ny * d, nz * d, nx, ny, nz, capArea, vx, vy, vz, wx, wy,
                          wz, e);
            AccumulateSurfaceElement(e, c, pose.px, pose.py, pose.pz, acc);
        }
        return;
    }
    default:
        return; // mesh (shape=3) は面サンプリングの対象外
    }
}

} // namespace mye
