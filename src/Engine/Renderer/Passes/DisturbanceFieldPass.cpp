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
#include "Engine/Renderer/Device/GpuBufferUtil.h"
#include "Engine/Renderer/Device/GraphicsDevice.h"
#include "Engine/Renderer/Pipeline/RenderTypes.h"

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

int SelectGroundFogLights(const SceneLightData& lights, const DisturbanceFieldCB& layers, uint32_t localLightMask,
                          GroundFogLightsCB& out)
{
    out = GroundFogLightsCB{};
    const uint32_t layerCount = (std::min)(layers.header[0], static_cast<uint32_t>(kDisturbanceMaxLayers));
    // 照らす層の板 (AABB)。ground_fog.hlsli の交差判定と同じ範囲
    float boxLo[kDisturbanceMaxLayers][3] = {};
    float boxHi[kDisturbanceMaxLayers][3] = {};
    uint32_t mask = 0;
    for (uint32_t i = 0; i < layerCount; ++i) {
        const DisturbanceLayerCB& l = layers.layers[i];
        if ((localLightMask & (1u << i)) == 0 || l.look[3] < 0.5f || l.look[0] <= 0.0f) {
            continue;
        }
        mask |= 1u << i;
        const float cs = l.originSize[2];
        boxLo[i][0] = l.originSize[0] * cs;
        boxHi[i][0] = (l.originSize[0] + l.extent[0]) * cs;
        boxLo[i][1] = l.originSize[3] + l.texScale[2];
        boxHi[i][1] = l.originSize[3] + l.texScale[3] + l.extent[2];
        boxLo[i][2] = l.originSize[1] * cs;
        boxHi[i][2] = (l.originSize[1] + l.extent[1]) * cs;
    }
    if (mask == 0) {
        return 0;
    }
    out.header[1] = mask;
    int total = 0;
    const int count = (std::min)(lights.count, static_cast<int>(kMaxLights));
    for (int n = 0; n < count; ++n) {
        const GpuLight& g = lights.lights[n];
        if (g.type == 0 || !(g.range > 0.0f)) {
            continue; // 平行光は fogSunLight の担当
        }
        const float c[3] = { g.position.x, g.position.y, g.position.z };
        bool reaches = false;
        for (uint32_t i = 0; i < layerCount && !reaches; ++i) {
            if ((mask & (1u << i)) == 0) {
                continue;
            }
            float d2 = 0.0f;
            for (int a = 0; a < 3; ++a) {
                const float q = (std::max)(boxLo[i][a], (std::min)(c[a], boxHi[i][a]));
                d2 += (c[a] - q) * (c[a] - q);
            }
            reaches = d2 <= g.range * g.range;
        }
        if (!reaches) {
            continue;
        }
        if (total < kGroundFogMaxLights) {
            GroundFogLightCB& o = out.lights[total];
            o.posRange[0] = g.position.x;
            o.posRange[1] = g.position.y;
            o.posRange[2] = g.position.z;
            o.posRange[3] = g.range;
            o.colorKind[0] = g.color.x * g.intensity;
            o.colorKind[1] = g.color.y * g.intensity;
            o.colorKind[2] = g.color.z * g.intensity;
            o.colorKind[3] = g.type == 2 ? 2.0f : 1.0f;
            o.dirInner[0] = g.direction.x;
            o.dirInner[1] = g.direction.y;
            o.dirInner[2] = g.direction.z;
            o.dirInner[3] = g.cosInner;
            o.outerPad[0] = g.cosOuter;
        }
        ++total;
    }
    out.header[0] = static_cast<uint32_t>((std::min)(total, static_cast<int>(kGroundFogMaxLights)));
    return total;
}

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

    // ---- 通番と層の構成が同じなら転送を省く (CB は下で毎回組む) ----
    LayerKey keys[kDisturbanceMaxLayers];
    for (int32_t i = 0; i < count; ++i) {
        const DisturbanceLayerUpload& l = src.layers[i];
        keys[i] = { l.entityIndex, l.entityGeneration, l.cellsX, l.cellsZ, l.hasState };
    }
    bool same = everUploaded_ && uploadedSerial_ == src.serial && uploadedCount_ == count;
    for (int32_t i = 0; same && i < count; ++i) {
        same = keys[i] == uploadedKeys_[i];
    }

    // ---- CB は毎回組み直す (数十バイトで、編集中の設定変更をすぐ反映するため) ----
    DisturbanceFieldCB cb;
    cb.header[0] = static_cast<uint32_t>(count);
    uint32_t localLightMask = 0;
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
        c.fogColor[3] = l.fogSunLight ? 1.0f : 0.0f;
        if (l.fogLocalLights) {
            localLightMask |= 1u << i;
        }
        c.texScale[0] = texWidth_ > 0 ? 1.0f / static_cast<float>(texWidth_) : 0.0f;
        c.texScale[1] = texHeight_ > 0 ? 1.0f / static_cast<float>(texHeight_) : 0.0f;
        if (!same) {
            // 状態を流し直すときだけ数え直す (描画は 60 Hz の sim より何倍も速く回る)
            float lo = 0.0f;
            float hi = 0.0f;
            if (readTex && l.baseOffset != nullptr) {
                const size_t n = static_cast<size_t>(l.cellsX) * static_cast<size_t>(l.cellsZ);
                lo = hi = l.baseOffset[0];
                for (size_t k = 1; k < n; ++k) {
                    lo = (std::min)(lo, l.baseOffset[k]);
                    hi = (std::max)(hi, l.baseOffset[k]);
                }
            }
            offsetRange_[i][0] = lo;
            offsetRange_[i][1] = hi;
        }
        c.texScale[2] = offsetRange_[i][0];
        c.texScale[3] = offsetRange_[i][1];
        // 霧のノイズ。強さ 0 なら CB も 0 = シェーダが読まない (従来の絵とビット一致)
        c.noise = PackDisturbanceNoise(l.noise, src.noiseTime);
        gpu_.noiseMapSRV[i] = c.noise.c[3] > 0.5f ? l.noise.mapSRV : nullptr;
    }
    gpu_.cb = cb;
    gpu_.localLightMask = localLightMask;
    if (anyState) {
        gpu_.flowSRV = flowSrv_.Get();
        gpu_.auxSRV = auxSrv_.Get();
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

bool DisturbanceFieldPass::CreateGroundFogResources(GraphicsDevice& device)
{
    if (groundFogLayersCb_ && groundFogViewCb_ && groundFogLightsCb_ && groundFogSampler_ && noiseSampler_) {
        return true;
    }
    ID3D11Device* dev = device.Device();
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    // 霧のノイズのマップは繰り返して敷く (wrap)。遠くで模様がちらつかないよう mip も使う
    D3D11_SAMPLER_DESC nd = sd;
    nd.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    nd.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    nd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    if (!gpubuf::CreateConstant(dev, sizeof(DisturbanceFieldCB), groundFogLayersCb_)
        || !gpubuf::CreateConstant(dev, sizeof(GroundFogViewCB), groundFogViewCb_)
        || !gpubuf::CreateConstant(dev, sizeof(GroundFogLightsCB), groundFogLightsCb_)
        || FAILED(dev->CreateSamplerState(&sd, groundFogSampler_.ReleaseAndGetAddressOf()))
        || FAILED(dev->CreateSamplerState(&nd, noiseSampler_.ReleaseAndGetAddressOf()))) {
        MYE_LOG_ERROR("DisturbanceFieldPass: 地面の霧の CB / サンプラを作れなかった (地面の霧は描かれない)");
        groundFogLayersCb_.Reset();
        groundFogViewCb_.Reset();
        groundFogLightsCb_.Reset();
        groundFogSampler_.Reset();
        noiseSampler_.Reset();
        return false;
    }
    MYE_LOG_INFO("DisturbanceFieldPass: 地面の霧のリソースを作成 (板の積分 %d 段)", kGroundFogSteps);
    return true;
}

bool DisturbanceFieldPass::BindGroundFog(GraphicsDevice& device, const DisturbanceFieldGpu* field,
                                         const GroundFogViewParams& view)
{
    if (field == nullptr || field->cb.header[0] == 0 || !CreateGroundFogResources(device)) {
        return false;
    }
    GroundFogViewCB v;
    v.camPos[0] = view.camPos[0];
    v.camPos[1] = view.camPos[1];
    v.camPos[2] = view.camPos[2];
    v.camPos[3] = kGroundFogAnisotropy;
    for (int k = 0; k < 3; ++k) {
        v.sunDir[k] = view.sunDir[k];
        v.sunColor[k] = view.sunColor[k];
        v.ambient[k] = view.ambient[k];
    }
    std::memcpy(v.invViewProj, view.invViewProj, sizeof(v.invViewProj));
    v.sunDir[3] = view.hasSun ? 1.0f : 0.0f;
    v.sunColor[3] = kGroundFogAlbedo;

    ID3D11DeviceContext* dc = device.Context();
    gpubuf::UploadCB(dc, groundFogLayersCb_.Get(), field->cb);
    gpubuf::UploadCB(dc, groundFogViewCb_.Get(), v);
    // 局所ライト。照らす層が無ければ本数 0 の CB を張る (シェーダは未束縛を読まない)
    GroundFogLightsCB lightsCb;
    if (view.lights != nullptr && field->localLightMask != 0) {
        const int total = SelectGroundFogLights(*view.lights, field->cb, field->localLightMask, lightsCb);
        if (total > kGroundFogMaxLights) {
            if (!lightOverflowWarned_) {
                MYE_LOG_WARN("DisturbanceFieldPass: 地面の霧を照らす局所ライトが %d 本ある (最大 %d 本。超えた %d 本は使われない)",
                             total, kGroundFogMaxLights, total - kGroundFogMaxLights);
                lightOverflowWarned_ = true;
            }
        } else {
            lightOverflowWarned_ = false;
        }
    }
    gpubuf::UploadCB(dc, groundFogLightsCb_.Get(), lightsCb);
    ID3D11Buffer* cbs[3] = { groundFogLayersCb_.Get(), groundFogViewCb_.Get(), groundFogLightsCb_.Get() };
    static_assert(kGroundFogViewCbSlot == kGroundFogCbSlot + 1 && kGroundFogLightCbSlot == kGroundFogViewCbSlot + 1,
                  "3 本まとめて張る");
    dc->PSSetConstantBuffers(kGroundFogCbSlot, 3, cbs);
    ID3D11ShaderResourceView* srvs[2] = { field->flowSRV, field->auxSRV };
    static_assert(kGroundFogSrvAuxSlot == kGroundFogSrvFlowSlot + 1, "2 本まとめて張る");
    dc->PSSetShaderResources(kGroundFogSrvFlowSlot, 2, srvs);
    ID3D11SamplerState* samp[1] = { groundFogSampler_.Get() };
    dc->PSSetSamplers(kGroundFogSamplerSlot, 1, samp);
    // 霧のノイズのマップ (層ごと。マップの無い層は null)。読むのは地面の霧の PS だけ
    dc->PSSetShaderResources(kGroundFogSrvNoiseMapSlot, kDisturbanceMaxLayers, field->noiseMapSRV);
    ID3D11SamplerState* noiseSamp[1] = { noiseSampler_.Get() };
    dc->PSSetSamplers(kGroundFogNoiseSamplerSlot, 1, noiseSamp);
    // サーフェスシェーダの VS (草の倒れ。MyeFieldSample) が同じ番号で読む。層の CB だけ (b7 / b8 は PS 専用)
    dc->VSSetConstantBuffers(kGroundFogCbSlot, 1, cbs);
    dc->VSSetShaderResources(kGroundFogSrvFlowSlot, 2, srvs);
    dc->VSSetSamplers(kGroundFogSamplerSlot, 1, samp);
    return true;
}

void DisturbanceFieldPass::UnbindGroundFog(GraphicsDevice& device)
{
    ID3D11DeviceContext* dc = device.Context();
    ID3D11Buffer* cbs[3] = {};
    dc->PSSetConstantBuffers(kGroundFogCbSlot, 3, cbs);
    ID3D11ShaderResourceView* srvs[2] = {};
    dc->PSSetShaderResources(kGroundFogSrvFlowSlot, 2, srvs);
    ID3D11SamplerState* samp[1] = {};
    dc->PSSetSamplers(kGroundFogSamplerSlot, 1, samp);
    ID3D11ShaderResourceView* noiseSrvs[kDisturbanceMaxLayers] = {};
    dc->PSSetShaderResources(kGroundFogSrvNoiseMapSlot, kDisturbanceMaxLayers, noiseSrvs);
    dc->PSSetSamplers(kGroundFogNoiseSamplerSlot, 1, samp);
    dc->VSSetConstantBuffers(kGroundFogCbSlot, 1, cbs);
    dc->VSSetShaderResources(kGroundFogSrvFlowSlot, 2, srvs);
    dc->VSSetSamplers(kGroundFogSamplerSlot, 1, samp);
}

void DisturbanceFieldPass::Release()
{
    groundFogLayersCb_.Reset();
    groundFogViewCb_.Reset();
    groundFogLightsCb_.Reset();
    groundFogSampler_.Reset();
    noiseSampler_.Reset();
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
