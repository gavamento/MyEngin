//====================================================================================
//                          DeviceRecoverySelfTest.cpp
//  MyEngine/ 秋田蓮音                                                      10/07/2026
//                                          デバイス復旧 (連続消失の制限・参照数ゲート) の回帰テスト
//====================================================================================
#include "Engine/Engine/Loop/DeviceRecoverySelfTest.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Loop/DeviceRecovery.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Renderer/Device/GpuResources.h"
#include "Engine/Renderer/Device/GraphicsDevice.h"

namespace mye {

namespace {

using CheckFn = std::function<void(bool, const char*)>;

// 24bit BMP (stb_image が読める最小の画像)。rgbaOut に期待するデコード結果 (上の行から) を返す
std::vector<uint8_t> MakeBmp(int w, int h, uint8_t seed, std::vector<uint8_t>& rgbaOut)
{
    const auto put32 = [](std::vector<uint8_t>& v, uint32_t x) {
        for (int i = 0; i < 4; ++i) {
            v.push_back(static_cast<uint8_t>((x >> (8 * i)) & 0xFF));
        }
    };
    const auto put16 = [](std::vector<uint8_t>& v, uint16_t x) {
        v.push_back(static_cast<uint8_t>(x & 0xFF));
        v.push_back(static_cast<uint8_t>(x >> 8));
    };
    const uint32_t rowBytes = static_cast<uint32_t>(w) * 3; // w は 4 の倍数で呼ぶ (行パディング無し)
    const uint32_t dataBytes = rowBytes * static_cast<uint32_t>(h);
    std::vector<uint8_t> bmp = { 'B', 'M' };
    put32(bmp, 54 + dataBytes);
    put32(bmp, 0);
    put32(bmp, 54);
    put32(bmp, 40);
    put32(bmp, static_cast<uint32_t>(w));
    put32(bmp, static_cast<uint32_t>(h));
    put16(bmp, 1);
    put16(bmp, 24);
    put32(bmp, 0);
    put32(bmp, dataBytes);
    put32(bmp, 2835);
    put32(bmp, 2835);
    put32(bmp, 0);
    put32(bmp, 0);
    rgbaOut.assign(static_cast<size_t>(w) * h * 4, 255);
    for (int y = h - 1; y >= 0; --y) { // BMP は下の行から
        for (int x = 0; x < w; ++x) {
            const uint8_t r = static_cast<uint8_t>(seed + x * 40);
            const uint8_t g = static_cast<uint8_t>(seed + y * 40);
            const uint8_t b = static_cast<uint8_t>(seed + (x + y) * 20);
            bmp.push_back(b);
            bmp.push_back(g);
            bmp.push_back(r);
            uint8_t* px = &rgbaOut[(static_cast<size_t>(y) * w + x) * 4];
            px[0] = r;
            px[1] = g;
            px[2] = b;
        }
    }
    return bmp;
}

// テクスチャの mip0 (RGBA8 系) を読み戻す。desc はフォーマット・サイズ・mip 数の比較用
bool ReadTexture(GraphicsDevice& device, Texture* t, std::vector<uint8_t>& pixels,
                 D3D11_TEXTURE2D_DESC& desc)
{
    if (t == nullptr || !t->tex) {
        return false;
    }
    t->tex->GetDesc(&desc);
    D3D11_TEXTURE2D_DESC sd = desc;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device.Device()->CreateTexture2D(&sd, nullptr, staging.GetAddressOf()))) {
        return false;
    }
    device.Context()->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, t->tex.Get(), 0, nullptr);
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(device.Context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
        return false;
    }
    const size_t rowBytes = static_cast<size_t>(desc.Width) * 4;
    pixels.resize(rowBytes * desc.Height);
    for (UINT y = 0; y < desc.Height; ++y) {
        std::memcpy(&pixels[y * rowBytes], static_cast<const uint8_t*>(m.pData) + y * m.RowPitch,
                    rowBytes);
    }
    device.Context()->Unmap(staging.Get(), 0);
    return true;
}

