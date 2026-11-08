// 地形の跡 (M96 sub-13): かき乱しフィールドの 2D 層の跡チャネル τ を読み、雪・砂の地面に通った筋を描く。
//
// ★束縛は新しく持たない。地面の霧 (ground_fog.hlsli) が全 PS に張る t32 / t33 / s8 / b6 をそのまま読む。
//   層が 0 枚 (b6 が空) のときは何もしない。
// ★terrain_common.hlsli の後に include すること (gTerrainTrail を使う)。
// ★描画だけ。地形の高さ (当たり判定) は変えない。
#ifndef MYE_TERRAIN_TRAIL_HLSLI
#define MYE_TERRAIN_TRAIL_HLSLI

#include "ground_fog.hlsli"

// 層 li の位置 posW での跡 τ [0,1] と、その水平勾配 (dτ/dx, dτ/dz) [1/m] を足し込む。
// 板の高さ (層の基準面 ± thickness) の外、格子の外の点は何も足さない。縁の 2 マスは勾配を 0 へ落とす
// (中心差分が片側になって半端になるため。水面の波紋と同じ扱い)
void TerrainTrailAccumulate(uint li, float3 posW, inout float tau, inout float2 grad)
{
    const DisturbanceLayer L = gDfldLayers[li];
    if (L.extent.w < 0.5f) {
        return; // 状態が無い (編集中・Play 前) 層は跡を持たない
    }
    const float cellSize = L.originSize.z;
    const float gx = posW.x / cellSize - L.originSize.x;
    const float gz = posW.z / cellSize - L.originSize.y;
    const float nx = L.extent.x;
    const float nz = L.extent.y;
    if (!(gx > 0.0f && gz > 0.0f && gx < nx && gz < nz)) {
        return;
    }
    // 層が配列より小さいときに隣の層の余白を混ぜないよう、層の中のテクセル中心の範囲へ切る
    const float2 uvLo = float2(0.5f, 0.5f) * L.texScale.xy;
    const float2 uvHi = float2(nx - 0.5f, nz - 0.5f) * L.texScale.xy;
    const float2 uv = clamp(float2(gx, gz) * L.texScale.xy, uvLo, uvHi);
    const float layer = (float)li;
    // 起伏に沿う層 (followTerrain) は列ごとに基準面がずれる。その高さの板の中にある面だけが跡を受ける
    const float baseOffset = gDfldAux.SampleLevel(gDfldSampler, float3(uv, layer), 0).y;
    if (abs(posW.y - (L.originSize.w + baseOffset)) > L.extent.z) {
        return;
    }
    const float2 dx = float2(L.texScale.x, 0.0f);
    const float2 dz = float2(0.0f, L.texScale.y);
    const float t0 = gDfldFlow.SampleLevel(gDfldSampler, float3(uv, layer), 0).w;
    const float txp = gDfldFlow.SampleLevel(gDfldSampler, float3(clamp(uv + dx, uvLo, uvHi), layer), 0).w;
    const float txm = gDfldFlow.SampleLevel(gDfldSampler, float3(clamp(uv - dx, uvLo, uvHi), layer), 0).w;
    const float tzp = gDfldFlow.SampleLevel(gDfldSampler, float3(clamp(uv + dz, uvLo, uvHi), layer), 0).w;
    const float tzm = gDfldFlow.SampleLevel(gDfldSampler, float3(clamp(uv - dz, uvLo, uvHi), layer), 0).w;
    const float edge = min(min(gx, nx - gx), min(gz, nz - gz));
    const float edgeFade = saturate(edge / MYE_DFLD_EDGE_FADE_CELLS);
    tau = max(tau, t0);
    grad += edgeFade * float2(txp - txm, tzp - tzm) / (2.0f * cellSize);
}

// 跡を色と法線へ反映する。gTerrainTrail.w (= 跡の強さ) が 0 なら何も触らない = 既存の地形の絵と同一。
// 色は跡の色へ τ × 強さで寄せ、法線は τ の勾配に深さ (m) を掛けた凹みの傾きで曲げる (跡が濃い所ほど凹むとみなす)。
// normalW はワールド法線 (正規化済み)、albedo はリニア
void ApplyTerrainTrail(float3 posW, inout float3 albedo, inout float3 normalW)
{
    if (gTerrainTrail.w <= 0.0f) {
        return;
    }
    const uint count = min(gDfldHeader.x, (uint)MYE_DFLD_MAX_LAYERS);
    float tau = 0.0f;
    float2 grad = float2(0.0f, 0.0f);
    [loop] for (uint li = 0; li < count; ++li) {
        TerrainTrailAccumulate(li, posW, tau, grad);
    }
    albedo = lerp(albedo, gTerrainTrail.rgb, saturate(tau * gTerrainTrail.w));
    // 深さ d の凹み h = -d·τ の法線は (-dh/dx, 1, -dh/dz) = (d·τx, 1, d·τz)
    const float depth = gTerrainSurface.w;
    normalW = normalize(normalW + float3(depth * grad.x, 0.0f, depth * grad.y));
}

#endif // MYE_TERRAIN_TRAIL_HLSLI
