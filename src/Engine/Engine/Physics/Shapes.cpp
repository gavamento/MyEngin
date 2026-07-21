#include "Engine/Engine/Physics/Shapes.h"

#include <algorithm>
#include <cmath>

#include "Engine/Core/Components.h"

namespace mye {
namespace shapes {
namespace {

// 決定論のため全て scalar float 演算 (XMVECTOR SIMD を使わない = Debug/Release で同一ビット)。
// M20 由来のコード (sphere-sphere / 無回転 box) は一字一句そのまま維持している —
// 無回転シーンの挙動をビット単位で変えないため、書き換え・共通化をしないこと。

constexpr float kDegenerateEps = 1e-12f;

float Dot3(const float* v, float x, float y, float z)
{
    return v[0] * x + v[1] * y + v[2] * z;
}

// ワールドベクトル (x,y,z) を pose のローカル基底成分 (lx,ly,lz) に分解
void WorldToLocal(const ShapePose& p, float x, float y, float z, float& lx, float& ly, float& lz)
{
    lx = Dot3(p.bx, x, y, z);
    ly = Dot3(p.by, x, y, z);
    lz = Dot3(p.bz, x, y, z);
}

// ローカル成分 (lx,ly,lz) をワールドベクトルへ
void LocalToWorld(const ShapePose& p, float lx, float ly, float lz, float& x, float& y, float& z)
{
    x = lx * p.bx[0] + ly * p.by[0] + lz * p.bz[0];
    y = lx * p.bx[1] + ly * p.by[1] + lz * p.bz[1];
    z = lx * p.bx[2] + ly * p.by[2] + lz * p.bz[2];
}

// capsule の線分端点 (ワールド)。a = 中心 − by·halfSeg、b = 中心 + by·halfSeg
void CapsuleSegment(const ShapePose& c, float& ax, float& ay, float& az, float& bx, float& by,
                    float& bz)
{
    ax = c.px - c.by[0] * c.halfSeg;
    ay = c.py - c.by[1] * c.halfSeg;
    az = c.pz - c.by[2] * c.halfSeg;
    bx = c.px + c.by[0] * c.halfSeg;
    by = c.py + c.by[1] * c.halfSeg;
    bz = c.pz + c.by[2] * c.halfSeg;
}

// 線分 P1(s)=p1+s·d1, P2(t)=p2+t·d2 (s,t∈[0,1]) の最近点パラメータ (Ericson 5.1.9)
void ClosestSegSeg(float p1x, float p1y, float p1z, float q1x, float q1y, float q1z, float p2x,
                   float p2y, float p2z, float q2x, float q2y, float q2z, float& s, float& t)
{
    const float d1x = q1x - p1x, d1y = q1y - p1y, d1z = q1z - p1z;
    const float d2x = q2x - p2x, d2y = q2y - p2y, d2z = q2z - p2z;
    const float rx = p1x - p2x, ry = p1y - p2y, rz = p1z - p2z;
    const float a = d1x * d1x + d1y * d1y + d1z * d1z;
    const float e = d2x * d2x + d2y * d2y + d2z * d2z;
    const float f = d2x * rx + d2y * ry + d2z * rz;
    if (a <= kDegenerateEps && e <= kDegenerateEps) {
        s = 0.0f;
        t = 0.0f;
        return;
    }
    if (a <= kDegenerateEps) {
        s = 0.0f;
        t = std::clamp(f / e, 0.0f, 1.0f);
        return;
    }
    const float c = d1x * rx + d1y * ry + d1z * rz;
    if (e <= kDegenerateEps) {
        t = 0.0f;
        s = std::clamp(-c / a, 0.0f, 1.0f);
        return;
    }
    const float b = d1x * d2x + d1y * d2y + d1z * d2z;
    const float denom = a * e - b * b;
    s = (denom > kDegenerateEps) ? std::clamp((b * f - c * e) / denom, 0.0f, 1.0f) : 0.0f;
    t = (b * s + f) / e;
    if (t < 0.0f) {
        t = 0.0f;
        s = std::clamp(-c / a, 0.0f, 1.0f);
    } else if (t > 1.0f) {
        t = 1.0f;
        s = std::clamp((b - c) / a, 0.0f, 1.0f);
    }
}

// ---- 球ペア (中心 2 点 + 合成半径)。sphere-sphere 本体 + capsule 系の最終段が共用 ----
// M20 の sphere-sphere と同一の式・分岐 (fast-path 保証のため変更しないこと)
bool SpherePair(float ax, float ay, float az, float bx, float by, float bz, float r, float& nx,
                float& ny, float& nz, float& depth)
{
    const float dx = ax - bx, dy = ay - by, dz = az - bz;
    const float d2 = dx * dx + dy * dy + dz * dz;
    if (d2 >= r * r) {
        return false;
    }
    const float d = std::sqrt(d2);
    if (d > 1e-6f) {
        nx = dx / d;
        ny = dy / d;
        nz = dz / d;
    } else {
        nx = 0;
        ny = 1;
        nz = 0; // 中心一致: 任意方向 (上向き)
    }
    depth = r - d;
    return true;
}

// ---- sphere-box。normal は box→sphere 方向 ----
// 球中心を箱ローカル軸平行系 (中心 cx..、半径 hx..) で判定する共通本体。
// M20 の sphere-aabb コードそのまま (fast-path はワールド座標で、回転版はローカル座標で呼ぶ)
bool SphereAabb(float spx, float spy, float spz, float sr, float cx, float cy, float cz, float hx,
                float hy, float hz, float& snx, float& sny, float& snz, float& depth)
{
    const float minx = cx - hx, maxx = cx + hx;
    const float miny = cy - hy, maxy = cy + hy;
    const float minz = cz - hz, maxz = cz + hz;
    const float px = std::clamp(spx, minx, maxx);
    const float py = std::clamp(spy, miny, maxy);
    const float pz = std::clamp(spz, minz, maxz);
    const float dx = spx - px, dy = spy - py, dz = spz - pz;
    const float d2 = dx * dx + dy * dy + dz * dz;
    if (d2 >= sr * sr) {
        return false;
    }
    if (d2 > 1e-12f) {
        const float d = std::sqrt(d2);
        snx = dx / d;
        sny = dy / d;
        snz = dz / d;
        depth = sr - d;
    } else {
        // 球中心が箱内部: 最も近い面へ押し出す
        const float dxp = maxx - spx, dxn = spx - minx;
        const float dyp = maxy - spy, dyn = spy - miny;
        const float dzp = maxz - spz, dzn = spz - minz;
        float best = dxp;
        snx = 1;
        sny = 0;
        snz = 0;
        auto consider = [&](float dist, float ax, float ay, float az) {
            if (dist < best) {
                best = dist;
                snx = ax;
                sny = ay;
                snz = az;
            }
        };
        consider(dxn, -1, 0, 0);
        consider(dyp, 0, 1, 0);
        consider(dyn, 0, -1, 0);
        consider(dzp, 0, 0, 1);
        consider(dzn, 0, 0, -1);
        depth = sr + best;
    }
    return true;
}

// 回転対応 sphere-box: 球中心 (ワールド) を箱ローカルへ変換して SphereAabb → 法線を戻す
bool SphereBox(float spx, float spy, float spz, float sr, const ShapePose& box, float& nx,
               float& ny, float& nz, float& depth)
{
    if (box.identityRot) {
        // fast-path: M20 と同一のワールド座標計算 (ビット同一)
        return SphereAabb(spx, spy, spz, sr, box.px, box.py, box.pz, box.hx, box.hy, box.hz, nx,
                          ny, nz, depth);
    }
    float lx, ly, lz;
    WorldToLocal(box, spx - box.px, spy - box.py, spz - box.pz, lx, ly, lz);
    float lnx, lny, lnz;
    if (!SphereAabb(lx, ly, lz, sr, 0, 0, 0, box.hx, box.hy, box.hz, lnx, lny, lnz, depth)) {
        return false;
    }
    LocalToWorld(box, lnx, lny, lnz, nx, ny, nz);
    return true;
}

// ---- box-box。normal は b→a 方向 ----
// 無回転ペアは M20 の aabb-aabb コードそのまま (fast-path)。回転ありは SAT 15 軸。
bool BoxBox(const ShapePose& a, const ShapePose& b, float& nx, float& ny, float& nz, float& depth)
{
    if (a.identityRot && b.identityRot) {
        const float ox = (a.hx + b.hx) - std::fabs(a.px - b.px);
        const float oy = (a.hy + b.hy) - std::fabs(a.py - b.py);
        const float oz = (a.hz + b.hz) - std::fabs(a.pz - b.pz);
        if (ox <= 0 || oy <= 0 || oz <= 0) {
            return false;
        }
        // 最小オーバーラップ軸を分離軸に選ぶ
        nx = ny = nz = 0;
        if (ox <= oy && ox <= oz) {
            nx = (a.px >= b.px) ? 1.0f : -1.0f;
            depth = ox;
        } else if (oy <= oz) {
            ny = (a.py >= b.py) ? 1.0f : -1.0f;
            depth = oy;
        } else {
            nz = (a.pz >= b.pz) ? 1.0f : -1.0f;
            depth = oz;
        }
        return true;
    }

    // SAT: 面軸 6 (a 基底 3 + b 基底 3) + 辺×辺 9。軸の列挙順は固定 (決定論)。
    // 最小重なり軸を厳密 < で選ぶ (同値は先勝ち = 決定論)。
    const float dx = a.px - b.px, dy = a.py - b.py, dz = a.pz - b.pz;
    const float* aAxes[3] = { a.bx, a.by, a.bz };
    const float* bAxes[3] = { b.bx, b.by, b.bz };
    const float aExt[3] = { a.hx, a.hy, a.hz };
    const float bExt[3] = { b.hx, b.hy, b.hz };
    float bestDepth = 0;
    float bestX = 0, bestY = 1, bestZ = 0;
    bool first = true;
    bool separated = false;

    auto testAxis = [&](float ax, float ay, float az) {
        if (separated) {
            return; // 分離確定後も列挙は続くが計算はスキップ (固定列挙 = 決定論)
        }
        const float len2 = ax * ax + ay * ay + az * az;
        if (len2 < 1e-8f) {
            return; // 縮退軸 (平行辺のクロス積) は決定論的にスキップ
        }
        const float inv = 1.0f / std::sqrt(len2);
        ax *= inv;
        ay *= inv;
        az *= inv;
        float ra = 0, rb = 0;
        for (int i = 0; i < 3; ++i) {
            ra += aExt[i] * std::fabs(Dot3(aAxes[i], ax, ay, az));
            rb += bExt[i] * std::fabs(Dot3(bAxes[i], ax, ay, az));
        }
        const float dist = ax * dx + ay * dy + az * dz;
        const float overlap = (ra + rb) - std::fabs(dist);
        if (overlap <= 0) {
            separated = true;
            return;
        }
        if (first || overlap < bestDepth) {
            first = false;
            bestDepth = overlap;
            // normal を b→a (a を押し出す方向) に揃える
            if (dist >= 0) {
                bestX = ax;
                bestY = ay;
                bestZ = az;
            } else {
                bestX = -ax;
                bestY = -ay;
                bestZ = -az;
            }
        }
    };

    for (int i = 0; i < 3; ++i) {
        testAxis(aAxes[i][0], aAxes[i][1], aAxes[i][2]);
    }
    for (int i = 0; i < 3; ++i) {
        testAxis(bAxes[i][0], bAxes[i][1], bAxes[i][2]);
    }
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            const float* u = aAxes[i];
            const float* v = bAxes[j];
            testAxis(u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2],
                     u[0] * v[1] - u[1] * v[0]);
        }
    }
    if (separated || first) {
        return false;
    }
    nx = bestX;
    ny = bestY;
    nz = bestZ;
    depth = bestDepth;
    return true;
}

