// 環境 BRDF LUT のベイク (M38c、split-sum の第 2 項)。
// uv.x = NdotV、uv.y = roughness → (scale, bias): spec = pre * (F0*scale + bias)。
// Geometry 項は IBL 用の k = a²/2 (直接光の (r+1)²/8 とは別 — Karis)。

#include "ibl_common.hlsli"

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut VSMain(uint vid : SV_VertexID)
{
    const float2 corners[3] = { float2(-1, -1), float2(-1, 3), float2(3, -1) };
    VSOut o;
    o.pos = float4(corners[vid], 0.0f, 1.0f);
    o.uv = corners[vid] * float2(0.5f, -0.5f) + 0.5f;
    return o;
}

float GeometrySchlickGGX_IBL(float ndv, float rough)
{
    const float a = rough * rough;
    const float k = (a * a) / 2.0f;
    return ndv / (ndv * (1.0f - k) + k);
}

float4 PSMain(VSOut i) : SV_Target
{
    const float ndv = max(i.uv.x, 1e-3f);
    const float rough = i.uv.y;
    const float3 V = float3(sqrt(1.0f - ndv * ndv), 0.0f, ndv);
    const float3 N = float3(0, 0, 1);
    const uint kSamples = 256u;
    float scale = 0.0f;
    float bias = 0.0f;
    for (uint s = 0u; s < kSamples; ++s) {
        const float2 xi = Hammersley(s, kSamples);
        const float3 H = ImportanceSampleGGX(xi, N, rough);
        const float3 L = normalize(2.0f * dot(V, H) * H - V);
        const float ndl = saturate(L.z);
        const float ndh = saturate(H.z);
        const float vdh = saturate(dot(V, H));
        if (ndl > 0.0f) {
            const float G = GeometrySchlickGGX_IBL(ndv, rough) * GeometrySchlickGGX_IBL(ndl, rough);
            const float gVis = (G * vdh) / max(ndh * ndv, 1e-4f);
            const float fc = pow(1.0f - vdh, 5.0f);
            scale += (1.0f - fc) * gVis;
            bias += fc * gVis;
        }
    }
    return float4(scale / float(kSamples), bias / float(kSamples), 0.0f, 1.0f);
}
