#include "Editor/ProbeBakerSelfTest.h"

#include <cmath>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/Components.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/ProbeBaker.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Renderer/RenderTypes.h"

using namespace DirectX;

namespace mye {
namespace {

const char* kFaceName[6] = { "+X", "-X", "+Y", "-Y", "+Z", "-Z" };

bool NearVec(const XMFLOAT3& a, const XMFLOAT3& b, float eps)
{
    return std::fabs(a.x - b.x) < eps && std::fabs(a.y - b.y) < eps && std::fabs(a.z - b.z) < eps;
}

float Dot(const XMFLOAT3& a, const XMFLOAT3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

XMFLOAT3 Cross(const XMFLOAT3& a, const XMFLOAT3& b)
{
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}

// 方向 → 滑らかな放射輝度。継ぎ目の検査に「向きだけが効く」試験材料が要る
// (実写だと物体の輪郭と面のずれが混ざって、しきい値が意味を持たなくなる)
void SmoothRadiance(const XMFLOAT3& d, float* rgb)
{
    rgb[0] = 0.50f + 0.40f * d.x;
    rgb[1] = 0.50f + 0.40f * d.y;
    rgb[2] = 0.50f + 0.40f * d.z;
}

std::vector<float> MakeSmoothCube(int size)
{
    std::vector<float> rgb(static_cast<size_t>(6) * size * size * 3, 0.0f);
    for (int f = 0; f < 6; ++f) {
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);
                const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);
                SmoothRadiance(ProbeFaceDir(f, u, v),
                               &rgb[((static_cast<size_t>(f) * size + y) * size + x) * 3]);
            }
        }
    }
    return rgb;
}

} // namespace

