// かき乱しフィールド (M96) の描画側の共有定義。CB のレイアウト・束縛番号・2D 層の読み取り式をここに 1 本だけ置く。
//
// ★C++ の DisturbanceFieldPass.h (kDisturbance* と DisturbanceFieldCB) と一致させること
//   (tools\check_rules.ps1 規則 9 が定数を、DisturbanceFieldPass.cpp の static_assert が CB の大きさを見る)。
//   食い違っても「霧の濃さがずれる」形で静かに壊れる。
// ★束縛番号の既定は**コンピュートシェーダ (フロクセル注入) の番号**。t0 / s0 / u0 はシャドウアトラスと注入先が使う。
//   別の段 (地面の霧は ground_fog.hlsli、粒子・水・地形・草) が読むときは、その段の空きスロットを取るため、
//   読む側のシェーダで MYE_DFLD_SRV_FLOW / MYE_DFLD_SRV_AUX / MYE_DFLD_SAMPLER / MYE_DFLD_CB を先に #define してからこれを include する。
#ifndef MYE_DISTURBANCE_COMMON_HLSLI
#define MYE_DISTURBANCE_COMMON_HLSLI

// 同時に描ける 2D 層の数 (C++ の kDisturbanceMaxLayers)
#define MYE_DFLD_MAX_LAYERS 4
// 層の縁で濃さを 0 → 1 へ立ち上げる幅 [マス]。箱の壁を見せないための値
#define MYE_DFLD_EDGE_FADE_CELLS 2.0f
// CB 全体のバイト数 (C++ の kDisturbanceFieldCbBytes)
#define MYE_DFLD_CB_BYTES 336

#ifndef MYE_DFLD_SRV_FLOW
#define MYE_DFLD_SRV_FLOW 1
#endif
#ifndef MYE_DFLD_SRV_AUX
#define MYE_DFLD_SRV_AUX 2
#endif
#ifndef MYE_DFLD_SAMPLER
#define MYE_DFLD_SAMPLER 1
#endif
#ifndef MYE_DFLD_CB
#define MYE_DFLD_CB 1
#endif

#define MYE_DFLD_CAT(a, b) a##b
#define MYE_DFLD_REG(kind, n) register(MYE_DFLD_CAT(kind, n))

// 2D 層 1 枚の設定。C++ の DisturbanceLayerCB と同じ並び (全て float4 = 80 バイト)
struct DisturbanceLayer
{
    float4 originSize; // x = 原点セルの x, y = 原点セルの z (整数セル番号), z = cellSize [m], w = baseY [m]
    float4 extent;     // x = cellsX, y = cellsZ, z = thickness [m], w = 1: テクスチャを読む / 0: restDensity の定数
    float4 look;       // x = fogDensity [1/m], y = fogBrightness, z = restDensity, w = 1: 濃さチャネルあり
    float4 fogColor;   // rgb = 霧の色, w = 1: 太陽光 (平行光) を当てる / 0: 当てない (地面の霧だけが読む)
    float4 texScale;   // x = 1 / テクスチャ幅, y = 1 / テクスチャ高さ (配列は最大寸法で作るので層ごとに違う),
                       // z / w = 基準高さのずれ (G チャネル) の最小 / 最大 [m] (板を包む範囲)
};

cbuffer DisturbanceFieldCB : MYE_DFLD_REG(b, MYE_DFLD_CB)
{
    uint4 gDfldHeader; // x = 有効な層の数 (0..MYE_DFLD_MAX_LAYERS)
    DisturbanceLayer gDfldLayers[MYE_DFLD_MAX_LAYERS];
};

// (vx, vz, 濃さ ρ, 跡 τ)
Texture2DArray<float4> gDfldFlow : MYE_DFLD_REG(t, MYE_DFLD_SRV_FLOW);
// (波紋 h, 基準面の高さのずれ = 列の基準高さ - baseY)
Texture2DArray<float2> gDfldAux : MYE_DFLD_REG(t, MYE_DFLD_SRV_AUX);
SamplerState gDfldSampler : MYE_DFLD_REG(s, MYE_DFLD_SAMPLER);

// 層の高さの形 p(y)。基準面より下は 1、上へ thickness かけて 0。
// **C++ の DisturbanceSampleAt と同じ式** (読み取りと描画が食い違うと「霧の中に居るのに濃さが 0」になる)
float DfldHeightShape(float y, float baseY, float thickness)
{
    return y <= baseY ? 1.0f : saturate(1.0f - (y - baseY) / thickness);
}

// 層 i の位置 posW での消散係数 σ [1/m] (fogDensity × ρ × p(y) × 縁の立ち上げ)。層の格子の外 / 濃さチャネルなしは 0。
// **sub-03 以降、読むのは地面の霧 (ground_fog.hlsli) だけ**。フロクセルは立体 (Volume) のみ読む
float DfldLayerSigma(uint i, float3 posW)
{
    const DisturbanceLayer L = gDfldLayers[i];
    if (L.look.w < 0.5f) {
        return 0.0f; // 濃さチャネルが無い層は霧を出さない
    }
    const float cellSize = L.originSize.z;
    const float gx = posW.x / cellSize - L.originSize.x;
    const float gz = posW.z / cellSize - L.originSize.y;
    const float nx = L.extent.x;
    const float nz = L.extent.y;
    if (!(gx > 0.0f && gz > 0.0f && gx < nx && gz < nz)) {
        return 0.0f; // 格子の外
    }
    const float edge = min(min(gx, nx - gx), min(gz, nz - gz));
    const float edgeFade = saturate(edge / MYE_DFLD_EDGE_FADE_CELLS);

    float rho = L.look.z;       // 状態が無い (編集中・Play 前) は静止状態の定数
    float baseOffset = 0.0f;
    if (L.extent.w > 0.5f) {
        // セル中心 (i + 0.5) がテクセル中心になる並び。層が配列より小さいときに隣の層の余白を
        // 混ぜないよう、層の中のテクセル中心の範囲へ切る
        const float2 uvLo = float2(0.5f, 0.5f) * L.texScale.xy;
        const float2 uvHi = float2(nx - 0.5f, nz - 0.5f) * L.texScale.xy;
        const float2 uv = clamp(float2(gx, gz) * L.texScale.xy, uvLo, uvHi);
        rho = gDfldFlow.SampleLevel(gDfldSampler, float3(uv, (float)i), 0).z;
        baseOffset = gDfldAux.SampleLevel(gDfldSampler, float3(uv, (float)i), 0).y;
    }
    const float shape = DfldHeightShape(posW.y, L.originSize.w + baseOffset, L.extent.z);
    return L.look.x * rho * shape * edgeFade;
}

#endif
