// GrassField.surface.hlsl  M96 サンプル: かき乱しフィールドの流れで倒れる草 (MeshRenderer.material 用)。
// メッシュは「底面が根元」の単位ボックス (-0.5..0.5) を細長くスケールした 1 本 = 1 エンティティを想定する。
// 根元は動かず、高さに比例して流れの向きへ倒れる。場が無い / 流れが 0 のときは真っ直ぐ立つ
// (風の揺れ _Sway は時間の sin で、描画だけの動き。シミュレーションには影響しない)。
// 倒れの量は根元の位置で 1 回だけ場を読んで決める (1 本の中で曲がり方が割れない)。
/*@MyEngineProperties
_BaseColor ("Root Color", Color) = (0.10, 0.28, 0.07, 1)
_TipColor ("Tip Color", Color) = (0.45, 0.70, 0.20, 1)
_BendPerSpeed ("Bend per m/s [rad]", Float) = 0.5
_MaxBend ("Max Bend [rad]", Float) = 1.2
_Sway ("Wind Sway", Float) = 0.04
@*/
#include "MyEngineSurface.hlsli"

cbuffer MyEnginePerMaterial
{
    float4 _BaseColor;
    float4 _TipColor;
    float _BendPerSpeed;
    float _MaxBend;
    float _Sway;
};

struct VSIn
{
    float3 pos : POSITION;
    float3 normal : NORMAL;
};

struct VSOut
{
    float4 pos : SV_Position;
    float3 posW : TEXCOORD0;
    float3 normalW : TEXCOORD1;
    float height01 : TEXCOORD2;
};

VSOut VSMain(VSIn v)
{
    VSOut o;
    const float4 basePosW = mul(float4(v.pos, 1.0f), gWorld);
    // 根元 (ボックスの底面の中心) と 1 本の高さ [m]、頂点の根元からの高さの割合
    const float3 rootW = mul(float4(0.0f, -0.5f, 0.0f, 1.0f), gWorld).xyz;
    const float bladeHeight = length(gWorld[1].xyz);
    const float h01 = saturate(v.pos.y + 0.5f);
    const float h = h01 * bladeHeight;

    // 流れの向きへ根元を軸に回す。長さを保つ (横へ寄った分だけ高さが縮む)
    const float2 flow = MyeFieldSample(rootW).xy;
    const float speed = length(flow);
    const float bend = min(speed * _BendPerSpeed, _MaxBend);
    const float2 dir = speed > 1e-4f ? flow / speed : float2(0.0f, 0.0f);
    float3 posW = basePosW.xyz;
    posW.xz += dir * (sin(bend) * h);
    posW.y -= (1.0f - cos(bend)) * h;

    // 風の揺れ。根元付近は動かさない (h01 の 2 乗)
    const float phase = gTime * 2.1f + rootW.x * 1.7f + rootW.z * 1.3f;
    posW.x += sin(phase) * _Sway * h01 * h01 * bladeHeight;
    posW.z += cos(phase * 0.8f) * _Sway * 0.5f * h01 * h01 * bladeHeight;

    o.pos = mul(float4(posW, 1.0f), gViewProj);
    o.posW = posW;
    o.normalW = normalize(mul(v.normal, (float3x3)gWorld));
    o.height01 = h01;
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    const float3 n = normalize(i.normalW);
    const float ndotl = saturate(dot(n, -gSunDirection)) * MyeSunShadow(i.posW);
    const float3 albedo = lerp(_BaseColor.rgb, _TipColor.rgb, i.height01);
    const float3 lit = albedo * (gAmbient + gSunColor * (0.15f + 0.85f * ndotl));
    return float4(MyeApplyFog(lit, i.posW), 1.0f);
}
