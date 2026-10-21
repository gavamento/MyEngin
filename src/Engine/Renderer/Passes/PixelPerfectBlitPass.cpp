//====================================================================================
//                          PixelPerfectBlitPass.cpp
//  MyEngin/ 秋田蓮音                                                       10/22/2026
//                                          低解像度 RT の整数倍拡大パスの実装
//====================================================================================
#include "Engine/Renderer/Passes/PixelPerfectBlitPass.h"

#include "Engine/Renderer/Device/GraphicsDevice.h"
#include "Engine/Renderer/Shader/ShaderManager.h"

namespace mye {

bool PixelPerfectBlitPass::Init(GraphicsDevice& device, ShaderManager& shaders)
{
    ID3D11Device* dev = device.Device();
    shader_ = shaders.Load("sprite2d_blit");

    D3D11_SAMPLER_DESC smp = {};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT; // ぼかさない
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateSamplerState(&smp, samplerPoint_.GetAddressOf()))) {
        return false;
    }
    D3D11_DEPTH_STENCIL_DESC dsd = {};
    dsd.DepthEnable = FALSE;
    if (FAILED(dev->CreateDepthStencilState(&dsd, depthOff_.GetAddressOf()))) {
        return false;
    }
    D3D11_BLEND_DESC bld = {};
    bld.RenderTarget[0].BlendEnable = FALSE;
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(dev->CreateBlendState(&bld, blendOff_.GetAddressOf()))) {
        return false;
    }
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(dev->CreateRasterizerState(&rd, raster_.GetAddressOf()))) {
        return false;
    }
    ready_ = true;
    return true;
}

bool PixelPerfectBlitPass::Blit(GraphicsDevice& device, ShaderManager& shaders, ID3D11ShaderResourceView* src,
                                int32_t srcW, int32_t srcH, ID3D11RenderTargetView* dst, int32_t dstW,
                                int32_t dstH, int32_t zoom)
{
    if (!ready_ || src == nullptr || dst == nullptr || zoom < 1 || srcW < 1 || srcH < 1) {
        return false;
    }
    ShaderProgram* prog = shaders.Get(shader_);
    if (prog == nullptr || !prog->valid) {
        return false;
    }
    ID3D11DeviceContext* dc = device.Context();

    // 拡大後の矩形を dst の中央へ。はみ出す分 (負のオフセット) は両端で切れる
    const int32_t scaledW = srcW * zoom;
    const int32_t scaledH = srcH * zoom;
    D3D11_VIEWPORT vp = {};
    vp.TopLeftX = static_cast<float>((dstW - scaledW) / 2);
    vp.TopLeftY = static_cast<float>((dstH - scaledH) / 2);
    vp.Width = static_cast<float>(scaledW);
    vp.Height = static_cast<float>(scaledH);
    vp.MaxDepth = 1.0f;

    dc->OMSetRenderTargets(1, &dst, nullptr);
    dc->RSSetViewports(1, &vp);
    dc->RSSetState(raster_.Get());
    dc->OMSetDepthStencilState(depthOff_.Get(), 0);
    dc->OMSetBlendState(blendOff_.Get(), nullptr, 0xFFFFFFFFu);
    dc->IASetInputLayout(nullptr);
    dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dc->VSSetShader(prog->vs.Get(), nullptr, 0);
    dc->PSSetShader(prog->ps.Get(), nullptr, 0);
    dc->PSSetShaderResources(0, 1, &src);
    ID3D11SamplerState* samp = samplerPoint_.Get();
    dc->PSSetSamplers(0, 1, &samp);
    dc->Draw(3, 0);

    ID3D11ShaderResourceView* nullSrv[1] = { nullptr };
    dc->PSSetShaderResources(0, 1, nullSrv);
    dc->OMSetRenderTargets(0, nullptr, nullptr);
    return true;
}

void PixelPerfectBlitPass::Release()
{
    ready_ = false;
    shader_ = {};
    samplerPoint_.Reset();
    depthOff_.Reset();
    blendOff_.Reset();
    raster_.Reset();
}

} // namespace mye
