//====================================================================================
//                          FractureSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                     09/25/2026
//                                          破壊分割コア (M80a) のヘッドレス回帰テスト実装
//====================================================================================
#include "Engine/Engine/Physics/FractureSelfTest.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/Log.h"
#include "Engine/Engine/Physics/FractureMesh.h"

using namespace DirectX;

namespace mye {
namespace {

int32_t AddVert(FractureMesh& m, float x, float y, float z, float nx, float ny, float nz, float u,
                float v)
{
    FractureVertex vert;
    vert.position = { x, y, z };
    vert.normal = { nx, ny, nz };
    vert.uv = { u, v };
    m.verts.push_back(vert);
    return static_cast<int32_t>(m.verts.size()) - 1;
}

void Tri(FractureMesh& m, int32_t a, int32_t b, int32_t c)
{
    m.indices.push_back(a);
    m.indices.push_back(b);
    m.indices.push_back(c);
}

// 共有 8 頂点の箱 (半径 hx,hy,hz)。CCW 外向き
FractureMesh MakeBox(float hx, float hy, float hz)
{
    FractureMesh m;
    const int32_t p[8] = {
        AddVert(m, -hx, -hy, -hz, -1, -1, -1, 0, 0), AddVert(m, hx, -hy, -hz, 1, -1, -1, 0, 0),
        AddVert(m, hx, hy, -hz, 1, 1, -1, 0, 0),      AddVert(m, -hx, hy, -hz, -1, 1, -1, 0, 0),
        AddVert(m, -hx, -hy, hz, -1, -1, 1, 0, 0),    AddVert(m, hx, -hy, hz, 1, -1, 1, 0, 0),
        AddVert(m, hx, hy, hz, 1, 1, 1, 0, 0),         AddVert(m, -hx, hy, hz, -1, 1, 1, 0, 0),
    };
    auto quad = [&](int32_t a, int32_t b, int32_t c, int32_t d) {
        Tri(m, a, b, c);
        Tri(m, a, c, d);
    };
    quad(p[0], p[3], p[2], p[1]); // -Z
    quad(p[4], p[5], p[6], p[7]); // +Z
    quad(p[0], p[1], p[5], p[4]); // -Y
    quad(p[3], p[7], p[6], p[2]); // +Y
    quad(p[0], p[4], p[7], p[3]); // -X
    quad(p[1], p[2], p[6], p[5]); // +X
    return m;
}

// 面ごとに頂点を複製した箱 (UV継ぎ目で頂点が割れたモデルの模擬)。位置は共有箱と同一
FractureMesh MakeSeamedBox(float hx, float hy, float hz)
{
    FractureMesh m;
    auto face = [&](XMFLOAT3 a, XMFLOAT3 b, XMFLOAT3 c, XMFLOAT3 d, XMFLOAT3 n) {
        const int32_t ia = AddVert(m, a.x, a.y, a.z, n.x, n.y, n.z, 0, 0);
        const int32_t ib = AddVert(m, b.x, b.y, b.z, n.x, n.y, n.z, 1, 0);
        const int32_t ic = AddVert(m, c.x, c.y, c.z, n.x, n.y, n.z, 1, 1);
        const int32_t id = AddVert(m, d.x, d.y, d.z, n.x, n.y, n.z, 0, 1);
        Tri(m, ia, ib, ic);
        Tri(m, ia, ic, id);
    };
    const XMFLOAT3 c000{ -hx, -hy, -hz }, c100{ hx, -hy, -hz }, c110{ hx, hy, -hz },
        c010{ -hx, hy, -hz }, c001{ -hx, -hy, hz }, c101{ hx, -hy, hz }, c111{ hx, hy, hz },
        c011{ -hx, hy, hz };
    face(c000, c010, c110, c100, { 0, 0, -1 });
    face(c001, c101, c111, c011, { 0, 0, 1 });
    face(c000, c100, c101, c001, { 0, -1, 0 });
    face(c010, c011, c111, c110, { 0, 1, 0 });
    face(c000, c001, c011, c010, { -1, 0, 0 });
    face(c100, c110, c111, c101, { 1, 0, 0 });
    return m;
}

// 蓋のない箱 (+Z 面を欠く。境界辺 4 本)
FractureMesh MakeOpenBox(float hx, float hy, float hz)
{
    FractureMesh m;
    const int32_t p[8] = {
        AddVert(m, -hx, -hy, -hz, 0, 0, 0, 0, 0), AddVert(m, hx, -hy, -hz, 0, 0, 0, 0, 0),
        AddVert(m, hx, hy, -hz, 0, 0, 0, 0, 0),   AddVert(m, -hx, hy, -hz, 0, 0, 0, 0, 0),
        AddVert(m, -hx, -hy, hz, 0, 0, 0, 0, 0),  AddVert(m, hx, -hy, hz, 0, 0, 0, 0, 0),
        AddVert(m, hx, hy, hz, 0, 0, 0, 0, 0),     AddVert(m, -hx, hy, hz, 0, 0, 0, 0, 0),
    };
    auto quad = [&](int32_t a, int32_t b, int32_t c, int32_t d) {
        Tri(m, a, b, c);
        Tri(m, a, c, d);
    };
    quad(p[0], p[3], p[2], p[1]); // -Z
    // +Z を作らない (穴)
    quad(p[0], p[1], p[5], p[4]); // -Y
    quad(p[3], p[7], p[6], p[2]); // +Y
    quad(p[0], p[4], p[7], p[3]); // -X
    quad(p[1], p[2], p[6], p[5]); // +X
    return m;
}

// 1 枚の平面 (quad)。境界辺 4 本
FractureMesh MakePlaneQuad(float half)
{
    FractureMesh m;
    const int32_t a = AddVert(m, -half, 0, -half, 0, 1, 0, 0, 0);
    const int32_t b = AddVert(m, half, 0, -half, 0, 1, 0, 1, 0);
    const int32_t c = AddVert(m, half, 0, half, 0, 1, 0, 1, 1);
    const int32_t d = AddVert(m, -half, 0, half, 0, 1, 0, 0, 1);
    Tri(m, a, b, c);
    Tri(m, a, c, d);
    return m;
}

// 辺を 3 枚の三角形で共有する非多様体メッシュ (箱 + 余分なフラップ)
FractureMesh MakeNonManifold(float hx, float hy, float hz)
{
    FractureMesh m = MakeBox(hx, hy, hz);
    // 既存の辺 (0,1) (-Z 面と -Y 面で共有済み) に、外へ張り出す第3の三角形を足す
    const int32_t extra = AddVert(m, 0, -hy * 2.0f, -hz, 0, -1, 0, 0, 0);
    Tri(m, 0, 1, extra);
    return m;
}

// -Z 面だけ巻き順を反転した箱 (向き不一致)
FractureMesh MakeFlippedFaceBox(float hx, float hy, float hz)
{
    FractureMesh m = MakeBox(hx, hy, hz);
    // MakeBox の先頭 2 三角形が -Z 面 (quad(p0,p3,p2,p1) → tri(0,3,2), tri(0,2,1))
    std::swap(m.indices[1], m.indices[2]);
    std::swap(m.indices[4], m.indices[5]);
    return m;
}

// 凹んだ L 字の三角柱 (XY 断面が L 字、Z 方向に depth 押し出し)。
// 断面の頂点は 6 個 (A,B,C,D,E,F)。7 個目の頂点 H=(0,1) を A-F 辺の分割点として足す案は
// 捨てた — H は A と F を結ぶ直線 (x=0) の上に乗るため、扇の最後の三角形 (A,F,H) が
// 面積 0 に潰れ、さらに平面で切ったときに同じ交点が 2 つの異なる辺として現れて破綻した
// (実測で確認済み)。6 頂点なら A から見通せない頂点がなく (star-shaped)、共線の三角形もない
FractureMesh MakeLShapePrism(float depth)
{
    const XMFLOAT2 profile[6] = {
        { 0, 0 }, { 2, 0 }, { 2, 1 }, { 1, 1 }, { 1, 2 }, { 0, 2 },
    };
    constexpr int32_t n = 6;
    FractureMesh m;
    int32_t bot[n], top[n];
    for (int32_t i = 0; i < n; ++i) {
        bot[i] = AddVert(m, profile[i].x, profile[i].y, 0.0f, 0, 0, -1, profile[i].x, profile[i].y);
    }
    for (int32_t i = 0; i < n; ++i) {
        top[i] = AddVert(m, profile[i].x, profile[i].y, depth, 0, 0, 1, profile[i].x, profile[i].y);
    }
    for (int32_t i = 0; i < n; ++i) {
        const int32_t j = (i + 1) % n;
        Tri(m, bot[i], bot[j], top[j]);
        Tri(m, bot[i], top[j], top[i]);
    }
    // 上面 (+Z) / 底面 (-Z): A (index0) からの扇
    for (int32_t i = 1; i + 1 < n; ++i) {
        Tri(m, top[0], top[i], top[i + 1]);
        Tri(m, bot[0], bot[i + 1], bot[i]);
    }
    return m;
}

// トーラス (軸 = Y)。majorSeg × minorSeg の格子で分割し、各セルを 2 三角形にする
FractureMesh MakeTorus(float majorR, float minorR, int32_t majorSeg, int32_t minorSeg)
{
    FractureMesh m;
    std::vector<int32_t> ids(static_cast<size_t>(majorSeg) * static_cast<size_t>(minorSeg));
    for (int32_t i = 0; i < majorSeg; ++i) {
        const float theta = 2.0f * XM_PI * static_cast<float>(i) / static_cast<float>(majorSeg);
        const float ct = std::cos(theta), st = std::sin(theta);
        for (int32_t j = 0; j < minorSeg; ++j) {
            const float phi = 2.0f * XM_PI * static_cast<float>(j) / static_cast<float>(minorSeg);
            const float cp = std::cos(phi), sp = std::sin(phi);
            const float ringR = majorR + minorR * cp;
            const float x = ringR * ct;
            const float y = minorR * sp;
            const float z = ringR * st;
            const float nx = cp * ct, ny = sp, nz = cp * st;
            ids[static_cast<size_t>(i) * static_cast<size_t>(minorSeg) + static_cast<size_t>(j)]
                = AddVert(m, x, y, z, nx, ny, nz, theta, phi);
        }
    }
    auto at = [&](int32_t i, int32_t j) {
        i = (i % majorSeg + majorSeg) % majorSeg;
        j = (j % minorSeg + minorSeg) % minorSeg;
        return ids[static_cast<size_t>(i) * static_cast<size_t>(minorSeg) + static_cast<size_t>(j)];
    };
    for (int32_t i = 0; i < majorSeg; ++i) {
        for (int32_t j = 0; j < minorSeg; ++j) {
            const int32_t a = at(i, j), b = at(i + 1, j), c = at(i + 1, j + 1), d = at(i, j + 1);
            // a→b は ∂/∂θ、a→d は ∂/∂φ 方向。∂θ×∂φ は管の中心へ向く内向きなので、
            // 外向きにするには a→d→b→c 側 (∂φ×∂θ) の巻きにする
            Tri(m, a, c, b);
            Tri(m, a, d, c);
        }
    }
    return m;
}

std::vector<uint8_t> SerializeMesh(const FractureMesh& mesh)
{
    std::vector<uint8_t> out;
    auto append = [&](const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        out.insert(out.end(), b, b + n);
    };
    const uint32_t vc = static_cast<uint32_t>(mesh.verts.size());
    const uint32_t ic = static_cast<uint32_t>(mesh.indices.size());
    append(&vc, sizeof(vc));
    append(&ic, sizeof(ic));
    if (vc > 0) {
        append(mesh.verts.data(), mesh.verts.size() * sizeof(FractureVertex));
    }
    if (ic > 0) {
        append(mesh.indices.data(), mesh.indices.size() * sizeof(int32_t));
    }
    return out;
}

std::vector<uint8_t> SerializeCutResult(const PlaneCutResult& r)
{
    std::vector<uint8_t> out;
    auto app = [&](const std::vector<uint8_t>& v) { out.insert(out.end(), v.begin(), v.end()); };
    const uint8_t ok = r.success ? 1 : 0;
    out.push_back(ok);
    app(SerializeMesh(r.positive.outer));
    app(SerializeMesh(r.positive.cap));
    app(SerializeMesh(r.negative.outer));
    app(SerializeMesh(r.negative.cap));
    return out;
}

// 蓋込みで閉じているか (外側面+蓋を連結して判定)
bool SideIsClosed(const PlaneCutSide& side)
{
    FractureMesh combined;
    combined.verts = side.outer.verts;
    combined.indices = side.outer.indices;
    const int32_t base = static_cast<int32_t>(combined.verts.size());
    combined.verts.insert(combined.verts.end(), side.cap.verts.begin(), side.cap.verts.end());
    for (int32_t idx : side.cap.indices) {
        combined.indices.push_back(idx + base);
    }
    const ClosedMeshCheck c = CheckClosedMesh(combined);
    if (!c.closed) {
        MYE_LOG_ERROR("    (SideIsClosed detail: boundary=%d nonManifold=%d mismatch=%d)",
                     c.boundaryEdges, c.nonManifoldEdges, c.orientationMismatches);
    }
    return c.closed;
}

double SideVolume(const PlaneCutSide& side)
{
    return SignedVolume(side.outer) + SignedVolume(side.cap);
}

bool Near(double a, double b, double relTol, double absTol = 1e-9)
{
    return std::fabs(a - b) <= absTol + relTol * std::fabs(b);
}

} // namespace

bool RunFractureSelfTest()
{
    MYE_LOG_INFO("==== Fracture mesh core (M80a) self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    // ---- 1. 閉じ判定 ----
    // 平面切断の入力は「閉じていて外向き (signedVolume > 0)」が前提。ここで全テスト入力
    // メッシュの符号付き体積を検査し、生成メッシュが内向きに巻かれる事故を機械的に防ぐ
    {
        const ClosedMeshCheck box = CheckClosedMesh(MakeBox(1, 1, 1));
        check(box.closed, "box: closed");
        check(box.signedVolume > 0.0, "box: outward (signedVolume > 0)");

        const ClosedMeshCheck torus = CheckClosedMesh(MakeTorus(2.0f, 0.6f, 24, 16));
        check(torus.closed, "torus: closed");
        check(torus.signedVolume > 0.0, "torus: outward (signedVolume > 0)");

        const ClosedMeshCheck seamed = CheckClosedMesh(MakeSeamedBox(1, 1, 1));
        check(seamed.closed, "seamed box (UV継ぎ目で頂点が割れた箱): closed");
        check(seamed.signedVolume > 0.0, "seamed box: outward (signedVolume > 0)");

        const ClosedMeshCheck lshape = CheckClosedMesh(MakeLShapePrism(1.0f));
        check(lshape.closed, "lshape prism: closed");
        check(lshape.signedVolume > 0.0, "lshape prism: outward (signedVolume > 0)");

        const ClosedMeshCheck openBox = CheckClosedMesh(MakeOpenBox(1, 1, 1));
        check(!openBox.closed && openBox.boundaryEdges == 4, "open box: 4 boundary edges");

        const ClosedMeshCheck quad = CheckClosedMesh(MakePlaneQuad(1));
        check(!quad.closed && quad.boundaryEdges == 4, "plane quad: 4 boundary edges");

        const ClosedMeshCheck nonManifold = CheckClosedMesh(MakeNonManifold(1, 1, 1));
        check(!nonManifold.closed && nonManifold.nonManifoldEdges >= 1,
              "non-manifold (edge shared by 3 faces): detected");

        const ClosedMeshCheck flipped = CheckClosedMesh(MakeFlippedFaceBox(1, 1, 1));
        check(!flipped.closed && flipped.orientationMismatches >= 1,
              "flipped face box: orientation mismatch detected");
    }

    // ---- 1b. 内向きメッシュの正規化: 全面が内向きに巻かれた閉じたメッシュを検出し、
    // FlipMeshWinding で外向きへ正規化してから切ると両側とも外向きで閉じる ----
    {
        FractureMesh inwardTorus = MakeTorus(2.0f, 0.6f, 24, 16);
        FlipMeshWinding(inwardTorus); // わざと内向きにする
        const ClosedMeshCheck inwardCheck = CheckClosedMesh(inwardTorus);
        check(inwardCheck.closed && inwardCheck.signedVolume < 0.0,
              "inward torus: closed but signedVolume < 0 (detected)");

        FlipMeshWinding(inwardTorus); // 正規化
        const ClosedMeshCheck normalizedCheck = CheckClosedMesh(inwardTorus);
        check(normalizedCheck.closed && normalizedCheck.signedVolume > 0.0,
              "inward torus: FlipMeshWinding normalizes to outward");

        PlaneCutResult r;
        const bool ok = CutMeshByPlane(inwardTorus, { 1, 0, 0 }, 0.5f, r);
        check(ok && r.success, "normalized torus: CutMeshByPlane succeeds");
        if (ok && r.success) {
            auto sideOutward = [](const PlaneCutSide& side) {
                FractureMesh combined;
                combined.verts = side.outer.verts;
                combined.indices = side.outer.indices;
                const int32_t base = static_cast<int32_t>(combined.verts.size());
                combined.verts.insert(combined.verts.end(), side.cap.verts.begin(),
                                      side.cap.verts.end());
                for (int32_t idx : side.cap.indices) {
                    combined.indices.push_back(idx + base);
                }
                const ClosedMeshCheck c = CheckClosedMesh(combined);
                return c.closed && c.signedVolume > 0.0;
            };
            check(sideOutward(r.positive), "normalized torus: positive side closed and outward");
            check(sideOutward(r.negative), "normalized torus: negative side closed and outward");
        }
    }

    // ---- 2. 平面切断 + 蓋: 箱・L字・トーラスを複数平面で切り、両側の閉じ判定と体積和を確認 ----
    auto testCut = [&](const char* label, const FractureMesh& mesh, XMFLOAT3 n, float d) {
        char volBuf[256];
        std::snprintf(volBuf, sizeof(volBuf), "%s: input mesh is outward (signedVolume > 0)", label);
        check(SignedVolume(mesh) > 0.0, volBuf);
        const float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
        n = { n.x / len, n.y / len, n.z / len };
        d /= len; // n・x = d は n の縮尺に依存するので、正規化に合わせて d も縮める
        PlaneCutResult r;
        const bool ok = CutMeshByPlane(mesh, n, d, r);
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s: CutMeshByPlane succeeds", label);
        check(ok && r.success, buf);
        if (!ok || !r.success) {
            MYE_LOG_ERROR("    reason: %s", r.failReason.c_str());
            return;
        }
        std::snprintf(buf, sizeof(buf), "%s: positive side closed", label);
        check(SideIsClosed(r.positive), buf);
        std::snprintf(buf, sizeof(buf), "%s: negative side closed", label);
        check(SideIsClosed(r.negative), buf);
        const double total = SignedVolume(mesh);
        const double sum = SideVolume(r.positive) + SideVolume(r.negative);
        std::snprintf(buf, sizeof(buf), "%s: volume conserved (%.6f vs %.6f)", label, sum, total);
        check(Near(sum, total, 1e-4, 1e-7), buf);
    };

    {
        const FractureMesh box = MakeBox(1, 1, 1);
        testCut("box/axis-aligned", box, { 1, 0, 0 }, 0.0f);
        testCut("box/oblique", box, { 1, 1, 1 }, 0.3f);
        testCut("box/near-corner", box, { 0, 1, 0 }, 0.9f);

        const FractureMesh lshape = MakeLShapePrism(1.0f);
        // 凹んだ角の近くを横切る (頂点をちょうど通る配置は §4 のエッジケースで別途確認する)
        testCut("lshape/axis-aligned", lshape, { 1, 0, 0 }, 0.85f);
        testCut("lshape/oblique", lshape, { 1, 1, 0 }, 1.2f);

        const FractureMesh torus = MakeTorus(2.0f, 0.6f, 28, 20);
        testCut("torus/axis-aligned", torus, { 1, 0, 0 }, 0.5f);
        testCut("torus/oblique", torus, { 0.3f, 1, 0.2f }, 0.8f);
    }

    // ---- 3. トーラスの輪切り (穴のある蓋) ----
    {
        const float majorR = 2.0f, minorR = 0.6f;
        // minorSeg は輪切り面 (外周・内周) の円弧近似の細かさを決める。線形補間による誤差は
        // (2π/minorSeg)^2/8 程度で減るので、相対 1e-3 の許容に対して 96 分割の余裕を見る
        const int32_t majorSeg = 48, minorSeg = 96;
        const FractureMesh torus = MakeTorus(majorR, minorR, majorSeg, minorSeg);
        check(SignedVolume(torus) > 0.0, "torus slice: input mesh is outward (signedVolume > 0)");
        const float k = 0.2f; // 赤道からずらした高さ (軸 = Y)
        PlaneCutResult r;
        const bool ok = CutMeshByPlane(torus, { 0, 1, 0 }, k, r);
        check(ok && r.success, "torus slice (annulus): CutMeshByPlane succeeds");
        if (!ok || !r.success) {
            MYE_LOG_ERROR("    reason: %s", r.failReason.c_str());
        }
        if (ok && r.success) {
            check(SideIsClosed(r.positive), "torus slice: positive side closed");
            check(SideIsClosed(r.negative), "torus slice: negative side closed");
            const double total = SignedVolume(torus);
            const double sum = SideVolume(r.positive) + SideVolume(r.negative);
            check(Near(sum, total, 1e-4, 1e-7), "torus slice: volume conserved");

            // 蓋 (穴あき) の面積 = 外周 majorSeg 角形 − 内周 majorSeg 角形の面積 (多角形近似)
            const double half = std::sqrt(static_cast<double>(minorR) * minorR
                                          - static_cast<double>(k) * k);
            const double outerR = majorR + half;
            const double innerR = majorR - half;
            const auto regularNGonArea = [&](double radius) {
                return 0.5 * majorSeg * radius * radius * std::sin(2.0 * XM_PI / majorSeg);
            };
            const double expectedCapArea = regularNGonArea(outerR) - regularNGonArea(innerR);
            double negCapArea = 0.0;
            for (int32_t t = 0; t < r.negative.cap.TriCount(); ++t) {
                const XMFLOAT3& p0 = r.negative.cap.verts[static_cast<size_t>(r.negative.cap.indices[static_cast<size_t>(t) * 3 + 0])].position;
                const XMFLOAT3& p1 = r.negative.cap.verts[static_cast<size_t>(r.negative.cap.indices[static_cast<size_t>(t) * 3 + 1])].position;
                const XMFLOAT3& p2 = r.negative.cap.verts[static_cast<size_t>(r.negative.cap.indices[static_cast<size_t>(t) * 3 + 2])].position;
                const XMFLOAT3 e1 = { p1.x - p0.x, p1.y - p0.y, p1.z - p0.z };
                const XMFLOAT3 e2 = { p2.x - p0.x, p2.y - p0.y, p2.z - p0.z };
                const XMFLOAT3 cr = { e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z,
                                      e1.x * e2.y - e1.y * e2.x };
                negCapArea += 0.5 * std::sqrt(static_cast<double>(cr.x) * cr.x
                                              + static_cast<double>(cr.y) * cr.y
                                              + static_cast<double>(cr.z) * cr.z);
            }
            char buf[256];
            std::snprintf(buf, sizeof(buf), "torus slice: hole cap area matches annulus (%.6f vs %.6f)",
                          negCapArea, expectedCapArea);
            check(Near(negCapArea, expectedCapArea, 1e-3, 1e-6), buf);
        }
    }

    // ---- 4. エッジケース ----
    {
        const FractureMesh box = MakeBox(1, 1, 1);
        // 平面がメッシュに触れない (全部片側)
        PlaneCutResult r;
        bool ok = CutMeshByPlane(box, { 0, 1, 0 }, 5.0f, r);
        check(ok && r.success && r.negative.outer.TriCount() == box.TriCount()
                  && r.positive.outer.TriCount() == 0 && r.positive.cap.TriCount() == 0,
              "plane misses mesh entirely: negative side unchanged, positive side empty");

        // 面の対角 (頂点をちょうど通る斜め平面) — 呼び出し自体は落ちない (成功/失敗どちらも許容)。
        // 成功したときだけ、閉じた結果になっていることを確認する
        PlaneCutResult r2;
        ok = CutMeshByPlane(box, { 1, 1, 0 }, 0.0f, r2);
        if (!ok) {
            MYE_LOG_INFO("    (plane through face corners: 安全側に倒して失敗。reason=%s)",
                        r2.failReason.c_str());
        }
        check(!ok || (SideIsClosed(r2.positive) && SideIsClosed(r2.negative)),
              "plane through face corners: succeeds with closed sides, or fails safely");

        // 面上に乗る平面 (箱の +X 面とちょうど一致) — 同様に成功/失敗どちらも許容
        PlaneCutResult r3;
        ok = CutMeshByPlane(box, { 1, 0, 0 }, 1.0f, r3);
        if (!ok) {
            MYE_LOG_INFO("    (plane on face: 安全側に倒して失敗。reason=%s)", r3.failReason.c_str());
        }
        bool r3Ok = !ok;
        if (ok) {
            const double total = SignedVolume(box);
            const double sum = SideVolume(r3.positive) + SideVolume(r3.negative);
            r3Ok = Near(sum, total, 1e-3, 1e-6);
        }
        check(r3Ok, "plane on face: succeeds with volume conserved, or fails safely");
    }

    // ---- 5. 決定論: 同じ入力を2回切って出力のバイト列が一致する ----
    {
        const FractureMesh torus = MakeTorus(2.0f, 0.6f, 28, 20);
        PlaneCutResult a, b;
        CutMeshByPlane(torus, { 0.3f, 1, 0.2f }, 0.8f, a);
        CutMeshByPlane(torus, { 0.3f, 1, 0.2f }, 0.8f, b);
        const std::vector<uint8_t> ba = SerializeCutResult(a);
        const std::vector<uint8_t> bb = SerializeCutResult(b);
        check(ba.size() == bb.size() && std::memcmp(ba.data(), bb.data(), ba.size()) == 0,
              "determinism: cutting the same input twice yields byte-identical output");
    }

    // ---- 6. 処理時間の記録 (sub-02 の焼き時間見積もり用。合否には数えない) ----
    {
        const FractureMesh big = MakeTorus(2.0f, 0.6f, 100, 50); // 100*50*2 = 10000 三角形
        const auto t0 = std::chrono::steady_clock::now();
        for (int32_t i = 0; i < 16; ++i) {
            const float a = static_cast<float>(i) * 0.4f;
            PlaneCutResult r;
            CutMeshByPlane(big, { std::sin(a), std::cos(a) * 0.5f, std::cos(a) }, 0.3f, r);
        }
        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        MYE_LOG_INFO("  timing: %d tri x 16 planes = %.2f ms (%.3f ms/cut)", big.TriCount(), ms,
                    ms / 16.0);
    }

    if (failCount == 0) {
        MYE_LOG_INFO("==== Fracture mesh core self test: ALL PASS ====");
    } else {
        MYE_LOG_ERROR("==== Fracture mesh core self test: %d FAILURE(S) ====", failCount);
    }
    return failCount == 0;
}

} // namespace mye
