// 地形の Deferred ジオメトリパス (M58c、spec §6.5)。GBuffer へ直接書く。
//
// ★**なぜ専用シェーダなのか**: Deferred の不透明パスは material->shader を**見ない**
//   (DeferredPath.cpp が deferred_gbuffer / _skinned / _instanced の 3 種を固定 bind する)。
//   だから地形を「マテリアル」として通そうとしても永久に GBuffer シェーダで描かれる。
//   TerrainPass が本ファイルを直接 bind するのが唯一の道 (Forward は material->shader を
//   見るので forward_terrain.hlsl も同じ形で用意してある)。
//
// ★**b0 はホストパス (DeferredPath) が張った PerFrame をそのまま読む。** 必要なのは
//   先頭の viewProj / cameraPos だけなので、deferred_gbuffer.hlsl と同じく「前半だけ」を
//   宣言する (cbuffer の実体が宣言より大きいのは合法)。
// ★**地形固有の値は b4。** b1-b3 は PerObject / MaterialParams / ボーンパレットで、
//   ここを張り替えると**後段の透明描画が地形の CB を読む**。空いている b4 を使えば
//   ホスト側は 1 バイトも張り直さなくてよい (TerrainPass.h の kTerrainObjectCbSlot が正本)。
//   b4 の中身とレイヤの bind / ブレンド本体は terrain_common.hlsli にある
//   (forward_terrain.hlsl と**必ず同じ地表**を出すための共有点)。
//
// ★**統合時の申し送り (M55c)**: 同じ Wave で GBuffer が 5 枚 (RT4 = velocity R16G16_FLOAT)
//   になる。このブランチにはまだその変更が無いので 4 枚のまま書いてある。統合では
//   PSOut の末尾へ `float2 velocity : SV_Target4;` を足し、**静的な地形なので 0 を書く**
//   のが正解 (地形は動かない = 画面速度 0)。

#include "common.hlsli"         // EncodeEmissive (gbMaterial.b の符号化規約) / PerturbNormal
#include "terrain_common.hlsli" // M58d: b4 の TerrainObject + t20.. のレイヤ + ブレンド本体

cbuffer PerFrame : register(b0)
{
    float4x4 gViewProj;
    float3   gCameraPos;
    float    _pad0;
};

// s0 = 異方性 WRAP (レイヤの繰り返し)、s2 = LINEAR CLAMP (スプラット)。
// **どちらもホスト (DeferredPath) がジオメトリパスの頭で張ったものをそのまま借りる**
SamplerState gLayerSampler : register(s0);
SamplerState gSplatSampler : register(s2);

struct VSIn
{
    float3 pos    : POSITION;
    float3 normal : NORMAL;
    float2 uv     : TEXCOORD0;
};

struct VSOut
{
    float4 pos     : SV_Position;
    float3 normalW : NORMAL;
    float2 uv      : TEXCOORD0;
    float3 posW    : TEXCOORD1;
};

VSOut VSMain(VSIn v)
{
    VSOut o;
    const float4 posW = mul(float4(v.pos, 1.0f), gTerrainWorld);
    o.pos = mul(posW, gViewProj);
    o.normalW = normalize(mul(v.normal, (float3x3)gTerrainWorld));
    o.uv = v.uv;
    o.posW = posW.xyz;
    return o;
}

struct PSOut
{
    float4 albedo   : SV_Target0; // rgb = albedo, a = 1 (ジオメトリ有り)
    float4 normal   : SV_Target1; // ワールド法線 *0.5+0.5 (R10G10B10A2)
    float4 position : SV_Target2; // ワールド座標 (R16G16B16A16_FLOAT)
    float4 material : SV_Target3; // r=metallic g=roughness b=emissive/MYE_EMISSIVE_MAX
    // ★M55c 統合時: float2 velocity : SV_Target4; を足して 0 を書く (静的地形)
};

PSOut PSMain(VSOut i)
{
    PSOut o;
    // 頂点法線は TerrainSystem が**地形全体の texel 座標**で中心差分を取ったもの =
    // チャンク境界で食い違わない (M58b)。ここへレイヤの法線マップを重ねる。
    // タンジェント属性は持たず、common.hlsli の PerturbNormal が画面微分から TBN を組む
    // (★TBN の基準 UV は**タイリング前の i.uv**。tiling はスケールでしかなく、
    //   PerturbNormal が invmax で規格化するので接空間の向きは変わらない)
    const float3 nGeom = normalize(i.normalW);
    const TerrainSurfaceSample surf = SampleTerrainSurface(gLayerSampler, gSplatSampler, i.uv);
    const float3 n = PerturbNormal(nGeom, i.posW, i.uv, normalize(surf.normalTS));

    o.albedo = float4(surf.albedo, 1.0f);
    o.normal = float4(n * 0.5f + 0.5f, 1.0f);
    o.position = float4(i.posW, 1.0f);
    o.material = float4(gTerrainSurface.x, gTerrainSurface.y, EncodeEmissive(0.0f), 1.0f);
    return o;
}
