#include "Engine/Engine/Physics/ConvexCollision.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace mye {
namespace convex {
namespace {

constexpr float kBig = 3.4e38f;

float Dot(float ax, float ay, float az, float bx, float by, float bz)
{
    return ax * bx + ay * by + az * bz;
}

// ---- 箱と三角形の位相テーブル ----
// 頂点の**座標は使わない** (ワールド頂点は Body 側に直接書く) ので、ここで意味を持つのは
// 面の頂点列と稜線の隣接だけ。巻き順は「外から見て CCW」= Newell 法線が外を向く向きで、
// 単位箱の符号表 (kBoxSign) と対で決めてある
const ConvexHullData& BoxTopology()
{
    static const ConvexHullData k = [] {
        ConvexHullData h;
        h.verts = {
            { -1, -1, -1 }, { 1, -1, -1 }, { 1, 1, -1 }, { -1, 1, -1 },
            { -1, -1, 1 },  { 1, -1, 1 },  { 1, 1, 1 },  { -1, 1, 1 },
        };
        const int32_t loops[6][4] = {
            { 0, 3, 2, 1 }, // -Z
            { 4, 5, 6, 7 }, // +Z
            { 0, 1, 5, 4 }, // -Y
            { 3, 7, 6, 2 }, // +Y
            { 0, 4, 7, 3 }, // -X
            { 1, 2, 6, 5 }, // +X
        };
        for (const auto& l : loops) {
            ConvexFace f;
            f.first = static_cast<int32_t>(h.faceVerts.size());
            f.count = 4;
            h.faces.push_back(f);
            for (int k2 = 0; k2 < 4; ++k2) {
                h.faceVerts.push_back(l[k2]);
            }
        }
        const int32_t edges[12][4] = {
            { 0, 1, 0, 2 }, { 1, 2, 0, 5 }, { 2, 3, 0, 3 }, { 0, 3, 0, 4 },
            { 4, 5, 1, 2 }, { 5, 6, 1, 5 }, { 6, 7, 1, 3 }, { 4, 7, 1, 4 },
            { 1, 5, 2, 5 }, { 0, 4, 2, 4 }, { 2, 6, 3, 5 }, { 3, 7, 3, 4 },
        };
        for (const auto& e : edges) {
            ConvexEdge ce;
            ce.v0 = e[0];
            ce.v1 = e[1];
            ce.f0 = e[2];
            ce.f1 = e[3];
            h.edges.push_back(ce);
        }
        return h;
    }();
    return k;
}

// 三角形は「表裏 2 面の潰れた凸体」。3 本の稜線はどちらも同じ 2 面を共有する
const ConvexHullData& TriangleTopology()
{
    static const ConvexHullData k = [] {
        ConvexHullData h;
        h.verts = { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 0, 1 } };
        ConvexFace f0;
        f0.first = 0;
        f0.count = 3;
        ConvexFace f1;
        f1.first = 3;
        f1.count = 3;
        h.faces = { f0, f1 };
        h.faceVerts = { 0, 1, 2, 0, 2, 1 };
        for (int32_t i = 0; i < 3; ++i) {
            ConvexEdge e;
            e.v0 = i;
            e.v1 = (i + 1) % 3;
            if (e.v0 > e.v1) {
                std::swap(e.v0, e.v1);
            }
            e.f0 = 0;
            e.f1 = 1;
            h.edges.push_back(e);
        }
        return h;
    }();
    return k;
}

const float kBoxSign[8][3] = {
    { -1, -1, -1 }, { 1, -1, -1 }, { 1, 1, -1 }, { -1, 1, -1 },
    { -1, -1, 1 },  { 1, -1, 1 },  { 1, 1, 1 },  { -1, 1, 1 },
};

// ワールド頂点から全面の支持平面を Newell 法で組み直す。
// 保存済みのローカル法線を回さないのは、非一様スケールで法線が保存量でないため
void ComputeFacePlanes(Body& b)
{
    const ConvexHullData& t = *b.topo;
    b.faceCount = std::min(static_cast<int32_t>(t.faces.size()), kConvexMaxFaces);
    for (int32_t fi = 0; fi < b.faceCount; ++fi) {
        const ConvexFace& f = t.faces[static_cast<size_t>(fi)];
        float ax = 0, ay = 0, az = 0, ccx = 0, ccy = 0, ccz = 0;
        for (int32_t k = 0; k < f.count; ++k) {
            const int32_t i0 = t.faceVerts[static_cast<size_t>(f.first + k)];
            const int32_t i1 = t.faceVerts[static_cast<size_t>(f.first + (k + 1) % f.count)];
            const float x0 = b.vx[i0], y0 = b.vy[i0], z0 = b.vz[i0];
            const float x1 = b.vx[i1], y1 = b.vy[i1], z1 = b.vz[i1];
            ax += (y0 - y1) * (z0 + z1);
            ay += (z0 - z1) * (x0 + x1);
            az += (x0 - x1) * (y0 + y1);
            ccx += x0;
            ccy += y0;
            ccz += z0;
        }
        const float len2 = ax * ax + ay * ay + az * az;
        if (len2 < 1e-20f) {
            // 縮退面 (潰れたスケール等)。零法線にしておくと軸テストが自動で飛ばす
            b.fnx[fi] = 0;
            b.fny[fi] = 0;
            b.fnz[fi] = 0;
            b.fd[fi] = 0;
            continue;
        }
        const float inv = 1.0f / std::sqrt(len2);
        b.fnx[fi] = ax * inv;
        b.fny[fi] = ay * inv;
        b.fnz[fi] = az * inv;
        const float invN = 1.0f / static_cast<float>(f.count);
        b.fd[fi] = b.fnx[fi] * ccx * invN + b.fny[fi] * ccy * invN + b.fnz[fi] * ccz * invN;
    }
}

int32_t Support(const Body& b, float dx, float dy, float dz)
{
    int32_t best = 0;
    float bestDot = -kBig;
    for (int32_t i = 0; i < b.vertCount; ++i) {
        const float d = b.vx[i] * dx + b.vy[i] * dy + b.vz[i] * dz;
        if (d > bestDot) { // strict > = 同値は index 小 (走査順非依存)
            bestDot = d;
            best = i;
        }
    }
    return best;
}

// Gauss map による稜線ペアの枝刈り (Gregorius)。2 本の稜線が Minkowski 差の面を
// 作るときだけ軸候補になる。**bxa / dxc が縮退する潰れた凸体 (三角形) では枝刈りしない** —
// 枝刈りは最適化なので、判定できないときは通してしまうほうが常に安全
bool IsMinkowskiFace(float ax, float ay, float az, float bx, float by, float bz, float bxa_x,
                     float bxa_y, float bxa_z, float cx, float cy, float cz, float dx, float dy,
                     float dz, float dxc_x, float dxc_y, float dxc_z)
{
    if (bxa_x * bxa_x + bxa_y * bxa_y + bxa_z * bxa_z < 1e-12f
        || dxc_x * dxc_x + dxc_y * dxc_y + dxc_z * dxc_z < 1e-12f) {
        return true;
    }
    const float cba = Dot(cx, cy, cz, bxa_x, bxa_y, bxa_z);
    const float dba = Dot(dx, dy, dz, bxa_x, bxa_y, bxa_z);
    const float adc = Dot(ax, ay, az, dxc_x, dxc_y, dxc_z);
    const float bdc = Dot(bx, by, bz, dxc_x, dxc_y, dxc_z);
    return cba * dba < 0.0f && adc * bdc < 0.0f && cba * bdc > 0.0f;
}

struct Axis {
    float nx = 0, ny = 1, nz = 0; // b→a
    float sep = -kBig;            // 分離距離 (正 = 離れている)
    int type = -1;                // 0 = a の面 / 1 = b の面 / 2 = 稜線ペア
    int32_t ia = -1, ib = -1;
};

// 分離軸探索。false = 分離している (または軸が 1 本も立たなかった)
bool SatQuery(const Body& a, const Body& b, Axis& out)
{
    out = Axis{};

    // ---- a の面 ----
    for (int32_t fi = 0; fi < a.faceCount; ++fi) {
        const float nx = a.fnx[fi], ny = a.fny[fi], nz = a.fnz[fi];
        if (nx * nx + ny * ny + nz * nz < 0.5f) {
            continue; // 縮退面
        }
        const int32_t s = Support(b, -nx, -ny, -nz);
        const float sep = Dot(nx, ny, nz, b.vx[s], b.vy[s], b.vz[s]) - a.fd[fi];
        if (sep > 0.0f) {
            return false;
        }
        if (sep > out.sep) { // strict > = 同値は先着 (= 面軸が稜線軸に勝つ)
            out.sep = sep;
            out.nx = -nx; // a の面の外向き法線の逆 = a を押し戻す向き
            out.ny = -ny;
            out.nz = -nz;
            out.type = 0;
            out.ia = fi;
        }
    }
    // ---- b の面 ----
    for (int32_t fi = 0; fi < b.faceCount; ++fi) {
        const float nx = b.fnx[fi], ny = b.fny[fi], nz = b.fnz[fi];
        if (nx * nx + ny * ny + nz * nz < 0.5f) {
            continue;
        }
        const int32_t s = Support(a, -nx, -ny, -nz);
        const float sep = Dot(nx, ny, nz, a.vx[s], a.vy[s], a.vz[s]) - b.fd[fi];
        if (sep > 0.0f) {
            return false;
        }
        if (sep > out.sep) {
            out.sep = sep;
            out.nx = nx; // b の面の外向き法線 = a を押し出す向き
            out.ny = ny;
            out.nz = nz;
            out.type = 1;
            out.ib = fi;
        }
    }
    // ---- 稜線ペア ----
    const auto& ea = a.topo->edges;
    const auto& eb = b.topo->edges;
    const int32_t na = std::min(static_cast<int32_t>(ea.size()), kConvexMaxEdges);
    const int32_t nb = std::min(static_cast<int32_t>(eb.size()), kConvexMaxEdges);
    for (int32_t i = 0; i < na; ++i) {
        const ConvexEdge& e1 = ea[static_cast<size_t>(i)];
        if (e1.f0 >= a.faceCount || e1.f1 >= a.faceCount) {
            continue;
        }
        const float p1x = a.vx[e1.v0], p1y = a.vy[e1.v0], p1z = a.vz[e1.v0];
        const float d1x = a.vx[e1.v1] - p1x, d1y = a.vy[e1.v1] - p1y, d1z = a.vz[e1.v1] - p1z;
        const float an0x = a.fnx[e1.f0], an0y = a.fny[e1.f0], an0z = a.fnz[e1.f0];
        const float an1x = a.fnx[e1.f1], an1y = a.fny[e1.f1], an1z = a.fnz[e1.f1];
        // bxa = cross(n1, n0) は稜線方向に平行
        const float bax = an1y * an0z - an1z * an0y;
        const float bay = an1z * an0x - an1x * an0z;
        const float baz = an1x * an0y - an1y * an0x;
        for (int32_t j = 0; j < nb; ++j) {
            const ConvexEdge& e2 = eb[static_cast<size_t>(j)];
            if (e2.f0 >= b.faceCount || e2.f1 >= b.faceCount) {
                continue;
            }
            // c, d は b の面法線を反転したもの (Minkowski 差の Gauss map)
            const float cnx = -b.fnx[e2.f0], cny = -b.fny[e2.f0], cnz = -b.fnz[e2.f0];
            const float dnx = -b.fnx[e2.f1], dny = -b.fny[e2.f1], dnz = -b.fnz[e2.f1];
            const float dcx = dny * cnz - dnz * cny;
            const float dcy = dnz * cnx - dnx * cnz;
            const float dcz = dnx * cny - dny * cnx;
            if (!IsMinkowskiFace(an0x, an0y, an0z, an1x, an1y, an1z, bax, bay, baz, cnx, cny, cnz,
                                 dnx, dny, dnz, dcx, dcy, dcz)) {
                continue;
            }
            const float p2x = b.vx[e2.v0], p2y = b.vy[e2.v0], p2z = b.vz[e2.v0];
            const float d2x = b.vx[e2.v1] - p2x, d2y = b.vy[e2.v1] - p2y, d2z = b.vz[e2.v1] - p2z;
            float axx = d1y * d2z - d1z * d2y;
            float axy = d1z * d2x - d1x * d2z;
            float axz = d1x * d2y - d1y * d2x;
            const float len2 = axx * axx + axy * axy + axz * axz;
            if (len2 < 1e-12f) {
                continue; // 平行な稜線 (面軸が拾う)
            }
            const float inv = 1.0f / std::sqrt(len2);
            axx *= inv;
            axy *= inv;
            axz *= inv;
            // ★分離量は**投影区間**で測る。稜線ペアの定番は「dot(axis, p2 - p1)」だが、
            //   あれは軸が Minkowski 差の面法線であることが前提の式で、Gauss map の
            //   枝刈りを外したとき (= 三角形のような潰れた凸体) には成り立たない。
            //   区間で測れば軸の向きにも枝刈りの有無にも依存しない (この式で
            //   一度スープをすり抜けさせている — 専用式のまま無効な軸を通すと、
            //   接触した瞬間に「分離している」と誤答して床が消える)
            float minA = kBig, maxA = -kBig, minB = kBig, maxB = -kBig;
            for (int32_t k = 0; k < a.vertCount; ++k) {
                const float d = Dot(axx, axy, axz, a.vx[k], a.vy[k], a.vz[k]);
                minA = (d < minA) ? d : minA;
                maxA = (d > maxA) ? d : maxA;
            }
            for (int32_t k = 0; k < b.vertCount; ++k) {
                const float d = Dot(axx, axy, axz, b.vx[k], b.vy[k], b.vz[k]);
                minB = (d < minB) ? d : minB;
                maxB = (d > maxB) ? d : maxB;
            }
            // b が軸の + 側なら a は - 側へ押し出す (押し出しは b→a の向き)
            const float sepPlus = minB - maxA;
            const float sepMinus = minA - maxB;
            const float sep = (sepPlus >= sepMinus) ? sepPlus : sepMinus;
            const float sign = (sepPlus >= sepMinus) ? -1.0f : 1.0f;
            if (sep > 0.0f) {
                return false;
            }
            if (sep > out.sep) {
                out.sep = sep;
                out.nx = axx * sign;
                out.ny = axy * sign;
                out.nz = axz * sign;
                out.type = 2;
                out.ia = i;
                out.ib = j;
            }
        }
    }
    return out.type >= 0;
}

// 参照面の側面平面で入射面をクリップし、参照面より内側の点を接触点にする
// (BoxBoxFaceManifold と同じ手順・同じ 4 点選択)
void ClipFaceManifold(const Body& ref, int32_t refFace, const Body& inc, float nx, float ny,
                      float nz, float satDepth, shapes::Manifold& out)
{
    const ConvexHullData& rt = *ref.topo;
    const ConvexFace& rf = rt.faces[static_cast<size_t>(refFace)];
    const float rnx = ref.fnx[refFace], rny = ref.fny[refFace], rnz = ref.fnz[refFace];
    const float rd = ref.fd[refFace];

    // 入射面 = inc の面のうち参照面法線に最も逆向きなもの (固定列挙・strict < = 決定論)
    int32_t incFace = -1;
    float bestDot = kBig;
    for (int32_t fi = 0; fi < inc.faceCount; ++fi) {
        const float d = Dot(inc.fnx[fi], inc.fny[fi], inc.fnz[fi], rnx, rny, rnz);
        if (d < bestDot) {
            bestDot = d;
            incFace = fi;
        }
    }
    out.count = 0;
    if (incFace < 0) {
        return;
    }
    const ConvexHullData& it = *inc.topo;
    const ConvexFace& inf = it.faces[static_cast<size_t>(incFace)];

    constexpr int kMaxPoly = 64;
    float poly[kMaxPoly][3];
    float tmp[kMaxPoly][3];
    int polyN = 0;
    for (int32_t k = 0; k < inf.count && polyN < kMaxPoly; ++k) {
        const int32_t vi = it.faceVerts[static_cast<size_t>(inf.first + k)];
        poly[polyN][0] = inc.vx[vi];
        poly[polyN][1] = inc.vy[vi];
        poly[polyN][2] = inc.vz[vi];
        ++polyN;
    }

    // 参照面の各辺で作る側面平面 (外向き = cross(辺方向, 参照面法線))
    for (int32_t k = 0; k < rf.count && polyN > 0; ++k) {
        const int32_t i0 = rt.faceVerts[static_cast<size_t>(rf.first + k)];
        const int32_t i1 = rt.faceVerts[static_cast<size_t>(rf.first + (k + 1) % rf.count)];
        const float ex = ref.vx[i1] - ref.vx[i0];
        const float ey = ref.vy[i1] - ref.vy[i0];
        const float ez = ref.vz[i1] - ref.vz[i0];
        const float pnx = ey * rnz - ez * rny;
        const float pny = ez * rnx - ex * rnz;
        const float pnz = ex * rny - ey * rnx;
        const float len2 = pnx * pnx + pny * pny + pnz * pnz;
        if (len2 < 1e-16f) {
            continue;
        }
        const float pd = Dot(pnx, pny, pnz, ref.vx[i0], ref.vy[i0], ref.vz[i0]);
        int outN = 0;
        for (int p = 0; p < polyN; ++p) {
            const int p2 = (p + 1) % polyN;
            const float d0 = Dot(pnx, pny, pnz, poly[p][0], poly[p][1], poly[p][2]) - pd;
            const float d1 = Dot(pnx, pny, pnz, poly[p2][0], poly[p2][1], poly[p2][2]) - pd;
            if (d0 <= 0.0f && outN < kMaxPoly) {
                tmp[outN][0] = poly[p][0];
                tmp[outN][1] = poly[p][1];
                tmp[outN][2] = poly[p][2];
                ++outN;
            }
            if (((d0 <= 0.0f) != (d1 <= 0.0f)) && outN < kMaxPoly) {
                const float t = d0 / (d0 - d1);
                for (int c = 0; c < 3; ++c) {
                    tmp[outN][c] = poly[p][c] + (poly[p2][c] - poly[p][c]) * t;
                }
                ++outN;
            }
        }
        polyN = outN;
        for (int p = 0; p < polyN; ++p) {
            poly[p][0] = tmp[p][0];
            poly[p][1] = tmp[p][1];
            poly[p][2] = tmp[p][2];
        }
    }

    // 参照面より内側 (貫通側) の点だけ採る
    shapes::Contact cand[kMaxPoly];
    int candN = 0;
    for (int p = 0; p < polyN; ++p) {
        const float dep = rd - Dot(rnx, rny, rnz, poly[p][0], poly[p][1], poly[p][2]);
        if (dep >= 0.0f) {
            cand[candN].px = poly[p][0];
            cand[candN].py = poly[p][1];
            cand[candN].pz = poly[p][2];
            cand[candN].depth = dep;
            ++candN;
        }
    }
    if (candN == 0) {
        // 数値縮退: SAT が出した深さで入射側の最深頂点 1 点に落とす
        const int32_t s = Support(inc, -nx, -ny, -nz);
        out.pts[0] = { inc.vx[s], inc.vy[s], inc.vz[s], satDepth };
        out.count = 1;
        return;
    }
    // depth 降順の上位 4 点 (同値は元順先勝ち = 決定論。BoxBoxFaceManifold と同じ選び方)
    bool used[kMaxPoly] = {};
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

// 2 線分の最近点パラメータ (Shapes.cpp の ClosestSegSeg と同じ式。稜線接触で使う)
void ClosestSegSegLocal(float p1x, float p1y, float p1z, float d1x, float d1y, float d1z,
                        float p2x, float p2y, float p2z, float d2x, float d2y, float d2z, float& s,
                        float& t)
{
    const float a = Dot(d1x, d1y, d1z, d1x, d1y, d1z);
    const float e = Dot(d2x, d2y, d2z, d2x, d2y, d2z);
    const float rx = p1x - p2x, ry = p1y - p2y, rz = p1z - p2z;
    const float f = Dot(d2x, d2y, d2z, rx, ry, rz);
    const float c = Dot(d1x, d1y, d1z, rx, ry, rz);
    s = 0.0f;
    t = 0.0f;
    if (a <= 1e-12f && e <= 1e-12f) {
        return;
    }
    if (a <= 1e-12f) {
        t = std::clamp(f / e, 0.0f, 1.0f);
        return;
    }
    if (e <= 1e-12f) {
        s = std::clamp(-c / a, 0.0f, 1.0f);
        return;
    }
    const float b = Dot(d1x, d1y, d1z, d2x, d2y, d2z);
    const float denom = a * e - b * b;
    s = (denom > 1e-12f) ? std::clamp((b * f - c * e) / denom, 0.0f, 1.0f) : 0.0f;
    t = std::clamp((b * s + f) / e, 0.0f, 1.0f);
    s = std::clamp((b * t - c) / a, 0.0f, 1.0f);
}

// 面 (凸多角形) 上で p に最も近い点
void ClosestOnFace(const Body& b, int32_t fi, float px, float py, float pz, float& qx, float& qy,
                   float& qz)
{
    const ConvexHullData& t = *b.topo;
    const ConvexFace& f = t.faces[static_cast<size_t>(fi)];
    const float nx = b.fnx[fi], ny = b.fny[fi], nz = b.fnz[fi];
    const float dist = Dot(nx, ny, nz, px, py, pz) - b.fd[fi];
    const float ppx = px - nx * dist, ppy = py - ny * dist, ppz = pz - nz * dist;
    bool inside = true;
    for (int32_t k = 0; k < f.count; ++k) {
        const int32_t i0 = t.faceVerts[static_cast<size_t>(f.first + k)];
        const int32_t i1 = t.faceVerts[static_cast<size_t>(f.first + (k + 1) % f.count)];
        const float ex = b.vx[i1] - b.vx[i0], ey = b.vy[i1] - b.vy[i0], ez = b.vz[i1] - b.vz[i0];
        const float sx = ey * nz - ez * ny; // 外向きの側面法線
        const float sy = ez * nx - ex * nz;
        const float sz = ex * ny - ey * nx;
        if (Dot(sx, sy, sz, ppx - b.vx[i0], ppy - b.vy[i0], ppz - b.vz[i0]) > 0.0f) {
            inside = false;
            break;
        }
    }
    if (inside) {
        qx = ppx;
        qy = ppy;
        qz = ppz;
        return;
    }
    // 面の外へ出たら辺へクランプ (全辺を舐めて最短。固定列挙 = 決定論)
    float best = kBig;
    qx = b.vx[t.faceVerts[static_cast<size_t>(f.first)]];
    qy = b.vy[t.faceVerts[static_cast<size_t>(f.first)]];
    qz = b.vz[t.faceVerts[static_cast<size_t>(f.first)]];
    for (int32_t k = 0; k < f.count; ++k) {
        const int32_t i0 = t.faceVerts[static_cast<size_t>(f.first + k)];
        const int32_t i1 = t.faceVerts[static_cast<size_t>(f.first + (k + 1) % f.count)];
        const float ex = b.vx[i1] - b.vx[i0], ey = b.vy[i1] - b.vy[i0], ez = b.vz[i1] - b.vz[i0];
        const float len2 = ex * ex + ey * ey + ez * ez;
        float s = 0.0f;
        if (len2 > 1e-16f) {
            s = std::clamp(Dot(ex, ey, ez, px - b.vx[i0], py - b.vy[i0], pz - b.vz[i0]) / len2,
                           0.0f, 1.0f);
        }
        const float cxp = b.vx[i0] + ex * s, cyp = b.vy[i0] + ey * s, czp = b.vz[i0] + ez * s;
        const float ddx = px - cxp, ddy = py - cyp, ddz = pz - czp;
        const float d2 = ddx * ddx + ddy * ddy + ddz * ddz;
        if (d2 < best) { // strict < = 同値は小さい辺番号が勝つ
            best = d2;
            qx = cxp;
            qy = cyp;
            qz = czp;
        }
    }
}

// 全面の支持平面までの最大距離 (内部なら負)。カプセルの黄金分割の目的関数に使う —
// t について凸なので固定回数の黄金分割で最小点が取れる
float MaxPlaneDist(const Body& b, float px, float py, float pz, int32_t& faceOut)
{
    float best = -kBig;
    faceOut = 0;
    for (int32_t fi = 0; fi < b.faceCount; ++fi) {
        if (b.fnx[fi] * b.fnx[fi] + b.fny[fi] * b.fny[fi] + b.fnz[fi] * b.fnz[fi] < 0.5f) {
            continue;
        }
        const float d = Dot(b.fnx[fi], b.fny[fi], b.fnz[fi], px, py, pz) - b.fd[fi];
        if (d > best) {
            best = d;
            faceOut = fi;
        }
    }
    return best;
}

void CapsuleSegmentWorld(const ShapePose& c, float& x0, float& y0, float& z0, float& x1,
                         float& y1, float& z1)
{
    x0 = c.px - c.by[0] * c.halfSeg;
    y0 = c.py - c.by[1] * c.halfSeg;
    z0 = c.pz - c.by[2] * c.halfSeg;
    x1 = c.px + c.by[0] * c.halfSeg;
    y1 = c.py + c.by[1] * c.halfSeg;
    z1 = c.pz + c.by[2] * c.halfSeg;
}

} // namespace

bool BuildFromPose(const ShapePose& p, Body& out)
{
    const ConvexHullData* h = static_cast<const ConvexHullData*>(p.meshData);
    if (!h || !h->Valid()) {
        return false;
    }
    out.topo = h;
    out.vertCount = std::min(static_cast<int32_t>(h->verts.size()), kConvexMaxVerts);
    for (int32_t i = 0; i < out.vertCount; ++i) {
        const DirectX::XMFLOAT3& v = h->verts[static_cast<size_t>(i)];
        const float lx = v.x * p.sx, ly = v.y * p.sy, lz = v.z * p.sz;
        out.vx[i] = p.px + p.bx[0] * lx + p.by[0] * ly + p.bz[0] * lz;
        out.vy[i] = p.py + p.bx[1] * lx + p.by[1] * ly + p.bz[1] * lz;
        out.vz[i] = p.pz + p.bx[2] * lx + p.by[2] * ly + p.bz[2] * lz;
    }
    ComputeFacePlanes(out);
    return true;
}

void BuildFromBox(const ShapePose& p, Body& out)
{
    out.topo = &BoxTopology();
    out.vertCount = 8;
    for (int32_t i = 0; i < 8; ++i) {
        const float lx = kBoxSign[i][0] * p.hx;
        const float ly = kBoxSign[i][1] * p.hy;
        const float lz = kBoxSign[i][2] * p.hz;
        out.vx[i] = p.px + p.bx[0] * lx + p.by[0] * ly + p.bz[0] * lz;
        out.vy[i] = p.py + p.bx[1] * lx + p.by[1] * ly + p.bz[1] * lz;
        out.vz[i] = p.pz + p.bx[2] * lx + p.by[2] * ly + p.bz[2] * lz;
    }
    ComputeFacePlanes(out);
}

void BuildFromTriangle(float ax, float ay, float az, float bx, float by, float bz, float cx,
                       float cy, float cz, Body& out)
{
    out.topo = &TriangleTopology();
    out.vertCount = 3;
    out.vx[0] = ax;
    out.vy[0] = ay;
    out.vz[0] = az;
    out.vx[1] = bx;
    out.vy[1] = by;
    out.vz[1] = bz;
    out.vx[2] = cx;
    out.vy[2] = cy;
    out.vz[2] = cz;
    ComputeFacePlanes(out);
}

bool Collide(const Body& a, const Body& b, float& nx, float& ny, float& nz, float& depth)
{
    Axis ax;
    if (!SatQuery(a, b, ax)) {
        return false;
    }
    nx = ax.nx;
    ny = ax.ny;
    nz = ax.nz;
    depth = -ax.sep;
    return true;
}

bool CollideManifold(const Body& a, const Body& b, shapes::Manifold& out)
{
    out.count = 0;
    Axis ax;
    if (!SatQuery(a, b, ax)) {
        return false;
    }
    out.nx = ax.nx;
    out.ny = ax.ny;
    out.nz = ax.nz;
    const float depth = -ax.sep;
    if (ax.type == 0) {
        ClipFaceManifold(a, ax.ia, b, ax.nx, ax.ny, ax.nz, depth, out);
    } else if (ax.type == 1) {
        ClipFaceManifold(b, ax.ib, a, ax.nx, ax.ny, ax.nz, depth, out);
    } else {
        // 稜線接触は 1 点 (2 本の最近点の中点)
        const ConvexEdge& e1 = a.topo->edges[static_cast<size_t>(ax.ia)];
        const ConvexEdge& e2 = b.topo->edges[static_cast<size_t>(ax.ib)];
        const float p1x = a.vx[e1.v0], p1y = a.vy[e1.v0], p1z = a.vz[e1.v0];
        const float d1x = a.vx[e1.v1] - p1x, d1y = a.vy[e1.v1] - p1y, d1z = a.vz[e1.v1] - p1z;
        const float p2x = b.vx[e2.v0], p2y = b.vy[e2.v0], p2z = b.vz[e2.v0];
        const float d2x = b.vx[e2.v1] - p2x, d2y = b.vy[e2.v1] - p2y, d2z = b.vz[e2.v1] - p2z;
        float s = 0, t = 0;
        ClosestSegSegLocal(p1x, p1y, p1z, d1x, d1y, d1z, p2x, p2y, p2z, d2x, d2y, d2z, s, t);
        const float qax = p1x + d1x * s, qay = p1y + d1y * s, qaz = p1z + d1z * s;
        const float qbx = p2x + d2x * t, qby = p2y + d2y * t, qbz = p2z + d2z * t;
        out.pts[0] = { (qax + qbx) * 0.5f, (qay + qby) * 0.5f, (qaz + qbz) * 0.5f, depth };
        out.count = 1;
    }
    return out.count > 0;
}

float SignedDistance(const Body& b, float px, float py, float pz, float& qx, float& qy, float& qz,
                     float& onx, float& ony, float& onz)
{
    int32_t face = 0;
    const float maxDist = MaxPlaneDist(b, px, py, pz, face);
    if (maxDist <= 0.0f) {
        // 内部: 最も浅い面へ射影する (押し出す向きはその面の外向き法線)
        onx = b.fnx[face];
        ony = b.fny[face];
        onz = b.fnz[face];
        qx = px - onx * maxDist;
        qy = py - ony * maxDist;
        qz = pz - onz * maxDist;
        return maxDist;
    }
    // 外部: 全面の多角形へ落として最短を採る (凸なので表面最近点は必ずどれかの面上)
    float best = kBig;
    qx = px;
    qy = py;
    qz = pz;
    for (int32_t fi = 0; fi < b.faceCount; ++fi) {
        if (b.fnx[fi] * b.fnx[fi] + b.fny[fi] * b.fny[fi] + b.fnz[fi] * b.fnz[fi] < 0.5f) {
            continue;
        }
        float cxp, cyp, czp;
        ClosestOnFace(b, fi, px, py, pz, cxp, cyp, czp);
        const float dx = px - cxp, dy = py - cyp, dz = pz - czp;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < best) { // strict < = 同値は小さい面番号が勝つ
            best = d2;
            qx = cxp;
            qy = cyp;
            qz = czp;
        }
    }
    const float dist = std::sqrt(best);
    if (dist > 1e-8f) {
        onx = (px - qx) / dist;
        ony = (py - qy) / dist;
        onz = (pz - qz) / dist;
    } else { // 表面上ぴったり = 向きが決まらないので面法線を採る
        onx = b.fnx[face];
        ony = b.fny[face];
        onz = b.fnz[face];
    }
    return dist;
}

