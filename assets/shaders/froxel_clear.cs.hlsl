// M57a: フロクセルグリッドのクリア。
//
// 見た目には何も足さない「土台」のシェーダで、役割は 2 つある:
//   ① 毎フレーム、注入 (M57b) の前にグリッドを既定値へ潰す。
//      ClearUnorderedAccessViewFloat でも同じことはできるが、注入と積分が
//      **同じスレッド割り** (XY タイル + Z 列) で走るかどうかをここで先に確定させたい —
//      フロクセルのコストはセル数がそのまま効くので、割り方の実測が設計の入力になる。
//   ② `Editor.exe --froxel-probe` が回す計測対象そのもの。「空の CS を 921,600 セルに
//      ディスパッチしたときの WARP の壁時計」が M57a の成果物なので、
//      本体が軽いほど測っているものが「ディスパッチとメモリ書き込みの下限」に近づく。
//
// ★FL11_0 の typed UAV **ロード** (RWTexture3D からの読み) は R32_FLOAT/UINT/SINT 限定
//   だが、ここは**ストアしかしない**ので R16G16B16A16_FLOAT で通る。
//   WARP が本当に通すかは机上で決まらない — プローブが書いて読み戻して確かめる。

// M57c: MYE_FROXEL_GROUP の正本は froxel_common.hlsli へ移した
// (注入 / テンポラル / 積分 の 3 本が同じ割り方を要求するようになったため)。
// C++ の mye::froxel::kGroupSize との一致は tools\check_rules.ps1 規則 9 が検査する
#include "froxel_common.hlsli"

// C++ の FroxelClearCB (VolumeTexture.cpp) とレイアウト一致 (32 バイト)
cbuffer FroxelClearCB : register(b0)
{
    uint3 gFroxelGridSize; // グリッドの実寸 (セル数)。境界のはみ出しを弾くのに使う
    uint gFroxelPad;
    float4 gFroxelClearValue; // rgb = inscatter / a = 消散 (M57b で意味が付く)
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
