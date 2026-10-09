//====================================================================================
//                          OcclusionMath.h
//  MyEngin/ 秋田蓮音                                                       10/09/2026
//                                          GPU オクルージョン判定の純関数 (HLSL の CPU 鏡)
//====================================================================================
#pragma once
#include <algorithm>
#include <cfloat>
#include <cmath>

#include <DirectXMath.h>

#include "Engine/Renderer/Passes/HzbPass.h"

namespace mye::occlusion {

// ---- occlusion_cull.cs.hlsl と共有する定数 ----
// 整数のものは tools\check_rules.ps1 が HLSL 側の #define と機械照合する。kMinW だけは小数なので
// 照合の対象外 (変更時は MYE_OCC_MIN_W も揃えること)
constexpr int kThreadGroupSize = 64; // MYE_OCC_TG
constexpr int kDepthBiasSteps = 32;  // MYE_OCC_BIAS_STEPS (24bit 深度の刻み幅の何個ぶん甘くするか)
constexpr int kRectMarginPx = 1;     // MYE_OCC_MARGIN_PX (ジッタと丸めの余白)
constexpr float kMinW = 0.001f;      // MYE_OCC_MIN_W (これ以下の w の頂点を含む箱は可視)
constexpr float kDepthStep = 1.0f / 16777215.0f;

// 箱 1 個を画面へ投影した結果。conservativeVisible が true なら HZB を引かずに可視と決まっている
struct Probe {
    bool conservativeVisible = true;
    float nearestDepth = 0.0f; // 箱の最も手前の非線形深度
    int mip = 0;               // 判定に使う HZB の段
    int tx0 = 0;               // その段のテクセル範囲 (両端含む)
    int ty0 = 0;
    int tx1 = 0;
    int ty1 = 0;
};

// AABB の 8 頂点をビュー射影行列 (行ベクトル規約: p * M) で画面へ投影する。
//
// 次のいずれかは保守的に可視にする (HZB を引かない):
//   ・w が kMinW 以下の頂点がある / 最前面の深度が 0 未満 = ニア面をまたぐ
//   ・余白込みの画面矩形が画面からはみ出す
// 箱は凸で、透視投影は w > 0 の範囲で凸性を保つので、最前面の深度と矩形は 8 頂点の min/max でよい
inline Probe ProjectAabb(const DirectX::XMFLOAT4X4& vp, const float bmin[3], const float bmax[3],
                         int width, int height, int mipCount)
{
    Probe probe; // 既定 = 可視
    if (width <= 0 || height <= 0 || mipCount <= 0) {
        return probe;
    }
    float minSx = FLT_MAX;
    float minSy = FLT_MAX;
    float maxSx = -FLT_MAX;
    float maxSy = -FLT_MAX;
    float minZ = FLT_MAX;
    for (int c = 0; c < 8; ++c) {
        const float x = (c & 1) ? bmax[0] : bmin[0];
        const float y = (c & 2) ? bmax[1] : bmin[1];
        const float z = (c & 4) ? bmax[2] : bmin[2];
        const float cx = x * vp._11 + y * vp._21 + z * vp._31 + vp._41;
        const float cy = x * vp._12 + y * vp._22 + z * vp._32 + vp._42;
        const float cz = x * vp._13 + y * vp._23 + z * vp._33 + vp._43;
        const float cw = x * vp._14 + y * vp._24 + z * vp._34 + vp._44;
        if (cw <= kMinW) {
            return probe;
        }
        const float invW = 1.0f / cw;
        const float sx = (cx * invW * 0.5f + 0.5f) * static_cast<float>(width);
        const float sy = (0.5f - cy * invW * 0.5f) * static_cast<float>(height);
        minSx = std::min(minSx, sx);
        maxSx = std::max(maxSx, sx);
        minSy = std::min(minSy, sy);
        maxSy = std::max(maxSy, sy);
        minZ = std::min(minZ, cz * invW);
    }
    if (minZ < 0.0f) {
        return probe;
    }
    const float margin = static_cast<float>(kRectMarginPx);
    const float x0 = minSx - margin;
    const float y0 = minSy - margin;
    const float x1 = maxSx + margin;
    const float y1 = maxSy + margin;
    if (x0 < 0.0f || y0 < 0.0f || x1 > static_cast<float>(width) || y1 > static_cast<float>(height)) {
        return probe;
    }

    // 矩形が 2 テクセル程度に収まる段。float の log2 を使わず整数で数える (CPU と GPU で段が割れない)
    const float size = std::max(x1 - x0, y1 - y0);
    int mip = 0;
    while (mip < mipCount - 1 && static_cast<float>(1 << mip) < size) {
        ++mip;
    }
    const int wm = HzbMipExtent(width, mip);
    const int hm = HzbMipExtent(height, mip);
    const float fw = static_cast<float>(width);
    const float fh = static_cast<float>(height);
    probe.conservativeVisible = false;
    probe.nearestDepth = minZ;
    probe.mip = mip;
    probe.tx0 = std::clamp(static_cast<int>(std::floor(x0 * static_cast<float>(wm) / fw)), 0, wm - 1);
    probe.ty0 = std::clamp(static_cast<int>(std::floor(y0 * static_cast<float>(hm) / fh)), 0, hm - 1);
    probe.tx1 = std::clamp(static_cast<int>(std::floor(x1 * static_cast<float>(wm) / fw)), 0, wm - 1);
    probe.ty1 = std::clamp(static_cast<int>(std::floor(y1 * static_cast<float>(hm) / fh)), 0, hm - 1);
    return probe;
}

// 範囲内の max-Z が箱の最前面より手前なら隠れている (甘く見るのは kDepthBiasSteps 刻みぶん)。
// 箱の面が深度バッファにそのまま書かれている場合 (床など厚みの無い箱) に、丸めで自分自身を
// 隠れていると誤判定しないための余裕
inline bool IsOccluded(const Probe& probe, float hzbMaxZ)
{
    if (probe.conservativeVisible) {
        return false;
    }
    return probe.nearestDepth > hzbMaxZ + static_cast<float>(kDepthBiasSteps) * kDepthStep;
}

// Probe の範囲を fetch(mip, x, y) で引いて判定する。戻り値 true = 可視 (描く)
template <typename Fetch>
bool IsVisible(const DirectX::XMFLOAT4X4& vp, const float bmin[3], const float bmax[3], int width,
               int height, int mipCount, Fetch&& fetch)
{
    const Probe probe = ProjectAabb(vp, bmin, bmax, width, height, mipCount);
    if (probe.conservativeVisible) {
        return true;
    }
    float maxZ = 0.0f;
    for (int y = probe.ty0; y <= probe.ty1; ++y) {
        for (int x = probe.tx0; x <= probe.tx1; ++x) {
            maxZ = std::max(maxZ, fetch(probe.mip, x, y));
        }
    }
    return !IsOccluded(probe, maxZ);
}

} // namespace mye::occlusion