// ---- sphere-capsule。normal は capsule→sphere 方向 ----
bool SphereCapsule(float spx, float spy, float spz, float sr, const ShapePose& c, float& nx,
                   float& ny, float& nz, float& depth)
{
    // 球中心に最も近い線分上の点
    const float t = std::clamp(Dot3(c.by, spx - c.px, spy - c.py, spz - c.pz), -c.halfSeg,
                               c.halfSeg);
    const float qx = c.px + c.by[0] * t;
    const float qy = c.py + c.by[1] * t;
    const float qz = c.pz + c.by[2] * t;
    return SpherePair(spx, spy, spz, qx, qy, qz, sr + c.radius, nx, ny, nz, depth);
}

// ---- capsule-capsule。normal は b→a 方向 ----
bool CapsuleCapsule(const ShapePose& a, const ShapePose& b, float& nx, float& ny, float& nz,
                    float& depth)
{
    float a0x, a0y, a0z, a1x, a1y, a1z;
    float b0x, b0y, b0z, b1x, b1y, b1z;
    CapsuleSegment(a, a0x, a0y, a0z, a1x, a1y, a1z);
    CapsuleSegment(b, b0x, b0y, b0z, b1x, b1y, b1z);
    float s, t;
    ClosestSegSeg(a0x, a0y, a0z, a1x, a1y, a1z, b0x, b0y, b0z, b1x, b1y, b1z, s, t);
    const float pax = a0x + (a1x - a0x) * s, pay = a0y + (a1y - a0y) * s,
                paz = a0z + (a1z - a0z) * s;
    const float pbx = b0x + (b1x - b0x) * t, pby = b0y + (b1y - b0y) * t,
                pbz = b0z + (b1z - b0z) * t;
    return SpherePair(pax, pay, paz, pbx, pby, pbz, a.radius + b.radius, nx, ny, nz, depth);
}

