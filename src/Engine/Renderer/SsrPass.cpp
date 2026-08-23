#include "Engine/Renderer/SsrPass.h"

#include <cstdint>

#include <DirectXMath.h>

#include "Engine/Core/Log.h"
#include "Engine/Renderer/GpuBufferUtil.h"
#include "Engine/Renderer/GraphicsDevice.h"
#include "Engine/Renderer/ShaderManager.h"

using namespace DirectX;

namespace mye {
namespace {

// ssr_trace.hlsl の SsrCB (b0) と同一レイアウト (16 バイト境界で手詰め)
struct SsrCB {
    XMFLOAT4X4 viewProj;    // transpose(view*proj)。**ジッタ込み** = 深度と同じ行列
    XMFLOAT4X4 invViewProj; // transpose(inverse(view*proj))
    XMFLOAT3 cameraPos;
    float intensity;
    float screenSize[2];
    float nearZ;
    float farZ;
    float maxRough;
    float thickness;
    float maxDistance;
    float edgeFade;
    int32_t mipCount;
    int32_t iblEnabled;
    float iblSpecMips;
    int32_t aoEnabled;
    int32_t fogMode;
    float fogDensity;
    float fogStart;
    float fogEnd;
    float fogHeightFalloff;
    float fogBaseHeight;
    float pad0[2];
};
static_assert(sizeof(SsrCB) == 224, "SsrCB must match the HLSL 16-byte packing");

using namespace gpubuf;

} // namespace

bool SsrPass::Init(GraphicsDevice& device, ShaderManager& shaders)
{
    shader_ = shaders.Load("ssr_trace");
    ID3D11Device* dev = device.Device();
    if (!CreateConstant(dev, sizeof(SsrCB), cb_)) {
        return false;
    }
    // 加算合成。**差分を足す**パスなので src の係数は 1 で、alpha は書かない
    // (シーン RT のアルファはトーンマップも FXAA も読まないが、触らないのが安全)
    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask =
        D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN
        | D3D11_COLOR_WRITE_ENABLE_BLUE;
    if (FAILED(dev->CreateBlendState(&bd, blendAdd_.GetAddressOf()))) {
        return false;
    }
    // 計測できなくても描画は続く (HzbPass / ShadowAtlas と同じ扱い)
    timer_.Init(device);
    return true;
}

void SsrPass::Shutdown()
{
    cb_.Reset();
    blendAdd_.Reset();
    sceneCopy_.Reset();
    sceneCopySrv_.Reset();
    copyW_ = 0;
    copyH_ = 0;
    copyFormat_ = DXGI_FORMAT_UNKNOWN;
}

bool SsrPass::EnsureSceneCopy(GraphicsDevice& device, ID3D11RenderTargetView* rtv)
{
    if (!rtv) {
        return false;
    }
    // **寸法もフォーマットも RTV の実体から取る** — CopyResource は完全一致でないと
    // 黙って何もしないので、ここで数値を二重管理しない (M56b の EnsureNormalCopy と同じ)。
    // シーン RT は HDR 経路なら R16G16B16A16F、直描き経路ならバックバッファの形式
    Microsoft::WRL::ComPtr<ID3D11Resource> res;
    rtv->GetResource(res.GetAddressOf());
    Microsoft::WRL::ComPtr<ID3D11Texture2D> srcTex;
    if (!res || FAILED(res.As(&srcTex))) {
        return false;
    }
    D3D11_TEXTURE2D_DESC td = {};
    srcTex->GetDesc(&td);
    if (td.Width == 0 || td.Height == 0 || td.SampleDesc.Count != 1) {
        return false; // MSAA の RT へは CopyResource できない (この経路には来ない)
    }
    if (sceneCopySrv_ && copyW_ == static_cast<int>(td.Width)
        && copyH_ == static_cast<int>(td.Height) && copyFormat_ == td.Format) {
        return true;
    }
    sceneCopy_.Reset();
    sceneCopySrv_.Reset();
    copyW_ = 0;
    copyH_ = 0;
    copyFormat_ = DXGI_FORMAT_UNKNOWN;

    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE; // RTV は要らない (読むだけ)
    td.CPUAccessFlags = 0;
    td.MiscFlags = 0;
    ID3D11Device* dev = device.Device();
    if (FAILED(dev->CreateTexture2D(&td, nullptr, sceneCopy_.GetAddressOf()))
        || FAILED(dev->CreateShaderResourceView(sceneCopy_.Get(), nullptr,
                                                sceneCopySrv_.GetAddressOf()))) {
        MYE_LOG_ERROR("SsrPass: scene color copy creation failed (%ux%u)", td.Width, td.Height);
        sceneCopy_.Reset();
        sceneCopySrv_.Reset();
        return false;
    }
    copyW_ = static_cast<int>(td.Width);
    copyH_ = static_cast<int>(td.Height);
    copyFormat_ = td.Format;
    return true;
}

bool SsrPass::Render(GraphicsDevice& device, ShaderManager& shaders, const RenderView& view,
                     const Inputs& in)
{
    if (!cb_ || !blendAdd_ || !view.rtv || in.hzb == nullptr || in.hzbMipCount <= 0
        || in.gbAlbedo == nullptr || in.gbNormal == nullptr || in.gbMaterial == nullptr) {
        return false;
    }
    ShaderProgram* prog = shaders.Get(shader_);
    if (!prog || !prog->valid) {
        return false; // コンパイル失敗時は SSR 無しで進む (絵は M56c 以前と同じ)
    }
    if (!EnsureSceneCopy(device, view.rtv)) {
        return false;
    }

    ID3D11DeviceContext* dc = device.Context();
    timer_.Begin(device);

    // ★呼び出し側が RTV を外している前提。CopyResource は RTV に bind されたままの
    //   リソースへは当てられない (M56b の RT1 コピーで踏んだのと同じ順序の罠)
    Microsoft::WRL::ComPtr<ID3D11Resource> srcRes;
    view.rtv->GetResource(srcRes.GetAddressOf());
    dc->CopyResource(sceneCopy_.Get(), srcRes.Get());

    const XMMATRIX v = XMLoadFloat4x4(&view.view);
    const XMMATRIX p = XMLoadFloat4x4(&view.proj); // **ジッタ込み** (深度をラスタライズした行列)
    const XMMATRIX vp = XMMatrixMultiply(v, p);
    XMVECTOR det = {};
    const XMMATRIX invVp = XMMatrixInverse(&det, vp);
    if (XMVectorGetX(XMVectorAbs(det)) < 1e-20f) {
        timer_.End(device);
        return false; // 退化した射影 (ヘッドレスの手組み view など)
    }

    SsrCB c = {};
    XMStoreFloat4x4(&c.viewProj, XMMatrixTranspose(vp));
    XMStoreFloat4x4(&c.invViewProj, XMMatrixTranspose(invVp));
    c.cameraPos = view.cameraPos;
    c.intensity = (view.ssrIntensity > 0.0f) ? view.ssrIntensity : 0.0f;
    c.screenSize[0] = static_cast<float>(view.width);
    c.screenSize[1] = static_cast<float>(view.height);
    c.nearZ = view.nearZ;
    c.farZ = view.farZ;
    c.maxRough = (view.ssrMaxRoughness > 0.0f) ? view.ssrMaxRoughness : 0.0f;
    c.thickness = kSsrThickness;
    c.maxDistance = kSsrMaxDistance;
    c.edgeFade = kSsrEdgeFade;
    c.mipCount = in.hzbMipCount;
    // IBL スペキュラを差し引くのは「ライトパスが実際に足したとき」だけ。
    // 判定式はライトパスの pf.iblEnabled と同一 (3 枚そろって初めて有効)
    c.iblEnabled = (view.iblIrradiance != nullptr && view.iblPrefiltered != nullptr
                    && view.iblBrdfLut != nullptr)
        ? 1
        : 0;
    c.iblSpecMips = view.iblSpecMips;
    c.aoEnabled = (in.ssao != nullptr) ? 1 : 0;
    // フォグ: ライトパスは環境項を足した**後**に霞ませている。後から足す差分にも
    // 同じ透過率を掛けないと、遠くの反射だけが霧を突き抜けて見える
    c.fogMode = view.fogMode;
    c.fogDensity = view.fogDensity;
    c.fogStart = view.fogStart;
    c.fogEnd = view.fogEnd;
    c.fogHeightFalloff = view.fogHeightFalloff;
    c.fogBaseHeight = view.fogBaseHeight;
    UploadCB(dc, cb_.Get(), c);

    ID3D11RenderTargetView* rtvs[1] = { view.rtv };
    dc->OMSetRenderTargets(1, rtvs, nullptr); // 深度は SRV (HZB) 経由で読む
    ID3D11Buffer* cbs[1] = { cb_.Get() };
    dc->PSSetConstantBuffers(0, 1, cbs);
    ID3D11SamplerState* samplers[1] = { in.linearClamp };
    dc->PSSetSamplers(0, 1, samplers);
    ID3D11ShaderResourceView* srvs[8] = { sceneCopySrv_.Get(),
                                          in.hzb,
                                          in.gbAlbedo,
                                          in.gbNormal,
                                          in.gbMaterial,
                                          view.iblPrefiltered,
                                          view.iblBrdfLut,
                                          in.ssao };
    dc->PSSetShaderResources(0, 8, srvs);
    dc->IASetInputLayout(nullptr);
    dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dc->OMSetBlendState(blendAdd_.Get(), nullptr, 0xFFFFFFFFu);
    dc->VSSetShader(prog->vs.Get(), nullptr, 0);
    dc->PSSetShader(prog->ps.Get(), nullptr, 0);
    dc->Draw(3, 0);

    // シーンコピーと GBuffer を SRV に残さない (次フレームの CopyResource / RTV 化と競合する)
    ID3D11ShaderResourceView* nullSrvs[8] = {};
    dc->PSSetShaderResources(0, 8, nullSrvs);
    timer_.End(device);
    return true;
}

} // namespace mye
