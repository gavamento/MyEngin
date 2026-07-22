// GGX プリフィルタ環境マップのベイク (M38c、split-sum の第 1 項)。
// 各 mip = roughness 段。N=V=R (Karis 近似) で GGX 重点サンプリング。

#include "ibl_common.hlsli"

BakeVSOut VSMain(uint vid : SV_VertexID)
{
    return BakeVS(vid);
}

float4 PSMain(BakeVSOut i) : SV_Target
{
    const float3 N = BakeDir(i.ndc);
    if (gRoughness <= 0.001f) {
        return float4(SampleEnv(N), 1.0f); // mip0 = 鏡面そのまま
    }
    const float3 R = N;
    const float3 V = N;
    const uint kSamples = 64u;
    float3 sum = float3(0, 0, 0);
    float weight = 0.0f;
    for (uint s = 0u; s < kSamples; ++s) {
        const float2 xi = Hammersley(s, kSamples);
        const float3 H = ImportanceSampleGGX(xi, N, gRoughness);
        const float3 L = normalize(2.0f * dot(V, H) * H - V);
        const float ndl = saturate(dot(N, L));
        if (ndl > 0.0f) {
            sum += SampleEnv(L) * ndl;
            weight += ndl;
        }
    }
    return float4(sum / max(weight, 1e-4f), 1.0f);
}