// ---- capsule-box。normal は box→capsule 方向 ----
// 箱ローカルで dist²(seg(t), AABB) が t について凸なことを利用し、固定 32 回の黄金分割で
// 最近パラメータ t を求め、その点で sphere-box 判定する (固定回数 = 決定論)。
bool CapsuleBox(const ShapePose& c, const ShapePose& box, float& nx, float& ny, float& nz,
                float& depth)
{
    // 線分端点を箱ローカルへ
    float w0x, w0y, w0z, w1x, w1y, w1z;
    CapsuleSegment(c, w0x, w0y, w0z, w1x, w1y, w1z);
    float a0x, a0y, a0z, a1x, a1y, a1z;
    WorldToLocal(box, w0x - box.px, w0y - box.py, w0z - box.pz, a0x, a0y, a0z);
    WorldToLocal(box, w1x - box.px, w1y - box.py, w1z - box.pz, a1x, a1y, a1z);

    auto distSq = [&](float t) {
        const float x = a0x + (a1x - a0x) * t;
        const float y = a0y + (a1y - a0y) * t;
        const float z = a0z + (a1z - a0z) * t;
        float ex = std::fabs(x) - box.hx;
        float ey = std::fabs(y) - box.hy;
        float ez = std::fabs(z) - box.hz;
        if (ex < 0) { ex = 0; }
        if (ey < 0) { ey = 0; }
        if (ez < 0) { ez = 0; }
        return ex * ex + ey * ey + ez * ez;
    };

    constexpr float kInvPhi = 0.6180339887f;
    float lo = 0.0f, hi = 1.0f;
    float m1 = hi - (hi - lo) * kInvPhi;
    float m2 = lo + (hi - lo) * kInvPhi;
    float f1 = distSq(m1);
    float f2 = distSq(m2);
    for (int i = 0; i < 32; ++i) { // 固定 32 回 (収束後も回し切る = 決定論)
        if (f1 < f2) {
            hi = m2;
            m2 = m1;
            f2 = f1;
            m1 = hi - (hi - lo) * kInvPhi;
            f1 = distSq(m1);
        } else {
            lo = m1;
            m1 = m2;
            f1 = f2;
            m2 = lo + (hi - lo) * kInvPhi;
            f2 = distSq(m2);
        }
    }
    const float t = (lo + hi) * 0.5f;
    const float sx = a0x + (a1x - a0x) * t;
    const float sy = a0y + (a1y - a0y) * t;
    const float sz = a0z + (a1z - a0z) * t;
    float lnx, lny, lnz;
    if (!SphereAabb(sx, sy, sz, c.radius, 0, 0, 0, box.hx, box.hy, box.hz, lnx, lny, lnz, depth)) {
        return false;
    }
    if (box.identityRot) {
        nx = lnx;
        ny = lny;
        nz = lnz;
    } else {
        LocalToWorld(box, lnx, lny, lnz, nx, ny, nz);
    }
    return true;
}

