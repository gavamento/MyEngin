#include "Engine/Renderer/PickingPass.h"

#include "Engine/Core/Components.h"
#include "Engine/Core/World.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/GraphicsDevice.h"
#include "Engine/Renderer/ShaderManager.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

namespace mye {
namespace {

struct PickPerFrameCB {
    XMFLOAT4X4 viewProj;
};

struct PickPerObjectCB {
    XMFLOAT4X4 world;
    uint32_t id;
    uint32_t pad[3];
};

bool CreateDynamicCB(ID3D11Device* dev, UINT size, ComPtr<ID3D11Buffer>& out)
{
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = size;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(dev->CreateBuffer(&bd, nullptr, out.GetAddressOf()));
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

} // namespace

bool PickingPass::Init(GraphicsDevice& device, ShaderManager& shaders)
{
    ID3D11Device* dev = device.Device();
    shader_ = shaders.Load("picking");

    if (!CreateDynamicCB(dev, sizeof(PickPerFrameCB), perFrameCB_)
        || !CreateDynamicCB(dev, sizeof(PickPerObjectCB), perObjectCB_)) {
        return false;
    }

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_BACK;
    rd.FrontCounterClockwise = FALSE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(dev->CreateRasterizerState(&rd, rasterizer_.GetAddressOf()))) {
        return false;
    }

    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    if (FAILED(dev->CreateDepthStencilState(&dd, depth_.GetAddressOf()))) {
        return false;
    }

    // UINT レンダーターゲットはブレンド非対応。既定 (nullptr) 状態だと D3D デバッグレイヤが
    // 誤検知エラーを出すため、ブレンド無効を明示した状態をバインドする
    D3D11_BLEND_DESC bld = {};
    bld.RenderTarget[0].BlendEnable = FALSE;
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(dev->CreateBlendState(&bld, blendOff_.GetAddressOf()))) {
        return false;
    }

    ready_ = true;
    return true;
}

void PickingPass::Shutdown()
{
    target_.Release();
    staging_.Reset();
    perFrameCB_.Reset();
    perObjectCB_.Reset();
    rasterizer_.Reset();
    depth_.Reset();
    ready_ = false;
}

EntityID PickingPass::Pick(GraphicsDevice& device, World& world, ShaderManager& shaders,
                           RenderResources& resources, const XMFLOAT4X4& view,
                           const XMFLOAT4X4& proj, int width, int height, int px, int py)
{
    if (!ready_ || width <= 0 || height <= 0) {
        return kNullEntity;
    }
    if (px < 0 || py < 0 || px >= width || py >= height) {
        return kNullEntity;
    }
    ShaderProgram* prog = shaders.Get(shader_);
    if (!prog || !prog->valid) {
        return kNullEntity;
    }

    ID3D11Device* dev = device.Device();
    ID3D11DeviceContext* dc = device.Context();

    target_.Resize(device, width, height, DXGI_FORMAT_R32_UINT, true);
    if (!target_.IsValid()) {
        return kNullEntity;
    }
    if (!staging_) {
        D3D11_TEXTURE2D_DESC sd = {};
        sd.Width = 1;
        sd.Height = 1;
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        sd.Format = DXGI_FORMAT_R32_UINT;
        sd.SampleDesc = { 1, 0 };
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev->CreateTexture2D(&sd, nullptr, staging_.GetAddressOf()))) {
            return kNullEntity;
        }
    }

    // ---- ID 描画 ----
    ID3D11RenderTargetView* rtv = target_.RTV();
    ID3D11DepthStencilView* dsv = target_.DSV();
    dc->OMSetRenderTargets(1, &rtv, dsv);
    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(width);
    vp.Height = static_cast<float>(height);
    vp.MaxDepth = 1.0f;
    dc->RSSetViewports(1, &vp);
    const float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f }; // ID=0 でクリア
    dc->ClearRenderTargetView(rtv, clear);
    dc->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);

    PickPerFrameCB pf = {};
    XMStoreFloat4x4(&pf.viewProj,
                    XMMatrixTranspose(XMMatrixMultiply(XMLoadFloat4x4(&view), XMLoadFloat4x4(&proj))));
    UploadCB(dc, perFrameCB_.Get(), pf);

    ID3D11Buffer* cbs[2] = { perFrameCB_.Get(), perObjectCB_.Get() };
    dc->VSSetConstantBuffers(0, 2, cbs);
    dc->PSSetConstantBuffers(0, 2, cbs);
    dc->RSSetState(rasterizer_.Get());
    dc->OMSetDepthStencilState(depth_.Get(), 0);
    dc->OMSetBlendState(blendOff_.Get(), nullptr, 0xFFFFFFFFu);
    dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dc->IASetInputLayout(prog->inputLayout.Get());
    dc->VSSetShader(prog->vs.Get(), nullptr, 0);
    dc->PSSetShader(prog->ps.Get(), nullptr, 0);

    idMap_.clear();
    const ComponentTypeId req[] = { MeshRendererComponent::sTypeId, WorldMatrixComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int mi = arch.FindTypeIndex(MeshRendererComponent::sTypeId);
        const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const auto* mr = static_cast<const MeshRendererComponent*>(arch.GetPtr(mi, row));
            Mesh* mesh = resources.meshes.Get(mr->mesh);
            if (!mesh) {
                continue;
            }
            idMap_.push_back(arch.EntityAt(row));
            const uint32_t id = static_cast<uint32_t>(idMap_.size()); // 1 始まり

            PickPerObjectCB po = {};
            const auto* wmc = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row));
            XMStoreFloat4x4(&po.world, XMMatrixTranspose(XMLoadFloat4x4(&wmc->value)));
            po.id = id;
            UploadCB(dc, perObjectCB_.Get(), po);

            const UINT stride = sizeof(MeshVertex);
            const UINT offset = 0;
            ID3D11Buffer* vb = mesh->vb.Get();
            dc->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
            dc->IASetIndexBuffer(mesh->ib.Get(), DXGI_FORMAT_R32_UINT, 0);
            dc->DrawIndexed(mesh->indexCount, 0, 0);
        }
    });

    // ---- 1 ピクセル読み戻し (クリック時のみ = stall 許容) ----
    ComPtr<ID3D11Resource> colorRes;
    rtv->GetResource(colorRes.GetAddressOf());
    const D3D11_BOX box = { static_cast<UINT>(px), static_cast<UINT>(py), 0,
                            static_cast<UINT>(px) + 1, static_cast<UINT>(py) + 1, 1 };
    dc->CopySubresourceRegion(staging_.Get(), 0, 0, 0, 0, colorRes.Get(), 0, &box);

    uint32_t hitId = 0;
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(dc->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        hitId = *reinterpret_cast<const uint32_t*>(mapped.pData);
        dc->Unmap(staging_.Get(), 0);
    }
    device.PumpDebugMessages();

    if (hitId == 0 || hitId > idMap_.size()) {
        return kNullEntity;
    }
    return idMap_[hitId - 1];
}

} // namespace mye
