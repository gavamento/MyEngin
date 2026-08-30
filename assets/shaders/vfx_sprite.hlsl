// VFX (M29c): Sprite / Trail / TextMesh のワールド空間クアッド描画。
// 頂点は CPU 構築済みのワールド座標。色 * テクスチャを出力 (アルファブレンド、深度書き込み無し)。
// Sprite は画像 or 白、Trail は白、TextMesh はフォントアトラス (rgb=1, a=カバレッジ) をバインド。
// M32c: シーンフォグを距離ベースで適用 (アルファブレンドなのでフォグ色へ lerp)。
// M57追補: その M32c の実装は **ApplyFog の手書き劣化コピー**で、M43a のハイトフォグと
// 太陽インスキャッタを 1 つも持っていなかった = 同じシーンでメッシュと VFX の霧の濃さが
// 食い違っていた。VFX クアッドは「深度を持つ alpha 合成のサーフェス」= 透明メッシュと
// まったく同じ分類なので、forward_lit.hlsl の形をそのまま写して共有の ApplyFog へ寄せた。

#include "common.hlsli"        // M57追補: ApplyFog (register 宣言ゼロなので衝突しない)
#include "froxel_common.hlsli" // M57追補: 受け持ちの分け方と合成 (同上)

cbuffer VfxCB : register(b0)
{
    float4x4 gViewProj; // transpose(view*proj) — 既存の「転置してアップロード」規約
    float3   gCameraPos;
    int      gFogMode;    // -1=off / 0=linear 1=exp 2=exp2
    float3   gFogColor;
    float    gFogDensity;
    float    gFogStart;
    float    gFogEnd;
    float2   _pad;
    // ---- M57追補: M43a のハイトフォグ + 太陽インスキャッタ (末尾 append) ----
    // 0/0 なら ApplyFog は M29d の距離フォグと同じ式に潰れる (= 従来の意味論)
    float    gFogHeightFalloff;
    float    gFogBaseHeight;
    float    gFogInscatterIntensity;
    float    gFogInscatterPower;
    float3   gSunDirection; // 光の進行方向 (正規化)
    float    _pad1;
    float3   gSunColor;     // リニア・強度込み
    float    _pad2;
    // ---- M57追補: フロクセル (0 = 従来経路へ厳密に落ちる分岐を持つ) ----
    int      gFroxelEnabled;
    float    gFroxelNearZ;
    float    gFroxelFarZ;
    float    gFroxelSlices;
    float2   gFroxelScreenSize; // SV_Position → uv
    float2   _pad3;
};

struct VSIn
{
    float3 pos : POSITION; // ワールド座標
    float2 uv : TEXCOORD;
    float4 color : COLOR;
};

struct VSOut
{
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR;
    // M57追補: dist (VS で計算したワールド距離) を **posW + viewZ に置き換えた**。
    //   ・posW  … ApplyFog / FroxelFogOrigin が要る (どちらもワールド座標で受ける)。
    //     距離を PS で毎ピクセル計算する形になり forward_lit と揃う — 大きなクアッドの
    //     角と中心で霧の量が変わるのが正しい (旧コードは VS 計算 + 線形補間だった)
    //   ・viewZ … フロクセルのスライス座標。透視投影では clip.w = ビュー空間 z。
    //     **PS の SV_Position.w は 1/w に化けている**ので別の補間子で運ぶ必要がある
    float3 posW  : TEXCOORD1;
    float  viewZ : TEXCOORD2;
};

VSOut VSMain(VSIn i)
{
    VSOut o;
    o.pos = mul(float4(i.pos, 1.0), gViewProj);
    o.uv = i.uv;
    o.color = i.color;
    o.posW = i.pos; // VSIn::pos は既にワールド座標 (CPU 側でビルボード展開済み)
    o.viewZ = o.pos.w;
    return o;
}

Texture2D    gTex           : register(t0);
Texture3D    gFroxelVolume  : register(t1); // M57追補 (rgb=積算内向き散乱 / a=透過率)
SamplerState gSamp          : register(s0); // バッチごとに LINEAR / POINT が入れ替わる
// ★フロクセル専用に s1 を取る。**s0 を流用してはいけない** — 内蔵 8x8 ビットマップフォントの
//   TextMesh バッチでは s0 が POINT に化けるので、そのバッチだけボリュームが最近傍サンプルに
//   なりブロックノイズが出る。オブジェクトは増やしていない (samplerLinear_ の相乗り)
SamplerState gFroxelSampler : register(s1);

float4 PSMain(VSOut i) : SV_TARGET
{
    float4 col = i.color * gTex.Sample(gSamp, i.uv);
    // ---- 大気散乱 (M29d + M43a、M57追補 でフロクセルと分担) ----
    // VFX クアッドは「深度を持つ alpha 合成のサーフェス」= 透明メッシュと同じ分類なので、
    // forward_lit.hlsl と同じ形をそのまま写す。gFroxelEnabled==0 なら else 側 = 従来経路
    if (gFroxelEnabled != 0)
    {
        // 解析フォグの起点を「視線がグリッドを出る点」まで押し出す = 受け持ちが 1m も
        // 重ならない (グリッド内なら posW と厳密に一致して ApplyFog が恒等になる)
        col.rgb = ApplyFog(col.rgb, gFogColor, gFogMode, gFogDensity, gFogStart, gFogEnd,
                           FroxelFogOrigin(gCameraPos, i.posW, i.viewZ, gFroxelFarZ), i.posW,
                           gFogHeightFalloff, gFogBaseHeight, gSunDirection, gSunColor,
                           gFogInscatterIntensity, gFogInscatterPower);
        // 加算経路が無い (ブレンドは SRC_ALPHA/INV_SRC_ALPHA の 1 種だけ) ので alpha 版で足りる。
        // src.rgb が「その場の放射輝度」なので scene·T + inscatter — ブレンドの SRC_ALPHA が
        // 後で a を掛ける形が、透明メッシュとまったく同じになる
        col.rgb = FroxelComposite(gFroxelVolume, gFroxelSampler, i.pos.xy, gFroxelScreenSize,
                                  i.viewZ, gFroxelSlices, gFroxelNearZ, gFroxelFarZ, col.rgb);
    }
    else
    {
        col.rgb = ApplyFog(col.rgb, gFogColor, gFogMode, gFogDensity, gFogStart, gFogEnd,
                           gCameraPos, i.posW, gFogHeightFalloff, gFogBaseHeight, gSunDirection,
                           gSunColor, gFogInscatterIntensity, gFogInscatterPower);
    }
    return col;
}
