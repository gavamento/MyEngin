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

// 太陽コーンのサンプリングと影レイのオフセット (M46g)。HLSL 側と同一式なので、
// ここが通れば rt_shadow.cs.hlsl の影レイも同じ方向分布・同じ eps になる
void TestShadowSampling()
{
    MYE_LOG_INFO("[selftest] rt: sun cone sampling / shadow ray epsilon");

    // ---- 半頂角 → cos ----
    TEST_CHECK(RtConeCosMax(0.0f) == 1.0f);                            // 点光源 = 硬い影
    TEST_CHECK(std::fabs(RtConeCosMax(90.0f)) < 1e-6f);                // 半球
    TEST_CHECK(RtConeCosMax(-5.0f) == 1.0f);                           // 負はクランプ
    TEST_CHECK(RtConeCosMax(1.0f) < RtConeCosMax(0.5f));               // 広いほど cos は小さい
    // 既定 (太陽の視半径 0.265°) はほぼ 1 = ほぼ硬い影
    TEST_CHECK(RtConeCosMax(kRtShadowSunAngleDeg) > 0.999f);

    // ---- 円錐サンプル ----
    const XMFLOAT3 dir = Normalize({ -0.4f, 0.8f, 0.45f });
    // cosMax = 1 (点光源) はどの乱数でも dir そのもの = 完全に硬い影に退化する
    const XMFLOAT3 exact = RtSampleCone(dir, 1.0f, { 0.37f, 0.81f });
    TEST_CHECK(std::fabs(exact.x - dir.x) < 1e-5f && std::fabs(exact.y - dir.y) < 1e-5f
               && std::fabs(exact.z - dir.z) < 1e-5f);
    // u.x = 1 は円錐の中心 (cos = 1)
    const XMFLOAT3 center = RtSampleCone(dir, 0.5f, { 1.0f, 0.25f });
    TEST_CHECK(std::fabs(center.x - dir.x) < 1e-5f && std::fabs(center.y - dir.y) < 1e-5f
               && std::fabs(center.z - dir.z) < 1e-5f);

    // 半頂角 10° の円錐: 全サンプルが単位長・円錐内・有限
    const float cosMax = RtConeCosMax(10.0f);
    RtSeed s{ 13u, 17u, 0u };
    bool inCone = true, unitOk = true, finiteOk = true;
    double cosSum = 0.0;
    constexpr int kS = 4096;
    for (int i = 0; i < kS; ++i) {
        const XMFLOAT3 d = RtSampleCone(dir, cosMax, RtNextRand2(s));
        const float dt = dir.x * d.x + dir.y * d.y + dir.z * d.z;
        const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        if (dt < cosMax - 1e-4f) {
            inCone = false;
        }
        if (std::fabs(len - 1.0f) > 1e-3f) {
            unitOk = false;
        }
        if (!std::isfinite(d.x) || !std::isfinite(d.y) || !std::isfinite(d.z)) {
            finiteOk = false;
        }
        cosSum += dt;
    }
    TEST_CHECK(inCone && unitOk && finiteOk);
    // 立体角に対して一様なら平均 cos は (1 + cosMax) / 2
    TEST_CHECK(std::fabs(cosSum / kS - 0.5 * (1.0 + cosMax)) < 1e-3);

    // z = -1 の方向でも ONB が破綻しない (RtCosineHemisphere と同じ Duff の基底)
    const XMFLOAT3 down = { 0.0f, 0.0f, -1.0f };
    const XMFLOAT3 dd = RtSampleCone(down, cosMax, { 0.5f, 0.25f });
    TEST_CHECK(std::isfinite(dd.x) && std::isfinite(dd.y) && std::isfinite(dd.z));
    TEST_CHECK(down.x * dd.x + down.y * dd.y + down.z * dd.z >= cosMax - 1e-4f);

    // ---- 影レイのオフセット (近景は絶対下限 / 遠景は距離比例) ----
    TEST_CHECK(RtSurfaceRayEps(0.0f) == kRtSurfaceEpsMin);
    TEST_CHECK(RtSurfaceRayEps(0.1f) == kRtSurfaceEpsMin); // 下限を割らない
    TEST_CHECK(RtSurfaceRayEps(100.0f) > RtSurfaceRayEps(10.0f));
    TEST_CHECK(std::fabs(RtSurfaceRayEps(100.0f) - 0.1f) < 1e-6f);
    // 半精度ワールド座標の相対誤差 (~5e-4) より必ず大きい = アクネが出ない下限
    TEST_CHECK(RtSurfaceRayEps(50.0f) > 50.0f * 4.9e-4f);
}

