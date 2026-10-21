// Sprite2D のライト形状 (M94o): ライトバッファ (RGBA16F) へ加算で描く。Sprite2DPass が群ごとに呼ぶ。
// gSpotB.w = 形状 (1 Spot / 2 Freeform / 3 Sprite)。頂点はワールド XY、Z は gSpotC.w (ライトの位置)。
// Global は Pass がクリア色で足すのでここには来ない。

cbuffer LightCB : register(b0)
{
    float4x4 gViewProj; // transpose(view*proj)
    float4 gColor;      // rgb = 色 × intensity
    float4 gSpotA;      // center.xy, dir.xy
    float4 gSpotB;      // inner, outer, falloff, shape
    float4 gSpotC;      // useAngle, cosOuter, invCosRange, z
};

struct VSIn
{
    float2 pos : POSITION; // ワールド XY
    float2 uv : TEXCOORD0;
    float edge : TEXCOORD1; // Freeform: 1 = 内側、0 = 外縁
};

struct VSOut
{
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
    float edge : TEXCOORD1;
    float2 world : TEXCOORD2;
};

VSOut VSMain(VSIn i)
{
    VSOut o;
    o.pos = mul(float4(i.pos, gSpotC.w, 1.0), gViewProj);
    o.uv = i.uv;
    o.edge = i.edge;
    o.world = i.pos;
    return o;
}

Texture2D    gTex  : register(t0);
SamplerState gSamp : register(s0);

float4 PSMain(VSOut i) : SV_TARGET
{
    const float shape = gSpotB.w;
    if (shape < 1.5) {
        // Spot: 半径方向 (inner..outer、falloff 乗) × 角度方向 (inner..outer の cos)
        const float2 d = i.world - gSpotA.xy;
        const float dist = length(d);
        const float inner = gSpotB.x;
        const float outer = gSpotB.y;
        float radial = (outer > inner) ? saturate((outer - dist) / (outer - inner)) : (dist <= outer ? 1.0 : 0.0);
        radial = (radial > 0.0) ? pow(radial, gSpotB.z) : 0.0;
        float angular = 1.0;
        if (gSpotC.x > 0.5) {
            const float c = (dist > 1.0e-6) ? dot(d / dist, gSpotA.zw) : 1.0;
            angular = saturate((c - gSpotC.y) * gSpotC.z);
        }
        return float4(gColor.rgb * (radial * angular), 1.0);
    }
    if (shape < 2.5) {
        return float4(gColor.rgb * i.edge, 1.0);
    }
    const float4 t = gTex.Sample(gSamp, i.uv);
    return float4(gColor.rgb * t.rgb * t.a, 1.0);
}
