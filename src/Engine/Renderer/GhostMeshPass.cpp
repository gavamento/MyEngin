#include "Engine/Renderer/GhostMeshPass.h"

#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/GraphicsDevice.h"
#include "Engine/Renderer/ShaderManager.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

namespace mye {
namespace {

// ghost_mesh.hlsl の cbuffer と同じ並び (CPU 側で転置してアップロード)
struct GhostPerFrameCB {
    XMFLOAT4X4 viewProj;
    XMFLOAT4 lightDir;
};

struct GhostPerObjectCB {
    XMFLOAT4X4 world;
    XMFLOAT4 color;
};

bool CreateDynamicCB(ID3D11Device* dev, UINT size, ComPtr<ID3D11Buffer>& out)
{
    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = size;
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(dev->CreateBuffer(&cbd, nullptr, out.GetAddressOf()));
}

template <typename T>
void UploadCB(ID3D11DeviceContext* dc, ID3D11Buffer* cb, const T& data)
{
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(dc->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, &data, sizeof(T));
        dc->Unmap(cb, 0);
    }
}

XMFLOAT4 Unpack(uint32_t rgba)
{
    return XMFLOAT4(((rgba >> 24) & 0xFF) / 255.0f, ((rgba >> 16) & 0xFF) / 255.0f,
                    ((rgba >> 8) & 0xFF) / 255.0f, (rgba & 0xFF) / 255.0f);
}

} // namespace

bool GhostMeshPass::Init(GraphicsDevice& device, ShaderManager& shaders)
{
    ID3D11Device* dev = device.Device();
    shader_ = shaders.Load("ghost_mesh");
    if (!CreateDynamicCB(dev, sizeof(GhostPerFrameCB), perFrameCB_)
        || !CreateDynamicCB(dev, sizeof(GhostPerObjectCB), perObjectCB_)) {
        return false;
    }

    // 裏面も描く: 半透明なので裏面が抜けると「殻」に見えて形が読みにくい
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(dev->CreateRasterizerState(&rd, raster_.GetAddressOf()))) {
        return false;
    }

    // 深度テストあり (ライブの壁の向こうは見えない) ・深度書き込みなし (シーンを汚さない)
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
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

void GhostMeshPass::Shutdown()
{
    items_.clear();
    perFrameCB_.Reset();
    perObjectCB_.Reset();
    raster_.Reset();
    depth_.Reset();
    blend_.Reset();
    ready_ = false;
}

void GhostMeshPass::Begin()
{
    items_.clear();
}

void GhostMeshPass::Add(const Mesh* mesh, const XMFLOAT4X4& world, uint32_t rgba)
{
    if (mesh == nullptr || mesh->vb == nullptr || mesh->ib == nullptr || mesh->indexCount == 0) {
        return;
    }
    Item it;
    it.mesh = mesh;
    it.world = world;
    it.color = Unpack(rgba);
    items_.push_back(it);
}

void GhostMeshPass::Render(GraphicsDevice& device, ShaderManager& shaders, ID3D11RenderTargetView* rtv,
                           ID3D11DepthStencilView* dsv, int width, int height,
                           const XMFLOAT4X4& view, const XMFLOAT4X4& proj)
{
    if (!ready_ || items_.empty()) {
        return;
    }
    ShaderProgram* prog = shaders.Get(shader_);
    if (prog == nullptr || !prog->valid) {
        return;
    }
    ID3D11DeviceContext* dc = device.Context();
    dc->OMSetRenderTargets(1, &rtv, dsv);
    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(width);
    vp.Height = static_cast<float>(height);
    vp.MaxDepth = 1.0f;
    dc->RSSetViewports(1, &vp);

    GhostPerFrameCB pf;
    XMStoreFloat4x4(&pf.viewProj,
                    XMMatrixTranspose(XMMatrixMultiply(XMLoadFloat4x4(&view), XMLoadFloat4x4(&proj))));
    // 固定の光 (斜め上から)。見た目の陰影だけなのでシーンのライトは読まない
    XMStoreFloat4(&pf.lightDir, XMVector3Normalize(XMVectorSet(0.4f, 0.8f, -0.45f, 0.0f)));
    UploadCB(dc, perFrameCB_.Get(), pf);

    ID3D11Buffer* cbs[2] = { perFrameCB_.Get(), perObjectCB_.Get() };
    dc->VSSetConstantBuffers(0, 2, cbs);
    dc->PSSetConstantBuffers(0, 2, cbs);
    dc->RSSetState(raster_.Get());
    dc->OMSetDepthStencilState(depth_.Get(), 0);
    dc->OMSetBlendState(blend_.Get(), nullptr, 0xFFFFFFFFu);
    dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dc->IASetInputLayout(prog->inputLayout.Get());
    dc->VSSetShader(prog->vs.Get(), nullptr, 0);
    dc->PSSetShader(prog->ps.Get(), nullptr, 0);

    // ★stride は MeshVertex 全体 (52B)。シェーダは先頭 2 要素しか宣言しないが VB は共有
    const UINT stride = sizeof(MeshVertex);
    const UINT offset = 0;
    for (const Item& it : items_) {
        GhostPerObjectCB po;
        XMStoreFloat4x4(&po.world, XMMatrixTranspose(XMLoadFloat4x4(&it.world)));
        po.color = it.color;
        UploadCB(dc, perObjectCB_.Get(), po);
        ID3D11Buffer* vb = it.mesh->vb.Get();
        dc->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        dc->IASetIndexBuffer(it.mesh->ib.Get(), DXGI_FORMAT_R32_UINT, 0);
        dc->DrawIndexed(it.mesh->indexCount, 0, 0);
    }
    dc->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
}

} // namespace mye