// GGX VNDF サンプリングと IBL フォールバック重み (M46h)。HLSL 側と同一式なので、
// ここが通れば rt_refl.cs.hlsl の反射方向と合成側の混色も同じになる
void TestReflection()
{
    MYE_LOG_INFO("[selftest] rt: GGX VNDF sampling / reflection blend weight");

    const XMFLOAT3 n = Normalize({ 0.2f, 0.9f, -0.35f });
    const XMFLOAT3 v = Normalize({ -0.3f, 0.6f, 0.75f }); // 面 → カメラ (法線側)

    // ---- alpha = 0 は完全鏡面: どの乱数でも half vector = 法線 ----
    const XMFLOAT3 h0 = RtGgxVndf(n, v, 0.0f, { 0.37f, 0.81f });
    TEST_CHECK(std::fabs(h0.x - n.x) < 1e-4f && std::fabs(h0.y - n.y) < 1e-4f
               && std::fabs(h0.z - n.z) < 1e-4f);
    const XMFLOAT3 h0b = RtGgxVndf(n, v, 0.0f, { 0.02f, 0.44f });
    TEST_CHECK(std::fabs(h0b.x - n.x) < 1e-4f && std::fabs(h0b.z - n.z) < 1e-4f);

    // ---- サンプルの健全性: 単位長・法線半球の内側・有限 ----
    // roughness 0.6 (= 反射を撃つ上限) の alpha で最も分布が広がる
    const float alphaMax = kRtReflMaxRoughness * kRtReflMaxRoughness;
    RtSeed s{ 23u, 91u, 0u };
    bool unitOk = true, hemiOk = true, finiteOk = true, lobeOk = true;
    double cosSum = 0.0;
    constexpr int kS = 4096;
    for (int i = 0; i < kS; ++i) {
        const XMFLOAT3 h = RtGgxVndf(n, v, alphaMax, RtNextRand2(s));
        const float len = std::sqrt(h.x * h.x + h.y * h.y + h.z * h.z);
        const float ndh = n.x * h.x + n.y * h.y + n.z * h.z;
        if (std::fabs(len - 1.0f) > 1e-3f) {
            unitOk = false;
        }
        if (ndh < -1e-4f) {
            hemiOk = false; // 可視法線は必ず法線半球の内側
        }
        if (!std::isfinite(h.x) || !std::isfinite(h.y) || !std::isfinite(h.z)) {
            finiteOk = false;
        }
        // 反射方向 = 視線を h で折り返したもの。視線側の半球に留まること
        const float vdh = v.x * h.x + v.y * h.y + v.z * h.z;
        if (vdh < -1e-3f) {
            lobeOk = false; // 視線の裏を向いた微小面は VNDF からは出ない
        }
        cosSum += ndh;
    }
    TEST_CHECK(unitOk && hemiOk && finiteOk && lobeOk);

    // ---- alpha が大きいほどローブが広がる (平均 N·H が小さくなる) ----
    double narrowSum = 0.0;
    RtSeed s2{ 23u, 91u, 0u };
    for (int i = 0; i < kS; ++i) {
        const XMFLOAT3 h = RtGgxVndf(n, v, 0.01f, RtNextRand2(s2));
        narrowSum += n.x * h.x + n.y * h.y + n.z * h.z;
    }
    TEST_CHECK(narrowSum / kS > cosSum / kS);       // 滑らかな面ほど法線に集中
    TEST_CHECK(narrowSum / kS > 0.999);             // alpha=0.01 はほぼ鏡面
    TEST_CHECK(cosSum / kS < 0.999 && cosSum / kS > 0.5); // alpha=0.36 は明確に広がる

    // ---- z = -1 の法線でも ONB が破綻しない (Duff の基底を使う理由) ----
    const XMFLOAT3 down = { 0.0f, 0.0f, -1.0f };
    const XMFLOAT3 hd = RtGgxVndf(down, down, alphaMax, { 0.5f, 0.25f });
    TEST_CHECK(std::isfinite(hd.x) && std::isfinite(hd.y) && std::isfinite(hd.z));
    TEST_CHECK(down.x * hd.x + down.y * hd.y + down.z * hd.z >= -1e-4f);

    // ---- IBL フォールバックの重み (合成の連続性を担保する式) ----
    TEST_CHECK(RtReflWeight(0.0f) == 1.0f);                    // 鏡面は反射 100%
    TEST_CHECK(RtReflWeight(kRtReflFadeStart) == 1.0f);        // フェード開始点まで 100%
    TEST_CHECK(RtReflWeight(kRtReflMaxRoughness) == 0.0f);     // カットオフで IBL 100%
    TEST_CHECK(RtReflWeight(1.0f) == 0.0f);
    // 単調減少 + 端点が滑らか (smoothstep なので微分が 0)
    bool monotonic = true;
    float prev = RtReflWeight(0.0f);
    for (int i = 1; i <= 200; ++i) {
        const float w = RtReflWeight(static_cast<float>(i) / 200.0f);
        if (w > prev + 1e-6f) {
            monotonic = false;
        }
        prev = w;
    }
    TEST_CHECK(monotonic);
    // 中点はちょうど 0.5 (smoothstep の対称性)
    const float mid = 0.5f * (kRtReflFadeStart + kRtReflMaxRoughness);
    TEST_CHECK(std::fabs(RtReflWeight(mid) - 0.5f) < 1e-5f);
    // 反射を撃たない領域では重みが 0 = 合成が反射バッファ (0 埋め) を見ない
    TEST_CHECK(RtReflWeight(kRtReflMaxRoughness + 0.01f) == 0.0f);
}

