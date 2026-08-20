#include "Engine/Renderer/ImageDiff.h"

#include <algorithm>
#include <cstdlib>
#include <vector>

#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/ImageWrite.h"

#include "stb/stb_image.h"

namespace mye {

namespace {

// stb_image は StbImpl.cpp で STBI_WINDOWS_UTF8 付きで実装されているので
// UTF-8 のパス文字列をそのまま渡せる (GpuResources のデコードと同じ経路)
struct LoadedImage {
    stbi_uc* pixels = nullptr;
    int w = 0;
    int h = 0;
    LoadedImage() = default;
    LoadedImage(const LoadedImage&) = delete;            // 生ポインタ所有なのでコピー禁止
    LoadedImage& operator=(const LoadedImage&) = delete;
    ~LoadedImage()
    {
        if (pixels) {
            stbi_image_free(pixels);
        }
    }
    bool Load(const std::wstring& path)
    {
        int comp = 0;
        pixels = stbi_load(WideToUtf8(path).c_str(), &w, &h, &comp, 4); // 常に RGBA8 へ正規化
        return pixels != nullptr;
    }
};

} // namespace

ImageDiffResult CompareImageFiles(const std::wstring& aPath, const std::wstring& bPath,
                                  int tolerance, const std::wstring& diffOutPath)
{
    ImageDiffResult r;
    LoadedImage a;
    LoadedImage b;
    if (!a.Load(aPath)) {
        r.error = "cannot read image A: " + WideToUtf8(aPath);
        return r;
    }
    if (!b.Load(bPath)) {
        r.error = "cannot read image B: " + WideToUtf8(bPath);
        return r;
    }
    if (a.w != b.w || a.h != b.h) {
        r.error = "size mismatch: " + std::to_string(a.w) + "x" + std::to_string(a.h) + " vs "
            + std::to_string(b.w) + "x" + std::to_string(b.h);
        return r;
    }

    r.valid = true;
    r.width = a.w;
    r.height = a.h;
    r.totalPixels = static_cast<int64_t>(a.w) * a.h;

    std::vector<uint8_t> heat;
    const bool wantHeat = !diffOutPath.empty();
    if (wantHeat) {
        heat.resize(static_cast<size_t>(r.totalPixels) * 4);
    }

    for (int64_t p = 0; p < r.totalPixels; ++p) {
        const stbi_uc* pa = a.pixels + p * 4;
        const stbi_uc* pb = b.pixels + p * 4;
        int d = 0;
        for (int c = 0; c < 4; ++c) {
            d = std::max(d, std::abs(static_cast<int>(pa[c]) - static_cast<int>(pb[c])));
        }
        if (d > r.maxChannelDiff) {
            r.maxChannelDiff = d;
            r.worstX = static_cast<int>(p % a.w);
            r.worstY = static_cast<int>(p / a.w);
        }
        if (d > 0) {
            ++r.diffPixelsAny;
        }
        if (d > tolerance) {
            ++r.diffPixels;
        }
        if (wantHeat) {
            uint8_t* o = heat.data() + static_cast<size_t>(p) * 4;
            if (d > 0) {
                // 差の大きさで黄 (小) → 赤 (大)。1 でも違えば必ず目に入る色にする
                o[0] = 255;
                o[1] = static_cast<uint8_t>(std::clamp(255 - d * 8, 0, 255));
                o[2] = 0;
            } else {
                // 一致部は元画像 (A) のグレースケールを暗く敷いて位置関係を分かるように
                const int g = (pa[0] * 77 + pa[1] * 150 + pa[2] * 29) >> 8;
                o[0] = o[1] = o[2] = static_cast<uint8_t>(g / 4);
            }
            o[3] = 255;
        }
    }

    if (wantHeat) {
        WritePngRGBA(diffOutPath, heat.data(), a.w, a.h, a.w * 4);
    }
    return r;
}

} // namespace mye
