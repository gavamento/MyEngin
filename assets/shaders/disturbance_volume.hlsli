// かき乱しフィールド (M96) の Volume (3D) の描画側の共有定義。CB のレイアウト・束縛番号・立体の霧の式をここに 1 本だけ置く。
//
// ★C++ の DisturbanceVolumePass.h (kDisturbanceMaxVolumes / kDisturbanceVolumeCbBytes / kDisturbanceVolumeSrvSlot /
//   kDisturbanceVolumeCbSlot と DisturbanceVolumeCB) と一致させること
//   (tools\check_rules.ps1 規則 9 が定数を、DisturbanceVolumePass.h の static_assert が CB の大きさを見る)。
// ★フロクセル注入 (コンピュートシェーダ) だけが読む。サンプラは disturbance_common.hlsli の s1 (線形・クランプ) を使う。
// ★Texture3D は配列にできないので、Volume 1 個につき 1 本の SRV (t3〜t6)。寸法は Volume ごとに違ってよい。
//   テクスチャの並びは sim の配列そのまま: 幅 = cellsX、高さ = cellsZ、奥行き = cellsY
#ifndef MYE_DISTURBANCE_VOLUME_HLSLI
#define MYE_DISTURBANCE_VOLUME_HLSLI

#include "disturbance_common.hlsli"

// 同時に描ける Volume の数 (C++ の kDisturbanceMaxVolumes)
#define MYE_DFLD_VOL_MAX 4
// CB 全体のバイト数 (C++ の kDisturbanceVolumeCbBytes)
#define MYE_DFLD_VOL_CB_BYTES 272
// 先頭の SRV の番号 (C++ の kDisturbanceVolumeSrvSlot。続く 3 本が連番) と CB の番号 (kDisturbanceVolumeCbSlot)
// 別の段 (粒子の sim CS) が読むときは、先に MYE_DFLD_VOL_T0〜T3 と MYE_DFLD_VOL_CB を #define して空きスロットへ張る
#ifndef MYE_DFLD_VOL_CB
#define MYE_DFLD_VOL_CB 2
#endif
#define MYE_DFLD_VOL_SRV 3
#ifndef MYE_DFLD_VOL_T0
#define MYE_DFLD_VOL_T0 3
#define MYE_DFLD_VOL_T1 4
#define MYE_DFLD_VOL_T2 5
#define MYE_DFLD_VOL_T3 6
#endif

// Volume 1 個の設定。C++ の DisturbanceVolumeEntryCB と同じ並び (全て float4 = 64 バイト)
struct DisturbanceVolume
{
    float4 originSize; // xyz = 原点セルの番号 (整数), w = cellSize [m]
    float4 extent;     // xyz = cellsX / cellsY / cellsZ, w = 1: テクスチャを読む / 0: restDensity の定数
    float4 look;       // x = fogDensity [1/m], y = fogBrightness, z = restDensity, w = 1: 濃さチャネルあり
    float4 fogColor;   // rgb = 霧の色
};

cbuffer DisturbanceVolumeCB : MYE_DFLD_REG(b, MYE_DFLD_VOL_CB)
{
    uint4 gDfldVolHeader; // x = 有効な Volume の数 (0..MYE_DFLD_VOL_MAX)
    DisturbanceVolume gDfldVolumes[MYE_DFLD_VOL_MAX];
};

// (vx, vy, vz, 濃さ ρ)
Texture3D<float4> gDfldVol0 : MYE_DFLD_REG(t, MYE_DFLD_VOL_T0);
Texture3D<float4> gDfldVol1 : MYE_DFLD_REG(t, MYE_DFLD_VOL_T1);
Texture3D<float4> gDfldVol2 : MYE_DFLD_REG(t, MYE_DFLD_VOL_T2);
Texture3D<float4> gDfldVol3 : MYE_DFLD_REG(t, MYE_DFLD_VOL_T3);

// Volume i の位置 posW での消散係数 σ [1/m] = fogDensity × ρ × 縁の立ち上げ (3D の縁 2 マス) を sigma へ、
// 自己照明 σ × fogColor × fogBrightness を emit へ足す。格子の AABB の外は読まずに抜ける (分岐)
void DfldVolumeAccumulate(uint i, Texture3D<float4> tex, float3 posW, inout float sigma, inout float3 emit)
{
    if (i >= gDfldVolHeader.x) {
        return;
    }
    const DisturbanceVolume V = gDfldVolumes[i];
    if (V.look.w < 0.5f) {
        return; // 濃さチャネルが無い Volume は霧を出さない
    }
    const float3 g = posW / V.originSize.w - V.originSize.xyz;
    const float3 n = V.extent.xyz;
    if (!(all(g > 0.0f) && all(g < n))) {
        return;
    }
    const float3 e = min(g, n - g);
    const float edgeFade = saturate(min(min(e.x, e.y), e.z) / MYE_DFLD_EDGE_FADE_CELLS);

    float rho = V.look.z; // 状態が無い (編集中・Play 前) は静止状態の定数
    if (V.extent.w > 0.5f) {
        // セル中心 (i + 0.5) がテクセル中心になる並び。テクスチャは (x, z, y) の順
        rho = tex.SampleLevel(gDfldSampler, float3(g.x / n.x, g.z / n.z, g.y / n.y), 0).w;
    }
    const float s = V.look.x * rho * edgeFade;
    sigma += s;
    emit += s * V.fogColor.rgb * V.look.y;
}

#endif