// テンポラル蓄積の判定式 (M46d)。HLSL 側と同一式なので、ここが通れば
// rt_temporal.cs.hlsl の再投影・履歴更新も同じ挙動になる
void TestTemporal()
{
    MYE_LOG_INFO("[selftest] rt: temporal reprojection");

    // ---- クリップ座標 → 履歴 UV ----
    XMFLOAT2 uv;
    // 画面中央 (ndc 0,0) は uv (0.5, 0.5)
    TEST_CHECK(RtClipToPrevUv({ 0.0f, 0.0f, 5.0f, 10.0f }, uv));
    TEST_CHECK(std::fabs(uv.x - 0.5f) < 1e-6f && std::fabs(uv.y - 0.5f) < 1e-6f);
    // ndc の +Y は画面の上 = uv の 0 側 (Y 反転が入っていること)
    TEST_CHECK(RtClipToPrevUv({ 0.0f, 1.0f, 5.0f, 2.0f }, uv));
    TEST_CHECK(uv.y < 0.5f);
    // カメラの背後 (w<=0) と画面外は履歴なし
    TEST_CHECK(!RtClipToPrevUv({ 0.0f, 0.0f, -1.0f, -2.0f }, uv));
    TEST_CHECK(!RtClipToPrevUv({ 3.0f, 0.0f, 1.0f, 2.0f }, uv)); // ndc.x = 1.5 → 画面右外
    TEST_CHECK(!RtClipToPrevUv({ 0.0f, -3.0f, 1.0f, 2.0f }, uv)); // ndc.y = -1.5 → 画面下外

    // ---- 再投影の妥当性 (深度 = カメラ距離の相対差 / 法線 = cos) ----
    const XMFLOAT3 n = { 0.0f, 1.0f, 0.0f };
    constexpr float kD = kRtTemporalDepthThreshold;   // 0.05
    constexpr float kN = kRtTemporalNormalThreshold;  // 0.9
    TEST_CHECK(RtReprojectValid(10.0f, 10.0f, n, n, kD, kN));       // 完全一致
    TEST_CHECK(RtReprojectValid(10.0f, 10.4f, n, n, kD, kN));       // 相対 4% = 許容内
    TEST_CHECK(!RtReprojectValid(10.0f, 11.0f, n, n, kD, kN));      // 相対 10% = disocclusion
    TEST_CHECK(!RtReprojectValid(10.0f, 0.0f, n, n, kD, kN));       // 履歴が未記録 (深度 0)
    TEST_CHECK(!RtReprojectValid(0.0f, 10.0f, n, n, kD, kN));       // 現在側が退化
    // 法線: しきい値をまたぐ 2 点 + 直交/裏返しは棄却
    const XMFLOAT3 tiltIn = Normalize({ 0.3122f, 0.95f, 0.0f });  // n との cos ≈ 0.95 > 0.9
    const XMFLOAT3 tiltOut = Normalize({ 0.5268f, 0.85f, 0.0f }); // n との cos ≈ 0.85 < 0.9
    TEST_CHECK(RtReprojectValid(10.0f, 10.0f, n, tiltIn, kD, kN));
    TEST_CHECK(!RtReprojectValid(10.0f, 10.0f, n, tiltOut, kD, kN));
    TEST_CHECK(!RtReprojectValid(10.0f, 10.0f, n, { 1.0f, 0.0f, 0.0f }, kD, kN));
    TEST_CHECK(!RtReprojectValid(10.0f, 10.0f, n, { 0.0f, -1.0f, 0.0f }, kD, kN));

    // ---- 履歴長と重み ----
    const float maxLen = static_cast<float>(kRtTemporalMaxHistory);
    // 無効 → 1 に若返る = alpha 1.0 = 今フレームの 1spp をそのまま採用
    TEST_CHECK(RtAdvanceHistory(20.0f, false, maxLen) == 1.0f);
    TEST_CHECK(RtTemporalAlpha(RtAdvanceHistory(20.0f, false, maxLen)) == 1.0f);
    // 有効 → 1 ずつ伸びて上限で頭打ち (追従を止めないための下限重み)
    TEST_CHECK(RtAdvanceHistory(0.0f, true, maxLen) == 1.0f);
    TEST_CHECK(RtAdvanceHistory(3.0f, true, maxLen) == 4.0f);
    TEST_CHECK(RtAdvanceHistory(maxLen, true, maxLen) == maxLen);
    TEST_CHECK(std::fabs(RtTemporalAlpha(maxLen) - 1.0f / maxLen) < 1e-6f);
    TEST_CHECK(RtTemporalAlpha(0.0f) == 1.0f); // 0 除算しない

    // 静止 (常に valid) なら履歴長は単調増加して上限で止まり、重みは 0 にならない
    float len = 0.0f;
    bool monotone = true;
    for (int i = 0; i < 200; ++i) {
        const float next = RtAdvanceHistory(len, true, maxLen);
        if (next < len) {
            monotone = false;
        }
        len = next;
    }
    TEST_CHECK(monotone && len == maxLen && RtTemporalAlpha(len) > 0.0f);

    // 移動平均の収束: 一定値 c を入れ続ければ蓄積値は c に近づく (バイアスが無い)
    float acc = 0.0f, hist = 0.0f;
    constexpr float kC = 0.75f;
    for (int i = 0; i < 64; ++i) {
        hist = RtAdvanceHistory(hist, i > 0, maxLen);
        const float a = RtTemporalAlpha(hist);
        acc = acc * (1.0f - a) + kC * a;
    }
    TEST_CHECK(std::fabs(acc - kC) < 1e-3f);
}

