// 地形のスプラットブレンド (M58d、spec §6.5)。deferred_terrain.hlsl と forward_terrain.hlsl の
// **唯一の共有本体** — 2 本のシェーダは「GBuffer へ書くか / その場でライティングするか」だけが
// 違い、地表の色と法線の作り方は同一でなければならない (食い違うと Forward と Deferred で
// 地形の見た目が変わる = 描画パスを切り替えた日に理由不明のピクセル差になる)。
//
// ★**common.hlsli には置けない。** あちらは「register 宣言を 1 つも持たない」ことを契約に
//   していて (postfx / cs 系が独自のスロット割当で include するため)、ここは register を
//   9 個持つ。ibl_common.hlsli / rt_common.hlsli と同じ「用途別の共有ヘッダ」の流儀。
//
// ★**スロットは t20 以降。** t0-t7 はホストパス (Deferred 光パス / Forward) の持ち物で、
//   t12-t15 / t6-t7 は他マイルストーンの予約席。地形は誰とも隣り合わない位置へ逃がす。
//   **C++ 側の正本は TerrainPass.h の kTerrainSplatSrvSlot / kTerrainAlbedoSrvSlot /
//   kTerrainNormalSrvSlot。** 食い違うと「地形だけが真っ黒」になるだけでコンパイルも実行も
//   通るので、tools\check_rules.ps1 の規則 9 が静的に照合している。
//
// ★**サンプラは 1 つも増やさない** (計画の付録「予約 2」)。ホストが張った
//   s0 (異方性 WRAP = マテリアル用) と s2 (LINEAR CLAMP = IBL 用) をそのまま借りる。

#ifndef MYE_TERRAIN_COMMON_INCLUDED
#define MYE_TERRAIN_COMMON_INCLUDED

// スプラットは RGBA8 の 4 チャンネル = レイヤ 4 枚が構造的な上限。
// **C++ 側の TerrainPass.h::kTerrainLayerCount / TerrainAsset::kMaxLayers と同値**
#define MYE_TERRAIN_LAYERS 4

Texture2D gTerrainSplat        : register(t20);
Texture2D gTerrainAlbedo[MYE_TERRAIN_LAYERS] : register(t21);
Texture2D gTerrainNormal[MYE_TERRAIN_LAYERS] : register(t25);

// TerrainPass.cpp の TerrainObjectCB と同一レイアウト (208 バイト)。
// **フィールドは末尾 append + 16 バイト境界**。C++ 側の static_assert がサイズを見張る
cbuffer TerrainObject : register(b4)
{
    float4x4 gTerrainWorld;
    float4   gTerrainSurface;                        // x=metallic y=roughness zw=予約
    float4   gTerrainTint[MYE_TERRAIN_LAYERS];       // rgb=リニア色 a=レイヤ有効フラグ
    float4   gTerrainTiling[MYE_TERRAIN_LAYERS];     // xy=繰り返し回数 zw=予約
};

// ブレンド結果。albedo はリニア、normalTS は接空間 (未正規化でよい — 呼び手が
// PerturbNormal へ渡す前に normalize する)
struct TerrainSurfaceSample
{
    float3 albedo;
    float3 normalTS;
};

// uv = 地形全体を [0,1] に張った正規化座標 (= ワールド XZ 由来。MeshVertex に 2 セット目の
// UV は足していない)。レイヤごとの繰り返しはここで tiling を掛けて作る。
//
// ★**重みの再正規化を持っている理由**: クック側 (TerrainAsset::QuantizeSplatWeights) が
//   1 テクセルの合計を 255 に量子化正規化しているので**本来は不要**。それでも掛けるのは、
//   レイヤ数が 4 未満のとき (gTerrainTint[i].a == 0 で殺したチャンネルに重みが残っている
//   ケース) に地表が暗く痩せるのを防ぐため。合計 0 はレイヤ 0 の 100% へ倒す
//   (0 除算で NaN を GBuffer へ書くと、そのピクセルが以降のパス全部で腐る)。
//
// ★**分岐を 1 つも持たない**: 未設定のレイヤにも白 (albedo) / 平坦法線 (normal) の 1x1 が
//   bind されている前提。`if` で読み分けると Sample が非一様フローに入り、
//   PerturbNormal が使う ddx/ddy の勾配が壊れる。
TerrainSurfaceSample SampleTerrainSurface(SamplerState layerSamp, SamplerState splatSamp,
                                          float2 uv)
{
    float4 w = gTerrainSplat.Sample(splatSamp, uv);
    // 有効フラグ (tint.a) を掛けてから合計 1 へ
    float4 mask = float4(gTerrainTint[0].a, gTerrainTint[1].a, gTerrainTint[2].a,
                         gTerrainTint[3].a);
    w *= mask;
    const float total = dot(w, 1.0f);
    w = (total > 1e-5f) ? (w / total) : float4(1.0f, 0.0f, 0.0f, 0.0f);

    TerrainSurfaceSample o;
    o.albedo = float3(0.0f, 0.0f, 0.0f);
    o.normalTS = float3(0.0f, 0.0f, 0.0f);
    [unroll] for (int i = 0; i < MYE_TERRAIN_LAYERS; ++i) {
        const float2 luv = uv * gTerrainTiling[i].xy;
        // アルベドは _SRGB フォーマットで bind してあるのでサンプル結果は既にリニア
        o.albedo += gTerrainAlbedo[i].Sample(layerSamp, luv).rgb * gTerrainTint[i].rgb * w[i];
        // 接空間法線の重み付き平均。個別に正規化してから混ぜても、混ぜてから正規化しても
        // 見た目は変わらないので後者 (乗算 1 回ぶん安い)
        const float3 nTS = gTerrainNormal[i].Sample(layerSamp, luv).xyz * 2.0f - 1.0f;
        o.normalTS += nTS * w[i];
    }
    return o;
}

#endif // MYE_TERRAIN_COMMON_INCLUDED
