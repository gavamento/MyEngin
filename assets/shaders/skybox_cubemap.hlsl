// スカイボックス (M38b、cubemap)。skybox.hlsl (gradient) と同じフルスクリーン三角形 +
// invViewProj の視線復元で、TextureCube をワールド方向でサンプルする。
// CB は b3 / テクスチャは t0 / サンプラは s0 (SkyboxPass が専用にバインドする)。

// M57e: フロクセルのサンプル座標 (register 宣言を持たないヘッダ)
#include "froxel_common.hlsli"

cbuffer SkyCB : register(b3)
{
    float4x4 gInvViewProj; // transpose(inverse(view*proj))
    float4 gTopColor;      // cubemap モードでは未使用 (レイアウト共有)
    float4 gHorizonColor;
    float4 gBottomColor;
    // ---- M57e: フロクセル (末尾 append。skybox.hlsl と同一レイアウト) ----
    float4 gSkyFroxel;       // x = enabled / y = スライス数 / zw = 未使用
    float4 gSkyFroxelScreen; // xy = レンダーターゲット実寸 (px) / zw = 未使用
};

TextureCube gSky : register(t0);
SamplerState gSampler : register(s0);
// M57e: フロクセルは **t7 / s2** — skybox.hlsl の頭のコメントと同じ理由で、
// ホストのパスが既に張っているスロットをそのまま読む (自分では張らない)
Texture3D gFroxelVolume : register(t7);
SamplerState gFroxelSampler : register(s2);

struct VSOut
{
    float4 pos : SV_Position;
    float2 ndc : TEXCOORD0;
};

VSOut VSMain(uint vid : SV_VertexID)
{
    const float2 corners[3] = { float2(-1, -1), float2(-1, 3), float2(3, -1) };
    VSOut o;
    o.pos = float4(corners[vid], 1.0f, 1.0f);
    o.ndc = corners[vid];
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    float4 pf = mul(float4(i.ndc, 1.0f, 1.0f), gInvViewProj);
    float4 pn = mul(float4(i.ndc, 0.0f, 1.0f), gInvViewProj);
    const float3 dir = normalize(pf.xyz / pf.w - pn.xyz / pn.w);
    float3 c = gSky.Sample(gSampler, dir).rgb;
    if (gSkyFroxel.x != 0.0f) {
        const float4 v = gFroxelVolume.SampleLevel(
            gFroxelSampler,
            float3(i.pos.xy / gSkyFroxelScreen.xy, FroxelSampleWFar(gSkyFroxel.y)), 0);
        c = c * v.a + v.rgb;
    }
    return float4(c, 1.0f);
}
