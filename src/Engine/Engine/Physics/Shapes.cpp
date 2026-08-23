#include "Engine/Engine/Physics/Shapes.h"

#include <algorithm>
#include <cmath>

#include "Engine/Core/Components.h"
#include "Engine/Engine/Physics/MeshColliderLibrary.h"
#include "Engine/Engine/Physics/TerrainColliderLibrary.h" // M59i: shape=4 の実体解決

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

// ---- box-box の SAT 本体。normal は b→a 方向 ----
// 面軸 6 (a 基底 3 + b 基底 3) + 辺×辺 9。軸の列挙順は固定 (決定論)。
// 最小重なり軸を厳密 < で選ぶ (同値は先勝ち = 決定論)。
// axisId: 0-2 = a の面軸 / 3-5 = b の面軸 / 6+ = 辺×辺 (6 + i*3 + j)
bool BoxBoxSat(const ShapePose& a, const ShapePose& b, float& nx, float& ny, float& nz,
               float& depth, int& axisId)
{
    const float dx = a.px - b.px, dy = a.py - b.py, dz = a.pz - b.pz;
    const float* aAxes[3] = { a.bx, a.by, a.bz };
    const float* bAxes[3] = { b.bx, b.by, b.bz };
    const float aExt[3] = { a.hx, a.hy, a.hz };
    const float bExt[3] = { b.hx, b.hy, b.hz };
    float bestDepth = 0;
    float bestX = 0, bestY = 1, bestZ = 0;
    int bestId = -1;
    bool first = true;
    bool separated = false;

    auto testAxis = [&](float ax, float ay, float az, int id) {
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
            bestId = id;
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
        testAxis(aAxes[i][0], aAxes[i][1], aAxes[i][2], i);
    }
    for (int i = 0; i < 3; ++i) {
        testAxis(bAxes[i][0], bAxes[i][1], bAxes[i][2], 3 + i);
    }
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            const float* u = aAxes[i];
            const float* v = bAxes[j];
            testAxis(u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2],
                     u[0] * v[1] - u[1] * v[0], 6 + i * 3 + j);
        }
    }
    if (separated || first) {
        return false;
    }
    nx = bestX;
    ny = bestY;
    nz = bestZ;
    depth = bestDepth;
    axisId = bestId;
    return true;
}

// ---- box-box (単一接触)。normal は b→a 方向 ----
// 無回転ペアは M20 の aabb-aabb コードそのまま (fast-path)。回転ありは SAT。
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
    int axisId;
    return BoxBoxSat(a, b, nx, ny, nz, depth, axisId);
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

// capsule 線分を箱ローカルへ変換する
void CapsuleSegmentLocalToBox(const ShapePose& c, const ShapePose& box, float& a0x, float& a0y,
                              float& a0z, float& a1x, float& a1y, float& a1z)
{
    float w0x, w0y, w0z, w1x, w1y, w1z;
    CapsuleSegment(c, w0x, w0y, w0z, w1x, w1y, w1z);
    WorldToLocal(box, w0x - box.px, w0y - box.py, w0z - box.pz, a0x, a0y, a0z);
    WorldToLocal(box, w1x - box.px, w1y - box.py, w1z - box.pz, a1x, a1y, a1z);
}