// ---- レイ交差 (M20 の RaySphere / RayAabb をそのまま移設 + capsule / OBB 追加) ----

bool RaySphere(float ox, float oy, float oz, float dx, float dy, float dz, float cx, float cy,
               float cz, float r, float maxDist, float& outT, float& nx, float& ny, float& nz)
{
    const float lx = ox - cx, ly = oy - cy, lz = oz - cz;
    const float b = lx * dx + ly * dy + lz * dz;
    const float c = lx * lx + ly * ly + lz * lz - r * r;
    const float disc = b * b - c;
    if (disc < 0) {
        return false;
    }
    const float sq = std::sqrt(disc);
    float t = -b - sq;
    if (t < 0) {
        t = -b + sq; // 内部始点
    }
    if (t < 0 || t > maxDist) {
        return false;
    }
    outT = t;
    const float hx = ox + dx * t, hy = oy + dy * t, hz = oz + dz * t;
    const float inv = (r > 1e-6f) ? 1.0f / r : 0.0f;
    nx = (hx - cx) * inv;
    ny = (hy - cy) * inv;
    nz = (hz - cz) * inv;
    return true;
}

bool RayAabb(float ox, float oy, float oz, float dx, float dy, float dz, float cx, float cy,
             float cz, float hx, float hy, float hz, float maxDist, float& outT, float& nx,
             float& ny, float& nz)
{
    const float o[3] = { ox, oy, oz };
    const float d[3] = { dx, dy, dz };
    const float lo[3] = { cx - hx, cy - hy, cz - hz };
    const float hi[3] = { cx + hx, cy + hy, cz + hz };
    float tmin = 0.0f, tmax = maxDist;
    int axis = 0;
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(d[i]) < 1e-8f) {
            if (o[i] < lo[i] || o[i] > hi[i]) {
                return false; // レイが軸に平行でスラブ外
            }
            continue;
        }
        const float inv = 1.0f / d[i];
        float t0 = (lo[i] - o[i]) * inv;
        float t1 = (hi[i] - o[i]) * inv;
        if (t0 > t1) {
            std::swap(t0, t1);
        }
        if (t0 > tmin) {
            tmin = t0;
            axis = i;
        }
        if (t1 < tmax) {
            tmax = t1;
        }
        if (tmin > tmax) {
            return false;
        }
    }
    outT = tmin;
    nx = ny = nz = 0;
    const float sign = (d[axis] > 0) ? -1.0f : 1.0f; // 入射面の外向き法線
    if (axis == 0) {
        nx = sign;
    } else if (axis == 1) {
        ny = sign;
    } else {
        nz = sign;
    }
    return true;
}