bool ReadBuffer(GraphicsDevice& device, ID3D11Buffer* buf, std::vector<uint8_t>& bytes)
{
    if (buf == nullptr) {
        return false;
    }
    D3D11_BUFFER_DESC bd = {};
    buf->GetDesc(&bd);
    bd.Usage = D3D11_USAGE_STAGING;
    bd.BindFlags = 0;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    bd.MiscFlags = 0;
    Microsoft::WRL::ComPtr<ID3D11Buffer> staging;
    if (FAILED(device.Device()->CreateBuffer(&bd, nullptr, staging.GetAddressOf()))) {
        return false;
    }
    device.Context()->CopyResource(staging.Get(), buf);
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(device.Context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
        return false;
    }
    bytes.assign(static_cast<const uint8_t*>(m.pData), static_cast<const uint8_t*>(m.pData) + bd.ByteWidth);
    device.Context()->Unmap(staging.Get(), 0);
    return true;
}

struct TextureSnapshot {
    std::vector<uint8_t> pixels;
    D3D11_TEXTURE2D_DESC desc = {};
    bool ok = false;
    bool Same(const TextureSnapshot& o) const
    {
        return ok && o.ok && pixels == o.pixels && desc.Width == o.desc.Width
            && desc.Height == o.desc.Height && desc.MipLevels == o.desc.MipLevels
            && desc.Format == o.desc.Format;
    }
};

struct MeshSnapshot {
    std::vector<DirectX::XMFLOAT3> positions;
    std::vector<uint32_t> indices;
    std::vector<DirectX::XMFLOAT3> normals;
    std::vector<DirectX::XMFLOAT2> uvs;
    DirectX::XMFLOAT3 aabbMin = {};
    DirectX::XMFLOAT3 aabbMax = {};
    uint32_t indexCount = 0;
    std::vector<uint8_t> vb;
    std::vector<uint8_t> ib;
    bool gpuOk = false;
};

bool SameBits(const void* a, const void* b, size_t n)
{
    return std::memcmp(a, b, n) == 0;
}

// CPU 側はビット一致、GPU 側は読み戻した内容が一致、を調べる (positions 等が書き換わっていないことの確認)
bool SameMesh(const MeshSnapshot& a, const MeshSnapshot& b)
{
    const auto sameVec = [](const auto& x, const auto& y) {
        return x.size() == y.size()
            && (x.empty() || SameBits(x.data(), y.data(), x.size() * sizeof(x[0])));
    };
    return a.gpuOk && b.gpuOk && sameVec(a.positions, b.positions) && sameVec(a.indices, b.indices)
        && sameVec(a.normals, b.normals) && sameVec(a.uvs, b.uvs)
        && SameBits(&a.aabbMin, &b.aabbMin, sizeof(a.aabbMin))
        && SameBits(&a.aabbMax, &b.aabbMax, sizeof(a.aabbMax)) && a.indexCount == b.indexCount
        && a.vb == b.vb && a.ib == b.ib;
}

MeshSnapshot SnapshotMesh(GraphicsDevice& device, MeshLibrary& meshes, AssetID id)
{
    MeshSnapshot s;
    Mesh* m = meshes.Get(id);
    if (m == nullptr) {
        return s;
    }
    s.positions = m->positions;
    s.indices = m->indices;
    s.normals = m->normals;
    s.uvs = m->uvs;
    s.aabbMin = m->aabbMin;
    s.aabbMax = m->aabbMax;
    s.indexCount = m->indexCount;
    s.gpuOk = ReadBuffer(device, m->vb.Get(), s.vb) && ReadBuffer(device, m->ib.Get(), s.ib);
    return s;
}

TextureSnapshot SnapshotTexture(GraphicsDevice& device, TextureLibrary& textures, AssetID id)
{
    TextureSnapshot s;
    s.ok = ReadTexture(device, textures.Get(id), s.pixels, s.desc);
    return s;
}

