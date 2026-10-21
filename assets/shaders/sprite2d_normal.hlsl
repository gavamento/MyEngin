// Sprite2D の法線 (M94p): 法線 RT (RGBA8、クリア = (0.5, 0.5, 1, 1)) へ、lit スプライトと同じ頂点列を描く。
// 頂点色 = 法線の 2x2 変換 (rgba = m11 m12 m21 m22: スプライトの回転と flip)。
// 色のテクスチャ (t1) のアルファでブレンドするので、透明な画素は奥の法線を残す。

cbuffer Sprite2DCB : register(b0)
{
    float4x4 gViewProj; // transpose(view*proj)
};

cbuffer NormalPsCB : register(b1)
{
    float gFlat; // 1 = 法線マップを持たない (t0 を読まず平らな法線 (0, 0, 1) で描く)
    float3 gPad;
};

struct VSIn
{
    float3 pos : POSITION;
    float2 uv : TEXCOORD;
    float4 color : COLOR;
};

struct VSOut
{
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 xform : COLOR;
};

VSOut VSMain(VSIn i)
{
    VSOut o;
    o.pos = mul(float4(i.pos, 1.0), gViewProj);
    o.uv = i.uv;
    o.xform = i.color;
    return o;
}

Texture2D    gNormalMap : register(t0);
Texture2D    gColorTex  : register(t1);
SamplerState gSamp      : register(s0);

float4 PSMain(VSOut i) : SV_TARGET
{
    const float alpha = gColorTex.Sample(gSamp, i.uv).a;
    float3 n = float3(0.0, 0.0, 1.0);
    if (gFlat < 0.5) {
        n = gNormalMap.Sample(gSamp, i.uv).xyz * 2.0 - 1.0;
        // タンジェント空間 (x 右、y 上、z 手前) の xy を、スプライトの向き / flip でワールドへ回す
        n.xy = float2(i.xform.x * n.x + i.xform.z * n.y, i.xform.y * n.x + i.xform.w * n.y);
    }
    return float4(n * 0.5 + 0.5, alpha);
}