// 回転 box: レイを箱ローカルへ変換して RayAabb → 法線をワールドへ戻す
bool RayBox(const ShapePose& box, float ox, float oy, float oz, float dx, float dy, float dz,
            float maxDist, float& outT, float& nx, float& ny, float& nz)
{
    if (box.identityRot) {
        // fast-path: M20 と同一のワールド座標計算 (ビット同一)
        return RayAabb(ox, oy, oz, dx, dy, dz, box.px, box.py, box.pz, box.hx, box.hy, box.hz,
                       maxDist, outT, nx, ny, nz);
    }
    float lox, loy, loz, ldx, ldy, ldz;
    WorldToLocal(box, ox - box.px, oy - box.py, oz - box.pz, lox, loy, loz);
    WorldToLocal(box, dx, dy, dz, ldx, ldy, ldz);
    float lnx, lny, lnz;
    if (!RayAabb(lox, loy, loz, ldx, ldy, ldz, 0, 0, 0, box.hx, box.hy, box.hz, maxDist, outT,
                 lnx, lny, lnz)) {
        return false;
    }
    LocalToWorld(box, lnx, lny, lnz, nx, ny, nz);
    return true;
}

// capsule: 無限円筒の解析解 + 軸範囲クランプ + 端球。全て scalar。
bool RayCapsule(const ShapePose& c, float ox, float oy, float oz, float dx, float dy, float dz,
                float maxDist, float& outT, float& nx, float& ny, float& nz)
{
    if (c.halfSeg <= 0.0f) {
        return RaySphere(ox, oy, oz, dx, dy, dz, c.px, c.py, c.pz, c.radius, maxDist, outT, nx,
                         ny, nz);
    }
    const float mx = ox - c.px, my = oy - c.py, mz = oz - c.pz;
    const float ma = Dot3(c.by, mx, my, mz); // 始点の軸成分
    const float da = Dot3(c.by, dx, dy, dz); // 方向の軸成分
    // 軸直交成分
    const float mpx = mx - ma * c.by[0], mpy = my - ma * c.by[1], mpz = mz - ma * c.by[2];
    const float dpx = dx - da * c.by[0], dpy = dy - da * c.by[1], dpz = dz - da * c.by[2];
    const float A = dpx * dpx + dpy * dpy + dpz * dpz;
    const float B = mpx * dpx + mpy * dpy + mpz * dpz;
    const float C = mpx * mpx + mpy * mpy + mpz * mpz - c.radius * c.radius;

    if (A > 1e-12f) {
        const float disc = B * B - A * C;
        if (disc < 0) {
            return false; // 無限円筒に当たらなければ端球にも当たらない
        }
        const float sq = std::sqrt(disc);
        float t = (-B - sq) / A;
        if (t < 0) {
            t = (-B + sq) / A; // 内部始点
        }
        if (t >= 0 && t <= maxDist) {
            const float y = ma + da * t; // ヒット点の軸座標
            if (y >= -c.halfSeg && y <= c.halfSeg) {
                outT = t;
                const float hx = ox + dx * t, hy = oy + dy * t, hz = oz + dz * t;
                const float axx = c.px + c.by[0] * y, axy = c.py + c.by[1] * y,
                            axz = c.pz + c.by[2] * y;
                const float inv = 1.0f / c.radius;
                nx = (hx - axx) * inv;
                ny = (hy - axy) * inv;
                nz = (hz - axz) * inv;
                return true;
            }
            // 円筒ヒットが線分範囲外 → その側の端球を試す (範囲を跨ぐ場合も近い側の
            // 端球平面円板を必ず通過するため、近い側だけで十分)
            const float side = (y > 0) ? c.halfSeg : -c.halfSeg;
            return RaySphere(ox, oy, oz, dx, dy, dz, c.px + c.by[0] * side,
                             c.py + c.by[1] * side, c.pz + c.by[2] * side, c.radius, maxDist,
                             outT, nx, ny, nz);
        }
        return false;
    }
    // レイが軸と平行: 直交距離が半径内なら進行方向側の端球のみ当たり得る
    if (C > 0) {
        return false;
    }
    const float side = (da > 0) ? -c.halfSeg : c.halfSeg; // 進行方向の手前側の端
    if (RaySphere(ox, oy, oz, dx, dy, dz, c.px + c.by[0] * side, c.py + c.by[1] * side,
                  c.pz + c.by[2] * side, c.radius, maxDist, outT, nx, ny, nz)) {
        return true;
    }
    const float far = -side;
    return RaySphere(ox, oy, oz, dx, dy, dz, c.px + c.by[0] * far, c.py + c.by[1] * far,
                     c.pz + c.by[2] * far, c.radius, maxDist, outT, nx, ny, nz);
}

