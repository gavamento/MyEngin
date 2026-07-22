#include "Engine/Renderer/PostProcess.h"

#include <algorithm>
#include <utility>

#include "Engine/Core/Components.h"
#include "Engine/Core/Log.h"
#include "Engine/Renderer/GraphicsDevice.h"
#include "Engine/Renderer/ShaderManager.h"

namespace mye {

PostProcess::Settings MergeCameraPostFx(const PostProcess::Settings& base,
                                        const CameraPostFxComponent& comp)
{
    PostProcess::Settings s = base; // applyGamma は base を維持
    s.exposure = (comp.exposure >= 0.0f) ? comp.exposure : 0.0f;
    s.tonemap = (comp.tonemapMode >= 0 && comp.tonemapMode <= 2) ? comp.tonemapMode : 1;
    s.bloom = comp.bloomOn != 0;
    s.bloomThreshold = comp.bloomThreshold;
    s.chromAberration = comp.chromAberration; // M32d
    s.vignetteIntensity = comp.vignetteIntensity;
    s.vignetteRadius = comp.vignetteRadius;
    s.saturation = comp.saturation;
    s.contrast = comp.contrast;
    s.colorFilter = comp.colorFilter;
    s.bloomIntensity = comp.bloomIntensity;
    s.fxaa = comp.fxaaOn != 0;
    return s;
}
namespace {

// postfx_tonemap.hlsl の PostFx cbuffer と一致 (64 バイト)
struct PostFxCB {
    float exposure;
    int32_t tonemap;
    float bloomIntensity;
    int32_t applyGamma;
    float chromAberration; // M32d
    float vignetteIntensity;
    float vignetteRadius;
    float saturation;
    float contrast;
    float pad[3];
    DirectX::XMFLOAT4 colorFilter;
};

// postfx_bright.hlsl の Bright cbuffer (16 バイト)
struct BrightCB {
    float threshold;
    float pad0, pad1, pad2;
};

// postfx_blur.hlsl の Blur cbuffer (16 バイト)
struct BlurCB {
    float texelX, texelY;
    float pad0, pad1;
};

// postfx_fxaa.hlsl の Fxaa cbuffer (16 バイト)
struct FxaaCB {
    float invW, invH;
    float pad0, pad1;
};

template <typename T>
void UploadCB(ID3D11DeviceContext* dc, ID3D11Buffer* cb, const T& data)
{
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(dc->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, &data, sizeof(T));
        dc->Unmap(cb, 0);
    }
}

bool CreateCB(ID3D11Device* dev, UINT size, Microsoft::WRL::ComPtr<ID3D11Buffer>& out)
{
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = size;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(dev->CreateBuffer(&bd, nullptr, out.GetAddressOf()));
}

} // namespace

bool PostProcess::Init(GraphicsDevice& device, ShaderManager& shaders)
{
    device_ = &device;
    ID3D11Device* dev = device.Device();

    tonemapShader_ = shaders.Load("postfx_tonemap");
    brightShader_ = shaders.Load("postfx_bright");
    blurShader_ = shaders.Load("postfx_blur");
    fxaaShader_ = shaders.Load("postfx_fxaa");

    if (!CreateCB(dev, sizeof(PostFxCB), cb_) || !CreateCB(dev, sizeof(BrightCB), brightCB_)
        || !CreateCB(dev, sizeof(BlurCB), blurCB_) || !CreateCB(dev, sizeof(FxaaCB), fxaaCB_)) {
        MYE_LOG_ERROR("PostProcess: CB creation failed");
        return false;
    }

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateSamplerState(&sd, linearClamp_.GetAddressOf()))) {
        return false;
    }

    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = FALSE;
    if (FAILED(dev->CreateDepthStencilState(&dd, depthDisabled_.GetAddressOf()))) {
        return false;
    }

    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable = FALSE;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(dev->CreateBlendState(&bd, blendOff_.GetAddressOf()))) {
        return false;
    }

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(dev->CreateRasterizerState(&rd, rasterizer_.GetAddressOf()))) {
        return false;
    }

    ready_ = true;
    return true;
}