bool SphereContact(const Body& b, float sx, float sy, float sz, float r, float& nx, float& ny,
                   float& nz, float& depth, float& qx, float& qy, float& qz)
{
    float onx, ony, onz;
    const float d = SignedDistance(b, sx, sy, sz, qx, qy, qz, onx, ony, onz);
    if (d >= r) {
        return false;
    }
    nx = onx; // 凸→球 (球を押し出す向き)
    ny = ony;
    nz = onz;
    depth = r - d;
    return true;
}

namespace {

// カプセルの芯に沿って「凸体へ最も食い込む点」を固定 32 回の黄金分割で探す。
// 目的関数 (支持平面までの最大距離) は t について凸 = 単峰なので収束する。
// 反復回数を固定するのは GoldenSegParamToLocalAabb と同じ理由 (収束判定は決定論を壊す)
float GoldenSegParam(const Body& b, float x0, float y0, float z0, float x1, float y1, float z1)
{
    auto eval = [&](float t) {
        int32_t f = 0;
        return MaxPlaneDist(b, x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, z0 + (z1 - z0) * t, f);
    };
    constexpr float kInvPhi = 0.6180339887f;
    float lo = 0.0f, hi = 1.0f;
    float m1 = hi - (hi - lo) * kInvPhi;
    float m2 = lo + (hi - lo) * kInvPhi;
    float f1 = eval(m1);
    float f2 = eval(m2);
    for (int i = 0; i < 32; ++i) {
        if (f1 < f2) {
            hi = m2;
            m2 = m1;
            f2 = f1;
            m1 = hi - (hi - lo) * kInvPhi;
            f1 = eval(m1);
        } else {
            lo = m1;
            m1 = m2;
            f1 = f2;
            m2 = lo + (hi - lo) * kInvPhi;
            f2 = eval(m2);
        }
    }
    return (lo + hi) * 0.5f;
}

} // namespace