// col のスケール適用済み寸法を p に書く。
// sphere 半径 = max スケール (M20 互換)、box は軸別、capsule 半径 = max(sx,sz)・高さ = sy
void ApplyScaledExtents(ShapePose& p, const ColliderComponent& col, float sx, float sy, float sz)
{
    p.radius = col.radius * std::max(sx, std::max(sy, sz));
    p.hx = col.halfExtents.x * sx;
    p.hy = col.halfExtents.y * sy;
    p.hz = col.halfExtents.z * sz;
    p.halfSeg = 0.0f;
    if (col.shape == 2) {
        const float wr = col.radius * std::max(sx, sz);
        const float wh = col.height * 0.5f * sy;
        p.radius = wr;
        p.halfSeg = (wh > wr) ? (wh - wr) : 0.0f;
    }
}

} // namespace

ShapePose MakePose(const ColliderComponent& col, const DirectX::XMFLOAT3& position,
                   const DirectX::XMFLOAT4& rotation, const DirectX::XMFLOAT3& scale)
{
    ShapePose p;
    p.shape = col.shape;
    p.px = position.x;
    p.py = position.y;
    p.pz = position.z;
    const float qx = rotation.x, qy = rotation.y, qz = rotation.z, qw = rotation.w;
    // 単位クォータニオン (ビット一致) は基底を単位のまま = M20 互換 fast-path
    if (qx == 0.0f && qy == 0.0f && qz == 0.0f && qw == 1.0f) {
        p.identityRot = 1;
    } else {
        const float len2 = qx * qx + qy * qy + qz * qz + qw * qw;
        if (len2 < 1e-12f) {
            p.identityRot = 1; // 縮退クォータニオンは単位扱い
        } else {
            p.identityRot = 0;
            const float s = 2.0f / len2; // 非正規化クォータニオンも吸収 (sqrt 不要)
            p.bx[0] = 1.0f - s * (qy * qy + qz * qz);
            p.bx[1] = s * (qx * qy + qz * qw);
            p.bx[2] = s * (qx * qz - qy * qw);
            p.by[0] = s * (qx * qy - qz * qw);
            p.by[1] = 1.0f - s * (qx * qx + qz * qz);
            p.by[2] = s * (qy * qz + qx * qw);
            p.bz[0] = s * (qx * qz + qy * qw);
            p.bz[1] = s * (qy * qz - qx * qw);
            p.bz[2] = 1.0f - s * (qx * qx + qy * qy);
        }
    }
    ApplyScaledExtents(p, col, std::fabs(scale.x), std::fabs(scale.y), std::fabs(scale.z));
    return p;
}