// メッシュ・テクスチャ・マテリアルの GPU を手放して新デバイスで作り直し、AssetID・CPU 側・読み戻した
// GPU 内容が変わらないこと、World のハッシュが動かないことを確かめる
void CheckAssetRecovery(const CheckFn& check)
{
    GraphicsDevice device;
    if (!device.Init(true)) {
        check(false, "WARP device creation for the asset recovery test");
        return;
    }
    RenderResources res;
    res.Init(device);

    // ---- 素材: スキン付きメッシュ・非スキンメッシュ、各作成元のテクスチャ、マテリアル ----
    const MeshVertex skinned[3] = {
        { { 0, 0, 0 }, { 0, 0, 1 }, { 0, 0 }, { 1, 2, 0, 0 }, { 0.5f, 0.5f, 0, 0 } },
        { { 1, 0, 0 }, { 0, 0, 1 }, { 1, 0 }, { 3, 0, 0, 0 }, { 1.0f, 0, 0, 0 } },
        { { 0, 1, 0 }, { 0, 0, 1 }, { 0, 1 }, { 2, 3, 1, 0 }, { 0.25f, 0.25f, 0.5f, 0 } },
    };
    const MeshVertex plain[3] = {
        { { 0, 0, 0 }, { 0, 1, 0 }, { 0, 0 } },
        { { 2, 0, 0 }, { 0, 1, 0 }, { 1, 0 } },
        { { 0, 0, 2 }, { 0, 1, 0 }, { 0, 1 } },
    };
    const uint32_t tri[3] = { 0, 1, 2 };
    const AssetID skinMesh = res.meshes.Register("devrec_skin", skinned, tri);
    const AssetID plainMesh = res.meshes.Register("devrec_plain", plain, tri);
    const AssetID cube = res.meshes.Cube();

    std::vector<uint8_t> rgba(4 * 4 * 4);
    for (size_t i = 0; i < rgba.size(); ++i) {
        rgba[i] = static_cast<uint8_t>(i * 7 + 3);
    }
    std::vector<uint8_t> encodedExpect;
    std::vector<uint8_t> fileExpect;
    std::vector<uint8_t> asyncExpect;
    const std::vector<uint8_t> encodedBmp = MakeBmp(4, 4, 11, encodedExpect);
    const std::vector<uint8_t> fileBmp = MakeBmp(4, 4, 23, fileExpect);
    const std::vector<uint8_t> asyncBmp = MakeBmp(4, 4, 37, asyncExpect);
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "mye_device_recovery_selftest";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::wstring filePath = (dir / "devrec_file.bmp").wstring();
    const std::wstring asyncPath = (dir / "devrec_async.bmp").wstring();
    const auto writeFile = [](const std::wstring& path, const std::vector<uint8_t>& bytes) {
        std::ofstream f(std::filesystem::path(path), std::ios::binary);
        f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    };
    writeFile(filePath, fileBmp);
    writeFile(asyncPath, asyncBmp);

    const AssetID white = res.textures.White();
    const AssetID texRgba = res.textures.CreateFromRgba8("devrec_rgba", rgba.data(), 4, 4, true, true);
    const AssetID texRgbaNoMip =
        res.textures.CreateFromRgba8("devrec_rgba_nomip", rgba.data(), 4, 4, false, false);
    const AssetID texSolid = res.textures.CreateSolid("devrec_solid", 10, 20, 30, 40);
    const AssetID texEncoded =
        res.textures.CreateFromEncoded("devrec_encoded", encodedBmp.data(), encodedBmp.size(), false);
    const AssetID texFile = res.textures.LoadFile(filePath, false);
    // 読み込み中 (まだ PollAsyncLoads していない) のままデバイスを作り直す
    const AssetID texAsync = res.textures.RequestLoadFileAsync(asyncPath);

    Material mat;
    mat.texture = texRgba;
    mat.baseColor = { 0.25f, 0.5f, 0.75f, 1.0f };
    mat.roughness = 0.125f;
    const AssetID matId = res.materials.Register("devrec_mat", mat);

    check(!skinMesh.IsNull() && !plainMesh.IsNull() && !cube.IsNull() && !white.IsNull()
              && !texRgba.IsNull() && !texRgbaNoMip.IsNull() && !texSolid.IsNull()
              && !texEncoded.IsNull() && !texFile.IsNull() && !texAsync.IsNull() && !matId.IsNull(),
          "assets of every creation path are registered");

    // ---- World (ECS) のハッシュ。復旧は描画専用で、ここへ触れないことの確認 ----
    Scene scene;
    GameObject go = scene.CreateGameObjectTracked("RecoveryProbe");
    if (auto* mr = go.AddComponent<MeshRendererComponent>()) {
        mr->mesh = skinMesh;
        mr->material = matId;
    }
    scene.GetWorld().ApplyStructuralChanges();
    const uint64_t hashBefore = HashWorld(scene.GetWorld());

    // ---- 復旧前の観測 ----
    const AssetID meshIds[] = { skinMesh, plainMesh, cube };
    const AssetID texIds[] = { white, texRgba, texRgbaNoMip, texSolid, texEncoded, texFile, texAsync };
    MeshSnapshot meshBefore[3];
    TextureSnapshot texBefore[7];
    for (int i = 0; i < 3; ++i) {
        meshBefore[i] = SnapshotMesh(device, res.meshes, meshIds[i]);
    }
    for (int i = 0; i < 7; ++i) {
        texBefore[i] = SnapshotTexture(device, res.textures, texIds[i]);
    }
    const std::vector<AssetEntry> meshListBefore = res.meshes.Enumerate();
    const std::vector<AssetEntry> texListBefore = res.textures.Enumerate();
    const std::vector<AssetEntry> matListBefore = res.materials.Enumerate();
    const Material matBefore = *res.materials.Get(matId);
    Texture* const texPtrBefore = res.textures.Get(texRgba);
    Mesh* const meshPtrBefore = res.meshes.Get(skinMesh);
    check(meshBefore[0].gpuOk && meshBefore[1].gpuOk && meshBefore[2].gpuOk && texBefore[1].ok
              && texBefore[2].ok && texBefore[3].ok && texBefore[4].ok && texBefore[5].ok,
          "assets can be read back before the recovery");
    check(texBefore[4].pixels == encodedExpect && texBefore[5].pixels == fileExpect,
          "encoded / file textures decode to the expected pixels");

    // ---- 手放す → 旧デバイス参照数ゲート → 新デバイス → 作り直し ----
    res.ReleaseGpu();
    const DeviceRecycleResult recycled = RecycleDevice(device, 3, 0);
    check(recycled.status == DeviceRecycleStatus::Ok,
          "no asset keeps the old device after ReleaseGpu (gate passes)");
    check(res.RecreateGpu(device) == 0, "every mesh and texture is recreated");

    // ---- 復旧後の観測 ----
    bool idsKept = true;
    for (int i = 0; i < 3; ++i) {
        idsKept = idsKept && SameMesh(meshBefore[i], SnapshotMesh(device, res.meshes, meshIds[i]));
    }
    check(idsKept, "meshes keep AssetID, CPU data (bitwise) and GPU buffer contents");
    check(res.meshes.Get(skinMesh) == meshPtrBefore && res.textures.Get(texRgba) == texPtrBefore,
          "Get() pointers stay valid across the recovery");

    // 読み込み中だったものを除く 6 本は、読み戻した画素・フォーマット・mip 数が元と一致する
    bool texSame = true;
    for (int i = 0; i < 6; ++i) {
        texSame = texSame && texBefore[i].Same(SnapshotTexture(device, res.textures, texIds[i]));
    }
    check(texSame, "textures (white / RGBA8 srgb+mips / RGBA8 no mips / solid / encoded / file) read back identical");

    // 読み込み中: 復旧直後は白のプレースホルダ (有効な SRV)、完了後に正しい画像へ差し替わる
    {
        Texture* t = res.textures.Get(texAsync);
        check(t != nullptr && t->srv && t->width == 1, "pending texture is a valid placeholder right after recovery");
        res.textures.WaitForAsyncLoads(10000);
        const TextureSnapshot done = SnapshotTexture(device, res.textures, texAsync);
        check(done.ok && done.pixels == asyncExpect && done.desc.Width == 4,
              "pending texture becomes the decoded file after recovery");
    }

    check(res.meshes.Enumerate().size() == meshListBefore.size()
              && res.textures.Enumerate().size() == texListBefore.size()
              && res.materials.Enumerate().size() == matListBefore.size(),
          "asset lists keep their entries");
    {
        const Material* m = res.materials.Get(matId);
        check(m != nullptr && SameBits(m, &matBefore, sizeof(Material)), "material is unchanged");
    }
    check(HashWorld(scene.GetWorld()) == hashBefore, "World hash is identical before and after the recovery");

    // ---- ファイルが消えていたら白で続行 (復旧は止まらない) ----
    std::filesystem::remove(std::filesystem::path(filePath), ec);
    res.ReleaseGpu();
    check(RecycleDevice(device, 3, 0).status == DeviceRecycleStatus::Ok, "second recycle passes the gate");
    check(res.RecreateGpu(device) == 1, "a vanished source file is reported, everything else is recreated");
    {
        Texture* t = res.textures.Get(texFile);
        const Texture* w = res.textures.Get(white);
        check(t != nullptr && w != nullptr && t->srv && t->tex.Get() == w->tex.Get(),
              "a vanished source file falls back to the white placeholder");
        const TextureSnapshot enc = SnapshotTexture(device, res.textures, texEncoded);
        check(enc.ok && enc.pixels == encodedExpect, "the other textures still recreate correctly");
    }

    std::filesystem::remove_all(dir, ec);
    device.Shutdown();
}

