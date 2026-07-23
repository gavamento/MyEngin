#include "Engine/Renderer/TextureCook.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

#include "Engine/Core/Log.h"
#include "Engine/Platform/PathUtil.h"

#include "stb/stb_dxt.h"
#include "stb/stb_image.h"

namespace mye::TextureCook {
namespace {

// ---- DDS 書き出し用ヘッダ (読込側 GpuResources.cpp と同レイアウト) ----
constexpr uint32_t kDdsMagic = 0x20534444; // "DDS "
constexpr uint32_t kDdsdCaps = 0x1;
constexpr uint32_t kDdsdHeight = 0x2;
constexpr uint32_t kDdsdWidth = 0x4;
constexpr uint32_t kDdsdPixelFormat = 0x1000;
constexpr uint32_t kDdsdMipMapCount = 0x20000;
constexpr uint32_t kDdsdPitch = 0x8;
constexpr uint32_t kDdsdLinearSize = 0x80000;
constexpr uint32_t kDdpfFourCC = 0x4;
constexpr uint32_t kDdpfRgb = 0x40;
constexpr uint32_t kDdpfAlphaPixels = 0x1;
constexpr uint32_t kDdsCapsComplex = 0x8;
constexpr uint32_t kDdsCapsTexture = 0x1000;
constexpr uint32_t kDdsCapsMipMap = 0x400000;

constexpr uint32_t MakeFourCC(char a, char b, char c, char d)
{
    return static_cast<uint32_t>(static_cast<uint8_t>(a))
        | (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8)
        | (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16)
        | (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
}

#pragma pack(push, 1)
struct DdsPixelFormat {
    uint32_t size, flags, fourCC, rgbBitCount, rMask, gMask, bMask, aMask;
};
struct DdsHeader {
    uint32_t size, flags, height, width, pitchOrLinearSize, depth, mipMapCount;
    uint32_t reserved1[11];
    DdsPixelFormat ddspf;
    uint32_t caps, caps2, caps3, caps4, reserved2;
};
#pragma pack(pop)

// (sw x sh) の RGBA8 から (dw x dh) へ 2x2 ボックスフィルタで縮小する。
std::vector<uint8_t> Downsample(const std::vector<uint8_t>& src, int sw, int sh, int dw, int dh)
{
    std::vector<uint8_t> dst(static_cast<size_t>(dw) * dh * 4);
    for (int y = 0; y < dh; ++y) {
        for (int x = 0; x < dw; ++x) {
            const int sx0 = x * 2;
            const int sy0 = y * 2;
            const int sx1 = (sx0 + 1 < sw) ? sx0 + 1 : sx0;
            const int sy1 = (sy0 + 1 < sh) ? sy0 + 1 : sy0;
            for (int c = 0; c < 4; ++c) {
                const int a = src[(static_cast<size_t>(sy0) * sw + sx0) * 4 + c];
                const int b = src[(static_cast<size_t>(sy0) * sw + sx1) * 4 + c];
                const int cc = src[(static_cast<size_t>(sy1) * sw + sx0) * 4 + c];
                const int d = src[(static_cast<size_t>(sy1) * sw + sx1) * 4 + c];
                dst[(static_cast<size_t>(y) * dw + x) * 4 + c] =
                    static_cast<uint8_t>((a + b + cc + d + 2) / 4);
            }
        }
    }
    return dst;
}

// 1 つの mip (w x h の RGBA8) を BCn ブロックへ圧縮して out に追記する。
void CompressMip(const std::vector<uint8_t>& rgba, int w, int h, bool alpha,
                 std::vector<uint8_t>& out)
{
    const int bw = (w + 3) / 4;
    const int bh = (h + 3) / 4;
    const int blockBytes = alpha ? 16 : 8;
    for (int by = 0; by < bh; ++by) {
        for (int bx = 0; bx < bw; ++bx) {
            uint8_t block[64]; // 4x4 RGBA
            for (int j = 0; j < 4; ++j) {
                for (int i = 0; i < 4; ++i) {
                    int sx = bx * 4 + i;
                    int sy = by * 4 + j;
                    if (sx >= w) {
                        sx = w - 1; // 端はクランプ (非 4 の倍数対応)
                    }
                    if (sy >= h) {
                        sy = h - 1;
                    }
                    std::memcpy(&block[(j * 4 + i) * 4],
                                &rgba[(static_cast<size_t>(sy) * w + sx) * 4], 4);
                }
            }
            uint8_t dst[16];
            stb_compress_dxt_block(dst, block, alpha ? 1 : 0, STB_DXT_HIGHQUAL);
            out.insert(out.end(), dst, dst + blockBytes);
        }
    }
}

} // namespace

bool CookImageToDds(const std::wstring& srcImagePath, const std::wstring& dstDdsPath,
                    const CookOptions& opts)
{
    const std::string srcUtf8 = WideToUtf8(srcImagePath);
    int w = 0, h = 0, comp = 0;
    stbi_uc* pixels = stbi_load(srcUtf8.c_str(), &w, &h, &comp, 4);
    if (!pixels) {
        MYE_LOG_ERROR("[cook] load failed: %s (%s)", srcUtf8.c_str(), stbi_failure_reason());
        return false;
    }
    std::vector<uint8_t> mip0(pixels, pixels + static_cast<size_t>(w) * h * 4);
    const bool hasAlpha = (comp == 4); // 4ch 画像はアルファ有りとみなし BC3 を選ぶ
    stbi_image_free(pixels);

    // mip チェーンを生成しつつ各 mip を積む (compress=false は RGBA8 のまま、M39b)
    std::vector<uint8_t> blocks;
    std::vector<uint8_t> cur = std::move(mip0);
    int cw = w, ch = h;
    uint32_t mipCount = 0;
    for (;;) {
        if (opts.compress) {
            CompressMip(cur, cw, ch, hasAlpha, blocks);
        } else {
            blocks.insert(blocks.end(), cur.begin(), cur.end());
        }
        ++mipCount;
        if (!opts.generateMips || (cw == 1 && ch == 1)) {
            break;
        }
        const int nw = (cw > 1) ? cw / 2 : 1;
        const int nh = (ch > 1) ? ch / 2 : 1;
        cur = Downsample(cur, cw, ch, nw, nh);
        cw = nw;
        ch = nh;
    }

    DdsHeader hdr;
    std::memset(&hdr, 0, sizeof(hdr));
    hdr.size = 124;
    hdr.flags = kDdsdCaps | kDdsdHeight | kDdsdWidth | kDdsdPixelFormat;
    hdr.height = static_cast<uint32_t>(h);
    hdr.width = static_cast<uint32_t>(w);
    hdr.mipMapCount = mipCount;
    hdr.ddspf.size = 32;
    hdr.caps = kDdsCapsTexture;
    if (mipCount > 1) {
        hdr.flags |= kDdsdMipMapCount;
        hdr.caps |= kDdsCapsMipMap | kDdsCapsComplex;
    }
    if (opts.compress) {
        // トップ mip の linearSize (ブロック行 * ブロック列 * ブロックバイト)
        const uint32_t blockBytes = hasAlpha ? 16u : 8u;
        hdr.flags |= kDdsdLinearSize;
        hdr.pitchOrLinearSize =
            static_cast<uint32_t>((w + 3) / 4) * ((h + 3) / 4) * blockBytes;
        hdr.ddspf.flags = kDdpfFourCC;
        hdr.ddspf.fourCC =
            hasAlpha ? MakeFourCC('D', 'X', 'T', '5') : MakeFourCC('D', 'X', 'T', '1');
    } else {
        // RGBA8 非圧縮 (レガシーマスク形式 — GpuResources の 32bit RGB マスク読込と対、M38b)
        hdr.flags |= kDdsdPitch;
        hdr.pitchOrLinearSize = static_cast<uint32_t>(w) * 4;
        hdr.ddspf.flags = kDdpfRgb | kDdpfAlphaPixels;
        hdr.ddspf.rgbBitCount = 32;
        hdr.ddspf.rMask = 0x000000FFu;
        hdr.ddspf.gMask = 0x0000FF00u;
        hdr.ddspf.bMask = 0x00FF0000u;
        hdr.ddspf.aMask = 0xFF000000u;
    }

    std::ofstream f(dstDdsPath, std::ios::binary);
    if (!f) {
        MYE_LOG_ERROR("[cook] cannot write: %s", WideToUtf8(dstDdsPath).c_str());
        return false;
    }
    const uint32_t magic = kDdsMagic;
    f.write(reinterpret_cast<const char*>(&magic), 4);
    f.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    f.write(reinterpret_cast<const char*>(blocks.data()),
            static_cast<std::streamsize>(blocks.size()));

    MYE_LOG_INFO("[cook] %s -> %s (%dx%d, %s, %u mips, %zu KB)", srcUtf8.c_str(),
                 WideToUtf8(dstDdsPath).c_str(), w, h,
                 opts.compress ? (hasAlpha ? "BC3" : "BC1") : "RGBA8", mipCount,
                 (blocks.size() + sizeof(hdr) + 4) / 1024);
    return true;
}

} // namespace mye::TextureCook