ShapePose MakePoseFromMatrix(const ColliderComponent& col, const DirectX::XMFLOAT4X4& wm)
{
    ShapePose p;
    p.shape = col.shape;
    p.px = wm._41;
    p.py = wm._42;
    p.pz = wm._43;
    // スケール近似: ワールド行列の各行ベクトル長 (M7/M20 と同じ式)
    const float sx = std::sqrt(wm._11 * wm._11 + wm._12 * wm._12 + wm._13 * wm._13);
    const float sy = std::sqrt(wm._21 * wm._21 + wm._22 * wm._22 + wm._23 * wm._23);
    const float sz = std::sqrt(wm._31 * wm._31 + wm._32 * wm._32 + wm._33 * wm._33);
    if (sx > 1e-8f) {
        p.bx[0] = wm._11 / sx;
        p.bx[1] = wm._12 / sx;
        p.bx[2] = wm._13 / sx;
    }
    if (sy > 1e-8f) {
        p.by[0] = wm._21 / sy;
        p.by[1] = wm._22 / sy;
        p.by[2] = wm._23 / sy;
    }
    if (sz > 1e-8f) {
        p.bz[0] = wm._31 / sz;
        p.bz[1] = wm._32 / sz;
        p.bz[2] = wm._33 / sz;
    }
    // 無回転行列は行の正規化が厳密に (1,0,0) 等になる (x/x=1, 0/x=0) → fast-path 判定
    p.identityRot = (p.bx[0] == 1.0f && p.bx[1] == 0.0f && p.bx[2] == 0.0f && p.by[0] == 0.0f
                     && p.by[1] == 1.0f && p.by[2] == 0.0f && p.bz[0] == 0.0f && p.bz[1] == 0.0f
                     && p.bz[2] == 1.0f)
        ? 1
        : 0;
    ApplyScaledExtents(p, col, sx, sy, sz);
    return p;
}