bool RunProbeBakerSelfTest()
{
    MYE_LOG_INFO("==== reflection probe capture (ProbeBaker) self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    // ---- 面基底そのもの ----
    {
        bool unit = true, ortho = true, handed = true, axisAligned = true;
        for (int f = 0; f < 6; ++f) {
            const CubeFaceBasis& b = CubeFace(f);
            unit = unit && std::fabs(Dot(b.forward, b.forward) - 1.0f) < 1e-6f
                && std::fabs(Dot(b.right, b.right) - 1.0f) < 1e-6f
                && std::fabs(Dot(b.up, b.up) - 1.0f) < 1e-6f;
            ortho = ortho && std::fabs(Dot(b.forward, b.right)) < 1e-6f
                && std::fabs(Dot(b.forward, b.up)) < 1e-6f && std::fabs(Dot(b.right, b.up)) < 1e-6f;
            // ★LH の右 = up × forward。**XMMatrixLookToLH が組む基底と同じ式**であることが、
            //   「撮影した向き == プリフィルタがサンプルする向き」の土台になっている
            handed = handed && NearVec(Cross(b.up, b.forward), b.right, 1e-6f);
            const XMFLOAT3& fw = b.forward;
            axisAligned = axisAligned
                && (std::fabs(std::fabs(fw.x) + std::fabs(fw.y) + std::fabs(fw.z) - 1.0f) < 1e-6f);
        }
        check(unit, "every cube face basis vector is unit length");
        check(ortho, "forward / right / up are mutually orthogonal on every face");
        check(handed, "right == up x forward on every face (matches XMMatrixLookToLH)");
        check(axisAligned, "every face forward is an axis direction");
        check(CubeFace(-1).forward.x == CubeFace(0).forward.x
                  && CubeFace(99).forward.x == CubeFace(0).forward.x,
              "an out-of-range face index is clamped instead of reading past the table");
    }

    // ---- 方向 <-> (面, uv) の往復 ----
    {
        bool centreOk = true;
        for (int f = 0; f < 6; ++f) {
            centreOk = centreOk && NearVec(ProbeFaceDir(f, 0.5f, 0.5f), CubeFace(f).forward, 1e-6f);
        }
        check(centreOk, "the centre texel of each face looks straight along that face's forward");

        bool roundTrip = true;
        int worstFace = -1;
        for (int f = 0; f < 6 && roundTrip; ++f) {
            for (int iy = 0; iy < 17; ++iy) {
                for (int ix = 0; ix < 17; ++ix) {
                    const float u = 0.02f + 0.96f * (static_cast<float>(ix) / 16.0f);
                    const float v = 0.02f + 0.96f * (static_cast<float>(iy) / 16.0f);
                    int f2 = -1;
                    float u2 = 0.0f, v2 = 0.0f;
                    const bool ok = ProbeDirToFaceUv(ProbeFaceDir(f, u, v), f2, u2, v2);
                    if (!ok || f2 != f || std::fabs(u2 - u) > 1e-5f || std::fabs(v2 - v) > 1e-5f) {
                        roundTrip = false;
                        worstFace = f;
                        break;
                    }
                }
            }
        }
        if (!roundTrip) {
            MYE_LOG_ERROR("  (round trip broke on face %s)",
                          (worstFace >= 0) ? kFaceName[worstFace] : "?");
        }
        check(roundTrip, "ProbeDirToFaceUv is the exact inverse of ProbeFaceDir inside every face");
    }

    // ---- 退化入力 ----
    {
        int f = -1;
        float u = 0.0f, v = 0.0f;
        check(!ProbeDirToFaceUv(XMFLOAT3{ 0, 0, 0 }, f, u, v),
              "the zero direction is rejected instead of producing a NaN uv");
    }

    // ---- 面の外へ 1/2 テクセル出ると必ず隣の面に入る (隙間が無い) ----
    {
        const int n = 32;
        const float inv = 1.0f / static_cast<float>(n);
        bool allNeighbours = true;
        for (int f = 0; f < 6; ++f) {
            for (int i = 0; i < n; ++i) {
                const float c = (static_cast<float>(i) + 0.5f) * inv;
                const float probes[4][2] = { { -0.5f * inv, c },
                                             { 1.0f + 0.5f * inv, c },
                                             { c, -0.5f * inv },
                                             { c, 1.0f + 0.5f * inv } };
                for (const auto& p : probes) {
                    int f2 = -1;
                    float u2 = 0.0f, v2 = 0.0f;
                    const bool ok = ProbeDirToFaceUv(ProbeFaceDir(f, p[0], p[1]), f2, u2, v2);
                    allNeighbours = allNeighbours && ok && f2 != f && u2 >= -1e-4f
                        && u2 <= 1.0f + 1e-4f && v2 >= -1e-4f && v2 <= 1.0f + 1e-4f;
                }
            }
        }
        check(allNeighbours,
              "stepping half a texel off any edge lands on a neighbouring face, inside its uv");
    }

    // ---- ★撮影カメラ == プリフィルタの方向 ----
    // 面のビュー行列 + 90 度の射影を通した逆投影が ProbeFaceDir と一致すること。
    // ここが崩れると「焼いた反射が面ごとに 90 度ずれる」が起き、絵からは追えない
    {
        const XMFLOAT3 pos = { 3.0f, -2.0f, 7.0f }; // 原点以外で試す (平行移動の取り違え検出)
        const XMFLOAT4X4 projM = ProbeFaceProj(0.1f, 500.0f);
        const XMMATRIX proj = XMLoadFloat4x4(&projM);
        bool match = true;
        float worst = 0.0f;
        for (int f = 0; f < 6; ++f) {
            const XMFLOAT4X4 viewM = ProbeFaceView(f, pos);
            const XMMATRIX view = XMLoadFloat4x4(&viewM);
            const XMMATRIX invVP = XMMatrixInverse(nullptr, XMMatrixMultiply(view, proj));
            for (int iy = 0; iy <= 8; ++iy) {
                for (int ix = 0; ix <= 8; ++ix) {
                    const float ndcX = -1.0f + 2.0f * (static_cast<float>(ix) / 8.0f);
                    const float ndcY = -1.0f + 2.0f * (static_cast<float>(iy) / 8.0f);
                    const XMVECTOR world =
                        XMVector3TransformCoord(XMVectorSet(ndcX, ndcY, 0.5f, 1.0f), invVP);
                    XMFLOAT3 dir;
                    XMStoreFloat3(&dir,
                                  XMVector3Normalize(XMVectorSubtract(
                                      world, XMVectorSet(pos.x, pos.y, pos.z, 0.0f))));
                    // NDC (x,y) と テクスチャ座標 (u,v) の対応: x = 2u-1 / y = 1-2v
                    const XMFLOAT3 want =
                        ProbeFaceDir(f, (ndcX + 1.0f) * 0.5f, (1.0f - ndcY) * 0.5f);
                    const float err = std::fabs(dir.x - want.x) + std::fabs(dir.y - want.y)
                        + std::fabs(dir.z - want.z);
                    worst = (err > worst) ? err : worst;
                    match = match && err < 1e-4f;
                }
            }
        }
        MYE_LOG_INFO("  (worst capture-vs-prefilter direction error: %.2e)",
                     static_cast<double>(worst));
        check(match,
              "unprojecting through the face camera reproduces the direction the prefilter samples");

        // 90 度・アスペクト 1 = 6 面がちょうど全方位を覆う条件 (m00 == m11 == 1)
        XMFLOAT4X4 p = ProbeFaceProj(0.1f, 500.0f);
        check(std::fabs(p._11 - 1.0f) < 1e-5f && std::fabs(p._22 - 1.0f) < 1e-5f,
              "the face projection is exactly 90 degrees with a square aspect");
    }

    // ---- 継ぎ目チェックの歯 ----
    {
        const int n = 32;
        std::vector<float> cube = MakeSmoothCube(n);
        ProbeSeamStats good = {};
        const bool okGood = ProbeSeamCheck(cube, n, good);
        check(okGood && good.samples == 6 * 4 * n,
              "the seam check visits every border texel of all six faces");

        // 面 +Z (4) を 90 度回す = 「撮影の面基底だけがずれた」ときに起きる形
        std::vector<float> broken = cube;
        for (int y = 0; y < n; ++y) {
            for (int x = 0; x < n; ++x) {
                const size_t dst = ((static_cast<size_t>(4) * n + y) * n + x) * 3;
                const size_t src = ((static_cast<size_t>(4) * n + x) * n + (n - 1 - y)) * 3;
                broken[dst + 0] = cube[src + 0];
                broken[dst + 1] = cube[src + 1];
                broken[dst + 2] = cube[src + 2];
            }
        }
        ProbeSeamStats bad = {};
        const bool okBad = ProbeSeamCheck(broken, n, bad);
        MYE_LOG_INFO("  (seam ratio: aligned %.3f -> one face rotated 90 deg %.3f, limit %.2f)",
                     static_cast<double>(good.seamRatio), static_cast<double>(bad.seamRatio),
                     static_cast<double>(kProbeSeamRatioLimit));
        // ★1 前後 = 「継ぎ目をまたぐ 1 テクセルの段差が、面の中の 1 テクセルと同じ」
        check(okGood && good.seamRatio < kProbeSeamRatioLimit,
              "a correctly oriented cube's seams are as smooth as the inside of its faces");
        check(okBad && bad.seamRatio >= kProbeSeamRatioLimit,
              "rotating a single face by 90 degrees pushes the seam ratio past the CLI limit");
        check(okBad && bad.seamRatio > 5.0f * good.seamRatio,
              "the rotated case is separated from the aligned case by more than 5x");

        ProbeSeamStats junk = {};
        check(!ProbeSeamCheck(cube, n + 1, junk),
              "a size that does not match the buffer is rejected instead of reading out of bounds");
        check(!ProbeSeamCheck({}, 0, junk), "an empty cube is rejected");
    }

    // ---- 定数の健全性 ----
    {
        check(ProbeBaker::kCaptureSize == EnvMapBaker::kSpecSize,
              "the capture resolution matches the prefiltered cube (finer would be thrown away)");
        check((ProbeBaker::kCaptureSize & (ProbeBaker::kCaptureSize - 1)) == 0,
              "the capture resolution is a power of two (the prefilter halves it per mip)");
    }

    // ================= M56f: 合成 (ボックス投影 + 影響の重み + 選択) =================
    // ★ここは**シェーダの CPU 鏡**。HLSL 側 (common.hlsli の ReflProbe*) と式が
    //   食い違っても絵は普通に出る (反射がわずかにずれるだけ) ので、機械検査でしか
    //   捕まらない。両者を同時に直す約束は各関数のコメントに書いてある
    RegisterBuiltinComponents(); // sTypeId 解決 (冪等)

    // 中心 (10, 2, -4) / 半径 (4, 2, 6) / ブレンド 1 / ボックス投影 on
    ReflectionProbeGpu probe;
    probe.centerIntensity = { 10.0f, 2.0f, -4.0f, 1.0f };
    probe.boxMin = { 6.0f, 0.0f, -10.0f, 1.0f };
    probe.boxMax = { 14.0f, 4.0f, 2.0f, 1.0f };

    // ---- レイアウト (CB の配列長そのもの) ----
    {
        check(sizeof(ReflectionProbeGpu) == 48, "ReflectionProbeGpu is 48 bytes (3 x float4)");
        ReflectionProbeSet empty;
        check(empty.count == 0 && empty.cubeArray == nullptr,
              "a default ReflectionProbeSet means 'no probes' (the light pass skips it)");
        check(kMaxReflectionProbes > 0
                  && sizeof(empty.probes) / sizeof(empty.probes[0]) == kMaxReflectionProbes,
              "the CB array length is kMaxReflectionProbes (mirrored by MYE_MAX_REFLECTION_PROBES)");
    }

    // ---- 影響の重み ----
    {
        check(ReflProbeWeight(probe, XMFLOAT3{ 10.0f, 2.0f, -4.0f }) == 1.0f,
              "the box centre has full weight");
        check(ReflProbeWeight(probe, XMFLOAT3{ 20.0f, 2.0f, -4.0f }) == 0.0f,
              "a point outside the box has exactly zero weight");
        check(ReflProbeWeight(probe, XMFLOAT3{ 14.0f, 2.0f, -4.0f }) == 0.0f,
              "a point exactly on the box surface has exactly zero weight");
        // ★ちょうど 0 が出ることが「プローブの外は 1 ビットも足さない」の根拠
        check(std::fabs(ReflProbeWeight(probe, XMFLOAT3{ 13.5f, 2.0f, -4.0f }) - 0.5f) < 1e-6f,
              "half a blend distance inside the surface gives half weight");
        // 一番近い面が効く (y は半径 2 なので、y 方向の縁の方が近い)
        check(std::fabs(ReflProbeWeight(probe, XMFLOAT3{ 10.0f, 3.7f, -4.0f }) - 0.3f) < 1e-6f,
              "the nearest face decides the weight, not the first axis");
        ReflectionProbeGpu hard = probe;
        hard.boxMin.w = 0.0f; // ブレンド距離 0 = 境界でいきなり切り替わる
        check(ReflProbeWeight(hard, XMFLOAT3{ 13.999f, 2.0f, -4.0f }) == 1.0f
                  && ReflProbeWeight(hard, XMFLOAT3{ 14.001f, 2.0f, -4.0f }) == 0.0f,
              "a zero blend distance is a hard edge instead of a divide by zero");
    }

    // ---- ボックス投影 (視差補正) ----
    {
        // 撮影点そのものから見た方向は補正しても変わらない (交点 - 中心 == R * t)
        const XMFLOAT3 c = { 10.0f, 2.0f, -4.0f };
        const XMFLOAT3 up = { 0.0f, 1.0f, 0.0f };
        check(NearVec(ReflProbeDir(probe, c, up), up, 1e-5f),
              "at the capture point box projection is the identity");

        // ★**これが視差補正の本体**: 同じ反射ベクトルでも立つ場所が違えば向きが変わる。
        //   ここが恒等になっていたら「無限遠キューブ」と同じ = 補正が効いていない
        const XMFLOAT3 offCentre = { 13.0f, 1.0f, 1.0f };
        const XMFLOAT3 d1 = ReflProbeDir(probe, offCentre, up);
        check(!NearVec(d1, up, 1e-3f),
              "away from the capture point the same reflection vector maps to a different direction");
        // 上向きの光線は必ず天井 (y = boxMax.y = 4) に当たる = 補正後も上を向いている
        check(d1.y > 0.0f, "an upward ray still points upward after the correction");
        // 交点は箱の面の上に乗る (天井なら y == 4)。撮影点 + dir*len で戻して確かめる
        {
            const XMFLOAT3 hit = { offCentre.x, 4.0f, offCentre.z };
            XMFLOAT3 want = { hit.x - c.x, hit.y - c.y, hit.z - c.z };
            const float len = std::sqrt(want.x * want.x + want.y * want.y + want.z * want.z);
            want = { want.x / len, want.y / len, want.z / len };
            check(NearVec(d1, want, 1e-5f),
                  "the corrected direction points from the capture point to the box wall hit");
        }
        // 軸に平行な光線で 0 除算しない (NaN が 1 つ混ざると min ごと壊れる)
        const XMFLOAT3 axis = ReflProbeDir(probe, offCentre, XMFLOAT3{ 1.0f, 0.0f, 0.0f });
        check(!std::isnan(axis.x) && !std::isnan(axis.y) && !std::isnan(axis.z),
              "an axis-parallel reflection vector does not produce NaN");
        // 無限遠プローブは素通し
        ReflectionProbeGpu infinite = probe;
        infinite.boxMax.w = 0.0f;
        check(NearVec(ReflProbeDir(infinite, offCentre, up), up, 1e-6f),
              "with box projection off the reflection vector is passed through unchanged");
    }

    // ---- 選択 (どのプローブが効くか) ----
    {
        ReflectionProbeGpu list[3];
        // 0: 大きい箱 (ブレンドが広い = 中心でも重み 0.5) / 1: 小さい箱 (中心で 1.0) /
        // 2: 遠くの箱。**重みが飽和しない値**にしてあるのが要点 — どちらも 1.0 だと
        // 同点になり、「深く入っている方が勝つ」ではなく「添字が小さい方が勝つ」を試験してしまう
        list[0].centerIntensity = { 0, 0, 0, 1 };
        list[0].boxMin = { -20.0f, -20.0f, -20.0f, 40.0f };
        list[0].boxMax = { 20.0f, 20.0f, 20.0f, 1.0f };
        list[1].centerIntensity = { 0, 0, 0, 1 };
        list[1].boxMin = { -3.0f, -3.0f, -3.0f, 0.5f };
        list[1].boxMax = { 3.0f, 3.0f, 3.0f, 1.0f };
        list[2].centerIntensity = { 100, 0, 0, 1 };
        list[2].boxMin = { 90.0f, -5.0f, -5.0f, 1.0f };
        list[2].boxMax = { 110.0f, 5.0f, 5.0f, 1.0f };

        float w = -1.0f;
        check(ReflProbeSelect(list, 3, XMFLOAT3{ 0.0f, 0.0f, 0.0f }, w) == 1 && w == 1.0f,
              "the probe the point is deepest inside wins");
        check(ReflProbeSelect(list, 3, XMFLOAT3{ 10.0f, 0.0f, 0.0f }, w) == 0,
              "outside the small box the large one takes over");
        check(ReflProbeSelect(list, 3, XMFLOAT3{ 200.0f, 0.0f, 0.0f }, w) == -1 && w == 0.0f,
              "a point outside every box selects nothing and reports zero weight");
        check(ReflProbeSelect(list, 0, XMFLOAT3{ 0.0f, 0.0f, 0.0f }, w) == -1 && w == 0.0f,
              "an empty probe list selects nothing");
        // ★同点は**添字の小さい方**。ここが揺れると、収集順が変わっただけで
        //   映り込みが別のプローブへ飛ぶ (規則 7 のタイブレークと同じ話)
        ReflectionProbeGpu tie[2] = { list[1], list[1] };
        check(ReflProbeSelect(tie, 2, XMFLOAT3{ 0.0f, 0.0f, 0.0f }, w) == 0,
              "a tie is broken towards the lower index (deterministic selection)");
    }

    // ---- kComponentNoHash: プローブを置いてもワールドハッシュが動かない ----
    // ★「M56 は .rep 互換の作業が 1 つも要らない」の機械証明 (DecalSelfTest と同じ檻)
    {
        Scene scene;
        GameObject host = scene.CreateGameObjectTracked("ProbeHost");
        World& w = scene.GetWorld();
        w.ApplyStructuralChanges();
        const uint64_t before = HashWorld(w, nullptr);
        auto* rp = w.AddComponent<ReflectionProbeComponent>(host.Id());
        w.ApplyStructuralChanges();
        check(rp != nullptr, "ReflectionProbeComponent can be added to an entity");
        if (rp != nullptr) {
            rp->extents = { 3.0f, 4.0f, 5.0f };
            rp->blendDistance = 2.0f;
            rp->intensity = 0.5f;
            rp->boxProjection = false;
        }
        check(before == HashWorld(w, nullptr),
              "ReflectionProbeComponent is kComponentNoHash (the world hash never moves)");
    }

    MYE_LOG_INFO("==== reflection probe capture self test: %s ====",
                 failCount == 0 ? "PASS" : "FAIL");
    return failCount == 0;
}

} // namespace mye
