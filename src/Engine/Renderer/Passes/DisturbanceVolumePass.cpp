//====================================================================================
//                          DisturbanceVolumePass.cpp
//  MyEngin/ 秋田蓮音                                                       11/09/2026
//                                          かき乱しフィールド (M96) の Volume (3D) を GPU の Texture3D へ転送する実装
//====================================================================================
#include "Engine/Renderer/Passes/DisturbanceVolumePass.h"

#include <algorithm>
#include <chrono>
#include <vector>

#include <DirectXPackedVector.h>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Renderer/Device/GraphicsDevice.h"

namespace mye {

namespace {

uint16_t ToHalf(float v)
{
    return DirectX::PackedVector::XMConvertFloatToHalf(v);
}

float At(const float* a, size_t i)
{
    return a != nullptr ? a[i] : 0.0f;
}

} // namespace

bool DisturbanceVolumePass::EnsureTexture(GraphicsDevice& device, Slot& slot, int32_t width, int32_t height, int32_t depth)
{
    if (slot.tex && slot.width == width && slot.height == height && slot.depth == depth) {
        return true;
    }
    if (slot.createFailed && slot.width == width && slot.height == height && slot.depth == depth) {
        return false;
    }
    slot.tex.Reset();
    slot.srv.Reset();
    slot.width = width;
    slot.height = height;
    slot.depth = depth;
    slot.uploaded = false;
    D3D11_TEXTURE3D_DESC td = {};
    td.Width = static_cast<UINT>(width);
    td.Height = static_cast<UINT>(height);
    td.Depth = static_cast<UINT>(depth);
    td.MipLevels = 1; // 霧は最細の格子をそのまま読む
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE; // GPU は書かない。CPU の sim が焼いた値を読むだけ
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = td.Format;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
    sd.Texture3D.MipLevels = 1;
    if (FAILED(device.Device()->CreateTexture3D(&td, nullptr, slot.tex.ReleaseAndGetAddressOf()))
        || FAILED(device.Device()->CreateShaderResourceView(slot.tex.Get(), &sd, slot.srv.ReleaseAndGetAddressOf()))) {
        MYE_LOG_ERROR("DisturbanceVolumePass: Texture3D (%dx%dx%d) を作れなかった (この Volume の霧は描かれない)", width,
                      height, depth);
        slot.tex.Reset();
        slot.srv.Reset();
        slot.createFailed = true;
        return false;
    }
    slot.createFailed = false;
    return true;
}

bool DisturbanceVolumePass::Upload(GraphicsDevice& device, const DisturbanceVolumeUpload& src)
{
    uploadMs_ = 0.0f;
    gpu_ = DisturbanceVolumeGpu{};
    const int32_t count = (std::min)(src.count, static_cast<int32_t>(kDisturbanceMaxVolumes));
    if (count <= 0) {
        return false;
    }

    const auto t0 = std::chrono::steady_clock::now();
    bool transferred = false;
    std::vector<uint16_t> half;
    gpu_.cb.header[0] = static_cast<uint32_t>(count);
    for (int32_t i = 0; i < count; ++i) {
        const DisturbanceVolumeEntryUpload& v = src.volumes[i];
        Slot& slot = slots_[i];
        const bool wantTexture = v.hasState && v.cellsX > 0 && v.cellsY > 0 && v.cellsZ > 0;
        // テクスチャの並びは配列の並びそのまま: 幅 = x、高さ = z、奥行き = y
        const bool readTex = wantTexture && EnsureTexture(device, slot, v.cellsX, v.cellsZ, v.cellsY);
        if (readTex) {
            const bool same = slot.uploaded && slot.serial == src.serial && slot.entityIndex == v.entityIndex
                              && slot.entityGeneration == v.entityGeneration;
            if (!same) {
                const size_t n = static_cast<size_t>(v.cellsX) * static_cast<size_t>(v.cellsY) * static_cast<size_t>(v.cellsZ);
                half.resize(n * 4);
                for (size_t k = 0; k < n; ++k) {
                    half[k * 4 + 0] = ToHalf(At(v.flowX, k));
                    half[k * 4 + 1] = ToHalf(At(v.flowY, k));
                    half[k * 4 + 2] = ToHalf(At(v.flowZ, k));
                    half[k * 4 + 3] = ToHalf(v.hasDensity ? At(v.density, k) : 0.0f);
                }
                const UINT rowPitch = static_cast<UINT>(v.cellsX) * 8u;
                device.Context()->UpdateSubresource(slot.tex.Get(), 0, nullptr, half.data(), rowPitch,
                                                    rowPitch * static_cast<UINT>(v.cellsZ));
                slot.uploaded = true;
                slot.serial = src.serial;
                slot.entityIndex = v.entityIndex;
                slot.entityGeneration = v.entityGeneration;
                transferred = true;
            }
            gpu_.srv[i] = slot.srv.Get();
        } else {
            slot.uploaded = false; // 次に状態が現れたら流し直す
        }

        DisturbanceVolumeEntryCB& c = gpu_.cb.volumes[i];
        c.originSize[0] = static_cast<float>(v.originCell[0]);
        c.originSize[1] = static_cast<float>(v.originCell[1]);
        c.originSize[2] = static_cast<float>(v.originCell[2]);
        c.originSize[3] = v.cellSize;
        c.extent[0] = static_cast<float>(v.cellsX);
        c.extent[1] = static_cast<float>(v.cellsY);
        c.extent[2] = static_cast<float>(v.cellsZ);
        c.extent[3] = readTex ? 1.0f : 0.0f;
        c.look[0] = v.fogDensity;
        c.look[1] = v.fogBrightness;
        c.look[2] = v.restDensity;
        c.look[3] = v.hasDensity ? 1.0f : 0.0f;
        c.fogColor[0] = v.fogColor[0];
        c.fogColor[1] = v.fogColor[1];
        c.fogColor[2] = v.fogColor[2];
    }
    // 使わなくなった末尾のテクスチャは持ち続けない (Volume が減ったときの VRAM)
    for (int32_t i = count; i < kDisturbanceMaxVolumes; ++i) {
        slots_[i] = Slot{};
    }
    if (transferred) {
        const auto t1 = std::chrono::steady_clock::now();
        uploadMs_ = static_cast<float>(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    return true;
}

void DisturbanceVolumePass::Release()
{
    for (Slot& s : slots_) {
        s = Slot{};
    }
    gpu_ = DisturbanceVolumeGpu{};
    uploadMs_ = 0.0f;
}

} // namespace mye
