#include "Engine/Renderer/EnvMapBaker.h"

#include <cstring>

#include "Engine/Core/Log.h"
#include "Engine/Renderer/GraphicsDevice.h"
#include "Engine/Renderer/ShaderManager.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

namespace mye {
namespace {

// ibl_common.hlsli の BakeCB と同一レイアウト
struct BakeCB {
    XMFLOAT3 faceForward; float roughness;
    XMFLOAT3 faceRight;   int32_t srcMode; // 0=cubemap 1=gradient
    XMFLOAT3 faceUp;      float pad0;
    XMFLOAT3 gradTop;     float pad1;
    XMFLOAT3 gradHorizon; float pad2;
    XMFLOAT3 gradBottom;  float pad3;
};

// D3D cubemap 面順 (+X,-X,+Y,-Y,+Z,-Z) の forward/right/up 基底
struct FaceBasis {
    XMFLOAT3 forward, right, up;
};
constexpr FaceBasis kFaces[6] = {
    { { 1, 0, 0 }, { 0, 0, -1 }, { 0, 1, 0 } },  // +X
    { { -1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 } },  // -X
    { { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, -1 } },  // +Y
    { { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, 1 } },  // -Y
    { { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 } },   // +Z
    { { 0, 0, -1 }, { -1, 0, 0 }, { 0, 1, 0 } }, // -Z
};

// 3 色のビットパターンハッシュ (gradient キャッシュキー。fnv-1a)
uint64_t HashGradient(const XMFLOAT3& a, const XMFLOAT3& b, const XMFLOAT3& c)
{
    const float vals[9] = { a.x, a.y, a.z, b.x, b.y, b.z, c.x, c.y, c.z };
    uint64_t h = 0xcbf29ce484222325ull;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(vals);
    for (size_t i = 0; i < sizeof(vals); ++i) {
        h = (h ^ p[i]) * 0x100000001b3ull;
    }
    return h | 0x8000000000000000ull; // AssetID 空間との衝突をビットで避ける (目印)
}

bool CreateCube(ID3D11Device* dev, int size, int mips, ComPtr<ID3D11Texture2D>& tex,
                ComPtr<ID3D11ShaderResourceView>& srv)
{
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = static_cast<UINT>(size);
    td.Height = static_cast<UINT>(size);
    td.MipLevels = static_cast<UINT>(mips);
    td.ArraySize = 6;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.SampleDesc = { 1, 0 };
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, tex.ReleaseAndGetAddressOf()))) {
        return false;
    }
    return SUCCEEDED(
        dev->CreateShaderResourceView(tex.Get(), nullptr, srv.ReleaseAndGetAddressOf()));
}

} // namespace

bool EnvMapBaker::EnsureCommon(GraphicsDevice& device, ShaderManager& shaders)
{
    if (commonReady_) {
        return true;
    }
    ID3D11Device* dev = device.Device();
    prefilterShader_ = shaders.Load("ibl_prefilter");
    irradianceShader_ = shaders.Load("ibl_irradiance");
    lutShader_ = shaders.Load("brdf_lut");

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(BakeCB);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, cb_.GetAddressOf()))) {
        return false;
    }
    D3D11_SAMPLER_DESC smp = {};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateSamplerState(&smp, sampler_.GetAddressOf()))) {
        return false;
    }

    // ---- BRDF LUT (グローバル 1 枚、初回のみ) ----
    ShaderProgram* lut = shaders.Get(lutShader_);
    if (!lut || !lut->valid) {
        return false;
    }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = kLutSize;
    td.Height = kLutSize;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R16G16_FLOAT;
    td.SampleDesc = { 1, 0 };
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, lutTex_.GetAddressOf()))) {
        return false;
    }
    if (FAILED(dev->CreateShaderResourceView(lutTex_.Get(), nullptr, lutSrv_.GetAddressOf()))) {
        return false;
    }
    ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(dev->CreateRenderTargetView(lutTex_.Get(), nullptr, rtv.GetAddressOf()))) {
        return false;
    }
    ID3D11DeviceContext* dc = device.Context();
    ID3D11RenderTargetView* rtvs[1] = { rtv.Get() };
    dc->OMSetRenderTargets(1, rtvs, nullptr);
    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(kLutSize);
    vp.Height = static_cast<float>(kLutSize);
    vp.MaxDepth = 1.0f;
    dc->RSSetViewports(1, &vp);
    dc->IASetInputLayout(nullptr);
    dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dc->VSSetShader(lut->vs.Get(), nullptr, 0);
    dc->PSSetShader(lut->ps.Get(), nullptr, 0);
    dc->Draw(3, 0);
    ID3D11RenderTargetView* nullRtv[1] = { nullptr };
    dc->OMSetRenderTargets(1, nullRtv, nullptr);

    MYE_LOG_INFO("[ibl] BRDF LUT baked (%dx%d)", kLutSize, kLutSize);
    commonReady_ = true;
    return true;
}

