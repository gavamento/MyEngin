//====================================================================================
//                          FractureMesh.cpp
//  MyEngin/ 秋田蓮音                                                     09/25/2026
//                                          破壊分割コアの実装: 閉じ判定・平面切断・蓋の三角形分割
//====================================================================================
#include "Engine/Engine/Physics/FractureMesh.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>

using namespace DirectX;

namespace mye {
namespace {

// ---- 小さな 3D ベクトル演算 (ConvexHull.cpp と同じ流儀。ファイルごとにローカルに持つ) ----
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

// -0.0 を +0.0 へ畳む (ConvexHull.cpp の Zeroed と同じ理由 — 足し算での畳みは
// −0.0 を化けさせる値ゲートになるので、分岐ゲートで書く)
float Zeroed(float v)
{
    return (v == 0.0f) ? 0.0f : v;
}

// 位置の全順序 (x→y→z)
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
struct PositionLessCmp {
    bool operator()(const XMFLOAT3& a, const XMFLOAT3& b) const { return PositionLess(a, b); }
};

// ---- 閉じ判定用の位置溶接 ----
// 各頂点に「同じ位置を持つグループの番号」を振る。番号はソート順から出るだけで、
// 頂点の入力順には依らない (常に同じ点集合から同じグループ分けになる)
std::vector<int32_t> WeldedIds(const std::vector<FractureVertex>& verts)
{
    const size_t n = verts.size();
    std::vector<int32_t> order(n);
    for (size_t i = 0; i < n; ++i) {
        order[i] = static_cast<int32_t>(i);
    }
    auto key = [&](int32_t i) {
        const XMFLOAT3& p = verts[static_cast<size_t>(i)].position;
        return XMFLOAT3{ Zeroed(p.x), Zeroed(p.y), Zeroed(p.z) };
    };
    std::sort(order.begin(), order.end(), [&](int32_t a, int32_t b) {
        const XMFLOAT3 ka = key(a), kb = key(b);
        if (PositionLess(ka, kb)) {
            return true;
        }
        if (PositionLess(kb, ka)) {
            return false;
        }
        return a < b; // 同位置は元 index で安定させる (グループ番号自体には影響しない)
    });
    std::vector<int32_t> weldId(n, 0);
    int32_t group = -1;
    XMFLOAT3 prevKey{};
    for (size_t k = 0; k < n; ++k) {
        const int32_t idx = order[k];
        const XMFLOAT3 kk = key(idx);
        if (group < 0 || kk.x != prevKey.x || kk.y != prevKey.y || kk.z != prevKey.z) {
            ++group;
            prevKey = kk;
        }
        weldId[static_cast<size_t>(idx)] = group;
    }
    return weldId;
}

// 開いた三角形メッシュ (平面切断でできた外側面の断片) の境界辺 (位置溶接した無向辺の使用数が
// 1) を、元の三角形の巻き順から向きを継承したまま集める。三角形をどう分割したかに関わらず
// 「外側面の巻き順」を直接の正とすることで、断面ループの向きを別途推測しなくて済む
// (推測に頼った実装はトーラス/L字で向きを取り違えた — 実測で確認済み)
std::vector<std::pair<FractureVertex, FractureVertex>> ExtractBoundaryEdges(const FractureMesh& mesh)
{
    std::vector<std::pair<FractureVertex, FractureVertex>> segs;
    const std::vector<int32_t> weld = WeldedIds(mesh.verts);
    struct Rec {
        int32_t lo, hi;
        int32_t rawFrom, rawTo;
    };
    std::vector<Rec> recs;
    const int32_t triCount = mesh.TriCount();
    recs.reserve(static_cast<size_t>(triCount) * 3);
    for (int32_t t = 0; t < triCount; ++t) {
        const int32_t raw[3] = { mesh.indices[static_cast<size_t>(t) * 3 + 0],
                                 mesh.indices[static_cast<size_t>(t) * 3 + 1],
                                 mesh.indices[static_cast<size_t>(t) * 3 + 2] };
        const int32_t w[3] = { weld[static_cast<size_t>(raw[0])], weld[static_cast<size_t>(raw[1])],
                               weld[static_cast<size_t>(raw[2])] };
        for (int k = 0; k < 3; ++k) {
            const int32_t u = w[k], v = w[(k + 1) % 3];
            if (u == v) {
                continue;
            }
            Rec r;
            r.lo = (u < v) ? u : v;
            r.hi = (u < v) ? v : u;
            r.rawFrom = raw[k];
            r.rawTo = raw[(k + 1) % 3];
            recs.push_back(r);
        }
    }
    std::sort(recs.begin(), recs.end(), [](const Rec& x, const Rec& y) {
        if (x.lo != y.lo) {
            return x.lo < y.lo;
        }
        return x.hi < y.hi;
    });
    for (size_t i = 0; i < recs.size();) {
        size_t j = i + 1;
        while (j < recs.size() && recs[j].lo == recs[i].lo && recs[j].hi == recs[i].hi) {
            ++j;
        }
        if (j - i == 1) {
            segs.emplace_back(mesh.verts[static_cast<size_t>(recs[i].rawFrom)],
                              mesh.verts[static_cast<size_t>(recs[i].rawTo)]);
        }
        // 使用数 2 以上はメッシュ内部の辺 (もしくは非多様体)。断面の輪郭ではないので無視する
        i = j;
    }
    return segs;
}

// n が非零なら (t, b) を正規直交基底にし、t×b == n にする (Duff et al. 2017 の分岐無し構成)。
// n.z == 0 のときは sign を +1 側に固定し、符号ビットに結果が依らないようにする
void OrthonormalBasis(const XMFLOAT3& n, XMFLOAT3& t, XMFLOAT3& b)
{
    const float sign = (n.z >= 0.0f) ? 1.0f : -1.0f;
    const float a = -1.0f / (sign + n.z);
    const float bxy = n.x * n.y * a;
    t = { 1.0f + sign * n.x * n.x * a, sign * bxy, -sign * n.x };
    b = { bxy, sign + n.y * n.y * a, -n.y };
}

FractureVertex LerpVertex(const FractureVertex& a, const FractureVertex& b, float t)
{
    FractureVertex out;
    out.position = ToF3(Add(FromF3(a.position), Mul3(Sub(FromF3(b.position), FromF3(a.position)), t)));
    V3 nrm = Add(FromF3(a.normal), Mul3(Sub(FromF3(b.normal), FromF3(a.normal)), t));
    const float nlen = Len3(nrm);
    out.normal = (nlen > 1e-12f) ? ToF3(Mul3(nrm, 1.0f / nlen)) : a.normal;
    out.uv = { a.uv.x + t * (b.uv.x - a.uv.x), a.uv.y + t * (b.uv.y - a.uv.y) };
    return out;
}

// 三角形の 1 辺 (curr→next) が平面をまたぐときの交点。位置の全順序で lo/hi を固定してから
// t を計算するので、この辺を共有するもう一方の三角形が逆向きに辿っても常にビット同一になる
// (呼び出し側は sCurr と sNext が異符号であることを保証する)
FractureVertex ComputeCrossing(const FractureVertex& curr, float sCurr, const FractureVertex& next,
                                float sNext)
{
    const bool currIsLo = PositionLess(curr.position, next.position);
    const FractureVertex& lo = currIsLo ? curr : next;
    const FractureVertex& hi = currIsLo ? next : curr;
    const float sLo = currIsLo ? sCurr : sNext;
    const float sHi = currIsLo ? sNext : sCurr;
    const float t = sLo / (sLo - sHi);
    return LerpVertex(lo, hi, t);
}

void AppendTriangle(FractureMesh& mesh, const FractureVertex& a, const FractureVertex& b,
                    const FractureVertex& c)
{
    const int32_t base = static_cast<int32_t>(mesh.verts.size());
    mesh.verts.push_back(a);
    mesh.verts.push_back(b);
    mesh.verts.push_back(c);
    mesh.indices.push_back(base);
    mesh.indices.push_back(base + 1);
    mesh.indices.push_back(base + 2);
}

// 凸多角形 (三角形を平面で切った断片。頂点数は常に 3 か 4) をファン三角形分割で追加する
bool FanTriangulate(const std::vector<FractureVertex>& poly, FractureMesh& outMesh)
{
    if (poly.size() < 3) {
        return false;
    }
    for (size_t k = 1; k + 1 < poly.size(); ++k) {
        AppendTriangle(outMesh, poly[0], poly[k], poly[k + 1]);
    }
    return true;
}

// ---- 切断網 (方向付き線分の集合) を単純閉ループ列へ繋ぐ ----
// 各位置は「出て行く辺」を高々 1 本しか持たない前提 (閉じたメッシュを 1 平面で切った結果は
// 単純閉曲線の集合になるという保証に基づく)。崩れていたら落ちずに false を返す
bool ChainAllLoops(const std::vector<std::pair<FractureVertex, FractureVertex>>& segs,
                   std::vector<std::vector<FractureVertex>>& loopsOut, std::string& failReason)
{
    loopsOut.clear();
    if (segs.empty()) {
        return true; // 平面がメッシュに触れていない (この側には断面がない)
    }

    struct Item {
        FractureVertex from, to;
    };
    std::vector<Item> items;
    items.reserve(segs.size());
    for (const auto& s : segs) {
        items.push_back({ s.first, s.second });
    }

    std::map<XMFLOAT3, int32_t, PositionLessCmp> fromIndex;
    for (int32_t i = 0; i < static_cast<int32_t>(items.size()); ++i) {
        const auto res = fromIndex.emplace(items[static_cast<size_t>(i)].from.position, i);
        if (!res.second) {
            failReason = "同じ位置から2本以上の切断辺が出ている (非多様体入力の疑い)";
            return false;
        }
    }

    std::vector<int32_t> order(items.size());
    for (int32_t i = 0; i < static_cast<int32_t>(items.size()); ++i) {
        order[static_cast<size_t>(i)] = i;
    }
    std::sort(order.begin(), order.end(), [&](int32_t a, int32_t b) {
        return PositionLess(items[static_cast<size_t>(a)].from.position,
                            items[static_cast<size_t>(b)].from.position);
    });

    std::vector<uint8_t> used(items.size(), 0);
    const int32_t guardMax = static_cast<int32_t>(items.size()) + 1;
    for (int32_t startI : order) {
        if (used[static_cast<size_t>(startI)]) {
            continue;
        }
        std::vector<FractureVertex> loop;
        int32_t cur = startI;
        int32_t steps = 0;
        for (;;) {
            if (used[static_cast<size_t>(cur)]) {
                failReason = "断面ループが想定外の位置で交わっている";
                return false;
            }
            used[static_cast<size_t>(cur)] = 1;
            loop.push_back(items[static_cast<size_t>(cur)].from);
            const auto it = fromIndex.find(items[static_cast<size_t>(cur)].to.position);
            if (it == fromIndex.end()) {
                failReason = "対応する後続の切断辺が見つからない (断面ループが閉じない)";
                return false;
            }
            const int32_t next = it->second;
            ++steps;
            if (steps > guardMax) {
                failReason = "断面ループが規定回数で閉じない (安全弁)";
                return false;
            }
            if (next == startI) {
                break;
            }
            cur = next;
        }
        if (loop.size() < 3) {
            failReason = "断面ループの頂点数が3未満";
            return false;
        }
        loopsOut.push_back(std::move(loop));
    }
    return true;
}

// ---- 平面上の 2D 多角形 (蓋の三角形分割用) ----
struct Pt2 {
    float u = 0, v = 0;
    int32_t vertexIdx = -1; // 出力メッシュの頂点 index (三角形分割の結果で使う)
};

float Cross2(float ax, float ay, float bx, float by)
{
    return ax * by - ay * bx;
}

// シューレースの符号付き面積 (正 = 反時計回り)
double SignedArea2(const std::vector<Pt2>& poly)
{
    double a = 0.0;
    const size_t n = poly.size();
    for (size_t i = 0; i < n; ++i) {
        const Pt2& p0 = poly[i];
        const Pt2& p1 = poly[(i + 1) % n];
        a += static_cast<double>(p0.u) * p1.v - static_cast<double>(p1.u) * p0.v;
    }
    return a * 0.5;
}

double Cross2D(double ax, double ay, double bx, double by)
{
    return ax * by - ay * bx;
}

// 点が三角形の内部 (境界含む) にあるか。巻き順は問わない (符号が全て同じなら内部)。
// 密な円弧近似 (穴あき蓋の環状ループなど) では float の丸め誤差でほぼ同一直線上の点の
// 内外判定がぶれ、耳切りが「塞がれている」と誤判定して詰まることがあった (実測で確認済み)。
// ここだけは double で計算し、桁落ちの余地を減らす
bool PointInTriangle2(float u, float v, const Pt2& p0, const Pt2& p1, const Pt2& p2)
{
    const double d1 = Cross2D(static_cast<double>(p1.u) - p0.u, static_cast<double>(p1.v) - p0.v,
                              static_cast<double>(u) - p0.u, static_cast<double>(v) - p0.v);
    const double d2 = Cross2D(static_cast<double>(p2.u) - p1.u, static_cast<double>(p2.v) - p1.v,
                              static_cast<double>(u) - p1.u, static_cast<double>(v) - p1.v);
    const double d3 = Cross2D(static_cast<double>(p0.u) - p2.u, static_cast<double>(p0.v) - p2.v,
                              static_cast<double>(u) - p2.u, static_cast<double>(v) - p2.v);
    const bool hasNeg = (d1 < 0.0) || (d2 < 0.0) || (d3 < 0.0);
    const bool hasPos = (d1 > 0.0) || (d2 > 0.0) || (d3 > 0.0);
    return !(hasNeg && hasPos);
}

// レイキャスト法 (+u 方向)。境界上は未定義 (呼び出し側は内部の代表点で使う)
bool PointInPoly2(const std::vector<Pt2>& poly, float u, float v)
{
    bool inside = false;
    const size_t n = poly.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const float ui = poly[i].u, vi = poly[i].v, uj = poly[j].u, vj = poly[j].v;
        if (((vi > v) != (vj > v)) && (u < (uj - ui) * (v - vi) / (vj - vi) + ui)) {
            inside = !inside;
        }
    }
    return inside;
}

// 多角形が (ほぼ) 凸か。しきい値は頂点ごとに隣接 2 辺の長さの積に対する相対値で決める —
// 断面切断を何度も重ねた蓋は場所によって点の密度が大きく違い (狭い範囲に点が密集する箇所と
// 疎な箇所が混在する)、多角形全体の面積を基準にした一律のしきい値では密集箇所で誤って
// 「凹」と判定してしまう (実測で確認済み)
bool IsConvexCCW(const std::vector<Pt2>& poly)
{
    const int32_t n = static_cast<int32_t>(poly.size());
    if (n < 3) {
        return false;
    }
    for (int32_t i = 0; i < n; ++i) {
        const Pt2& prev = poly[static_cast<size_t>((i + n - 1) % n)];
        const Pt2& curr = poly[static_cast<size_t>(i)];
        const Pt2& next = poly[static_cast<size_t>((i + 1) % n)];
        const double e1u = static_cast<double>(curr.u) - prev.u, e1v = static_cast<double>(curr.v) - prev.v;
        const double e2u = static_cast<double>(next.u) - curr.u, e2v = static_cast<double>(next.v) - curr.v;
        const double cr = Cross2D(e1u, e1v, e2u, e2v);
        const double localEps = std::sqrt((e1u * e1u + e1v * e1v) * (e2u * e2u + e2v * e2v)) * 1e-4;
        if (cr < -localEps) {
            return false;
        }
    }
    return true;
}

// ---- 耳切り三角形分割 (単純多角形、CCW 前提) ----
// 毎回、有効な耳のうち最も丸い (cr が最大の) ものを選んで切る (同値は index 最小、決定的)。
// O(n^3) だが蓋の頂点数は高々数百程度 (メッシュの断面) なので実用上問題ない
bool EarClip(std::vector<Pt2> poly, double areaEps, std::vector<std::array<int32_t, 3>>& trisOut)
{
    if (poly.size() < 3) {
        return false;
    }
    const int32_t maxIter = static_cast<int32_t>(poly.size()) * static_cast<int32_t>(poly.size()) + 64;
    int32_t iter = 0;
    while (poly.size() > 3) {
        bool clipped = false;
        const int32_t n = static_cast<int32_t>(poly.size());

        // 共線点 (prev,curr,next が一直線上で curr が prev→next の間にある) を最優先で外す。
        // 面積0の耳として記録するので体積にも辺の使用回数にも影響しない。ボクセル化した
        // 断面のように共線点が大量に連なる輪郭では、後段の「最も丸い耳」探索がその共線点に
        // 隣の耳を塞がれて詰まるため、ここで先に間引く
        for (int32_t i = 0; i < n; ++i) {
            const Pt2& prev = poly[static_cast<size_t>((i + n - 1) % n)];
            const Pt2& curr = poly[static_cast<size_t>(i)];
            const Pt2& next = poly[static_cast<size_t>((i + 1) % n)];
            const double e1u = static_cast<double>(curr.u) - prev.u, e1v = static_cast<double>(curr.v) - prev.v;
            const double e2u = static_cast<double>(next.u) - curr.u, e2v = static_cast<double>(next.v) - curr.v;
            if (std::fabs(Cross2D(e1u, e1v, e2u, e2v)) > areaEps) {
                continue;
            }
            if (e1u * e2u + e1v * e2v <= 0.0) {
                continue; // 折り返し (鋭い切り返し) は共線除去の対象にしない
            }
            trisOut.push_back({ prev.vertexIdx, curr.vertexIdx, next.vertexIdx });
            poly.erase(poly.begin() + i);
            clipped = true;
            break;
        }
        if (clipped) {
            if (++iter > maxIter) {
                return false; // 安全弁 (理論上到達しないはずだが無限ループを避ける)
            }
            continue;
        }

        // 最初に見つかった耳ではなく、有効な耳のうち最も丸い (cr が最大の) ものを選ぶ。
        // 「最初に見つかった耳」を毎回優先すると、穴を橋渡しした細い縫い目の周りで
        // 先に周囲を食い尽くしてしまい、残りがほぼ全周反射角の帯だけになって
        // 二耳定理が保証するはずの耳が実際には見つからず詰まることがあった
        // (穴あき蓋の環状ループで実測)。最も丸い耳を優先すると細い帯を作りにくい
        int32_t bestI = -1;
        double bestCr = areaEps;
        for (int32_t i = 0; i < n; ++i) {
            const Pt2& prev = poly[static_cast<size_t>((i + n - 1) % n)];
            const Pt2& curr = poly[static_cast<size_t>(i)];
            const Pt2& next = poly[static_cast<size_t>((i + 1) % n)];
            const double cr = Cross2D(static_cast<double>(curr.u) - prev.u,
                                      static_cast<double>(curr.v) - prev.v,
                                      static_cast<double>(next.u) - curr.u,
                                      static_cast<double>(next.v) - curr.v);
            if (cr <= bestCr) {
                continue; // 凹・縮退、またはこれまでの最良候補以下
            }
            bool anyInside = false;
            for (int32_t k = 0; k < n; ++k) {
                if (k == (i + n - 1) % n || k == i || k == (i + 1) % n) {
                    continue;
                }
                const Pt2& pk = poly[static_cast<size_t>(k)];
                // 穴の橋渡しは同じ座標の点 (橋の両端) を配列の離れた場所に 2 回置く。
                // その複製が耳の 3 頂点のどれかと同じ座標なら、それは耳を塞ぐ別の点ではなく
                // 単なる自分自身の写しなので「塞いでいる」に数えない (数えると橋の周りの
                // 耳が永遠に選べず詰まる。穴あき蓋の環状ループで実測)
                if ((pk.u == prev.u && pk.v == prev.v) || (pk.u == curr.u && pk.v == curr.v)
                    || (pk.u == next.u && pk.v == next.v)) {
                    continue;
                }
                if (PointInTriangle2(pk.u, pk.v, prev, curr, next)) {
                    anyInside = true;
                    break;
                }
            }
            if (anyInside) {
                continue;
            }
            bestI = i;
            bestCr = cr;
        }
        if (bestI >= 0) {
            const int32_t i = bestI;
            const Pt2& prev = poly[static_cast<size_t>((i + n - 1) % n)];
            const Pt2& curr = poly[static_cast<size_t>(i)];
            const Pt2& next = poly[static_cast<size_t>((i + 1) % n)];
            trisOut.push_back({ prev.vertexIdx, curr.vertexIdx, next.vertexIdx });
            poly.erase(poly.begin() + i);
            clipped = true;
        }
        if (!clipped) {
            // 密に並んだ点が続く凸多角形では、"塞がれているか" の判定が丸め誤差で
            // 偽陽性になり、本来存在するはずの耳が見つからないことがある (連続する
            // 平面切断を重ねた蓋で実測)。残りの多角形が (数値誤差の範囲で) 凸なら、
            // 自己交差の心配がないファン分割へ切り替える。ただし IsConvexCCW 自体が
            // 丸め誤差で誤判定する可能性を潰すため、ファン分割後の面積が多角形本来の
            // 面積 (シューレース公式) と一致することを検算してから採用する —
            // 一致しなければ凸という前提自体が誤りなので安全側に倒して失敗を返す
            if (IsConvexCCW(poly)) {
                std::vector<std::array<int32_t, 3>> fanTris;
                double fanArea = 0.0;
                for (size_t k = 1; k + 1 < poly.size(); ++k) {
                    fanTris.push_back({ poly[0].vertexIdx, poly[static_cast<size_t>(k)].vertexIdx,
                                       poly[static_cast<size_t>(k) + 1].vertexIdx });
                    fanArea += Cross2D(static_cast<double>(poly[k].u) - poly[0].u,
                                       static_cast<double>(poly[k].v) - poly[0].v,
                                       static_cast<double>(poly[k + 1].u) - poly[0].u,
                                       static_cast<double>(poly[k + 1].v) - poly[0].v)
                             * 0.5;
                }
                const double polyArea = SignedArea2(poly);
                if (std::fabs(fanArea - polyArea) <= (std::max)(std::fabs(polyArea) * 1e-6, areaEps * 4.0)) {
                    trisOut.insert(trisOut.end(), fanTris.begin(), fanTris.end());
                    return true;
                }
            }
            return false; // 安全網: 有効な耳が見つからない (縮退・自己交差の疑い)
        }
        if (++iter > maxIter) {
            return false; // 安全弁 (理論上到達しないはずだが無限ループを避ける)
        }
    }
    trisOut.push_back({ poly[0].vertexIdx, poly[1].vertexIdx, poly[2].vertexIdx });
    return true;
}

// 穴 hole を outer へ橋渡しし、1 本の単純多角形に組み替える (Held の手法を簡略化)。
// outer は CCW (面積 > 0)、hole は CW (面積 < 0) の頂点順であることを前提にする
bool BridgeHoleIntoOuter(std::vector<Pt2>& outer, const std::vector<Pt2>& hole)
{
    if (hole.empty()) {
        return true;
    }
    // M: hole の中で u が最大 (同値は v が最大) の頂点 — 橋渡しの入口
    size_t mIdx = 0;
    for (size_t i = 1; i < hole.size(); ++i) {
        if (hole[i].u > hole[mIdx].u || (hole[i].u == hole[mIdx].u && hole[i].v > hole[mIdx].v)) {
            mIdx = i;
        }
    }
    const Pt2 M = hole[mIdx];

    // M から +u 方向のレイと outer の辺との交点のうち、M に最も近いもの (u が最小) を探す
    bool found = false;
    float bestU = 0.0f;
    Pt2 crossPt{};
    size_t edgeA = 0, edgeB = 0;
    const size_t n = outer.size();
    for (size_t i = 0; i < n; ++i) {
        const Pt2& a = outer[i];
        const Pt2& b = outer[(i + 1) % n];
        if ((a.v > M.v) == (b.v > M.v)) {
            continue; // M の水平線をまたがない辺
        }
        if (a.v == b.v) {
            continue; // 水平な辺 (交点が一意にならない)
        }
        const float t = (M.v - a.v) / (b.v - a.v);
        const float u = a.u + t * (b.u - a.u);
        if (u < M.u) {
            continue;
        }
        if (!found || u < bestU) {
            found = true;
            bestU = u;
            crossPt = { u, M.v, -1 };
            edgeA = i;
            edgeB = (i + 1) % n;
        }
    }
    if (!found) {
        return false; // 橋渡し不能 (穴が外周の内側にない)
    }

    // 交点を含む辺の 2 端点のうち u が大きい方を橋の相手候補にする
    size_t pIdx = (outer[edgeA].u > outer[edgeB].u) ? edgeA : edgeB;

    // 三角形 (M, crossPt, candidate) の内側にある outer 頂点のうち、レイに最も近い
    // (= M から見て candidate より反時計回り側にある) ものへ選び直す。選び直さないと
    // 候補の裏に隠れた頂点をまたいで橋が外周の外へ出ることがある
    for (size_t i = 0; i < n; ++i) {
        if (i == edgeA || i == edgeB) {
            continue;
        }
        const Pt2& c = outer[i];
        if (!PointInTriangle2(c.u, c.v, M, crossPt, outer[pIdx])) {
            continue;
        }
        const float crossCur
            = Cross2(outer[pIdx].u - M.u, outer[pIdx].v - M.v, c.u - M.u, c.v - M.v);
        if (crossCur >= 0.0f && (c.u > outer[pIdx].u || (c.u == outer[pIdx].u && i < pIdx))) {
            pIdx = i;
        }
    }

    std::vector<Pt2> merged;
    merged.reserve(outer.size() + hole.size() + 2);
    for (size_t i = 0; i <= pIdx; ++i) {
        merged.push_back(outer[i]);
    }
    for (size_t k = 0; k < hole.size(); ++k) {
        merged.push_back(hole[(mIdx + k) % hole.size()]);
    }
    merged.push_back(hole[mIdx]); // 穴を一周して M へ戻る
    for (size_t i = pIdx; i < outer.size(); ++i) {
        merged.push_back(outer[i]);
    }
    outer.swap(merged);
    return true;
}

// ループ列 (方向付きの単純閉曲線の集合) から蓋の三角形メッシュを作る。
// capNormal は蓋の外向き法線 (このまま出力頂点の法線になる)
bool CapLoops(const std::vector<std::vector<FractureVertex>>& loops, const XMFLOAT3& capNormal,
             FractureMesh& capOut, std::string& failReason)
{
    capOut = FractureMesh{};
    if (loops.empty()) {
        return true;
    }

    XMFLOAT3 tangent, bitangent;
    OrthonormalBasis(capNormal, tangent, bitangent);

    struct LoopInfo {
        std::vector<FractureVertex> verts3;
        std::vector<Pt2> pts2;
        double area = 0.0;
    };
    std::vector<LoopInfo> infos;
    infos.reserve(loops.size());
    double maxAbsArea = 0.0;
    for (const auto& loop : loops) {
        LoopInfo info;
        info.verts3 = loop;
        info.pts2.reserve(loop.size());
        for (const FractureVertex& v : loop) {
            const float u = v.position.x * tangent.x + v.position.y * tangent.y
                          + v.position.z * tangent.z;
            const float w = v.position.x * bitangent.x + v.position.y * bitangent.y
                          + v.position.z * bitangent.z;
            info.pts2.push_back({ u, w, -1 });
        }
        info.area = SignedArea2(info.pts2);
        maxAbsArea = (std::max)(maxAbsArea, std::fabs(info.area));
        infos.push_back(std::move(info));
    }
    const double areaEps = (std::max)(maxAbsArea * 1e-9, 1e-12);

    // 有効なループ (数値的なスリバーを除く) だけを対象にする
    std::vector<int32_t> validIdx;
    for (int32_t i = 0; i < static_cast<int32_t>(infos.size()); ++i) {
        if (std::fabs(infos[static_cast<size_t>(i)].area) > areaEps) {
            validIdx.push_back(i);
        }
    }
    if (validIdx.empty()) {
        return true; // 全部退化 = 蓋なし (安全側)
    }

    // 外周/穴の判定は面積の符号 (捻れ方向) に頼らない — t×b = capNormal になるよう
    // OrthonormalBasis を作ってあるので、外向きの外側面の境界辺から継承した向きは
    // (tangent, bitangent) へ射影すると外周は常に CW・穴は常に CCW になる (符号は一定で、
    // どちらにもなり得るわけではない)。それでも符号ではなく「他の何本のループに内包されて
    // いるか」の偶奇で外周/穴を決める (偶数=外周、奇数=穴。TrueType 等のグリフ輪郭と同じ
    // nonzero 系の判定。符号が一定でも、この方式なら穴のネスト構造の取り違えが起きない)。
    // EarClip は CCW 前提なので、向きは下で強制的に揃える
    const size_t n = validIdx.size();
    std::vector<int32_t> containCount(n, 0);
    for (size_t i = 0; i < n; ++i) {
        const LoopInfo& li = infos[static_cast<size_t>(validIdx[i])];
        for (size_t j = 0; j < n; ++j) {
            if (i == j) {
                continue;
            }
            const LoopInfo& lj = infos[static_cast<size_t>(validIdx[j])];
            if (PointInPoly2(lj.pts2, li.pts2[0].u, li.pts2[0].v)) {
                ++containCount[i];
            }
        }
    }

    std::vector<int32_t> outerLoops, holeLoops;
    for (size_t i = 0; i < n; ++i) {
        if (containCount[i] % 2 == 0) {
            outerLoops.push_back(validIdx[i]);
        } else {
            holeLoops.push_back(validIdx[i]);
        }
    }

    // 各穴を「直接の親」(1 段内側の外周) へ割り当てる。親候補は containCount がちょうど 1 少なく、
    // かつこの穴を内包しているもの。複数あれば面積が最小 (最も内側) のものを選ぶ
    std::map<int32_t, std::vector<int32_t>> holesOfOuter;
    for (size_t hi = 0; hi < n; ++hi) {
        if (containCount[hi] % 2 == 0) {
            continue;
        }
        const int32_t h = validIdx[hi];
        int32_t best = -1;
        double bestArea = 0.0;
        for (size_t oi = 0; oi < n; ++oi) {
            if (containCount[oi] != containCount[hi] - 1) {
                continue;
            }
            const int32_t o = validIdx[oi];
            if (!PointInPoly2(infos[static_cast<size_t>(o)].pts2, infos[static_cast<size_t>(h)].pts2[0].u,
                              infos[static_cast<size_t>(h)].pts2[0].v)) {
                continue;
            }
            const double a = std::fabs(infos[static_cast<size_t>(o)].area);
            if (best < 0 || a < bestArea) {
                best = o;
                bestArea = a;
            }
        }
        if (best < 0) {
            failReason = "穴の直接の親となる外周ループが見つからない";
            return false;
        }
        holesOfOuter[best].push_back(h);
    }

    for (int32_t o : outerLoops) {
        LoopInfo& outerInfo = infos[static_cast<size_t>(o)];
        std::vector<Pt2> poly = outerInfo.pts2;
        std::vector<FractureVertex> outerVerts3 = outerInfo.verts3;
        if (outerInfo.area < 0.0) { // 外周は CCW (面積 > 0) に揃える
            std::reverse(poly.begin(), poly.end());
            std::reverse(outerVerts3.begin(), outerVerts3.end());
        }
        for (size_t k = 0; k < poly.size(); ++k) {
            FractureVertex fv = outerVerts3[k];
            fv.normal = capNormal;
            fv.uv = { poly[k].u, poly[k].v }; // 箱投影 (tangent/bitangent への正射影)
            poly[k].vertexIdx = static_cast<int32_t>(capOut.verts.size());
            capOut.verts.push_back(fv);
        }
        const auto it = holesOfOuter.find(o);
        if (it != holesOfOuter.end()) {
            for (int32_t h : it->second) {
                LoopInfo& holeInfo = infos[static_cast<size_t>(h)];
                std::vector<Pt2> holePts = holeInfo.pts2;
                std::vector<FractureVertex> holeVerts3 = holeInfo.verts3;
                if (holeInfo.area > 0.0) { // 穴は CW (面積 < 0) に揃える
                    std::reverse(holePts.begin(), holePts.end());
                    std::reverse(holeVerts3.begin(), holeVerts3.end());
                }
                for (size_t k = 0; k < holePts.size(); ++k) {
                    FractureVertex fv = holeVerts3[k];
                    fv.normal = capNormal;
                    fv.uv = { holePts[k].u, holePts[k].v };
                    holePts[k].vertexIdx = static_cast<int32_t>(capOut.verts.size());
                    capOut.verts.push_back(fv);
                }
                if (!BridgeHoleIntoOuter(poly, holePts)) {
                    failReason = "穴の橋渡しに失敗";
                    return false;
                }
            }
        }
        std::vector<std::array<int32_t, 3>> tris;
        if (!EarClip(poly, areaEps, tris)) {
            failReason = "蓋の三角形分割に失敗";
            return false;
        }
        for (const auto& t : tris) {
            capOut.indices.push_back(t[0]);
            capOut.indices.push_back(t[1]);
            capOut.indices.push_back(t[2]);
        }
    }
    return true;
}

} // namespace