// 箱ローカルで dist²(seg(t), AABB) が t について凸なことを利用し、固定 32 回の黄金分割で
// 最近パラメータ t を求める (固定回数 = 決定論)
float GoldenSegParamToLocalAabb(float a0x, float a0y, float a0z, float a1x, float a1y, float a1z,
                                float hx, float hy, float hz)
{
    auto distSq = [&](float t) {
        const float x = a0x + (a1x - a0x) * t;
        const float y = a0y + (a1y - a0y) * t;
        const float z = a0z + (a1z - a0z) * t;
        float ex = std::fabs(x) - hx;
        float ey = std::fabs(y) - hy;
        float ez = std::fabs(z) - hz;
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
    return (lo + hi) * 0.5f;
}

// ---- capsule-box。normal は box→capsule 方向 ----
bool CapsuleBox(const ShapePose& c, const ShapePose& box, float& nx, float& ny, float& nz,
                float& depth)
{
    float a0x, a0y, a0z, a1x, a1y, a1z;
    CapsuleSegmentLocalToBox(c, box, a0x, a0y, a0z, a1x, a1y, a1z);
    const float t = GoldenSegParamToLocalAabb(a0x, a0y, a0z, a1x, a1y, a1z, box.hx, box.hy,
                                              box.hz);
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

// ---- 接触点つき判定 (M28b マニフォールド用) ----

// 球ペア + 接触点 (両表面の中点)。n は b→a
bool SpherePairContact(float ax, float ay, float az, float bx, float by, float bz, float ra,
                       float rb, float& nx, float& ny, float& nz, Contact& c)
{
    float depth;
    if (!SpherePair(ax, ay, az, bx, by, bz, ra + rb, nx, ny, nz, depth)) {
        return false;
    }
    c.px = 0.5f * ((ax - nx * ra) + (bx + nx * rb));
    c.py = 0.5f * ((ay - ny * ra) + (by + ny * rb));
    c.pz = 0.5f * ((az - nz * ra) + (bz + nz * rb));
    c.depth = depth;
    return true;
}

// sphere-box + 接触点 (箱表面の最近点。中心が箱内なら球中心)。n は box→sphere
bool SphereBoxContact(float spx, float spy, float spz, float sr, const ShapePose& box, float& nx,
                      float& ny, float& nz, Contact& c)
{
    if (box.identityRot) {
        float depth;
        if (!SphereAabb(spx, spy, spz, sr, box.px, box.py, box.pz, box.hx, box.hy, box.hz, nx, ny,
                        nz, depth)) {
            return false;
        }
        c.px = std::clamp(spx, box.px - box.hx, box.px + box.hx);
        c.py = std::clamp(spy, box.py - box.hy, box.py + box.hy);
        c.pz = std::clamp(spz, box.pz - box.hz, box.pz + box.hz);
        c.depth = depth;
        return true;
    }
    float lx, ly, lz;
    WorldToLocal(box, spx - box.px, spy - box.py, spz - box.pz, lx, ly, lz);
    float lnx, lny, lnz, depth;
    if (!SphereAabb(lx, ly, lz, sr, 0, 0, 0, box.hx, box.hy, box.hz, lnx, lny, lnz, depth)) {
        return false;
    }
    LocalToWorld(box, lnx, lny, lnz, nx, ny, nz);
    const float qx = std::clamp(lx, -box.hx, box.hx);
    const float qy = std::clamp(ly, -box.hy, box.hy);
    const float qz = std::clamp(lz, -box.hz, box.hz);
    float wx, wy, wz;
    LocalToWorld(box, qx, qy, qz, wx, wy, wz);
    c.px = box.px + wx;
    c.py = box.py + wy;
    c.pz = box.pz + wz;
    c.depth = depth;
    return true;
}

// box-box 面接触: 入射面を参照面の側面 4 平面でクリップし最大 4 点 (Box2D 流)。
// n はワールド共通法線 (b→a)、refIsA = SAT 勝者軸が a の面軸か。
void BoxBoxFaceManifold(const ShapePose& a, const ShapePose& b, bool refIsA, float nx, float ny,
                        float nz, float satDepth, Manifold& out)
{
    const ShapePose& ref = refIsA ? a : b;
    const ShapePose& inc = refIsA ? b : a;
    // 参照面の外向き法線 f (参照 box → 相手): a 参照なら -n、b 参照なら +n
    const float fx = refIsA ? -nx : nx;
    const float fy = refIsA ? -ny : ny;
    const float fz = refIsA ? -nz : nz;
    const float* refAxes[3] = { ref.bx, ref.by, ref.bz };
    const float refExt[3] = { ref.hx, ref.hy, ref.hz };
    int refAxis = 0;
    float bestAlign = -2.0f;
    for (int i = 0; i < 3; ++i) {
        const float d = std::fabs(Dot3(refAxes[i], fx, fy, fz));
        if (d > bestAlign) {
            bestAlign = d;
            refAxis = i;
        }
    }
    // 入射面: inc の 6 面から f に最も逆向きの面 (固定列挙・厳密 < = 決定論)
    const float* incAxes[3] = { inc.bx, inc.by, inc.bz };
    const float incExt[3] = { inc.hx, inc.hy, inc.hz };
    int incAxis = 0;
    float incSign = 1.0f;
    float bestD = 2.0f;
    for (int i = 0; i < 3; ++i) {
        const float d = Dot3(incAxes[i], fx, fy, fz);
        if (d < bestD) {
            bestD = d;
            incAxis = i;
            incSign = 1.0f;
        }
        if (-d < bestD) {
            bestD = -d;
            incAxis = i;
            incSign = -1.0f;
        }
    }
    // 入射面の 4 頂点 (ワールド)
    const int u = (incAxis + 1) % 3;
    const int v = (incAxis + 2) % 3;
    float poly[8][3];
    float tmp[8][3];
    int polyN = 4;
    const float su[4] = { 1, 1, -1, -1 };
    const float sv[4] = { 1, -1, -1, 1 };
    const float incC[3] = { inc.px, inc.py, inc.pz };
    for (int k = 0; k < 4; ++k) {
        for (int d = 0; d < 3; ++d) {
            poly[k][d] = incC[d] + incAxes[incAxis][d] * incExt[incAxis] * incSign
                       + incAxes[u][d] * incExt[u] * su[k] + incAxes[v][d] * incExt[v] * sv[k];
        }
    }
    // 参照面の側面 4 平面でクリップ (Sutherland-Hodgman): dot(p - c, ±axes[m]) <= ext[m]
    for (int m = 0; m < 3 && polyN > 0; ++m) {
        if (m == refAxis) {
            continue;
        }
        for (int side = 0; side < 2 && polyN > 0; ++side) {
            const float sgn = side ? -1.0f : 1.0f;
            const float pnx = refAxes[m][0] * sgn;
            const float pny = refAxes[m][1] * sgn;
            const float pnz = refAxes[m][2] * sgn;
            int outN = 0;
            for (int k = 0; k < polyN; ++k) {
                const int k2 = (k + 1) % polyN;
                const float dk = (poly[k][0] - ref.px) * pnx + (poly[k][1] - ref.py) * pny
                               + (poly[k][2] - ref.pz) * pnz - refExt[m];
                const float dk2 = (poly[k2][0] - ref.px) * pnx + (poly[k2][1] - ref.py) * pny
                                + (poly[k2][2] - ref.pz) * pnz - refExt[m];
                if (dk <= 0 && outN < 8) {
                    tmp[outN][0] = poly[k][0];
                    tmp[outN][1] = poly[k][1];
                    tmp[outN][2] = poly[k][2];
                    ++outN;
                }
                if (((dk <= 0) != (dk2 <= 0)) && outN < 8) {
                    const float t = dk / (dk - dk2);
                    for (int d = 0; d < 3; ++d) {
                        tmp[outN][d] = poly[k][d] + (poly[k2][d] - poly[k][d]) * t;
                    }
                    ++outN;
                }
            }
            polyN = outN;
            for (int k = 0; k < polyN; ++k) {
                poly[k][0] = tmp[k][0];
                poly[k][1] = tmp[k][1];
                poly[k][2] = tmp[k][2];
            }
        }
    }
    // 参照面より下 (貫通側) の点を集める
    Contact cand[8];
    int candN = 0;
    for (int k = 0; k < polyN && candN < 8; ++k) {
        const float fdot = (poly[k][0] - ref.px) * fx + (poly[k][1] - ref.py) * fy
                         + (poly[k][2] - ref.pz) * fz;
        const float dep = refExt[refAxis] - fdot;
        if (dep >= 0) {
            cand[candN].px = poly[k][0];
            cand[candN].py = poly[k][1];
            cand[candN].pz = poly[k][2];
            cand[candN].depth = dep;
            ++candN;
        }
    }
    if (candN == 0) {
        // 数値縮退: 入射 box の最深頂点 1 点 (SAT depth を採用)
        float vx = inc.px, vy = inc.py, vz = inc.pz;
        for (int k = 0; k < 3; ++k) {
            const float s = (Dot3(incAxes[k], fx, fy, fz) >= 0) ? -1.0f : 1.0f;
            vx += incAxes[k][0] * incExt[k] * s;
            vy += incAxes[k][1] * incExt[k] * s;
            vz += incAxes[k][2] * incExt[k] * s;
        }
        out.pts[0] = { vx, vy, vz, satDepth };
        out.count = 1;
        return;
    }
    // depth 降順の上位 4 点 (同値は元順先勝ち = 決定論)
    bool used[8] = {};
    out.count = 0;
    const int take = (candN < 4) ? candN : 4;
    for (int slot = 0; slot < take; ++slot) {
        int best = -1;
        for (int k = 0; k < candN; ++k) {
            if (used[k]) {
                continue;
            }
            if (best < 0 || cand[k].depth > cand[best].depth) {
                best = k;
            }
        }
        used[best] = true;
        out.pts[out.count++] = cand[best];
    }
}

// box-box 辺-辺接触: 支持辺同士の最近点 1 点。n は b→a
void BoxBoxEdgeManifold(const ShapePose& a, const ShapePose& b, int ai, int bi, float nx, float ny,
                        float nz, float depth, Manifold& out)
{
    const float* aAxes[3] = { a.bx, a.by, a.bz };
    const float* bAxes[3] = { b.bx, b.by, b.bz };
    const float aExt[3] = { a.hx, a.hy, a.hz };
    const float bExt[3] = { b.hx, b.hy, b.hz };
    // a の支持辺 (b 側 = -n 方向の面上、方向 aAxes[ai])
    float ac[3] = { a.px, a.py, a.pz };
    for (int k = 0; k < 3; ++k) {
        if (k == ai) {
            continue;
        }
        const float s = (Dot3(aAxes[k], nx, ny, nz) >= 0) ? -1.0f : 1.0f;
        ac[0] += aAxes[k][0] * aExt[k] * s;
        ac[1] += aAxes[k][1] * aExt[k] * s;
        ac[2] += aAxes[k][2] * aExt[k] * s;
    }
    // b の支持辺 (a 側 = +n 方向の面上、方向 bAxes[bi])
    float bc[3] = { b.px, b.py, b.pz };
    for (int k = 0; k < 3; ++k) {
        if (k == bi) {
            continue;
        }
        const float s = (Dot3(bAxes[k], nx, ny, nz) >= 0) ? 1.0f : -1.0f;
        bc[0] += bAxes[k][0] * bExt[k] * s;
        bc[1] += bAxes[k][1] * bExt[k] * s;
        bc[2] += bAxes[k][2] * bExt[k] * s;
    }
    float s, t;
    ClosestSegSeg(ac[0] - aAxes[ai][0] * aExt[ai], ac[1] - aAxes[ai][1] * aExt[ai],
                  ac[2] - aAxes[ai][2] * aExt[ai], ac[0] + aAxes[ai][0] * aExt[ai],
                  ac[1] + aAxes[ai][1] * aExt[ai], ac[2] + aAxes[ai][2] * aExt[ai],
                  bc[0] - bAxes[bi][0] * bExt[bi], bc[1] - bAxes[bi][1] * bExt[bi],
                  bc[2] - bAxes[bi][2] * bExt[bi], bc[0] + bAxes[bi][0] * bExt[bi],
                  bc[1] + bAxes[bi][1] * bExt[bi], bc[2] + bAxes[bi][2] * bExt[bi], s, t);
    const float pax = ac[0] + aAxes[ai][0] * aExt[ai] * (2.0f * s - 1.0f);
    const float pay = ac[1] + aAxes[ai][1] * aExt[ai] * (2.0f * s - 1.0f);
    const float paz = ac[2] + aAxes[ai][2] * aExt[ai] * (2.0f * s - 1.0f);
    const float pbx = bc[0] + bAxes[bi][0] * bExt[bi] * (2.0f * t - 1.0f);
    const float pby = bc[1] + bAxes[bi][1] * bExt[bi] * (2.0f * t - 1.0f);
    const float pbz = bc[2] + bAxes[bi][2] * bExt[bi] * (2.0f * t - 1.0f);
    out.pts[0] = { (pax + pbx) * 0.5f, (pay + pby) * 0.5f, (paz + pbz) * 0.5f, depth };
    out.count = 1;
}

// capsule-box マニフォールド。n は box→capsule。横倒しの線接触は端点も追加 (最大 3 点)
bool CapsuleBoxManifold(const ShapePose& c, const ShapePose& box, Manifold& out)
{
    float a0x, a0y, a0z, a1x, a1y, a1z;
    CapsuleSegmentLocalToBox(c, box, a0x, a0y, a0z, a1x, a1y, a1z);
    const float t = GoldenSegParamToLocalAabb(a0x, a0y, a0z, a1x, a1y, a1z, box.hx, box.hy,
                                              box.hz);
    float w0x, w0y, w0z, w1x, w1y, w1z;
    CapsuleSegment(c, w0x, w0y, w0z, w1x, w1y, w1z);
    auto segW = [&](float tt, float& x, float& y, float& z) {
        x = w0x + (w1x - w0x) * tt;
        y = w0y + (w1y - w0y) * tt;
        z = w0z + (w1z - w0z) * tt;
    };
    float sx, sy, sz;
    segW(t, sx, sy, sz);
    float nx, ny, nz;
    Contact c0;
    if (!SphereBoxContact(sx, sy, sz, c.radius, box, nx, ny, nz, c0)) {
        return false;
    }
    out.nx = nx;
    out.ny = ny;
    out.nz = nz;
    out.count = 0;
    out.pts[out.count++] = c0;
    const float ends[2] = { 0.0f, 1.0f };
    for (float te : ends) {
        if (std::fabs(te - t) < 1e-3f || out.count >= 4) {
            continue;
        }
        float ex, ey, ez;
        segW(te, ex, ey, ez);
        float enx, eny, enz;
        Contact ce;
        if (SphereBoxContact(ex, ey, ez, c.radius, box, enx, eny, enz, ce)
            && enx * nx + eny * ny + enz * nz > 0.99f) {
            out.pts[out.count++] = ce;
        }
    }
    return true;
}

// capsule-capsule マニフォールド。n は b→a。ほぼ平行な側面接触は a の両端点で最大 2 点
bool CapsuleCapsuleManifold(const ShapePose& a, const ShapePose& b, Manifold& out)
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
    float nx, ny, nz;
    Contact c0;
    if (!SpherePairContact(pax, pay, paz, pbx, pby, pbz, a.radius, b.radius, nx, ny, nz, c0)) {
        return false;
    }
    out.nx = nx;
    out.ny = ny;
    out.nz = nz;
    out.pts[0] = c0;
    out.count = 1;
    // ほぼ平行 (cos² > 0.98) なら a の両端点球 vs b 線分で線接触 2 点に置き換え
    const float dax = a1x - a0x, day = a1y - a0y, daz = a1z - a0z;
    const float dbx = b1x - b0x, dby = b1y - b0y, dbz = b1z - b0z;
    const float la2 = dax * dax + day * day + daz * daz;
    const float lb2 = dbx * dbx + dby * dby + dbz * dbz;
    if (la2 > 1e-8f && lb2 > 1e-8f) {
        const float d = dax * dbx + day * dby + daz * dbz;
        if (d * d > 0.98f * la2 * lb2) {
            Contact epts[2];
            int en = 0;
            const float ends[2][3] = { { a0x, a0y, a0z }, { a1x, a1y, a1z } };
            for (int k = 0; k < 2; ++k) {
                const float ex = ends[k][0], ey = ends[k][1], ez = ends[k][2];
                const float tb = std::clamp(
                    ((ex - b0x) * dbx + (ey - b0y) * dby + (ez - b0z) * dbz) / lb2, 0.0f, 1.0f);
                const float qx = b0x + dbx * tb, qy = b0y + dby * tb, qz = b0z + dbz * tb;
                float enx, eny, enz;
                Contact ce;
                if (SpherePairContact(ex, ey, ez, qx, qy, qz, a.radius, b.radius, enx, eny, enz,
                                      ce)
                    && enx * nx + eny * ny + enz * nz > 0.99f) {
                    epts[en++] = ce;
                }
            }
            if (en > 0) {
                out.count = en;
                for (int k = 0; k < en; ++k) {
                    out.pts[k] = epts[k];
                }
            }
        }
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
    // M41: メッシュはスケールを別持ち (三角形をワールドへ変換する時に掛ける)。
    // 実体解決もここで一元化 — 全 pose 構築サイト (ソルバ/CC/トリガー/クエリ/レイ) が
    // 自動で対応する。未接続/未ロードは null のまま = shape=3 は衝突なしに落ちる
    p.sx = sx;
    p.sy = sy;
    p.sz = sz;
    if (col.shape == 3) {
        p.meshData = meshcol::Resolve(col.meshAsset);
    } else if (col.shape == 4) {
        // M59i: 同じスロットに地形データを載せる (判別は shape 値)。
        // meshAsset が `.terrain.json` を指す — 自然な使い方は「TerrainComponent と
        // 同じエンティティに shape=4 の Collider を置き、同じ地形を指す」
        p.meshData = terraincol::Resolve(col.meshAsset);
    }
}

// ================================================================ 静的メッシュ (M41)
// 全て「動的形状 vs ワールド変換済み三角形 1 枚」に帰着させる。候補三角形は BVH で AABB
// カリング → **昇順** 判定 (MeshGatherTris がソート済みで返す = 走査順非依存の決定論)。
// 最深貫通の三角形を採用 (同深度は小さい三角形番号が勝つ = strict > 更新)。

constexpr int kMeshMaxCandidates = 256; // AABB カリング後の候補三角形上限 (十分に大きい)

const MeshColliderData* MeshOf(const ShapePose& m)
{
    return static_cast<const MeshColliderData*>(m.meshData);
}

// メッシュローカル頂点 → ワールド (スケール → 基底回転 → 平行移動)
void MeshVertToWorld(const ShapePose& m, const DirectX::XMFLOAT3& v, float& x, float& y, float& z)
{
    float wx, wy, wz;
    LocalToWorld(m, v.x * m.sx, v.y * m.sy, v.z * m.sz, wx, wy, wz);
    x = m.px + wx;
    y = m.py + wy;
    z = m.pz + wz;
}

void MeshWorldTri(const ShapePose& m, const MeshColliderData& md, int32_t tri, float& ax,
                  float& ay, float& az, float& bx, float& by, float& bz, float& cx, float& cy,
                  float& cz)
{
    MeshVertToWorld(m, md.positions[md.indices[tri * 3 + 0]], ax, ay, az);
    MeshVertToWorld(m, md.positions[md.indices[tri * 3 + 1]], bx, by, bz);
    MeshVertToWorld(m, md.positions[md.indices[tri * 3 + 2]], cx, cy, cz);
}

// ワールド AABB → メッシュローカル AABB (保守的。逆回転 = 転置基底、逆スケール)。
// スケールは MakePose 系で fabs 済み = 常に正
void WorldAabbToMeshLocal(const ShapePose& m, float wminX, float wminY, float wminZ, float wmaxX,
                          float wmaxY, float wmaxZ, float& lminX, float& lminY, float& lminZ,
                          float& lmaxX, float& lmaxY, float& lmaxZ)
{
    const float wcx = (wminX + wmaxX) * 0.5f - m.px;
    const float wcy = (wminY + wmaxY) * 0.5f - m.py;
    const float wcz = (wminZ + wmaxZ) * 0.5f - m.pz;
    const float ex = (wmaxX - wminX) * 0.5f;
    const float ey = (wmaxY - wminY) * 0.5f;
    const float ez = (wmaxZ - wminZ) * 0.5f;
    float lcx, lcy, lcz;
    WorldToLocal(m, wcx, wcy, wcz, lcx, lcy, lcz);
    const float lex = std::fabs(m.bx[0]) * ex + std::fabs(m.bx[1]) * ey + std::fabs(m.bx[2]) * ez;
    const float ley = std::fabs(m.by[0]) * ex + std::fabs(m.by[1]) * ey + std::fabs(m.by[2]) * ez;
    const float lez = std::fabs(m.bz[0]) * ex + std::fabs(m.bz[1]) * ey + std::fabs(m.bz[2]) * ez;
    const float isx = 1.0f / std::max(m.sx, 1e-6f);
    const float isy = 1.0f / std::max(m.sy, 1e-6f);
    const float isz = 1.0f / std::max(m.sz, 1e-6f);
    lminX = (lcx - lex) * isx;
    lmaxX = (lcx + lex) * isx;
    lminY = (lcy - ley) * isy;
    lmaxY = (lcy + ley) * isy;
    lminZ = (lcz - lez) * isz;
    lmaxZ = (lcz + lez) * isz;
}

// 他形状のワールド AABB (+margin) と重なる候補三角形を昇順収集
int GatherTrisForShape(const ShapePose& mesh, const ShapePose& other, float margin, int32_t* buf,
                       int cap)
{
    const MeshColliderData* md = MeshOf(mesh);
    if (!md) {
        return 0;
    }
    float wminX, wminY, wminZ, wmaxX, wmaxY, wmaxZ;
    ComputeAabb(other, wminX, wminY, wminZ, wmaxX, wmaxY, wmaxZ);
    float lminX, lminY, lminZ, lmaxX, lmaxY, lmaxZ;
    WorldAabbToMeshLocal(mesh, wminX - margin, wminY - margin, wminZ - margin, wmaxX + margin,
                         wmaxY + margin, wmaxZ + margin, lminX, lminY, lminZ, lmaxX, lmaxY, lmaxZ);
    return MeshGatherTris(*md, lminX, lminY, lminZ, lmaxX, lmaxY, lmaxZ, buf, cap);
}

// ================================================================ 地形 (M59i)
// ハイトフィールドは「三角形の集まり」という一点でメッシュと同じ扱いができる。
// 違うのは 2 つだけ:
//   (a) BVH が要らない — セル格子なので候補は AABB からの矩形走査で直接刻める
//   (b) 三角形番号が座標そのもの — tri = (iz * tilesX + ix) * 2 + half
// 巻き順・頂点位置は TerrainSystem::BuildChunkMesh (LOD 0) と**同じ式**にしてある。
// ずらすと「見えている地面」と「当たる地面」が食い違う。
const TerrainCollisionData* TerrainOf(const ShapePose& s)
{
    return static_cast<const TerrainCollisionData*>(s.meshData);
}

bool TerrainUsable(const TerrainCollisionData* t)
{
    return t && t->data.heightW >= 2 && t->data.heightH >= 2 && t->data.worldSizeX > 0.0f
        && t->data.worldSizeZ > 0.0f;
}

// texel (tx, tz) の**地形ローカル**頂点。
// ★HeightAtTexel を直接呼ばない — あちらの引数は uint32_t なので負値が 0xFFFFFFFF に
//   化け、内部の std::min クランプで「反対側の端」に落ちる。符号付きで受けて自分で
//   クランプするのが唯一の正しい入口 (TerrainSystem.cpp の HeightClamped と同じ罠)
void TerrainLocalVert(const TerrainAsset::TerrainData& d, int32_t tx, int32_t tz, float& x,
                      float& y, float& z)
{
    const int32_t maxX = static_cast<int32_t>(d.heightW) - 1;
    const int32_t maxZ = static_cast<int32_t>(d.heightH) - 1;
    const int32_t cx = (tx < 0) ? 0 : ((tx > maxX) ? maxX : tx);
    const int32_t cz = (tz < 0) ? 0 : ((tz > maxZ) ? maxZ : tz);
    const float u = static_cast<float>(cx) / static_cast<float>(maxX);
    const float v = static_cast<float>(cz) / static_cast<float>(maxZ);
    x = (u - 0.5f) * d.worldSizeX; // 中心原点 (BuildChunkMesh と同じ規約)
    y = d.HeightAtTexel(static_cast<uint32_t>(cx), static_cast<uint32_t>(cz));
    z = (v - 0.5f) * d.worldSizeZ;
}

void TerrainWorldTri(const ShapePose& s, const TerrainCollisionData& t, int32_t tri, float& ax,
                     float& ay, float& az, float& bx, float& by, float& bz, float& cx, float& cy,
                     float& cz)
{
    const TerrainAsset::TerrainData& d = t.data;
    const int32_t tilesX = static_cast<int32_t>(d.heightW) - 1;
    const int32_t cell = tri >> 1;
    const int32_t half = tri & 1;
    const int32_t ix = cell % tilesX;
    const int32_t iz = cell / tilesX;
    // BuildChunkMesh と同じ巻き順 (i00,i01,i11) / (i00,i11,i10) — 法線が +Y になる
    int32_t vx[3], vz[3];
    if (half == 0) {
        vx[0] = ix;     vz[0] = iz;
        vx[1] = ix;     vz[1] = iz + 1;
        vx[2] = ix + 1; vz[2] = iz + 1;
    } else {
        vx[0] = ix;     vz[0] = iz;
        vx[1] = ix + 1; vz[1] = iz + 1;
        vx[2] = ix + 1; vz[2] = iz;
    }
    float out[3][3];
    for (int k = 0; k < 3; ++k) {
        float lx, ly, lz;
        TerrainLocalVert(d, vx[k], vz[k], lx, ly, lz);
        float wx, wy, wz;
        LocalToWorld(s, lx * s.sx, ly * s.sy, lz * s.sz, wx, wy, wz);
        out[k][0] = s.px + wx;
        out[k][1] = s.py + wy;
        out[k][2] = s.pz + wz;
    }
    ax = out[0][0]; ay = out[0][1]; az = out[0][2];
    bx = out[1][0]; by = out[1][1]; bz = out[1][2];
    cx = out[2][0]; cy = out[2][1]; cz = out[2][2];
}

// ローカル x/z 範囲 → セル範囲 (地形の外は空になる)。戻り値 = 範囲があるか
bool TerrainCellRange(const TerrainAsset::TerrainData& d, float lminX, float lminZ, float lmaxX,
                      float lmaxZ, int32_t& ix0, int32_t& iz0, int32_t& ix1, int32_t& iz1)
{
    const int32_t tilesX = static_cast<int32_t>(d.heightW) - 1;
    const int32_t tilesZ = static_cast<int32_t>(d.heightH) - 1;
    const float stepX = d.worldSizeX / static_cast<float>(tilesX);
    const float stepZ = d.worldSizeZ / static_cast<float>(tilesZ);
    ix0 = static_cast<int32_t>(std::floor((lminX + d.worldSizeX * 0.5f) / stepX));
    ix1 = static_cast<int32_t>(std::floor((lmaxX + d.worldSizeX * 0.5f) / stepX));
    iz0 = static_cast<int32_t>(std::floor((lminZ + d.worldSizeZ * 0.5f) / stepZ));
    iz1 = static_cast<int32_t>(std::floor((lmaxZ + d.worldSizeZ * 0.5f) / stepZ));
    if (ix0 < 0) { ix0 = 0; }
    if (iz0 < 0) { iz0 = 0; }
    if (ix1 > tilesX - 1) { ix1 = tilesX - 1; }
    if (iz1 > tilesZ - 1) { iz1 = tilesZ - 1; }
    return ix0 <= ix1 && iz0 <= iz1;
}

// 他形状のワールド AABB (+margin) と重なるセルの三角形番号を**昇順**で収集
int TerrainGatherTris(const ShapePose& terr, const ShapePose& other, float margin, int32_t* buf,
                      int cap)
{
    const TerrainCollisionData* t = TerrainOf(terr);
    if (!TerrainUsable(t)) {
        return 0;
    }
    float wminX, wminY, wminZ, wmaxX, wmaxY, wmaxZ;
    ComputeAabb(other, wminX, wminY, wminZ, wmaxX, wmaxY, wmaxZ);
    float lminX, lminY, lminZ, lmaxX, lmaxY, lmaxZ;
    WorldAabbToMeshLocal(terr, wminX - margin, wminY - margin, wminZ - margin, wmaxX + margin,
                         wmaxY + margin, wmaxZ + margin, lminX, lminY, lminZ, lmaxX, lmaxY, lmaxZ);
    int32_t ix0, iz0, ix1, iz1;
    if (!TerrainCellRange(t->data, lminX, lminZ, lmaxX, lmaxZ, ix0, iz0, ix1, iz1)) {
        return 0;
    }
    const int32_t tilesX = static_cast<int32_t>(t->data.heightW) - 1;
    int n = 0;
    // iz 外・ix 内の順 = セル番号昇順 = 三角形番号昇順 (走査順非依存の決定論)
    for (int32_t iz = iz0; iz <= iz1; ++iz) {
        for (int32_t ix = ix0; ix <= ix1; ++ix) {
            const int32_t cell = iz * tilesX + ix;
            if (n + 2 > cap) {
                return n; // メッシュ側と同じ「上限で打ち切る」規約 (保守的に取りこぼす)
            }
            buf[n++] = cell * 2;
            buf[n++] = cell * 2 + 1;
        }
    }
    return n;
}

// ---- 三角形スープの共通入口 (shape=3 / shape=4) ----
// 「候補の集め方」と「番号 → ワールド三角形」だけ差し替えれば、衝突・マニフォールド・
// 最近点の本体は 1 つで済む
bool IsSoup(const ShapePose& s)
{
    return s.shape == 3 || s.shape == 4;
}

bool SoupUsable(const ShapePose& s)
{
    if (s.shape == 3) {
        return MeshOf(s) != nullptr;
    }
    if (s.shape == 4) {
        return TerrainUsable(TerrainOf(s));
    }
    return false;
}

int SoupGatherTris(const ShapePose& s, const ShapePose& other, float margin, int32_t* buf, int cap)
{
    if (s.shape == 4) {
        return TerrainGatherTris(s, other, margin, buf, cap);
    }
    return GatherTrisForShape(s, other, margin, buf, cap);
}

void SoupWorldTri(const ShapePose& s, int32_t tri, float& ax, float& ay, float& az, float& bx,
                  float& by, float& bz, float& cx, float& cy, float& cz)
{
    if (s.shape == 4) {
        TerrainWorldTri(s, *TerrainOf(s), tri, ax, ay, az, bx, by, bz, cx, cy, cz);
        return;
    }
    MeshWorldTri(s, *MeshOf(s), tri, ax, ay, az, bx, by, bz, cx, cy, cz);
}

// Ericson 5.1.5: 点 P の三角形 ABC 上の最近点 (scalar、分岐は入力のみに依存)
void ClosestPtPointTri(float px, float py, float pz, float ax, float ay, float az, float bx,
                       float by, float bz, float cx, float cy, float cz, float& qx, float& qy,
                       float& qz)
{
    const float abx = bx - ax, aby = by - ay, abz = bz - az;
    const float acx = cx - ax, acy = cy - ay, acz = cz - az;
    const float apx = px - ax, apy = py - ay, apz = pz - az;
    const float d1 = abx * apx + aby * apy + abz * apz;
    const float d2 = acx * apx + acy * apy + acz * apz;
    if (d1 <= 0.0f && d2 <= 0.0f) {
        qx = ax; qy = ay; qz = az;
        return;
    }
    const float bpx = px - bx, bpy = py - by, bpz = pz - bz;
    const float d3 = abx * bpx + aby * bpy + abz * bpz;
    const float d4 = acx * bpx + acy * bpy + acz * bpz;
    if (d3 >= 0.0f && d4 <= d3) {
        qx = bx; qy = by; qz = bz;
        return;
    }
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        const float denom = d1 - d3;
        const float v = (std::fabs(denom) > kDegenerateEps) ? d1 / denom : 0.0f;
        qx = ax + abx * v; qy = ay + aby * v; qz = az + abz * v;
        return;
    }
    const float cpx = px - cx, cpy = py - cy, cpz = pz - cz;
    const float d5 = abx * cpx + aby * cpy + abz * cpz;
    const float d6 = acx * cpx + acy * cpy + acz * cpz;
    if (d6 >= 0.0f && d5 <= d6) {
        qx = cx; qy = cy; qz = cz;
        return;
    }
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        const float denom = d2 - d6;
        const float w = (std::fabs(denom) > kDegenerateEps) ? d2 / denom : 0.0f;
        qx = ax + acx * w; qy = ay + acy * w; qz = az + acz * w;
        return;
    }
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        const float denom = (d4 - d3) + (d5 - d6);
        const float w = (std::fabs(denom) > kDegenerateEps) ? (d4 - d3) / denom : 0.0f;
        qx = bx + (cx - bx) * w; qy = by + (cy - by) * w; qz = bz + (cz - bz) * w;
        return;
    }
    const float denom = va + vb + vc;
    const float inv = (std::fabs(denom) > kDegenerateEps) ? 1.0f / denom : 0.0f;
    const float v = vb * inv;
    const float w = vc * inv;
    qx = ax + abx * v + acx * w;
    qy = ay + aby * v + acy * w;
    qz = az + abz * v + acz * w;
}

