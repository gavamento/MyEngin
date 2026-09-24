// WaterGerstner.surface.hlsl  M79 sub-05 サンプル: WaterWave.surfaceMaterial 用の水面サーフェス。
// 既定シーンからは参照しない。作者向けお手本 (WaterWave の波パラメータ・時計・色を
// 予約 CB MyEngineWater からそのまま読み、手書き cbuffer が要らないことを示す)。
//
// ★頂点変位には gTime ではなく gWaterTime を使うこと。gWaterTime は WaterWave.timeScale 込みの
//   水面時計で、速度エントリでは前後の値が自動で入れ替わる (影エントリは常に今フレーム)。
//   gTime を使うと WaterWave (浮力) と別の時計になり、TAA の速度・CSM の影が波と食い違う
#include "MyEngineSurface.hlsli"

struct VSIn
{
    float3 pos : POSITION;
};

struct VSOut
{
    float4 pos : SV_Position;
    float3 posW : TEXCOORD0;
    float3 normalW : TEXCOORD1;
};

// water_surface.hlsl (組込み WaterPass) の AccumulateGerstnerWave と同じ式。
// MyEngineWater の波は WaterWaveComponent.ExtractWaves と同じ並び (振幅・波長・速度・角度・急峻度)
void AccumulateWave(MyeGerstnerWave w, float overallScale, float time, float x, float z,
                    inout float3 disp, inout float3 nDiff)
{
    const float a = max(0.0f, w.amplitude) * overallScale;
    if (a <= 1e-6f) {
        return;
    }
    const float len = max(0.1f, w.wavelength);
    const float k = 6.28318530718f / len;
    const float omega = k * w.speed;
    const float rad = w.dirAngleDeg * (3.14159265359f / 180.0f);
    const float dirX = cos(rad);
    const float dirZ = sin(rad);
    const float qa = saturate(w.steepness) * a;

    const float phi = k * (dirX * x + dirZ * z) - omega * time;
    const float s = sin(phi);
    const float c = cos(phi);

    disp.x -= dirX * qa * s;
    disp.z -= dirZ * qa * s;
    disp.y += a * c;

    const float ka = k * a;
    nDiff.x -= dirX * ka * s;
    nDiff.z -= dirZ * ka * s;
    nDiff.y -= saturate(w.steepness) * ka * c;
}

VSOut VSMain(VSIn v)
{
    VSOut o;
    const float4 basePosW = mul(float4(v.pos, 1.0f), gWorld);

    float3 disp = float3(0.0f, 0.0f, 0.0f);
    float3 nDiff = float3(0.0f, 1.0f, 0.0f);
    for (int i = 0; i < gMyeWaterWaveCount; ++i) {
        AccumulateWave(gMyeWaterWaves[i], gMyeWaterOverallScale, gWaterTime, basePosW.x, basePosW.z,
                       disp, nDiff);
    }

    float3 posW = basePosW.xyz + disp;
    posW.y += gMyeWaterBaseHeight;

    o.pos = mul(float4(posW, 1.0f), gViewProj);
    o.posW = posW;
    o.normalW = normalize(nDiff);
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    const float3 n = normalize(i.normalW);
    const float shadow = MyeSunShadow(i.posW);
    const float ndotl = saturate(dot(n, -gSunDirection)) * shadow;
    const float4 waterColor = lerp(gMyeWaterDeepColor, gMyeWaterShallowColor, ndotl);
    const float3 lit = waterColor.rgb * (0.35f + 0.65f * ndotl);
    return float4(MyeApplyFog(lit, i.posW), waterColor.a);
}