bool CapsuleContact(const Body& b, const ShapePose& cap, float& nx, float& ny, float& nz,
                    float& depth)
{
    float x0, y0, z0, x1, y1, z1;
    CapsuleSegmentWorld(cap, x0, y0, z0, x1, y1, z1);
    const float t = GoldenSegParam(b, x0, y0, z0, x1, y1, z1);
    float qx, qy, qz;
    return SphereContact(b, x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, z0 + (z1 - z0) * t, cap.radius,
                         nx, ny, nz, depth, qx, qy, qz);
}

bool CapsuleManifold(const Body& b, const ShapePose& cap, shapes::Manifold& out)
{
    out.count = 0;
    float x0, y0, z0, x1, y1, z1;
    CapsuleSegmentWorld(cap, x0, y0, z0, x1, y1, z1);
    const float t = GoldenSegParam(b, x0, y0, z0, x1, y1, z1);
    float nx, ny, nz, depth, qx, qy, qz;
    if (!SphereContact(b, x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, z0 + (z1 - z0) * t, cap.radius,
                       nx, ny, nz, depth, qx, qy, qz)) {
        return false;
    }
    out.nx = nx;
    out.ny = ny;
    out.nz = nz;
    out.pts[out.count++] = { qx, qy, qz, depth };
    // 側面接触 (芯が面に平行) を 2 点にする。法線が揃うときだけ足す —
    // CapsuleBoxManifold と同じ判定 (揃わない端は別の面に当たっているので混ぜない)
    const float ends[2] = { 0.0f, 1.0f };
    for (float te : ends) {
        if (std::fabs(te - t) < 1e-3f || out.count >= 4) {
            continue;
        }
        float enx, eny, enz, edep, eqx, eqy, eqz;
        if (SphereContact(b, x0 + (x1 - x0) * te, y0 + (y1 - y0) * te, z0 + (z1 - z0) * te,
                          cap.radius, enx, eny, enz, edep, eqx, eqy, eqz)
            && enx * nx + eny * ny + enz * nz > 0.99f) {
            out.pts[out.count++] = { eqx, eqy, eqz, edep };
        }
    }
    return true;
}

