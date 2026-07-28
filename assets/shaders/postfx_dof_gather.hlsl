// M44c: DoF ギャザーパス — 半解像度のプリフィルタ結果 (rgb=色, a=符号付き CoC) を
// 24 タップ円盤 (8+16 の 2 リング) で集める。scatter-as-gather: サンプル自身の
// ボケ半径がタップ距離まで届く場合のみ寄与させる (シャープな前景が背景ボケに
// 滲むのを抑える)。α は中心の CoC を素通し。
// CB は PostProcess.cpp の DofCB と同一レイアウト。

cbuffer DofCB : register(b0)
{
    float gFocusDist;
    float gFocusRange;
    float gNearZ;
    float gFarZ;
    float gMaxRadius; // 最大ボケ半径 (フル解像度 px)
    float gTexelX;    // 半解像度の 1/幅
    float gTexelY;    // 半解像度の 1/高さ
    float _dofPad;
};

Texture2D gSrc : register(t0); // dofA (プリフィルタ済み)
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

// 円盤カーネル 24 タップ (リング1 = 半径 0.4 x8、リング2 = 半径 1.0 x16)
static const float2 kDisc[24] = {
    float2( 0.400f,  0.000f), float2( 0.283f,  0.283f), float2( 0.000f,  0.400f),
    float2(-0.283f,  0.283f), float2(-0.400f,  0.000f), float2(-0.283f, -0.283f),
    float2( 0.000f, -0.400f), float2( 0.283f, -0.283f),
    float2( 1.000f,  0.000f), float2( 0.924f,  0.383f), float2( 0.707f,  0.707f),
    float2( 0.383f,  0.924f), float2( 0.000f,  1.000f), float2(-0.383f,  0.924f),
    float2(-0.707f,  0.707f), float2(-0.924f,  0.383f), float2(-1.000f,  0.000f),
    float2(-0.924f, -0.383f), float2(-0.707f, -0.707f), float2(-0.383f, -0.924f),
    float2( 0.000f, -1.000f), float2( 0.383f, -0.924f), float2( 0.707f, -0.707f),
    float2( 0.924f, -0.383f)
};

float4 PSMain(VSOut i) : SV_Target
{
    const float4 center = gSrc.Sample(gLinear, i.uv);
    const float maxRadiusHalf = gMaxRadius * 0.5f; // 半解像度 px 換算
    const float radiusPx = abs(center.a) * maxRadiusHalf;
    if (radiusPx < 0.5f) {
        return center; // ボケ半径がピクセル未満 — そのまま
    }
    float3 acc = center.rgb;
    float wsum = 1.0f;
    [unroll]
    for (int k = 0; k < 24; ++k) {
        const float tapDist = radiusPx * length(kDisc[k]);
        const float2 uvS = i.uv + kDisc[k] * radiusPx * float2(gTexelX, gTexelY);
        const float4 s = gSrc.Sample(gLinear, uvS);
        // サンプルのボケ円がこのタップ距離まで届くか (1px 幅でフェード)
        const float w = saturate(abs(s.a) * maxRadiusHalf - tapDist + 1.0f);
        acc += s.rgb * w;
        wsum += w;
    }
    return float4(acc / wsum, center.a);
}