ClosedMeshCheck CheckClosedMesh(const FractureMesh& mesh)
{
    ClosedMeshCheck result;
    const std::vector<int32_t> weld = WeldedIds(mesh.verts);
    const size_t vcount = mesh.verts.size();

    struct Rec {
        int32_t lo, hi;
        int8_t dir; // +1: 元の向きが lo→hi、-1: hi→lo
    };
    std::vector<Rec> recs;
    const int32_t triCount = mesh.TriCount();
    recs.reserve(static_cast<size_t>(triCount) * 3);
    for (int32_t t = 0; t < triCount; ++t) {
        const int32_t raw[3] = { mesh.indices[static_cast<size_t>(t) * 3 + 0],
                                 mesh.indices[static_cast<size_t>(t) * 3 + 1],
                                 mesh.indices[static_cast<size_t>(t) * 3 + 2] };
        for (int k = 0; k < 3; ++k) {
            if (raw[k] < 0 || static_cast<size_t>(raw[k]) >= vcount) {
                continue; // 壊れた index は無視 (呼び出し側が保証すべきだが落ちない)
            }
        }
        const int32_t a = weld[static_cast<size_t>(raw[0])];
        const int32_t b = weld[static_cast<size_t>(raw[1])];
        const int32_t c = weld[static_cast<size_t>(raw[2])];
        const int32_t tri[3] = { a, b, c };
        for (int k = 0; k < 3; ++k) {
            const int32_t u = tri[k], v = tri[(k + 1) % 3];
            if (u == v) {
                continue; // 縮退辺は数えない
            }
            Rec r;
            r.lo = (u < v) ? u : v;
            r.hi = (u < v) ? v : u;
            r.dir = (u < v) ? 1 : -1;
            recs.push_back(r);
        }
    }
    std::sort(recs.begin(), recs.end(), [](const Rec& x, const Rec& y) {
        if (x.lo != y.lo) {
            return x.lo < y.lo;
        }
        if (x.hi != y.hi) {
            return x.hi < y.hi;
        }
        return x.dir < y.dir;
    });

    for (size_t i = 0; i < recs.size();) {
        size_t j = i + 1;
        while (j < recs.size() && recs[j].lo == recs[i].lo && recs[j].hi == recs[i].hi) {
            ++j;
        }
        const size_t count = j - i;
        if (count == 1) {
            ++result.boundaryEdges;
        } else if (count >= 3) {
            ++result.nonManifoldEdges;
        } else { // count == 2
            if (recs[i].dir == recs[i + 1].dir) {
                ++result.orientationMismatches;
            }
        }
        i = j;
    }
    result.closed
        = (result.boundaryEdges == 0 && result.nonManifoldEdges == 0 && result.orientationMismatches == 0);
    result.signedVolume = SignedVolume(mesh);
    return result;
}

