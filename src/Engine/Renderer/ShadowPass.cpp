#include "Engine/Renderer/ShadowPass.h"

#include "Engine/Core/Log.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/GraphicsDevice.h"
#include "Engine/Renderer/RenderTypes.h"
#include "Engine/Renderer/ShaderManager.h"

using namespace DirectX;

namespace mye {
namespace {

struct ShadowObjectCB {
    XMFLOAT4X4 mvp; // transpose(world * lightViewProj)
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

} // namespace

bool ShadowPass::Init(GraphicsDevice& device, ShaderManager& shaders, int resolution)
{
    resolution_ = resolution;
    ID3D11Device* dev = device.Device();

    depthShader_ = shaders.Load("shadow_depth");

    // 深度テクスチャ: TYPELESS で作り DSV(D32_FLOAT) と SRV(R32_FLOAT) を両方生成
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = static_cast<UINT>(resolution);
    td.Height = static_cast<UINT>(resolution);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32_TYPELESS;
    td.SampleDesc = { 1, 0 };
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, tex_.GetAddressOf()))) {
        MYE_LOG_ERROR("ShadowPass: depth texture creation failed");
        return false;
    }
    D3D11_DEPTH_STENCIL_VIEW_DESC dvd = {};
    dvd.Format = DXGI_FORMAT_D32_FLOAT;
    dvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    if (FAILED(dev->CreateDepthStencilView(tex_.Get(), &dvd, dsv_.GetAddressOf()))) {
        return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC svd = {};
    svd.Format = DXGI_FORMAT_R32_FLOAT;
    svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    svd.Texture2D.MipLevels = 1;
    if (FAILED(dev->CreateShaderResourceView(tex_.Get(), &svd, srv_.GetAddressOf()))) {
        return false;
    }

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(ShadowObjectCB);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, objectCB_.GetAddressOf()))) {
        return false;
    }

    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    if (FAILED(dev->CreateDepthStencilState(&dd, depthState_.GetAddressOf()))) {
        return false;
    }

    // 深度バイアス (シャドウアクネ低減)。傾斜依存 + 定数。
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_BACK;
    rd.DepthClipEnable = TRUE;
    rd.DepthBias = 800;
    rd.SlopeScaledDepthBias = 2.5f;
    rd.DepthBiasClamp = 0.0f;
    if (FAILED(dev->CreateRasterizerState(&rd, rasterizer_.GetAddressOf()))) {
        return false;
    }

    ready_ = true;
    return true;
}

void ShadowPass::Render(GraphicsDevice& device, ShaderManager& shaders, const RenderQueue& queue,
                        RenderResources& resources, const XMFLOAT4X4& lightViewProj)
{
    ShaderProgram* prog = shaders.Get(depthShader_);
    if (!ready_ || !prog || !prog->valid) {
        return;
    }
    ID3D11DeviceContext* dc = device.Context();

    // シャドウテクスチャが前フレームから SRV に残っていると DSV へ束ねられない → 先に解除
    ID3D11ShaderResourceView* nullSrvs[4] = { nullptr, nullptr, nullptr, nullptr };
    dc->PSSetShaderResources(0, 4, nullSrvs);

    ID3D11RenderTargetView* noRtv[1] = { nullptr };
    dc->OMSetRenderTargets(1, noRtv, dsv_.Get());
    dc->ClearDepthStencilView(dsv_.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(resolution_);
    vp.Height = static_cast<float>(resolution_);
    vp.MaxDepth = 1.0f;
    dc->RSSetViewports(1, &vp);
    dc->OMSetDepthStencilState(depthState_.Get(), 0);
    dc->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
    dc->RSSetState(rasterizer_.Get());
    dc->IASetInputLayout(prog->inputLayout.Get());
    dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dc->VSSetShader(prog->vs.Get(), nullptr, 0);
    dc->PSSetShader(nullptr, nullptr, 0); // 深度のみ
    ID3D11Buffer* cbs[1] = { objectCB_.Get() };
    dc->VSSetConstantBuffers(0, 1, cbs);

    const XMMATRIX lvp = XMLoadFloat4x4(&lightViewProj);
    uint64_t boundMesh = 0;
    for (const RenderItem& item : queue.opaque) {
        Mesh* mesh = resources.meshes.Get(item.mesh);
        if (!mesh) {
            continue;
        }
        ShadowObjectCB cb;
        XMStoreFloat4x4(&cb.mvp, XMMatrixTranspose(XMMatrixMultiply(XMLoadFloat4x4(&item.world), lvp)));
        UploadCB(dc, objectCB_.Get(), cb);
        if (item.mesh.value != boundMesh) {
            const UINT stride = sizeof(MeshVertex);
            const UINT offset = 0;
            ID3D11Buffer* vb = mesh->vb.Get();
            dc->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
            dc->IASetIndexBuffer(mesh->ib.Get(), DXGI_FORMAT_R32_UINT, 0);
            boundMesh = item.mesh.value;
        }
        dc->DrawIndexed(mesh->indexCount, 0, 0);
    }

    // SRV として使う前に DSV バインドを解除 (同一リソースの DSV と SRV 同時バインド禁止)
    dc->OMSetRenderTargets(1, noRtv, nullptr);
}

} // namespace mye
