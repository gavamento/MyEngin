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
    // ---- インスタンシング (M38f、末尾 append)。インスタンス版のみ参照
    //      (このとき mvp は transpose(lightViewProj) 単体 = world を含まない) ----
    int32_t instanceBase;
    float instPad[3];
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
    depthInstancedShader_ = shaders.Load("shadow_depth_instanced"); // M38f

    // 深度テクスチャ: TYPELESS の Texture2DArray (M38d カスケード) で作り、
    // DSV(D32_FLOAT) はスライス毎、SRV(R32_FLOAT) は配列全体で生成
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = static_cast<UINT>(resolution);
    td.Height = static_cast<UINT>(resolution);
    td.MipLevels = 1;
    td.ArraySize = kCascades;
    td.Format = DXGI_FORMAT_R32_TYPELESS;
    td.SampleDesc = { 1, 0 };
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, tex_.GetAddressOf()))) {
        MYE_LOG_ERROR("ShadowPass: depth texture creation failed");
        return false;
    }
    for (int c = 0; c < kCascades; ++c) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dvd = {};
        dvd.Format = DXGI_FORMAT_D32_FLOAT;
        dvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
        dvd.Texture2DArray.FirstArraySlice = static_cast<UINT>(c);
        dvd.Texture2DArray.ArraySize = 1;
        if (FAILED(dev->CreateDepthStencilView(tex_.Get(), &dvd, dsv_[c].GetAddressOf()))) {
            return false;
        }
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC svd = {};
    svd.Format = DXGI_FORMAT_R32_FLOAT;
    svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    svd.Texture2DArray.MipLevels = 1;
    svd.Texture2DArray.ArraySize = kCascades;
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

    timer_.Init(device); // M54d: 失敗しても計測が 0 になるだけなので戻り値は見ない

    ready_ = true;
    return true;
}

void ShadowPass::Render(GraphicsDevice& device, ShaderManager& shaders, const RenderQueue& queue,
                        RenderResources& resources, const XMFLOAT4X4* lightViewProjs, int count,
                        bool instancing)
{
    ShaderProgram* prog = shaders.Get(depthShader_);
    if (!ready_ || !prog || !prog->valid || lightViewProjs == nullptr || count <= 0) {
        return;
    }
    if (count > kCascades) {
        count = kCascades;
    }
    ID3D11DeviceContext* dc = device.Context();

    // インスタンス run 検出 (M38f)。run とバッファ充填はカスケード間で共通なので 1 回だけ。
    // 判定は本描画パスと同じ (material,mesh) の連続 run — シャドウは material 無関係だが
    // ソートキー由来の境界をそのまま使う (保守的だが正しく、run 構築関数を共有できる)
    runs_.clear();
    worlds_.clear();
    ShaderProgram* instProg = shaders.Get(depthInstancedShader_);
    if (instancing && instProg && instProg->valid) {
        canInstance_.resize(queue.opaque.size());
        for (size_t i = 0; i < queue.opaque.size(); ++i) {
            const RenderItem& it = queue.opaque[i];
            canInstance_[i] = (it.bones == nullptr && resources.meshes.Get(it.mesh)) ? 1 : 0;
        }
        BuildInstanceRuns(queue.opaque, canInstance_, runs_, worlds_);
        if (worlds_.empty() || !instanceBuf_.Upload(device, worlds_)) {
            runs_.clear();
        }
    }

    timer_.Begin(device); // M54d

    // シャドウテクスチャが前フレームから SRV に残っていると DSV へ束ねられない → 先に解除
    ID3D11ShaderResourceView* nullSrvs[8] = {};
    dc->PSSetShaderResources(0, 8, nullSrvs);

    ID3D11RenderTargetView* noRtv[1] = { nullptr };
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
    if (!runs_.empty()) {
        ID3D11ShaderResourceView* isrv = instanceBuf_.SRV();
        dc->VSSetShaderResources(0, 1, &isrv);
    }

    // M38d: カスケード毎にスライス DSV へ全不透明キャスターを描く
    uint64_t boundShader = depthShader_.value; // 上で prog を bind 済み
    for (int c = 0; c < count; ++c) {
        dc->OMSetRenderTargets(1, noRtv, dsv_[c].Get());
        dc->ClearDepthStencilView(dsv_[c].Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
        const XMMATRIX lvp = XMLoadFloat4x4(&lightViewProjs[c]);
        uint64_t boundMesh = 0;
        size_t nextRun = 0;
        for (size_t idx = 0; idx < queue.opaque.size(); ++idx) {
            const RenderItem& item = queue.opaque[idx];
            Mesh* mesh = resources.meshes.Get(item.mesh);
            if (!mesh) {
                continue;
            }
            // インスタンス run の先頭なら一括描画 (M38f)
            if (nextRun < runs_.size() && runs_[nextRun].first == idx) {
                const MeshInstanceRun& run = runs_[nextRun];
                ++nextRun;
                if (depthInstancedShader_.value != boundShader) {
                    dc->IASetInputLayout(instProg->inputLayout.Get());
                    dc->VSSetShader(instProg->vs.Get(), nullptr, 0);
                    boundShader = depthInstancedShader_.value;
                }
                ShadowObjectCB cb = {};
                XMStoreFloat4x4(&cb.mvp, XMMatrixTranspose(lvp)); // world はインスタンス側
                cb.instanceBase = static_cast<int32_t>(run.base);
                UploadCB(dc, objectCB_.Get(), cb);
                if (item.mesh.value != boundMesh) {
                    const UINT stride = sizeof(MeshVertex);
                    const UINT offset = 0;
                    ID3D11Buffer* vb = mesh->vb.Get();
                    dc->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
                    dc->IASetIndexBuffer(mesh->ib.Get(), DXGI_FORMAT_R32_UINT, 0);
                    boundMesh = item.mesh.value;
                }
                dc->DrawIndexedInstanced(mesh->indexCount, run.count, 0, 0, 0);
                idx += run.count - 1; // for の ++idx と合わせて run 全体を飛ばす
                continue;
            }
            if (depthShader_.value != boundShader) {
                dc->IASetInputLayout(prog->inputLayout.Get());
                dc->VSSetShader(prog->vs.Get(), nullptr, 0);
                boundShader = depthShader_.value;
            }
            ShadowObjectCB cb = {};
            XMStoreFloat4x4(&cb.mvp,
                            XMMatrixTranspose(XMMatrixMultiply(XMLoadFloat4x4(&item.world), lvp)));
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
    }

    timer_.End(device); // M54d

    // インスタンス SRV を外す (次フレームの Map と競合させない、M38f)
    if (!runs_.empty()) {
        ID3D11ShaderResourceView* nullVsSrv = nullptr;
        dc->VSSetShaderResources(0, 1, &nullVsSrv);
    }
    // SRV として使う前に DSV バインドを解除 (同一リソースの DSV と SRV 同時バインド禁止)
    dc->OMSetRenderTargets(1, noRtv, nullptr);
}

} // namespace mye
