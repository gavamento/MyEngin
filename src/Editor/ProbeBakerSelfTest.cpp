#include "Editor/ProbeBakerSelfTest.h"

#include <cmath>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/Log.h"
#include "Engine/Engine/ProbeBaker.h"

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

    MYE_LOG_INFO("==== reflection probe capture self test: %s ====",
                 failCount == 0 ? "PASS" : "FAIL");
    return failCount == 0;
}

} // namespace mye
