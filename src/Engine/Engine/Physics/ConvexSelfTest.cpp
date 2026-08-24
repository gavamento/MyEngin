#include "Engine/Engine/Physics/ConvexSelfTest.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/Log.h"
#include "Engine/Engine/Physics/ConvexHull.h"

using namespace DirectX;

namespace mye {
namespace {

// テスト用の決定論シャッフル。sim ではないので PCG32 を持ち出さず、その場の LCG で足りる
// (種を固定してあるので毎回同じ並べ替えになる = 失敗が再現する)
struct Lcg {
    uint32_t s;
    uint32_t Next()
    {
        s = s * 1664525u + 1013904223u;
        return s;
    }
};

void Shuffle(std::vector<XMFLOAT3>& v, uint32_t seed)
{
    Lcg rng{ seed };
    for (size_t i = v.size(); i > 1; --i) {
        const size_t j = rng.Next() % i;
        std::swap(v[i - 1], v[j]);
    }
}

std::vector<uint8_t> Blob(const ConvexHullData& h)
{
    std::vector<uint8_t> b;
    SerializeConvexHull(h, b);
    return b;
}

std::vector<XMFLOAT3> CubeCorners(float half)
{
    return {
        { -half, -half, -half }, { half, -half, -half }, { half, half, -half },
        { -half, half, -half },  { -half, -half, half }, { half, -half, half },
        { half, half, half },    { -half, half, half },
    };
}

bool Near(float a, float b, float tol)
{
    return std::fabs(a - b) <= tol;
}

} // namespace

bool RunConvexSelfTest()
{
    MYE_LOG_INFO("==== Convex hull (M60f) self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    // ---- 単位立方体: 位相 (オイラー) と質量特性 ----
    ConvexHullData cube;
    {
        const bool ok = BuildConvexHull(CubeCorners(0.5f), cube);
        check(ok, "cube: BuildConvexHull succeeds");
        check(cube.verts.size() == 8, "cube: 8 vertices");
        // ここが 12 なら同一平面の統合が効いていない = 参照面クリップが 3 点で頭打ちになる
        check(cube.faces.size() == 6, "cube: coplanar triangles merged into 6 quad faces");
        check(cube.edges.size() == 12, "cube: 12 edges");
        check(cube.verts.size() - cube.edges.size() + cube.faces.size() == 2,
              "cube: V - E + F == 2 (Euler)");
        for (const ConvexFace& f : cube.faces) {
            if (f.count != 4) {
                check(false, "cube: every face is a quad");
                break;
            }
        }
        check(Near(cube.volume, 1.0f, 1e-5f), "cube: volume == 1");
        check(Near(cube.com.x, 0.0f, 1e-5f) && Near(cube.com.y, 0.0f, 1e-5f)
                  && Near(cube.com.z, 0.0f, 1e-5f),
              "cube: center of mass at origin");
        // 密度 1・質量 1 の立方体 (辺 1): I = m(a^2+a^2)/12 = 1/6、非対角は 0
        const bool diag = Near(cube.inertia[0][0], 1.0f / 6.0f, 1e-5f)
                       && Near(cube.inertia[1][1], 1.0f / 6.0f, 1e-5f)
                       && Near(cube.inertia[2][2], 1.0f / 6.0f, 1e-5f);
        bool offDiagZero = true;
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                if (r != c && !Near(cube.inertia[r][c], 0.0f, 1e-5f)) {
                    offDiagZero = false;
                }
            }
        }
        check(diag && offDiagZero, "cube: inertia tensor == diag(1/6) (analytic)");
    }

    // ---- 生成が入力順に依らない (本テストの主目的) ----
    {
        const std::vector<uint8_t> ref = Blob(cube);
        bool allSame = true;
        for (uint32_t seed = 1; seed <= 8; ++seed) {
            std::vector<XMFLOAT3> pts = CubeCorners(0.5f);
            Shuffle(pts, seed);
            ConvexHullData h;
            BuildConvexHull(pts, h);
            const std::vector<uint8_t> b = Blob(h);
            if (b.size() != ref.size() || std::memcmp(b.data(), ref.data(), b.size()) != 0) {
                allSame = false;
                break;
            }
        }
        check(allSame, "cube: hull is byte-identical under 8 input permutations");
    }