// 三角形の面法線 (正規化)。縮退は (0,1,0)
void TriFaceNormal(float ax, float ay, float az, float bx, float by, float bz, float cx, float cy,
                   float cz, float& nx, float& ny, float& nz)
{
    const float ux = bx - ax, uy = by - ay, uz = bz - az;
    const float vx = cx - ax, vy = cy - ay, vz = cz - az;
    nx = uy * vz - uz * vy;
    ny = uz * vx - ux * vz;
    nz = ux * vy - uy * vx;
    const float len2 = nx * nx + ny * ny + nz * nz;
    if (len2 < kDegenerateEps) {
        nx = 0.0f; ny = 1.0f; nz = 0.0f;
        return;
    }
    const float inv = 1.0f / std::sqrt(len2);
    nx *= inv; ny *= inv; nz *= inv;
}

// 球 vs 三角形。normal は三角形→球 (球を押し出す方向)、contact は三角形上の最近点
bool SphereTriContact(float spx, float spy, float spz, float r, float ax, float ay, float az,
                      float bx, float by, float bz, float cx, float cy, float cz, float& nx,
                      float& ny, float& nz, float& depth, float& qx, float& qy, float& qz)
{
    ClosestPtPointTri(spx, spy, spz, ax, ay, az, bx, by, bz, cx, cy, cz, qx, qy, qz);
    const float dx = spx - qx, dy = spy - qy, dz = spz - qz;
    const float d2 = dx * dx + dy * dy + dz * dz;
    if (d2 > r * r) {
        return false;
    }
    const float d = std::sqrt(d2);
    if (d > 1e-6f) {
        nx = dx / d; ny = dy / d; nz = dz / d;
    } else {
        TriFaceNormal(ax, ay, az, bx, by, bz, cx, cy, cz, nx, ny, nz);
    }
    depth = r - d;
    return true;
}

