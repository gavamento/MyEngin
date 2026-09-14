// M57a: フロクセルグリッドのクリア CS。
//
// 見た目には何も足さない。使うのは `Editor.exe --froxel-probe` (VolumeTexture.cpp) の計測だけで、
// 「空の CS を 921,600 セルにディスパッチしたときの WARP の壁時計」を測る対象そのもの —
// 本体が軽いほど測っているものが「ディスパッチとメモリ書き込みの下限」に近づく。
//
// ★FL11_0 の typed UAV **ロード** (RWTexture3D からの読み) は R32_FLOAT/UINT/SINT 限定
//   だが、ここは**ストアしかしない**ので R16G16B16A16_FLOAT で通る。
//   WARP が本当に通すかは机上で決まらない — プローブが書いて読み戻して確かめる。

// MYE_FROXEL_GROUP の正本は froxel_common.hlsli (注入 / テンポラル / 積分と同じ割り方)。
// C++ の mye::froxel::kGroupSize との一致は tools\check_rules.ps1 規則 9 が検査する
#include "froxel_common.hlsli"

// C++ の FroxelClearCB (VolumeTexture.cpp) とレイアウト一致 (32 バイト)
cbuffer FroxelClearCB : register(b0)
{
    uint3 gFroxelGridSize; // グリッドの実寸 (セル数)。境界のはみ出しを弾くのに使う
    uint gFroxelPad;
    float4 gFroxelClearValue; // rgb = inscatter / a = 消散 (注入の出力と同じ意味)
};

RWTexture3D<float4> gFroxelOut : register(u0);

[numthreads(MYE_FROXEL_GROUP, MYE_FROXEL_GROUP, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    // グリッド寸法は kGroupSize の倍数とは限らない (160x90x64 の 90 が 8 で割り切れない)。
    // 範囲外を弾かないと隣のスライスへ回り込む — 3D UAV の範囲外書き込みは
    // D3D が捨ててくれるが、依存しないで明示的に落とす
    if (any(id >= gFroxelGridSize)) {
        return;
    }
    gFroxelOut[id] = gFroxelClearValue;
}
