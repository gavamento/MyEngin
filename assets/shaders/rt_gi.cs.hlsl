// M46c: 拡散 GI (1spp)。G-Buffer の可視点からコサイン重点サンプリングでレイを 1 本撃ち、
// 入射放射輝度をそのまま出力する (albedo を掛けない demodulated 形式)。
// この次元は IBL の irradiance (平均入射色) と揃えてあるので、合成時に
// gi * albedo * kD とすれば IBL 拡散項と置き換えても明るさの段差が出ない。

#include "rt_common.hlsli"

cbuffer RtGiCB : register(b2)
{
    float2 gGiOutSize; // GI バッファの解像度 (内部解像度)
    float2 gGiGbSize;  // G-Buffer の解像度 (フル)
    float gGiTMax;
    uint gGiFrameIndex; // フレーム毎に乱数列をずらす (テンポラル蓄積で平均される)
    int gGiBounces;
    int gGiPad;
};

Texture2D gGiNormal : register(t7);   // GBuffer 法線 (*0.5+0.5 のワールド法線)
Texture2D gGiPosition : register(t8); // GBuffer ワールド座標
Texture2D gGiMark : register(t9);     // GBuffer アルベド (a = ジオメトリ有りマーク)

RWTexture2D<float4> gGiOut : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= (uint)gGiOutSize.x || tid.y >= (uint)gGiOutSize.y) {
        return;
    }
    // 内部解像度のピクセル中心を G-Buffer の座標へ写す
    const float2 uv = (float2(tid.xy) + 0.5f) / gGiOutSize;
    const int3 gp = int3(int2(uv * gGiGbSize), 0);
    if (gGiMark.Load(gp).a < 0.5f) {
        gGiOut[tid.xy] = float4(0.0f, 0.0f, 0.0f, 0.0f); // ジオメトリ無し (空)
        return;
    }
    const float3 N = normalize(gGiNormal.Load(gp).xyz * 2.0f - 1.0f);
    const float3 P = gGiPosition.Load(gp).xyz;

    uint3 seed = uint3(tid.x, tid.y, gGiFrameIndex * 16u);
    const float3 wi = RtCosineHemisphere(N, RtNextRand2(seed));
    const float3 radiance =
        RtTraceRadiance(P + N * gRtRayEps, wi, gGiTMax, max(gGiBounces, 1), seed);
    gGiOut[tid.xy] = float4(radiance, 1.0f);
}