// 線分 (p0→p1) と三角形の最近点対。候補 = 両端点 vs 面 / 線分 vs 3 辺 / 平面貫通点 (固定順)
void ClosestSegTri(float p0x, float p0y, float p0z, float p1x, float p1y, float p1z, float ax,
                   float ay, float az, float bx, float by, float bz, float cx, float cy, float cz,
                   float& sx, float& sy, float& sz, float& tx, float& ty, float& tz)
{
    float bestD2 = 3.4e38f;
    auto consider = [&](float csx, float csy, float csz, float ctx, float cty, float ctz) {
        const float dx = csx - ctx, dy = csy - cty, dz = csz - ctz;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < bestD2) { // strict < = 先の候補 (固定順) が勝つ
            bestD2 = d2;
            sx = csx; sy = csy; sz = csz;
            tx = ctx; ty = cty; tz = ctz;
        }
    };
    float qx, qy, qz;
    // 1) 両端点 vs 三角形
    ClosestPtPointTri(p0x, p0y, p0z, ax, ay, az, bx, by, bz, cx, cy, cz, qx, qy, qz);
    consider(p0x, p0y, p0z, qx, qy, qz);
    ClosestPtPointTri(p1x, p1y, p1z, ax, ay, az, bx, by, bz, cx, cy, cz, qx, qy, qz);
    consider(p1x, p1y, p1z, qx, qy, qz);
    // 2) 線分 vs 3 辺 (a→b, b→c, c→a の固定順)
    const float ex[3] = { ax, bx, cx }, ey[3] = { ay, by, cy }, ez[3] = { az, bz, cz };
    for (int i = 0; i < 3; ++i) {
        const int j = (i + 1) % 3;
        float s, t;
        ClosestSegSeg(p0x, p0y, p0z, p1x, p1y, p1z, ex[i], ey[i], ez[i], ex[j], ey[j], ez[j], s,
                      t);
        const float csx = p0x + (p1x - p0x) * s, csy = p0y + (p1y - p0y) * s,
                    csz = p0z + (p1z - p0z) * s;
        const float ctx = ex[i] + (ex[j] - ex[i]) * t, cty = ey[i] + (ey[j] - ey[i]) * t,
                    ctz = ez[i] + (ez[j] - ez[i]) * t;
        consider(csx, csy, csz, ctx, cty, ctz);
    }
    // 3) 線分が三角形平面を貫通し、交点が三角形内部 → 距離 0
    float fnx, fny, fnz;
    TriFaceNormal(ax, ay, az, bx, by, bz, cx, cy, cz, fnx, fny, fnz);
    const float d0 = fnx * (p0x - ax) + fny * (p0y - ay) + fnz * (p0z - az);
    const float d1 = fnx * (p1x - ax) + fny * (p1y - ay) + fnz * (p1z - az);
    if (d0 * d1 < 0.0f) {
        const float t = d0 / (d0 - d1);
        const float ix = p0x + (p1x - p0x) * t, iy = p0y + (p1y - p0y) * t,
                    iz = p0z + (p1z - p0z) * t;
        ClosestPtPointTri(ix, iy, iz, ax, ay, az, bx, by, bz, cx, cy, cz, qx, qy, qz);
        const float dx = ix - qx, dy = iy - qy, dz = iz - qz;
        if (dx * dx + dy * dy + dz * dz < 1e-10f) {
            consider(ix, iy, iz, ix, iy, iz); // 貫通 = 距離 0
        }
    }
}

// カプセル vs 三角形。normal は三角形→カプセル
bool CapsuleTriContact(const ShapePose& cap, float ax, float ay, float az, float bx, float by,
                       float bz, float cx, float cy, float cz, float& nx, float& ny, float& nz,
                       float& depth, float& qx, float& qy, float& qz)
{
    float a0x, a0y, a0z, a1x, a1y, a1z;
    CapsuleSegment(cap, a0x, a0y, a0z, a1x, a1y, a1z);
    float sx, sy, sz;
    ClosestSegTri(a0x, a0y, a0z, a1x, a1y, a1z, ax, ay, az, bx, by, bz, cx, cy, cz, sx, sy, sz,
                  qx, qy, qz);
    const float dx = sx - qx, dy = sy - qy, dz = sz - qz;
    const float d2 = dx * dx + dy * dy + dz * dz;
    if (d2 > cap.radius * cap.radius) {
        return false;
    }
    const float d = std::sqrt(d2);
    if (d > 1e-6f) {
        nx = dx / d; ny = dy / d; nz = dz / d;
    } else {
        TriFaceNormal(ax, ay, az, bx, by, bz, cx, cy, cz, nx, ny, nz);
        // 貫通時はカプセル中心のある側へ向ける
        const float side = nx * (cap.px - ax) + ny * (cap.py - ay) + nz * (cap.pz - az);
        if (side < 0.0f) {
            nx = -nx; ny = -ny; nz = -nz;
        }
    }
    depth = cap.radius - d;
    return true;
}

