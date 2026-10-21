//====================================================================================
//                          PixelPerfect2DSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                       10/22/2026
//                                          pixelperfect2d::Compute の回帰テスト実装
//====================================================================================
#include "Engine/Renderer/Pipeline/PixelPerfect2DSelfTest.h"

#include <cmath>
#include <cstdint>
#include <cstdio>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Renderer/Pipeline/PixelPerfect2D.h"

namespace mye {
namespace {

struct Case {
    int32_t w;
    int32_t h;
    int32_t ppu;
    int32_t refX;
    int32_t refY;
    bool upscaleRT;
    int32_t zoom;
    bool upscale;
    int32_t rtW;
    int32_t rtH;
    float orthoSize;
    float viewOrthoSize;
};

// 期待値は Unity の式 (zoom = min(H / refY, W / refX)、orthoSize = RT 高 / 2 / PPU) を手計算したもの。
// 端数の W / zoom は切り上げてから偶数にする (RT が画面より小さいと黒帯が出る)
constexpr Case kCases[] = {
    // w     h   ppu refX refY up   zoom upscale rtW rtH  ortho     view
    { 960, 540, 16, 320, 180, true, 3, true, 320, 180, 5.625f, 5.625f },
    { 1280, 720, 16, 320, 180, true, 4, true, 320, 180, 5.625f, 5.625f },
    { 1920, 1080, 16, 320, 180, true, 6, true, 320, 180, 5.625f, 5.625f },
    { 959, 540, 16, 320, 180, true, 2, true, 480, 270, 8.4375f, 8.4375f },    // 479.5 -> 480 (偶数)
    { 1000, 540, 16, 320, 180, true, 3, true, 334, 180, 5.625f, 5.625f },     // 333.33 -> 334
    { 1003, 543, 16, 320, 180, true, 3, true, 336, 182, 5.6875f, 5.65625f },  // 334.33 -> 335 -> 336 / 181 -> 182
    { 640, 360, 100, 320, 180, true, 2, true, 320, 180, 0.9f, 0.9f },
    { 300, 170, 16, 320, 180, true, 1, false, 300, 170, 5.3125f, 5.3125f },   // 基準より小さい = 等倍
    { 960, 540, 16, 320, 180, false, 3, false, 960, 540, 5.625f, 5.625f },    // upscaleRT off: 解像度そのまま
    { 960, 400, 16, 320, 180, true, 2, true, 480, 200, 6.25f, 6.25f },        // 高さが先に詰まる (400 / 180 = 2)
};

bool Near(float a, float b)
{
    return std::fabs(a - b) < 1.0e-5f;
}

} // namespace

bool RunPixelPerfect2DSelfTest()
{
    MYE_LOG_INFO("==== PixelPerfect2D self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    for (const Case& c : kCases) {
        pixelperfect2d::Settings s;
        s.assetsPPU = c.ppu;
        s.refResolutionX = c.refX;
        s.refResolutionY = c.refY;
        s.upscaleRT = c.upscaleRT;
        const pixelperfect2d::Result r = pixelperfect2d::Compute(c.w, c.h, s);
        MYE_LOG_INFO("  [pixelperfect] %dx%d ref %dx%d ppu %d up %d -> zoom %d upscale %d rt %dx%d ortho %.5f view %.5f", c.w, c.h,
                     c.refX, c.refY, c.ppu, c.upscaleRT ? 1 : 0, r.zoom, r.upscale ? 1 : 0, r.rtW, r.rtH,
                     static_cast<double>(r.orthoSize), static_cast<double>(r.viewOrthoSize));
        const bool ok = r.zoom == c.zoom && r.upscale == c.upscale && r.rtW == c.rtW && r.rtH == c.rtH
            && Near(r.orthoSize, c.orthoSize) && Near(r.viewOrthoSize, c.viewOrthoSize);
        char what[96];
        std::snprintf(what, sizeof(what), "pixelperfect: %dx%d (ref %dx%d, ppu %d%s) matches the table", c.w, c.h, c.refX, c.refY,
                      c.ppu, c.upscaleRT ? "" : ", no RT");
        check(ok, what);
    }

    // 960x540 / ref 320x180 / PPU 16: 1 タイル (16 px) は RT で 16 px、画面で 48 px、ワールドで 1 単位
    {
        pixelperfect2d::Settings s;
        s.assetsPPU = 16;
        const pixelperfect2d::Result r = pixelperfect2d::Compute(960, 540, s);
        const float tilePixelsOnRt = 1.0f / (2.0f * r.orthoSize / static_cast<float>(r.rtH));
        check(Near(tilePixelsOnRt, 16.0f) && Near(tilePixelsOnRt * static_cast<float>(r.zoom), 48.0f),
              "pixelperfect: a 16 px tile is 16 px on the RT and 48 px on a 960x540 screen");
    }

    // 総当たり: RT は画面を覆う最小の偶数 (黒帯なし、無駄な追加行なし)、zoom は基準解像度が収まる最大
    bool covered = true;
    bool minimal = true;
    bool evenSize = true;
    bool maxZoom = true;
    for (int32_t w = 320; w <= 2600 && covered && minimal && evenSize && maxZoom; w += 7) {
        for (int32_t h = 180; h <= 1500; h += 11) {
            pixelperfect2d::Settings s;
            s.assetsPPU = 16;
            const pixelperfect2d::Result r = pixelperfect2d::Compute(w, h, s);
            if (!r.upscale) {
                continue;
            }
            covered = covered && r.rtW * r.zoom >= w && r.rtH * r.zoom >= h;
            minimal = minimal && (r.rtW - 2) * r.zoom < w && (r.rtH - 2) * r.zoom < h;
            evenSize = evenSize && (r.rtW % 2 == 0) && (r.rtH % 2 == 0);
            maxZoom = maxZoom && ((r.zoom + 1) * 180 > h || (r.zoom + 1) * 320 > w)
                && r.zoom * 180 <= h && r.zoom * 320 <= w;
        }
    }
    check(covered, "pixelperfect: the RT magnified by zoom always covers the screen (no black bands)");
    check(minimal, "pixelperfect: the RT is the smallest even size that covers the screen");
    check(evenSize, "pixelperfect: the RT width and height are even");
    check(maxZoom, "pixelperfect: zoom is the largest integer for which the reference resolution still fits");

    // 不正な入力でも落ちない (0 以下は 1 として扱う)
    {
        pixelperfect2d::Settings s;
        s.assetsPPU = 0;
        s.refResolutionX = 0;
        s.refResolutionY = -5;
        const pixelperfect2d::Result r = pixelperfect2d::Compute(0, -3, s);
        check(r.zoom >= 1 && r.rtW >= 1 && r.rtH >= 1 && std::isfinite(r.orthoSize) && r.orthoSize > 0.0f,
              "pixelperfect: zero / negative sizes and settings are treated as 1 (no division by zero)");
    }

    MYE_LOG_INFO("==== PixelPerfect2D self test: %s ====", failCount == 0 ? "PASS" : "FAIL");
    return failCount == 0;
}

} // namespace mye
