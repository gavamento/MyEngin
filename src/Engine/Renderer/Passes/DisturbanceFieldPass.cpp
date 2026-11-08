//====================================================================================
//                          DisturbanceFieldPass.cpp
//  MyEngin/ 秋田蓮音                                                       11/08/2026
//                                          かき乱しフィールド (M96) の 2D 層を GPU のテクスチャ配列へ転送する実装
//====================================================================================
#include "Engine/Renderer/Passes/DisturbanceFieldPass.h"

#include <algorithm>
#include <chrono>
#include <cstring>
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

bool DisturbanceFieldPass::EnsureArrays(GraphicsDevice& device, int32_t width, int32_t height)
{
    if (flowTex_ && texWidth_ >= width && texHeight_ >= height) {
        return true;
    }
    if (createFailed_ && failedWidth_ == width && failedHeight_ == height) {
        return false;
    }
    // 大きくなるときだけ作り直す。縮むときは今の配列を使い回す (層ごとの UV 範囲は CB が持つ)
    const int32_t w = (std::max)(width, texWidth_);
    const int32_t h = (std::max)(height, texHeight_);
    ID3D11Device* dev = device.Device();

    const auto create = [&](DXGI_FORMAT format, Microsoft::WRL::ComPtr<ID3D11Texture2D>& tex,
                            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& srv) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = static_cast<UINT>(w);
        td.Height = static_cast<UINT>(h);
        td.MipLevels = 1; // 霧は最細の格子をそのまま読む
        td.ArraySize = static_cast<UINT>(kDisturbanceMaxLayers);
        td.Format = format;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE; // GPU は書かない。CPU の sim が焼いた値を読むだけ
        if (FAILED(dev->CreateTexture2D(&td, nullptr, tex.ReleaseAndGetAddressOf()))) {
            return false;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = format;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        sd.Texture2DArray.MipLevels = 1;
        sd.Texture2DArray.ArraySize = static_cast<UINT>(kDisturbanceMaxLayers);
        return SUCCEEDED(dev->CreateShaderResourceView(tex.Get(), &sd, srv.ReleaseAndGetAddressOf()));
    };

    Microsoft::WRL::ComPtr<ID3D11Texture2D> flow;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> aux;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> flowSrv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> auxSrv;
    if (!create(DXGI_FORMAT_R16G16B16A16_FLOAT, flow, flowSrv) || !create(DXGI_FORMAT_R16G16_FLOAT, aux, auxSrv)) {
        MYE_LOG_ERROR("DisturbanceFieldPass: Texture2DArray (%dx%dx%d) を作れなかった (場の霧は描かれない)", w, h,
                      kDisturbanceMaxLayers);
        createFailed_ = true;
        failedWidth_ = width;
        failedHeight_ = height;
        return false;
    }
    flowTex_ = std::move(flow);
    auxTex_ = std::move(aux);
    flowSrv_ = std::move(flowSrv);
    auxSrv_ = std::move(auxSrv);
    texWidth_ = w;
    texHeight_ = h;
    createFailed_ = false;
    everUploaded_ = false; // 作り直したら通番に関係なく全層を流し直す
    return true;
}