PostProcess::Target* PostProcess::Acquire(GraphicsDevice& device, int width, int height)
{
    if (width <= 0 || height <= 0) {
        return nullptr;
    }
    const uint64_t key =
        (static_cast<uint64_t>(static_cast<uint32_t>(width)) << 32) | static_cast<uint32_t>(height);
    for (size_t i = 0; i < cache_.size(); ++i) {
        if (cache_[i].key == key) {
            if (i != 0) { // move-to-front (簡易 LRU)
                Target tmp = std::move(cache_[i]);
                cache_.erase(cache_.begin() + i);
                cache_.insert(cache_.begin(), std::move(tmp));
            }
            return &cache_[0];
        }
    }
    Target t;
    t.key = key;
    const int bw = std::max(1, width / 2);
    const int bh = std::max(1, height / 2);
    if (!t.scene.Create(device, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, false)
        || !t.bloomA.Create(device, bw, bh, DXGI_FORMAT_R16G16B16A16_FLOAT, false)
        || !t.bloomB.Create(device, bw, bh, DXGI_FORMAT_R16G16B16A16_FLOAT, false)
        || !t.ldr.Create(device, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, false)) {
        return nullptr;
    }
    cache_.insert(cache_.begin(), std::move(t));
    constexpr size_t kMaxEntries = 4; // SceneView/GameView/backbuffer 想定の上限
    if (cache_.size() > kMaxEntries) {
        cache_.resize(kMaxEntries); // 末尾 (最古) を破棄
    }
    return &cache_[0];
}

void PostProcess::RunBloom(GraphicsDevice& device, ShaderManager& shaders, Target& t,
                           const Settings& s)
{
    ShaderProgram* bright = shaders.Get(brightShader_);
    ShaderProgram* blur = shaders.Get(blurShader_);
    if (!bright || !bright->valid || !blur || !blur->valid) {
        return; // シェーダ未コンパイル — Resolve 側で bloom 無効にフォールバック
    }
    ID3D11DeviceContext* dc = device.Context();
    const int bw = t.bloomA.Width();
    const int bh = t.bloomA.Height();

    // 共通ステート (フルスクリーン、深度/ブレンド無し、半解像度ビューポート)
    dc->IASetInputLayout(nullptr);
    dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dc->OMSetDepthStencilState(depthDisabled_.Get(), 0);
    dc->OMSetBlendState(blendOff_.Get(), nullptr, 0xFFFFFFFFu);
    dc->RSSetState(rasterizer_.Get());
    ID3D11SamplerState* samp[1] = { linearClamp_.Get() };
    dc->PSSetSamplers(0, 1, samp);
    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(bw);
    vp.Height = static_cast<float>(bh);
    vp.MaxDepth = 1.0f;
    dc->RSSetViewports(1, &vp);
    ID3D11ShaderResourceView* nullSrv[1] = { nullptr };

    // 1) bright-pass: scene(full) → bloomA(half)。linear sample がダウンサンプルも兼ねる
    {
        BrightCB cb = {};
        cb.threshold = s.bloomThreshold;
        UploadCB(dc, brightCB_.Get(), cb);
        ID3D11Buffer* cbs[1] = { brightCB_.Get() };
        dc->PSSetConstantBuffers(0, 1, cbs);
        ID3D11RenderTargetView* rtv = t.bloomA.RTV();
        dc->OMSetRenderTargets(1, &rtv, nullptr);
        ID3D11ShaderResourceView* srv[1] = { t.scene.SRV() };
        dc->PSSetShaderResources(0, 1, srv);
        dc->VSSetShader(bright->vs.Get(), nullptr, 0);
        dc->PSSetShader(bright->ps.Get(), nullptr, 0);
        dc->Draw(3, 0);
        dc->PSSetShaderResources(0, 1, nullSrv);
    }

    // 2) 分離ガウスブラー (H, V) を 2 回 (グロー幅を確保)
    dc->VSSetShader(blur->vs.Get(), nullptr, 0);
    dc->PSSetShader(blur->ps.Get(), nullptr, 0);
    for (int iter = 0; iter < 2; ++iter) {
        // H: bloomA → bloomB
        {
            BlurCB cb = {};
            cb.texelX = 1.0f / static_cast<float>(bw);
            UploadCB(dc, blurCB_.Get(), cb);
            ID3D11Buffer* cbs[1] = { blurCB_.Get() };
            dc->PSSetConstantBuffers(0, 1, cbs);
            ID3D11RenderTargetView* rtv = t.bloomB.RTV();
            dc->OMSetRenderTargets(1, &rtv, nullptr);
            ID3D11ShaderResourceView* srv[1] = { t.bloomA.SRV() };
            dc->PSSetShaderResources(0, 1, srv);
            dc->Draw(3, 0);
            dc->PSSetShaderResources(0, 1, nullSrv);
        }
        // V: bloomB → bloomA
        {
            BlurCB cb = {};
            cb.texelY = 1.0f / static_cast<float>(bh);
            UploadCB(dc, blurCB_.Get(), cb);
            ID3D11Buffer* cbs[1] = { blurCB_.Get() };
            dc->PSSetConstantBuffers(0, 1, cbs);
            ID3D11RenderTargetView* rtv = t.bloomA.RTV();
            dc->OMSetRenderTargets(1, &rtv, nullptr);
            ID3D11ShaderResourceView* srv[1] = { t.bloomB.SRV() };
            dc->PSSetShaderResources(0, 1, srv);
            dc->Draw(3, 0);
            dc->PSSetShaderResources(0, 1, nullSrv);
        }
    }
}