bool EnvMapBaker::Bake(GraphicsDevice& device, ShaderManager& shaders,
                       ID3D11ShaderResourceView* src, const GradientColors& grad, Baked& out)
{
    if (!EnsureCommon(device, shaders)) {
        return false;
    }
    ShaderProgram* pre = shaders.Get(prefilterShader_);
    ShaderProgram* irr = shaders.Get(irradianceShader_);
    if (!pre || !pre->valid || !irr || !irr->valid) {
        return false;
    }
    ID3D11Device* dev = device.Device();
    ID3D11DeviceContext* dc = device.Context();
    if (!CreateCube(dev, kSpecSize, kSpecMips, out.preTex, out.preSrv)
        || !CreateCube(dev, kIrrSize, 1, out.irrTex, out.irrSrv)) {
        return false;
    }

    // 直前パスの SRV 束縛を外す (RTV/SRV ハザード回避 — メイン描画の t0-7 が残っている場合)
    ID3D11ShaderResourceView* nullSrvs[8] = {};
    dc->PSSetShaderResources(0, 8, nullSrvs);

    dc->IASetInputLayout(nullptr);
    dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11Buffer* cbs[1] = { cb_.Get() };
    dc->PSSetConstantBuffers(0, 1, cbs);
    ID3D11SamplerState* samps[1] = { sampler_.Get() };
    dc->PSSetSamplers(0, 1, samps);
    ID3D11ShaderResourceView* srcSrvs[1] = { src };
    dc->PSSetShaderResources(0, 1, srcSrvs);
    dc->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
    dc->OMSetDepthStencilState(nullptr, 0);

    auto bakeFaces = [&](ShaderProgram* prog, ID3D11Texture2D* tex, int size, int mips,
                         float roughnessScale) {
        dc->VSSetShader(prog->vs.Get(), nullptr, 0);
        dc->PSSetShader(prog->ps.Get(), nullptr, 0);
        for (int m = 0; m < mips; ++m) {
            const int mipSize = size >> m;
            D3D11_VIEWPORT vp = {};
            vp.Width = static_cast<float>(mipSize);
            vp.Height = static_cast<float>(mipSize);
            vp.MaxDepth = 1.0f;
            dc->RSSetViewports(1, &vp);
            for (int f = 0; f < 6; ++f) {
                D3D11_RENDER_TARGET_VIEW_DESC rd = {};
                rd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                rd.Texture2DArray.MipSlice = static_cast<UINT>(m);
                rd.Texture2DArray.FirstArraySlice = static_cast<UINT>(f);
                rd.Texture2DArray.ArraySize = 1;
                ComPtr<ID3D11RenderTargetView> rtv;
                if (FAILED(dev->CreateRenderTargetView(tex, &rd, rtv.GetAddressOf()))) {
                    return false;
                }
                BakeCB cb = {};
                cb.faceForward = kFaces[f].forward;
                cb.faceRight = kFaces[f].right;
                cb.faceUp = kFaces[f].up;
                cb.roughness = (mips > 1)
                    ? roughnessScale * static_cast<float>(m) / static_cast<float>(mips - 1)
                    : 0.0f;
                cb.srcMode = (src != nullptr) ? 0 : 1;
                cb.gradTop = grad.top;
                cb.gradHorizon = grad.horizon;
                cb.gradBottom = grad.bottom;
                D3D11_MAPPED_SUBRESOURCE mapped = {};
                if (SUCCEEDED(dc->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                    memcpy(mapped.pData, &cb, sizeof(cb));
                    dc->Unmap(cb_.Get(), 0);
                }
                ID3D11RenderTargetView* rtvs[1] = { rtv.Get() };
                dc->OMSetRenderTargets(1, rtvs, nullptr);
                dc->Draw(3, 0);
            }
        }
        return true;
    };

    const bool ok = bakeFaces(pre, out.preTex.Get(), kSpecSize, kSpecMips, 1.0f)
        && bakeFaces(irr, out.irrTex.Get(), kIrrSize, 1, 0.0f);

    ID3D11RenderTargetView* nullRtv[1] = { nullptr };
    dc->OMSetRenderTargets(1, nullRtv, nullptr);
    dc->PSSetShaderResources(0, 1, nullSrvs);
    return ok;
}

EnvMaps EnvMapBaker::Result(const Baked& b) const
{
    EnvMaps em;
    em.irradiance = b.irrSrv.Get();
    em.prefiltered = b.preSrv.Get();
    em.brdfLut = lutSrv_.Get();
    em.specMips = static_cast<float>(kSpecMips - 1);
    return em;
}

EnvMaps EnvMapBaker::GetForCubemap(GraphicsDevice& device, ShaderManager& shaders, AssetID id,
                                   ID3D11ShaderResourceView* src)
{
    if (id.IsNull() || src == nullptr) {
        return {};
    }
    if (const auto it = cache_.find(id.value); it != cache_.end()) {
        return Result(it->second);
    }
    if (cache_.size() >= 4) {
        cache_.clear(); // 再ベイクは安いので単純化 (LRU 不要)
    }
    Baked b;
    if (!Bake(device, shaders, src, {}, b)) {
        return {};
    }
    MYE_LOG_INFO("[ibl] env maps baked for cubemap %016llx",
                 static_cast<unsigned long long>(id.value));
    return Result(cache_.emplace(id.value, std::move(b)).first->second);
}

EnvMaps EnvMapBaker::GetForGradient(GraphicsDevice& device, ShaderManager& shaders,
                                    const XMFLOAT3& top, const XMFLOAT3& horizon,
                                    const XMFLOAT3& bottom)
{
    const uint64_t key = HashGradient(top, horizon, bottom);
    if (const auto it = cache_.find(key); it != cache_.end()) {
        return Result(it->second);
    }
    if (cache_.size() >= 4) {
        cache_.clear();
    }
    Baked b;
    GradientColors grad;
    grad.top = top;
    grad.horizon = horizon;
    grad.bottom = bottom;
    if (!Bake(device, shaders, nullptr, grad, b)) {
        return {};
    }
    MYE_LOG_INFO("[ibl] env maps baked for gradient sky");
    return Result(cache_.emplace(key, std::move(b)).first->second);
}

// M46h: BRDF LUT だけを確保する。EnsureCommon の中でグローバル 1 枚として
// ベイクされるので、キューブマップの生成 (Bake) は一切走らない
ID3D11ShaderResourceView* EnvMapBaker::GetBrdfLut(GraphicsDevice& device, ShaderManager& shaders)
{
    if (!EnsureCommon(device, shaders)) {
        return nullptr;
    }
    return lutSrv_.Get();
}

void EnvMapBaker::Shutdown()
{
    cache_.clear();
    lutSrv_.Reset();
    lutTex_.Reset();
    sampler_.Reset();
    cb_.Reset();
    commonReady_ = false;
}

} // namespace mye
