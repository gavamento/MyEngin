#include "Engine/Engine/Physics/ConvexHull.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

using namespace DirectX;

namespace mye {
namespace {

struct V3 {
    float x = 0, y = 0, z = 0;
};

V3 Sub(const V3& a, const V3& b)
{
    return { a.x - b.x, a.y - b.y, a.z - b.z };
}
V3 Cross3(const V3& a, const V3& b)
{
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}
float Dot3(const V3& a, const V3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
float Len3(const V3& a)
{
    return std::sqrt(Dot3(a, a));
}
V3 Mul3(const V3& a, float s)
{
    return { a.x * s, a.y * s, a.z * s };
}

// -0.0 を +0.0 へ畳む。**分岐ゲートで書く** — `v + 0.0f` は -0.0 を +0.0 に化けさせる
// 値ゲートで、M59f1-4 で禁止した書き方 (引き算は安全だが足し算は危険)。
// これをやらないと「入力に -0.0 と +0.0 が混ざっていると重複除去でどちらが残るかが
// 入力順で変わる」= 生成が入力順に依らないという主張が崩れる
float Zeroed(float v)
{
    return (v == 0.0f) ? 0.0f : v;
}

struct PointRef {
    V3 p;
    int32_t idx = 0; // 入力での元 index (タイブレーク専用)
};

// (x, y, z, 元 index) の全順序。同じ点集合なら入力順に依らず必ず同じ列になる
bool PointLess(const PointRef& a, const PointRef& b)
{
    if (a.p.x != b.p.x) {
        return a.p.x < b.p.x;
    }
    if (a.p.y != b.p.y) {
        return a.p.y < b.p.y;
    }
    if (a.p.z != b.p.z) {
        return a.p.z < b.p.z;
    }
    return a.idx < b.idx;
}

struct HullTri {
    int32_t v[3] = { 0, 0, 0 };
    V3 n;        // 外向き単位法線
    float d = 0; // n·x = d
    bool alive = true;
};

// 三角形の支持平面。面積が縮退していれば false (呼び出し側はその点の追加を諦める)
bool MakeTriPlane(const std::vector<PointRef>& pts, int32_t a, int32_t b, int32_t c, float areaEps,
                  V3& n, float& d)
{
    const V3 e1 = Sub(pts[static_cast<size_t>(b)].p, pts[static_cast<size_t>(a)].p);
    const V3 e2 = Sub(pts[static_cast<size_t>(c)].p, pts[static_cast<size_t>(a)].p);
    const V3 cr = Cross3(e1, e2);
    const float len = Len3(cr);
    if (!(len > areaEps)) {
        return false;
    }
    n = Mul3(cr, 1.0f / len);
    d = Dot3(n, pts[static_cast<size_t>(a)].p);
    return true;
}

float PlaneDist(const HullTri& t, const V3& p)
{
    return Dot3(t.n, p) - t.d;
}

// 三角形が有向辺 (u,v) を**この向きで**含むか
bool HasDirectedEdge(const HullTri& t, int32_t u, int32_t v)
{
    for (int k = 0; k < 3; ++k) {
        if (t.v[k] == u && t.v[(k + 1) % 3] == v) {
            return true;
        }
    }
    return false;
}

// ---- 逐次追加による三角形凸包 ----
// pts はソート済み・重複なし。false = 縮退 (点 / 線分 / 平面) で四面体すら作れなかった
bool BuildTriHull(const std::vector<PointRef>& pts, float eps, float areaEps,
                  std::vector<HullTri>& tris)
{
    const int32_t n = static_cast<int32_t>(pts.size());
    if (n < 4) {
        return false;
    }

    // 初期四面体。すべて「距離最大、同値は index 小」で選ぶ (strict > のタイブレーク)
    const int32_t i0 = 0; // ソート後の先頭 = 辞書順最小
    int32_t i1 = -1;
    float best = eps;
    for (int32_t i = 1; i < n; ++i) {
        const float len = Len3(Sub(pts[static_cast<size_t>(i)].p, pts[static_cast<size_t>(i0)].p));
        if (len > best) {
            best = len;
            i1 = i;
        }
    }
    if (i1 < 0) {
        return false; // 全点が同一 = 点に潰れている
    }

    const V3 axis
        = Mul3(Sub(pts[static_cast<size_t>(i1)].p, pts[static_cast<size_t>(i0)].p), 1.0f / best);
    int32_t i2 = -1;
    best = eps;
    for (int32_t i = 1; i < n; ++i) {
        if (i == i1) {
            continue;
        }
        const V3 w = Sub(pts[static_cast<size_t>(i)].p, pts[static_cast<size_t>(i0)].p);
        const float h = Len3(Cross3(w, axis)); // 直線からの距離
        if (h > best) {
            best = h;
            i2 = i;
        }
    }
    if (i2 < 0) {
        return false; // 線分に潰れている
    }

    V3 baseN;
    float baseD;
    if (!MakeTriPlane(pts, i0, i1, i2, areaEps, baseN, baseD)) {
        return false;
    }
    int32_t i3 = -1;
    best = eps;
    for (int32_t i = 1; i < n; ++i) {
        if (i == i1 || i == i2) {
            continue;
        }
        const float h = std::fabs(Dot3(baseN, pts[static_cast<size_t>(i)].p) - baseD);
        if (h > best) {
            best = h;
            i3 = i;
        }
    }
    if (i3 < 0) {
        return false; // 平面に潰れている
    }

    // 4 面を作り、**重心を使って向きを揃える**。巻き順を手で詰めるより
    // 「重心が必ず裏側に来る」で機械的に直すほうが取り違えようがない
    const int32_t quad[4] = { i0, i1, i2, i3 };
    V3 centroid{};
    for (int k = 0; k < 4; ++k) {
        const V3& p = pts[static_cast<size_t>(quad[k])].p;
        centroid.x += p.x * 0.25f;
        centroid.y += p.y * 0.25f;
        centroid.z += p.z * 0.25f;
    }
    tris.clear();
    const int32_t faceIdx[4][3] = { { 0, 1, 2 }, { 0, 1, 3 }, { 0, 2, 3 }, { 1, 2, 3 } };
    for (const auto& f : faceIdx) {
        int32_t a = quad[f[0]], b = quad[f[1]], c = quad[f[2]];
        V3 nn;
        float dd;
        if (!MakeTriPlane(pts, a, b, c, areaEps, nn, dd)) {
            return false;
        }
        if (Dot3(nn, centroid) - dd > 0.0f) { // 重心が表側 = 向きが逆
            std::swap(b, c);
            if (!MakeTriPlane(pts, a, b, c, areaEps, nn, dd)) {
                return false;
            }
        }
        HullTri t;
        t.v[0] = a;
        t.v[1] = b;
        t.v[2] = c;
        t.n = nn;
        t.d = dd;
        tris.push_back(t);
    }

    // 残りの点を「まだ外側にある候補」として index 昇順で保持する
    std::vector<int32_t> outside;
    outside.reserve(static_cast<size_t>(n));
    for (int32_t i = 0; i < n; ++i) {
        if (i != i0 && i != i1 && i != i2 && i != i3) {
            outside.push_back(i);
        }
    }

    int32_t vertCount = 4;
    std::vector<int32_t> visible, keep;
    std::vector<std::pair<int32_t, int32_t>> horizon;
    std::vector<HullTri> newTris;
    while (vertCount < kConvexMaxVerts && !outside.empty()) {
        // 1 パスで「最遠点の選定」と「内側に入った点の除去」を兼ねる。
        // 内側に落ちた点を毎回捨てるので、頂点数の多いメッシュでも候補列は急速に縮む
        int32_t pick = -1;
        float pickDist = eps;
        keep.clear();
        for (int32_t pi : outside) {
            const V3& p = pts[static_cast<size_t>(pi)].p;
            float dmax = -std::numeric_limits<float>::max();
            for (const HullTri& t : tris) {
                if (!t.alive) {
                    continue;
                }
                const float dist = PlaneDist(t, p);
                if (dist > dmax) {
                    dmax = dist;
                }
            }
            if (dmax > eps) {
                keep.push_back(pi);
                if (dmax > pickDist) { // strict > = 同値なら先着 (index 小) が勝つ
                    pickDist = dmax;
                    pick = pi;
                }
            }
        }
        outside.swap(keep);
        if (pick < 0) {
            break;
        }
        const V3& p = pts[static_cast<size_t>(pick)].p;

        visible.clear();
        for (int32_t ti = 0; ti < static_cast<int32_t>(tris.size()); ++ti) {
            const HullTri& t = tris[static_cast<size_t>(ti)];
            if (t.alive && PlaneDist(t, p) > eps) {
                visible.push_back(ti);
            }
        }

        // 地平線 = 可視面の有向辺のうち、逆向きの相方が別の可視面に無いもの。
        // 面 index 昇順 × 面内の辺順で走査するので列は決定的
        horizon.clear();
        for (int32_t vi : visible) {
            const HullTri& t = tris[static_cast<size_t>(vi)];
            for (int k = 0; k < 3; ++k) {
                const int32_t a = t.v[k], b = t.v[(k + 1) % 3];
                bool shared = false;
                for (int32_t vj : visible) {
                    if (vj != vi && HasDirectedEdge(tris[static_cast<size_t>(vj)], b, a)) {
                        shared = true;
                        break;
                    }
                }
                if (!shared) {
                    horizon.emplace_back(a, b);
                }
            }
        }

        // 新面を**全部作れることを確かめてから**差し替える。1 枚でも縮退していたら
        // その点を捨てる — 半分だけ張り替えると凸包が開いてしまい復旧できない
        bool ok = horizon.size() >= 3;
        newTris.clear();
        for (const auto& e : horizon) {
            V3 nn;
            float dd;
            if (!MakeTriPlane(pts, e.first, e.second, pick, areaEps, nn, dd)) {
                ok = false;
                break;
            }
            HullTri t;
            t.v[0] = e.first;
            t.v[1] = e.second;
            t.v[2] = pick;
            t.n = nn;
            t.d = dd;
            newTris.push_back(t);
        }
        if (!ok) {
            continue; // pick は既に outside から外れている
        }
        for (int32_t vi : visible) {
            tris[static_cast<size_t>(vi)].alive = false;
        }
        tris.insert(tris.end(), newTris.begin(), newTris.end());
        ++vertCount;
    }

    std::vector<HullTri> compact;
    compact.reserve(tris.size());
    for (const HullTri& t : tris) {
        if (t.alive) {
            compact.push_back(t);
        }
    }
    tris.swap(compact);
    return tris.size() >= 4;
}

// ---- 有向辺の集合を 1 本の環に繋ぐ。false = 単純な閉路にならなかった ----
bool ChainLoop(std::vector<std::pair<int32_t, int32_t>> edges, std::vector<int32_t>& loop)
{
    loop.clear();
    if (edges.size() < 3) {
        return false;
    }
    std::sort(edges.begin(), edges.end());
    for (size_t i = 1; i < edges.size(); ++i) {
        if (edges[i].first == edges[i - 1].first) {
            return false; // 同じ頂点から 2 本出ている = 単純な多角形でない
        }
    }
    const int32_t start = edges[0].first; // 最小の始点から回す (決定的)
    int32_t cur = start;
    for (size_t i = 0; i < edges.size(); ++i) {
        loop.push_back(cur);
        const auto it = std::lower_bound(
            edges.begin(), edges.end(),
            std::make_pair(cur, std::numeric_limits<int32_t>::lowest()));
        if (it == edges.end() || it->first != cur) {
            loop.clear();
            return false;
        }
        cur = it->second;
    }
    if (cur != start) {
        loop.clear();
        return false; // 環が閉じない (穴あき / 分裂)
    }
    return true;
}

// 多角形の Newell 法線 (巻き順に一致した向き)。false = 面積が縮退
bool NewellPlane(const std::vector<XMFLOAT3>& verts, const int32_t* loop, int32_t count,
                 float areaEps, V3& n, float& d)
{
    V3 acc{};
    V3 c{};
    for (int32_t i = 0; i < count; ++i) {
        const XMFLOAT3& a = verts[static_cast<size_t>(loop[i])];
        const XMFLOAT3& b = verts[static_cast<size_t>(loop[(i + 1) % count])];
        acc.x += (a.y - b.y) * (a.z + b.z);
        acc.y += (a.z - b.z) * (a.x + b.x);
        acc.z += (a.x - b.x) * (a.y + b.y);
        c.x += a.x;
        c.y += a.y;
        c.z += a.z;
    }
    const float len = Len3(acc);
    if (!(len > areaEps)) {
        return false;
    }
    n = Mul3(acc, 1.0f / len);
    d = Dot3(n, Mul3(c, 1.0f / static_cast<float>(count)));
    return true;
}

// ---- 稜線表 (面の巻き順から一意な無向辺 + 隣接 2 面) ----
void BuildEdges(ConvexHullData& h)
{
    struct Rec {
        int32_t a, b, face;
    };
    std::vector<Rec> recs;
    recs.reserve(h.faceVerts.size());
    for (int32_t fi = 0; fi < static_cast<int32_t>(h.faces.size()); ++fi) {
        const ConvexFace& f = h.faces[static_cast<size_t>(fi)];
        for (int32_t k = 0; k < f.count; ++k) {
            const int32_t u = h.faceVerts[static_cast<size_t>(f.first + k)];
            const int32_t v = h.faceVerts[static_cast<size_t>(f.first + (k + 1) % f.count)];
            recs.push_back({ (u < v) ? u : v, (u < v) ? v : u, fi });
        }
    }
    std::sort(recs.begin(), recs.end(), [](const Rec& x, const Rec& y) {
        if (x.a != y.a) {
            return x.a < y.a;
        }
        if (x.b != y.b) {
            return x.b < y.b;
        }
        return x.face < y.face;
    });
    h.edges.clear();
    for (size_t i = 0; i + 1 < recs.size();) {
        if (recs[i].a == recs[i + 1].a && recs[i].b == recs[i + 1].b) {
            ConvexEdge e;
            e.v0 = recs[i].a;
            e.v1 = recs[i].b;
            e.f0 = recs[i].face;
            e.f1 = recs[i + 1].face;
            h.edges.push_back(e);
            i += 2;
        } else {
            ++i; // 相方のいない辺 = 非多様体。SAT の軸候補から外すだけで済ませる
        }
    }
}

// ---- 質量特性 (四面体積分、密度 1) ----
// 面を先頭頂点から扇状に三角形分割し、原点を頂点とする四面体の符号付き寄与を足す。
// 共分散 C = ∫ x xᵀ dV を貯めてから I = tr(C)·E − C にする (Blow & Binstock の手)。
// 走査順は 面 index 昇順 × 扇の順で固定 = 和の丸めまで決定的
void IntegrateMass(const ConvexHullData& h, float sx, float sy, float sz, float& volume,
                   XMFLOAT3& com, float inertia[3][3])
{
    volume = 0.0f;
    com = { 0, 0, 0 };
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            inertia[r][c] = 0.0f;
        }
    }
    float C[3][3] = {};
    V3 comAcc{};
    for (const ConvexFace& f : h.faces) {
        for (int32_t k = 1; k + 1 < f.count; ++k) {
            const XMFLOAT3& q0 = h.verts[static_cast<size_t>(h.faceVerts[static_cast<size_t>(
                f.first)])];
            const XMFLOAT3& q1 = h.verts[static_cast<size_t>(h.faceVerts[static_cast<size_t>(
                f.first + k)])];
            const XMFLOAT3& q2 = h.verts[static_cast<size_t>(h.faceVerts[static_cast<size_t>(
                f.first + k + 1)])];
            const V3 p0 = { q0.x * sx, q0.y * sy, q0.z * sz };
            const V3 p1 = { q1.x * sx, q1.y * sy, q1.z * sz };
            const V3 p2 = { q2.x * sx, q2.y * sy, q2.z * sz };
            const float det = Dot3(p0, Cross3(p1, p2)); // = 6 × 四面体の符号付き体積
            volume += det * (1.0f / 6.0f);
            const V3 s = { p0.x + p1.x + p2.x, p0.y + p1.y + p2.y, p0.z + p1.z + p2.z };
            comAcc.x += s.x * det * (1.0f / 24.0f);
            comAcc.y += s.y * det * (1.0f / 24.0f);
            comAcc.z += s.z * det * (1.0f / 24.0f);
            // A·Ccanon·Aᵀ = (1/120)(p0p0ᵀ + p1p1ᵀ + p2p2ᵀ + s sᵀ)。行列積を展開せずに済む
            const float w = det * (1.0f / 120.0f);
            const float px[4] = { p0.x, p1.x, p2.x, s.x };
            const float py[4] = { p0.y, p1.y, p2.y, s.y };
            const float pz[4] = { p0.z, p1.z, p2.z, s.z };
            for (int t = 0; t < 4; ++t) {
                const float v[3] = { px[t], py[t], pz[t] };
                for (int r = 0; r < 3; ++r) {
                    for (int c = 0; c < 3; ++c) {
                        C[r][c] += w * v[r] * v[c];
                    }
                }
            }
        }
    }
    if (!(volume > 1e-12f)) {
        volume = 0.0f;
        return;
    }
    com = { comAcc.x / volume, comAcc.y / volume, comAcc.z / volume };
    // 重心へ平行移動 (Huygens-Steiner を共分散の形で)
    const float cm[3] = { com.x, com.y, com.z };
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            C[r][c] -= volume * cm[r] * cm[c];
        }
    }
    const float tr = C[0][0] + C[1][1] + C[2][2];
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            inertia[r][c] = ((r == c) ? tr : 0.0f) - C[r][c];
        }
    }
}

