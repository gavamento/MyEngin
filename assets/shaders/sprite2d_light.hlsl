// Sprite2D のライト形状 (M94o): ライトバッファ (RGBA16F) へ加算で描く。Sprite2DPass が群ごとに呼ぶ。
// gSpotB.w = 形状 (1 Spot / 2 Freeform / 3 Sprite)。頂点はワールド XY、Z は gSpotC.w (ライトの位置)。
// Global は Pass がクリア色で足すのでここには来ない。
// 影 (t2) と法線 (t1) は gFx のフラグが立つときだけ掛ける (無いシーンは従来と同じ値)。

cbuffer LightCB : register(b0)
{
    float4x4 gViewProj; // transpose(view*proj)
    float4 gColor;      // rgb = 色 × intensity
    float4 gSpotA;      // center.xy (ライトの位置、全形状共通), dir.xy
    float4 gSpotB;      // inner, outer, falloff, shape
    float4 gSpotC;      // useAngle, cosOuter, invCosRange, z
    float4 gFx;         // x = 法線を掛ける, y = normalMapDistance, z = 影を掛ける, w = shadowStrength
    float4 gScreen;     // xy = 1 / ライトバッファの寸法 (SV_POSITION → UV)
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

Texture2D    gTex    : register(t0);
Texture2D    gNormal : register(t1); // 法線 RT (rgb = 法線 × 0.5 + 0.5)
Texture2D    gShadow : register(t2); // 影 RT (r = 影の濃さ)
SamplerState gSamp   : register(s0);

float4 PSMain(VSOut i) : SV_TARGET
{
    float3 rgb;
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
        rgb = gColor.rgb * (radial * angular);
    } else if (shape < 2.5) {
        rgb = gColor.rgb * i.edge;
    } else {
        const float4 t = gTex.Sample(gSamp, i.uv);
        rgb = gColor.rgb * t.rgb * t.a;
    }

    const float2 screenUv = i.pos.xy * gScreen.xy;
    if (gFx.z > 0.5) {
        // 影: 1 − shadow × strength
        rgb *= 1.0 - gShadow.SampleLevel(gSamp, screenUv, 0).r * gFx.w;
    }
    if (gFx.x > 0.5) {
        // 法線: N·L。L はライトの位置 (XY) からの向きに、平面からの高さ gFx.y を z として正規化する
        float3 n = gNormal.SampleLevel(gSamp, screenUv, 0).xyz * 2.0 - 1.0;
        const float nLen = length(n);
        n = (nLen > 1.0e-4) ? n / nLen : float3(0.0, 0.0, 1.0);
        const float3 l = normalize(float3(gSpotA.xy - i.world, gFx.y));
        rgb *= max(0.0, dot(n, l));
    }
    return float4(rgb, 1.0);
}
