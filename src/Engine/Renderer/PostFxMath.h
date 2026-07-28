#pragma once
#include <algorithm>
#include <cmath>

#include <DirectXMath.h>

// ポストプロセス/大気系シェーダの数式を C++ に複製した純関数群 (D3D 非依存)。
// HLSL 側 (common.hlsli / postfx_*.hlsl) とコメント同期で複製し、selftest がこちらを検証する。
// 描画専用 (sim/hash 非関与)。
namespace mye {

// M43a: ハイトフォグ。高度で exp 減衰する密度 ρ(y)=e^{-k(y-base)} の視線積分を
// 「実効距離」に畳む: ∫ρ = e^{-k(camY-base)} · (1-e^{-kΔy})/(kΔy) · dist (Δy=posY-camY)。
// common.hlsli::ApplyFog と同一式 — 変更時は両方更新。
// heightFalloff<=0 は dist をそのまま返す (従来とビット同一)。指数は overflow 回避で ±60 clamp
inline float HeightFogEffectiveDistance(float dist, float camY, float posY, float heightFalloff,
                                        float baseHeight)
{
    if (heightFalloff <= 0.0f) {
        return dist;
    }
    const float kd = heightFalloff * (posY - camY);
    // 分母は ±1e-4 でクランプ — C4723 対策 (定数畳み込みが 0 除算を検出して警告する)。
    // |kd|<=1e-4 は分岐で 1 を返すため結果は HLSL 側の素の除算と同じ
    const float safeKd = (kd >= 0.0f) ? (std::max)(kd, 1e-4f) : (std::min)(kd, -1e-4f);
    const float slope = (std::abs(kd) > 1e-4f) ? (1.0f - std::exp(-safeKd)) / safeKd : 1.0f;
    return dist * std::exp(std::clamp(-heightFalloff * (camY - baseHeight), -60.0f, 60.0f))
           * slope;
}

// M43a: 太陽インスキャッタ係数 [0,1]。視線 rayDir が太陽 (光の進行方向 sunDir の逆) へ
// 向くほど 1 に近づく。common.hlsli::ApplyFog の sunAmount と同一式
inline float SunInscatterFactor(const DirectX::XMFLOAT3& rayDir, const DirectX::XMFLOAT3& sunDir,
                                float power)
{
    const float d = -(rayDir.x * sunDir.x + rayDir.y * sunDir.y + rayDir.z * sunDir.z);
    return std::pow(std::clamp(d, 0.0f, 1.0f), std::max(power, 1e-2f));
}

} // namespace mye