// SVGF の分散推定とエッジ停止重み (M46e)。HLSL 側 (rt_variance.cs.hlsl /
// rt_atrous.cs.hlsl) と同一式なので、ここが通れば GPU 側の重みも同じ挙動になる
void TestSvgf()
{
    MYE_LOG_INFO("[selftest] rt: svgf variance / edge-stopping weights");

    // ---- 輝度 (Rec.709) ----
    TEST_CHECK(std::fabs(RtLuminance({ 1.0f, 1.0f, 1.0f }) - 1.0f) < 1e-6f); // 係数の和 = 1
    TEST_CHECK(RtLuminance({ 0.0f, 1.0f, 0.0f }) > RtLuminance({ 1.0f, 0.0f, 0.0f }));
    TEST_CHECK(RtLuminance({ 0.0f, 0.0f, 0.0f }) == 0.0f);

    // ---- モーメント → 分散 ----
    // 一定値なら分散 0 (μ² - μ² が丸めで負に落ちても 0 で止まる)
    TEST_CHECK(RtVarianceFromMoments(0.5f, 0.25f) == 0.0f);
    TEST_CHECK(RtVarianceFromMoments(0.7f, 0.49f) < 1e-6f);  // 丸めで微小な正 (0.7² は非正確)
    TEST_CHECK(RtVarianceFromMoments(1.0f, 0.999f) == 0.0f); // 丸めで負 → クランプ
    // {0, 1} を半々で見たときの分散は 0.25
    TEST_CHECK(std::fabs(RtVarianceFromMoments(0.5f, 0.5f) - 0.25f) < 1e-6f);

    // ---- サンプル分散 → 推定値の分散 (履歴長で割る / 凍結時は割らない) ----
    TEST_CHECK(std::fabs(RtVarianceEstimate(0.4f, 4.0f, false) - 0.1f) < 1e-6f);
    TEST_CHECK(std::fabs(RtVarianceEstimate(0.4f, 1.0f, false) - 0.4f) < 1e-6f);
    TEST_CHECK(std::fabs(RtVarianceEstimate(0.4f, 0.0f, false) - 0.4f) < 1e-6f); // 0 除算しない
    // 凍結中は履歴がいくら伸びても実効サンプル数 1 (テンポラル分散が 0 に潰れるため)
    TEST_CHECK(RtVarianceEstimate(0.4f, 32.0f, true) == 0.4f);
    TEST_CHECK(RtVarianceEstimate(0.4f, 32.0f, false) < RtVarianceEstimate(0.4f, 4.0f, false));

    // ---- A-Trous カーネル (B3 スプライン) ----
    TEST_CHECK(RtAtrousKernel(0) > RtAtrousKernel(1) && RtAtrousKernel(1) > RtAtrousKernel(2));
    TEST_CHECK(RtAtrousKernel(-1) == RtAtrousKernel(1)); // 対称
    TEST_CHECK(RtAtrousKernel(3) == 0.0f);               // 半径外
    float kSum = 0.0f;
    for (int d = -kRtAtrousRadius; d <= kRtAtrousRadius; ++d) {
        kSum += RtAtrousKernel(d);
    }
    TEST_CHECK(std::fabs(kSum - 1.0f) < 1e-6f); // 1 次元で正規化済み (2 次元も外積で 1)

    // ---- 深度 (カメラ距離) の重み ----
    constexpr float kSd = kRtAtrousSigmaDepth;
    TEST_CHECK(RtAtrousDepthWeight(10.0f, 10.0f, 1.0f, kSd) == 1.0f); // 同じ深度は減衰なし
    TEST_CHECK(RtAtrousDepthWeight(10.0f, 10.5f, 1.0f, kSd)
               < RtAtrousDepthWeight(10.0f, 10.1f, 1.0f, kSd)); // 離れるほど小さい
    // 相対差で見るので、距離が 10 倍でも「同じ相対差」なら同じ重み
    TEST_CHECK(std::fabs(RtAtrousDepthWeight(10.0f, 10.1f, 1.0f, kSd)
                         - RtAtrousDepthWeight(100.0f, 101.0f, 1.0f, kSd))
               < 1e-5f);
    // 刻み幅を上げた (タップが遠い) ぶんだけ許容が広がる = 平面がぼけ続ける
    TEST_CHECK(RtAtrousDepthWeight(10.0f, 10.4f, 4.0f, kSd)
               > RtAtrousDepthWeight(10.0f, 10.4f, 1.0f, kSd));
    // tapDist は 1 未満に潰さない (中心タップで許容が 0 にならないように)
    TEST_CHECK(RtAtrousDepthWeight(10.0f, 10.0f, 0.0f, kSd) == 1.0f);

    // ---- 法線の重み ----
    constexpr float kSn = kRtAtrousSigmaNormal;
    const XMFLOAT3 up = { 0.0f, 1.0f, 0.0f };
    TEST_CHECK(std::fabs(RtAtrousNormalWeight(up, up, kSn) - 1.0f) < 1e-6f);
    TEST_CHECK(RtAtrousNormalWeight(up, { 1.0f, 0.0f, 0.0f }, kSn) == 0.0f); // 直交
    TEST_CHECK(RtAtrousNormalWeight(up, { 0.0f, -1.0f, 0.0f }, kSn) == 0.0f); // 裏向き
    // 少し傾いた面は残り、大きく傾いた面はほぼ切れる (指数 64 の効き)
    TEST_CHECK(RtAtrousNormalWeight(up, Normalize({ 0.1f, 1.0f, 0.0f }), kSn) > 0.5f);
    TEST_CHECK(RtAtrousNormalWeight(up, Normalize({ 0.5f, 1.0f, 0.0f }), kSn) < 0.05f);

    // ---- 輝度の重み (推定標準偏差でスケール) ----
    constexpr float kSl = kRtAtrousSigmaLuma;
    TEST_CHECK(RtAtrousLumaWeight(0.5f, 0.5f, 0.04f, kSl) == 1.0f); // 同じ輝度は減衰なし
    // 分散が大きい (ノイズ中) ほど輝度差を許す = よくぼける
    TEST_CHECK(RtAtrousLumaWeight(0.5f, 0.7f, 0.04f, kSl)
               > RtAtrousLumaWeight(0.5f, 0.7f, 0.0004f, kSl));
    // 収束して分散 0 になったらエッジは残す (重みがほぼ 0)
    TEST_CHECK(RtAtrousLumaWeight(0.5f, 0.7f, 0.0f, kSl) < 1e-6f);
    // 標準偏差 σ の σ_l 倍だけ離れた点は exp(-1) 付近 (スケールが合っている)
    const float sd = 0.1f;
    TEST_CHECK(std::fabs(RtAtrousLumaWeight(0.5f, 0.5f + kSl * sd, sd * sd, kSl)
                         - std::exp(-1.0f))
               < 1e-3f);
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
    TestShadowSampling();
    TestReflection();
    TestTemporal();
    TestSvgf();
    if (g_failCount == 0) {
        MYE_LOG_INFO("==== Ray tracing self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== Ray tracing self test: %d FAILED ====", g_failCount);
    return false;
}

} // namespace mye
