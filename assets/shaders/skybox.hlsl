// スカイボックス (M29d、gradient)。フルスクリーン三角形を z=1 (far) で描き、
// 深度 LESS_EQUAL でジオメトリの無いピクセルだけを塗る。
// 視線方向は invViewProj で NDC の near/far 2 点を逆射影して求める。
// CB は b3 (b0-b2 はメッシュ描画の PerFrame/PerObject/Material が使用中)。

// M57e: フロクセルのサンプル座標 (register 宣言を持たないヘッダ)
#include "froxel_common.hlsli"

cbuffer SkyCB : register(b3)
{
    float4x4 gInvViewProj; // transpose(inverse(view*proj))
    float4 gTopColor;
    float4 gHorizonColor;
    float4 gBottomColor;
    // ---- M57e: フロクセル (末尾 append。x=0 = 従来と 1 ビットも変わらない) ----
    // 空は深度を持たないので「グリッド全体ぶん」を引く (FroxelSampleWFar)。
    // ★グリッドより奥の解析フォグは掛けない — 空に ApplyFog が掛かる挙動は M29d 以来
    //   一度も無く、足すと濃霧のとき空が丸ごとフォグ色に潰れる。フロクセル区間ぶんの
    //   段 (= 地表と空の食い違い) だけを消すのが M57e の受け持ち
    float4 gSkyFroxel;       // x = enabled / y = スライス数 / zw = 未使用
    float4 gSkyFroxelScreen; // xy = レンダーターゲット実寸 (px) / zw = 未使用
};

// ★t7 / s2 は **SkyboxPath 自身が張らない** — ホストのパス (ForwardPath / DeferredPath) が
//   フレーム内で既に張っているものをそのまま読む。ここで別のスロットへ張ると、
//   Forward ではスカイの直後に描く半透明メッシュの t1 (CSM) / s0 (異方性 WRAP) を
//   潰してしまう (スカイは不透明と透明の間に入るパスなので、触った SRV が後段へ漏れる)。
//   t7 = フロクセル積分結果 (統合契約 予約 2) / s2 = IBL 用 LINEAR/CLAMP
Texture3D    gFroxelVolume  : register(t7); // M57e
SamplerState gFroxelSampler : register(s2); // LINEAR/CLAMP (ホストがフレーム頭で張る)

struct VSOut
{
    float4 pos : SV_Position;
    float2 ndc : TEXCOORD0;
};

VSOut VSMain(uint vid : SV_VertexID)
{
    // フルスクリーン三角形 (deferred_light.hlsl と同じ頂点列)。z=1 = far 平面
    const float2 corners[3] = { float2(-1, -1), float2(-1, 3), float2(3, -1) };
    VSOut o;
    o.pos = float4(corners[vid], 1.0f, 1.0f);
    o.ndc = corners[vid];
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    // NDC の far/near 点をワールドへ戻し、視線方向を得る
    float4 pf = mul(float4(i.ndc, 1.0f, 1.0f), gInvViewProj);
    float4 pn = mul(float4(i.ndc, 0.0f, 1.0f), gInvViewProj);
    const float3 dir = normalize(pf.xyz / pf.w - pn.xyz / pn.w);
    const float t = dir.y;
    float3 c;
    if (t >= 0.0f) {
        c = lerp(gHorizonColor.rgb, gTopColor.rgb, saturate(t * 1.4f));
    } else {
        c = lerp(gHorizonColor.rgb, gBottomColor.rgb, saturate(-t * 1.4f));
    }
    if (gSkyFroxel.x != 0.0f) {
        const float4 v = gFroxelVolume.SampleLevel(
            gFroxelSampler,
            float3(i.pos.xy / gSkyFroxelScreen.xy, FroxelSampleWFar(gSkyFroxel.y)), 0);
        c = c * v.a + v.rgb;
    }
    return float4(c, 1.0f);
}
