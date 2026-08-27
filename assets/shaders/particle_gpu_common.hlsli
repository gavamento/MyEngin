// GPU パーティクルの共通定義 (emit / sim / render で共有)

// M55a: LinearizeDepth を共有するために include する。common.hlsli は register 宣言を
// 一切持たない純関数/構造体だけの束なので、CS (emit/sim) へ持ち込んでも衝突しない。
// ★懸念だった「PerturbNormal の ddx/ddy が cs_5_0 で落ちる」は起きない —
//   D3DCompile は未使用関数を検証前に落とすため (fxc /T cs_5_0 /Ges /O3 で実測。
//   deferred_gbuffer.hlsl の VSMain が同じ理由で通っているのが既存の傍証)。
#include "common.hlsli"

struct GpuParticle
{
    float3 pos;
    float  life;     // 残り秒
    float3 vel;
    float  invLife;  // 1/寿命
    float  size0;
    float3 _pad;
};

cbuffer GpuParticleCB : register(b0)
{
    float4 gGravityWind;   // xyz = gravity+wind, w = dt
    float4 gParams;        // x = emitCount, y = turbulence, z = sizeEndScale, w = capacity
    float4 gColorBegin;
    float4 gColorEnd;
    float4 gParams2;       // M42b: x = softFade (0=off), y = nearZ, z = farZ / w = 予約
    float4 gParams3;       // M42c: x = useTexture, y = flipTilesX, z = flipTilesY, w = flipCycles
    // ---- M42e: 深度バッファ衝突 (GPU バックエンド限定の見た目効果。sim CS のみ参照) ----
    float4x4 gCollViewProj;    // transpose 済み (mul(float4, M) 規約は gViewProj と同じ)
    float4x4 gCollInvViewProj; // transpose 済み
    float4 gCollParams;        // x = enabled, y = restitution, z = 追加 thickness, w = 予約
    float4 gCollScreen;        // xy = 画面サイズ (px), z = nearZ, w = farZ
    // ---- M61d: カールノイズ乱流 (sim CS のみ参照。C++ 側 GpuParticleBackend.cpp の
    //      GpuParticleCB::params4 と一致 — 末尾 append なので既存フィールドのオフセット不変) ----
    float4 gParams4;           // x = turbulenceMode, y = noiseFrequency, z = noiseSpeed,
                               // w = noiseTime (ageTicks * dt — 実時間ではない)
};