void PostProcess::Resolve(GraphicsDevice& device, ShaderManager& shaders, Target& t,
                          ID3D11RenderTargetView* dst, int width, int height, const Settings& s)
{
    ID3D11DeviceContext* dc = device.Context();
    ShaderProgram* prog = shaders.Get(tonemapShader_);
    if (!prog || !prog->valid || dst == nullptr) {
        return; // 解決不能 (シェーダ未コンパイル等)。呼び出し側は既に HDR に描画済み
    }

    ID3D11ShaderResourceView* bloomSRV = t.scene.SRV(); // プレースホルダ (intensity 0 で不参照)
    float bloomIntensity = 0.0f;
    if (s.bloom) {
        RunBloom(device, shaders, t, s);
        ShaderProgram* bright = shaders.Get(brightShader_);
        ShaderProgram* blur = shaders.Get(blurShader_);
        if (bright && bright->valid && blur && blur->valid) {
            bloomSRV = t.bloomA.SRV();
            bloomIntensity = s.bloomIntensity;
        }
    }

    // FXAA 有効時はトーンマップを LDR 中間 (t.ldr) に描き、その後 FXAA で dst へ。
    ShaderProgram* fxaa = shaders.Get(fxaaShader_);
    const bool useFxaa = s.fxaa && fxaa && fxaa->valid && t.ldr.IsValid();
    ID3D11RenderTargetView* tonemapDst = useFxaa ? t.ldr.RTV() : dst;

    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(width);
    vp.Height = static_cast<float>(height);
    vp.MaxDepth = 1.0f;

    // ---- トーンマップ: scene(t0) + bloom(t1) → tonemapDst ----
    dc->OMSetRenderTargets(1, &tonemapDst, nullptr);
    dc->RSSetViewports(1, &vp);
    dc->RSSetState(rasterizer_.Get());

    PostFxCB cb = {};
    cb.exposure = s.exposure;
    cb.tonemap = s.tonemap;
    cb.bloomIntensity = bloomIntensity;
    cb.applyGamma = s.applyGamma ? 1 : 0;
    cb.chromAberration = s.chromAberration;
    cb.vignetteIntensity = s.vignetteIntensity;
    cb.vignetteRadius = s.vignetteRadius;
    cb.saturation = s.saturation;
    cb.contrast = s.contrast;
    cb.colorFilter = s.colorFilter;
    UploadCB(dc, cb_.Get(), cb);
    ID3D11Buffer* cbs[1] = { cb_.Get() };
    dc->PSSetConstantBuffers(0, 1, cbs);

    ID3D11ShaderResourceView* srvs[2] = { t.scene.SRV(), bloomSRV };
    dc->PSSetShaderResources(0, 2, srvs);
    ID3D11SamplerState* samps[1] = { linearClamp_.Get() };
    dc->PSSetSamplers(0, 1, samps);

    dc->IASetInputLayout(nullptr);
    dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dc->VSSetShader(prog->vs.Get(), nullptr, 0);
    dc->PSSetShader(prog->ps.Get(), nullptr, 0);
    dc->OMSetDepthStencilState(depthDisabled_.Get(), 0);
    dc->OMSetBlendState(blendOff_.Get(), nullptr, 0xFFFFFFFFu);
    dc->Draw(3, 0);

    ID3D11ShaderResourceView* nulls[2] = { nullptr, nullptr };
    dc->PSSetShaderResources(0, 2, nulls); // t.scene を次フレーム RTV に戻すため解除

    // ---- FXAA: t.ldr → dst ----
    if (useFxaa) {
        FxaaCB fcb = {};
        fcb.invW = 1.0f / static_cast<float>(width);
        fcb.invH = 1.0f / static_cast<float>(height);
        UploadCB(dc, fxaaCB_.Get(), fcb);
        ID3D11Buffer* fcbs[1] = { fxaaCB_.Get() };
        dc->PSSetConstantBuffers(0, 1, fcbs);
        dc->OMSetRenderTargets(1, &dst, nullptr);
        dc->RSSetViewports(1, &vp);
        ID3D11ShaderResourceView* fsrv[1] = { t.ldr.SRV() };
        dc->PSSetShaderResources(0, 1, fsrv);
        dc->VSSetShader(fxaa->vs.Get(), nullptr, 0);
        dc->PSSetShader(fxaa->ps.Get(), nullptr, 0);
        dc->Draw(3, 0);
        ID3D11ShaderResourceView* fnull[1] = { nullptr };
        dc->PSSetShaderResources(0, 1, fnull);
    }

    dc->OMSetDepthStencilState(nullptr, 0);
    dc->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
}

} // namespace mye