// ボックス vs 三角形の SAT (13 軸、ボックスローカル空間)。normal は三角形→ボックス (ワールド)
bool BoxTriSat(const ShapePose& box, float ax, float ay, float az, float bx, float by, float bz,
               float cx, float cy, float cz, float& nx, float& ny, float& nz, float& depth)
{
    // 三角形をボックスローカルへ (中心原点、半幅 hx/hy/hz)
    float v[3][3];
    WorldToLocal(box, ax - box.px, ay - box.py, az - box.pz, v[0][0], v[0][1], v[0][2]);
    WorldToLocal(box, bx - box.px, by - box.py, bz - box.pz, v[1][0], v[1][1], v[1][2]);
    WorldToLocal(box, cx - box.px, cy - box.py, cz - box.pz, v[2][0], v[2][1], v[2][2]);
    const float f[3][3] = {
        { v[1][0] - v[0][0], v[1][1] - v[0][1], v[1][2] - v[0][2] }, // b-a
        { v[2][0] - v[1][0], v[2][1] - v[1][1], v[2][2] - v[1][2] }, // c-b
        { v[0][0] - v[2][0], v[0][1] - v[2][1], v[0][2] - v[2][2] }, // a-c
    };
    const float h[3] = { box.hx, box.hy, box.hz };

    float bestDepth = 3.4e38f;
    float bestAxis[3] = { 0, 1, 0 };
    bool found = false;

    auto testAxis = [&](float lx, float ly, float lz) -> bool {
        const float len2 = lx * lx + ly * ly + lz * lz;
        if (len2 < 1e-10f) {
            return true; // 縮退軸はスキップ (分離を主張しない)
        }
        float tmin = v[0][0] * lx + v[0][1] * ly + v[0][2] * lz;
        float tmax = tmin;
        for (int i = 1; i < 3; ++i) {
            const float p = v[i][0] * lx + v[i][1] * ly + v[i][2] * lz;
            tmin = (p < tmin) ? p : tmin;
            tmax = (p > tmax) ? p : tmax;
        }
        const float r = h[0] * std::fabs(lx) + h[1] * std::fabs(ly) + h[2] * std::fabs(lz);
        if (tmin > r || tmax < -r) {
            return false; // 分離軸あり
        }
        // 貫通深度 = 分離に要する移動量 (区間の交差長ではない — 平坦な三角形の射影は
        // 幅 0 になるため min(正方向, 負方向) の押し出し量を取る)
        const float overlap = ((tmax + r) < (r - tmin)) ? (tmax + r) : (r - tmin);
        const float invLen = 1.0f / std::sqrt(len2);
        const float d = overlap * invLen; // ワールド単位の重なり
        if (d < bestDepth) {              // strict < = 先の軸 (固定順) が勝つ
            bestDepth = d;
            bestAxis[0] = lx * invLen;
            bestAxis[1] = ly * invLen;
            bestAxis[2] = lz * invLen;
            found = true;
        }
        return true;
    };

    // 1) ボックス 3 軸 → 2) 三角形法線 → 3) 9 クロス軸 (固定順)
    if (!testAxis(1, 0, 0) || !testAxis(0, 1, 0) || !testAxis(0, 0, 1)) {
        return false;
    }
    if (!testAxis(f[0][1] * f[1][2] - f[0][2] * f[1][1], f[0][2] * f[1][0] - f[0][0] * f[1][2],
                  f[0][0] * f[1][1] - f[0][1] * f[1][0])) {
        return false;
    }
    for (int j = 0; j < 3; ++j) { // a0=(1,0,0) × f_j = (0, -fz, fy)
        if (!testAxis(0.0f, -f[j][2], f[j][1])) {
            return false;
        }
    }
    for (int j = 0; j < 3; ++j) { // a1=(0,1,0) × f_j = (fz, 0, -fx)
        if (!testAxis(f[j][2], 0.0f, -f[j][0])) {
            return false;
        }
    }
    for (int j = 0; j < 3; ++j) { // a2=(0,0,1) × f_j = (-fy, fx, 0)
        if (!testAxis(-f[j][1], f[j][0], 0.0f)) {
            return false;
        }
    }
    if (!found) {
        return false;
    }
    // 法線の向き: 三角形→ボックス (ボックス中心 = ローカル原点)。三角形重心が軸の正側なら反転
    const float gx = (v[0][0] + v[1][0] + v[2][0]) / 3.0f;
    const float gy = (v[0][1] + v[1][1] + v[2][1]) / 3.0f;
    const float gz = (v[0][2] + v[1][2] + v[2][2]) / 3.0f;
    float lx = bestAxis[0], ly = bestAxis[1], lz = bestAxis[2];
    if (gx * lx + gy * ly + gz * lz > 0.0f) {
        lx = -lx; ly = -ly; lz = -lz;
    }
    LocalToWorld(box, lx, ly, lz, nx, ny, nz);
    depth = bestDepth;
    return true;
}

// ボックス vs 三角形マニフォールド: 三角形をボックス 6 スラブでクリップ → 最大 4 点。
// 各点の depth は SAT 深度 (一様近似 — 静的メッシュ相手の安定接地には十分)
bool BoxTriManifold(const ShapePose& box, float ax, float ay, float az, float bx, float by,
                    float bz, float cx, float cy, float cz, Manifold& out)
{
    float nx, ny, nz, depth;
    if (!BoxTriSat(box, ax, ay, az, bx, by, bz, cx, cy, cz, nx, ny, nz, depth)) {
        return false;
    }
    // ローカル頂点で Sutherland–Hodgman クリップ (6 平面固定順)
    float polyA[16][3], polyB[16][3];
    WorldToLocal(box, ax - box.px, ay - box.py, az - box.pz, polyA[0][0], polyA[0][1],
                 polyA[0][2]);
    WorldToLocal(box, bx - box.px, by - box.py, bz - box.pz, polyA[1][0], polyA[1][1],
                 polyA[1][2]);
    WorldToLocal(box, cx - box.px, cy - box.py, cz - box.pz, polyA[2][0], polyA[2][1],
                 polyA[2][2]);
    int countA = 3;
    float (*src)[3] = polyA;
    float (*dst)[3] = polyB;
    const float h[3] = { box.hx, box.hy, box.hz };
    for (int plane = 0; plane < 6 && countA > 0; ++plane) {
        const int axis = plane / 2;
        const float sign = (plane % 2 == 0) ? 1.0f : -1.0f; // +axis <= h / -axis <= h
        int countB = 0;
        for (int i = 0; i < countA; ++i) {
            const int j = (i + 1) % countA;
            const float di = h[axis] - sign * src[i][axis]; // >=0 = 内側
            const float dj = h[axis] - sign * src[j][axis];
            if (di >= 0.0f && countB < 16) {
                dst[countB][0] = src[i][0];
                dst[countB][1] = src[i][1];
                dst[countB][2] = src[i][2];
                ++countB;
            }
            if ((di >= 0.0f) != (dj >= 0.0f) && countB < 16) {
                const float t = di / (di - dj);
                dst[countB][0] = src[i][0] + (src[j][0] - src[i][0]) * t;
                dst[countB][1] = src[i][1] + (src[j][1] - src[i][1]) * t;
                dst[countB][2] = src[i][2] + (src[j][2] - src[i][2]) * t;
                ++countB;
            }
        }
        float (*tmp)[3] = src;
        src = dst;
        dst = tmp;
        countA = countB;
    }

    out.nx = nx; out.ny = ny; out.nz = nz;
    out.count = 0;
    for (int i = 0; i < countA && out.count < 4; ++i) {
        float wx, wy, wz;
        LocalToWorld(box, src[i][0], src[i][1], src[i][2], wx, wy, wz);
        out.pts[out.count].px = box.px + wx;
        out.pts[out.count].py = box.py + wy;
        out.pts[out.count].pz = box.pz + wz;
        out.pts[out.count].depth = depth;
        ++out.count;
    }
    if (out.count == 0) { // クリップが空 (接触ぎりぎり) → 最近点 1 点
        float qx, qy, qz;
        ClosestPtPointTri(box.px, box.py, box.pz, ax, ay, az, bx, by, bz, cx, cy, cz, qx, qy, qz);
        out.pts[0] = { qx, qy, qz, depth };
        out.count = 1;
    }
    return true;
}

// レイ vs 三角形 (Möller–Trumbore、両面ヒット)。normal はレイ進行方向と逆側の面法線
bool RayTri(float ox, float oy, float oz, float dx, float dy, float dz, float ax, float ay,
            float az, float bx, float by, float bz, float cx, float cy, float cz, float maxDist,
            float& outT, float& nx, float& ny, float& nz)
{
    const float e1x = bx - ax, e1y = by - ay, e1z = bz - az;
    const float e2x = cx - ax, e2y = cy - ay, e2z = cz - az;
    const float px = dy * e2z - dz * e2y;
    const float py = dz * e2x - dx * e2z;
    const float pz = dx * e2y - dy * e2x;
    const float det = e1x * px + e1y * py + e1z * pz;
    if (std::fabs(det) < 1e-10f) {
        return false; // 平行
    }
    const float inv = 1.0f / det;
    const float tx = ox - ax, ty = oy - ay, tz = oz - az;
    const float u = (tx * px + ty * py + tz * pz) * inv;
    if (u < 0.0f || u > 1.0f) {
        return false;
    }
    const float qx = ty * e1z - tz * e1y;
    const float qy = tz * e1x - tx * e1z;
    const float qz = tx * e1y - ty * e1x;
    const float vv = (dx * qx + dy * qy + dz * qz) * inv;
    if (vv < 0.0f || u + vv > 1.0f) {
        return false;
    }
    const float t = (e2x * qx + e2y * qy + e2z * qz) * inv;
    if (t < 0.0f || t > maxDist) {
        return false;
    }
    outT = t;
    TriFaceNormal(ax, ay, az, bx, by, bz, cx, cy, cz, nx, ny, nz);
    if (nx * dx + ny * dy + nz * dz > 0.0f) { // 両面: レイと逆向きに揃える
        nx = -nx; ny = -ny; nz = -nz;
    }
    return true;
}

