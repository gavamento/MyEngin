// project_post_blit.hlsl  AfterTonemap LDR コピー用パススルー PS (M78d)
// AfterTonemap CS はあるがポスト 0 件のとき、t.userPostLdr → dst への blit に使う。
// VSMain / バインド規約は ProjectPostCommon.hlsli で提供。
#include "ProjectPostCommon.hlsli"

float4 PSMain(ProjectPostVSOut i) : SV_Target
{
    // gSceneColor (t0) = t.userPostLdr.SRV() をそのままコピー
    return gSceneColor.SampleLevel(gLinearClamp, i.uv, 0);
}
