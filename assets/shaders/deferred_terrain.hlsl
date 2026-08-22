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
//
// ★**統合時の申し送り (M55c)**: 同じ Wave で GBuffer が 5 枚 (RT4 = velocity R16G16_FLOAT)
//   になる。このブランチにはまだその変更が無いので 4 枚のまま書いてある。統合では
//   PSOut の末尾へ `float2 velocity : SV_Target4;` を足し、**静的な地形なので 0 を書く**
//   のが正解 (地形は動かない = 画面速度 0)。

#include "common.hlsli" // EncodeEmissive (gbMaterial.b の符号化規約)

cbuffer PerFrame : register(b0)
{
    float4x4 gViewProj;
    float3   gCameraPos;
    float    _pad0;
};

// TerrainPass.cpp の TerrainObjectCB と同一レイアウト (96 バイト)
cbuffer TerrainObject : register(b4)
{
    float4x4 gWorld;
    float4   gBaseColor;   // リニア変換済み
    float    gMetallic;
    float    gRoughness;
    float2   _terrainPad;
};

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
    const float4 posW = mul(float4(v.pos, 1.0f), gWorld);
    o.pos = mul(posW, gViewProj);
    o.normalW = normalize(mul(v.normal, (float3x3)gWorld));
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
    // 法線は頂点法線をそのまま使う。TerrainSystem が**地形全体の texel 座標**で中心差分を
    // 取っているのでチャンク境界で食い違わない (M58b)。法線マップは M58d のレイヤと一緒に
    // 入る (タンジェントは持たず common.hlsli の PerturbNormal で画面微分から組む)
    const float3 n = normalize(i.normalW);
    // M58c v1: 単色サーフェス。4 レイヤのスプラットブレンドは M58d で gBaseColor を
    // 置き換える形で入る (UV はワールド XZ 由来 = i.uv が地形全体の [0,1] 正規化座標)
    o.albedo = float4(gBaseColor.rgb, 1.0f);
    o.normal = float4(n * 0.5f + 0.5f, 1.0f);
    o.position = float4(i.posW, 1.0f);
    o.material = float4(gMetallic, gRoughness, EncodeEmissive(0.0f), 1.0f);
    return o;
}
