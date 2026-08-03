// M46b: RT の中間バッファをシーンの上に貼り付けるフルスクリーンパス。
// 内部解像度 (GI 等) の拡大にも使うので、描画先サイズを CB で受けて線形サンプルする。

cbuffer RtBlitCB : register(b0)
{
    float2 gBlitDstSize; // 描画先の解像度 (px)
    float2 gBlitPad;
};

Texture2D gSrc : register(t0);
SamplerState gBlitSamp : register(s0); // LINEAR / CLAMP

struct VSOut {
    float4 pos : SV_Position;
};

VSOut VSMain(uint vid : SV_VertexID)
{
    const float2 corners[3] = { float2(-1, -1), float2(-1, 3), float2(3, -1) };
    VSOut o;
    o.pos = float4(corners[vid], 0.0f, 1.0f);
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    const float2 uv = i.pos.xy / max(gBlitDstSize, float2(1.0f, 1.0f));
    return float4(gSrc.SampleLevel(gBlitSamp, uv, 0).rgb, 1.0f);
}
