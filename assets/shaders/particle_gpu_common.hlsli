// GPU パーティクルの共通定義 (emit / sim / render で共有)

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
};