bool Raycast(const Body& b, float ox, float oy, float oz, float dx, float dy, float dz,
             float maxDist, float& outT, float& nx, float& ny, float& nz)
{
    float tmin = 0.0f, tmax = maxDist;
    int32_t hitFace = -1;
    for (int32_t fi = 0; fi < b.faceCount; ++fi) {
        const float fnx = b.fnx[fi], fny = b.fny[fi], fnz = b.fnz[fi];
        if (fnx * fnx + fny * fny + fnz * fnz < 0.5f) {
            continue;
        }
        const float denom = Dot(fnx, fny, fnz, dx, dy, dz);
        const float num = b.fd[fi] - Dot(fnx, fny, fnz, ox, oy, oz);
        if (std::fabs(denom) < 1e-8f) {
            if (num < 0.0f) {
                return false; // 平面に平行で外側 = 交わらない
            }
            continue;
        }
        const float t = num / denom;
        if (denom < 0.0f) { // 入る側の面
            if (t > tmin) {
                tmin = t;
                hitFace = fi;
            }
        } else if (t < tmax) { // 出る側の面
            tmax = t;
        }
        if (tmin > tmax) {
            return false;
        }
    }
    if (hitFace < 0) {
        return false; // 始点が内部 (面を 1 枚も通らない) = ヒットとして扱わない
    }
    if (tmin < 0.0f || tmin > maxDist) {
        return false;
    }
    outT = tmin;
    nx = b.fnx[hitFace];
    ny = b.fny[hitFace];
    nz = b.fnz[hitFace];
    return true;
}

} // namespace convex
} // namespace mye