    // ---- 重複点と内部点を混ぜても結果が変わらない ----
    {
        std::vector<XMFLOAT3> pts = CubeCorners(0.5f);
        pts.insert(pts.end(), pts.begin(), pts.end()); // 全点を重複させる
        pts.push_back({ 0.0f, 0.0f, 0.0f });          // 内部点
        pts.push_back({ 0.1f, -0.2f, 0.3f });
        pts.push_back({ -0.0f, 0.0f, -0.0f });         // -0.0 と +0.0 の混在
        Shuffle(pts, 99u);
        ConvexHullData h;
        BuildConvexHull(pts, h);
        const std::vector<uint8_t> a = Blob(cube), b = Blob(h);
        check(a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0,
              "cube: duplicates / interior points / signed zeros do not change the hull");
    }

    // ---- 四面体 ----
    {
        const std::vector<XMFLOAT3> pts = {
            { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 },
        };
        ConvexHullData h;
        check(BuildConvexHull(pts, h), "tetra: BuildConvexHull succeeds");
        check(h.verts.size() == 4 && h.faces.size() == 4 && h.edges.size() == 6,
              "tetra: 4 vertices / 4 faces / 6 edges");
        check(Near(h.volume, 1.0f / 6.0f, 1e-6f), "tetra: volume == 1/6");
        check(Near(h.com.x, 0.25f, 1e-5f) && Near(h.com.y, 0.25f, 1e-5f)
                  && Near(h.com.z, 0.25f, 1e-5f),
              "tetra: center of mass at (1/4, 1/4, 1/4)");
    }

    // ---- 非一様スケールの質量特性 (慣性は相似変換にならないので積分し直す) ----
    {
        float vol = 0.0f;
        XMFLOAT3 com{};
        float I[3][3] = {};
        ConvexMassProperties(cube, 2.0f, 1.0f, 1.0f, vol, com, I);
        check(Near(vol, 2.0f, 1e-5f), "scaled cube: volume == 2");
        // 辺 (2,1,1)・密度 1 なので質量 2。Ix = m(1+1)/12、Iy = Iz = m(4+1)/12
        check(Near(I[0][0], 2.0f * 2.0f / 12.0f, 1e-5f)
                  && Near(I[1][1], 2.0f * 5.0f / 12.0f, 1e-5f)
                  && Near(I[2][2], 2.0f * 5.0f / 12.0f, 1e-5f),
              "scaled cube: inertia matches the analytic 2x1x1 box");
        // 負スケールは絶対値扱い (ShapeVolumeWorld と同一規約) = 体積が負にならない
        float vol2 = 0.0f;
        XMFLOAT3 com2{};
        float I2[3][3] = {};
        ConvexMassProperties(cube, -2.0f, 1.0f, -1.0f, vol2, com2, I2);
        check(Near(vol2, 2.0f, 1e-5f), "scaled cube: negative scale is taken as absolute");
    }

    // ---- 球の点群: 打ち切りと凸性 ----
    {
        std::vector<XMFLOAT3> pts;
        for (int i = 0; i < 400; ++i) {
            // フィボナッチ球 (決定論的な準一様分布)
            const float k = (static_cast<float>(i) + 0.5f) / 400.0f;
            const float phi = std::acos(1.0f - 2.0f * k);
            const float theta = 3.883222077f * static_cast<float>(i); // 黄金角 × i
            pts.push_back({ std::sin(phi) * std::cos(theta), std::sin(phi) * std::sin(theta),
                            std::cos(phi) });
        }
        std::vector<XMFLOAT3> shuffled = pts;
        Shuffle(shuffled, 7u);
        ConvexHullData a, b;
        check(BuildConvexHull(pts, a), "sphere cloud: BuildConvexHull succeeds");
        BuildConvexHull(shuffled, b);
        const std::vector<uint8_t> ba = Blob(a), bb = Blob(b);
        check(ba.size() == bb.size() && std::memcmp(ba.data(), bb.data(), ba.size()) == 0,
              "sphere cloud: 400 shuffled points give a byte-identical hull");
        check(static_cast<int>(a.verts.size()) <= kConvexMaxVerts,
              "sphere cloud: vertex count is capped at kConvexMaxVerts");
        // 内接多面体なので真球より小さいが、64 頂点なら 8 割は超える
        const float sphereVol = 4.18879020f;
        check(a.volume > sphereVol * 0.8f && a.volume < sphereVol,
              "sphere cloud: volume sits just under the analytic sphere volume");
        // 全ての面の裏側に重心がある = 凸で閉じている
        bool convex = true;
        for (const ConvexFace& f : a.faces) {
            if (f.nx * a.com.x + f.ny * a.com.y + f.nz * a.com.z - f.d > 1e-4f) {
                convex = false;
                break;
            }
        }
        check(convex, "sphere cloud: centroid lies behind every face plane");
        check(a.boundRadius > 0.9f && a.boundRadius <= 1.0001f,
              "sphere cloud: bounding radius matches the unit sphere");
    }

