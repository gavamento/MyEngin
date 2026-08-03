#include "Engine/Renderer/RayTracing/RtPasses.h"

#include "Engine/Core/Log.h"
#include "Engine/Renderer/GpuBufferUtil.h"
#include "Engine/Renderer/GraphicsDevice.h"
#include "Engine/Renderer/ShaderManager.h"

using namespace DirectX;

namespace mye {
namespace {

using namespace gpubuf;

// rt_common.hlsli の RtSceneCB (b0) と一致
struct RtSceneCB {
    int32_t instanceCount = 0;
    int32_t pad0 = 0;
    int32_t pad1 = 0;
    int32_t pad2 = 0;
};

// rt_debug.cs.hlsl の RtDebugCB (b1) と一致
struct RtDebugCB {
    XMFLOAT4X4 invViewProj = {};
    XMFLOAT3 cameraPos = { 0, 0, 0 };
    float tMax = 1000.0f;
    float screenSize[2] = { 0, 0 };
    int32_t debugMode = 0;
    float heatScale = 128.0f;
};
static_assert(sizeof(RtDebugCB) == 96, "HLSL の RtDebugCB と一致させること");

} // namespace

bool RtPasses::Init(GraphicsDevice& device, ShaderManager& shaders)
{
    if (inited_) {
        return true;
    }
    ID3D11Device* dev = device.Device();

    debugCS_ = shaders.LoadCompute("rt_debug.cs");
    blitShader_ = shaders.Load("rt_blit");

    if (!CreateConstant(dev, sizeof(RtSceneCB), sceneCB_)
        || !CreateConstant(dev, sizeof(RtDebugCB), debugCB_)) {
        MYE_LOG_ERROR("RtPasses: constant buffer creation failed");
        return false;
    }

    D3D11_DEPTH_STENCIL_DESC dsd = {};
    dsd.DepthEnable = FALSE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dsd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(dev->CreateDepthStencilState(&dsd, depthDisabled_.GetAddressOf()))
        || FAILED(dev->CreateBlendState(&bd, blendOpaque_.GetAddressOf()))
        || FAILED(dev->CreateRasterizerState(&rd, raster_.GetAddressOf()))) {
        MYE_LOG_ERROR("RtPasses: pipeline state creation failed");
        return false;
    }

    debugTimer_.Init(device);
    inited_ = true;
    return true;
}

void RtPasses::Shutdown()
{
    debugRt_.Release();
    sceneCB_.Reset();
    debugCB_.Reset();
    depthDisabled_.Reset();
    blendOpaque_.Reset();
    raster_.Reset();
    inited_ = false;
}

bool RtPasses::RenderDebug(GraphicsDevice& device, ShaderManager& shaders, const RenderView& view,
                           const RtSceneBindings& scene)
{
    if (!inited_ || view.rtDebugMode == 0 || !scene.IsValid() || view.rtv == nullptr
        || view.width <= 0 || view.height <= 0) {
        return false;
    }
    ShaderProgram* csProg = shaders.Get(debugCS_);
    ShaderProgram* blitProg = shaders.Get(blitShader_);
    if (!csProg || !csProg->valid || !csProg->cs || !blitProg || !blitProg->valid) {
        return false; // コンパイル失敗時は何も描かない (従来の絵が残る)
    }

    // 出力先はフル解像度。UAV 非対応環境なら Resize が失敗するのでそこで諦める
    debugRt_.Resize(device, view.width, view.height, DXGI_FORMAT_R16G16B16A16_FLOAT,
                    /*withDepth=*/false, /*withUav=*/true);
    if (!debugRt_.UAV()) {
        return false;
    }

    ID3D11DeviceContext* dc = device.Context();
    debugTimer_.Begin(device);

    RtSceneCB sc = {};
    sc.instanceCount = scene.instanceCount;
    UploadCB(dc, sceneCB_.Get(), sc);

    RtDebugCB db = {};
    const XMMATRIX vp = XMLoadFloat4x4(&view.view) * XMLoadFloat4x4(&view.proj);
    XMStoreFloat4x4(&db.invViewProj, XMMatrixTranspose(XMMatrixInverse(nullptr, vp)));
    db.cameraPos = view.cameraPos;
    db.tMax = (view.farZ > 0.0f) ? view.farZ : 1000.0f;
    db.screenSize[0] = static_cast<float>(view.width);
    db.screenSize[1] = static_cast<float>(view.height);
    db.debugMode = view.rtDebugMode;
    db.heatScale = 128.0f;
    UploadCB(dc, debugCB_.Get(), db);

    // ---- コンピュート: プライマリレイを撃つ ----
    ID3D11Buffer* cbs[2] = { sceneCB_.Get(), debugCB_.Get() };
    dc->CSSetConstantBuffers(0, 2, cbs);
    ID3D11ShaderResourceView* srvs[6] = { scene.nodes, scene.tris,      scene.attrs,
                                          scene.tlas,  scene.instances, scene.materials };
    dc->CSSetShaderResources(0, 6, srvs);
    ID3D11UnorderedAccessView* uavs[1] = { debugRt_.UAV() };
    dc->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    dc->CSSetShader(csProg->cs.Get(), nullptr, 0);
    dc->Dispatch(static_cast<UINT>((view.width + 7) / 8), static_cast<UINT>((view.height + 7) / 8),
                 1);
    // 次のパスで同じテクスチャを SRV として読むので必ず外す
    ID3D11ShaderResourceView* nullSrvs[6] = {};
    ID3D11UnorderedAccessView* nullUavs[1] = {};
    dc->CSSetShaderResources(0, 6, nullSrvs);
    dc->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);
    dc->CSSetShader(nullptr, nullptr, 0);

    // ---- ブリット: 結果をシーンの上に貼る ----
    D3D11_VIEWPORT vpDesc = {};
    vpDesc.Width = static_cast<float>(view.width);
    vpDesc.Height = static_cast<float>(view.height);
    vpDesc.MaxDepth = 1.0f;
    ID3D11RenderTargetView* rtvs[1] = { view.rtv };
    dc->OMSetRenderTargets(1, rtvs, nullptr);
    dc->RSSetViewports(1, &vpDesc);
    dc->RSSetState(raster_.Get());
    dc->OMSetDepthStencilState(depthDisabled_.Get(), 0);
    dc->OMSetBlendState(blendOpaque_.Get(), nullptr, 0xFFFFFFFFu);
    dc->IASetInputLayout(nullptr);
    dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11ShaderResourceView* blitSrv[1] = { debugRt_.SRV() };
    dc->PSSetShaderResources(0, 1, blitSrv);
    dc->VSSetShader(blitProg->vs.Get(), nullptr, 0);
    dc->PSSetShader(blitProg->ps.Get(), nullptr, 0);
    dc->Draw(3, 0);
    ID3D11ShaderResourceView* nullBlit[1] = {};
    dc->PSSetShaderResources(0, 1, nullBlit);

    debugTimer_.End(device);
    return true;
}

} // namespace mye
