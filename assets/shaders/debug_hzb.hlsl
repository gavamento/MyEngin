// M56c: HZB (min-Z ピラミッド) の可視化。
// HZB を読む本番の消費者は M56d (SSR) まで存在しないので、「本当に段が積めているか」を
// 人間が確かめられる唯一の口がこれになる (M55c の debug_velocity と同じ立ち位置)。
// 既定 off (RenderView::hzbDebug == 0) のときは呼ばれない = 絵は 1 ビットも変わらない。
//
// 表示規約:
//   ・選んだミップを**最近傍で画面いっぱいに引き伸ばす** — 段が上がるほど四角が粗くなるので、
//     ピラミッドが本当に段を持っていることが一目で分かる (バイリニアだと粗さが溶けて分からない)。
//   ・明るいほど手前。gHzbDbgRange [world 単位] で黒 (= それ以遠) に振り切る。
//   ・ジオメトリが 1 つも無かった領域 (深度 1.0 のまま) は**青**にする。
//     灰色の濃淡だけだと「遠い床」と「何も無い」が見分けられず、
//     min-Z が far のまま残っている取りこぼしを見逃す。

#include "common.hlsli" // LinearizeDepth (M55a の共有版)

cbuffer HzbDebugCB : register(b0)
{
    float2 gHzbDbgDstSize; // 描画先の解像度 (px)
    float2 gHzbDbgMipSize; // 表示するミップの解像度 (texel)
    float  gHzbDbgNearZ;
    float  gHzbDbgFarZ;
    float  gHzbDbgRange; // この距離 [world] で黒に振り切る
    float  gHzbDbgMip;   // 表示するミップ番号
};

Texture2D<float> gHzb : register(t0);

struct VSOut
{
    float4 pos : SV_Position;
};

VSOut VSMain(uint vid : SV_VertexID)
{
    const float2 corners[3] = { float2(-1, -1), float2(-1, 3), float2(3, -1) };
    VSOut o;
    o.pos = float4(corners[vid], 0.0f, 1.0f);
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    // 画面 px → ミップの texel (最近傍)。ミップが 1x1 まで縮んでも 0 除算しない
    const float2 t = i.pos.xy * gHzbDbgMipSize / max(gHzbDbgDstSize, float2(1.0f, 1.0f));
    const int2 px = clamp(int2(t), int2(0, 0), int2(gHzbDbgMipSize) - int2(1, 1));
    const float d = gHzb.Load(int3(px, int(gHzbDbgMip)));
    if (d >= 0.99999f) {
        return float4(0.05f, 0.10f, 0.35f, 1.0f); // ジオメトリ無し (min-Z が far のまま)
    }
    const float z = LinearizeDepth(d, gHzbDbgNearZ, gHzbDbgFarZ);
    const float g = saturate(1.0f - z / max(gHzbDbgRange, 1e-3f));
    return float4(g, g, g, 1.0f);
}