bool DisturbanceFieldPass::Upload(GraphicsDevice& device, const DisturbanceFieldUpload& src)
{
    uploadMs_ = 0.0f;
    gpu_ = DisturbanceFieldGpu{};
    const int32_t count = (std::min)(src.layerCount, static_cast<int32_t>(kDisturbanceMaxLayers));
    if (count <= 0) {
        return false;
    }

    // テクスチャを読む層の最大寸法で配列を確保する (全層が定数なら配列は要らない)
    int32_t maxW = 0;
    int32_t maxH = 0;
    bool anyState = false;
    for (int32_t i = 0; i < count; ++i) {
        const DisturbanceLayerUpload& l = src.layers[i];
        if (l.hasState && l.cellsX > 0 && l.cellsZ > 0) {
            anyState = true;
            maxW = (std::max)(maxW, l.cellsX);
            maxH = (std::max)(maxH, l.cellsZ);
        }
    }
    if (anyState && !EnsureArrays(device, maxW, maxH)) {
        return false;
    }

    // ---- CB は毎回組み直す (数十バイトで、編集中の設定変更をすぐ反映するため) ----
    DisturbanceFieldCB cb;
    cb.header[0] = static_cast<uint32_t>(count);
    for (int32_t i = 0; i < count; ++i) {
        const DisturbanceLayerUpload& l = src.layers[i];
        DisturbanceLayerCB& c = cb.layers[i];
        const bool readTex = l.hasState && anyState && flowTex_;
        c.originSize[0] = static_cast<float>(l.originCellX);
        c.originSize[1] = static_cast<float>(l.originCellZ);
        c.originSize[2] = l.cellSize;
        c.originSize[3] = l.baseY;
        c.extent[0] = static_cast<float>(l.cellsX);
        c.extent[1] = static_cast<float>(l.cellsZ);
        c.extent[2] = l.thickness;
        c.extent[3] = readTex ? 1.0f : 0.0f;
        c.look[0] = l.fogDensity;
        c.look[1] = l.fogBrightness;
        c.look[2] = l.restDensity;
        c.look[3] = l.hasDensity ? 1.0f : 0.0f;
        c.fogColor[0] = l.fogColor[0];
        c.fogColor[1] = l.fogColor[1];
        c.fogColor[2] = l.fogColor[2];
        c.texScale[0] = texWidth_ > 0 ? 1.0f / static_cast<float>(texWidth_) : 0.0f;
        c.texScale[1] = texHeight_ > 0 ? 1.0f / static_cast<float>(texHeight_) : 0.0f;
    }
    gpu_.cb = cb;
    if (anyState) {
        gpu_.flowSRV = flowSrv_.Get();
        gpu_.auxSRV = auxSrv_.Get();
    }

    // ---- 転送。通番と層の構成が同じなら省く ----
    LayerKey keys[kDisturbanceMaxLayers];
    for (int32_t i = 0; i < count; ++i) {
        const DisturbanceLayerUpload& l = src.layers[i];
        keys[i] = { l.entityIndex, l.entityGeneration, l.cellsX, l.cellsZ, l.hasState };
    }
    bool same = everUploaded_ && uploadedSerial_ == src.serial && uploadedCount_ == count;
    for (int32_t i = 0; same && i < count; ++i) {
        same = keys[i] == uploadedKeys_[i];
    }
    if (same || !anyState) {
        // 状態を読む層が無ければ転送するものが無い。鍵は更新して「次に状態が現れたら流す」ようにする
        if (!anyState) {
            everUploaded_ = false;
        }
        return true;
    }

    const auto t0 = std::chrono::steady_clock::now();
    ID3D11DeviceContext* dc = device.Context();
    std::vector<uint16_t> flowHalf;
    std::vector<uint16_t> auxHalf;
    for (int32_t i = 0; i < count; ++i) {
        const DisturbanceLayerUpload& l = src.layers[i];
        if (!l.hasState || l.cellsX <= 0 || l.cellsZ <= 0) {
            continue;
        }
        const size_t n = static_cast<size_t>(l.cellsX) * static_cast<size_t>(l.cellsZ);
        flowHalf.resize(n * 4);
        auxHalf.resize(n * 2);
        for (size_t k = 0; k < n; ++k) {
            flowHalf[k * 4 + 0] = ToHalf(At(l.flowX, k));
            flowHalf[k * 4 + 1] = ToHalf(At(l.flowZ, k));
            flowHalf[k * 4 + 2] = ToHalf(l.hasDensity ? At(l.density, k) : 0.0f);
            flowHalf[k * 4 + 3] = ToHalf(At(l.trail, k));
            auxHalf[k * 2 + 0] = ToHalf(At(l.ripple, k));
            auxHalf[k * 2 + 1] = ToHalf(At(l.baseOffset, k));
        }
        // 層 i の左上 (0,0) から cellsX x cellsZ だけ書く。RowPitch は 1 行のバイト数
        D3D11_BOX box = {};
        box.right = static_cast<UINT>(l.cellsX);
        box.bottom = static_cast<UINT>(l.cellsZ);
        box.back = 1;
        const UINT sub = D3D11CalcSubresource(0, static_cast<UINT>(i), 1);
        dc->UpdateSubresource(flowTex_.Get(), sub, &box, flowHalf.data(), static_cast<UINT>(l.cellsX) * 8u, 0);
        dc->UpdateSubresource(auxTex_.Get(), sub, &box, auxHalf.data(), static_cast<UINT>(l.cellsX) * 4u, 0);
    }
    const auto t1 = std::chrono::steady_clock::now();
    uploadMs_ = static_cast<float>(std::chrono::duration<double, std::milli>(t1 - t0).count());
    for (int32_t i = 0; i < count; ++i) {
        uploadedKeys_[i] = keys[i];
    }
    uploadedCount_ = count;
    uploadedSerial_ = src.serial;
    everUploaded_ = true;
    return true;
}

void DisturbanceFieldPass::Release()
{
    flowSrv_.Reset();
    auxSrv_.Reset();
    flowTex_.Reset();
    auxTex_.Reset();
    texWidth_ = 0;
    texHeight_ = 0;
    createFailed_ = false;
    uploadedCount_ = 0;
    uploadedSerial_ = 0;
    everUploaded_ = false;
    gpu_ = DisturbanceFieldGpu{};
    uploadMs_ = 0.0f;
}

} // namespace mye
