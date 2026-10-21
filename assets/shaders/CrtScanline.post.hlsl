// CrtScanline.post.hlsl  M94 サンプル: 2D 向けの CRT 風ポスト (走査線 + わずかな色収差)。AfterTonemap で使う
/*@MyEngineProperties
[Range(0.0, 1.0)] _Scanline ("Scanline Strength", Float) = 0.35
[Range(0.0, 8.0)] _Aberration ("Aberration (px)", Float) = 1.5
_ScanlinePitch ("Scanline Pitch (px)", Float) = 3
@*/
#include "ProjectPostCommon.hlsli"

cbuffer MyEnginePerEffect : register(b1)
{
    float _Scanline;
    float _Aberration;
    float _ScanlinePitch;
};

float4 PSMain(ProjectPostVSOut i) : SV_Target
{
    // 色収差: 赤と青を水平方向へ逆向きにずらす
    const float shift = _Aberration * gInvScreenW;
    const float r = SampleSceneColor(i.uv + float2(shift, 0.0f)).r;
    const float2 ga = SampleSceneColor(i.uv).ga;
    const float b = SampleSceneColor(i.uv - float2(shift, 0.0f)).b;

    // 走査線: pitch 画素ごとに 1 本、行の位置に応じて明るさを落とす
    const float row = i.uv.y * gScreenH / max(_ScanlinePitch, 1.0f);
    const float wave = 0.5f + 0.5f * cos(row * 6.2831853f);
    const float dim = 1.0f - _Scanline * wave;
    return float4(float3(r, ga.x, b) * dim, ga.y);
}
