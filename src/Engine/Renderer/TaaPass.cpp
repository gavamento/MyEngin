#include "Engine/Renderer/TaaPass.h"

#include <algorithm>

#include "Engine/Core/Log.h"
#include "Engine/Renderer/GpuBufferUtil.h"
#include "Engine/Renderer/GraphicsDevice.h"
#include "Engine/Renderer/RenderTypes.h"
#include "Engine/Renderer/ShaderManager.h"

namespace mye {
namespace {

// postfx_taa.hlsl の TaaCB と同一レイアウト (16 バイト)
struct TaaCB {
    float sizeW;
    float sizeH;
    float feedback;
    int32_t histValid;
};

// 履歴の残し率の上限。1.0 に張り付くと今フレームが永久に混ざらず絵が固まるので、
// 「最大でも 1/20 は今フレーム」を保証する。既定 0.9 は 10 フレームで概ね収束する値
constexpr float kMaxFeedback = 0.95f;

using namespace gpubuf;

} // namespace

bool TaaPass::Init(GraphicsDevice& device, ShaderManager& shaders)
{
    ID3D11Device* dev = device.Device();
    shader_ = shaders.Load("postfx_taa");
    if (!CreateConstant(dev, sizeof(TaaCB), cb_)) {
        MYE_LOG_ERROR("TaaPass: CB creation failed");
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

void TaaPass::Shutdown()
{
    for (History& h : hist_) {
        h.color[0].Release();
        h.color[1].Release();
        h.hasLast = false;
        h.w = 0;
        h.h = 0;
    }
    cb_.Reset();
    linearClamp_.Reset();
    depthDisabled_.Reset();
    blendOff_.Reset();
    rasterizer_.Reset();
    ready_ = false;
}

ID3D11ShaderResourceView* TaaPass::Run(GraphicsDevice& device, ShaderManager& shaders,
                                       const RenderView& view, ID3D11ShaderResourceView* sceneSRV,
                                       float feedback)
{
    // ---- 成立条件 (TaaPass.h の ①〜③)。1 つでも欠けたら何もしない ----
    if (!ready_ || view.taaEnabled == 0 || sceneSRV == nullptr || view.velocitySRV == nullptr) {
        return nullptr;
    }
    if (view.viewKey == 0 || view.viewKey >= static_cast<uint32_t>(kHistorySlots)) {
        return nullptr; // AssetPreview (0) と想定外のキーは履歴を持たない
    }
    if (view.width <= 0 || view.height <= 0) {
        return nullptr;
    }
    ShaderProgram* prog = shaders.Get(shader_);
    if (!prog || !prog->valid) {
        return nullptr; // シェーダ未コンパイル — TAA 抜きで従来どおり解決させる
    }

    History& h = hist_[view.viewKey];
    if (h.w != view.width || h.h != view.height) {
        // リサイズ: 再投影の前提 (画素の対応) が崩れるので履歴を捨てる
        for (int i = 0; i < 2; ++i) {
            h.color[i].Resize(device, view.width, view.height, DXGI_FORMAT_R16G16B16A16_FLOAT,
                              false);
        }
        h.w = view.width;
        h.h = view.height;
        h.write = 0;
        h.hasLast = false;
    }
    if (!h.color[0].IsValid() || !h.color[1].IsValid()) {
        return nullptr;
    }
    // ④ 前フレームも同じビューが描かれたか。viewFrameIndex は viewKey 毎の描画通番
    // (RenderSystem が Render の末尾で +1 する) なので、1 つ違いなら連続している
    const bool histValid = h.hasLast && (h.lastSerial + 1u == view.viewFrameIndex);

    RenderTexture& dst = h.color[h.write];
    RenderTexture& src = h.color[1 - h.write];

    ID3D11DeviceContext* dc = device.Context();
    TaaCB cb = {};
    cb.sizeW = static_cast<float>(view.width);
    cb.sizeH = static_cast<float>(view.height);
    cb.feedback = std::clamp(feedback, 0.0f, kMaxFeedback);
    cb.histValid = histValid ? 1 : 0;
    UploadCB(dc, cb_.Get(), cb);

    // フルスクリーンパス (PostProcess の各パスと同じ規約)。入力 SRV を読むため RTV を先に外す
    dc->OMSetRenderTargets(0, nullptr, nullptr);
    dc->IASetInputLayout(nullptr);
    dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dc->OMSetDepthStencilState(depthDisabled_.Get(), 0);
    dc->OMSetBlendState(blendOff_.Get(), nullptr, 0xFFFFFFFFu);
    dc->RSSetState(rasterizer_.Get());
    D3D11_VIEWPORT vp = {};
    vp.Width = cb.sizeW;
    vp.Height = cb.sizeH;
    vp.MaxDepth = 1.0f;
    dc->RSSetViewports(1, &vp);
    ID3D11SamplerState* samps[1] = { linearClamp_.Get() };
    dc->PSSetSamplers(0, 1, samps);
    ID3D11Buffer* cbs[1] = { cb_.Get() };
    dc->PSSetConstantBuffers(0, 1, cbs);
    ID3D11RenderTargetView* rtv = dst.RTV();
    dc->OMSetRenderTargets(1, &rtv, nullptr);
    // t2 = GBuffer RT4。ジオメトリパスの MRT は光パスの OMSetRenderTargets で
    // 既に外れているので、SRV として読んでもハザードにならない
    ID3D11ShaderResourceView* srvs[3] = { sceneSRV, src.SRV(), view.velocitySRV };
    dc->PSSetShaderResources(0, 3, srvs);
    dc->VSSetShader(prog->vs.Get(), nullptr, 0);
    dc->PSSetShader(prog->ps.Get(), nullptr, 0);
    dc->Draw(3, 0);

    // dst は直後に呼び出し側が SRV として読む — RTV と SRV を必ず外してから返す
    dc->OMSetRenderTargets(0, nullptr, nullptr);
    ID3D11ShaderResourceView* nulls[3] = { nullptr, nullptr, nullptr };
    dc->PSSetShaderResources(0, 3, nulls);

    h.write = 1 - h.write;
    h.lastSerial = view.viewFrameIndex;
    h.hasLast = true;
    return dst.SRV();
}

} // namespace mye