// AABB / 外接半径 / 質量特性をまとめて確定する
void FinalizeHull(ConvexHullData& h)
{
    BuildEdges(h);
    if (!h.verts.empty()) {
        h.aabbMin = h.verts[0];
        h.aabbMax = h.verts[0];
        for (const XMFLOAT3& v : h.verts) {
            h.aabbMin.x = (v.x < h.aabbMin.x) ? v.x : h.aabbMin.x;
            h.aabbMin.y = (v.y < h.aabbMin.y) ? v.y : h.aabbMin.y;
            h.aabbMin.z = (v.z < h.aabbMin.z) ? v.z : h.aabbMin.z;
            h.aabbMax.x = (v.x > h.aabbMax.x) ? v.x : h.aabbMax.x;
            h.aabbMax.y = (v.y > h.aabbMax.y) ? v.y : h.aabbMax.y;
            h.aabbMax.z = (v.z > h.aabbMax.z) ? v.z : h.aabbMax.z;
        }
        float r2 = 0.0f;
        for (const XMFLOAT3& v : h.verts) {
            const float d2 = v.x * v.x + v.y * v.y + v.z * v.z;
            r2 = (d2 > r2) ? d2 : r2;
        }
        h.boundRadius = std::sqrt(r2);
    }
    IntegrateMass(h, 1.0f, 1.0f, 1.0f, h.volume, h.com, h.inertia);
}

