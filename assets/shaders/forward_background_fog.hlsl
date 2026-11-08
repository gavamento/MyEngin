// Forward の背景 (スカイ無し) へ地面の霧とフロクセルを掛ける全画面パス。
// 式は deferred_light.hlsl の背景分岐 (albedo.a < 0.5) と同じ。順序も同じ (地面の霧 → フロクセル)。
// 頂点は z=1 (far) なので、深度テスト LESS_EQUAL で不透明が何も描かなかった画素にだけ乗る。
// 解析フォグは掛けない (背景は ApplyFog を受けない仕様)。
// t7 / s2 はスカイボックスと同じく、ホストのパスが張ったものをそのまま読む。

#include "froxel_common.hlsli"
#include "ground_fog.hlsli" // 地面の霧 (b6 / b7 / b8 / t32 / t33 / s8 を宣言する)

cbuffer BackgroundFogCB : register(b3)
{
    float4 gBgClear;       // rgb = clear 色 (リニア), a = clear のアルファ
    float4 gBgFroxel;      // x = 有効 / y = スライス数 / zw = 未使用
    float4 gBgFroxelScreen; // xy = レンダーターゲット実寸 (px) / zw = 未使用
};

Texture3D    gFroxelVolume  : register(t7);
SamplerState gFroxelSampler : register(s2); // LINEAR/CLAMP (ホストがフレーム頭で張る)

struct VSOut
{
    float4 pos : SV_Position;
};

VSOut VSMain(uint vid : SV_VertexID)
{
    const float2 corners[3] = { float2(-1, -1), float2(-1, 3), float2(3, -1) };
    VSOut o;
    o.pos = float4(corners[vid], 1.0f, 1.0f);
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    const float3 bgColor = GroundFogCompositeBackground(gBgClear.rgb, i.pos.xy, gBgFroxelScreen.xy);
    if (gBgFroxel.x != 0.0f) {
        const float4 vol = gFroxelVolume.SampleLevel(
            gFroxelSampler, float3(i.pos.xy / gBgFroxelScreen.xy, FroxelSampleWFar(gBgFroxel.y)), 0);
        return float4(bgColor * vol.a + vol.rgb, gBgClear.a);
    }
    return float4(bgColor, gBgClear.a);
}
