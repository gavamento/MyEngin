#include "Engine/Renderer/WaterPass.h"

#include <DirectXMath.h>

#include "Engine/Renderer/GraphicsDevice.h"
#include "Engine/Renderer/ShaderManager.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/GpuBufferUtil.h"

namespace mye {

bool WaterPass::Init(GraphicsDevice& device, ShaderManager& shaders)
{
    ID3D11Device* dev = device.Device();
    if (!dev) {
        return false;
    }

    shader_ = shaders.Load("water_surface");

    if (!gpubuf::CreateConstant(dev, sizeof(PerObjectCB), objectCB_)) {
        return false;
    }
    if (!gpubuf::CreateConstant(dev, sizeof(WaterMaterialCB), materialCB_)) {
        return false;
    }

    // パイプラインステート構築
    // 1. ラスタライザ (両面描画 & ワイヤーフレーム)
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE; // 水面は上からも下からも描画
    rd.FrontCounterClockwise = FALSE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(dev->CreateRasterizerState(&rd, rasterizerCullNone_.GetAddressOf()))) {
        return false;
    }
    rd.FillMode = D3D11_FILL_WIREFRAME;
    if (FAILED(dev->CreateRasterizerState(&rd, rasterizerWire_.GetAddressOf()))) {
        return false;
    }

    // 2. 深度ステート (水面は水中オブジェクトを遮蔽しつつ手前パーティクルの基準とするため深度書き込み)
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    if (FAILED(dev->CreateDepthStencilState(&dd, depthState_.GetAddressOf()))) {
        return false;
    }

    // 3. 半透明ブレンドステート (α合成)
    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(dev->CreateBlendState(&bd, blendTransparent_.GetAddressOf()))) {
        return false;
    }

    // 4. サンプラーステート (s0: wrap, s1: shadow cmp, s2: clamp)
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateSamplerState(&sd, samplerLinear_.GetAddressOf()))) {
        return false;
    }
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    if (FAILED(dev->CreateSamplerState(&sd, samplerClamp_.GetAddressOf()))) {
        return false;
    }
    D3D11_SAMPLER_DESC ssd = {};
    ssd.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
    ssd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    ssd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    ssd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ssd.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
    if (FAILED(dev->CreateSamplerState(&ssd, samplerShadow_.GetAddressOf()))) {
        return false;
    }

    ready_ = true;
    return true;
}

void WaterPass::Shutdown()
{
    ready_ = false;
    objectCB_.Reset();
    materialCB_.Reset();
    blendTransparent_.Reset();
    depthState_.Reset();
    rasterizerCullNone_.Reset();
    rasterizerWire_.Reset();
    samplerLinear_.Reset();
    samplerShadow_.Reset();
    samplerClamp_.Reset();
}

