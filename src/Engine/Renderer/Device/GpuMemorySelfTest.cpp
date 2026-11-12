#include "Engine/Renderer/Device/GpuMemorySelfTest.h"

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Renderer/Device/GpuMemoryStats.h"

namespace mye {

bool RunGpuMemorySelfTest()
{
    MYE_LOG_INFO("==== GpuMemory self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    using gpumem::TextureBytes;
    // RGBA8 (32 bpp) 256x256、ミップ無し = 256 * 256 * 4
    check(TextureBytes(256, 256, 1, 1, 1, 32, false) == 262144u, "RGBA8 256x256, 1 mip");
    // 全 9 段 (256 .. 1): 4 * (65536 + 16384 + ... + 1) = 4 * 87381
    check(TextureBytes(256, 256, 1, 1, 9, 32, false) == 349524u, "RGBA8 256x256, full mip chain");
    // 正方形でないとき、短い辺は 1 で止まる: 4x1 → 2x1 → 1x1 = 4 + 2 + 1 画素
    check(TextureBytes(4, 1, 1, 1, 3, 32, false) == 28u, "non-square mips clamp the short side at 1");
    // キューブ = 6 面
    check(TextureBytes(64, 64, 1, 6, 1, 32, false) == 98304u, "cube map counts 6 faces");
    // 3D: 奥行きも段ごとに半分になる。16x16x16 (1 段) と、2 段目 8x8x8
    check(TextureBytes(16, 16, 16, 1, 1, 64, false) == 32768u, "volume texture, 1 mip (RGBA16F)");
    check(TextureBytes(16, 16, 16, 1, 2, 64, false) == 32768u + 4096u, "volume texture halves depth per mip");
    // BC1 (8 バイト / 4x4 ブロック = 4 bpp): 256x256 = 64 * 64 ブロック * 8
    check(TextureBytes(256, 256, 1, 1, 1, 4, true) == 32768u, "BC1 256x256");
    // BC7 (16 バイト / ブロック = 8 bpp): 5x5 は 2x2 ブロックへ切り上がる
    check(TextureBytes(5, 5, 1, 1, 1, 8, true) == 64u, "block-compressed sizes round up to 4x4 blocks");
    // 1x1 の段でも 1 ブロックぶん要る
    check(TextureBytes(1, 1, 1, 1, 1, 8, true) == 16u, "block-compressed 1x1 still takes one block");
    // 0 は 1 として扱う (未初期化の desc で 0 除算や 0 バイトにならない)
    check(TextureBytes(8, 8, 0, 0, 0, 32, false) == 256u, "zero depth / array / mips are treated as 1");

    MYE_LOG_INFO("==== GpuMemory self test: %s ====", failCount == 0 ? "PASS" : "FAIL");
    return failCount == 0;
}

} // namespace mye
