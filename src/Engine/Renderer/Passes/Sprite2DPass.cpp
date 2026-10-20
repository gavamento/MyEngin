//====================================================================================
//                          Sprite2DPass.cpp
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                          Sprite2D (2D スプライト) の描画パスの実装
//====================================================================================
#include "Engine/Renderer/Passes/Sprite2DPass.h"

#include <cstring>

#include "Engine/Renderer/Device/GpuResources.h"
#include "Engine/Renderer/Device/GraphicsDevice.h"
#include "Engine/Renderer/Shader/ShaderManager.h"

using namespace DirectX;

namespace mye {

namespace {
// VB が足りないときの成長の余白 (頂点数)。必要数の 1.5 倍にこの値を足す
constexpr uint32_t kVertexCapacityMargin = 512;
constexpr UINT kStencilMaskRef = 1;    // マスクが書く値 (Inside が一致を見る値)
constexpr UINT kStencilOutsideRef = 0; // Outside が一致を見る値 (クリア値)

// sprite2d.hlsl の Sprite2DPsCB と一致させる
struct PsCb {
    float maskClip[2]; // x = 1 でクリップ、y = alpha の閾値
    float pad[2];
};
} // namespace

bool Sprite2DPass::Init(GraphicsDevice& device, ShaderManager& shaders)
{
    ID3D11Device* dev = device.Device();
    shader_ = shaders.Load("sprite2d");

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = sizeof(XMFLOAT4X4);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateBuffer(&cbd, nullptr, cb_.GetAddressOf()))) {
        return false;
    }

    cbd.ByteWidth = sizeof(PsCb);
    if (FAILED(dev->CreateBuffer(&cbd, nullptr, psCb_.GetAddressOf()))) {
        return false;
    }

    D3D11_SAMPLER_DESC smp = {};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateSamplerState(&smp, samplerLinear_.GetAddressOf()))) {
        return false;
    }
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT; // ピクセルアート用: にじませない
    if (FAILED(dev->CreateSamplerState(&smp, samplerPoint_.GetAddressOf()))) {
        return false;
    }

    D3D11_BLEND_DESC bld = {};
    bld.RenderTarget[0].BlendEnable = TRUE;
    bld.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bld.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(dev->CreateBlendState(&bld, blend_.GetAddressOf()))) {
        return false;
    }
    bld.RenderTarget[0].RenderTargetWriteMask = 0; // マスクはステンシルだけ書く
    if (FAILED(dev->CreateBlendState(&bld, blendNoColor_.GetAddressOf()))) {
        return false;
    }

    // 3D メッシュには隠れるが、スプライト同士の前後は整数ソートキーで決まる (深度は書かない)
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    if (FAILED(dev->CreateDepthStencilState(&dd, depth_.GetAddressOf()))) {
        return false;
    }

    // マスク: 深度テストなしでステンシルへ ref を書く (REPLACE)
    D3D11_DEPTH_STENCIL_DESC md = {};
    md.DepthEnable = FALSE;
    md.StencilEnable = TRUE;
    md.StencilReadMask = 0xFF;
    md.StencilWriteMask = 0xFF;
    md.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
    md.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
    md.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
    md.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
    md.BackFace = md.FrontFace;
    if (FAILED(dev->CreateDepthStencilState(&md, depthMaskWrite_.GetAddressOf()))) {
        return false;
    }

    // マスク対象: 通常の深度設定のまま、ステンシルが ref と等しい画素だけ通す (ステンシルは書き換えない)
    D3D11_DEPTH_STENCIL_DESC td = dd;
    td.StencilEnable = TRUE;
    td.StencilReadMask = 0xFF;
    td.StencilWriteMask = 0;
    td.FrontFace.StencilFunc = D3D11_COMPARISON_EQUAL;
    td.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
    td.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
    td.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
    td.BackFace = td.FrontFace;
    if (FAILED(dev->CreateDepthStencilState(&td, depthTestStencil_.GetAddressOf()))) {
        return false;
    }

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE; // flip で巻きが反転するため両面
    rd.DepthClipEnable = TRUE;
    if (FAILED(dev->CreateRasterizerState(&rd, raster_.GetAddressOf()))) {
        return false;
    }

    ready_ = true;
    return true;
}

void Sprite2DPass::Shutdown()
{
    cb_.Reset();
    psCb_.Reset();
    vb_.Reset();
    vbCapacity_ = 0; // 0 に戻さないと復旧後に vb_ 無しで Map する
    samplerLinear_.Reset();
    samplerPoint_.Reset();
    blend_.Reset();
    blendNoColor_.Reset();
    depth_.Reset();
    depthMaskWrite_.Reset();
    depthTestStencil_.Reset();
    raster_.Reset();
    ready_ = false;
}

