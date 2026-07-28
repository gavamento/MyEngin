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

// M44a: 256x16 ストリップ LUT のサンプル UV。blue でスライス 2 枚 (u0/u1) と補間係数 frac を
// 選び、呼び出し側が 2 サンプルを lerp する = 実質トリリニア。
// postfx_tonemap.hlsl::SampleLutStrip とコメント同期 — 変更時は両方更新
inline void LutStripUv(float r, float g, float b, float& u0, float& u1, float& v, float& frac)
{
    const float bb = std::clamp(b, 0.0f, 1.0f) * 15.0f;
    const float slice0 = std::floor(bb);
    frac = bb - slice0;
    const float slice1 = (std::min)(slice0 + 1.0f, 15.0f);
    const float rr = std::clamp(r, 0.0f, 1.0f);
    u0 = (slice0 * 16.0f + rr * 15.0f + 0.5f) / 256.0f;
    u1 = (slice1 * 16.0f + rr * 15.0f + 0.5f) / 256.0f;
    v = (std::clamp(g, 0.0f, 1.0f) * 15.0f + 0.5f) / 16.0f;
}

// M43b: 太陽のスクリーン位置。sunDir (光の進行方向) の逆 = 太陽方向を方向ベクトル (w=0)
// として view*proj で射影し、UV [0,1] とフェード係数 [0,1] を返す。
// 背面 (clip.w<=0) は 0。画面内 = 1、画面外は UV が [0,1] を出た距離 0.25 で線形減衰
// (ゴッドレイのソースが視界から離れるほど自然に消える)。postfx_godray_*.hlsl の CB 供給元
inline float ComputeSunScreenPos(const DirectX::XMFLOAT4X4& view,
                                 const DirectX::XMFLOAT4X4& proj,
                                 const DirectX::XMFLOAT3& sunDir, float& outU, float& outV)
{
    using namespace DirectX;
    const XMMATRIX vp = XMLoadFloat4x4(&view) * XMLoadFloat4x4(&proj);
    const XMVECTOR clip =
        XMVector4Transform(XMVectorSet(-sunDir.x, -sunDir.y, -sunDir.z, 0.0f), vp);
    const float w = XMVectorGetW(clip);
    outU = 0.5f;
    outV = 0.5f;
    if (w <= 1e-4f) {
        return 0.0f; // 太陽が背面
    }
    outU = 0.5f + 0.5f * (XMVectorGetX(clip) / w);
    outV = 0.5f - 0.5f * (XMVectorGetY(clip) / w);
    const float ox = (std::max)({ 0.0f, -outU, outU - 1.0f });
    const float oy = (std::max)({ 0.0f, -outV, outV - 1.0f });
    return std::clamp(1.0f - (std::max)(ox, oy) / 0.25f, 0.0f, 1.0f);
}

} // namespace mye
