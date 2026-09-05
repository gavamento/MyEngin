#include "Engine/Engine/RayTracing/RtSelfTest.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits> // M67f: 半径スケールの NaN ケース
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

    // ---- M55f: 履歴 UV の 2 経路 (画面速度 / 前フレーム VP への射影) ----
    {
        const XMFLOAT2 px = { 0.25f, 0.75f };
        // 画面中央を指すクリップ座標。velocity 経路のときは**使われない**ことをこれで見る
        const XMFLOAT4 centerClip = { 0.0f, 0.0f, 5.0f, 10.0f };
        XMFLOAT2 out;
        // ① 速度 0 = 静止物 → 同じ画素をそのまま引く (ビット単位で一致すること。
        //    ここがずれると静止した絵が毎フレーム 1 テクセル揺れる)
        TEST_CHECK(RtHistoryUv(true, px, { 0.0f, 0.0f }, centerClip, out));
        TEST_CHECK(out.x == px.x && out.y == px.y);
        // ② 速度あり → prevUv = uv - velocity (TAA / モーションブラーと同じ規約)
        TEST_CHECK(RtHistoryUv(true, px, { 0.1f, -0.05f }, centerClip, out));
        TEST_CHECK(std::fabs(out.x - 0.15f) < 1e-6f && std::fabs(out.y - 0.80f) < 1e-6f);
        // ③ 前フレームの画面外 = 履歴なし ([0,1) 規約なので 1.0 ちょうども棄却)
        TEST_CHECK(!RtHistoryUv(true, px, { 0.30f, 0.0f }, centerClip, out));
        TEST_CHECK(!RtHistoryUv(true, px, { -0.75f, 0.0f }, centerClip, out)); // x = 1.0 ちょうど
        // ④ useVelocity=false は M46d の経路へ縮退 — velocity をいくら渡しても無視される
        TEST_CHECK(RtHistoryUv(false, px, { 0.4f, 0.4f }, centerClip, out));
        TEST_CHECK(std::fabs(out.x - 0.5f) < 1e-6f && std::fabs(out.y - 0.5f) < 1e-6f);
        TEST_CHECK(!RtHistoryUv(false, px, { 0.0f, 0.0f }, { 3.0f, 0.0f, 1.0f, 2.0f }, out));
    }

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

// ---- M67c: ReSTIR の数学 ----

// Duff らの分岐なし ONB (RtGgxVndf / RtCosineHemisphere が使っているものと同じ基底)。
// 半球の一様グリッドを張るのに使う
void BuildOnb(const XMFLOAT3& n, XMFLOAT3& t1, XMFLOAT3& t2)
{
    const float sgn = (n.z >= 0.0f) ? 1.0f : -1.0f;
    const float a = -1.0f / (sgn + n.z);
    const float b = n.x * n.y * a;
    t1 = { 1.0f + sgn * n.x * n.x * a, sgn * b, -sgn * n.x };
    t2 = { b, sgn + n.y * n.y * a, -n.y };
}

