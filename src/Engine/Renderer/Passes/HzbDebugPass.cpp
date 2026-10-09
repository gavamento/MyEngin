//====================================================================================
//                          HzbDebugPass.cpp
//  MyEngin/ 秋田蓮音                                                       10/09/2026
//                                          HZB ピラミッドの全画面可視化 (--hzb-debug) 実装
//====================================================================================
#include "Engine/Renderer/Passes/HzbDebugPass.h"

#include <algorithm>

#include "Engine/Renderer/Device/GpuBufferUtil.h"
#include "Engine/Renderer/Device/GraphicsDevice.h"
#include "Engine/Renderer/Passes/HzbPass.h"
#include "Engine/Renderer/Pipeline/RenderTypes.h"
#include "Engine/Renderer/Shader/ShaderManager.h"

namespace mye {
namespace {

using namespace gpubuf;

// debug_hzb.hlsl の HzbDebugCB と同一レイアウト
struct HzbDebugCB {
    float dstSize[2];
    float mipSize[2];
    float nearZ;
    float farZ;
    float range;
    float mip;
};

// 黒に振り切る距離 [world]。--render-demo のカメラ (原点から約 18.6) で、手前の柱が白く
// 床の奥が黒へ落ちる階調になる値。デバッグ表示専用
constexpr float kHzbDebugRange = 40.0f;

} // namespace

bool HzbDebugPass::Init(GraphicsDevice& device, ShaderManager& shaders)
{
    shader_ = shaders.Load("debug_hzb");
    if (!CreateConstant(device.Device(), sizeof(HzbDebugCB), cb_)) {
        return false;
    }
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = FALSE;
    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable = FALSE;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    return SUCCEEDED(device.Device()->CreateDepthStencilState(&dd, depthDisabled_.GetAddressOf()))
        && SUCCEEDED(device.Device()->CreateBlendState(&bd, blendOpaque_.GetAddressOf()));
}

void HzbDebugPass::Shutdown()
{
    cb_.Reset();
    depthDisabled_.Reset();
    blendOpaque_.Reset();
}

void HzbDebugPass::Render(GraphicsDevice& device, ShaderManager& shaders, const RenderView& view,
                          const HzbPass& pyramid, int hzbDebug)
{
    ShaderProgram* prog = shaders.Get(shader_);
    if (!prog || !prog->valid || !cb_ || pyramid.SRV() == nullptr || pyramid.MipCount() <= 0) {
        return;
    }
    ID3D11DeviceContext* dc = device.Context();
    // 指定が段数を超えたら最上段で頭打ち。範囲外の Load は 0 を返して真っ白になり、
    // 指定ミスと本物の深度が区別できなくなる。下側も 0 で止める
    const int mip = std::clamp(hzbDebug - 1, 0, pyramid.MipCount() - 1);
    HzbDebugCB hd = {};
    hd.dstSize[0] = static_cast<float>(view.width);
    hd.dstSize[1] = static_cast<float>(view.height);
    hd.mipSize[0] = static_cast<float>(HzbMipExtent(pyramid.Width(), mip));
    hd.mipSize[1] = static_cast<float>(HzbMipExtent(pyramid.Height(), mip));
    hd.nearZ = view.nearZ;
    hd.farZ = view.farZ;
    hd.range = kHzbDebugRange;
    hd.mip = static_cast<float>(mip);
    UploadCB(dc, cb_.Get(), hd);

    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(view.width);
    vp.Height = static_cast<float>(view.height);
    vp.MaxDepth = 1.0f;
    dc->OMSetRenderTargets(1, &view.rtv, nullptr); // 深度は SRV で読んでいるので外す
    dc->RSSetViewports(1, &vp);
    ID3D11Buffer* cbs[1] = { cb_.Get() };
    dc->PSSetConstantBuffers(0, 1, cbs);
    ID3D11ShaderResourceView* srv[1] = { pyramid.SRV() }; // 全段を覆う SRV
    dc->PSSetShaderResources(0, 1, srv);
    dc->IASetInputLayout(nullptr);
    dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dc->OMSetDepthStencilState(depthDisabled_.Get(), 0);
    dc->OMSetBlendState(blendOpaque_.Get(), nullptr, 0xFFFFFFFFu);
    dc->VSSetShader(prog->vs.Get(), nullptr, 0);
    dc->PSSetShader(prog->ps.Get(), nullptr, 0);
    dc->Draw(3, 0);
    ID3D11ShaderResourceView* nullSrv[1] = {};
    dc->PSSetShaderResources(0, 1, nullSrv); // 次フレームの UAV 書込前に解除
    dc->OMSetRenderTargets(1, &view.rtv, view.dsv);
    dc->OMSetDepthStencilState(nullptr, 0);
    dc->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
}

} // namespace mye
