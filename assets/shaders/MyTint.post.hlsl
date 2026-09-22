// MyTint.post.hlsl  M78 サンプル: BeforeTonemap でシーンカラーを Tint × Intensity (sub-03 手動検証用)
/*@MyEngineProperties
[Range(0.0, 1.0)] _Intensity ("Intensity", Float) = 1.0
_Tint ("Tint", Color) = (1, 1, 1, 1)
@*/
#include "ProjectPostCommon.hlsli"

cbuffer MyEnginePerEffect : register(b1)
{
    float  _Intensity;
    float4 _Tint;
};

float4 PSMain(ProjectPostVSOut i) : SV_Target
{
    float4 c = SampleSceneColor(i.uv);
    return c * _Tint * _Intensity;
}