void FlipMeshWinding(FractureMesh& mesh)
{
    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        std::swap(mesh.indices[i + 1], mesh.indices[i + 2]);
    }
}

namespace {

// 蓋の巻き順が外側面と噛み合っているかを、合成メッシュの閉じ判定そのもので検算する。
// 裏返して直す補正はしない: 向き不一致は CutMeshByPlane の前提 (外向きの閉じたメッシュ) が
// 破られている入力でも起き得るので、ここで黙って裏返すと本物のバグを隠してしまう。
// 閉じていると検証できなかった蓋は素直に失敗を返す
bool VerifyCapOrientation(const FractureMesh& outer, const FractureMesh& cap)
{
    if (cap.indices.empty()) {
        return true; // 蓋なし (平面がこの側に触れていない)
    }
    FractureMesh combined;
    combined.verts = outer.verts;
    combined.indices = outer.indices;
    const int32_t base = static_cast<int32_t>(combined.verts.size());
    combined.verts.insert(combined.verts.end(), cap.verts.begin(), cap.verts.end());
    for (int32_t idx : cap.indices) {
        combined.indices.push_back(idx + base);
    }
    return CheckClosedMesh(combined).closed;
}

} // namespace

bool CutMeshByPlane(const FractureMesh& mesh, const XMFLOAT3& planeNormal, float planeD,
                    PlaneCutResult& out)
{
    out = PlaneCutResult{};
    if (mesh.verts.empty() || mesh.indices.empty()) {
        out.success = true; // 空メッシュは両側とも空のまま
        return true;
    }

    const float nlen = Len3(FromF3(planeNormal));
    if (!(nlen > 1e-12f)) {
        out.failReason = "平面法線が縮退している";
        return false;
    }
    const V3 n = Mul3(FromF3(planeNormal), 1.0f / nlen);
    const XMFLOAT3 nf = ToF3(n);
    const float d = planeD;

    XMFLOAT3 lo = mesh.verts[0].position, hi = mesh.verts[0].position;
    for (const FractureVertex& v : mesh.verts) {
        lo.x = (std::min)(lo.x, v.position.x);
        lo.y = (std::min)(lo.y, v.position.y);
        lo.z = (std::min)(lo.z, v.position.z);
        hi.x = (std::max)(hi.x, v.position.x);
        hi.y = (std::max)(hi.y, v.position.y);
        hi.z = (std::max)(hi.z, v.position.z);
    }
    const float extent = (std::max)(hi.x - lo.x, (std::max)(hi.y - lo.y, hi.z - lo.z));
    // しきい値は形状の広がりに比例させる (ConvexHull.cpp と同じ流儀)
    const float eps = (std::max)(extent * 1e-5f, 1e-6f);

    std::vector<float> s(mesh.verts.size());
    std::vector<uint8_t> isPos(mesh.verts.size());
    bool anyPos = false, anyNeg = false;
    for (size_t i = 0; i < mesh.verts.size(); ++i) {
        const XMFLOAT3& p = mesh.verts[i].position;
        s[i] = nf.x * p.x + nf.y * p.y + nf.z * p.z - d;
        // 平面にほぼ乗る頂点は positive 側へ数える (一貫した側へ倒す規則)
        isPos[i] = (s[i] >= -eps) ? 1 : 0;
        if (isPos[i]) {
            anyPos = true;
        } else {
            anyNeg = true;
        }
    }

    if (!anyNeg) {
        out.positive.outer = mesh; // 切断なし。そのまま
        out.success = true;
        return true;
    }
    if (!anyPos) {
        out.negative.outer = mesh;
        out.success = true;
        return true;
    }

    const int32_t triCount = mesh.TriCount();
    for (int32_t t = 0; t < triCount; ++t) {
        const int32_t ia = mesh.indices[static_cast<size_t>(t) * 3 + 0];
        const int32_t ib = mesh.indices[static_cast<size_t>(t) * 3 + 1];
        const int32_t ic = mesh.indices[static_cast<size_t>(t) * 3 + 2];
        if (ia < 0 || ib < 0 || ic < 0 || static_cast<size_t>(ia) >= mesh.verts.size()
            || static_cast<size_t>(ib) >= mesh.verts.size()
            || static_cast<size_t>(ic) >= mesh.verts.size()) {
            out.failReason = "頂点 index が範囲外";
            return false;
        }
        const FractureVertex tri[3] = { mesh.verts[static_cast<size_t>(ia)],
                                        mesh.verts[static_cast<size_t>(ib)],
                                        mesh.verts[static_cast<size_t>(ic)] };
        const float ts[3] = { s[static_cast<size_t>(ia)], s[static_cast<size_t>(ib)],
                              s[static_cast<size_t>(ic)] };
        const bool tp[3] = { isPos[static_cast<size_t>(ia)] != 0, isPos[static_cast<size_t>(ib)] != 0,
                             isPos[static_cast<size_t>(ic)] != 0 };
        const int posCount = (tp[0] ? 1 : 0) + (tp[1] ? 1 : 0) + (tp[2] ? 1 : 0);
        if (posCount == 3) {
            AppendTriangle(out.positive.outer, tri[0], tri[1], tri[2]);
            continue;
        }
        if (posCount == 0) {
            AppendTriangle(out.negative.outer, tri[0], tri[1], tri[2]);
            continue;
        }

        std::vector<FractureVertex> posPoly, negPoly;
        int32_t crossCount = 0;
        for (int k = 0; k < 3; ++k) {
            const int kn = (k + 1) % 3;
            if (tp[k]) {
                posPoly.push_back(tri[k]);
            } else {
                negPoly.push_back(tri[k]);
            }
            if (tp[k] != tp[kn]) {
                const FractureVertex cp = ComputeCrossing(tri[k], ts[k], tri[kn], ts[kn]);
                posPoly.push_back(cp);
                negPoly.push_back(cp);
                ++crossCount;
            }
        }
        if (crossCount != 2) {
            out.failReason = "三角形の平面交差が想定外 (縮退した三角形の疑い)";
            return false;
        }
        // 断片自体の三角形分割 (ファン) は元の三角形の巻き順をそのまま保つので、断片の外向きは
        // 常に正しい。断面の輪郭は後で out.positive.outer / out.negative.outer 自身の
        // 境界辺から取り直す (ExtractBoundaryEdges)
        if (!FanTriangulate(posPoly, out.positive.outer) || !FanTriangulate(negPoly, out.negative.outer)) {
            out.failReason = "分割断片の三角形分割に失敗";
            return false;
        }
    }

    // 断面の輪郭は、切断でできた外側面の断片 (今はまだ蓋がなく「開いた」メッシュ) 自身の
    // 境界辺から取る。個々の三角形から新しい辺の向きを推測するより、外側面の巻き順という
    // 既に検証済みの正 (CheckClosedMesh と同じ考え方) をそのまま使うほうが頑健
    const std::vector<std::pair<FractureVertex, FractureVertex>> posSegs
        = ExtractBoundaryEdges(out.positive.outer);
    const std::vector<std::pair<FractureVertex, FractureVertex>> negSegs
        = ExtractBoundaryEdges(out.negative.outer);

    std::vector<std::vector<FractureVertex>> posLoops, negLoops;
    std::string chainFail;
    if (!ChainAllLoops(posSegs, posLoops, chainFail)) {
        out.failReason = "positive側の断面ループが閉じない: " + chainFail;
        return false;
    }
    if (!ChainAllLoops(negSegs, negLoops, chainFail)) {
        out.failReason = "negative側の断面ループが閉じない: " + chainFail;
        return false;
    }

    const XMFLOAT3 posCapNormal = { -nf.x, -nf.y, -nf.z };
    const XMFLOAT3 negCapNormal = nf;
    std::string capFail;
    if (!CapLoops(posLoops, posCapNormal, out.positive.cap, capFail)) {
        out.failReason = "positive側の蓋: " + capFail;
        return false;
    }
    if (!CapLoops(negLoops, negCapNormal, out.negative.cap, capFail)) {
        out.failReason = "negative側の蓋: " + capFail;
        return false;
    }
    if (!VerifyCapOrientation(out.positive.outer, out.positive.cap)) {
        out.failReason = "positive側の蓋: 外側面と閉じ合わない";
        return false;
    }
    if (!VerifyCapOrientation(out.negative.outer, out.negative.cap)) {
        out.failReason = "negative側の蓋: 外側面と閉じ合わない";
        return false;
    }

    out.success = true;
    return true;
}

double SignedVolume(const FractureMesh& mesh)
{
    double vol = 0.0;
    const int32_t triCount = mesh.TriCount();
    for (int32_t t = 0; t < triCount; ++t) {
        const int32_t ia = mesh.indices[static_cast<size_t>(t) * 3 + 0];
        const int32_t ib = mesh.indices[static_cast<size_t>(t) * 3 + 1];
        const int32_t ic = mesh.indices[static_cast<size_t>(t) * 3 + 2];
        const XMFLOAT3& p0 = mesh.verts[static_cast<size_t>(ia)].position;
        const XMFLOAT3& p1 = mesh.verts[static_cast<size_t>(ib)].position;
        const XMFLOAT3& p2 = mesh.verts[static_cast<size_t>(ic)].position;
        const double d0x = p0.x, d0y = p0.y, d0z = p0.z;
        const double d1x = p1.x, d1y = p1.y, d1z = p1.z;
        const double d2x = p2.x, d2y = p2.y, d2z = p2.z;
        vol += (d0x * (d1y * d2z - d1z * d2y) - d0y * (d1x * d2z - d1z * d2x)
               + d0z * (d1x * d2y - d1y * d2x))
             / 6.0;
    }
    return vol;
}

} // namespace mye
