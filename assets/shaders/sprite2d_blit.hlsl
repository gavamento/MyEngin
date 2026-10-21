// M94v: ピクセルパーフェクトカメラの低解像度 RT を、整数倍にして最終ターゲットへ貼るフルスクリーンパス。
// ビューポートを拡大後の矩形に合わせて描くので、uv は頂点から作る (CB なし)。
// 点サンプルなので RT の 1 ピクセルが zoom x zoom の同色ブロックになり、ぼけない。

Texture2D gSrc : register(t0);
SamplerState gPoint : register(s0); // POINT / CLAMP

struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut VSMain(uint vid : SV_VertexID)
{
    const float2 corners[3] = { float2(-1, -1), float2(-1, 3), float2(3, -1) };
    VSOut o;
    o.pos = float4(corners[vid], 0.0f, 1.0f);
    o.uv = float2(corners[vid].x * 0.5f + 0.5f, 0.5f - corners[vid].y * 0.5f);
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    return float4(gSrc.SampleLevel(gPoint, i.uv, 0).rgb, 1.0f);
}