// ---- 縮退入力の安全網: AABB の箱を凸包として返す ----
// 「形状なし」に落として物体をすり抜けさせるより、太らせた箱で当たるほうが常に安全。
// 潰れた軸には最小厚みを与える (零体積は慣性が発散して剛体が壊れる)
void BuildBoxHull(const V3& lo, const V3& hi, float thick, ConvexHullData& out)
{
    V3 a = lo, b = hi;
    const float half = thick * 0.5f;
    if (b.x - a.x < thick) {
        const float c = (a.x + b.x) * 0.5f;
        a.x = c - half;
        b.x = c + half;
    }
    if (b.y - a.y < thick) {
        const float c = (a.y + b.y) * 0.5f;
        a.y = c - half;
        b.y = c + half;
    }
    if (b.z - a.z < thick) {
        const float c = (a.z + b.z) * 0.5f;
        a.z = c - half;
        b.z = c + half;
    }
    out = ConvexHullData{};
    out.verts = {
        { a.x, a.y, a.z }, { b.x, a.y, a.z }, { b.x, b.y, a.z }, { a.x, b.y, a.z },
        { a.x, a.y, b.z }, { b.x, a.y, b.z }, { b.x, b.y, b.z }, { a.x, b.y, b.z },
    };
    const int32_t quads[6][4] = {
        { 0, 1, 2, 3 }, { 4, 5, 6, 7 }, { 0, 1, 5, 4 }, { 3, 2, 6, 7 }, { 0, 3, 7, 4 },
        { 1, 2, 6, 5 },
    };
    const V3 center = { (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f };
    for (const auto& q : quads) {
        int32_t loop[4] = { q[0], q[1], q[2], q[3] };
        V3 n;
        float d;
        if (!NewellPlane(out.verts, loop, 4, 0.0f, n, d)) {
            continue;
        }
        if (Dot3(n, center) - d > 0.0f) { // 中心が表側 = 巻き順が逆
            std::swap(loop[1], loop[3]);
            if (!NewellPlane(out.verts, loop, 4, 0.0f, n, d)) {
                continue;
            }
        }
        ConvexFace f;
        f.nx = n.x;
        f.ny = n.y;
        f.nz = n.z;
        f.d = d;
        f.first = static_cast<int32_t>(out.faceVerts.size());
        f.count = 4;
        out.faces.push_back(f);
        for (int32_t k = 0; k < 4; ++k) {
            out.faceVerts.push_back(loop[k]);
        }
    }
    FinalizeHull(out);
}

// 同一平面の三角形を凸多角形の面へ統合する。
// 統合しないと箱の凸包が 12 枚の三角形のままになり、参照面クリップの接触点が 3 点で
// 頭打ちになる = box-box の 4 点マニフォールドと同じ安定性が出ない。
// グループ分けは「三角形 index 昇順に、条件を満たす**最初の**既存グループへ入れる」= 決定的。
// 凸包では同一平面の三角形は必ず連結なので、隣接を辿らず全体で突き合わせても結果は同じ
void MergeFaces(const std::vector<HullTri>& tris, float planeEps, float areaEps,
                ConvexHullData& h)
{
    // 約 0.57 度。64 頂点まで落とした球でも面同士は十数度離れるので誤統合しない
    constexpr float kMergeCos = 0.99995f;
    struct Group {
        V3 n;
        float d = 0;
        std::vector<int32_t> tris;
    };
    std::vector<Group> groups;
    for (int32_t ti = 0; ti < static_cast<int32_t>(tris.size()); ++ti) {
        const HullTri& t = tris[static_cast<size_t>(ti)];
        int32_t hit = -1;
        for (int32_t gi = 0; gi < static_cast<int32_t>(groups.size()); ++gi) {
            const Group& g = groups[static_cast<size_t>(gi)];
            if (Dot3(g.n, t.n) > kMergeCos && std::fabs(g.d - t.d) <= planeEps) {
                hit = gi;
                break;
            }
        }
        if (hit < 0) {
            Group g;
            g.n = t.n;
            g.d = t.d;
            g.tris.push_back(ti);
            groups.push_back(std::move(g));
        } else {
            groups[static_cast<size_t>(hit)].tris.push_back(ti);
        }
    }

    std::vector<std::pair<int32_t, int32_t>> boundary;
    std::vector<int32_t> loop;
    for (const Group& g : groups) {
        // 三角形 1 枚のグループはそのまま面にする (統合の必要がない)
        if (g.tris.size() > 1) {
            boundary.clear();
            for (int32_t ti : g.tris) {
                const HullTri& t = tris[static_cast<size_t>(ti)];
                for (int k = 0; k < 3; ++k) {
                    const int32_t a = t.v[k], b = t.v[(k + 1) % 3];
                    bool inner = false;
                    for (int32_t tj : g.tris) {
                        if (tj != ti && HasDirectedEdge(tris[static_cast<size_t>(tj)], b, a)) {
                            inner = true;
                            break;
                        }
                    }
                    if (!inner) {
                        boundary.emplace_back(a, b);
                    }
                }
            }
            if (ChainLoop(boundary, loop)) {
                // 一直線に並んだ頂点を落とす (凸包の面上に乗ってしまった点の掃除)。
                // 判定は元の隣接で行うので 1 パスで決まる = 決定的
                std::vector<int32_t> trimmed;
                const int32_t cnt = static_cast<int32_t>(loop.size());
                for (int32_t i = 0; i < cnt; ++i) {
                    const XMFLOAT3& pv = h.verts[static_cast<size_t>(loop[(i + cnt - 1) % cnt])];
                    const XMFLOAT3& cv = h.verts[static_cast<size_t>(loop[i])];
                    const XMFLOAT3& nv = h.verts[static_cast<size_t>(loop[(i + 1) % cnt])];
                    const V3 e1 = { cv.x - pv.x, cv.y - pv.y, cv.z - pv.z };
                    const V3 e2 = { nv.x - cv.x, nv.y - cv.y, nv.z - cv.z };
                    if (Len3(Cross3(e1, e2)) > areaEps) {
                        trimmed.push_back(loop[i]);
                    }
                }
                if (trimmed.size() >= 3) {
                    loop.swap(trimmed);
                }
                V3 n;
                float d;
                if (loop.size() >= 3
                    && NewellPlane(h.verts, loop.data(), static_cast<int32_t>(loop.size()),
                                   areaEps, n, d)) {
                    ConvexFace f;
                    f.nx = n.x;
                    f.ny = n.y;
                    f.nz = n.z;
                    f.d = d;
                    f.first = static_cast<int32_t>(h.faceVerts.size());
                    f.count = static_cast<int32_t>(loop.size());
                    h.faces.push_back(f);
                    h.faceVerts.insert(h.faceVerts.end(), loop.begin(), loop.end());
                    continue;
                }
            }
        }
        // 統合できなかったグループは三角形のまま出す (品質は落ちるが必ず閉じる)
        for (int32_t ti : g.tris) {
            const HullTri& t = tris[static_cast<size_t>(ti)];
            ConvexFace f;
            f.nx = t.n.x;
            f.ny = t.n.y;
            f.nz = t.n.z;
            f.d = t.d;
            f.first = static_cast<int32_t>(h.faceVerts.size());
            f.count = 3;
            h.faces.push_back(f);
            h.faceVerts.push_back(t.v[0]);
            h.faceVerts.push_back(t.v[1]);
            h.faceVerts.push_back(t.v[2]);
        }
    }
}

} // namespace

