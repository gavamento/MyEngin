// スカイボックス (パノラマ / Equirectangular 2D テクスチャ)。
// skybox_cubemap.hlsl (cubemap) と同様のフルスクリーン三角形 +
// invViewProj の視線復元で、Texture2D を正距円筒図法でサンプルする。
// CB は b3 / テクスチャは t0 / サンプラは s0 (SkyboxPass が専用にバインドする)。

// M57e: フロクセルのサンプル座標 (register 宣言を持たないヘッダ)
#include "froxel_common.hlsli"

static const float SKY_PI = 3.14159265358979323846f;

cbuffer SkyCB : register(b3)
{
    float4x4 gInvViewProj; // transpose(inverse(view*proj))
    float4 gTopColor;      // テクスチャモードでは未使用 (レイアウト共有)
    float4 gHorizonColor;
    float4 gBottomColor;
    // ---- M57e: フロクセル (末尾 append。skybox.hlsl と同一レイアウト) ----
    float4 gSkyFroxel;       // x = enabled / y = スライス数 / zw = 未使用
    float4 gSkyFroxelScreen; // xy = レンダーターゲット実寸 (px) / zw = 未使用
    // ---- 2026-09-14: 手続きの星空 (cubemap と同様にテクスチャモードでは未使用) ----
    float4 gStars;
    float4 gStarsTime;
};

Texture2D gSky2D : register(t0);
SamplerState gSampler : register(s0);

// M57e: フロクセルは **t7 / s2** — skybox.hlsl の gFroxelVolume 宣言のコメントと同じ理由で、
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

    // 正距円筒図法 (Equirectangular) の UV 算出
    // 方位角 phi (-PI .. PI) -> u (0 .. 1)
    // 仰角 theta (-PI/2 .. PI/2) -> v (1 .. 0, 上が天頂)
    float2 uv;
    uv.x = 0.5f + atan2(dir.z, dir.x) / (2.0f * SKY_PI);
    uv.y = 0.5f - asin(clamp(dir.y, -1.0f, 1.0f)) / SKY_PI;

    float3 c = gSky2D.Sample(gSampler, uv).rgb;
    if (gSkyFroxel.x != 0.0f) {
        const float4 v = gFroxelVolume.SampleLevel(
            gFroxelSampler,
            float3(i.pos.xy / gSkyFroxelScreen.xy, FroxelSampleWFar(gSkyFroxel.y)), 0);
        c = c * v.a + v.rgb;
    }
    return float4(c, 1.0f);
}
