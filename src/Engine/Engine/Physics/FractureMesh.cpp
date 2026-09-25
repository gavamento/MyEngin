//====================================================================================
//                          FractureMesh.cpp
//  MyEngin/ 秋田蓮音                                                     09/25/2026
//                                          破壊分割コアの実装: 閉じ判定・平面切断・蓋の三角形分割
//====================================================================================
#include "Engine/Engine/Physics/FractureMesh.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "libtess2/Include/tesselator.h"

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

// ---- 平面上の 2D 点 (蓋の三角形分割の入力・面積計算用) ----
struct Pt2 {
    float u = 0, v = 0;
};

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

// libtess2 (掃引線法、external/libtess2、SGI Free Software License B 2.0) の RAII ラッパー。
// 本エンジンは例外を使わないので、生成失敗は tess == nullptr で表す
struct TessHandle {
    TESStesselator* tess = tessNewTess(nullptr);
    TessHandle() = default;
    ~TessHandle()
    {
        if (tess != nullptr) {
            tessDeleteTess(tess);
        }
    }
    TessHandle(const TessHandle&) = delete;
    TessHandle& operator=(const TessHandle&) = delete;
};

// ループ列 (方向付きの単純閉曲線の集合) から蓋の三角形メッシュを作る。
// 外周/穴の判定と自己交差の解決は libtess2 の掃引線法 (TESS_WINDING_ODD、内包数の偶奇)
// に委ねる。輪郭が接触・重なる縮退入力 (薄い壁の断面など) でも O(n log n) で確定的に
// 処理できる — 耳切り (O(n^3)、単純多角形前提) の自前実装は、蓋の輪郭が接触・重なる
// 入力や高解像度ボクセル化の断面で 3 回同種の失敗を出した経緯がある (詳細は sub-14)。
// capNormal は蓋の外向き法線 (このまま出力頂点の法線になる)。hadNewVertices は、輪郭の
// 接触・交差で入力に無い頂点が新しくできたか (呼び出し側が閉じの検算方式を選ぶのに使う)
bool CapLoops(const std::vector<std::vector<FractureVertex>>& loops, const XMFLOAT3& capNormal,
             FractureMesh& capOut, bool& hadNewVertices, std::string& failReason)
{
    capOut = FractureMesh{};
    hadNewVertices = false;
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
            info.pts2.push_back({ u, w });
        }
        info.area = SignedArea2(info.pts2);
        maxAbsArea = (std::max)(maxAbsArea, std::fabs(info.area));
        infos.push_back(std::move(info));
    }
    const double areaEps = (std::max)(maxAbsArea * 1e-9, 1e-12);

    TessHandle handle;
    if (handle.tess == nullptr) {
        failReason = "libtess2 の初期化に失敗 (メモリ不足)";
        return false;
    }

    // libtess2 が振る出力頂点 index は、tessAddContour した順につながる連番になる
    // (tesselator.h の tessGetVertexIndices の仕様)。同じ順で flatInput/flatPts2 を積み、
    // 入力頂点由来の出力 (TESS_UNDEF でない) をそのまま引けるようにする。数値的なスリバー
    // (ChainAllLoops の交点計算の丸めで生じ得る面積ほぼ0のループ) だけは輪郭として渡さない
    // — 外周/穴そのものの分類は libtess2 に任せる
    std::vector<FractureVertex> flatInput;
    std::vector<Pt2> flatPts2;
    int32_t validLoopCount = 0;
    for (const LoopInfo& info : infos) {
        if (std::fabs(info.area) <= areaEps) {
            continue;
        }
        tessAddContour(handle.tess, 2, info.pts2.data(), static_cast<int>(sizeof(Pt2)),
                      static_cast<int>(info.pts2.size()));
        flatInput.insert(flatInput.end(), info.verts3.begin(), info.verts3.end());
        flatPts2.insert(flatPts2.end(), info.pts2.begin(), info.pts2.end());
        ++validLoopCount;
    }
    if (validLoopCount == 0) {
        return true; // 全部退化 = 蓋なし (安全側)
    }

    // 入力は (u,v) の平面内2D点として渡しているので、法線は常に (0,0,1) で固定する。
    // capNormal (3D) をそのまま渡すと、libtess2 内部の掃引平面への再投影が u,v の
    // 意味と噛み合わなくなる (z 成分が常に0のデータに無関係な3D法線を当てはめてしまう)
    const float flatNormal[3] = { 0.0f, 0.0f, 1.0f };
    if (!tessTesselate(handle.tess, TESS_WINDING_ODD, TESS_POLYGONS, 3, 2, flatNormal)) {
        const TESSstatus status = tessGetStatus(handle.tess);
        failReason = (status == TESS_STATUS_OUT_OF_MEMORY) ? "蓋の三角形分割に失敗 (メモリ不足)"
                                                            : "蓋の三角形分割に失敗 (libtess2、不正な入力)";
        return false;
    }

    const int32_t vertCount = tessGetVertexCount(handle.tess);
    const TESSreal* outVerts = tessGetVertices(handle.tess);
    const TESSindex* vertIdx = tessGetVertexIndices(handle.tess);

    // 出力頂点数が入力点数と食い違っていれば、輪郭が接触・交差した縮退入力だったと分かる。
    // 交差で新しい頂点が増える (TESS_UNDEF) だけでなく、位置が一致する入力点どうしを
    // libtess2 が黙って1つの頂点へ統合する場合もある (交点が生じない自己接触のケースで
    // 実測: 532 入力点 → 531 出力頂点、TESS_UNDEF は0件)。どちらも「入力の単純多角形の
    // 前提が崩れている」証拠なので、まとめて幾何的な閉じ判定へ倒す
    if (vertCount != static_cast<int32_t>(flatInput.size())) {
        hadNewVertices = true;
    }
    // オイラーの公式による三角形数の検算: 単純な輪郭群 (穴・輪郭どうしの自己交差なし) を
    // 三角形分割すると、頂点数 V・輪郭本数 L に対して必ず V + 2(L-1) - 2 枚になる
    // (L=1: V-2 の通常の単純多角形の式、L=2 の外周+穴でも実測どおり成立)。密なボクセル化
    // 断面のような大きい輪郭で、この枚数に届かない結果が実測で見つかった (原因未特定、
    // 自己交差・重複点のいずれの兆候もない)。式から外れること自体が「単純多角形の前提が
    // 実は崩れている」signal として使えるので、面が欠けたまま閉じ判定を通すより安全側へ倒す
    const int32_t expectedElemCount = vertCount + 2 * (validLoopCount - 1) - 2;
    if (tessGetElementCount(handle.tess) != expectedElemCount) {
        hadNewVertices = true;
    }

    // 平面上の任意の (u,v) から 3D 位置を厳密に復元するための平面オフセット。
    // (tangent, bitangent, capNormal) は直交基底なので、位置 p は
    // p = (p・tangent)*tangent + (p・bitangent)*bitangent + (p・capNormal)*capNormal と
    // 一意に分解できる。ループの頂点は全て同一平面上にあるので (p・capNormal) は共通の定数
    const XMFLOAT3& anyVert = flatInput[0].position;
    const float planeOffset
        = anyVert.x * capNormal.x + anyVert.y * capNormal.y + anyVert.z * capNormal.z;

    capOut.verts.reserve(static_cast<size_t>(vertCount));
    std::vector<int32_t> capVertOf(static_cast<size_t>(vertCount), -1);
    for (int32_t k = 0; k < vertCount; ++k) {
        FractureVertex fv;
        const TESSindex src = vertIdx[static_cast<size_t>(k)];
        if (src != TESS_UNDEF) {
            // 入力頂点そのまま。位置・UV は既存頂点とビット同一にするため、libtess2 が
            // 出す座標配列は経由せず元データを直接使う (外側面との厳密な閉じ判定は
            // 位置のビット一致溶接で行うため)
            fv = flatInput[static_cast<size_t>(src)];
            fv.normal = capNormal;
            fv.uv = { flatPts2[static_cast<size_t>(src)].u, flatPts2[static_cast<size_t>(src)].v };
        } else {
            // 輪郭どうしの交差で新しくできた頂点 (縮退入力でのみ発生。hadNewVertices は
            // 直前の頂点数比較で既に立っている)。法線は平面法線、位置と UV は libtess2 が
            // 出した (u,v) を平面へ逆射影して作る
            const float u = outVerts[static_cast<size_t>(k) * 2 + 0];
            const float v = outVerts[static_cast<size_t>(k) * 2 + 1];
            fv.position = { u * tangent.x + v * bitangent.x + planeOffset * capNormal.x,
                           u * tangent.y + v * bitangent.y + planeOffset * capNormal.y,
                           u * tangent.z + v * bitangent.z + planeOffset * capNormal.z };
            fv.normal = capNormal;
            fv.uv = { u, v };
        }
        capVertOf[static_cast<size_t>(k)] = static_cast<int32_t>(capOut.verts.size());
        capOut.verts.push_back(fv);
    }

    const int32_t elemCount = tessGetElementCount(handle.tess);
    const TESSindex* elems = tessGetElements(handle.tess);
    capOut.indices.reserve(static_cast<size_t>(elemCount) * 3);
    for (int32_t e = 0; e < elemCount; ++e) {
        const TESSindex a = elems[static_cast<size_t>(e) * 3 + 0];
        const TESSindex b = elems[static_cast<size_t>(e) * 3 + 1];
        const TESSindex c = elems[static_cast<size_t>(e) * 3 + 2];
        if (a == TESS_UNDEF || b == TESS_UNDEF || c == TESS_UNDEF) {
            failReason = "蓋の三角形分割が不完全な要素を返した (polySize=3 で TESS_UNDEF は想定外)";
            return false;
        }
        capOut.indices.push_back(capVertOf[static_cast<size_t>(a)]);
        capOut.indices.push_back(capVertOf[static_cast<size_t>(b)]);
        capOut.indices.push_back(capVertOf[static_cast<size_t>(c)]);
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

// 輪郭の接触・交差で蓋に新しい頂点ができた縮退入力向けの検算。位相的な閉じ (辺の
// 共有) までは求めず、体積が正で、面の欠けを示すベクトル面積の和が表面積の 1e-4 以下
// であることだけを見る (FractureBake.cpp の ValidatePieceGeometry と同じ式。破片の合否
// はもともとこの基準で判定しているので、下流への影響はない)
bool GeometricCapClosureValid(const FractureMesh& outer, const FractureMesh& cap)
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
    double vx = 0.0, vy = 0.0, vz = 0.0, surfaceArea = 0.0;
    const int32_t triCount = combined.TriCount();
    for (int32_t t = 0; t < triCount; ++t) {
        const XMFLOAT3& a = combined.verts[static_cast<size_t>(combined.indices[static_cast<size_t>(t) * 3 + 0])].position;
        const XMFLOAT3& b = combined.verts[static_cast<size_t>(combined.indices[static_cast<size_t>(t) * 3 + 1])].position;
        const XMFLOAT3& c = combined.verts[static_cast<size_t>(combined.indices[static_cast<size_t>(t) * 3 + 2])].position;
        const double e1x = static_cast<double>(b.x) - a.x, e1y = static_cast<double>(b.y) - a.y,
                    e1z = static_cast<double>(b.z) - a.z;
        const double e2x = static_cast<double>(c.x) - a.x, e2y = static_cast<double>(c.y) - a.y,
                    e2z = static_cast<double>(c.z) - a.z;
        const double cx = e1y * e2z - e1z * e2y, cy = e1z * e2x - e1x * e2z, cz = e1x * e2y - e1y * e2x;
        vx += cx * 0.5;
        vy += cy * 0.5;
        vz += cz * 0.5;
        surfaceArea += 0.5 * std::sqrt(cx * cx + cy * cy + cz * cz);
    }
    const double vectorAreaMag = std::sqrt(vx * vx + vy * vy + vz * vz);
    return (SignedVolume(combined) > 0.0) && (vectorAreaMag <= 1e-4 * surfaceArea);
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
    bool posHadNewVerts = false, negHadNewVerts = false;
    if (!CapLoops(posLoops, posCapNormal, out.positive.cap, posHadNewVerts, capFail)) {
        out.failReason = "positive側の蓋: " + capFail;
        return false;
    }
    if (!CapLoops(negLoops, negCapNormal, out.negative.cap, negHadNewVerts, capFail)) {
        out.failReason = "negative側の蓋: " + capFail;
        return false;
    }
    // 輪郭が接触・交差した縮退入力 (蓋に新しい頂点ができた側) だけ、厳密な位相的閉じの
    // 代わりに幾何的な閉じ (体積>0 かつベクトル面積の和が表面積の1e-4以下) で合否を決める。
    // 正常な入力 (新しい頂点0) では従来どおり厳密に閉じることを求める
    const bool posClosed = posHadNewVerts ? GeometricCapClosureValid(out.positive.outer, out.positive.cap)
                                          : VerifyCapOrientation(out.positive.outer, out.positive.cap);
    if (!posClosed) {
        out.failReason = "positive側の蓋: 外側面と閉じ合わない";
        return false;
    }
    const bool negClosed = negHadNewVerts ? GeometricCapClosureValid(out.negative.outer, out.negative.cap)
                                          : VerifyCapOrientation(out.negative.outer, out.negative.cap);
    if (!negClosed) {
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