bool Collide(const ShapePose& a, const ShapePose& b, float& nx, float& ny, float& nz, float& depth)
{
    const int32_t sa = a.shape, sb = b.shape;
    if (sa == 0 && sb == 0) {
        return SpherePair(a.px, a.py, a.pz, b.px, b.py, b.pz, a.radius + b.radius, nx, ny, nz,
                          depth);
    }
    if (sa == 1 && sb == 1) {
        return BoxBox(a, b, nx, ny, nz, depth);
    }
    if (sa == 2 && sb == 2) {
        return CapsuleCapsule(a, b, nx, ny, nz, depth);
    }
    if (sa == 0 && sb == 1) {
        return SphereBox(a.px, a.py, a.pz, a.radius, b, nx, ny, nz, depth); // box→sphere = b→a
    }
    if (sa == 1 && sb == 0) {
        if (!SphereBox(b.px, b.py, b.pz, b.radius, a, nx, ny, nz, depth)) {
            return false;
        }
        nx = -nx;
        ny = -ny;
        nz = -nz;
        return true;
    }
    if (sa == 0 && sb == 2) {
        return SphereCapsule(a.px, a.py, a.pz, a.radius, b, nx, ny, nz, depth);
    }
    if (sa == 2 && sb == 0) {
        if (!SphereCapsule(b.px, b.py, b.pz, b.radius, a, nx, ny, nz, depth)) {
            return false;
        }
        nx = -nx;
        ny = -ny;
        nz = -nz;
        return true;
    }
    if (sa == 2 && sb == 1) {
        return CapsuleBox(a, b, nx, ny, nz, depth); // box→capsule = b→a
    }
    if (sa == 1 && sb == 2) {
        if (!CapsuleBox(b, a, nx, ny, nz, depth)) {
            return false;
        }
        nx = -nx;
        ny = -ny;
        nz = -nz;
        return true;
    }
    return false; // 未知の shape 値は判定しない
}

bool Overlap(const ShapePose& a, const ShapePose& b)
{
    float nx, ny, nz, depth;
    return Collide(a, b, nx, ny, nz, depth);
}

bool Raycast(const ShapePose& s, float ox, float oy, float oz, float dx, float dy, float dz,
             float maxDist, float& outT, float& nx, float& ny, float& nz)
{
    if (s.shape == 0) {
        return RaySphere(ox, oy, oz, dx, dy, dz, s.px, s.py, s.pz, s.radius, maxDist, outT, nx,
                         ny, nz);
    }
    if (s.shape == 1) {
        return RayBox(s, ox, oy, oz, dx, dy, dz, maxDist, outT, nx, ny, nz);
    }
    if (s.shape == 2) {
        return RayCapsule(s, ox, oy, oz, dx, dy, dz, maxDist, outT, nx, ny, nz);
    }
    return false;
}

} // namespace shapes
} // namespace mye