bool BuildConvexHull(const std::vector<XMFLOAT3>& points, ConvexHullData& out)
{
    out = ConvexHullData{};

    // 非有限値を落としつつ -0.0 を畳み、(座標, 元 index) で全順序ソート → 重複除去。
    // ここまでで「入力順への依存」を全部殺す
    std::vector<PointRef> pts;
    pts.reserve(points.size());
    for (size_t i = 0; i < points.size(); ++i) {
        const XMFLOAT3& v = points[i];
        if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) {
            continue;
        }
        PointRef r;
        r.p = { Zeroed(v.x), Zeroed(v.y), Zeroed(v.z) };
        r.idx = static_cast<int32_t>(i);
        pts.push_back(r);
    }
    std::sort(pts.begin(), pts.end(), PointLess);
    pts.erase(std::unique(pts.begin(), pts.end(),
                          [](const PointRef& a, const PointRef& b) {
                              return a.p.x == b.p.x && a.p.y == b.p.y && a.p.z == b.p.z;
                          }),
              pts.end());
    if (pts.empty()) {
        BuildBoxHull({ -0.5f, -0.5f, -0.5f }, { 0.5f, 0.5f, 0.5f }, 1e-4f, out);
        return false;
    }

    V3 lo = pts[0].p, hi = pts[0].p;
    for (const PointRef& r : pts) {
        lo.x = (r.p.x < lo.x) ? r.p.x : lo.x;
        lo.y = (r.p.y < lo.y) ? r.p.y : lo.y;
        lo.z = (r.p.z < lo.z) ? r.p.z : lo.z;
        hi.x = (r.p.x > hi.x) ? r.p.x : hi.x;
        hi.y = (r.p.y > hi.y) ? r.p.y : hi.y;
        hi.z = (r.p.z > hi.z) ? r.p.z : hi.z;
    }
    const float extent = std::max(hi.x - lo.x, std::max(hi.y - lo.y, hi.z - lo.z));
    // しきい値は必ず形状の広がりに比例させる (絶対値で書くと cm 単位のモデルと
    // km 単位の地形で意味が変わる)
    const float eps = std::max(extent * 1e-5f, 1e-7f);
    const float areaEps = std::max(extent * eps, 1e-12f);
    const float planeEps = std::max(extent * 1e-4f, 1e-6f);
    const float thick = std::max(extent * 1e-4f, 1e-6f);

    std::vector<HullTri> tris;
    if (!BuildTriHull(pts, eps, areaEps, tris)) {
        BuildBoxHull(lo, hi, thick, out);
        return false;
    }

    // 三角形が参照している頂点だけを (ソート順のまま) 詰め直す
    std::vector<int32_t> remap(pts.size(), -1);
    for (const HullTri& t : tris) {
        for (int k = 0; k < 3; ++k) {
            remap[static_cast<size_t>(t.v[k])] = 0;
        }
    }
    for (size_t i = 0; i < remap.size(); ++i) {
        if (remap[i] == 0) {
            remap[i] = static_cast<int32_t>(out.verts.size());
            out.verts.push_back({ pts[i].p.x, pts[i].p.y, pts[i].p.z });
        }
    }
    std::vector<HullTri> mapped = tris;
    for (HullTri& t : mapped) {
        for (int k = 0; k < 3; ++k) {
            t.v[k] = remap[static_cast<size_t>(t.v[k])];
        }
    }

    MergeFaces(mapped, planeEps, areaEps, out);
    FinalizeHull(out);

    // 妥当性検査: 重心が全面の裏側にあり、体積が正であること。
    // 壊れた凸包を返すくらいなら箱へ落ちるほうがデバッグできる
    bool sane = out.Valid();
    if (sane) {
        for (const ConvexFace& f : out.faces) {
            if (f.nx * out.com.x + f.ny * out.com.y + f.nz * out.com.z - f.d > planeEps) {
                sane = false;
                break;
            }
        }
    }
    if (!sane) {
        BuildBoxHull(lo, hi, thick, out);
        return false;
    }
    return true;
}

