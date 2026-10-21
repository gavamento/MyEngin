//====================================================================================
//                          PixelPerfect2D.cpp
//  MyEngin/ 秋田蓮音                                                       10/22/2026
//                                          ピクセルパーフェクトカメラの計算の実装
//====================================================================================
#include "Engine/Renderer/Pipeline/PixelPerfect2D.h"

#include <algorithm>

namespace mye::pixelperfect2d {
namespace {

// 画面を覆う最小の偶数 (偶数にするのは、RT の中心が画素の境界に来て拡大後の中央寄せが左右対称になるため)
int32_t CoverEven(int32_t screen, int32_t zoom)
{
    const int32_t covering = (screen + zoom - 1) / zoom;
    return covering + (covering & 1);
}

} // namespace

Result Compute(int32_t screenW, int32_t screenH, const Settings& settings)
{
    const int32_t w = std::max(screenW, 1);
    const int32_t h = std::max(screenH, 1);
    const int32_t refX = std::max(settings.refResolutionX, 1);
    const int32_t refY = std::max(settings.refResolutionY, 1);
    const int32_t ppu = std::max(settings.assetsPPU, 1);

    Result r;
    r.zoom = std::max(1, std::min(h / refY, w / refX));
    r.upscale = settings.upscaleRT && r.zoom > 1;
    r.viewOrthoSize = static_cast<float>(h) * 0.5f / static_cast<float>(r.zoom * ppu);
    if (r.upscale) {
        r.rtW = CoverEven(w, r.zoom);
        r.rtH = CoverEven(h, r.zoom);
        r.orthoSize = static_cast<float>(r.rtH) * 0.5f / static_cast<float>(ppu);
    } else {
        r.rtW = w;
        r.rtH = h;
        r.orthoSize = r.viewOrthoSize;
    }
    return r;
}

} // namespace mye::pixelperfect2d
