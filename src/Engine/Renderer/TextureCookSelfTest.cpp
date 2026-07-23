#include "Engine/Renderer/TextureCookSelfTest.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

#include "Engine/Core/Log.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/TextureCook.h"

#include "stb/stb_image_write.h"

namespace fs = std::filesystem;

namespace mye {

namespace {

constexpr uint32_t MakeFourCC(char a, char b, char c, char d)
{
    return static_cast<uint32_t>(static_cast<uint8_t>(a))
        | (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8)
        | (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16)
        | (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
}

// クックが生成すべき mip 数とブロックデータ総バイトを独立計算 (検証の期待値)。
void ExpectedLayout(int w, int h, int blockBytes, uint32_t& mipCount, uint32_t& blockTotal)
{
    mipCount = 0;
    blockTotal = 0;
    int cw = w, ch = h;
    for (;;) {
        blockTotal += static_cast<uint32_t>((cw + 3) / 4) * ((ch + 3) / 4) * blockBytes;
        ++mipCount;
        if (cw == 1 && ch == 1) {
            break;
        }
        cw = (cw > 1) ? cw / 2 : 1;
        ch = (ch > 1) ? ch / 2 : 1;
    }
}

// 生成 DDS の magic/width/height/mipCount/fourCC + ブロックデータ長を検証する。
struct DdsProbe {
    bool ok = false;
    uint32_t width = 0, height = 0, mipCount = 0, fourCC = 0;
    uint32_t rgbBitCount = 0; // 非圧縮 (マスク形式) のみ非 0 (M39b)
    size_t blockBytes = 0;    // 総ブロックデータ長 (ファイル - 128)
};

DdsProbe ProbeDds(const std::wstring& path)
{
    DdsProbe p;
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return p;
    }
    const std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
    if (b.size() < 128) {
        return p;
    }
    uint32_t magic = 0;
    std::memcpy(&magic, b.data(), 4);
    if (magic != 0x20534444u) {
        return p;
    }
    // DDS_HEADER 内オフセット (magic 後): height@8, width@12, mipMapCount@24,
    // ddspf.fourCC@80, ddspf.rgbBitCount@84
    std::memcpy(&p.height, b.data() + 4 + 8, 4);
    std::memcpy(&p.width, b.data() + 4 + 12, 4);
    std::memcpy(&p.mipCount, b.data() + 4 + 24, 4);
    std::memcpy(&p.fourCC, b.data() + 4 + 80, 4);
    std::memcpy(&p.rgbBitCount, b.data() + 4 + 84, 4);
    p.blockBytes = b.size() - 128; // magic(4) + header(124)
    p.ok = true;
    return p;
}

} // namespace

bool RunTextureCookSelfTest()
{
    MYE_LOG_INFO("==== TextureCook self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    std::error_code ec;
    const fs::path root = fs::temp_directory_path(ec) / L"mye_texcook_selftest";
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    constexpr int W = 16, H = 16;

    // ---- RGBA (アルファ有) → BC3/DXT5 ----
    {
        std::vector<uint8_t> rgba(static_cast<size_t>(W) * H * 4);
        for (int i = 0; i < W * H; ++i) {
            rgba[i * 4 + 0] = static_cast<uint8_t>(i * 3);
            rgba[i * 4 + 1] = static_cast<uint8_t>(i * 5);
            rgba[i * 4 + 2] = static_cast<uint8_t>(i * 7);
            rgba[i * 4 + 3] = static_cast<uint8_t>(128); // アルファ < 255
        }
        const std::wstring png = (root / L"rgba.png").wstring();
        const std::wstring dds = (root / L"rgba.dds").wstring();
        stbi_write_png(WideToUtf8(png).c_str(), W, H, 4, rgba.data(), W * 4);
        check(TextureCook::CookImageToDds(png, dds), "cook RGBA png -> dds");
        const DdsProbe p = ProbeDds(dds);
        check(p.ok, "RGBA dds has valid magic/header");
        check(p.width == W && p.height == H, "RGBA dds dimensions 16x16");
        check(p.fourCC == MakeFourCC('D', 'X', 'T', '5'), "RGBA -> DXT5 (BC3)");
        uint32_t expMips = 0, expBytes = 0;
        ExpectedLayout(W, H, 16, expMips, expBytes);
        check(p.mipCount == expMips, "RGBA mip count == 5");
        check(p.blockBytes == expBytes, "RGBA block data length matches mip chain");
    }

    // ---- RGB (不透明) → BC1/DXT1 ----
    {
        std::vector<uint8_t> rgb(static_cast<size_t>(W) * H * 3);
        for (int i = 0; i < W * H; ++i) {
            rgb[i * 3 + 0] = static_cast<uint8_t>(i * 2);
            rgb[i * 3 + 1] = static_cast<uint8_t>(255 - i);
            rgb[i * 3 + 2] = static_cast<uint8_t>(i);
        }
        const std::wstring png = (root / L"rgb.png").wstring();
        const std::wstring dds = (root / L"rgb.dds").wstring();
        stbi_write_png(WideToUtf8(png).c_str(), W, H, 3, rgb.data(), W * 3);
        check(TextureCook::CookImageToDds(png, dds), "cook RGB png -> dds");
        const DdsProbe p = ProbeDds(dds);
        check(p.fourCC == MakeFourCC('D', 'X', 'T', '1'), "RGB -> DXT1 (BC1)");
        uint32_t expMips = 0, expBytes = 0;
        ExpectedLayout(W, H, 8, expMips, expBytes);
        check(p.mipCount == expMips, "RGB mip count == 5");
        check(p.blockBytes == expBytes, "RGB block data length matches mip chain");
    }

    // ---- CookOptions (M39b): RGBA8 非圧縮 + mips 無し ----
    {
        std::vector<uint8_t> rgba(static_cast<size_t>(W) * H * 4);
        for (int i = 0; i < W * H; ++i) {
            rgba[i * 4 + 0] = static_cast<uint8_t>(i);
            rgba[i * 4 + 1] = static_cast<uint8_t>(i * 2);
            rgba[i * 4 + 2] = static_cast<uint8_t>(i * 4);
            rgba[i * 4 + 3] = 255;
        }
        const std::wstring png = (root / L"raw.png").wstring();
        const std::wstring dds = (root / L"raw.dds").wstring();
        stbi_write_png(WideToUtf8(png).c_str(), W, H, 4, rgba.data(), W * 4);
        TextureCook::CookOptions co;
        co.generateMips = false;
        co.compress = false;
        check(TextureCook::CookImageToDds(png, dds, co), "cook uncompressed no-mips dds");
        const DdsProbe p = ProbeDds(dds);
        check(p.ok && p.width == W && p.height == H, "uncompressed dds header valid");
        check(p.fourCC == 0 && p.rgbBitCount == 32, "no fourCC + 32bit masks (RGBA8)");
        check(p.mipCount == 1, "generateMips=off -> single mip");
        check(p.blockBytes == static_cast<size_t>(W) * H * 4,
              "uncompressed data length == w*h*4");

        // mips 有り非圧縮: データ長 = 全 mip の RGBA8 合計
        const std::wstring dds2 = (root / L"raw_mips.dds").wstring();
        TextureCook::CookOptions co2;
        co2.compress = false;
        check(TextureCook::CookImageToDds(png, dds2, co2), "cook uncompressed with mips");
        const DdsProbe p2 = ProbeDds(dds2);
        size_t expTotal = 0;
        for (int cw = W, chh = H;;) {
            expTotal += static_cast<size_t>(cw) * chh * 4;
            if (cw == 1 && chh == 1) {
                break;
            }
            cw = (cw > 1) ? cw / 2 : 1;
            chh = (chh > 1) ? chh / 2 : 1;
        }
        check(p2.mipCount == 5 && p2.blockBytes == expTotal,
              "uncompressed mip chain length matches");
    }

    fs::remove_all(root, ec);

    if (failCount == 0) {
        MYE_LOG_INFO("==== TextureCook self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== TextureCook self test: %d FAILURE(S) ====", failCount);
    return false;
}

} // namespace mye