bool Sprite2DPass::Upload(GraphicsDevice& device, const Sprite2DDrawList& list)
{
    const uint32_t needed = static_cast<uint32_t>(list.vertices.size());
    if (!vb_ || needed > vbCapacity_) {
        vbCapacity_ = needed + needed / 2 + kVertexCapacityMargin;
        D3D11_BUFFER_DESC vbd = {};
        vbd.ByteWidth = vbCapacity_ * sizeof(Sprite2DVertex);
        vbd.Usage = D3D11_USAGE_DYNAMIC;
        vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        vbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        vb_.Reset();
        if (FAILED(device.Device()->CreateBuffer(&vbd, nullptr, vb_.GetAddressOf()))) {
            vbCapacity_ = 0;
            return false;
        }
    }
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(device.Context()->Map(vb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return false;
    }
    std::memcpy(mapped.pData, list.vertices.data(), static_cast<size_t>(needed) * sizeof(Sprite2DVertex));
    device.Context()->Unmap(vb_.Get(), 0);
    return true;
}

uint32_t Sprite2DPass::Render(GraphicsDevice& device, ShaderManager& shaders, TextureLibrary& textures,
                              const Sprite2DDrawList& list, const XMFLOAT4X4& view, const XMFLOAT4X4& proj,
                              ID3D11DepthStencilView* stencilDsv)
{
    // Sprite2D の無いシーンはここで終わる (D3D の状態を一切触らない)
    if (!ready_ || list.Empty() || list.vertices.empty()) {
        return 0;
    }
    ShaderProgram* prog = shaders.Get(shader_);
    if (!prog || !prog->valid || !Upload(device, list)) {
        return 0;
    }
    ID3D11DeviceContext* dc = device.Context();

    // マスクがあるときだけ DSV を書き込み可能なものへ替え、ステンシルを 0 に戻す (深度は残す)。元の RT / DSV は終了時に戻す
    const bool useStencil = stencilDsv != nullptr && list.UsesStencil();
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> prevRtv;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> prevDsv;
    bool swappedDsv = false;
    if (useStencil) {
        dc->OMGetRenderTargets(1, prevRtv.GetAddressOf(), prevDsv.GetAddressOf());
        if (prevDsv.Get() != stencilDsv) {
            ID3D11RenderTargetView* rtv = prevRtv.Get();
            dc->OMSetRenderTargets(1, &rtv, stencilDsv);
            swappedDsv = true;
        }
        dc->ClearDepthStencilView(stencilDsv, D3D11_CLEAR_STENCIL, 1.0f, 0);
    }

    XMFLOAT4X4 viewProj;
    XMStoreFloat4x4(&viewProj,
                    XMMatrixTranspose(XMMatrixMultiply(XMLoadFloat4x4(&view), XMLoadFloat4x4(&proj))));
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(dc->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        std::memcpy(mapped.pData, &viewProj, sizeof(viewProj));
        dc->Unmap(cb_.Get(), 0);
    }

    const UINT stride = sizeof(Sprite2DVertex);
    const UINT offset = 0;
    ID3D11Buffer* vb = vb_.Get();
    dc->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dc->IASetInputLayout(prog->inputLayout.Get());
    dc->VSSetShader(prog->vs.Get(), nullptr, 0);
    dc->PSSetShader(prog->ps.Get(), nullptr, 0);
    ID3D11Buffer* cbs[1] = { cb_.Get() };
    dc->VSSetConstantBuffers(0, 1, cbs);
    ID3D11Buffer* psCbs[1] = { psCb_.Get() };
    dc->PSSetConstantBuffers(1, 1, psCbs);
    dc->RSSetState(raster_.Get());

    uint32_t drawn = 0;
    for (const Sprite2DBatch& b : list.batches) {
        const Texture* t = textures.Get(b.texture);
        if (t == nullptr || !t->srv || b.count == 0) {
            continue;
        }
        // ステンシルが使えないときはマスクを描かず、マスク対象は素通りで描く
        if (!useStencil && b.stencil == sprite2dstencil::kWriteMask) {
            continue;
        }
        const uint8_t mode = useStencil ? b.stencil : sprite2dstencil::kNone;
        PsCb ps = {};
        if (mode == sprite2dstencil::kWriteMask) {
            ps.maskClip[0] = 1.0f;
            ps.maskClip[1] = b.alphaCutoff;
            dc->OMSetBlendState(blendNoColor_.Get(), nullptr, 0xFFFFFFFFu);
            dc->OMSetDepthStencilState(depthMaskWrite_.Get(), kStencilMaskRef);
        } else {
            dc->OMSetBlendState(blend_.Get(), nullptr, 0xFFFFFFFFu);
            if (mode == sprite2dstencil::kTestInside) {
                dc->OMSetDepthStencilState(depthTestStencil_.Get(), kStencilMaskRef);
            } else if (mode == sprite2dstencil::kTestOutside) {
                dc->OMSetDepthStencilState(depthTestStencil_.Get(), kStencilOutsideRef);
            } else {
                dc->OMSetDepthStencilState(depth_.Get(), 0);
            }
        }
        if (SUCCEEDED(dc->Map(psCb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            std::memcpy(mapped.pData, &ps, sizeof(ps));
            dc->Unmap(psCb_.Get(), 0);
        }
        ID3D11SamplerState* samp = b.pointFilter ? samplerPoint_.Get() : samplerLinear_.Get();
        dc->PSSetSamplers(0, 1, &samp);
        ID3D11ShaderResourceView* srv = t->srv.Get();
        dc->PSSetShaderResources(0, 1, &srv);
        dc->Draw(b.count, b.start);
        ++drawn;
    }
    // 張った SRV は剥がす (次段が同じテクスチャを書き込み先に取ったとき D3D が黙って外すのを避ける)
    ID3D11ShaderResourceView* nullSrv[1] = { nullptr };
    dc->PSSetShaderResources(0, 1, nullSrv);
    if (useStencil) {
        // 次段へ通常のブレンド / 深度状態と元の DSV を返す
        dc->OMSetBlendState(blend_.Get(), nullptr, 0xFFFFFFFFu);
        dc->OMSetDepthStencilState(depth_.Get(), 0);
        if (swappedDsv) {
            ID3D11RenderTargetView* rtv = prevRtv.Get();
            dc->OMSetRenderTargets(1, &rtv, prevDsv.Get());
        }
    }
    return drawn;
}

} // namespace mye