// 地形表面上の最近点 (M59i)。**その点の真下のセルとその周り 1 枚**だけを見る。
// ハイトフィールドは高さ方向に一価なので、点の XZ を含むセル近傍に必ず最近点がある
// (メッシュのように「裏側の面のほうが近い」が起きない)。地形の外の点は縁に丸まる
float TerrainClosestPoint(const ShapePose& s, float px, float py, float pz, float& qx, float& qy,
                          float& qz)
{
    const TerrainCollisionData* t = TerrainOf(s);
    if (!TerrainUsable(t)) {
        return 3.4e38f;
    }
    const TerrainAsset::TerrainData& d = t->data;
    float lx, ly, lz;
    WorldToLocal(s, px - s.px, py - s.py, pz - s.pz, lx, ly, lz);
    lx /= std::max(s.sx, 1e-6f);
    lz /= std::max(s.sz, 1e-6f);
    const int32_t tilesX = static_cast<int32_t>(d.heightW) - 1;
    const float stepX = d.worldSizeX / static_cast<float>(tilesX);
    const float stepZ = d.worldSizeZ / static_cast<float>(static_cast<int32_t>(d.heightH) - 1);
    int32_t ix0, iz0, ix1, iz1;
    if (!TerrainCellRange(d, lx - stepX, lz - stepZ, lx + stepX, lz + stepZ, ix0, iz0, ix1, iz1)) {
        return 3.4e38f;
    }
    float best = 3.4e38f;
    for (int32_t iz = iz0; iz <= iz1; ++iz) {
        for (int32_t ix = ix0; ix <= ix1; ++ix) {
            for (int32_t half = 0; half < 2; ++half) {
                const int32_t tri = (iz * tilesX + ix) * 2 + half;
                float ax, ay, az, bx, by, bz, cx, cy, cz;
                TerrainWorldTri(s, *t, tri, ax, ay, az, bx, by, bz, cx, cy, cz);
                float tqx, tqy, tqz;
                ClosestPtPointTri(px, py, pz, ax, ay, az, bx, by, bz, cx, cy, cz, tqx, tqy, tqz);
                const float ddx = px - tqx, ddy = py - tqy, ddz = pz - tqz;
                const float dist = std::sqrt(ddx * ddx + ddy * ddy + ddz * ddz);
                if (dist < best) { // strict < = 同距離は小さい三角形番号が勝つ
                    best = dist;
                    qx = tqx; qy = tqy; qz = tqz;
                }
            }
        }
    }
    return best;
}

// メッシュ表面上の最近点 (固定順 DFS + ノード AABB 下限で枝刈り)。
// 戻り値 = ワールド距離 (メッシュ無し = 3.4e38f)。最小距離は走査順に依らず一意、
// 等距離タイは固定走査順の先勝ち = 決定論
float MeshClosestPoint(const ShapePose& mesh, float px, float py, float pz, float& qx, float& qy,
                       float& qz)
{
    const MeshColliderData* md = MeshOf(mesh);
    if (!md || md->nodes.empty()) {
        return 3.4e38f;
    }
    float lx, ly, lz;
    WorldToLocal(mesh, px - mesh.px, py - mesh.py, pz - mesh.pz, lx, ly, lz);
    const float lpx = lx / std::max(mesh.sx, 1e-6f);
    const float lpy = ly / std::max(mesh.sy, 1e-6f);
    const float lpz = lz / std::max(mesh.sz, 1e-6f);
    const float minScale = std::min(mesh.sx, std::min(mesh.sy, mesh.sz));
    float best = 3.4e38f;
    int32_t stack[64];
    int top = 0;
    stack[top++] = 0;
    while (top > 0) {
        const MeshBvhNode& node = md->nodes[stack[--top]];
        float ddx = 0.0f, ddy = 0.0f, ddz = 0.0f;
        if (lpx < node.minX) { ddx = node.minX - lpx; } else if (lpx > node.maxX) { ddx = lpx - node.maxX; }
        if (lpy < node.minY) { ddy = node.minY - lpy; } else if (lpy > node.maxY) { ddy = lpy - node.maxY; }
        if (lpz < node.minZ) { ddz = node.minZ - lpz; } else if (lpz > node.maxZ) { ddz = lpz - node.maxZ; }
        const float lower = std::sqrt(ddx * ddx + ddy * ddy + ddz * ddz) * minScale;
        if (lower >= best) {
            continue; // 枝刈り (結果には影響しない — 下限が現ベスト以上)
        }
        if (node.left < 0) {
            for (int i = 0; i < node.triCount; ++i) {
                const int32_t tri = md->triOrder[node.triStart + i];
                float ax, ay, az, bx, by, bz, cx, cy, cz;
                MeshWorldTri(mesh, *md, tri, ax, ay, az, bx, by, bz, cx, cy, cz);
                float tqx, tqy, tqz;
                ClosestPtPointTri(px, py, pz, ax, ay, az, bx, by, bz, cx, cy, cz, tqx, tqy, tqz);
                const float dx = px - tqx, dy = py - tqy, dz = pz - tqz;
                const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (d < best) {
                    best = d;
                    qx = tqx; qy = tqy; qz = tqz;
                }
            }
            continue;
        }
        if (top + 2 <= 64) {
            stack[top++] = node.right;
            stack[top++] = node.left;
        }
    }
    return best;
}

// メッシュ vs 他形状 (sphere/box/capsule)。normal は **メッシュ→他形状**、最深三角形を採用
bool CollideMeshOther(const ShapePose& mesh, const ShapePose& other, float& nx, float& ny,
                      float& nz, float& depth)
{
    int32_t tris[kMeshMaxCandidates];
    const int n = SoupGatherTris(mesh, other, 0.01f, tris, kMeshMaxCandidates);
    if (n == 0 || !SoupUsable(mesh)) {
        return false;
    }
    bool hit = false;
    float bestDepth = -1.0f;
    for (int i = 0; i < n; ++i) {
        float ax, ay, az, bx, by, bz, cx, cy, cz;
        SoupWorldTri(mesh, tris[i], ax, ay, az, bx, by, bz, cx, cy, cz);
        float tnx = 0, tny = 1, tnz = 0, td = 0, qx = 0, qy = 0, qz = 0;
        bool triHit = false;
        if (other.shape == 0) {
            triHit = SphereTriContact(other.px, other.py, other.pz, other.radius, ax, ay, az, bx,
                                      by, bz, cx, cy, cz, tnx, tny, tnz, td, qx, qy, qz);
        } else if (other.shape == 1) {
            triHit = BoxTriSat(other, ax, ay, az, bx, by, bz, cx, cy, cz, tnx, tny, tnz, td);
        } else if (other.shape == 2) {
            triHit = CapsuleTriContact(other, ax, ay, az, bx, by, bz, cx, cy, cz, tnx, tny, tnz,
                                       td, qx, qy, qz);
        }
        if (triHit && td > bestDepth) { // strict > = 同深度は小さい三角形番号が勝つ
            bestDepth = td;
            nx = tnx; ny = tny; nz = tnz;
            hit = true;
        }
    }
    depth = bestDepth;
    return hit;
}

