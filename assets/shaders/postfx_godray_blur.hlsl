// M43b: ゴッドレイの放射ブラーパス — 各ピクセルから太陽のスクリーン位置へ向けて
// 16 タップを decay^i 重みで積分する。2 パス実行 (密度 0.5 → 1.0) で実効 256 タップ相当。
// CB は PostProcess.cpp の GodrayBlurCB と同一レイアウト。

cbuffer GodrayBlur : register(b0)
{
    float2 gSunUV;   // 太陽のスクリーン UV
    float  gDecay;   // タップ毎減衰 (0..1)
    float  gDensity; // 16 タップで太陽までの距離の何割を進むか
};

Texture2D gSrc : register(t0);
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
    const float2 step = (gSunUV - i.uv) * gDensity / 16.0f;
    float2 uv = i.uv;
    float w = 1.0f;
    float wsum = 0.0f;
    float3 acc = float3(0.0f, 0.0f, 0.0f);
    [unroll]
    for (int k = 0; k < 16; ++k) {
        acc += gSrc.Sample(gLinear, uv).rgb * w;
        wsum += w;
        uv += step;
        w *= gDecay;
    }
    return float4(acc / wsum, 1.0f);
}