void ConvexMassProperties(const ConvexHullData& h, float sx, float sy, float sz, float& volume,
                          XMFLOAT3& com, float inertia[3][3])
{
    IntegrateMass(h, std::fabs(sx), std::fabs(sy), std::fabs(sz), volume, com, inertia);
}

int32_t ConvexSupportLocal(const ConvexHullData& h, float dx, float dy, float dz)
{
    int32_t best = 0;
    float bestDot = -std::numeric_limits<float>::max();
    for (int32_t i = 0; i < static_cast<int32_t>(h.verts.size()); ++i) {
        const XMFLOAT3& v = h.verts[static_cast<size_t>(i)];
        const float d = v.x * dx + v.y * dy + v.z * dz;
        if (d > bestDot) { // strict > = 同値は index 小
            bestDot = d;
            best = i;
        }
    }
    return best;
}

// ---- .mcvx blob ----
namespace {

void AppendBytes(std::vector<uint8_t>& buf, const void* src, size_t n)
{
    const uint8_t* b = static_cast<const uint8_t*>(src);
    buf.insert(buf.end(), b, b + n);
}
template <typename T> void AppendPod(std::vector<uint8_t>& buf, const T& v)
{
    AppendBytes(buf, &v, sizeof(T));
}

struct Reader {
    const uint8_t* p = nullptr;
    size_t size = 0;
    size_t pos = 0;