// メッシュ vs 他形状のマニフォールド。最深三角形のマニフォールド (normal = メッシュ→他形状)
bool MeshOtherManifold(const ShapePose& mesh, const ShapePose& other, Manifold& out)
{
    int32_t tris[kMeshMaxCandidates];
    const int n = SoupGatherTris(mesh, other, 0.01f, tris, kMeshMaxCandidates);
    if (n == 0 || !SoupUsable(mesh)) {
        return false;
    }
    bool hit = false;
    float bestDepth = -1.0f;
    int32_t bestTri = -1;
    for (int i = 0; i < n; ++i) {
        float ax, ay, az, bx, by, bz, cx, cy, cz;
        SoupWorldTri(mesh, tris[i], ax, ay, az, bx, by, bz, cx, cy, cz);
        float tnx = 0, tny = 1, tnz = 0, td = 0, qx = 0, qy = 0, qz = 0;
        bool triHit = false;
        if (other.shape == 0) {
            triHit = SphereTriContact(other.px, other.py, other.pz, other.radius, ax, ay, az, bx,
                                      by, bz, cx, cy, cz, tnx, tny, tnz, td, qx, qy, qz);
        } else if (other.shape == 1) {
            triHit = BoxTriSat(other, ax, ay, az, bx, by, bz, cx, cy, cz, tnx, tny, tnz, td);
        } else if (other.shape == 2) {
            triHit = CapsuleTriContact(other, ax, ay, az, bx, by, bz, cx, cy, cz, tnx, tny, tnz,
                                       td, qx, qy, qz);
        }
        if (triHit && td > bestDepth) {
            bestDepth = td;
            bestTri = tris[i];
            hit = true;
        }
    }
    if (!hit) {
        return false;
    }
    float ax, ay, az, bx, by, bz, cx, cy, cz;
    SoupWorldTri(mesh, bestTri, ax, ay, az, bx, by, bz, cx, cy, cz);
    if (other.shape == 1) {
        return BoxTriManifold(other, ax, ay, az, bx, by, bz, cx, cy, cz, out);
    }
    float tnx = 0, tny = 1, tnz = 0, td = 0, qx = 0, qy = 0, qz = 0;
    bool triHit = false;
    if (other.shape == 0) {
        triHit = SphereTriContact(other.px, other.py, other.pz, other.radius, ax, ay, az, bx, by,
                                  bz, cx, cy, cz, tnx, tny, tnz, td, qx, qy, qz);
    } else {
        triHit = CapsuleTriContact(other, ax, ay, az, bx, by, bz, cx, cy, cz, tnx, tny, tnz, td,
                                   qx, qy, qz);
    }
    if (!triHit) {
        return false;
    }
    out.nx = tnx; out.ny = tny; out.nz = tnz;
    out.pts[0] = { qx, qy, qz, td };
    out.count = 1;
    return true;
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
    // ---- 三角形スープ (M41 メッシュ / M59i 地形) vs sphere/box/capsule。スープ同士は解かない ----
    if (IsSoup(a) || IsSoup(b)) {
        if (IsSoup(a) && IsSoup(b)) {
            return false;
        }
        const ShapePose& m = IsSoup(a) ? a : b;
        const ShapePose& o = IsSoup(a) ? b : a;
        float mnx, mny, mnz, md;
        if (!CollideMeshOther(m, o, mnx, mny, mnz, md)) {
            return false;
        }
        // 規約: normal は b→a。CollideMeshOther は スープ→other を返す
        if (IsSoup(a)) {
            nx = -mnx; ny = -mny; nz = -mnz;
        } else {
            nx = mnx; ny = mny; nz = mnz;
        }
        depth = md;
        return true;
    }
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

bool CollideManifold(const ShapePose& a, const ShapePose& b, Manifold& out)
{
    out.count = 0;
    const int32_t sa = a.shape, sb = b.shape;
    float nx, ny, nz;
    Contact c0;

    // ---- 静的メッシュ (M41)。normal 規約は Collide と同じ (b→a) ----
    if (IsSoup(a) || IsSoup(b)) {
        if (IsSoup(a) && IsSoup(b)) {
            return false;
        }
        const ShapePose& m = IsSoup(a) ? a : b;
        const ShapePose& o = IsSoup(a) ? b : a;
        if (!MeshOtherManifold(m, o, out)) {
            return false;
        }
        if (IsSoup(a)) { // MeshOtherManifold は スープ→other を返す
            out.nx = -out.nx;
            out.ny = -out.ny;
            out.nz = -out.nz;
        }
        return true;
    }

    if (sa == 0 && sb == 0) {
        if (!SpherePairContact(a.px, a.py, a.pz, b.px, b.py, b.pz, a.radius, b.radius, nx, ny, nz,
                               c0)) {
            return false;
        }
        out.nx = nx; out.ny = ny; out.nz = nz;
        out.pts[0] = c0;
        out.count = 1;
        return true;
    }
    if (sa == 1 && sb == 1) {
        int axisId;
        float depth;
        if (!BoxBoxSat(a, b, nx, ny, nz, depth, axisId)) {
            return false;
        }
        out.nx = nx; out.ny = ny; out.nz = nz;
        if (axisId < 6) {
            BoxBoxFaceManifold(a, b, axisId < 3, nx, ny, nz, depth, out);
        } else {
            BoxBoxEdgeManifold(a, b, (axisId - 6) / 3, (axisId - 6) % 3, nx, ny, nz, depth, out);
        }
        return out.count > 0;
    }
    if (sa == 2 && sb == 2) {
        return CapsuleCapsuleManifold(a, b, out);
    }
    if (sa == 0 && sb == 1) {
        if (!SphereBoxContact(a.px, a.py, a.pz, a.radius, b, nx, ny, nz, c0)) {
            return false;
        }
        out.nx = nx; out.ny = ny; out.nz = nz; // box→sphere = b→a
        out.pts[0] = c0;
        out.count = 1;
        return true;
    }
    if (sa == 1 && sb == 0) {
        if (!SphereBoxContact(b.px, b.py, b.pz, b.radius, a, nx, ny, nz, c0)) {
            return false;
        }
        out.nx = -nx; out.ny = -ny; out.nz = -nz;
        out.pts[0] = c0;
        out.count = 1;
        return true;
    }
    if (sa == 0 && sb == 2) {
        const float t = std::clamp(Dot3(b.by, a.px - b.px, a.py - b.py, a.pz - b.pz), -b.halfSeg,
                                   b.halfSeg);
        const float qx = b.px + b.by[0] * t, qy = b.py + b.by[1] * t, qz = b.pz + b.by[2] * t;
        if (!SpherePairContact(a.px, a.py, a.pz, qx, qy, qz, a.radius, b.radius, nx, ny, nz, c0)) {
            return false;
        }
        out.nx = nx; out.ny = ny; out.nz = nz;
        out.pts[0] = c0;
        out.count = 1;
        return true;
    }
    if (sa == 2 && sb == 0) {
        const float t = std::clamp(Dot3(a.by, b.px - a.px, b.py - a.py, b.pz - a.pz), -a.halfSeg,
                                   a.halfSeg);
        const float qx = a.px + a.by[0] * t, qy = a.py + a.by[1] * t, qz = a.pz + a.by[2] * t;
        if (!SpherePairContact(b.px, b.py, b.pz, qx, qy, qz, b.radius, a.radius, nx, ny, nz, c0)) {
            return false;
        }
        out.nx = -nx; out.ny = -ny; out.nz = -nz;
        out.pts[0] = c0;
        out.count = 1;
        return true;
    }
    if (sa == 2 && sb == 1) {
        return CapsuleBoxManifold(a, b, out); // n = box→capsule = b→a
    }
    if (sa == 1 && sb == 2) {
        if (!CapsuleBoxManifold(b, a, out)) {
            return false;
        }
        out.nx = -out.nx; out.ny = -out.ny; out.nz = -out.nz;
        return true;
    }
    return false; // 未知の shape 値は判定しない
}

float DistanceToShape(const ShapePose& s, float px, float py, float pz)
{
    // 静的メッシュ (M41): 表面 (三角形群) までの距離。メッシュ無しは「無限遠」=
    // SphereCast の保守的前進が自由に進める (障害物として扱わない)
    if (s.shape == 3) {
        float qx, qy, qz;
        return MeshClosestPoint(s, px, py, pz, qx, qy, qz);
    }
    if (s.shape == 4) { // M59i: 地形も同じ意味 (表面までの距離)
        float qx, qy, qz;
        return TerrainClosestPoint(s, px, py, pz, qx, qy, qz);
    }
    if (s.shape == 0) {
        const float dx = px - s.px, dy = py - s.py, dz = pz - s.pz;
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz) - s.radius;
        return (d > 0.0f) ? d : 0.0f;
    }
    if (s.shape == 1) {
        float lx, ly, lz;
        WorldToLocal(s, px - s.px, py - s.py, pz - s.pz, lx, ly, lz);
        float ex = std::fabs(lx) - s.hx;
        float ey = std::fabs(ly) - s.hy;
        float ez = std::fabs(lz) - s.hz;
        if (ex < 0) { ex = 0; }
        if (ey < 0) { ey = 0; }
        if (ez < 0) { ez = 0; }
        return std::sqrt(ex * ex + ey * ey + ez * ez);
    }
    // capsule: 線分までの距離 − 半径
    const float t = std::clamp(Dot3(s.by, px - s.px, py - s.py, pz - s.pz), -s.halfSeg,
                               s.halfSeg);
    const float qx = s.px + s.by[0] * t, qy = s.py + s.by[1] * t, qz = s.pz + s.by[2] * t;
    const float dx = px - qx, dy = py - qy, dz = pz - qz;
    const float d = std::sqrt(dx * dx + dy * dy + dz * dz) - s.radius;
    return (d > 0.0f) ? d : 0.0f;
}

void ClosestPointOnShape(const ShapePose& s, float px, float py, float pz, float& qx, float& qy,
                         float& qz)
{
    // 静的メッシュ (M41): 三角形群上の最近点。メッシュ無しはその点自身
    if (s.shape == 3 || s.shape == 4) {
        qx = px; qy = py; qz = pz;
        float mqx, mqy, mqz;
        const float d = (s.shape == 3) ? MeshClosestPoint(s, px, py, pz, mqx, mqy, mqz)
                                       : TerrainClosestPoint(s, px, py, pz, mqx, mqy, mqz);
        if (d < 3.4e38f) {
            qx = mqx; qy = mqy; qz = mqz;
        }
        return;
    }
    if (s.shape == 1) {
        float lx, ly, lz;
        WorldToLocal(s, px - s.px, py - s.py, pz - s.pz, lx, ly, lz);
        const float cx = std::clamp(lx, -s.hx, s.hx);
        const float cy = std::clamp(ly, -s.hy, s.hy);
        const float cz = std::clamp(lz, -s.hz, s.hz);
        float wx, wy, wz;
        LocalToWorld(s, cx, cy, cz, wx, wy, wz);
        qx = s.px + wx;
        qy = s.py + wy;
        qz = s.pz + wz;
        return;
    }
    // sphere / capsule: 中心 (または軸最近点) から半径方向へ
    float cx = s.px, cy = s.py, cz = s.pz;
    if (s.shape == 2) {
        const float t = std::clamp(Dot3(s.by, px - s.px, py - s.py, pz - s.pz), -s.halfSeg,
                                   s.halfSeg);
        cx = s.px + s.by[0] * t;
        cy = s.py + s.by[1] * t;
        cz = s.pz + s.by[2] * t;
    }
    const float dx = px - cx, dy = py - cy, dz = pz - cz;
    const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (d <= s.radius || d < 1e-8f) {
        qx = px; qy = py; qz = pz; // 内部 (または中心一致) はその点自身
        return;
    }
    const float k = s.radius / d;
    qx = cx + dx * k;
    qy = cy + dy * k;
    qz = cz + dz * k;
}

void ComputeAabb(const ShapePose& s, float& minX, float& minY, float& minZ, float& maxX,
                 float& maxY, float& maxZ)
{
    // 地形 (M59i): ローカル AABB は「幅 x 高さ範囲 x 奥行」。高さ範囲はロード時に 1 回
    // 測ってある (毎フレーム O(W*H) で走査し直さないための TerrainCollisionData)
    if (s.shape == 4) {
        const TerrainCollisionData* t = static_cast<const TerrainCollisionData*>(s.meshData);
        if (!TerrainUsable(t)) {
            minX = maxX = s.px;
            minY = maxY = s.py;
            minZ = maxZ = s.pz;
            return;
        }
        const float lcy = (t->minHeight + t->maxHeight) * 0.5f * s.sy;
        const float lex = t->data.worldSizeX * 0.5f * s.sx;
        const float ley = (t->maxHeight - t->minHeight) * 0.5f * s.sy;
        const float lez = t->data.worldSizeZ * 0.5f * s.sz;
        const float wcx = s.px + s.by[0] * lcy;
        const float wcy = s.py + s.by[1] * lcy;
        const float wcz = s.pz + s.by[2] * lcy;
        const float wex = std::fabs(s.bx[0]) * lex + std::fabs(s.by[0]) * ley + std::fabs(s.bz[0]) * lez;
        const float wey = std::fabs(s.bx[1]) * lex + std::fabs(s.by[1]) * ley + std::fabs(s.bz[1]) * lez;
        const float wez = std::fabs(s.bx[2]) * lex + std::fabs(s.by[2]) * ley + std::fabs(s.bz[2]) * lez;
        minX = wcx - wex; maxX = wcx + wex;
        minY = wcy - wey; maxY = wcy + wey;
        minZ = wcz - wez; maxZ = wcz + wez;
        return;
    }
    // 静的メッシュ (M41): BVH ルートのローカル AABB をワールドへ (スケール → |基底| 変換)
    if (s.shape == 3) {
        const MeshColliderData* md = static_cast<const MeshColliderData*>(s.meshData);
        if (!md || md->nodes.empty()) {
            minX = maxX = s.px;
            minY = maxY = s.py;
            minZ = maxZ = s.pz;
            return;
        }
        const MeshBvhNode& root = md->nodes[0];
        const float lcx = (root.minX + root.maxX) * 0.5f * s.sx;
        const float lcy = (root.minY + root.maxY) * 0.5f * s.sy;
        const float lcz = (root.minZ + root.maxZ) * 0.5f * s.sz;
        const float lex = (root.maxX - root.minX) * 0.5f * s.sx;
        const float ley = (root.maxY - root.minY) * 0.5f * s.sy;
        const float lez = (root.maxZ - root.minZ) * 0.5f * s.sz;
        const float wcx = s.px + s.bx[0] * lcx + s.by[0] * lcy + s.bz[0] * lcz;
        const float wcy = s.py + s.bx[1] * lcx + s.by[1] * lcy + s.bz[1] * lcz;
        const float wcz = s.pz + s.bx[2] * lcx + s.by[2] * lcy + s.bz[2] * lcz;
        const float wex = std::fabs(s.bx[0]) * lex + std::fabs(s.by[0]) * ley + std::fabs(s.bz[0]) * lez;
        const float wey = std::fabs(s.bx[1]) * lex + std::fabs(s.by[1]) * ley + std::fabs(s.bz[1]) * lez;
        const float wez = std::fabs(s.bx[2]) * lex + std::fabs(s.by[2]) * ley + std::fabs(s.bz[2]) * lez;
        minX = wcx - wex;
        minY = wcy - wey;
        minZ = wcz - wez;
        maxX = wcx + wex;
        maxY = wcy + wey;
        maxZ = wcz + wez;
        return;
    }
    float ex, ey, ez; // 中心からの半径 (各ワールド軸)
    if (s.shape == 1) {
        // box: 各ワールド軸への投影半径 = Σ |基底成分|·half
        ex = std::fabs(s.bx[0]) * s.hx + std::fabs(s.by[0]) * s.hy + std::fabs(s.bz[0]) * s.hz;
        ey = std::fabs(s.bx[1]) * s.hx + std::fabs(s.by[1]) * s.hy + std::fabs(s.bz[1]) * s.hz;
        ez = std::fabs(s.bx[2]) * s.hx + std::fabs(s.by[2]) * s.hy + std::fabs(s.bz[2]) * s.hz;
    } else if (s.shape == 2) {
        // capsule: 軸方向の線分半長 + 半径
        ex = std::fabs(s.by[0]) * s.halfSeg + s.radius;
        ey = std::fabs(s.by[1]) * s.halfSeg + s.radius;
        ez = std::fabs(s.by[2]) * s.halfSeg + s.radius;
    } else {
        ex = ey = ez = s.radius;
    }
    minX = s.px - ex;
    minY = s.py - ey;
    minZ = s.pz - ez;
    maxX = s.px + ex;
    maxY = s.py + ey;
    maxZ = s.pz + ez;
}