void WaterPass::Render(GraphicsDevice& device, ShaderManager& shaders, const RenderView& view,
                       RenderResources& resources, ID3D11Buffer* perFrameCB,
                       ID3D11ShaderResourceView* rtReflSRV)
{
    // 水面情報がない、または非アクティブなシーンは何も触らず即座に早期 return
    // (従来シーンとのビット完全一致の担保)。
    // M79 sub-05: surfaceMaterial が解決済み (useSurfaceRoute) なら、水面は既に通常の
    // RenderItem 経路 (サーフェス/従来メッシュ) で描かれている — ここで重ねて描くと二重になる
    if (!ready_ || view.water == nullptr || !view.water->active || view.water->useSurfaceRoute) {
        return;
    }

    ShaderProgram* prog = shaders.Get(shader_);
    if (!prog || !prog->valid) {
        return;
    }

    ID3D11DeviceContext* dc = device.Context();
    if (!dc) {
        return;
    }

    // メッシュ取得 (高解像度 XZ 平面グリッド)
    AssetID meshId = resources.meshes.WaterPlane();
    Mesh* mesh = resources.meshes.Get(meshId);
    if (!mesh || !mesh->vb || !mesh->ib || mesh->indexCount == 0) {
        return;
    }

    // RTV / DSV 再バインド (Deferred のライトパス後は DSV が外れている場合がある)
    if (view.rtv && view.dsv) {
        dc->OMSetRenderTargets(1, &view.rtv, view.dsv);
    }

    // ビューポート
    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(view.width);
    vp.Height = static_cast<float>(view.height);
    vp.MaxDepth = 1.0f;
    dc->RSSetViewports(1, &vp);

    // ステート設定
    const bool wire = (view.debugViewMode == 2);
    dc->RSSetState(wire ? rasterizerWire_.Get() : rasterizerCullNone_.Get());
    dc->OMSetDepthStencilState(depthState_.Get(), 0);
    dc->OMSetBlendState(blendTransparent_.Get(), nullptr, 0xFFFFFFFFu);

    // スロット b0: DeferredPath で lightCB_/ssrCB 等により b0 が上書きされている場合、
    // 正しい PerFrameCB (カメラ ViewProj・カメラ位置・ライティング情報) を張り直す
    if (perFrameCB) {
        ID3D11Buffer* fcb[1] = { perFrameCB };
        dc->VSSetConstantBuffers(0, 1, fcb);
        dc->PSSetConstantBuffers(0, 1, fcb);
    }

    // 定数バッファの更新 (b1: PerObject, b2: WaterMaterialParams)
    PerObjectCB po = {};
    DirectX::XMMATRIX w = DirectX::XMLoadFloat4x4(&view.water->world);
    DirectX::XMStoreFloat4x4(&po.world, DirectX::XMMatrixTranspose(w));
    po.baseColor = { 1.0f, 1.0f, 1.0f, 1.0f };
    po.instanceBase = 0;
    po.rtReceiver = 0.0f;
    gpubuf::UploadCB(dc, objectCB_.Get(), po);

    // RT 反射有効フラグ (waterOptics.z) および スカイボックスモード (waterOptics.w: 0=Gradient, 1=Cube, 2=Pano) を通知
    WaterMaterialCB mat = view.water->material;
    mat.waterOptics.z = (rtReflSRV != nullptr) ? 1.0f : 0.0f;
    mat.waterOptics.w = static_cast<float>(view.skyMode);
    gpubuf::UploadCB(dc, materialCB_.Get(), mat);

    ID3D11Buffer* vscbs[2] = { objectCB_.Get(), materialCB_.Get() };
    dc->VSSetConstantBuffers(1, 2, vscbs);

    ID3D11Buffer* pscbs[2] = { objectCB_.Get(), materialCB_.Get() };
    dc->PSSetConstantBuffers(1, 2, pscbs);

    // パノラマ (2D) スカイテクスチャは t0 に、キューブマップは t4 にバインド
    ID3D11ShaderResourceView* panoSky = (view.skyMode == 2) ? view.skyCubemap : nullptr;
    ID3D11ShaderResourceView* prefiltered = view.iblPrefiltered;
    if (!prefiltered && view.skyMode == 1) {
        prefiltered = view.skyCubemap;
    }

    // テクスチャ SRV (t0: パノラマスカイ, t1: 影, t3-t5: IBL, t6: 局所影アトラス, t7: Froxel, t8: RT反射)
    ID3D11ShaderResourceView* srvs[9] = {
        panoSky,
        view.shadowSRV,
        nullptr,
        view.iblIrradiance,
        prefiltered,
        view.iblBrdfLut,
        view.shadowAtlasSRV,
        view.froxelSRV,
        rtReflSRV
    };
    dc->PSSetShaderResources(0, 9, srvs);

    // サンプラー (s0: wrap, s1: shadow, s2: ibl clamp)
    ID3D11SamplerState* samplers[3] = {
        samplerLinear_.Get(),
        samplerShadow_.Get(),
        samplerClamp_.Get()
    };
    dc->PSSetSamplers(0, 3, samplers);

    // シェーダー & 入力レイアウトバインド
    dc->IASetInputLayout(prog->inputLayout.Get());
    dc->VSSetShader(prog->vs.Get(), nullptr, 0);
    dc->PSSetShader(prog->ps.Get(), nullptr, 0);
    dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // 頂点バッファ・インデックスバッファバインド
    const UINT stride = sizeof(MeshVertex);
    const UINT offset = 0;
    ID3D11Buffer* vb = mesh->vb.Get();
    dc->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    dc->IASetIndexBuffer(mesh->ib.Get(), DXGI_FORMAT_R32_UINT, 0);

    // ドローコール発行！
    dc->DrawIndexed(mesh->indexCount, 0, 0);

    // 後始末: SRV 解除
    ID3D11ShaderResourceView* nullSrvs[9] = {};
    dc->PSSetShaderResources(0, 9, nullSrvs);
}

} // namespace mye