    bool Bytes(void* dst, size_t n)
    {
        if (pos + n > size || pos + n < pos) {
            return false;
        }
        std::memcpy(dst, p + pos, n);
        pos += n;
        return true;
    }
    template <typename T> bool Pod(T& v) { return Bytes(&v, sizeof(T)); }
};

constexpr uint32_t kHullBlobVersion = 1;
// 壊れた blob で巨大 allocate に走らないための上限 (正常値の遥か上)
constexpr uint32_t kBlobSanityCap = 1u << 20;

} // namespace

void SerializeConvexHull(const ConvexHullData& h, std::vector<uint8_t>& out)
{
    AppendPod(out, kHullBlobVersion);
    AppendPod(out, static_cast<uint32_t>(h.verts.size()));
    AppendBytes(out, h.verts.data(), h.verts.size() * sizeof(XMFLOAT3));
    AppendPod(out, static_cast<uint32_t>(h.faces.size()));
    AppendBytes(out, h.faces.data(), h.faces.size() * sizeof(ConvexFace));
    AppendPod(out, static_cast<uint32_t>(h.faceVerts.size()));
    AppendBytes(out, h.faceVerts.data(), h.faceVerts.size() * sizeof(int32_t));
    AppendPod(out, static_cast<uint32_t>(h.edges.size()));
    AppendBytes(out, h.edges.data(), h.edges.size() * sizeof(ConvexEdge));
    AppendPod(out, h.volume);
    AppendPod(out, h.com);
    AppendBytes(out, h.inertia, sizeof(h.inertia));
    AppendPod(out, h.aabbMin);
    AppendPod(out, h.aabbMax);
    AppendPod(out, h.boundRadius);
}

