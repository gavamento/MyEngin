#pragma once
#include <cstdint>

namespace mye {

// スクリプトの DebugDrawLine (ABI v7、M37) が tick 内に積む線分コマンド。
// 描画専用 (非 hash) — EngineLoop が tick 頭にクリアし、RenderSystem が
// EditorLinePass でシーンに重ね描きする。record/verify 中に積まれても sim に無関係。
struct DebugLineCmd {
    float ax = 0, ay = 0, az = 0;
    float bx = 0, by = 0, bz = 0;
    uint32_t rgba = 0xFFFFFFFFu; // EditorLinePass::Unpack と同じ 0xRRGGBBAA
};

// 半透明の塗りの頂点 (3 頂点で三角形 1 枚)。NavFillPass の頂点バッファと同じ並び (位置 3 + 色 4 float)
struct DebugFillVertex {
    float x = 0, y = 0, z = 0;
    float r = 0, g = 0, b = 0, a = 0; // 0..1、非乗算
};
static_assert(sizeof(DebugFillVertex) == 28, "NavFillPass の頂点ストライドと揃える");

} // namespace mye
