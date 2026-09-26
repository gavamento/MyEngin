//====================================================================================
//                          FractureBake.cpp
//  MyEngin/ 秋田蓮音                                                     09/25/2026
//                                          Voronoi分割・凸包・接着グラフの実装
//====================================================================================
#include "Engine/Engine/Physics/FractureBake.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <numeric>
#include <set>
#include <utility>

#include "Engine/Core/Check.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Random.h"
#include "Engine/Engine/Physics/FractureVoxel.h"

using namespace DirectX;

namespace mye {
namespace {

// ---- 小さな 3D ベクトル演算 (FractureMesh.cpp / ConvexHull.cpp と同じ流儀。
// ファイルごとにローカルに持つのがこのコードベースの慣例) ----
struct V3 {
    float x = 0, y = 0, z = 0;
};
V3 Sub(const V3& a, const V3& b)
{
    return { a.x - b.x, a.y - b.y, a.z - b.z };
}
V3 Add(const V3& a, const V3& b)
{
    return { a.x + b.x, a.y + b.y, a.z + b.z };
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
XMFLOAT3 ToF3(const V3& a)
{
    return { a.x, a.y, a.z };
}
V3 FromF3(const XMFLOAT3& a)
{
    return { a.x, a.y, a.z };
}

// n が非零なら (t, b) を正規直交基底にし、t×b == n にする (Duff et al. 2017 の分岐無し構成。
// FractureMesh.cpp の OrthonormalBasis と同じ実装をここに複製する — 非公開ヘルパなのでこの
// ファイル群の既存の流儀に従う)
void OrthonormalBasis(const XMFLOAT3& n, XMFLOAT3& t, XMFLOAT3& b)
{
    const float sign = (n.z >= 0.0f) ? 1.0f : -1.0f;
    const float a = -1.0f / (sign + n.z);
    const float bxy = n.x * n.y * a;
    t = { 1.0f + sign * n.x * n.x * a, sign * bxy, -sign * n.x };
    b = { bxy, sign + n.y * n.y * a, -n.y };
}

bool PositionLess(const XMFLOAT3& a, const XMFLOAT3& b)
{
    if (a.x != b.x) {
        return a.x < b.x;
    }
    if (a.y != b.y) {
        return a.y < b.y;
    }
    return a.z < b.z;
}

// 位置がビット一致する点をまとめる (任意の重複除去)。パイプラインの各段階
// (外側面クリップ・断面クリップ) は三角形ごとに独立した頂点レコードを作るので、同じ位置が
// 何度も重複して残る。`BuildConvexHull` (このファイルの外、変更しない) は入力点数に対して
// 重い処理を含むため、幾何を変えずに (完全一致だけをまとめる、ε ではない) 入力点数を
// 減らしてから渡す
std::vector<XMFLOAT3> DedupPositionsExact(std::vector<XMFLOAT3> pts)
{
    std::sort(pts.begin(), pts.end(), PositionLess);
    pts.erase(std::unique(pts.begin(), pts.end(),
                          [](const XMFLOAT3& a, const XMFLOAT3& b) {
                              return a.x == b.x && a.y == b.y && a.z == b.z;
                          }),
             pts.end());
    return pts;
}

// 位置のビット一致での溶接 id (FractureMesh.cpp の WeldedIds と同じアルゴリズム。
// 非公開ヘルパなのでここに複製する — このファイル群の既存の流儀)
std::vector<int32_t> WeldedIds(const std::vector<FractureVertex>& verts)
{
    const size_t n = verts.size();
    std::vector<int32_t> order(n);
    for (size_t i = 0; i < n; ++i) {
        order[i] = static_cast<int32_t>(i);
    }
    std::sort(order.begin(), order.end(), [&](int32_t a, int32_t b) {
        const XMFLOAT3 &ka = verts[static_cast<size_t>(a)].position, &kb = verts[static_cast<size_t>(b)].position;
        if (PositionLess(ka, kb)) {
            return true;
        }
        if (PositionLess(kb, ka)) {
            return false;
        }
        return a < b;
    });
    std::vector<int32_t> weldId(n, 0);
    int32_t group = -1;
    XMFLOAT3 prevKey{};
    for (size_t k = 0; k < n; ++k) {
        const int32_t idx = order[k];
        const XMFLOAT3& kk = verts[static_cast<size_t>(idx)].position;
        if (group < 0 || kk.x != prevKey.x || kk.y != prevKey.y || kk.z != prevKey.z) {
            ++group;
            prevKey = kk;
        }
        weldId[static_cast<size_t>(idx)] = group;
    }
    return weldId;
}

// 決定的な union-find。タイブレーク (小さい根へ揃える) は処理順に依らない結果にするため
struct UnionFind {
    std::vector<int32_t> parent;
    explicit UnionFind(int32_t n) : parent(static_cast<size_t>(n))
    {
        for (int32_t i = 0; i < n; ++i) {
            parent[static_cast<size_t>(i)] = i;
        }
    }
    int32_t Find(int32_t x)
    {
        while (parent[static_cast<size_t>(x)] != x) {
            parent[static_cast<size_t>(x)] = parent[static_cast<size_t>(parent[static_cast<size_t>(x)])];
            x = parent[static_cast<size_t>(x)];
        }
        return x;
    }
    void Union(int32_t a, int32_t b)
    {
        a = Find(a);
        b = Find(b);
        if (a != b) {
            if (b < a) {
                std::swap(a, b);
            }
            parent[static_cast<size_t>(b)] = a;
        }
    }
};

XMFLOAT3 MeshAabbExtent(const FractureMesh& mesh, XMFLOAT3& outLo, XMFLOAT3& outHi)
{
    if (mesh.verts.empty()) {
        outLo = outHi = { 0, 0, 0 };
        return { 0, 0, 0 };
    }
    XMFLOAT3 lo = mesh.verts[0].position, hi = mesh.verts[0].position;
    for (const FractureVertex& v : mesh.verts) {
        lo.x = (std::min)(lo.x, v.position.x);
        lo.y = (std::min)(lo.y, v.position.y);
        lo.z = (std::min)(lo.z, v.position.z);
        hi.x = (std::max)(hi.x, v.position.x);
        hi.y = (std::max)(hi.y, v.position.y);
        hi.z = (std::max)(hi.z, v.position.z);
    }
    outLo = lo;
    outHi = hi;
    return { hi.x - lo.x, hi.y - lo.y, hi.z - lo.z };
}

// ---- 内部シードのレイパリティ内外判定 (+X 方向の軸平行レイ) ----
// 三角形の縁・頂点のごく近くをレイが通る (境界近傍のバリセントリック座標) 場合は
// 「あいまい」を返す。呼び出し側はその候補点を棄却してやり直す。これが
// 「縮退で頂点・辺を通る場合の扱いを固定する」規則 (棄却は決定的: 同じ候補点は毎回同じ判定になる)
enum class RayHit { Outside, Inside, Ambiguous };

RayHit RayParityInsideX(const FractureMesh& mesh, const XMFLOAT3& p, float extent)
{
    constexpr V3 dir{ 1, 0, 0 };
    const float baryEps = 1e-4f;
    const float tEps = (std::max)(extent * 1e-5f, 1e-6f);
    int32_t crossCount = 0;
    const int32_t triCount = mesh.TriCount();
    for (int32_t t = 0; t < triCount; ++t) {
        const V3 v0 = FromF3(mesh.verts[static_cast<size_t>(mesh.indices[static_cast<size_t>(t) * 3 + 0])].position);
        const V3 v1 = FromF3(mesh.verts[static_cast<size_t>(mesh.indices[static_cast<size_t>(t) * 3 + 1])].position);
        const V3 v2 = FromF3(mesh.verts[static_cast<size_t>(mesh.indices[static_cast<size_t>(t) * 3 + 2])].position);
        const V3 e1 = Sub(v1, v0);
        const V3 e2 = Sub(v2, v0);
        const float detEps = ((std::max)(Len3(e1), 1e-9f)) * ((std::max)(Len3(e2), 1e-9f)) * 1e-6f;
        const V3 pvec = Cross3(dir, e2);
        const float det = Dot3(e1, pvec);
        if (std::fabs(det) < detEps) {
            continue; // レイと三角形の平面がほぼ平行 (この三角形は横切らない)
        }
        const float invDet = 1.0f / det;
        const V3 tvec = Sub(FromF3(p), v0);
        const float u = Dot3(tvec, pvec) * invDet;
        if (u < -baryEps || u > 1.0f + baryEps) {
            continue;
        }
        const V3 qvec = Cross3(tvec, e1);
        const float v = Dot3(dir, qvec) * invDet;
        if (v < -baryEps || (u + v) > 1.0f + baryEps) {
            continue;
        }
        const float tt = Dot3(e2, qvec) * invDet;
        const bool nearBoundary = (std::fabs(u) < baryEps) || (std::fabs(u - 1.0f) < baryEps)
                                 || (std::fabs(v) < baryEps) || (std::fabs(u + v - 1.0f) < baryEps)
                                 || (std::fabs(tt) < tEps);
        if (nearBoundary) {
            return RayHit::Ambiguous;
        }
        if (tt > tEps) {
            ++crossCount;
        }
    }
    return (crossCount % 2 == 1) ? RayHit::Inside : RayHit::Outside;
}

// ---- 内部シード配置 ----
struct SeedPlacement {
    std::vector<XMFLOAT3> seeds;
    int32_t placed = 0;
};

SeedPlacement PlaceSeeds(const FractureMesh& mesh, uint32_t seed, int32_t pieceCount)
{
    SeedPlacement out;
    if (pieceCount <= 0) {
        return out;
    }
    XMFLOAT3 lo, hi;
    const XMFLOAT3 extentVec = MeshAabbExtent(mesh, lo, hi);
    const float extent = (std::max)(extentVec.x, (std::max)(extentVec.y, extentVec.z));
    if (!(extent > 0.0f)) {
        return out; // 潰れたメッシュ (点/縮退)
    }

    Pcg32 rng;
    rng.Seed(static_cast<uint64_t>(seed));
    const int32_t maxTrials = (std::max)(2000, pieceCount * 500);
    int32_t trials = 0;
    while (out.placed < pieceCount && trials < maxTrials) {
        ++trials;
        const XMFLOAT3 candidate{ rng.Range(lo.x, hi.x), rng.Range(lo.y, hi.y), rng.Range(lo.z, hi.z) };
        const RayHit hit = RayParityInsideX(mesh, candidate, extent);
        if (hit == RayHit::Inside) {
            out.seeds.push_back(candidate);
            ++out.placed;
        }
        // Outside / Ambiguous はどちらも「この候補は不採用」として引き続き次を試す
    }
    return out;
}

// ---- 平面ヘルパ ----
struct Plane {
    XMFLOAT3 normal{ 0, 1, 0 };
    float d = 0.0f;
};

bool Bisector(const XMFLOAT3& a, const XMFLOAT3& b, Plane& out)
{
    const V3 diff = Sub(FromF3(b), FromF3(a));
    const float len = Len3(diff);
    if (!(len > 1e-9f)) {
        return false; // ほぼ同一位置のシード (縮退) — この隣接は作らない
    }
    const V3 n = Mul3(diff, 1.0f / len);
    const V3 mid = Mul3(Add(FromF3(a), FromF3(b)), 0.5f);
    out.normal = ToF3(n);
    out.d = Dot3(n, mid);
    return true;
}

// ---- セル多面体専用の凸多面体クリッパ (面リスト表現) ----
// CutMeshByPlane で三角形スープを再三角形化しながら何度も切ると、蓋の輪郭が
// 頂点数十個の細片だらけになる (実測で確認済み: 8 シード目までに116 三角形/312 頂点まで
// 膨れた)。ここはセル自体が
// 「凸」だと分かっている特別な場合なので、Sutherland-Hodgman による面ごとのクリップ +
// 新しい面を「平面上の角度でソートする」凸順序付けに置き換える。三角形分割が要らないぶん
// 頑健で、このセル計算にしか使わない (実際のソースメッシュの切断は引き続き
// CutMeshByPlane を使う — 頂点/法線/UV を運ぶ必要があるのはそちら側だけ)
struct PolyFace {
    std::vector<XMFLOAT3> pts; // 外向き法線側から見て CCW
};

std::vector<PolyFace> MakeBoxFaces(const XMFLOAT3& center, const XMFLOAT3& half)
{
    const float hx = half.x, hy = half.y, hz = half.z;
    const float cx = center.x, cy = center.y, cz = center.z;
    const XMFLOAT3 p[8] = {
        { cx - hx, cy - hy, cz - hz }, { cx + hx, cy - hy, cz - hz }, { cx + hx, cy + hy, cz - hz },
        { cx - hx, cy + hy, cz - hz }, { cx - hx, cy - hy, cz + hz }, { cx + hx, cy - hy, cz + hz },
        { cx + hx, cy + hy, cz + hz }, { cx - hx, cy + hy, cz + hz },
    };
    std::vector<PolyFace> faces(6);
    faces[0].pts = { p[0], p[3], p[2], p[1] }; // -Z
    faces[1].pts = { p[4], p[5], p[6], p[7] }; // +Z
    faces[2].pts = { p[0], p[1], p[5], p[4] }; // -Y
    faces[3].pts = { p[3], p[7], p[6], p[2] }; // +Y
    faces[4].pts = { p[0], p[4], p[7], p[3] }; // -X
    faces[5].pts = { p[1], p[2], p[6], p[5] }; // +X
    return faces;
}

// 辺 (a,b) と平面 (n,d) の交点。辺の両端の位置の全順序で lo/hi を固定してから t を計算する
// ので、隣り合う 2 面がこの辺を逆向きに辿っても常にビット同一の交点になる
// (FractureMesh.cpp の ComputeCrossing と同じ理由)
XMFLOAT3 EdgePlaneCrossing(const XMFLOAT3& a, const XMFLOAT3& b, const XMFLOAT3& n, float d)
{
    const bool aIsLo = PositionLess(a, b);
    const XMFLOAT3 &lo = aIsLo ? a : b, &hi = aIsLo ? b : a;
    const float sLo = Dot3(FromF3(n), FromF3(lo)) - d;
    const float sHi = Dot3(FromF3(n), FromF3(hi)) - d;
    const float t = sLo / (sLo - sHi);
    return ToF3(Add(FromF3(lo), Mul3(Sub(FromF3(hi), FromF3(lo)), t)));
}

// 1 枚の凸多角形面を半空間 (n・x <= d を残す) で切る (Sutherland-Hodgman)。
// newPts には、この面から新しく生まれた (平面上に乗る) 交点を集める
std::vector<XMFLOAT3> ClipFaceKeepNegative(const std::vector<XMFLOAT3>& poly, const XMFLOAT3& n, float d,
                                           std::vector<XMFLOAT3>& newPts)
{
    std::vector<XMFLOAT3> out;
    const size_t cnt = poly.size();
    if (cnt == 0) {
        return out;
    }
    for (size_t i = 0; i < cnt; ++i) {
        const XMFLOAT3& cur = poly[i];
        const XMFLOAT3& nxt = poly[(i + 1) % cnt];
        const float sCur = Dot3(FromF3(n), FromF3(cur)) - d;
        const float sNext = Dot3(FromF3(n), FromF3(nxt)) - d;
        const bool curIn = sCur <= 0.0f;
        if (curIn) {
            out.push_back(cur);
        }
        if (curIn != (sNext <= 0.0f)) {
            const XMFLOAT3 cross = EdgePlaneCrossing(cur, nxt, n, d);
            out.push_back(cross);
            newPts.push_back(cross);
        }
    }
    return out;
}

// ---- セルの候補面 (実際にセルの境界になった二等分面だけ) ----
struct CandidatePlane {
    XMFLOAT3 normal{ 0, 1, 0 };
    float d = 0.0f;
    int32_t neighborSeed = -1;
};

void ComputeCandidatePlanes(int32_t seedIdx, const std::vector<XMFLOAT3>& seeds,
                            const std::vector<PolyFace>& box, std::vector<CandidatePlane>& out)
{
    out.clear();
    std::vector<PolyFace> cell = box;
    const int32_t n = static_cast<int32_t>(seeds.size());
    for (int32_t j = 0; j < n; ++j) {
        if (j == seedIdx) {
            continue;
        }
        Plane pl;
        if (!Bisector(seeds[static_cast<size_t>(seedIdx)], seeds[static_cast<size_t>(j)], pl)) {
            continue; // 縮退 (ほぼ同一位置のシード)
        }
        std::vector<XMFLOAT3> newPts;
        std::vector<PolyFace> clipped;
        clipped.reserve(cell.size());
        for (const PolyFace& f : cell) {
            std::vector<XMFLOAT3> c = ClipFaceKeepNegative(f.pts, pl.normal, pl.d, newPts);
            if (c.size() >= 3) {
                clipped.push_back({ std::move(c) });
            }
        }
        if (clipped.empty()) {
            // 自分自身の二等分面の負側には必ず seedIdx 自身が入るはずなので、通常は起きない。
            // 安全側フォールバック: この平面は無視してセルを変えずに次へ進む
            MYE_LOG_WARN("FractureBake: cell for seed %d vanished while clipping against seed %d (skipped)",
                        seedIdx, j);
            continue;
        }
        if (!newPts.empty()) {
            // 新しい交点群 (平面上、凸) を重心まわりの角度でソートして 1 枚の凸面にする。
            // 交点は隣接面から重複して来ることがあるので位置で束ねてから並べる
            std::sort(newPts.begin(), newPts.end(), PositionLess);
            newPts.erase(std::unique(newPts.begin(), newPts.end(),
                                     [](const XMFLOAT3& a, const XMFLOAT3& b) {
                                         return a.x == b.x && a.y == b.y && a.z == b.z;
                                     }),
                        newPts.end());
            if (newPts.size() >= 3) {
                XMFLOAT3 tangent, bitangent;
                OrthonormalBasis(pl.normal, tangent, bitangent);
                V3 centroid{ 0, 0, 0 };
                for (const XMFLOAT3& p : newPts) {
                    centroid = Add(centroid, FromF3(p));
                }
                centroid = Mul3(centroid, 1.0f / static_cast<float>(newPts.size()));
                std::sort(newPts.begin(), newPts.end(), [&](const XMFLOAT3& a, const XMFLOAT3& b) {
                    const V3 da = Sub(FromF3(a), centroid), db = Sub(FromF3(b), centroid);
                    const float angA = std::atan2(Dot3(da, FromF3(bitangent)), Dot3(da, FromF3(tangent)));
                    const float angB = std::atan2(Dot3(db, FromF3(bitangent)), Dot3(db, FromF3(tangent)));
                    return angA < angB;
                });
                // 新しい面の外向き法線 = pl.normal (負側を残す = 正側を取り除いた断面の
                // 外向きは +pl.normal 方向)。tangent × bitangent == pl.normal になるよう
                // OrthonormalBasis を作ってあるので、角度昇順 (反時計) がそのまま CCW になる
                clipped.push_back({ newPts });
            }
            // この平面が実際にセルへ食い込んだ (交点が生まれた) ときだけ、ソースメッシュを
            // 切る候補にする。触れなかった平面は完全に冗長 (セルはすでにその内側) なので、
            // 除外しておくとソースメッシュ側の切断回数を最小限に抑えられる
            out.push_back({ pl.normal, pl.d, j });
        }
        cell = std::move(clipped);
    }
}

// ---- ソースメッシュを候補面で順に切り、最終三角形を面の由来で分類する ----
struct RawPiece {
    int32_t originSeed = -1;
    FractureMesh mesh;             // outer+cap 混在 (ワールド空間)
    std::vector<int32_t> triTag;   // 三角形ごと: -1 = 元の外側面、それ以外 = 隣接シード index
    double volume = 0.0;
    XMFLOAT3 centroid{ 0, 0, 0 };
    XMFLOAT3 sortKeyPos{ 0, 0, 0 }; // 決定的な並び替え用 (このピースの最小頂点位置)
};

// FractureVertex の線形補間 (FractureMesh.cpp の LerpVertex と同じ規則)
FractureVertex LerpFractureVertex(const FractureVertex& a, const FractureVertex& b, float t)
{
    FractureVertex out;
    out.position = ToF3(Add(FromF3(a.position), Mul3(Sub(FromF3(b.position), FromF3(a.position)), t)));
    V3 nrm = Add(FromF3(a.normal), Mul3(Sub(FromF3(b.normal), FromF3(a.normal)), t));
    const float nlen = Len3(nrm);
    out.normal = (nlen > 1e-12f) ? ToF3(Mul3(nrm, 1.0f / nlen)) : a.normal;
    out.uv = { a.uv.x + t * (b.uv.x - a.uv.x), a.uv.y + t * (b.uv.y - a.uv.y) };
    return out;
}

// 辺 (curr→next) と平面 (n,d) の交点。辺の両端の位置の全順序で lo/hi を固定してから t を
// 計算するので、この辺を逆向きに辿る側 (隣り合う三角形) でも常にビット同一の交点になる
// (FractureMesh.cpp の ComputeCrossing と同じ理由)
FractureVertex CrossFractureVertex(const FractureVertex& curr, float sCurr, const FractureVertex& next,
                                   float sNext)
{
    const bool currIsLo = PositionLess(curr.position, next.position);
    const FractureVertex& lo = currIsLo ? curr : next;
    const FractureVertex& hi = currIsLo ? next : curr;
    const float sLo = currIsLo ? sCurr : sNext;
    const float sHi = currIsLo ? sNext : sCurr;
    const float t = sLo / (sLo - sHi);
    return LerpFractureVertex(lo, hi, t);
}

// 三角形単位の平面クリップ (Sutherland-Hodgman、負側 n・x<=d を残す)。
// 凸多角形を半空間で切った結果は必ず凸多角形になるので、断面の三角形分割が要らず
// 頑健 — CutMeshByPlane をソースメッシュへ何度も連続適用すると (実測で確認済み) 断面が
// 数百頂点まで膨れて分割が重くなることがあった。外側面はこの関数で元メッシュの三角形を
// 自分の候補面だけで直接クリップする。蓋は「対ごとに CutMeshByPlane で 1 回だけ
// 切った断面」を、この関数でさらに他の候補面すべてにかけて絞り込む
// (`TriangleClipMesh`/`ProcessAdjacentPair`)
std::vector<FractureVertex> ClipTriKeepNegative(const std::vector<FractureVertex>& poly, const XMFLOAT3& n,
                                                float d)
{
    std::vector<FractureVertex> out;
    const size_t cnt = poly.size();
    if (cnt == 0) {
        return out;
    }
    for (size_t i = 0; i < cnt; ++i) {
        const FractureVertex& cur = poly[i];
        const FractureVertex& nxt = poly[(i + 1) % cnt];
        const float sCur = Dot3(FromF3(n), FromF3(cur.position)) - d;
        const float sNext = Dot3(FromF3(n), FromF3(nxt.position)) - d;
        const bool curIn = sCur <= 0.0f;
        if (curIn) {
            out.push_back(cur);
        }
        if (curIn != (sNext <= 0.0f)) {
            out.push_back(CrossFractureVertex(cur, sCur, nxt, sNext));
        }
    }
    return out;
}

// ---- ソースメッシュを候補面で切る (三角形単位クリップ + 平面ごとの蓋組み立て) ----
// 各ソース三角形を独立に (直前の切断結果を引きずらずに) 候補面すべてで順にクリップするので、
// CutMeshByPlane をメッシュ全体へ繰り返し適用したときのような複雑さの積み上がりが起きない。
// 出力メッシュの三角形ごとの隣接タグ (-1=外側面、それ以外=隣接シード index) も、由来がそのまま
// 分かっているのでこの場で決定的に振れる (後から平面と突き合わせる分類は不要)
// 三角形の集まりを候補面 skipIdx 以外の全平面で三角形単位クリップし、生き残った断片を
// ファン分割して outMesh/triTag (すべて同じ tag) へ追加する
void ClipTrianglesAndAppend(const FractureMesh& src, const std::vector<CandidatePlane>& planes, size_t skipIdx,
                            int32_t tag, FractureMesh& outMesh, std::vector<int32_t>& triTag)
{
    const int32_t triCount = src.TriCount();
    for (int32_t t = 0; t < triCount; ++t) {
        std::vector<FractureVertex> frag
            = { src.verts[static_cast<size_t>(src.indices[static_cast<size_t>(t) * 3 + 0])],
               src.verts[static_cast<size_t>(src.indices[static_cast<size_t>(t) * 3 + 1])],
               src.verts[static_cast<size_t>(src.indices[static_cast<size_t>(t) * 3 + 2])] };
        for (size_t p = 0; p < planes.size() && frag.size() >= 3; ++p) {
            if (p == skipIdx) {
                continue;
            }
            frag = ClipTriKeepNegative(frag, planes[p].normal, planes[p].d);
        }
        if (frag.size() < 3) {
            continue;
        }
        const int32_t base = static_cast<int32_t>(outMesh.verts.size());
        outMesh.verts.insert(outMesh.verts.end(), frag.begin(), frag.end());
        for (size_t k = 1; k + 1 < frag.size(); ++k) {
            outMesh.indices.push_back(base);
            outMesh.indices.push_back(base + static_cast<int32_t>(k));
            outMesh.indices.push_back(base + static_cast<int32_t>(k) + 1);
            triTag.push_back(tag);
        }
    }
    // 三角形数を変える処理のたびに triTag の本数がずれていないか確認する
    MYE_CHECK(triTag.size() == static_cast<size_t>(outMesh.TriCount()));
}

// 平面ペア表現 (neighborSeed は持たない、外側面クリップの
// 「他の候補面すべて」用の軽量な (n,d) だけの平面)
struct Plane2 {
    XMFLOAT3 normal{ 0, 1, 0 };
    float d = 0.0f;
};

// mesh の全三角形を、与えた平面すべてで順に三角形単位クリップする (タグ付けなし)。
// 断面 (蓋) の絞り込み専用 — 蓋メッシュは候補面で切った元メッシュの断面 1 つぶんなので、
// 対の数だけ呼んでも全体のコストは元メッシュ全体を何度も切るより十分小さい
FractureMesh TriangleClipMesh(const FractureMesh& mesh, const std::vector<Plane2>& planes)
{
    FractureMesh out;
    const int32_t triCount = mesh.TriCount();
    for (int32_t t = 0; t < triCount; ++t) {
        std::vector<FractureVertex> frag
            = { mesh.verts[static_cast<size_t>(mesh.indices[static_cast<size_t>(t) * 3 + 0])],
               mesh.verts[static_cast<size_t>(mesh.indices[static_cast<size_t>(t) * 3 + 1])],
               mesh.verts[static_cast<size_t>(mesh.indices[static_cast<size_t>(t) * 3 + 2])] };
        for (const Plane2& p : planes) {
            if (frag.size() < 3) {
                break;
            }
            frag = ClipTriKeepNegative(frag, p.normal, p.d);
        }
        if (frag.size() < 3) {
            continue;
        }
        const int32_t base = static_cast<int32_t>(out.verts.size());
        out.verts.insert(out.verts.end(), frag.begin(), frag.end());
        for (size_t k = 1; k + 1 < frag.size(); ++k) {
            out.indices.push_back(base);
            out.indices.push_back(base + static_cast<int32_t>(k));
            out.indices.push_back(base + static_cast<int32_t>(k) + 1);
        }
    }
    return out;
}

// 三角形の巻きを反転し (index 1,2 を入れ替え)、法線を反転したコピーを返す。
// 対 (i,j) の断面は i 側で 1 回だけ作り、j 側はこの反転コピーをそのまま使う
// (両側でジオメトリが完全に同じ入力から作られるので、位置は常にビット同一。
// 丸めの違いは仕上げの ε 溶接で吸収する)
FractureMesh ReverseWindingNegateNormal(const FractureMesh& mesh)
{
    FractureMesh out = mesh;
    for (FractureVertex& v : out.verts) {
        v.normal = { -v.normal.x, -v.normal.y, -v.normal.z };
    }
    for (size_t t = 0; t + 2 < out.indices.size(); t += 3) {
        std::swap(out.indices[t + 1], out.indices[t + 2]);
    }
    return out;
}

void AppendTagged(FractureMesh& dst, std::vector<int32_t>& dstTag, const FractureMesh& src, int32_t tag)
{
    const int32_t base = static_cast<int32_t>(dst.verts.size());
    dst.verts.insert(dst.verts.end(), src.verts.begin(), src.verts.end());
    for (int32_t idx : src.indices) {
        dst.indices.push_back(idx + base);
    }
    dstTag.insert(dstTag.end(), static_cast<size_t>(src.TriCount()), tag);
    // 三角形数を変える処理のたびに triTag の本数がずれていないか確認する
    MYE_CHECK(dstTag.size() == static_cast<size_t>(dst.TriCount()));
}

// 対 (iSide, jSide) の断面を 1 回だけ作り、両側の破片メッシュへタグ付きで追加する。
// P (n,d) は iSide が常に負側になるよう計算済みの二等分面 (Bisector(seeds[iSide],seeds[jSide]))。
// 元メッシュは閉じていて外向きなので CutMeshByPlane の前提を満たす (検証済みの経路)。
// 得た断面を「iSide と jSide の他の候補面の和集合」で三角形単位クリップして
// F_ij = P ∩ セルiSide ∩ セルjSide まで絞り込む
void ProcessAdjacentPair(int32_t iSide, int32_t jSide, const XMFLOAT3& n, float d,
                         const std::vector<CandidatePlane>& candidatesI,
                         const std::vector<CandidatePlane>& candidatesJ, const FractureMesh& source,
                         std::vector<FractureMesh>& seedMesh, std::vector<std::vector<int32_t>>& seedTriTag)
{
    PlaneCutResult r;
    if (!CutMeshByPlane(source, n, d, r)) {
        MYE_LOG_WARN("FractureBake: single-plane cut for pair (%d,%d) failed (%s) — no cap for it", iSide,
                    jSide, r.failReason.c_str());
        return;
    }
    if (r.negative.cap.TriCount() == 0) {
        return; // 平面が元メッシュに触れない (このソースメッシュ形状では隣接しない)
    }
    std::vector<Plane2> others;
    others.reserve(candidatesI.size() + candidatesJ.size());
    for (const CandidatePlane& c : candidatesI) {
        if (c.neighborSeed != jSide) {
            others.push_back({ c.normal, c.d });
        }
    }
    for (const CandidatePlane& c : candidatesJ) {
        if (c.neighborSeed != iSide) {
            others.push_back({ c.normal, c.d });
        }
    }
    const FractureMesh capRefined = TriangleClipMesh(r.negative.cap, others);
    if (capRefined.TriCount() == 0) {
        return; // 絞り込みで消えた = 実際には隣接しない (候補は冗長だった)
    }
    AppendTagged(seedMesh[static_cast<size_t>(iSide)], seedTriTag[static_cast<size_t>(iSide)], capRefined,
                jSide);
    const FractureMesh capForJ = ReverseWindingNegateNormal(capRefined);
    AppendTagged(seedMesh[static_cast<size_t>(jSide)], seedTriTag[static_cast<size_t>(jSide)], capForJ,
                iSide);
}

// ---- 連結成分分離 (位相的な閉じを求めず、頂点の近さでつなぐ) ----
// 破片は外側面と蓋を別々に作るので、継ぎ目の頂点は位置がビット一致しない (数 ulp ずれる)。
// ε = 元メッシュの AABB 対角 × kProximityEpsRelative の近さで頂点をまとめ、同じ組を持つ
// 三角形どうしを連結とみなす (辺の完全一致は求めない)
inline constexpr float kProximityEpsRelative = 1e-6f;

// 頂点位置を辞書順にソートし、x 方向の掃き出しで 3 軸とも ε 以内の組を union-find で結ぶ。
// 根は Find の実装 (小さい index へ寄せる) により決定的。戻り値は頂点ごとの根 index
std::vector<int32_t> WeldByProximity(const std::vector<FractureVertex>& verts, float eps)
{
    const size_t n = verts.size();
    std::vector<int32_t> order(n);
    for (size_t i = 0; i < n; ++i) {
        order[i] = static_cast<int32_t>(i);
    }
    std::sort(order.begin(), order.end(), [&](int32_t a, int32_t b) {
        const XMFLOAT3 &pa = verts[static_cast<size_t>(a)].position, &pb = verts[static_cast<size_t>(b)].position;
        if (PositionLess(pa, pb)) {
            return true;
        }
        if (PositionLess(pb, pa)) {
            return false;
        }
        return a < b;
    });
    UnionFind uf(static_cast<int32_t>(n));
    for (size_t i = 0; i < n; ++i) {
        const XMFLOAT3& pi = verts[static_cast<size_t>(order[i])].position;
        for (size_t j = i + 1; j < n; ++j) {
            const XMFLOAT3& pj = verts[static_cast<size_t>(order[j])].position;
            if (pj.x - pi.x > eps) {
                break; // x でソート済みなので、これ以降も超える
            }
            if (std::fabs(pj.y - pi.y) <= eps && std::fabs(pj.z - pi.z) <= eps) {
                uf.Union(order[static_cast<size_t>(i)], order[static_cast<size_t>(j)]);
            }
        }
    }
    std::vector<int32_t> group(n);
    for (size_t i = 0; i < n; ++i) {
        group[i] = uf.Find(static_cast<int32_t>(i));
    }
    return group;
}

struct Component {
    FractureMesh mesh;
    std::vector<int32_t> triTag;
};

// 頂点の近さ (eps) でつないだ三角形どうしを 1 つの破片とする。ある三角形が別の三角形と
// (どれか 1 頂点でも) 同じ近接グループを共有していれば連結とみなす — 蓋どうしの継ぎ目は
// ビット一致しないので、辺 (2 頂点一致) ではなく頂点単位の共有で判定する
std::vector<Component> SplitConnectedComponents(const FractureMesh& mesh, const std::vector<int32_t>& triTag,
                                                float eps)
{
    const int32_t triCount = mesh.TriCount();
    std::vector<Component> result;
    if (triCount == 0) {
        return result;
    }
    const std::vector<int32_t> group = WeldByProximity(mesh.verts, eps);
    std::map<int32_t, std::vector<int32_t>> groupTris; // 近接グループ根 -> 三角形 index 列
    for (int32_t t = 0; t < triCount; ++t) {
        for (int k = 0; k < 3; ++k) {
            const int32_t raw = mesh.indices[static_cast<size_t>(t) * 3 + static_cast<size_t>(k)];
            groupTris[group[static_cast<size_t>(raw)]].push_back(t);
        }
    }
    UnionFind uf(triCount);
    for (const auto& [g, tris] : groupTris) {
        for (size_t i = 1; i < tris.size(); ++i) {
            uf.Union(tris[0], tris[static_cast<size_t>(i)]);
        }
    }

    std::map<int32_t, std::vector<int32_t>> groups; // root -> 三角形 index 列 (昇順)
    for (int32_t t = 0; t < triCount; ++t) {
        groups[uf.Find(t)].push_back(t);
    }
    for (auto& [root, tris] : groups) {
        Component comp;
        std::map<int32_t, int32_t> remap; // 元頂点 index -> 新頂点 index
        for (int32_t t : tris) {
            std::array<int32_t, 3> newIdx{};
            for (int k = 0; k < 3; ++k) {
                const int32_t orig = mesh.indices[static_cast<size_t>(t) * 3 + static_cast<size_t>(k)];
                const auto it = remap.find(orig);
                if (it == remap.end()) {
                    const int32_t ni = static_cast<int32_t>(comp.mesh.verts.size());
                    comp.mesh.verts.push_back(mesh.verts[static_cast<size_t>(orig)]);
                    remap.emplace(orig, ni);
                    newIdx[static_cast<size_t>(k)] = ni;
                } else {
                    newIdx[static_cast<size_t>(k)] = it->second;
                }
            }
            comp.mesh.indices.push_back(newIdx[0]);
            comp.mesh.indices.push_back(newIdx[1]);
            comp.mesh.indices.push_back(newIdx[2]);
            comp.triTag.push_back(triTag[static_cast<size_t>(t)]);
        }
        result.push_back(std::move(comp));
    }
    return result;
}

// 破片の合否判定: 位相的な閉じは求めず、(a) 体積 > 0、(b) ベクトル面積の和
// |Σ(b-a)×(c-a)/2| が表面積 Σ|(b-a)×(c-a)|/2 の 1e-4 以下 (蓋が欠けていないこと) で判定する。
// 閉じたメッシュならベクトル面積は正確に 0 (各面の寄与が打ち消し合う)。面が欠けていると
// 打ち消し損ねた分だけ非零になるので、表面積に対する相対値で「欠けの大きさ」を測れる
// 発散定理による体積と体積重心 (原点基準の四面体分割)。閉じたメッシュでのみ意味を持つ
void ComputeVolumeCentroid(const FractureMesh& mesh, double& volumeOut, XMFLOAT3& centroidOut)
{
    double vol = 0.0;
    double cx = 0.0, cy = 0.0, cz = 0.0;
    const int32_t triCount = mesh.TriCount();
    for (int32_t t = 0; t < triCount; ++t) {
        const XMFLOAT3& p0 = mesh.verts[static_cast<size_t>(mesh.indices[static_cast<size_t>(t) * 3 + 0])].position;
        const XMFLOAT3& p1 = mesh.verts[static_cast<size_t>(mesh.indices[static_cast<size_t>(t) * 3 + 1])].position;
        const XMFLOAT3& p2 = mesh.verts[static_cast<size_t>(mesh.indices[static_cast<size_t>(t) * 3 + 2])].position;
        const double v_t = (static_cast<double>(p0.x) * (static_cast<double>(p1.y) * p2.z - static_cast<double>(p1.z) * p2.y)
                            - static_cast<double>(p0.y) * (static_cast<double>(p1.x) * p2.z - static_cast<double>(p1.z) * p2.x)
                            + static_cast<double>(p0.z) * (static_cast<double>(p1.x) * p2.y - static_cast<double>(p1.y) * p2.x))
                          / 6.0;
        vol += v_t;
        const double tcx = (static_cast<double>(p0.x) + p1.x + p2.x) / 4.0;
        const double tcy = (static_cast<double>(p0.y) + p1.y + p2.y) / 4.0;
        const double tcz = (static_cast<double>(p0.z) + p1.z + p2.z) / 4.0;
        cx += v_t * tcx;
        cy += v_t * tcy;
        cz += v_t * tcz;
    }
    volumeOut = vol;
    if (std::fabs(vol) > 1e-15) {
        centroidOut = { static_cast<float>(cx / vol), static_cast<float>(cy / vol), static_cast<float>(cz / vol) };
    } else {
        centroidOut = { 0, 0, 0 };
    }
}

// 破片の合否判定: 位相的な閉じは求めず、(a) 体積 > 0、(b) ベクトル面積の和
// |Σ(b-a)×(c-a)/2| が表面積 Σ|(b-a)×(c-a)|/2 の 1e-4 以下 (蓋が欠けていないこと) で判定する。
// 閉じたメッシュならベクトル面積は正確に 0 (各面の寄与が打ち消し合う)。面が欠けていると
// 打ち消し損ねた分だけ非零になるので、表面積に対する相対値で「欠けの大きさ」を測れる
struct PieceValidity {
    bool valid = false;
    double volume = 0.0;
    double vectorAreaMag = 0.0;
    double surfaceArea = 0.0;
};

PieceValidity ValidatePieceGeometry(const FractureMesh& mesh)
{
    PieceValidity r;
    double vx = 0.0, vy = 0.0, vz = 0.0;
    const int32_t triCount = mesh.TriCount();
    for (int32_t t = 0; t < triCount; ++t) {
        const XMFLOAT3& a = mesh.verts[static_cast<size_t>(mesh.indices[static_cast<size_t>(t) * 3 + 0])].position;
        const XMFLOAT3& b = mesh.verts[static_cast<size_t>(mesh.indices[static_cast<size_t>(t) * 3 + 1])].position;
        const XMFLOAT3& c = mesh.verts[static_cast<size_t>(mesh.indices[static_cast<size_t>(t) * 3 + 2])].position;
        const double e1x = static_cast<double>(b.x) - a.x, e1y = static_cast<double>(b.y) - a.y,
                    e1z = static_cast<double>(b.z) - a.z;
        const double e2x = static_cast<double>(c.x) - a.x, e2y = static_cast<double>(c.y) - a.y,
                    e2z = static_cast<double>(c.z) - a.z;
        const double cx = e1y * e2z - e1z * e2y, cy = e1z * e2x - e1x * e2z, cz = e1x * e2y - e1y * e2x;
        vx += cx * 0.5;
        vy += cy * 0.5;
        vz += cz * 0.5;
        r.surfaceArea += 0.5 * std::sqrt(cx * cx + cy * cy + cz * cz);
    }
    r.vectorAreaMag = std::sqrt(vx * vx + vy * vy + vz * vz);
    double vol = 0.0;
    XMFLOAT3 centroidUnused{};
    ComputeVolumeCentroid(mesh, vol, centroidUnused);
    r.volume = vol;
    r.valid = (vol > 0.0) && (r.vectorAreaMag <= 1e-4 * r.surfaceArea);
    return r;
}

XMFLOAT3 MinVertexPosition(const FractureMesh& mesh)
{
    XMFLOAT3 best{ 0, 0, 0 };
    bool has = false;
    for (const FractureVertex& v : mesh.verts) {
        if (!has || PositionLess(v.position, best)) {
            best = v.position;
            has = true;
        }
    }
    return best;
}

// ---- ピース間の接着 (面積) グラフ ----
using AdjMap = std::map<int32_t, float>; // 相手ピース index -> 面積

// seed タグ由来の隣接候補を、実ピース同士のペアへ対応付ける。片側だけに複数の成分がある
// まれなケースは、面積の大きい順に貪欲マッチングする (spec は方法をコード側の判断に委ねている)
void ResolvePieceAdjacency(const std::vector<RawPiece>& pieces, std::vector<AdjMap>& adjOut)
{
    const int32_t n = static_cast<int32_t>(pieces.size());
    adjOut.assign(static_cast<size_t>(n), AdjMap{});

    // (シードi, シードj) の無向ペアごとに、i 側 (originSeed==i かつタグ j) のピースと
    // j 側 (originSeed==j かつタグ i) のピースを集める
    std::map<std::pair<int32_t, int32_t>, std::vector<std::pair<int32_t, float>>> sideA, sideB;
    for (int32_t p = 0; p < n; ++p) {
        const RawPiece& rp = pieces[static_cast<size_t>(p)];
        std::map<int32_t, float> areaByTag;
        const int32_t triCount = rp.mesh.TriCount();
        for (int32_t t = 0; t < triCount; ++t) {
            const int32_t tag = rp.triTag[static_cast<size_t>(t)];
            if (tag < 0) {
                continue;
            }
            const XMFLOAT3& p0 = rp.mesh.verts[static_cast<size_t>(rp.mesh.indices[static_cast<size_t>(t) * 3 + 0])].position;
            const XMFLOAT3& p1 = rp.mesh.verts[static_cast<size_t>(rp.mesh.indices[static_cast<size_t>(t) * 3 + 1])].position;
            const XMFLOAT3& p2 = rp.mesh.verts[static_cast<size_t>(rp.mesh.indices[static_cast<size_t>(t) * 3 + 2])].position;
            const V3 cr = Cross3(Sub(FromF3(p1), FromF3(p0)), Sub(FromF3(p2), FromF3(p0)));
            areaByTag[tag] += 0.5f * Len3(cr);
        }
        for (const auto& [neighborSeed, area] : areaByTag) {
            const int32_t lo = (std::min)(rp.originSeed, neighborSeed);
            const int32_t hi = (std::max)(rp.originSeed, neighborSeed);
            if (rp.originSeed == lo) {
                sideA[{ lo, hi }].push_back({ p, area });
            } else {
                sideB[{ lo, hi }].push_back({ p, area });
            }
        }
    }

    for (auto& [key, listARaw] : sideA) {
        auto itB = sideB.find(key);
        if (itB == sideB.end()) {
            continue; // 対応する反対側が見つからない (統合等でタグが消えた場合)
        }
        std::vector<std::pair<int32_t, float>> listA = listARaw;
        std::vector<std::pair<int32_t, float>> listB = itB->second;
        // 面積の大きい順、同値はピース index 昇順で安定ソートしてから先頭同士を組にする
        auto byAreaDesc = [](const std::pair<int32_t, float>& a, const std::pair<int32_t, float>& b) {
            if (a.second != b.second) {
                return a.second > b.second;
            }
            return a.first < b.first;
        };
        std::sort(listA.begin(), listA.end(), byAreaDesc);
        std::sort(listB.begin(), listB.end(), byAreaDesc);
        const size_t m = (std::min)(listA.size(), listB.size());
        for (size_t i = 0; i < m; ++i) {
            const int32_t pa = listA[i].first, pb = listB[i].first;
            const float area = 0.5f * (listA[i].second + listB[i].second);
            adjOut[static_cast<size_t>(pa)][pb] += area;
            adjOut[static_cast<size_t>(pb)][pa] += area;
        }
    }
}

// small を target へ統合する (ジオメトリ結合・体積再計算・隣接の付け替え)
void MergePieceInto(std::vector<RawPiece>& pieces, std::vector<AdjMap>& adj, int32_t small, int32_t target)
{
    RawPiece& t = pieces[static_cast<size_t>(target)];
    const RawPiece& s = pieces[static_cast<size_t>(small)];
    const int32_t base = static_cast<int32_t>(t.mesh.verts.size());
    t.mesh.verts.insert(t.mesh.verts.end(), s.mesh.verts.begin(), s.mesh.verts.end());
    for (int32_t idx : s.mesh.indices) {
        t.mesh.indices.push_back(idx + base);
    }
    t.triTag.insert(t.triTag.end(), s.triTag.begin(), s.triTag.end());
    // 三角形数を変える処理のたびに triTag の本数がずれていないか確認する
    // (SplitTJunctions のタグ更新漏れが未定義動作の原因になった不具合の再発防止)
    MYE_CHECK(t.triTag.size() == static_cast<size_t>(t.mesh.TriCount()));
    ComputeVolumeCentroid(t.mesh, t.volume, t.centroid);
    if (PositionLess(s.sortKeyPos, t.sortKeyPos)) {
        t.sortKeyPos = s.sortKeyPos;
    }
    if (s.originSeed < t.originSeed) {
        t.originSeed = s.originSeed;
    }

    for (const auto& [otherIdx, area] : adj[static_cast<size_t>(small)]) {
        if (otherIdx == target) {
            continue; // small<->target の境界は内部化して消える
        }
        adj[static_cast<size_t>(target)][otherIdx] += area;
        adj[static_cast<size_t>(otherIdx)].erase(small);
        adj[static_cast<size_t>(otherIdx)][target] += area;
    }
    adj[static_cast<size_t>(target)].erase(small);
    adj[static_cast<size_t>(small)].clear();
}

// small を配列から取り除き、pieces / adj のインデックスを詰める
void RemovePiece(std::vector<RawPiece>& pieces, std::vector<AdjMap>& adj, int32_t small)
{
    const int32_t n = static_cast<int32_t>(pieces.size());
    std::vector<int32_t> remap(static_cast<size_t>(n), -1);
    int32_t next = 0;
    for (int32_t i = 0; i < n; ++i) {
        if (i == small) {
            continue;
        }
        remap[static_cast<size_t>(i)] = next++;
    }
    std::vector<RawPiece> newPieces;
    std::vector<AdjMap> newAdj;
    newPieces.reserve(static_cast<size_t>(next));
    newAdj.reserve(static_cast<size_t>(next));
    for (int32_t i = 0; i < n; ++i) {
        if (i == small) {
            continue;
        }
        newPieces.push_back(std::move(pieces[static_cast<size_t>(i)]));
        AdjMap remapped;
        for (const auto& [otherIdx, area] : adj[static_cast<size_t>(i)]) {
            const int32_t ni = remap[static_cast<size_t>(otherIdx)];
            if (ni >= 0) {
                remapped[ni] += area;
            }
        }
        newAdj.push_back(std::move(remapped));
    }
    pieces = std::move(newPieces);
    adj = std::move(newAdj);
}

// 三角形を tag (-1 or 隣接シード) で outer / cap の 2 本へ振り分ける (index は詰め直す)
void SplitOuterCap(const FractureMesh& mesh, const std::vector<int32_t>& triTag, FractureMesh& outer,
                   FractureMesh& cap)
{
    outer = FractureMesh{};
    cap = FractureMesh{};
    const int32_t triCount = mesh.TriCount();
    for (int32_t t = 0; t < triCount; ++t) {
        FractureMesh& dst = (triTag[static_cast<size_t>(t)] < 0) ? outer : cap;
        const int32_t base = static_cast<int32_t>(dst.verts.size());
        for (int k = 0; k < 3; ++k) {
            dst.verts.push_back(mesh.verts[static_cast<size_t>(mesh.indices[static_cast<size_t>(t) * 3 + static_cast<size_t>(k)])]);
        }
        dst.indices.push_back(base + 0);
        dst.indices.push_back(base + 1);
        dst.indices.push_back(base + 2);
    }
}

void AppendBytes(std::vector<uint8_t>& out, const void* p, size_t n)
{
    const uint8_t* b = static_cast<const uint8_t*>(p);
    out.insert(out.end(), b, b + n);
}

void AppendMesh(std::vector<uint8_t>& out, const FractureMesh& mesh)
{
    const uint32_t vc = static_cast<uint32_t>(mesh.verts.size());
    const uint32_t ic = static_cast<uint32_t>(mesh.indices.size());
    AppendBytes(out, &vc, sizeof(vc));
    AppendBytes(out, &ic, sizeof(ic));
    if (vc > 0) {
        AppendBytes(out, mesh.verts.data(), mesh.verts.size() * sizeof(FractureVertex));
    }
    if (ic > 0) {
        AppendBytes(out, mesh.indices.data(), mesh.indices.size() * sizeof(int32_t));
    }
}

} // namespace

namespace {

// PlaceSeeds が済んだ (または SelfTest が明示的に位置を渡した) シード列から、
// spec §4.1 焼きの 2〜8 を最後まで行う。BakeFracture と BakeFractureWithSeeds の共通部分。
// progress/progressUserData は M80i (Editor の非同期焼き) の段階表示専用、出力には影響しない
bool BakeFractureCore(const FractureMesh& source, const std::vector<XMFLOAT3>& seedPositions,
                      float minVolumeRatio, FractureBakeResult& out,
                      FractureBakeProgressFn progress = nullptr, void* progressUserData = nullptr)
{
    // ---- 破片 1 個 (分割の必要なし): 焼きはそのまま 1 破片を返す ----
    if (seedPositions.size() <= 1) {
        RawPiece rp;
        rp.originSeed = 0;
        rp.mesh = source;
        rp.triTag.assign(static_cast<size_t>(source.TriCount()), -1);
        ComputeVolumeCentroid(rp.mesh, rp.volume, rp.centroid);
        FractureMesh outerLocal, capLocal;
        SplitOuterCap(rp.mesh, rp.triTag, outerLocal, capLocal);
        for (FractureVertex& v : outerLocal.verts) {
            v.position.x -= rp.centroid.x;
            v.position.y -= rp.centroid.y;
            v.position.z -= rp.centroid.z;
        }
        FracturePieceBake piece;
        piece.origin = rp.centroid;
        piece.volume = rp.volume;
        piece.outer = std::move(outerLocal);
        piece.cap = capLocal;
        if (progress) {
            progress(FractureBakeStage::Hull, progressUserData);
        }
        std::vector<XMFLOAT3> hullPts;
        for (const FractureVertex& v : piece.outer.verts) {
            hullPts.push_back(v.position);
        }
        BuildConvexHull(DedupPositionsExact(std::move(hullPts)), piece.hull);
        out.pieces.push_back(std::move(piece));
        out.success = true;
        return true;
    }

    // ---- 2. セル多面体 (シードごとの候補面) ----
    XMFLOAT3 lo, hi;
    const XMFLOAT3 extentVec = MeshAabbExtent(source, lo, hi);
    const XMFLOAT3 center{ (lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f };
    const XMFLOAT3 half{ (std::max)(extentVec.x, 1e-3f) * 2.0f + 1.0f, (std::max)(extentVec.y, 1e-3f) * 2.0f + 1.0f,
                         (std::max)(extentVec.z, 1e-3f) * 2.0f + 1.0f };
    const std::vector<PolyFace> inflatedBox = MakeBoxFaces(center, half);
    // 連結成分の頂点近接しきい値 = 元メッシュの AABB 対角 × kProximityEpsRelative (名前付き定数)
    const float proximityEps = (std::max)(Len3(FromF3(extentVec)), 1e-6f) * kProximityEpsRelative;

    const int32_t numSeeds = static_cast<int32_t>(seedPositions.size());
    std::vector<std::vector<CandidatePlane>> allCandidates(static_cast<size_t>(numSeeds));
    for (int32_t i = 0; i < numSeeds; ++i) {
        ComputeCandidatePlanes(i, seedPositions, inflatedBox, allCandidates[static_cast<size_t>(i)]);
    }

    // ---- 3 外側面: シードごとに自分の候補面だけで元メッシュを三角形単位クリップする ----
    std::vector<FractureMesh> seedMesh(static_cast<size_t>(numSeeds));
    std::vector<std::vector<int32_t>> seedTriTag(static_cast<size_t>(numSeeds));
    for (int32_t i = 0; i < numSeeds; ++i) {
        const std::vector<CandidatePlane>& ci = allCandidates[static_cast<size_t>(i)];
        ClipTrianglesAndAppend(source, ci, ci.size(), -1, seedMesh[static_cast<size_t>(i)],
                               seedTriTag[static_cast<size_t>(i)]);
    }

    // ---- 3 断面 (蓋): シード対ごとに 1 回だけ作り、両側 (i は as-is、j は巻き反転) で共有する ----
    std::set<std::pair<int32_t, int32_t>> processedPairs;
    for (int32_t i = 0; i < numSeeds; ++i) {
        for (const CandidatePlane& entry : allCandidates[static_cast<size_t>(i)]) {
            const int32_t j = entry.neighborSeed;
            const std::pair<int32_t, int32_t> key{ (std::min)(i, j), (std::max)(i, j) };
            if (!processedPairs.insert(key).second) {
                continue; // 反対側からもう処理済み
            }
            ProcessAdjacentPair(i, j, entry.normal, entry.d, allCandidates[static_cast<size_t>(i)],
                                allCandidates[static_cast<size_t>(j)], source, seedMesh, seedTriTag);
        }
    }

    // ---- 4 非連結の分離 (頂点の近さでつなぐ) + 破片の合否判定 ----
    std::vector<RawPiece> rawPieces;
    for (int32_t i = 0; i < numSeeds; ++i) {
        FractureMesh& mesh = seedMesh[static_cast<size_t>(i)];
        if (mesh.TriCount() == 0) {
            continue; // このセルはメッシュに触れなかった (何も残らない) — 破片を作らない
        }
        std::vector<Component> comps
            = SplitConnectedComponents(mesh, seedTriTag[static_cast<size_t>(i)], proximityEps);
        for (Component& comp : comps) {
            const PieceValidity validity = ValidatePieceGeometry(comp.mesh);
            if (!validity.valid) {
                out.failReason = "破片 (シード " + std::to_string(i) + ") の面が欠けている (volume="
                                + std::to_string(validity.volume)
                                + " vectorArea=" + std::to_string(validity.vectorAreaMag)
                                + " surfaceArea=" + std::to_string(validity.surfaceArea)
                                + " ratio=" + std::to_string(validity.surfaceArea > 0.0
                                                                  ? validity.vectorAreaMag / validity.surfaceArea
                                                                  : -1.0)
                                + ")";
                // 回避策を積まずに入力と理由をそのまま返す
                return false;
            }
            RawPiece rp;
            rp.originSeed = i;
            rp.mesh = std::move(comp.mesh);
            rp.triTag = std::move(comp.triTag);
            ComputeVolumeCentroid(rp.mesh, rp.volume, rp.centroid);
            if (!(rp.volume > 1e-12)) {
                continue; // 数値的なスリバー (体積ほぼ 0) は捨てる
            }
            rp.sortKeyPos = MinVertexPosition(rp.mesh);
            rawPieces.push_back(std::move(rp));
        }
    }
    if (rawPieces.empty()) {
        out.failReason = "有効な破片が 1 つも得られなかった";
        return false;
    }

    // ---- 接着グラフ (仮、統合前) ----
    std::vector<AdjMap> adj;
    ResolvePieceAdjacency(rawPieces, adj);

    // ---- 4. 極小片の統合 ----
    // しきい値 (平均 × minVolumeRatio) は統合のたびに残ったピースで数え直す。統合は体積の
    // 総量を変えずにピース数を減らすので平均は単調に増える (収束は必ず 1 ピースで止まる)。
    // 「統合後に平均未満が無い」という不変量をそのまま満たす定義
    const float ratio = (std::max)(minVolumeRatio, 0.0f);
    bool progressed = true;
    while (progressed && rawPieces.size() > 1) {
        progressed = false;
        const double avgVolume
            = std::accumulate(rawPieces.begin(), rawPieces.end(), 0.0, [](double s, const RawPiece& p) { return s + p.volume; })
            / static_cast<double>(rawPieces.size());
        const double threshold = avgVolume * static_cast<double>(ratio);
        for (int32_t idx = 0; idx < static_cast<int32_t>(rawPieces.size()); ++idx) {
            if (rawPieces[static_cast<size_t>(idx)].volume >= threshold) {
                continue;
            }
            int32_t bestNeighbor = -1;
            float bestArea = -1.0f;
            for (const auto& [otherIdx, area] : adj[static_cast<size_t>(idx)]) {
                if (area > bestArea || (area == bestArea && (bestNeighbor < 0 || otherIdx < bestNeighbor))) {
                    bestArea = area;
                    bestNeighbor = otherIdx;
                }
            }
            if (bestNeighbor < 0) {
                continue; // 隣接なし (孤立) — 統合できない。そのまま残す
            }
            MergePieceInto(rawPieces, adj, idx, bestNeighbor);
            RemovePiece(rawPieces, adj, idx); // 統合後は idx を詰めて次のループ先頭からやり直す
            ++out.mergedCount;
            progressed = true;
            break;
        }
    }
    if (progress) {
        progress(FractureBakeStage::Hull, progressUserData);
    }
    // ---- 7. 決定的な並び替え (originSeed 昇順 → 最小頂点位置) ----
    std::vector<int32_t> order(rawPieces.size());
    for (size_t i = 0; i < order.size(); ++i) {
        order[i] = static_cast<int32_t>(i);
    }
    std::sort(order.begin(), order.end(), [&](int32_t a, int32_t b) {
        const RawPiece &ra = rawPieces[static_cast<size_t>(a)], &rb = rawPieces[static_cast<size_t>(b)];
        if (ra.originSeed != rb.originSeed) {
            return ra.originSeed < rb.originSeed;
        }
        return PositionLess(ra.sortKeyPos, rb.sortKeyPos);
    });
    std::vector<int32_t> newIndexOf(rawPieces.size());
    for (size_t rank = 0; rank < order.size(); ++rank) {
        newIndexOf[static_cast<size_t>(order[rank])] = static_cast<int32_t>(rank);
    }

    // ---- 6a. 隣接 (相手 index 昇順、まだ切り捨てない) を全破片ぶん先に組む ----
    // 切り捨てを対称にするには、自分の隣接数だけでなく相手の判断も見る必要があるため、
    // 破片ごとに独立処理はできない (CapNeighborsSymmetrically へ全破片ぶんまとめて渡す)
    std::vector<std::vector<FractureNeighbor>> allNeighbors(order.size());
    for (size_t rank = 0; rank < order.size(); ++rank) {
        std::vector<FractureNeighbor>& neighbors = allNeighbors[rank];
        for (const auto& [otherIdx, area] : adj[static_cast<size_t>(order[rank])]) {
            neighbors.push_back({ newIndexOf[static_cast<size_t>(otherIdx)], area });
        }
        std::sort(neighbors.begin(), neighbors.end(), [](const FractureNeighbor& a, const FractureNeighbor& b) {
            return a.pieceIndex < b.pieceIndex;
        });
    }
    std::vector<int32_t> droppedCounts;
    CapNeighborsSymmetrically(allNeighbors, droppedCounts);

    // ---- 6b/8. 破片ごとの出力 (原点シフト・凸包) ----
    out.pieces.resize(order.size());
    for (size_t rank = 0; rank < order.size(); ++rank) {
        const RawPiece& rp = rawPieces[static_cast<size_t>(order[rank])];
        FracturePieceBake piece;
        piece.origin = rp.centroid;
        piece.volume = rp.volume;
        FractureMesh outerLocal, capLocal;
        SplitOuterCap(rp.mesh, rp.triTag, outerLocal, capLocal);
        for (FractureVertex& v : outerLocal.verts) {
            v.position.x -= rp.centroid.x;
            v.position.y -= rp.centroid.y;
            v.position.z -= rp.centroid.z;
        }
        for (FractureVertex& v : capLocal.verts) {
            v.position.x -= rp.centroid.x;
            v.position.y -= rp.centroid.y;
            v.position.z -= rp.centroid.z;
        }
        piece.outer = std::move(outerLocal);
        piece.cap = std::move(capLocal);

        std::vector<XMFLOAT3> hullPts;
        hullPts.reserve(piece.outer.verts.size() + piece.cap.verts.size());
        for (const FractureVertex& v : piece.outer.verts) {
            hullPts.push_back(v.position);
        }
        for (const FractureVertex& v : piece.cap.verts) {
            hullPts.push_back(v.position);
        }
        BuildConvexHull(DedupPositionsExact(std::move(hullPts)), piece.hull);

        piece.neighbors = std::move(allNeighbors[rank]);
        piece.droppedNeighbors = droppedCounts[rank];

        out.pieces[rank] = std::move(piece);
    }

    out.success = true;
    return true;
}

} // namespace

void CapNeighborsSymmetrically(std::vector<std::vector<FractureNeighbor>>& neighbors,
                               std::vector<int32_t>& droppedCount)
{
    const int32_t n = static_cast<int32_t>(neighbors.size());
    droppedCount.assign(static_cast<size_t>(n), 0);

    // 破片ごとに「自分の隣接数の超過分」を面積の小さい順 (同値は相手 index 小) に選び、
    // 消す組 (min(i,j), max(i,j)) の集合を作る。和集合を取るので、どちらか一方が
    // 落とすと判断すれば両側から消える (対称性はここで保証する)
    std::set<std::pair<int32_t, int32_t>> toDrop;
    for (int32_t i = 0; i < n; ++i) {
        const auto& list = neighbors[static_cast<size_t>(i)];
        const int32_t over = static_cast<int32_t>(list.size()) - kMaxFractureNeighbors;
        if (over <= 0) {
            continue;
        }
        std::vector<int32_t> byAreaAsc(list.size());
        for (size_t k = 0; k < byAreaAsc.size(); ++k) {
            byAreaAsc[k] = static_cast<int32_t>(k);
        }
        std::sort(byAreaAsc.begin(), byAreaAsc.end(), [&](int32_t a, int32_t b) {
            if (list[static_cast<size_t>(a)].area != list[static_cast<size_t>(b)].area) {
                return list[static_cast<size_t>(a)].area < list[static_cast<size_t>(b)].area;
            }
            return list[static_cast<size_t>(a)].pieceIndex < list[static_cast<size_t>(b)].pieceIndex;
        });
        for (int32_t k = 0; k < over; ++k) {
            const int32_t j = list[static_cast<size_t>(byAreaAsc[static_cast<size_t>(k)])].pieceIndex;
            toDrop.insert({ (std::min)(i, j), (std::max)(i, j) });
        }
    }
    if (toDrop.empty()) {
        return;
    }

    for (int32_t i = 0; i < n; ++i) {
        auto& list = neighbors[static_cast<size_t>(i)];
        const size_t before = list.size();
        list.erase(std::remove_if(list.begin(), list.end(),
                                  [&](const FractureNeighbor& nb) {
                                      return toDrop.count({ (std::min)(i, nb.pieceIndex),
                                                            (std::max)(i, nb.pieceIndex) })
                                          != 0;
                                  }),
                   list.end());
        droppedCount[static_cast<size_t>(i)] = static_cast<int32_t>(before - list.size());
    }
}

bool BakeFracture(const FractureBakeInput& input, FractureBakeResult& out)
{
    out = FractureBakeResult{};
    out.seedsRequested = input.pieceCount;
    const auto Report = [&](FractureBakeStage stage) {
        if (input.progress) {
            input.progress(stage, input.progressUserData);
        }
    };

    if (input.sourceMesh.verts.empty() || input.sourceMesh.indices.empty()) {
        out.failReason = "ソースメッシュが空";
        return false;
    }

    // ---- 1. 閉じ判定 → 拒否 / ボクセル化、内向きなら正規化 ----
    Report(FractureBakeStage::ClosedCheck);
    FractureMesh effectiveSource = input.sourceMesh;
    ClosedMeshCheck check = CheckClosedMesh(effectiveSource);
    if (!check.closed) {
        if (input.openMeshMode == 0) {
            out.failReason = "メッシュが閉じていない (境界辺 " + std::to_string(check.boundaryEdges)
                            + " 本 / 非多様体辺 " + std::to_string(check.nonManifoldEdges)
                            + " 本 / 向き不一致 " + std::to_string(check.orientationMismatches) + " 本)";
            out.rejectedOpenMesh = true;
            out.boundaryEdges = check.boundaryEdges;
            out.nonManifoldEdges = check.nonManifoldEdges;
            out.orientationMismatches = check.orientationMismatches;
            return false;
        }
        Report(FractureBakeStage::Voxelize);
        FractureVoxelizeResult voxelized;
        if (!VoxelizeMeshForFracture(effectiveSource, input.voxelResolution, voxelized)) {
            out.failReason = "ボクセル化に失敗: " + voxelized.failReason;
            return false;
        }
        effectiveSource = std::move(voxelized.mesh);
        check = CheckClosedMesh(effectiveSource); // signedVolume を得るための再検査 (閉じているのは保証済み)
    }
    if (check.signedVolume < 0.0) {
        FlipMeshWinding(effectiveSource);
    }

    Report(FractureBakeStage::Split);
    const int32_t pieceCount = std::clamp(input.pieceCount, 1, kMaxFracturePieces);
    if (pieceCount <= 1) {
        out.seedsPlaced = 1;
        return BakeFractureCore(effectiveSource, { XMFLOAT3{ 0, 0, 0 } }, input.minVolumeRatio, out,
                                input.progress, input.progressUserData);
    }

    // ---- 2. 内部シード ----
    const SeedPlacement seeds = PlaceSeeds(effectiveSource, input.seed, pieceCount);
    out.seedsPlaced = seeds.placed;
    if (seeds.placed <= 0) {
        out.failReason = "内部シードを 1 つも置けなかった";
        return false;
    }
    return BakeFractureCore(effectiveSource, seeds.seeds, input.minVolumeRatio, out, input.progress,
                            input.progressUserData);
}

bool BakeFractureWithSeeds(const FractureMesh& sourceMesh, const std::vector<XMFLOAT3>& seeds,
                           float minVolumeRatio, FractureBakeResult& out)
{
    out = FractureBakeResult{};
    out.seedsRequested = static_cast<int32_t>(seeds.size());
    out.seedsPlaced = static_cast<int32_t>(seeds.size());
    if (sourceMesh.verts.empty() || sourceMesh.indices.empty()) {
        out.failReason = "ソースメッシュが空";
        return false;
    }
    if (seeds.empty()) {
        out.failReason = "シードが 1 つも指定されていない";
        return false;
    }
    return BakeFractureCore(sourceMesh, seeds, minVolumeRatio, out);
}

uint64_t FractureBakeDigest(const FractureBakeResult& result)
{
    std::vector<uint8_t> bytes;
    const uint8_t ok = result.success ? 1 : 0;
    AppendBytes(bytes, &ok, sizeof(ok));
    const uint32_t pieceCount = static_cast<uint32_t>(result.pieces.size());
    AppendBytes(bytes, &pieceCount, sizeof(pieceCount));
    const int32_t seedsPlaced = result.seedsPlaced;
    const int32_t mergedCount = result.mergedCount;
    AppendBytes(bytes, &seedsPlaced, sizeof(seedsPlaced));
    AppendBytes(bytes, &mergedCount, sizeof(mergedCount));
    for (const FracturePieceBake& piece : result.pieces) {
        AppendBytes(bytes, &piece.origin, sizeof(piece.origin));
        AppendBytes(bytes, &piece.volume, sizeof(piece.volume));
        AppendMesh(bytes, piece.outer);
        AppendMesh(bytes, piece.cap);
        SerializeConvexHull(piece.hull, bytes);
        const uint32_t nc = static_cast<uint32_t>(piece.neighbors.size());
        AppendBytes(bytes, &nc, sizeof(nc));
        for (const FractureNeighbor& nb : piece.neighbors) {
            AppendBytes(bytes, &nb.pieceIndex, sizeof(nb.pieceIndex));
            AppendBytes(bytes, &nb.area, sizeof(nb.area));
        }
        AppendBytes(bytes, &piece.droppedNeighbors, sizeof(piece.droppedNeighbors));
    }
    return HashBytes(bytes.data(), bytes.size());
}

} // namespace mye
