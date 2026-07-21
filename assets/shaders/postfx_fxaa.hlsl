// FXAA (M16): トーンマップ済み LDR に対する輝度ベースのエッジアンチエイリアス。
// NVIDIA の簡易 FXAA (4 コーナー + 中心のルマ勾配でエッジ方向を推定し、方向に沿って平均)。

cbuffer Fxaa : register(b0)
{
    float2 gInvSize; // (1/width, 1/height)
    float2 gPad;
};

Texture2D gTex : register(t0);
SamplerState gLinear : register(s0);

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

float4 PSMain(VSOut i) : SV_Target
{
    const float3 LUMA = float3(0.299f, 0.587f, 0.114f);
    const float SPAN_MAX = 8.0f;
    const float REDUCE_MUL = 1.0f / 8.0f;
    const float REDUCE_MIN = 1.0f / 128.0f;

    const float2 uv = i.uv;
    const float3 rgbNW = gTex.Sample(gLinear, uv + float2(-1, -1) * gInvSize).rgb;
    const float3 rgbNE = gTex.Sample(gLinear, uv + float2(1, -1) * gInvSize).rgb;
    const float3 rgbSW = gTex.Sample(gLinear, uv + float2(-1, 1) * gInvSize).rgb;
    const float3 rgbSE = gTex.Sample(gLinear, uv + float2(1, 1) * gInvSize).rgb;
    const float3 rgbM = gTex.Sample(gLinear, uv).rgb;

    const float lNW = dot(rgbNW, LUMA);
    const float lNE = dot(rgbNE, LUMA);
    const float lSW = dot(rgbSW, LUMA);
    const float lSE = dot(rgbSE, LUMA);
    const float lM = dot(rgbM, LUMA);
    const float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
    const float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));

    float2 dir;
    dir.x = -((lNW + lNE) - (lSW + lSE));
    dir.y = ((lNW + lSW) - (lNE + lSE));

    const float dirReduce = max((lNW + lNE + lSW + lSE) * 0.25f * REDUCE_MUL, REDUCE_MIN);
    const float rcpDirMin = 1.0f / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * rcpDirMin, -SPAN_MAX.xx, SPAN_MAX.xx) * gInvSize;

    const float3 rgbA = 0.5f
        * (gTex.Sample(gLinear, uv + dir * (1.0f / 3.0f - 0.5f)).rgb
           + gTex.Sample(gLinear, uv + dir * (2.0f / 3.0f - 0.5f)).rgb);
    const float3 rgbB = rgbA * 0.5f
        + 0.25f
            * (gTex.Sample(gLinear, uv + dir * -0.5f).rgb
               + gTex.Sample(gLinear, uv + dir * 0.5f).rgb);
    const float lB = dot(rgbB, LUMA);

    const float3 result = (lB < lMin || lB > lMax) ? rgbA : rgbB;
    return float4(result, 1.0f);
}
