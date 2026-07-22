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

} // namespace mye