float Dot3(const XMFLOAT3& a, const XMFLOAT3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

// HLSL の reflect(-v, h) と同じ (視線 v を微小面法線 h で折り返した方向)
XMFLOAT3 ReflectAbout(const XMFLOAT3& v, const XMFLOAT3& h)
{
    const float d = 2.0f * Dot3(v, h);
    return { d * h.x - v.x, d * h.y - v.y, d * h.z - v.z };
}

// ReSTIR (M67) の数式。HLSL 側 rt_restir_common.hlsli と同一式なので、ここが通れば
// GPU 側の reservoir 統合・解決も同じ挙動になる
void TestRestir()
{
    MYE_LOG_INFO("[selftest] rt: restir (VNDF pdf / reservoir / jacobian)");

    const XMFLOAT3 N = Normalize({ 0.2f, 0.9f, -0.35f });
    XMFLOAT3 t1, t2;
    BuildOnb(N, t1, t2);
    // 法線から 35° 傾いた視線。**真上 (0°) を避ける**のは、ローブの頂点が cosθ=1 の
    // グリッド端に載ると中点則が α の小さい側で収束しないため (実測: α=0.04 / 0° は
    // 256×256 でも 0.909、35° なら 128×128 で 0.99821)
    constexpr float kViewCos = 0.8191520f; // cos 35°
    constexpr float kViewSin = 0.5735764f;
    const XMFLOAT3 V = Normalize({ N.x * kViewCos + t1.x * kViewSin,
                                   N.y * kViewCos + t1.y * kViewSin,
                                   N.z * kViewCos + t1.z * kViewSin });
    const XMFLOAT3 mirror = ReflectAbout(V, N);

    // ---- VNDF pdf の正規化: 半球積分 + 下半球へ抜けた分 = 1 ----
    // ★半球積分そのものは 1 にならない (α=0.36 で約 0.885)。VNDF は半ベクトル側で
    //   正規化されており、反射後に地平線の下へ回ったサンプルは pdf の定義域から
    //   外れるため。**サンプラ (RtGgxVndf) と pdf が同じ分布を指している**ことは
    //   「積分 + 漏れ = 1」でしか確かめられない (どちらか片方だけでは検査にならない)
    constexpr int kGrid = 256;   // (cosθ, φ) の一様グリッド (中点則)
    constexpr int kMc = 32768;   // 下半球へ抜けた割合の推定サンプル数
    const float kAlphas[2] = { kRtReflMaxRoughness * kRtReflMaxRoughness, 0.04f };
    for (int ai = 0; ai < 2; ++ai) {
        const float alpha = kAlphas[ai];
        double sum = 0.0;
        for (int i = 0; i < kGrid; ++i) {
            const float ct = (static_cast<float>(i) + 0.5f) / static_cast<float>(kGrid);
            const float st = std::sqrt((std::max)(0.0f, 1.0f - ct * ct));
            for (int j = 0; j < kGrid; ++j) {
                const float phi = 6.28318530718f * (static_cast<float>(j) + 0.5f)
                                  / static_cast<float>(kGrid);
                const float c1 = st * std::cos(phi);
                const float c2 = st * std::sin(phi);
                const XMFLOAT3 L = { t1.x * c1 + t2.x * c2 + N.x * ct,
                                     t1.y * c1 + t2.y * c2 + N.y * ct,
                                     t1.z * c1 + t2.z * c2 + N.z * ct };
                sum += RtGgxVndfPdf(N, V, L, alpha);
            }
        }
        const double integral = sum / (kGrid * kGrid) * 6.283185307179586;
        // サンプラ側の漏れ (反射方向が面の下へ回った割合)
        RtSeed s{ 17u, 41u, static_cast<uint32_t>(ai) };
        int below = 0;
        for (int i = 0; i < kMc; ++i) {
            const XMFLOAT3 h = RtGgxVndf(N, V, alpha, RtNextRand2(s));
            if (Dot3(N, ReflectAbout(V, h)) <= 0.0f) {
                ++below;
            }
        }
        const double leak = static_cast<double>(below) / kMc;
        MYE_LOG_INFO("  restir: alpha=%.3f  integral=%.4f  leak=%.4f  sum=%.4f",
                     static_cast<double>(alpha), integral, leak, integral + leak);
        TEST_CHECK(std::fabs(integral + leak - 1.0) < 0.01);
        // 粗い面ほど地平線の下へ多く漏れる (= 半球積分は 1 から離れる)。
        // ここが 0 になったら「漏れを測れていない」= 上の等式が形だけになる
        TEST_CHECK((ai == 0) ? (leak > 0.05) : (leak < 0.01));
    }

    // ---- pdf の絶対値: ピーク (半ベクトル = 法線) の解析値と突き合わせる ----
    // D(N·H=1) = 1/(π α²) なので pdf(鏡面方向) = G1 / (4π α² (N·V))。
    // 定数 4 (半ベクトル → 反射方向のヤコビアン)・π・α² のスケールをここで固定する。
    // ★G1 は実装 (2c / (c + √(α²+(1−α²)c²))) と**別形の Smith Λ** で書く —
    //   同じ式を 2 回書いても転記ミスの検査にならない (両者は代数的に同一)
    const float ndotv = Dot3(N, V);
    for (int ai = 0; ai < 2; ++ai) {
        const float alpha = kAlphas[ai];
        const float a2 = alpha * alpha;
        const float tan2 = (1.0f - ndotv * ndotv) / (ndotv * ndotv);
        const float lambda = 0.5f * (-1.0f + std::sqrt(1.0f + a2 * tan2));
        const float expect = (1.0f / (1.0f + lambda))
                             / (4.0f * 3.14159265358979f * a2 * ndotv);
        const float got = RtGgxVndfPdf(N, V, mirror, alpha);
        MYE_LOG_INFO("  restir: pdf peak alpha=%.3f  got=%.3f  expect=%.3f",
                     static_cast<double>(alpha), static_cast<double>(got),
                     static_cast<double>(expect));
        TEST_CHECK(std::fabs(got / expect - 1.0f) < 1e-3f);
    }
    // α の下限クランプが効いている (α=0 と α=αmin が同じ値、かつ発散しない)。
    // ★αmin では解析値と突き合わせられない — α²=1e-6 に対して normalize(V+L) の
    //   丸め (1−(N·H)² ≈ 2e-7) が分母の 2 割を占めてしまい、式が正しくても 3 割ずれる
    const float pdfMirror = RtGgxVndfPdf(N, V, mirror, 0.0f);
    TEST_CHECK(RtGgxVndfPdf(N, V, mirror, kRtRestirAlphaMin) == pdfMirror);
    TEST_CHECK(std::isfinite(pdfMirror) && pdfMirror > 0.0f);
    // 面の裏は 0 (target function がここで消えるので「借りない」判断になる)
    TEST_CHECK(RtGgxVndfPdf(N, V, { -N.x, -N.y, -N.z }, 0.36f) == 0.0f);
    // 粗いほどピークが低い / 鏡面方向から離れるほど小さい
    TEST_CHECK(RtGgxVndfPdf(N, V, mirror, 0.36f) < RtGgxVndfPdf(N, V, mirror, 0.04f));
    const XMFLOAT3 offMirror = Normalize({ mirror.x + 0.25f * t2.x, mirror.y + 0.25f * t2.y,
                                           mirror.z + 0.25f * t2.z });
    TEST_CHECK(RtGgxVndfPdf(N, V, offMirror, 0.36f) < RtGgxVndfPdf(N, V, mirror, 0.36f));

    // ---- target function = 輝度 × pdf ----
    const XMFLOAT3 kLs = { 0.8f, 0.4f, 0.1f };
    const float pHatMirror = RtRestirTargetPdf(kLs, mirror, V, N, 0.36f);
    TEST_CHECK(std::fabs(pHatMirror - RtLuminance(kLs) * RtGgxVndfPdf(N, V, mirror, 0.36f))
               < 1e-6f);
    TEST_CHECK(RtRestirTargetPdf({ 0.0f, 0.0f, 0.0f }, mirror, V, N, 0.36f) == 0.0f);
    // 輝度に比例する (2 倍明るいサンプルは 2 倍重い)
    const XMFLOAT3 kLs2 = { 1.6f, 0.8f, 0.2f };
    TEST_CHECK(std::fabs(RtRestirTargetPdf(kLs2, mirror, V, N, 0.36f) / pHatMirror - 2.0f)
               < 1e-4f);

    // ---- streaming RIS: 採用確率が重みに比例する ----
    // 重み 1:2:7 の 3 候補を順に流し、最後に残ったサンプルの内訳を数える
    {
        constexpr int kTrials = 20000;
        const float kW[3] = { 1.0f, 2.0f, 7.0f };
        int picked[3] = { 0, 0, 0 };
        bool mOk = true;
        RtSeed s{ 7u, 13u, 0u };
        for (int i = 0; i < kTrials; ++i) {
            RtReservoirCpu r = RtReservoirEmpty();
            float wSum = 0.0f;
            for (int k = 0; k < 3; ++k) {
                RtReservoirUpdate(r, wSum, { static_cast<float>(k), 0.0f, 0.0f }, N, kLs, k,
                                  1.0f, kW[k], RtNextRand2(s).x);
            }
            if (r.M != 3.0f) {
                mOk = false;
            }
            if (r.cls >= 0 && r.cls < 3) {
                ++picked[r.cls];
            }
        }
        TEST_CHECK(mOk); // 重みに関わらず M は候補の数だけ進む
        const double f0 = static_cast<double>(picked[0]) / kTrials;
        const double f1 = static_cast<double>(picked[1]) / kTrials;
        const double f2 = static_cast<double>(picked[2]) / kTrials;
        MYE_LOG_INFO("  restir: RIS pick ratio %.4f / %.4f / %.4f (expect 0.1/0.2/0.7)", f0, f1,
                     f2);
        TEST_CHECK(std::fabs(f0 - 0.1) < 0.02);
        TEST_CHECK(std::fabs(f1 - 0.2) < 0.02);
        TEST_CHECK(std::fabs(f2 - 0.7) < 0.02);
        // 重み 0 の候補は絶対に採用されない (真っ黒を借りて絵を暗くしない)
        RtReservoirCpu r = RtReservoirEmpty();
        float wSum = 0.0f;
        TEST_CHECK(!RtReservoirUpdate(r, wSum, { 9.0f, 9.0f, 9.0f }, N, kLs, 2, 1.0f, 0.0f,
                                      0.0f));
        TEST_CHECK(r.M == 1.0f && wSum == 0.0f && r.cls == -1);
    }

    // ---- resolve: M=1 は現行 (1spp) とビット一致 ----
    {
        RtReservoirCpu r = RtReservoirEmpty();
        float wSum = 0.0f;
        // 初期サンプルの重みは p̂/p = lum(Ls) (ソース pdf = VNDF で D_vis が約分される)
        const XMFLOAT3 ls = { 0.3125f, 1.7f, 0.041f };
        RtReservoirUpdate(r, wSum, mirror, N, ls, kRtReflClassProp, 1.0f, RtLuminance(ls),
                          0.5f);
        const XMFLOAT3 out = RtRestirResolve(r, wSum);
        TEST_CHECK(r.M == 1.0f);
        TEST_CHECK(out.x == ls.x && out.y == ls.y && out.z == ls.z); // ★ビット一致
        // 真っ黒 / 空 reservoir は 0 (0 除算しない)
        RtReservoirCpu black = RtReservoirEmpty();
        float bSum = 0.0f;
        RtReservoirUpdate(black, bSum, mirror, N, { 0.0f, 0.0f, 0.0f }, 0, 1.0f, 0.0f, 0.5f);
        const XMFLOAT3 bo = RtRestirResolve(black, bSum);
        TEST_CHECK(bo.x == 0.0f && bo.y == 0.0f && bo.z == 0.0f);
        const XMFLOAT3 eo = RtRestirResolve(RtReservoirEmpty(), 0.0f);
        TEST_CHECK(eo.x == 0.0f && eo.y == 0.0f && eo.z == 0.0f);
        // 空 reservoir の既定値 (cls = -1 = 範囲外 = デバッグ表示で黒)
        const RtReservoirCpu empty = RtReservoirEmpty();
        TEST_CHECK(empty.M == 0.0f && empty.W == 0.0f && empty.cls == -1);
    }

    // ---- Jacobian ----
    {
        const XMFLOAT3 xs = { 1.0f, 2.0f, -3.0f };
        const XMFLOAT3 ns = Normalize({ 0.3f, 0.8f, 0.5f });
        const XMFLOAT3 pa = { 4.0f, 1.0f, 2.0f };
        const XMFLOAT3 pb = { -2.0f, 5.0f, 1.5f };
        const float jab = RtRestirJacobian(xs, ns, pa, pb);
        const float jba = RtRestirJacobian(xs, ns, pb, pa);
        TEST_CHECK(std::fabs(jab * jba - 1.0f) < 1e-5f); // 逆向きに掛けると 1
        TEST_CHECK(RtRestirJacobian(xs, ns, pa, pa) == 1.0f); // 同じ受け側 = 伸縮なし
        // ★静止シーンではこれが効いて J = 1 ちょうどになる = temporal が棄却されない
        TEST_CHECK(RtRestirJacobian(xs, { 0.0f, 0.0f, 0.0f }, pa, pb) == 1.0f); // スカイ
        // 受け側が 2 倍遠ざかる (向きは同じ = cosθ 不変) → 1/4
        const XMFLOAT3 far = { xs.x + 2.0f * (pa.x - xs.x), xs.y + 2.0f * (pa.y - xs.y),
                               xs.z + 2.0f * (pa.z - xs.z) };
        TEST_CHECK(std::fabs(RtRestirJacobian(xs, ns, pa, far) - 0.25f) < 1e-6f);
        // 距離は同じで cosθ が半分 → J も半分 (cos の比が効いていることの確認)
        const XMFLOAT3 org = { 0.0f, 0.0f, 0.0f };
        const XMFLOAT3 up = { 0.0f, 1.0f, 0.0f };
        const XMFLOAT3 pUp = { 0.0f, 3.0f, 0.0f };                    // cos = 1
        const XMFLOAT3 pTilt = { 3.0f * 0.8660254f, 1.5f, 0.0f };     // 60° = cos 0.5
        TEST_CHECK(std::fabs(RtRestirJacobian(org, up, pUp, pTilt) - 0.5f) < 1e-5f);
    }

    // ---- 統合 (merge): 重み = p̂ · W' · min(M', mCap) · J ----
    {
        RtReservoirCpu cand = RtReservoirEmpty();
        cand.xs = mirror;
        cand.ns = N;
        cand.Ls = kLs;
        cand.cls = kRtReflClassHero;
        cand.W = 3.0f;
        cand.M = 1.0f;
        const float pHat = 2.0f;
        // J = 1 / J = 2 で重みがちょうど 2 倍になる
        RtReservoirCpu r1 = RtReservoirEmpty();
        float w1 = 0.0f;
        RtReservoirMerge(r1, w1, cand, pHat, 8.0f, 1.0f, kRtRestirJacobianMax, 0.5f);
        RtReservoirCpu r2 = RtReservoirEmpty();
        float w2 = 0.0f;
        RtReservoirMerge(r2, w2, cand, pHat, 8.0f, 2.0f, kRtRestirJacobianMax, 0.5f);
        TEST_CHECK(std::fabs(w1 - pHat * cand.W * 1.0f) < 1e-6f);
        TEST_CHECK(std::fabs(w2 - 2.0f * w1) < 1e-6f);
        TEST_CHECK(r1.M == 1.0f && r1.cls == kRtReflClassHero);

        // J が範囲外の候補は **重みも M も動かさない** (幾何が違いすぎる = 別物)
        RtReservoirCpu r3 = RtReservoirEmpty();
        float w3 = 0.0f;
        TEST_CHECK(!RtReservoirMerge(r3, w3, cand, pHat, 8.0f, kRtRestirJacobianMax + 1.0f,
                                     kRtRestirJacobianMax, 0.5f));
        TEST_CHECK(!RtReservoirMerge(r3, w3, cand, pHat, 8.0f,
                                     0.5f / kRtRestirJacobianMax, kRtRestirJacobianMax, 0.5f));
        TEST_CHECK(w3 == 0.0f && r3.M == 0.0f);
        // 空 reservoir は候補にならない
        TEST_CHECK(!RtReservoirMerge(r3, w3, RtReservoirEmpty(), pHat, 8.0f, 1.0f,
                                     kRtRestirJacobianMax, 0.5f));
        TEST_CHECK(r3.M == 0.0f);

        // ★p̂_q(y') = 0 の候補も「候補から外す」= **M も wSum も動かさない**。
        //   ここで M を足すと W = wSum/(M·p̂) が縮んで鏡面が暗化する (spec §4.2 の
        //   「M を数える規則」。粗さ 0.10 の面では半径 8px の候補の大半が p̂ ≈ 0)
        TEST_CHECK(!RtReservoirMerge(r3, w3, cand, 0.0f, 8.0f, 1.0f, kRtRestirJacobianMax,
                                     0.5f));
        TEST_CHECK(w3 == 0.0f && r3.M == 0.0f);
        // W' = 0 (一度も有効な重みを積めなかった reservoir) も同じ
        RtReservoirCpu dark = cand;
        dark.W = 0.0f;
        TEST_CHECK(!RtReservoirMerge(r3, w3, dark, pHat, 8.0f, 1.0f, kRtRestirJacobianMax,
                                     0.5f));
        TEST_CHECK(w3 == 0.0f && r3.M == 0.0f);
        // 非有限の重み (0 除算などで inf が入った候補) も外す — 足すと wSum が二度と戻らない
        TEST_CHECK(!RtReservoirMerge(r3, w3, cand, INFINITY, 8.0f, 1.0f, kRtRestirJacobianMax,
                                     0.5f));
        TEST_CHECK(w3 == 0.0f && r3.M == 0.0f);
        // ★対比: **自画素の**黒サンプルは Update を直接呼ぶので M = 1 になる
        //   (上の「重み 0 の候補は採用されない」の検査と同じ経路。Update に w>0 の
        //    ゲートを足すとここが 0 に落ちて resolve が 0 を返し続ける)
        RtReservoirCpu self = RtReservoirEmpty();
        float wSelf = 0.0f;
        RtReservoirUpdate(self, wSelf, mirror, N, { 0.0f, 0.0f, 0.0f }, kRtReflClassDefault,
                          1.0f, 0.0f, 0.5f);
        TEST_CHECK(self.M == 1.0f && wSelf == 0.0f);

        // クラス別 M 上限: M'=100 は cap 8 に切り詰められる (M は 1 + 8 = 9)
        RtReservoirCpu big = cand;
        big.M = 100.0f;
        RtReservoirCpu r4 = RtReservoirEmpty();
        float w4 = 0.0f;
        RtReservoirUpdate(r4, w4, mirror, N, kLs, kRtReflClassProp, 1.0f, 1.0f, 0.5f);
        RtReservoirMerge(r4, w4, big, pHat, 8.0f, 1.0f, kRtRestirJacobianMax, 0.99f);
        TEST_CHECK(r4.M == 9.0f);
        TEST_CHECK(std::fabs(w4 - (1.0f + pHat * big.W * 8.0f)) < 1e-5f);
    }

    // ---- 書き戻し前の M クランプ: W が変わらない (= 絵が変わらない) ----
    {
        RtReservoirCpu r = RtReservoirEmpty();
        r.M = 20.0f;
        r.Ls = kLs;
        float wSum = 5.0f;
        const float pHat = 1.75f;
        const float wBefore = RtRestirWeight(wSum, r.M, pHat);
        const XMFLOAT3 outBefore = RtRestirResolve(r, wSum);
        RtRestirClampM(r, wSum, 8.0f);
        TEST_CHECK(r.M == 8.0f);
        TEST_CHECK(std::fabs(wSum - 2.0f) < 1e-6f); // 5 * 8/20
        TEST_CHECK(std::fabs(RtRestirWeight(wSum, r.M, pHat) - wBefore) < 1e-6f);
        const XMFLOAT3 outAfter = RtRestirResolve(r, wSum);
        TEST_CHECK(std::fabs(outAfter.x - outBefore.x) < 1e-6f
                   && std::fabs(outAfter.y - outBefore.y) < 1e-6f);
        // 上限より小さい M は触らない
        RtReservoirCpu r2 = RtReservoirEmpty();
        r2.M = 3.0f;
        float w2 = 1.25f;
        RtRestirClampM(r2, w2, 8.0f);
        TEST_CHECK(r2.M == 3.0f && w2 == 1.25f);
        // W の定義 (0 除算しない)
        TEST_CHECK(RtRestirWeight(5.0f, 0.0f, 1.0f) == 0.0f);
        TEST_CHECK(RtRestirWeight(5.0f, 2.0f, 0.0f) == 0.0f);
        TEST_CHECK(std::fabs(RtRestirWeight(6.0f, 2.0f, 1.5f) - 2.0f) < 1e-6f);
    }

    // ---- M67e/M67f: temporal 再利用の 2 パス往復 (rt_refl → spatial → 次フレーム) ----
    // 「静止カメラ・静止面・毎フレーム同じサンプル」= 凍結シードのスクリーンショットと
    // 同じ条件を CPU で回し、**M が 1 フレームに 1 ずつ伸びてクラス上限で止まる**ことと
    // **推定値が Ls のまま動かない**ことを固定する。
    // ★M67f で**履歴は rt_refl (temporal) の出力だけ**になった (spatial は書き戻さない)
    //   ので、このループも「パス 1 の出力を次フレームの候補にする」流れで回す。
    //   不変量は「**パス 1 が書き出す前に W を作り直す**」— テクスチャに載るのは wSum では
    //   なく W = wSum/(M·p̂) なので、ここを空 reservoir の 0 のまま書くと次フレームの
    //   `w = p̂ · W · min(M', mCap) · J` が 0 になって候補ごと外れ、**M が永久に 1 のまま**
    //   になる。M67e の実装で実際に踏んだバグで、GPU 側ではデバッグ 12 が
    //   frame 3 / 40 / 80 で同一画像になる形でしか出ない (絵は 1spp と同じなので golden も
    //   A5 も緑のまま通る) — CPU で先に落とす
    {
        const XMFLOAT3 P = { 0.0f, 0.0f, 0.0f }; // 受け側 (静止 = P_prev と同じ)
        // ヒット点はレイの先 4m、法線はレイに向く側 (両面規約)
        const XMFLOAT3 hitN = { -mirror.x, -mirror.y, -mirror.z };
        const XMFLOAT3 xs = { P.x + mirror.x * 4.0f, P.y + mirror.y * 4.0f,
                              P.z + mirror.z * 4.0f };
        const XMFLOAT3 ls = { 0.7f, 0.55f, 0.2f };
        const float alpha = 0.36f;
        const float mCap = kRtReflClassTable[kRtReflClassDefault].mCap;
        // 前フレームに rt_refl が書いた面 (ping-pong の read 側)
        RtReservoirCpu prev = RtReservoirEmpty();
        bool histValid = false;
        bool growOk = true;
        bool unbiasedOk = true;
        for (int frame = 0; frame < 24; ++frame) {
            // --- パス 1 = rt_refl.cs.hlsl (初期 reservoir → temporal 統合 → ClampM → W) ---
            RtReservoirCpu r = RtReservoirEmpty();
            float wSum = 0.0f;
            RtReservoirUpdate(r, wSum, xs, hitN, ls, kRtReflClassDefault, 1.0f,
                              RtLuminance(ls), 0.0f);
            if (histValid) {
                const XMFLOAT3 lPrev =
                    Normalize({ prev.xs.x - P.x, prev.xs.y - P.y, prev.xs.z - P.z });
                const float pHatPrev = RtRestirTargetPdf(prev.Ls, lPrev, V, N, alpha);
                // 受け側が動いていないので J = 1 ちょうど (rpos が正しく往復した状態)
                const float j = RtRestirJacobian(prev.xs, prev.ns, P, P);
                RtReservoirMerge(r, wSum, prev, pHatPrev, mCap, j, kRtRestirJacobianMax,
                                 0.75f);
            }
            RtRestirClampM(r, wSum, mCap);
            const XMFLOAT3 lSel = Normalize({ r.xs.x - P.x, r.xs.y - P.y, r.xs.z - P.z });
            r.W = RtRestirWeight(wSum, r.M, RtRestirTargetPdf(r.Ls, lSel, V, N, alpha));

            // --- パス 2 = rt_refl_restir_spatial.cs.hlsl (自画素 → ClampM → resolve) ---
            // ★M67f: **書き戻さない**ので prev には入れない。タップ 0 (静止した単一画素の
            //   モデル) なので、統合するのは自画素 1 つだけ = 出力は r を resolve した値
            RtReservoirCpu s = RtReservoirEmpty();
            float sSum = 0.0f;
            const float pc = RtRestirTargetPdf(r.Ls, lSel, V, N, alpha);
            RtReservoirMerge(s, sSum, r, pc, mCap, 1.0f, kRtRestirJacobianMax, 0.0f);
            RtRestirClampM(s, sSum, mCap);
            const XMFLOAT3 out = RtRestirResolve(s, sSum);

            const float expectM = std::min(static_cast<float>(frame + 1), mCap);
            if (s.M != expectM || r.M != expectM) {
                growOk = false; // 履歴側 (r) と表示側 (s) の両方が同じ伸び方をすること
            }
            // 同じサンプルを何度積んでも推定値は Ls のまま (再利用で暗化・明化しない)
            if (std::fabs(out.x - ls.x) > 1e-4f || std::fabs(out.y - ls.y) > 1e-4f
                || std::fabs(out.z - ls.z) > 1e-4f) {
                unbiasedOk = false;
            }
            // ★次フレームへ渡すのは**パス 1 の出力** (spatial の s ではない)
            prev = r;
            histValid = true;
        }
        MYE_LOG_INFO("  restir: temporal loop M = %.0f after 24 frames (cap %.0f)",
                     static_cast<double>(prev.M), static_cast<double>(mCap));
        TEST_CHECK(growOk);      // 1 フレームに 1 ずつ、cap で頭打ち
        TEST_CHECK(unbiasedOk);  // 出力は Ls のまま
        TEST_CHECK(prev.M == mCap);

        // ★変異テスト: **rt_refl 側の** W を 0 にする (= 書き出しで W を作り忘れた状態) と、
        //   次フレームの候補の重みが 0 になって M が 1 から伸びない = 上のループが守る性質。
        //   手元で再現するなら rt_refl.cs.hlsl の `r.W = RtRestirWeight(...)` を 0 にする
        RtReservoirCpu stale = prev;
        stale.W = 0.0f;
        RtReservoirCpu r2 = RtReservoirEmpty();
        float w2 = 0.0f;
        RtReservoirUpdate(r2, w2, xs, hitN, ls, kRtReflClassDefault, 1.0f, RtLuminance(ls),
                          0.0f);
        const XMFLOAT3 lStale =
            Normalize({ stale.xs.x - P.x, stale.xs.y - P.y, stale.xs.z - P.z });
        RtReservoirMerge(r2, w2, stale, RtRestirTargetPdf(stale.Ls, lStale, V, N, alpha),
                         mCap, 1.0f, kRtRestirJacobianMax, 0.75f);
        TEST_CHECK(r2.M == 1.0f);
    }

    // ---- M67f: 半径を受け側の α で縮める係数 ----
    // 「滑らかな面ほど空間再利用を切る」を決める唯一の式。ここが 1 に張り付くと
    // 鏡面で spatial が全開になり、round 1 で観測した暗化 (-5.2%) とフリッカー増加が戻る
    {
        // 基準そのもの (粗さ kRtReflMaxRoughness) で等倍、それ以上は 1 に飽和
        TEST_CHECK(RtRestirRadiusScale(kRtRestirRadiusAlphaRef, kRtRestirRadiusAlphaRef)
                   == 1.0f);
        TEST_CHECK(RtRestirRadiusScale(1.0f, kRtRestirRadiusAlphaRef) == 1.0f);
        // 粗さ 0.5 (α 0.25) = 目標帯、粗さ 0.10 (α 0.01) = 鏡面
        const float mid = RtRestirRadiusScale(0.25f, kRtRestirRadiusAlphaRef);
        const float spec = RtRestirRadiusScale(0.01f, kRtRestirRadiusAlphaRef);
        MYE_LOG_INFO("  restir: radius scale alpha 0.25 -> %.3f / 0.01 -> %.3f",
                     static_cast<double>(mid), static_cast<double>(spec));
        TEST_CHECK(std::fabs(mid - 0.6944f) < 1e-3f);
        TEST_CHECK(std::fabs(spec - 0.0278f) < 1e-3f);
        // 退化入力は全て 0 (= タップ 0)。**NaN も比較 2 つで落ちる**
        TEST_CHECK(RtRestirRadiusScale(0.0f, kRtRestirRadiusAlphaRef) == 0.0f);
        TEST_CHECK(RtRestirRadiusScale(-0.5f, kRtRestirRadiusAlphaRef) == 0.0f);
        TEST_CHECK(RtRestirRadiusScale(std::numeric_limits<float>::quiet_NaN(),
                                       kRtRestirRadiusAlphaRef)
                   == 0.0f);
        TEST_CHECK(RtRestirRadiusScale(0.25f, 0.0f) == 0.0f);
        // 鏡面では Default の半径 8px が 1px を切る = spatial が自然に切れる
        TEST_CHECK(kRtReflClassTable[kRtReflClassDefault].radiusPx * spec < 1.0f);
        // 目標帯では Prop の半径 12px が 1px を超える = spatial が効く
        TEST_CHECK(kRtReflClassTable[kRtReflClassProp].radiusPx * mid >= 1.0f);
    }

    // ---- M67f: 空間タップ (Vogel 螺旋) ----
    // 空間再利用でどこから借りるかを決める唯一の幾何。**GPU 側と同じ式**なので、
    // ここが通れば「クラスの半径を超えて借りない」「タップが同じ点に重ならない」
    // 「回転で配置が回るだけ」が GPU でも成り立つ。クラス表の数字が実際の
    // タップ距離に効いていることを機械で固定するのはここだけ (絵では読めない)
    {
        bool insideOk = true, distinctOk = true, rotOk = true, monotoneOk = true;
        double maxRatio = 0.0;
        for (int k = 1; k <= kRtRestirMaxTaps; ++k) {
            for (int ci = 0; ci < kRtReflClassCount; ++ci) {
                const float radius = kRtReflClassTable[ci].radiusPx;
                std::vector<XMFLOAT2> taps;
                for (int i = 0; i < k; ++i) {
                    const XMFLOAT2 t = RtRestirVogelTap(i, k, radius, 0.7f);
                    const double len = std::sqrt(static_cast<double>(t.x) * t.x
                                                 + static_cast<double>(t.y) * t.y);
                    if (len > static_cast<double>(radius) + 1e-4) {
                        insideOk = false; // 半径をはみ出したら「クラスで縛る」が嘘になる
                    }
                    maxRatio = (std::max)(maxRatio, len / static_cast<double>(radius));
                    for (const XMFLOAT2& p : taps) {
                        if (std::fabs(p.x - t.x) < 1e-5f && std::fabs(p.y - t.y) < 1e-5f) {
                            distinctOk = false; // 同じ点を 2 回借りると M だけ増える
                        }
                    }
                    taps.push_back(t);

                    // 回転角を足すと**全タップがちょうどその角度だけ回る** (配置は不変)。
                    // 画素ごとに回すのが「螺旋の偏りを絵に焼き付けない」仕掛けなので、
                    // 回転が半径や順序を変えていないことをここで固定する
                    constexpr float kRot = 1.234f;
                    const XMFLOAT2 tRot = RtRestirVogelTap(i, k, radius, 0.7f + kRot);
                    const float c = std::cos(kRot), s = std::sin(kRot);
                    if (std::fabs(t.x * c - t.y * s - tRot.x) > 1e-3f
                        || std::fabs(t.x * s + t.y * c - tRot.y) > 1e-3f) {
                        rotOk = false;
                    }
                }
                // 半径を 2 倍にすると全タップの距離もちょうど 2 倍 (線形スケール)
                for (int i = 0; i < k; ++i) {
                    const XMFLOAT2 a = RtRestirVogelTap(i, k, radius, 0.7f);
                    const XMFLOAT2 b = RtRestirVogelTap(i, k, radius * 2.0f, 0.7f);
                    if (std::fabs(a.x * 2.0f - b.x) > 1e-3f
                        || std::fabs(a.y * 2.0f - b.y) > 1e-3f) {
                        monotoneOk = false;
                    }
                }
            }
        }
        MYE_LOG_INFO("  restir: vogel taps max |offset| / radius = %.4f", maxRatio);
        TEST_CHECK(insideOk && distinctOk && rotOk && monotoneOk);
        // count = 0 で呼んでも 0 除算しない (呼ぶ側がループを回さないのが正だが、
        // ここが NaN を返すと画面全体が NaN になるので二重に守っている)
        const XMFLOAT2 degenerate = RtRestirVogelTap(0, 0, 8.0f, 0.0f);
        TEST_CHECK(degenerate.x == degenerate.x && degenerate.y == degenerate.y);
        // **主役ほど近くからしか借りない** — Hero の全タップは Prop の半径の内側。
        // クラス表の向き (§4.1) が実際のタップ距離に効いていることの機械証明
        bool heroInsideProp = true;
        for (int i = 0; i < kRtRestirMaxTaps; ++i) {
            const XMFLOAT2 h = RtRestirVogelTap(i, kRtRestirMaxTaps,
                                                kRtReflClassTable[kRtReflClassHero].radiusPx,
                                                0.3f);
            if (std::sqrt(h.x * h.x + h.y * h.y)
                > kRtReflClassTable[kRtReflClassProp].radiusPx) {
                heroInsideProp = false;
            }
        }
        TEST_CHECK(heroInsideProp);
    }

    // ---- クラス別パラメータ表 ----
    {
        bool tapsOk = true, capOk = true, radiusOk = true;
        for (int i = 0; i < kRtReflClassCount; ++i) {
            const RtReflClassParams& p = kRtReflClassTable[i];
            if (!(p.taps >= 0.0f && p.taps <= static_cast<float>(kRtRestirMaxTaps))) {
                tapsOk = false; // [loop] の静的上限を超えるとタップが黙って落ちる
            }
            if (!(p.mCap >= 1.0f && p.mCap <= kRtRestirMaxM)) {
                capOk = false; // 半精度の w に載らない M は書き戻しで値が動く
            }
            if (!(p.radiusPx > 0.0f) || p.pad != 0.0f) {
                radiusOk = false;
            }
        }
        TEST_CHECK(tapsOk && capOk && radiusOk);
        // 中立クラスは元計画の {8, 4, 16}
        TEST_CHECK(kRtReflClassTable[kRtReflClassDefault].radiusPx == 8.0f
                   && kRtReflClassTable[kRtReflClassDefault].taps == 4.0f
                   && kRtReflClassTable[kRtReflClassDefault].mCap == 16.0f);
        // **主役ほど保守的** (半径・タップ・M 上限のどれも Hero < Prop)。
        // 向きが逆だと「主役が最もにじむ」という真逆の絵になるので機械で固定する
        const RtReflClassParams& hero = kRtReflClassTable[kRtReflClassHero];
        const RtReflClassParams& prop = kRtReflClassTable[kRtReflClassProp];
        TEST_CHECK(hero.radiusPx < prop.radiusPx && hero.taps < prop.taps
                   && hero.mCap < prop.mCap);
        TEST_CHECK(kRtReflClassTable[kRtReflClassCharacter].mCap
                   < kRtReflClassTable[kRtReflClassVehicle].mCap);

        // 実行時パラメータの既定 = 定数表 + M46h の SVGF 設定そのもの
        const RtReflRestirParams def;
        bool tableOk = true;
        for (int i = 0; i < kRtReflClassCount; ++i) {
            if (def.classTable[i].radiusPx != kRtReflClassTable[i].radiusPx
                || def.classTable[i].taps != kRtReflClassTable[i].taps
                || def.classTable[i].mCap != kRtReflClassTable[i].mCap) {
                tableOk = false;
            }
        }
        TEST_CHECK(tableOk);
        TEST_CHECK(def.svgfHistory == kRtReflMaxHistory);
        TEST_CHECK(def.atrousIterations == kRtReflAtrousIterations);
        // ★spatial の既定は **0** (sub-06 round 2 の計測で決めた。spec §7 U7) —
        //   目標帯で temporal 単独より悪化したので既定 off、ノブは残す。
        //   ここを 1 に戻すなら「目標帯で改善する」計測をやり直すこと
        TEST_CHECK(def.spatial == 0 && def.visRay == 0 && def.classOverride == -1);
        TEST_CHECK(def.radiusAlphaRef == kRtRestirRadiusAlphaRef);
    }
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
    TestRestir();
    if (g_failCount == 0) {
        MYE_LOG_INFO("==== Ray tracing self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== Ray tracing self test: %d FAILED ====", g_failCount);
    return false;
}

} // namespace mye
