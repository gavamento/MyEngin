#include "Engine/Renderer/ForwardPath.h"

#include "Engine/Core/Log.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/GraphicsDevice.h"
#include "Engine/Renderer/ShaderManager.h"

using namespace DirectX;

namespace mye {
namespace {

// HLSL 側は既定の column_major packing のため、書き込み前に転置する
struct PerFrameCB {
    XMFLOAT4X4 viewProj;
    XMFLOAT3 cameraPos;
    float pad0;
    XMFLOAT3 lightDir;
    float pad1;
    XMFLOAT3 lightColor;
    float lightIntensity;
    XMFLOAT3 ambient;
    float pad2;
};

struct PerObjectCB {
    XMFLOAT4X4 world;
    XMFLOAT4 baseColor;
};

bool CreateConstantBuffer(ID3D11Device* dev, UINT size, Microsoft::WRL::ComPtr<ID3D11Buffer>& out)
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

bool ForwardPath::Init(GraphicsDevice& device, ShaderManager& shaders)
{
    ID3D11Device* dev = device.Device();

    if (!CreateConstantBuffer(dev, sizeof(PerFrameCB), perFrameCB_)
        || !CreateConstantBuffer(dev, sizeof(PerObjectCB), perObjectCB_)) {
        MYE_LOG_ERROR("ForwardPath: constant buffer creation failed");
        return false;
    }

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_ANISOTROPIC;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxAnisotropy = 4;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateSamplerState(&sd, sampler_.GetAddressOf()))) {
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
    if (FAILED(dev->CreateDepthStencilState(&dd, depthOpaque_.GetAddressOf()))) {
        return false;
    }
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    if (FAILED(dev->CreateDepthStencilState(&dd, depthTransparent_.GetAddressOf()))) {
        return false;
    }

    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable = FALSE;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(dev->CreateBlendState(&bd, blendOpaque_.GetAddressOf()))) {
        return false;
    }
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    if (FAILED(dev->CreateBlendState(&bd, blendAlpha_.GetAddressOf()))) {
        return false;
    }

    (void)shaders;
    return true;
}

void ForwardPath::Shutdown()
{
    perFrameCB_.Reset();
    perObjectCB_.Reset();
    sampler_.Reset();
    rasterizer_.Reset();
    depthOpaque_.Reset();
    depthTransparent_.Reset();
    blendOpaque_.Reset();
    blendAlpha_.Reset();
}

void ForwardPath::Render(GraphicsDevice& device, const RenderView& view, const RenderQueue& queue,
                         const DirectionalLightData& light, RenderResources& resources,
                         ShaderManager& shaders)
{
    ID3D11DeviceContext* dc = device.Context();

    dc->OMSetRenderTargets(1, &view.rtv, view.dsv);
    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(view.width);
    vp.Height = static_cast<float>(view.height);
    vp.MaxDepth = 1.0f;
    dc->RSSetViewports(1, &vp);
    dc->ClearRenderTargetView(view.rtv, view.clearColor);
    if (view.dsv) {
        dc->ClearDepthStencilView(view.dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    }

    // フレーム共通 CB
    PerFrameCB pf = {};
    const XMMATRIX v = XMLoadFloat4x4(&view.view);
    const XMMATRIX p = XMLoadFloat4x4(&view.proj);
    XMStoreFloat4x4(&pf.viewProj, XMMatrixTranspose(XMMatrixMultiply(v, p)));
    pf.cameraPos = view.cameraPos;
    pf.lightDir = light.dir;
    pf.lightColor = light.color;
    pf.lightIntensity = light.intensity;
    pf.ambient = light.ambient;
    UploadCB(dc, perFrameCB_.Get(), pf);

    ID3D11Buffer* cbs[2] = { perFrameCB_.Get(), perObjectCB_.Get() };
    dc->VSSetConstantBuffers(0, 2, cbs);
    dc->PSSetConstantBuffers(0, 2, cbs);
    ID3D11SamplerState* samplers[1] = { sampler_.Get() };
    dc->PSSetSamplers(0, 1, samplers);
    dc->RSSetState(rasterizer_.Get());
    dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // 不透明
    dc->OMSetDepthStencilState(depthOpaque_.Get(), 0);
    dc->OMSetBlendState(blendOpaque_.Get(), nullptr, 0xFFFFFFFFu);
    DrawItems(device, queue.opaque, view, resources, shaders);

    // 半透明
    if (!queue.transparent.empty()) {
        dc->OMSetDepthStencilState(depthTransparent_.Get(), 0);
        dc->OMSetBlendState(blendAlpha_.Get(), nullptr, 0xFFFFFFFFu);
        DrawItems(device, queue.transparent, view, resources, shaders);
    }
}

void ForwardPath::DrawItems(GraphicsDevice& device, const std::vector<RenderItem>& items,
                            const RenderView& view, RenderResources& resources,
                            ShaderManager& shaders)
{
    (void)view;
    ID3D11DeviceContext* dc = device.Context();

    uint64_t boundShader = 0;
    uint64_t boundTexture = 0;
    uint64_t boundMesh = 0;

    for (const RenderItem& item : items) {
        Material* mat = resources.materials.Get(item.material);
        if (!mat) {
            continue;
        }
        Mesh* mesh = resources.meshes.Get(item.mesh);
        if (!mesh) {
            continue;
        }
        ShaderProgram* prog = shaders.Get(mat->shader);
        if (!prog || !prog->valid) {
            continue;
        }

        if (mat->shader.value != boundShader) {
            dc->IASetInputLayout(prog->inputLayout.Get());
            dc->VSSetShader(prog->vs.Get(), nullptr, 0);
            dc->PSSetShader(prog->ps.Get(), nullptr, 0);
            boundShader = mat->shader.value;
        }
        const AssetID texId = mat->texture.IsNull() ? resources.textures.White() : mat->texture;
        if (texId.value != boundTexture) {
            Texture* tex = resources.textures.Get(texId);
            ID3D11ShaderResourceView* srv = tex ? tex->srv.Get() : nullptr;
            dc->PSSetShaderResources(0, 1, &srv);
            boundTexture = texId.value;
        }
        if (item.mesh.value != boundMesh) {
            const UINT stride = sizeof(MeshVertex);
            const UINT offset = 0;
            ID3D11Buffer* vb = mesh->vb.Get();
            dc->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
            dc->IASetIndexBuffer(mesh->ib.Get(), DXGI_FORMAT_R32_UINT, 0);
            boundMesh = item.mesh.value;
        }

        PerObjectCB po = {};
        XMStoreFloat4x4(&po.world, XMMatrixTranspose(XMLoadFloat4x4(&item.world)));
        po.baseColor = mat->baseColor;
        UploadCB(dc, perObjectCB_.Get(), po);

        dc->DrawIndexed(mesh->indexCount, 0, 0);
    }
}

} // namespace mye