bool DeserializeConvexHull(const uint8_t* p, size_t size, size_t& pos, ConvexHullData& out)
{
    out = ConvexHullData{};
    Reader r{ p, size, pos };
    uint32_t version = 0;
    if (!r.Pod(version) || version != kHullBlobVersion) {
        return false;
    }
    auto readArray = [&r](auto& vec) {
        uint32_t n = 0;
        if (!r.Pod(n) || n > kBlobSanityCap) {
            return false;
        }
        vec.resize(n);
        return n == 0 || r.Bytes(vec.data(), static_cast<size_t>(n) * sizeof(vec[0]));
    };
    if (!readArray(out.verts) || !readArray(out.faces) || !readArray(out.faceVerts)
        || !readArray(out.edges)) {
        return false;
    }
    if (!r.Pod(out.volume) || !r.Pod(out.com) || !r.Bytes(out.inertia, sizeof(out.inertia))
        || !r.Pod(out.aabbMin) || !r.Pod(out.aabbMax) || !r.Pod(out.boundRadius)) {
        return false;
    }
    // 面の参照範囲だけは信用せずに検査する (壊れた blob で範囲外を舐めない)
    for (const ConvexFace& f : out.faces) {
        if (f.count < 3 || f.first < 0
            || static_cast<size_t>(f.first) + static_cast<size_t>(f.count) > out.faceVerts.size()) {
            return false;
        }
    }
    for (int32_t vi : out.faceVerts) {
        if (vi < 0 || static_cast<size_t>(vi) >= out.verts.size()) {
            return false;
        }
    }
    pos = r.pos;
    return true;
}

} // namespace mye