// 旧デバイスの子オブジェクトとして握らせるための小さな定数バッファ
bool CreateProbeBuffer(GraphicsDevice& device, Microsoft::WRL::ComPtr<ID3D11Buffer>& out)
{
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 16;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    return SUCCEEDED(device.Device()->CreateBuffer(&bd, nullptr, out.GetAddressOf()));
}

} // namespace

bool RunDeviceRecoverySelfTest()
{
    MYE_LOG_INFO("==== DeviceRecovery self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    // ---- 連続消失の窓判定: 2 回までは復旧し、窓内の 3 回目で諦める。窓の外へ出れば数え直す ----
    {
        DeviceLostLimiter limiter;
        check(limiter.RecordLoss(0.0), "1st loss is recoverable");
        check(limiter.RecordLoss(10.0), "2nd loss within the window is recoverable");
        check(!limiter.RecordLoss(20.0), "3rd loss within the window is refused");
        check(limiter.RecordLoss(20.0 + kDeviceLostWindowSec + 1.0),
              "losses older than the window are forgotten");
    }

    // ---- 旧デバイスの子オブジェクトを握ったままだとゲート不合格 (復旧せず旧デバイスを残す) ----
    GraphicsDevice device;
    if (!device.Init(true)) {
        check(false, "WARP device creation");
        return false;
    }
    {
        Microsoft::WRL::ComPtr<ID3D11Buffer> leaked;
        check(CreateProbeBuffer(device, leaked), "probe buffer creation");
        ID3D11Device* oldDevice = device.Device();
        const DeviceRecycleResult r = RecycleDevice(device, 1, 0);
        check(r.status == DeviceRecycleStatus::StaleDeviceRefs && r.staleRefs > kExpectedExternalDeviceRefs,
              "a leaked child object fails the gate");
        check(device.Device() == oldDevice, "the old device is kept when the gate fails");
        leaked.Reset();
    }

    // ---- 誰も握っていなければ同種 (WARP) の新デバイスへ作り直せる ----
    {
        const DeviceRecycleResult r = RecycleDevice(device, 3, 0);
        check(r.status == DeviceRecycleStatus::Ok && r.attempts == 1, "clean recycle succeeds on the first attempt");
        check(device.Device() != nullptr && device.IsWarp(), "the recreated device keeps the WARP kind");
        Microsoft::WRL::ComPtr<ID3D11Buffer> fresh;
        check(CreateProbeBuffer(device, fresh), "the recreated device is usable");
    }
    device.Shutdown();

    CheckAssetRecovery(check);

    MYE_LOG_INFO("DeviceRecovery self test: %s", failCount == 0 ? "ALL PASS" : "FAILED");
    return failCount == 0;
}

} // namespace mye