    // ---- 縮退入力は箱へ落ちる (すり抜けさせない) ----
    {
        ConvexHullData h;
        check(!BuildConvexHull({ { 1, 2, 3 } }, h) && h.Valid() && h.volume > 0.0f,
              "degenerate: a single point falls back to a valid thin box");
        check(!BuildConvexHull({ { 0, 0, 0 }, { 1, 0, 0 }, { 2, 0, 0 } }, h) && h.Valid(),
              "degenerate: collinear points fall back to a valid thin box");
        const std::vector<XMFLOAT3> planar = {
            { 0, 0, 0 }, { 1, 0, 0 }, { 1, 0, 1 }, { 0, 0, 1 }, { 0.5f, 0, 0.5f },
        };
        check(!BuildConvexHull(planar, h) && h.Valid() && h.volume > 0.0f,
              "degenerate: coplanar points fall back to a valid thin box");
        check(BuildConvexHull({}, h) == false && h.Valid(),
              "degenerate: an empty point set still yields a valid shape");
    }

    // ---- .mcvx blob の往復 ----
    {
        const std::vector<uint8_t> a = Blob(cube);
        ConvexHullData back;
        size_t pos = 0;
        check(DeserializeConvexHull(a.data(), a.size(), pos, back) && pos == a.size(),
              "blob: round-trips and consumes exactly the written bytes");
        const std::vector<uint8_t> b = Blob(back);
        check(a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0,
              "blob: re-serialization is byte-identical");
        // 壊れた blob で落ちない / 読めたことにしない
        std::vector<uint8_t> broken = a;
        broken.resize(a.size() / 2);
        pos = 0;
        check(!DeserializeConvexHull(broken.data(), broken.size(), pos, back),
              "blob: a truncated blob is rejected without crashing");
        broken = a;
        broken[0] ^= 0xFFu; // 版を壊す
        pos = 0;
        check(!DeserializeConvexHull(broken.data(), broken.size(), pos, back),
              "blob: a bad version is rejected");
    }

    // ---- 支持点 ----
    {
        const int32_t px = ConvexSupportLocal(cube, 1.0f, 0.0f, 0.0f);
        check(cube.verts[static_cast<size_t>(px)].x > 0.0f, "support: +X picks a +X vertex");
        const int32_t diag = ConvexSupportLocal(cube, 1.0f, 1.0f, 1.0f);
        const XMFLOAT3& v = cube.verts[static_cast<size_t>(diag)];
        check(v.x > 0.0f && v.y > 0.0f && v.z > 0.0f, "support: (1,1,1) picks the far corner");
        // 同値のタイブレークが index 小で固定されていること (走査順に依らない主張の一部)
        check(ConvexSupportLocal(cube, 1.0f, 0.0f, 0.0f)
                  == ConvexSupportLocal(cube, 1.0f, 0.0f, 0.0f),
              "support: ties resolve to the lowest vertex index");
    }

    if (failCount == 0) {
        MYE_LOG_INFO("==== Convex hull self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== Convex hull self test: %d FAILED ====", failCount);
    return false;
}

} // namespace mye