// 地形へのレイキャスト (M59i)。**XZ 平面のセルを DDA で辿る** — AABB 一括収集にすると
// 斜めに長いレイが矩形全体を候補にしてしまい、上限で静かに取りこぼす
// (メッシュは BVH があるので同じ形でも困らない)。
// ★方向ベクトルは正規化し直さない: ローカル変換は原点も方向も同じスケールで割るので
//   パラメータ t (= ワールド距離) がそのまま通る
bool RayTerrain(const ShapePose& s, float ox, float oy, float oz, float dx, float dy, float dz,
                float maxDist, float& outT, float& nx, float& ny, float& nz)
{
    const TerrainCollisionData* t = TerrainOf(s);
    if (!TerrainUsable(t)) {
        return false;
    }
    const TerrainAsset::TerrainData& d = t->data;
    const float isx = 1.0f / std::max(s.sx, 1e-6f);
    const float isz = 1.0f / std::max(s.sz, 1e-6f);
    float lox, loy, loz, ldx, ldy, ldz;
    WorldToLocal(s, ox - s.px, oy - s.py, oz - s.pz, lox, loy, loz);
    WorldToLocal(s, dx, dy, dz, ldx, ldy, ldz);
    lox *= isx; loz *= isz;
    ldx *= isx; ldz *= isz;
    (void)loy;
    (void)ldy;
    const int32_t tilesX = static_cast<int32_t>(d.heightW) - 1;
    const int32_t tilesZ = static_cast<int32_t>(d.heightH) - 1;
    const float stepX = d.worldSizeX / static_cast<float>(tilesX);
    const float stepZ = d.worldSizeZ / static_cast<float>(tilesZ);
    // セル座標 (連続値)。cx in [0, tilesX] が地形の範囲
    float cx = (lox + d.worldSizeX * 0.5f) / stepX;
    float cz = (loz + d.worldSizeZ * 0.5f) / stepZ;
    const float vx = ldx / stepX; // セル / 単位 t
    const float vz = ldz / stepZ;
    // 格子の外から入ってくるレイは入口まで t を進める (スラブ法、XZ の 2 軸だけ)
    float tEnter = 0.0f, tExit = maxDist;
    for (int axis = 0; axis < 2; ++axis) {
        const float o0 = (axis == 0) ? cx : cz;
        const float v0 = (axis == 0) ? vx : vz;
        const float hi = static_cast<float>((axis == 0) ? tilesX : tilesZ);
        if (std::fabs(v0) < 1e-12f) {
            if (o0 < 0.0f || o0 > hi) {
                return false; // 平行かつ外 = 交わらない
            }
            continue;
        }
        float t0 = (0.0f - o0) / v0;
        float t1 = (hi - o0) / v0;
        if (t0 > t1) {
            const float tmp = t0;
            t0 = t1;
            t1 = tmp;
        }
        if (t0 > tEnter) { tEnter = t0; }
        if (t1 < tExit) { tExit = t1; }
    }
    if (tEnter > tExit || tExit < 0.0f) {
        return false;
    }
    if (tEnter < 0.0f) { tEnter = 0.0f; }
    cx += vx * tEnter;
    cz += vz * tEnter;
    int32_t ix = static_cast<int32_t>(std::floor(cx));
    int32_t iz = static_cast<int32_t>(std::floor(cz));
    if (ix < 0) { ix = 0; } else if (ix > tilesX - 1) { ix = tilesX - 1; }
    if (iz < 0) { iz = 0; } else if (iz > tilesZ - 1) { iz = tilesZ - 1; }
    const int32_t stepIX = (vx > 0.0f) ? 1 : ((vx < 0.0f) ? -1 : 0);
    const int32_t stepIZ = (vz > 0.0f) ? 1 : ((vz < 0.0f) ? -1 : 0);
    const float kBig = 3.4e38f;
    float tMaxX = kBig, tMaxZ = kBig, tDeltaX = kBig, tDeltaZ = kBig;
    if (stepIX != 0) {
        const float next = (stepIX > 0) ? static_cast<float>(ix + 1) : static_cast<float>(ix);
        tMaxX = tEnter + (next - cx) / vx;
        tDeltaX = std::fabs(1.0f / vx);
    }
    if (stepIZ != 0) {
        const float next = (stepIZ > 0) ? static_cast<float>(iz + 1) : static_cast<float>(iz);
        tMaxZ = tEnter + (next - cz) / vz;
        tDeltaZ = std::fabs(1.0f / vz);
    }
    // 訪問セル数の上限 (暴走防止。地形 1 辺の 4 倍あれば対角も足りる)
    const int32_t maxVisit = (tilesX + tilesZ) * 2 + 8;
    bool hit = false;
    for (int32_t visit = 0; visit < maxVisit; ++visit) {
        for (int32_t half = 0; half < 2; ++half) {
            const int32_t tri = (iz * tilesX + ix) * 2 + half;
            float ax, ay, az, bx2, by2, bz2, cx2, cy2, cz2;
            TerrainWorldTri(s, *t, tri, ax, ay, az, bx2, by2, bz2, cx2, cy2, cz2);
            float th, tnx, tny, tnz;
            if (RayTri(ox, oy, oz, dx, dy, dz, ax, ay, az, bx2, by2, bz2, cx2, cy2, cz2, maxDist,
                       th, tnx, tny, tnz)) {
                if (!hit || th < outT) { // strict < = 同距離は小さい三角形番号が勝つ
                    outT = th;
                    nx = tnx; ny = tny; nz = tnz;
                    hit = true;
                }
            }
        }
        // ★見つかっても即 return しない — 三角形はセルの外まで伸びないが、隣接セルの
        //   三角形がより手前で当たることがある (斜面を横切るレイ)。次のセルの入口 t が
        //   すでに現ベストを超えていたら、そこで初めて打ち切ってよい
        const float tNext = (tMaxX < tMaxZ) ? tMaxX : tMaxZ;
        if (tNext > tExit || tNext > maxDist || (hit && tNext > outT)) {
            break;
        }
        if (tMaxX < tMaxZ) {
            ix += stepIX;
            tMaxX += tDeltaX;
        } else {
            iz += stepIZ;
            tMaxZ += tDeltaZ;
        }
        if (ix < 0 || ix > tilesX - 1 || iz < 0 || iz > tilesZ - 1) {
            break;
        }
    }
    return hit;
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
    if (s.shape == 4) { // M59i: 地形は XZ セルの DDA
        return RayTerrain(s, ox, oy, oz, dx, dy, dz, maxDist, outT, nx, ny, nz);
    }
    // 静的メッシュ (M41): 線分 AABB で BVH 候補収集 → 三角形番号昇順に MT 判定、最近 t を採用
    if (s.shape == 3) {
        const MeshColliderData* md = static_cast<const MeshColliderData*>(s.meshData);
        if (!md) {
            return false;
        }
        const float ex2 = ox + dx * maxDist, ey2 = oy + dy * maxDist, ez2 = oz + dz * maxDist;
        const float wminX = (ox < ex2) ? ox : ex2, wmaxX = (ox > ex2) ? ox : ex2;
        const float wminY = (oy < ey2) ? oy : ey2, wmaxY = (oy > ey2) ? oy : ey2;
        const float wminZ = (oz < ez2) ? oz : ez2, wmaxZ = (oz > ez2) ? oz : ez2;
        float lminX, lminY, lminZ, lmaxX, lmaxY, lmaxZ;
        WorldAabbToMeshLocal(s, wminX, wminY, wminZ, wmaxX, wmaxY, wmaxZ, lminX, lminY, lminZ,
                             lmaxX, lmaxY, lmaxZ);
        // レイは接触より広い範囲を掃く可能性があるため候補上限を大きめに取る
        int32_t tris[1024];
        const int n = MeshGatherTris(*md, lminX, lminY, lminZ, lmaxX, lmaxY, lmaxZ, tris, 1024);
        bool hit = false;
        for (int i = 0; i < n; ++i) {
            float ax, ay, az, bx2, by2, bz2, cx, cy, cz;
            MeshWorldTri(s, *md, tris[i], ax, ay, az, bx2, by2, bz2, cx, cy, cz);
            float t, tnx, tny, tnz;
            if (RayTri(ox, oy, oz, dx, dy, dz, ax, ay, az, bx2, by2, bz2, cx, cy, cz, maxDist, t,
                       tnx, tny, tnz)) {
                if (!hit || t < outT) { // strict < = 同距離は小さい三角形番号が勝つ
                    outT = t;
                    nx = tnx; ny = tny; nz = tnz;
                    hit = true;
                }
            }
        }
        return hit;
    }
    return false;
}

} // namespace shapes
} // namespace mye
