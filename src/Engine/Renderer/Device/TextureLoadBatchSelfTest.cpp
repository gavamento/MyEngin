//====================================================================================
//                          TextureLoadBatchSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                     11/24/2026
//                                          テクスチャのまとめ読み (並列デコード) の自己テスト
//====================================================================================
#include "Engine/Renderer/Device/TextureLoadBatchSelfTest.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <process.h>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Renderer/Device/GpuResources.h"
#include "Engine/Renderer/Device/GraphicsDevice.h"
#include "Engine/Renderer/Texture/ImageWrite.h"
#include "Engine/Renderer/Texture/TextureCook.h"

namespace mye {

namespace {

namespace fs = std::filesystem;

constexpr int kImageSize = 8;   // テスト画像の一辺 (ピクセル)
constexpr int kImageCount = 6;  // 同期とまとめ読みで読み比べる枚数
constexpr int kWaitTimeoutMs = 10000;

// seed ごとに中身の違う不透明な RGBA8 画像を PNG で書く
bool WriteTestPng(const fs::path& path, int seed)
{
    std::vector<uint8_t> pixels(static_cast<size_t>(kImageSize) * kImageSize * 4);
    for (size_t i = 0; i < pixels.size(); ++i) {
        pixels[i] = (i % 4 == 3) ? 255 : static_cast<uint8_t>(i * 5 + static_cast<size_t>(seed) * 31);
    }
    return WritePngRGBA(path.wstring(), pixels.data(), kImageSize, kImageSize, kImageSize * 4);
}

// GPU 上の見た目を決める項目と mip0 の画素
struct TextureSnapshot {
    bool ok = false;
    int width = 0;
    int height = 0;
    bool srgb = false;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT mipLevels = 0;
    std::vector<uint8_t> pixels;
};

TextureSnapshot Snapshot(GraphicsDevice& device, TextureLibrary& textures, AssetID id)
{
    TextureSnapshot s;
    Texture* t = textures.Get(id);
    if (t == nullptr || !t->tex) {
        return s;
    }
    s.width = t->width;
    s.height = t->height;
    s.srgb = t->srgb;
    D3D11_TEXTURE2D_DESC desc = {};
    t->tex->GetDesc(&desc);
    s.format = desc.Format;
    s.mipLevels = desc.MipLevels;

    D3D11_TEXTURE2D_DESC staging = desc;
    staging.MipLevels = 1;
    staging.ArraySize = 1;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging.MiscFlags = 0;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> copy;
    if (FAILED(device.Device()->CreateTexture2D(&staging, nullptr, copy.GetAddressOf()))) {
        return s;
    }
    device.Context()->CopySubresourceRegion(copy.Get(), 0, 0, 0, 0, t->tex.Get(), 0, nullptr);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(device.Context()->Map(copy.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return s;
    }
    const size_t rowBytes = static_cast<size_t>(desc.Width) * 4;
    s.pixels.resize(rowBytes * desc.Height);
    for (UINT y = 0; y < desc.Height; ++y) {
        std::memcpy(&s.pixels[y * rowBytes], static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch,
                    rowBytes);
    }
    device.Context()->Unmap(copy.Get(), 0);
    s.ok = true;
    return s;
}

bool SameTexture(const TextureSnapshot& a, const TextureSnapshot& b)
{
    return a.ok && b.ok && a.width == b.width && a.height == b.height && a.srgb == b.srgb
        && a.format == b.format && a.mipLevels == b.mipLevels && a.pixels == b.pixels;
}

} // namespace

bool RunTextureLoadBatchSelfTest()
{
    MYE_LOG_INFO("==== TextureLoadBatch self test ====");
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
    // 並列に走る別プロセスの同じテストと混ざらないよう、PID を入れる
    const fs::path tempRoot =
        fs::temp_directory_path(ec) / (L"mye_texture_batch_selftest_" + std::to_wstring(_getpid()));
    fs::remove_all(tempRoot, ec);
    fs::create_directories(tempRoot, ec);

    std::vector<std::wstring> paths;
    bool filesOk = true;
    for (int i = 0; i < kImageCount; ++i) {
        const fs::path p = tempRoot / (L"image_" + std::to_wstring(i) + L".png");
        filesOk = WriteTestPng(p, i) && filesOk;
        paths.push_back(p.wstring());
    }
    check(filesOk, "setup: test images are written");

    const int workers = TextureLibrary::DecodeWorkerCount();
    check(workers >= 1 && workers <= 8, "workers: the decode worker count is within 1..8");

    // ---- (1) デバイスなしのまとめ読みは、同期ロードと同じく空の ID ----
    {
        TextureLibrary headless;
        AssetID id;
        {
            TextureLoadBatch batch(headless);
            id = headless.LoadFile(paths[0], true);
        }
        check(id.IsNull() && headless.PendingAsyncCount() == 0,
              "headless: a batch without a device returns a null id and queues nothing");
    }

    GraphicsDevice device;
    if (!device.Init(true)) {
        check(false, "WARP device creation for the texture batch test");
        fs::remove_all(tempRoot, ec);
        MYE_LOG_ERROR("==== TextureLoadBatch self test: %d FAILED ====", failCount);
        return false;
    }

    // ---- (2) まとめ読みの結果は同期ロードと一致する ----
    {
        TextureLibrary syncLib;
        syncLib.Init(device);
        TextureLibrary batchLib;
        batchLib.Init(device);

        std::vector<AssetID> syncIds;
        for (int i = 0; i < kImageCount; ++i) {
            syncIds.push_back(syncLib.LoadFile(paths[i], (i % 2) == 0));
        }

        std::vector<AssetID> batchIds;
        bool allPendingInside = true;
        AssetID repeated;
        {
            TextureLoadBatch batch(batchLib);
            for (int i = 0; i < kImageCount; ++i) {
                batchIds.push_back(batchLib.LoadFile(paths[i], (i % 2) == 0));
                allPendingInside = allPendingInside && batchLib.IsPending(batchIds.back());
            }
            repeated = batchLib.LoadFile(paths[0], true); // 同じファイルの 2 回目
            check(batchLib.PendingAsyncCount() == kImageCount,
                  "batch: every image is queued once, and a repeated request adds nothing");
        }
        check(allPendingInside, "batch: LoadFile inside the batch returns before the texture is published");
        check(batchLib.PendingAsyncCount() == 0, "batch: nothing is pending after the batch ends");
        check(repeated.value == batchIds[0].value, "batch: a repeated request returns the same id");

        bool same = true;
        for (int i = 0; i < kImageCount; ++i) {
            same = same && syncIds[i].value == batchIds[i].value && !batchIds[i].IsNull()
                && SameTexture(Snapshot(device, syncLib, syncIds[i]), Snapshot(device, batchLib, batchIds[i]));
        }
        check(same, "batch: ids, size, sRGB flag, format, mip count and pixels match the synchronous load");

        const TextureSnapshot srgbOn = Snapshot(device, batchLib, batchIds[0]);
        const TextureSnapshot srgbOff = Snapshot(device, batchLib, batchIds[1]);
        check(srgbOn.srgb && srgbOn.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && !srgbOff.srgb
                  && srgbOff.format == DXGI_FORMAT_R8G8B8A8_UNORM,
              "batch: the sRGB hint of each LoadFile is honored");
    }

    // ---- (3) 入れ子は外側で待つ / 対象外のファイルは同期のまま ----
    {
        TextureLibrary lib;
        lib.Init(device);
        const std::wstring ddsPath = (tempRoot / L"cooked.dds").wstring();
        const bool cooked = TextureCook::CookImageToDds(paths[1], ddsPath);
        AssetID inner;
        AssetID dds;
        AssetID missing;
        bool pendingAfterInner = false;
        {
            TextureLoadBatch outer(lib);
            {
                TextureLoadBatch nested(lib);
                inner = lib.LoadFile(paths[2], false);
            }
            pendingAfterInner = lib.IsPending(inner);
            dds = lib.LoadFile(ddsPath, false);
            missing = lib.LoadFile((tempRoot / L"no_such_file.png").wstring(), false);
            const Texture* ddsTexture = lib.Get(dds);
            check(cooked && !dds.IsNull() && !lib.IsPending(dds) && ddsTexture != nullptr
                      && ddsTexture->width == kImageSize,
                  "batch: a DDS file is loaded synchronously inside the batch");
            check(missing.IsNull(), "batch: a missing file returns a null id inside the batch");
        }
        check(pendingAfterInner, "nested: the inner batch does not wait");
        const Texture* innerTexture = lib.Get(inner);
        check(!lib.IsPending(inner) && innerTexture != nullptr && innerTexture->width == kImageSize,
              "nested: the outermost batch waits for everything");
    }

    // ---- (4) 範囲外の非同期ロード: 指定なしは今までどおり非 sRGB、指定ありは sRGB ----
    {
        TextureLibrary lib;
        lib.Init(device);
        const AssetID plain = lib.RequestLoadFileAsync(paths[3]);
        const AssetID hinted = lib.RequestLoadFileAsync(paths[4], true);
        lib.WaitForAsyncLoads(kWaitTimeoutMs);
        const TextureSnapshot plainShot = Snapshot(device, lib, plain);
        const TextureSnapshot hintedShot = Snapshot(device, lib, hinted);
        check(plainShot.ok && !plainShot.srgb && plainShot.format == DXGI_FORMAT_R8G8B8A8_UNORM,
              "async: a request without a hint stays non-sRGB");
        check(hintedShot.ok && hintedShot.srgb && hintedShot.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
              "async: a request with the sRGB hint is created as sRGB");
    }

    // ---- (5) 何も回さなかったまとめ読みは、待たずに終わる ----
    {
        TextureLibrary lib;
        lib.Init(device);
        {
            TextureLoadBatch batch(lib);
        }
        check(lib.PendingAsyncCount() == 0, "batch: an empty batch ends cleanly");
    }

    fs::remove_all(tempRoot, ec);

    if (failCount == 0) {
        MYE_LOG_INFO("==== TextureLoadBatch self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== TextureLoadBatch self test: %d FAILED ====", failCount);
    return false;
}

} // namespace mye
