// ToonFlat.surface.hlsl  M79 サンプル: 平塗り (トゥーン) ライティング (MyTint.post.hlsl と同じ位置付け。
// 既定シーンからは参照しない。手動検証・作成メニューのお手本用)
/*@MyEngineProperties
_BaseColor ("Base Color", Color) = (1, 1, 1, 1)
_ShadowColor ("Shadow Color", Color) = (0.35, 0.35, 0.45, 1)
[Range(1, 8)] _Steps ("Toon Steps", Float) = 3
_MainTex ("Main Tex", 2D) = "white" {}
@*/
#include "MyEngineSurface.hlsli"

cbuffer MyEnginePerMaterial
{
    float4 _BaseColor;
    float4 _ShadowColor;
    float _Steps;
};

Texture2D _MainTex;

struct VSIn
{
    float3 pos : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

struct VSOut
{
    float4 pos : SV_Position;
    float3 normalW : NORMAL;
    float3 posW : TEXCOORD0;
    float2 uv : TEXCOORD1;
};

VSOut VSMain(VSIn v)
{
    VSOut o;
    const float4 worldPos = mul(float4(v.pos, 1.0f), gWorld);
    o.pos = mul(worldPos, gViewProj);
    o.posW = worldPos.xyz;
    o.normalW = normalize(mul(v.normal, (float3x3)gWorld));
    o.uv = v.uv;
    return o;
}

// 平塗りトゥーン: N・L と CSM の影を _Steps 段に量子化し、影/ハイライト 2 色を補間する
float4 PSMain(VSOut i) : SV_Target
{
    const float3 n = normalize(i.normalW);
    const float ndotl = saturate(dot(n, -gSunDirection));
    const float shadow = MyeSunShadow(i.posW);
    const float steps = max(_Steps, 1.0f);
    const float toon = floor(ndotl * shadow * steps) / steps;
    const float4 tex = _MainTex.Sample(gSampler, i.uv);
    const float3 lit = lerp(_ShadowColor.rgb, _BaseColor.rgb, toon) * tex.rgb;
    return float4(MyeApplyFog(lit, i.posW), _BaseColor.a * tex.a);
}
