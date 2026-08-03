#include "Engine/Engine/RayTracing/RtSelfTest.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/Log.h"
#include "Engine/Core/Random.h"
#include "Engine/Engine/Physics/MeshColliderLibrary.h"
#include "Engine/Engine/RayTracing/RtSceneBuild.h"
#include "Engine/Renderer/RayTracing/RtMath.h"

using namespace DirectX;

namespace mye {
namespace {

int g_failCount = 0;

#define TEST_CHECK(cond)                                                    \
    do {                                                                    \
        if (cond) {                                                         \
            MYE_LOG_INFO("  PASS: %s", #cond);                              \
        } else {                                                            \
            MYE_LOG_ERROR("  FAIL: %s (%s:%d)", #cond, __FILE__, __LINE__); \
            ++g_failCount;                                                  \
        }                                                                   \
    } while (0)

// 散らばった小三角形のメッシュ (BVH が実際に枝分かれする形にする)
void MakeRandomMesh(Pcg32& rng, int triCount, std::vector<XMFLOAT3>& pos,
                    std::vector<uint32_t>& idx)
{
    pos.clear();
    idx.clear();
    for (int i = 0; i < triCount; ++i) {
        const XMFLOAT3 c = { rng.Range(-5.0f, 5.0f), rng.Range(-5.0f, 5.0f),
                             rng.Range(-5.0f, 5.0f) };
        for (int k = 0; k < 3; ++k) {
            pos.push_back({ c.x + rng.Range(-0.8f, 0.8f), c.y + rng.Range(-0.8f, 0.8f),
                            c.z + rng.Range(-0.8f, 0.8f) });
            idx.push_back(static_cast<uint32_t>(pos.size() - 1));
        }
    }
}

XMFLOAT3 Normalize(const XMFLOAT3& v)
{
    XMFLOAT3 r;
    XMStoreFloat3(&r, XMVector3Normalize(XMLoadFloat3(&v)));
    return r;
}

// 全三角形を総当りした最近ヒット (BVH の答え合わせ用)
bool BruteForceClosest(const std::vector<RtTri>& tris, const XMFLOAT3& ro, const XMFLOAT3& rd,
                       float tMax, float& outT, int32_t& outTri)
{
    outT = tMax;
    outTri = -1;
    for (size_t i = 0; i < tris.size(); ++i) {
        float t = 0.0f, u = 0.0f, v = 0.0f;
        if (RtRayTri(ro, rd, tris[i], t, u, v) && t < outT) {
            outT = t;
            outTri = static_cast<int32_t>(i);
        }
    }
    return outTri >= 0;
}

// BLAS: flatten したツリーのトラバーサルが総当りと同じ最近ヒットを返すこと。
// これが通れば HLSL 側 (同一ロジック) のノードエンコードとスタック走査も正しい
void TestBlasTraversal()
{
    MYE_LOG_INFO("[selftest] rt: BLAS traversal vs brute force");
    Pcg32 rng;
    rng.Seed(0x4D3436621ull);

    std::vector<XMFLOAT3> pos;
    std::vector<uint32_t> idx;
    MakeRandomMesh(rng, 200, pos, idx); // 200 三角形 → 葉 8 なので数十ノードに分岐する

    MeshColliderData md;
    BuildMeshColliderData(pos, idx, md);
    TEST_CHECK(!md.nodes.empty() && md.triOrder.size() == 200);

    // 頂点属性なしでも面法線で埋まること (旧メッシュ互換の経路)
    RtBlas blas;
    FlattenBlas(md, {}, {}, blas);
    TEST_CHECK(blas.nodes.size() == md.nodes.size());
    TEST_CHECK(blas.tris.size() == md.triOrder.size() && blas.attrs.size() == blas.tris.size());

    // 葉のエンコード (left = -(start+1)) が三角形配列の範囲に収まること
    bool leafRangesOk = true;
    int32_t leafTriTotal = 0;
    for (const RtBvhNode& n : blas.nodes) {
        if (n.left < 0) {
            const int32_t start = -n.left - 1;
            leafTriTotal += n.right;
            if (start < 0 || start + n.right > static_cast<int32_t>(blas.tris.size())) {
                leafRangesOk = false;
            }
        } else if (n.left >= static_cast<int32_t>(blas.nodes.size())
                   || n.right >= static_cast<int32_t>(blas.nodes.size())) {
            leafRangesOk = false; // 内部ノードの子 index が範囲外
        }
    }
    TEST_CHECK(leafRangesOk);
    TEST_CHECK(leafTriTotal == static_cast<int32_t>(blas.tris.size())); // 全三角形が葉に 1 回ずつ

    int hits = 0, mismatches = 0;
    for (int i = 0; i < 256; ++i) {
        const XMFLOAT3 ro = { rng.Range(-15.0f, 15.0f), rng.Range(-15.0f, 15.0f),
                              rng.Range(-15.0f, 15.0f) };
        const XMFLOAT3 aim = { rng.Range(-5.0f, 5.0f), rng.Range(-5.0f, 5.0f),
                               rng.Range(-5.0f, 5.0f) };
        const XMFLOAT3 rd = Normalize({ aim.x - ro.x, aim.y - ro.y, aim.z - ro.z });
        constexpr float kTMax = 1000.0f;

        float refT = 0.0f;
        int32_t refTri = -1;
        const bool refHit = BruteForceClosest(blas.tris, ro, rd, kTMax, refT, refTri);

        RtBlasHit hit;
        hit.t = kTMax; // 呼び出し側が探索上限として初期化する規約
        const bool bvhHit = RtTraceBlasCpu(blas.nodes, blas.tris, 0, ro, rd, hit);

        if (refHit != bvhHit || (refHit && (hit.tri != refTri || std::fabs(hit.t - refT) > 1e-4f))) {
            ++mismatches;
        }
        if (refHit) {
            ++hits;
        }
    }
    TEST_CHECK(mismatches == 0);
    TEST_CHECK(hits > 20); // レイの張り方が退化していない (ヒットが十分ある) ことの確認
}

// 同じ入力からは必ず同じバイト列が出ること (決定論 — GPU バッファの再現性の前提)
void TestBuildDeterminism()
{
    MYE_LOG_INFO("[selftest] rt: build determinism");
    Pcg32 rng;
    rng.Seed(0x9E3779B97F4A7C15ull);
    std::vector<XMFLOAT3> pos;
    std::vector<uint32_t> idx;
    MakeRandomMesh(rng, 120, pos, idx);
    std::vector<XMFLOAT3> normals(pos.size(), XMFLOAT3{ 0.0f, 1.0f, 0.0f });
    std::vector<XMFLOAT2> uvs(pos.size(), XMFLOAT2{ 0.25f, 0.75f });

    MeshColliderData md1, md2;
    BuildMeshColliderData(pos, idx, md1);
    BuildMeshColliderData(pos, idx, md2);
    RtBlas a, b;
    FlattenBlas(md1, normals, uvs, a);
    FlattenBlas(md2, normals, uvs, b);

    TEST_CHECK(a.nodes.size() == b.nodes.size() && a.tris.size() == b.tris.size());
    TEST_CHECK(std::memcmp(a.nodes.data(), b.nodes.data(), a.nodes.size() * sizeof(RtBvhNode))
               == 0);
    TEST_CHECK(std::memcmp(a.tris.data(), b.tris.data(), a.tris.size() * sizeof(RtTri)) == 0);
    TEST_CHECK(std::memcmp(a.attrs.data(), b.attrs.data(), a.attrs.size() * sizeof(RtTriAttr))
               == 0);
    // 属性が頂点データから来ていること (面法線フォールバックに落ちていない)
    TEST_CHECK(a.attrs[0].n0u0.y == 1.0f && a.attrs[0].n0u0.w == 0.25f);
    TEST_CHECK(a.attrs[0].n1v0.w == 0.75f);
}

// TLAS: 全インスタンスが葉に 1 回ずつ現れ、ルート AABB が全体を包むこと
void TestTlas()
{
    MYE_LOG_INFO("[selftest] rt: TLAS build");
    Pcg32 rng;
    rng.Seed(0xD1B54A32D192ED03ull);
    std::vector<RtAabb> bounds;
    for (int i = 0; i < 37; ++i) { // 葉サイズ 2 で割り切れない数にしておく
        const XMFLOAT3 c = { rng.Range(-20.0f, 20.0f), rng.Range(-20.0f, 20.0f),
                             rng.Range(-20.0f, 20.0f) };
        RtAabb ab;
        ab.min = { c.x - 0.5f, c.y - 0.5f, c.z - 0.5f };
        ab.max = { c.x + 0.5f, c.y + 0.5f, c.z + 0.5f };
        bounds.push_back(ab);
    }

    std::vector<RtBvhNode> nodes;
    std::vector<int32_t> order;
    BuildTlas(bounds, nodes, order);
    TEST_CHECK(!nodes.empty() && order.size() == bounds.size());

    // order が 0..N-1 の並べ替えになっていること
    std::vector<int32_t> sorted = order;
    std::sort(sorted.begin(), sorted.end());
    bool permutationOk = true;
    for (size_t i = 0; i < sorted.size(); ++i) {
        if (sorted[i] != static_cast<int32_t>(i)) {
            permutationOk = false;
        }
    }
    TEST_CHECK(permutationOk);

    // 葉が指す範囲の合計 = インスタンス数、かつ範囲が order 内に収まる
    int32_t leafTotal = 0;
    bool rangesOk = true;
    for (const RtBvhNode& n : nodes) {
        if (n.left < 0) {
            const int32_t start = -n.left - 1;
            leafTotal += n.right;
            if (start < 0 || start + n.right > static_cast<int32_t>(order.size())) {
                rangesOk = false;
            }
        }
    }
    TEST_CHECK(rangesOk && leafTotal == static_cast<int32_t>(bounds.size()));

    // ルートが全インスタンスを包含
    bool rootContains = true;
    for (const RtAabb& ab : bounds) {
        if (ab.min.x < nodes[0].aabbMin.x || ab.min.y < nodes[0].aabbMin.y
            || ab.min.z < nodes[0].aabbMin.z || ab.max.x > nodes[0].aabbMax.x
            || ab.max.y > nodes[0].aabbMax.y || ab.max.z > nodes[0].aabbMax.z) {
            rootContains = false;
        }
    }
    TEST_CHECK(rootContains);

    // 空入力は空を返す (呼び出し側が instanceCount==0 で無効化できる)
    std::vector<RtBvhNode> emptyNodes;
    std::vector<int32_t> emptyOrder;
    BuildTlas({}, emptyNodes, emptyOrder);
    TEST_CHECK(emptyNodes.empty() && emptyOrder.empty());
}

// 数式そのものの境界: スラブテストと Möller-Trumbore
void TestRayPrimitives()
{
    MYE_LOG_INFO("[selftest] rt: ray primitives");
    const XMFLOAT3 bmin = { -1, -1, -1 }, bmax = { 1, 1, 1 };
    const XMFLOAT3 ro = { 0, 0, -5 };
    const XMFLOAT3 rd = { 0, 0, 1 };
    const XMFLOAT3 invD = { RtSafeInv(rd.x), RtSafeInv(rd.y), RtSafeInv(rd.z) };
    TEST_CHECK(RtSlabTest(bmin, bmax, ro, invD, 1000.0f));
    TEST_CHECK(!RtSlabTest(bmin, bmax, ro, invD, 3.0f)); // tMax が箱に届かない
    // 軸に平行 (成分 0) でも NaN を出さずに判定できること
    const XMFLOAT3 side = { 5, 0, 0 };
    const XMFLOAT3 sideInv = { RtSafeInv(0.0f), RtSafeInv(0.0f), RtSafeInv(1.0f) };
    TEST_CHECK(!RtSlabTest(bmin, bmax, side, sideInv, 1000.0f));

    RtTri tri;
    tri.p0 = { -1, -1, 0 };
    tri.e1 = { 2, 0, 0 };
    tri.e2 = { 0, 2, 0 };
    float t = 0.0f, u = 0.0f, v = 0.0f;
    TEST_CHECK(RtRayTri({ -0.5f, -0.5f, -2.0f }, { 0, 0, 1 }, tri, t, u, v));
    TEST_CHECK(std::fabs(t - 2.0f) < 1e-5f);
    TEST_CHECK(std::fabs(u - 0.25f) < 1e-5f && std::fabs(v - 0.25f) < 1e-5f);
    // 三角形の外 / 背後 / 平行
    TEST_CHECK(!RtRayTri({ 0.9f, 0.9f, -2.0f }, { 0, 0, 1 }, tri, t, u, v));
    TEST_CHECK(!RtRayTri({ -0.5f, -0.5f, 2.0f }, { 0, 0, 1 }, tri, t, u, v));
    TEST_CHECK(!RtRayTri({ -0.5f, -0.5f, -2.0f }, { 1, 0, 0 }, tri, t, u, v));
}

// 乱数とコサイン重点サンプリング (M46c)。HLSL 側と同一式なので、
// ここが通れば GI シェーダのサンプリングも同じ分布になる
void TestSampling()
{
    MYE_LOG_INFO("[selftest] rt: sampling");
    // ハッシュは状態レス = 同じ入力から常に同じ値 (スクリーンショットの決定性の前提)
    TEST_CHECK(RtPcg3d(RtSeed{ 1, 2, 3 }).x == RtPcg3d(RtSeed{ 1, 2, 3 }).x);
    TEST_CHECK(RtPcg3d(RtSeed{ 1, 2, 3 }).x != RtPcg3d(RtSeed{ 1, 2, 4 }).x);

    // [0,1) に収まり、平均が 0.5 付近に来ること
    RtSeed s{ 7u, 11u, 0u };
    double sumX = 0.0, sumY = 0.0;
    bool inRange = true;
    constexpr int kN = 4096;
    for (int i = 0; i < kN; ++i) {
        const XMFLOAT2 u = RtNextRand2(s);
        if (!(u.x >= 0.0f && u.x < 1.0f && u.y >= 0.0f && u.y < 1.0f)) {
            inRange = false;
        }
        sumX += u.x;
        sumY += u.y;
    }
    TEST_CHECK(inRange);
    TEST_CHECK(std::fabs(sumX / kN - 0.5) < 0.02 && std::fabs(sumY / kN - 0.5) < 0.02);

    // コサイン重点: 常に法線半球の内側・単位長・有限値
    const XMFLOAT3 n = Normalize({ 0.3f, 0.8f, -0.5f });
    RtSeed s2{ 3u, 5u, 0u };
    bool hemiOk = true, unitOk = true, finiteOk = true;
    double cosSum = 0.0;
    constexpr int kS = 2048;
    for (int i = 0; i < kS; ++i) {
        const XMFLOAT3 d = RtCosineHemisphere(n, RtNextRand2(s2));
        const float dt = n.x * d.x + n.y * d.y + n.z * d.z;
        const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        if (dt < -1e-4f) {
            hemiOk = false;
        }
        if (std::fabs(len - 1.0f) > 1e-3f) {
            unitOk = false;
        }
        if (!std::isfinite(d.x) || !std::isfinite(d.y) || !std::isfinite(d.z)) {
            finiteOk = false;
        }
        cosSum += dt;
    }
    TEST_CHECK(hemiOk && unitOk && finiteOk);
    // コサイン分布の平均 cos は 2/3 (一様半球なら 1/2) — 重点サンプリングが効いている証拠
    TEST_CHECK(std::fabs(cosSum / kS - 2.0 / 3.0) < 0.03);

    // u=(0,*) は法線そのもの (円盤半径 0 = 天頂)
    const XMFLOAT3 top = RtCosineHemisphere(n, { 0.0f, 0.37f });
    TEST_CHECK(std::fabs(top.x - n.x) < 1e-4f && std::fabs(top.y - n.y) < 1e-4f
               && std::fabs(top.z - n.z) < 1e-4f);

    // z=-1 の法線でも基底が破綻しないこと (Duff の分岐なし ONB を使う理由)
    const XMFLOAT3 down = { 0.0f, 0.0f, -1.0f };
    const XMFLOAT3 dd = RtCosineHemisphere(down, { 0.5f, 0.25f });
    TEST_CHECK(std::isfinite(dd.x) && std::isfinite(dd.y) && std::isfinite(dd.z));
    TEST_CHECK(down.x * dd.x + down.y * dd.y + down.z * dd.z >= -1e-4f);
}

} // namespace

bool RunRtSelfTest()
{
    g_failCount = 0;
    MYE_LOG_INFO("==== Ray tracing self test ====");
    TestRayPrimitives();
    TestBlasTraversal();
    TestBuildDeterminism();
    TestTlas();
    TestSampling();
    if (g_failCount == 0) {
        MYE_LOG_INFO("==== Ray tracing self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== Ray tracing self test: %d FAILED ====", g_failCount);
    return false;
}

} // namespace mye
