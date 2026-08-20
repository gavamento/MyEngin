#include "Engine/Renderer/ImageDiffSelfTest.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "Engine/Core/Log.h"
#include "Engine/Renderer/ImageDiff.h"
#include "Engine/Renderer/ImageWrite.h"

namespace fs = std::filesystem;

namespace mye {
namespace {

constexpr int kW = 16;
constexpr int kH = 12;

// 決定的な模様の RGBA8 画像 (エンジン RNG を使わずに済むよう座標から作る)
std::vector<uint8_t> MakePattern()
{
    std::vector<uint8_t> px(static_cast<size_t>(kW) * kH * 4);
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            uint8_t* p = px.data() + (static_cast<size_t>(y) * kW + x) * 4;
            p[0] = static_cast<uint8_t>(x * 16);
            p[1] = static_cast<uint8_t>(y * 20);
            p[2] = static_cast<uint8_t>((x + y) * 8);
            p[3] = 255;
        }
    }
    return px;
}

} // namespace

bool RunImageDiffSelfTest()
{
    MYE_LOG_INFO("==== ImageDiff (screenshot regression) self test ====");
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
    const fs::path root = fs::temp_directory_path(ec) / L"mye_imagediff_selftest";
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    const std::wstring aPath = (root / L"a.png").wstring();
    const std::wstring bPath = (root / L"b.png").wstring();
    const std::wstring smallPath = (root / L"small.png").wstring();
    const std::wstring heatPath = (root / L"heat.png").wstring();

    const std::vector<uint8_t> base = MakePattern();
    check(WritePngRGBA(aPath, base.data(), kW, kH, kW * 4), "test image A is written");

    // ---- 同一画像は差分ゼロ ----
    {
        check(WritePngRGBA(bPath, base.data(), kW, kH, kW * 4), "test image B (identical) is written");
        const ImageDiffResult r = CompareImageFiles(aPath, bPath);
        check(r.valid && r.error.empty(), "identical images compare successfully");
        check(r.width == kW && r.height == kH && r.totalPixels == kW * kH,
              "the reported geometry matches the source");
        check(r.maxChannelDiff == 0 && r.diffPixels == 0 && r.diffPixelsAny == 0,
              "identical images report zero difference");
        check(r.worstX == -1 && r.worstY == -1, "identical images report no worst pixel");
    }

    // ---- 1 画素 1 チャンネルだけ変えたら差分 1 ----
    // 「golden を 1 画素改竄したら回帰テストが赤になる」の機械証明
    {
        std::vector<uint8_t> one = base;
        const int px = 5, py = 7;
        uint8_t* p = one.data() + (static_cast<size_t>(py) * kW + px) * 4;
        const int before = p[1];
        p[1] = static_cast<uint8_t>(before + 3);
        check(WritePngRGBA(bPath, one.data(), kW, kH, kW * 4), "test image B (1 pixel off) is written");

        const ImageDiffResult r = CompareImageFiles(aPath, bPath);
        check(r.valid, "a one-pixel difference still compares successfully");
        check(r.diffPixels == 1 && r.diffPixelsAny == 1, "exactly one pixel is counted as different");
        check(r.maxChannelDiff == 3, "the max channel difference is the injected delta");
        check(r.worstX == px && r.worstY == py, "the worst pixel is located exactly");

        // tolerance は「数えるか」だけを変え、実測の maxChannelDiff は隠さない
        const ImageDiffResult t3 = CompareImageFiles(aPath, bPath, 3);
        check(t3.valid && t3.diffPixels == 0, "a tolerance of 3 swallows a delta of 3");
        check(t3.maxChannelDiff == 3 && t3.diffPixelsAny == 1,
              "the tolerance does not hide the measured delta");
        const ImageDiffResult t2 = CompareImageFiles(aPath, bPath, 2);
        check(t2.valid && t2.diffPixels == 1, "a tolerance of 2 still reports a delta of 3");

        // ---- 差分ヒート PNG ----
        fs::remove(heatPath, ec);
        const ImageDiffResult h = CompareImageFiles(aPath, bPath, 0, heatPath);
        check(h.valid && fs::exists(heatPath), "the heat map png is written");
        const ImageDiffResult heatVsA = CompareImageFiles(heatPath, aPath);
        check(heatVsA.valid && heatVsA.width == kW && heatVsA.height == kH,
              "the heat map keeps the source geometry");
    }

    // ---- 寸法違いはエラー (差分 0 と混同しない) ----
    {
        std::vector<uint8_t> small(static_cast<size_t>(kW) * (kH - 1) * 4, 0);
        check(WritePngRGBA(smallPath, small.data(), kW, kH - 1, kW * 4), "a smaller image is written");
        const ImageDiffResult r = CompareImageFiles(aPath, smallPath);
        check(!r.valid && !r.error.empty(), "a size mismatch is an error, not a zero difference");
        check(r.diffPixels == 0 && r.maxChannelDiff == 0,
              "an invalid comparison reports no misleading counts");
    }

    // ---- 読めないファイル ----
    {
        const ImageDiffResult r =
            CompareImageFiles(aPath, (root / L"does_not_exist.png").wstring());
        check(!r.valid && !r.error.empty(), "a missing file is an error");
    }

    fs::remove_all(root, ec);
    MYE_LOG_INFO("==== ImageDiff self test: %s ====", failCount == 0 ? "PASS" : "FAIL");
    return failCount == 0;
}

} // namespace mye
