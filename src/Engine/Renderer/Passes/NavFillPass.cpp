//====================================================================================
//                          NavFillPass.cpp
//  MyEngin/ 秋田蓮音                                                     10/04/2026
//                                          ナビメッシュの半透明の塗りの描画パスの実装
//====================================================================================
#include "Engine/Renderer/Passes/NavFillPass.h"

#include <cstring>

#include "Engine/Renderer/Device/GraphicsDevice.h"
#include "Engine/Renderer/Shader/ShaderManager.h"

using namespace DirectX;

namespace mye {

namespace {
// 傾いた面でも塗りが床より手前に出るようにする深度バイアス (負 = 手前へ)
constexpr float kSlopeScaledDepthBias = -1.5f;
} // namespace

bool NavFillPass::Init(GraphicsDevice& device, ShaderManager& shaders)
{
    ID3D11Device* dev = device.Device();
    shader_ = shaders.Load("editor_line");

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = sizeof(XMFLOAT4X4);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateBuffer(&cbd, nullptr, cb_.GetAddressOf()))) {
        return false;
    }

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    rd.SlopeScaledDepthBias = kSlopeScaledDepthBias;
    if (FAILED(dev->CreateRasterizerState(&rd, raster_.GetAddressOf()))) {
        return false;
    }

    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; // 半透明は深度を書かない (後ろの物を隠さない)
    dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    if (FAILED(dev->CreateDepthStencilState(&dd, depth_.GetAddressOf()))) {
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

    ready_ = true;
    return true;
}

void NavFillPass::Shutdown()
{
    vb_.Reset();
    cb_.Reset();
    raster_.Reset();
    depth_.Reset();
    blend_.Reset();
    vbCapacity_ = 0;
    uploadedCount_ = 0;
    uploadedSerial_ = ~0ull;
    ready_ = false;
}

bool NavFillPass::Upload(GraphicsDevice& device, const void* vertices, uint32_t vertexCount, uint64_t serial)
{
    if (vb_ && uploadedSerial_ == serial && uploadedCount_ == vertexCount) {
        return true;
    }
    if (vertexCount > vbCapacity_ || !vb_) {
        vbCapacity_ = vertexCount + vertexCount / 2 + 256;
        D3D11_BUFFER_DESC vbd = {};
        vbd.ByteWidth = vbCapacity_ * kVertexStride;
        vbd.Usage = D3D11_USAGE_DYNAMIC;
        vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        vbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        vb_.Reset();
        uploadedSerial_ = ~0ull;
        if (FAILED(device.Device()->CreateBuffer(&vbd, nullptr, vb_.GetAddressOf()))) {
            return false;
        }
    }
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(device.Context()->Map(vb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return false;
    }
    std::memcpy(mapped.pData, vertices, static_cast<size_t>(vertexCount) * kVertexStride);
    device.Context()->Unmap(vb_.Get(), 0);
    uploadedSerial_ = serial;
    uploadedCount_ = vertexCount;
    return true;
}

void NavFillPass::Render(GraphicsDevice& device, ShaderManager& shaders, const void* vertices,
                         uint32_t vertexCount, uint64_t serial, ID3D11RenderTargetView* rtv,
                         ID3D11DepthStencilView* dsv, int width, int height, const XMFLOAT4X4& view,
                         const XMFLOAT4X4& proj)
{
    if (!ready_ || vertexCount == 0 || vertices == nullptr) {
        return;
    }
    ShaderProgram* prog = shaders.Get(shader_);
    if (!prog || !prog->valid || !Upload(device, vertices, vertexCount, serial)) {
        return;
    }
    ID3D11DeviceContext* dc = device.Context();

    dc->OMSetRenderTargets(1, &rtv, dsv);
    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(width);
    vp.Height = static_cast<float>(height);
    vp.MaxDepth = 1.0f;
    dc->RSSetViewports(1, &vp);

    XMFLOAT4X4 viewProj;
    XMStoreFloat4x4(&viewProj,
                    XMMatrixTranspose(XMMatrixMultiply(XMLoadFloat4x4(&view), XMLoadFloat4x4(&proj))));
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(dc->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        std::memcpy(mapped.pData, &viewProj, sizeof(viewProj));
        dc->Unmap(cb_.Get(), 0);
    }
    ID3D11Buffer* cbs[1] = { cb_.Get() };
    dc->VSSetConstantBuffers(0, 1, cbs);
    dc->RSSetState(raster_.Get());
    dc->OMSetBlendState(blend_.Get(), nullptr, 0xFFFFFFFFu);
    dc->OMSetDepthStencilState(depth_.Get(), 0);
    dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dc->IASetInputLayout(prog->inputLayout.Get());
    dc->VSSetShader(prog->vs.Get(), nullptr, 0);
    dc->PSSetShader(prog->ps.Get(), nullptr, 0);

    ID3D11Buffer* vb = vb_.Get();
    const UINT stride = kVertexStride;
    const UINT offset = 0;
    dc->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    dc->Draw(uploadedCount_, 0);
}

} // namespace mye
