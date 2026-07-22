#include "Engine/Renderer/DeferredPath.h"

#include "Engine/Core/Log.h"
#include "Engine/Core/Profiler.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/GraphicsDevice.h"
#include "Engine/Renderer/ShaderManager.h"

using namespace DirectX;

namespace mye {
namespace {

// ボーンパレット最大数 (deferred_gbuffer_skinned.hlsl / forward_skinned.hlsl の MYE_MAX_BONES と一致)
constexpr int kMaxBones = 64;

// ForwardPath と同一レイアウト (forward_lit.hlsl を透明後段でそのまま使うため)
struct PerFrameCB {
    XMFLOAT4X4 viewProj;
    XMFLOAT3 cameraPos;
    int32_t lightCount;
    XMFLOAT3 ambient;
    float pad0;
    GpuLight lights[kMaxLights];
    XMFLOAT4X4 shadowVP;
    float shadowTexel;
    int32_t shadowEnabled;
    float pad1[2];
    // ---- フォグ (M29d、末尾 append)。透明後段の forward_lit が参照 ----
    XMFLOAT3 fogColor;
    int32_t fogMode; // -1=無効
    float fogDensity;
    float fogStart;
    float fogEnd;
    float fogPad;
    // ---- IBL (M38c、末尾 append) ----
    int32_t iblEnabled;
    float iblSpecMips;
    float iblPad[2];
    // ---- CSM (M38d、末尾 append)。shadowVP はカスケード 0 として温存 ----
    XMFLOAT4X4 shadowVP12[2];
    float cascadeInfo[4]; // xyz = split far 境界 / w = カスケード数
};

struct PerObjectCB {
    XMFLOAT4X4 world;
    XMFLOAT4 baseColor;
};

// deferred_gbuffer.hlsl / forward_lit.hlsl の MaterialParams (b2) と一致 (16 バイト)
struct MaterialCB {
    float metallic;
    float roughness;
    int32_t hasNormal; // 0=ノーマルマップ無し
    float pad0;
};

// deferred_light.hlsl の LightPass と同一レイアウト
struct LightPassCB {
    XMFLOAT3 ambient;
    int32_t lightCount;
    XMFLOAT4 clearColor;
    GpuLight lights[kMaxLights];
    XMFLOAT4X4 shadowVP;
    float shadowTexel;
    int32_t shadowEnabled;
    float pad1[2];
    XMFLOAT3 cameraPos;
    float pad2;
    // ---- フォグ (M29d、末尾 append) ----
    XMFLOAT3 fogColor;
    int32_t fogMode; // -1=無効
    float fogDensity;
    float fogStart;
    float fogEnd;
    float fogPad;
    // ---- IBL (M38c、末尾 append) ----
    int32_t iblEnabled;
    float iblSpecMips;
    float iblPad[2];
    // ---- CSM (M38d、末尾 append) ----
    XMFLOAT4X4 shadowVP12[2];
    float cascadeInfo[4]; // xyz = split far 境界 / w = カスケード数
};

bool CreateCB(ID3D11Device* dev, UINT size, Microsoft::WRL::ComPtr<ID3D11Buffer>& out)
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

bool DeferredPath::Init(GraphicsDevice& device, ShaderManager& shaders)
{
    ID3D11Device* dev = device.Device();

    gbufferShader_ = shaders.Load("deferred_gbuffer");
    lightShader_ = shaders.Load("deferred_light");
    // スキンメッシュ用の GBuffer シェーダをプリロード (BLENDINDICES 入力レイアウトもここで構築)
    gbufferSkinnedShader_ = shaders.Load("deferred_gbuffer_skinned");
    // スカイボックス (M29d)。失敗しても続行 (空が clearColor になるだけ)
    skybox_.Init(device, shaders);

    if (!CreateCB(dev, sizeof(PerFrameCB), perFrameCB_)
        || !CreateCB(dev, sizeof(PerObjectCB), perObjectCB_)
        || !CreateCB(dev, sizeof(MaterialCB), materialCB_)
        || !CreateCB(dev, sizeof(LightPassCB), lightCB_)
        || !CreateCB(dev, sizeof(XMFLOAT4X4) * kMaxBones, boneCB_)) {
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

    // シャドウ PCF 用の比較サンプラ (M17)
    D3D11_SAMPLER_DESC cs = {};
    cs.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    cs.AddressU = cs.AddressV = cs.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    cs.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
    cs.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateSamplerState(&cs, shadowSampler_.GetAddressOf()))) {
        return false;
    }

    // IBL 用の LINEAR/CLAMP サンプラ (光パス s0 / 透明後段 s2、M38c)
    D3D11_SAMPLER_DESC is = {};
    is.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    is.AddressU = is.AddressV = is.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    is.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateSamplerState(&is, iblSampler_.GetAddressOf()))) {
        return false;
    }

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_BACK;
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
    dd.DepthEnable = FALSE;
    if (FAILED(dev->CreateDepthStencilState(&dd, depthDisabled_.GetAddressOf()))) {
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
    return true;
}

void DeferredPath::Shutdown()
{
    gbAlbedo_.Release();
    gbNormal_.Release();
    gbPosition_.Release();
    gbMaterial_.Release();
    perFrameCB_.Reset();
    perObjectCB_.Reset();
    materialCB_.Reset();
    lightCB_.Reset();
    sampler_.Reset();
    rasterizer_.Reset();
    depthOpaque_.Reset();
    depthDisabled_.Reset();
    depthTransparent_.Reset();
    blendOpaque_.Reset();
    blendAlpha_.Reset();
}

void DeferredPath::Render(GraphicsDevice& device, const RenderView& view, const RenderQueue& queue,
                          const SceneLightData& lights, RenderResources& resources,
                          ShaderManager& shaders)
{
    ShaderProgram* gbProg = shaders.Get(gbufferShader_);
    ShaderProgram* lightProg = shaders.Get(lightShader_);
    if (!gbProg || !gbProg->valid || !lightProg || !lightProg->valid) {
        return;
    }
    ShaderProgram* gbSkinnedProg = shaders.Get(gbufferSkinnedShader_); // スキンメッシュ用 (M18)
    ID3D11DeviceContext* dc = device.Context();

    // GBuffer をビューサイズに追従 (パス所有 RT のみ再生成 — spec 7.4 と同じ精神)
    gbAlbedo_.Resize(device, view.width, view.height, DXGI_FORMAT_R8G8B8A8_UNORM, false);
    gbNormal_.Resize(device, view.width, view.height, DXGI_FORMAT_R10G10B10A2_UNORM, false);
    gbPosition_.Resize(device, view.width, view.height, DXGI_FORMAT_R16G16B16A16_FLOAT, false);
    gbMaterial_.Resize(device, view.width, view.height, DXGI_FORMAT_R8G8B8A8_UNORM, false);
    if (!gbAlbedo_.IsValid() || !gbNormal_.IsValid() || !gbPosition_.IsValid()
        || !gbMaterial_.IsValid()) {
        return;
    }

    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(view.width);
    vp.Height = static_cast<float>(view.height);
    vp.MaxDepth = 1.0f;

    // ---- 1) ジオメトリパス ----
    ID3D11RenderTargetView* gbufs[4] = { gbAlbedo_.RTV(), gbNormal_.RTV(), gbPosition_.RTV(),
                                         gbMaterial_.RTV() };
    dc->OMSetRenderTargets(4, gbufs, view.dsv);
    dc->RSSetViewports(1, &vp);
    const float zero[4] = { 0, 0, 0, 0 };
    dc->ClearRenderTargetView(gbAlbedo_.RTV(), zero);
    dc->ClearRenderTargetView(gbNormal_.RTV(), zero);
    dc->ClearRenderTargetView(gbPosition_.RTV(), zero);
    dc->ClearRenderTargetView(gbMaterial_.RTV(), zero);
    if (view.dsv) {
        dc->ClearDepthStencilView(view.dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    }

    PerFrameCB pf = {};
    const XMMATRIX v = XMLoadFloat4x4(&view.view);
    const XMMATRIX p = XMLoadFloat4x4(&view.proj);
    XMStoreFloat4x4(&pf.viewProj, XMMatrixTranspose(XMMatrixMultiply(v, p)));
    pf.cameraPos = view.cameraPos;
    pf.lightCount = lights.count;
    pf.ambient = lights.ambient;
    memcpy(pf.lights, lights.lights, sizeof(pf.lights));
    pf.shadowVP = view.lightViewProj[0]; // 透明後段の forward_lit 用
    pf.shadowTexel = view.shadowTexelSize;
    pf.shadowEnabled = (view.shadowSRV != nullptr) ? 1 : 0;
    pf.shadowVP12[0] = view.lightViewProj[1]; // M38d CSM
    pf.shadowVP12[1] = view.lightViewProj[2];
    pf.cascadeInfo[0] = view.cascadeSplits[0];
    pf.cascadeInfo[1] = view.cascadeSplits[1];
    pf.cascadeInfo[2] = view.cascadeSplits[2];
    pf.cascadeInfo[3] = static_cast<float>(view.cascadeCount);
    pf.fogColor = view.fogColor;
    pf.fogMode = view.fogMode;
    pf.fogDensity = view.fogDensity;
    pf.fogStart = view.fogStart;
    pf.fogEnd = view.fogEnd;
    // IBL (M38c): 透明後段の forward_lit / 光パスの deferred_light が参照
    const bool ibl = view.iblIrradiance != nullptr && view.iblPrefiltered != nullptr
        && view.iblBrdfLut != nullptr;
    pf.iblEnabled = ibl ? 1 : 0;
    pf.iblSpecMips = view.iblSpecMips;
    UploadCB(dc, perFrameCB_.Get(), pf);

    ID3D11Buffer* cbs[2] = { perFrameCB_.Get(), perObjectCB_.Get() };
    dc->VSSetConstantBuffers(0, 2, cbs);
    dc->PSSetConstantBuffers(0, 2, cbs);
    ID3D11Buffer* matCbs[1] = { materialCB_.Get() };
    dc->PSSetConstantBuffers(2, 1, matCbs);
    ID3D11SamplerState* samplers[3] = { sampler_.Get(), shadowSampler_.Get(), iblSampler_.Get() };
    dc->PSSetSamplers(0, 3, samplers);
    dc->RSSetState(rasterizer_.Get());
    dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dc->OMSetDepthStencilState(depthOpaque_.Get(), 0);
    dc->OMSetBlendState(blendOpaque_.Get(), nullptr, 0xFFFFFFFFu);

    dc->IASetInputLayout(gbProg->inputLayout.Get());
    dc->VSSetShader(gbProg->vs.Get(), nullptr, 0);
    dc->PSSetShader(gbProg->ps.Get(), nullptr, 0);

    uint64_t boundMesh = 0;
    uint64_t boundTexture = 0;
    uint64_t boundNormal = 0;
    uint64_t boundGbShader = gbufferShader_.value; // 上で gbProg を bind 済み
    for (const RenderItem& item : queue.opaque) {
        Material* mat = resources.materials.Get(item.material);
        Mesh* mesh = resources.meshes.Get(item.mesh);
        if (!mat || !mesh) {
            continue;
        }
        // スキンメッシュは GBuffer シェーダをスキニング版に差し替え + ボーン CB を b3 に (M18)
        const bool skinned =
            (item.bones != nullptr && item.boneCount > 0 && gbSkinnedProg && gbSkinnedProg->valid);
        const AssetID gbShaderId = skinned ? gbufferSkinnedShader_ : gbufferShader_;
        if (gbShaderId.value != boundGbShader) {
            ShaderProgram* gp = skinned ? gbSkinnedProg : gbProg;
            dc->IASetInputLayout(gp->inputLayout.Get());
            dc->VSSetShader(gp->vs.Get(), nullptr, 0);
            dc->PSSetShader(gp->ps.Get(), nullptr, 0);
            boundGbShader = gbShaderId.value;
        }
        if (skinned) {
            D3D11_MAPPED_SUBRESOURCE bm = {};
            if (SUCCEEDED(dc->Map(boneCB_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &bm))) {
                memcpy(bm.pData, item.bones,
                       sizeof(XMFLOAT4X4) * static_cast<size_t>(item.boneCount));
                dc->Unmap(boneCB_.Get(), 0);
            }
            ID3D11Buffer* bcb = boneCB_.Get();
            dc->VSSetConstantBuffers(3, 1, &bcb);
        }
        const AssetID texId = mat->texture.IsNull() ? resources.textures.White() : mat->texture;
        if (texId.value != boundTexture) {
            Texture* tex = resources.textures.Get(texId);
            ID3D11ShaderResourceView* srv = tex ? tex->srv.Get() : nullptr;
            dc->PSSetShaderResources(0, 1, &srv);
            boundTexture = texId.value;
        }
        // GBuffer パスはノーマルマップを t1 に (無ければ White。gHasNormal で使用可否を判定)
        const AssetID nrmId = mat->normalTex.IsNull() ? resources.textures.White() : mat->normalTex;
        if (nrmId.value != boundNormal) {
            Texture* ntex = resources.textures.Get(nrmId);
            ID3D11ShaderResourceView* nsrv = ntex ? ntex->srv.Get() : nullptr;
            dc->PSSetShaderResources(1, 1, &nsrv);
            boundNormal = nrmId.value;
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
        po.baseColor = SrgbToLinear(mat->baseColor); // M38a: authored 色をリニアへ
        UploadCB(dc, perObjectCB_.Get(), po);
        MaterialCB mc = {};
        mc.metallic = mat->metallic;
        mc.roughness = mat->roughness;
        mc.hasNormal = mat->normalTex.IsNull() ? 0 : 1;
        UploadCB(dc, materialCB_.Get(), mc);
        dc->DrawIndexed(mesh->indexCount, 0, 0);
        prof::AddDraw(static_cast<int>(mesh->indexCount / 3));
    }

    // ---- 2) ライティングパス (フルスクリーン解決) ----
    dc->OMSetRenderTargets(1, &view.rtv, nullptr); // GBuffer を SRV で読むため depth も外す
    dc->RSSetViewports(1, &vp);
    LightPassCB lp = {};
    lp.ambient = lights.ambient;
    lp.lightCount = lights.count;
    lp.clearColor = { view.clearColor[0], view.clearColor[1], view.clearColor[2],
                      view.clearColor[3] };
    memcpy(lp.lights, lights.lights, sizeof(lp.lights));
    lp.shadowVP = view.lightViewProj[0];
    lp.shadowTexel = view.shadowTexelSize;
    lp.shadowEnabled = (view.shadowSRV != nullptr) ? 1 : 0;
    lp.shadowVP12[0] = view.lightViewProj[1]; // M38d CSM
    lp.shadowVP12[1] = view.lightViewProj[2];
    lp.cascadeInfo[0] = view.cascadeSplits[0];
    lp.cascadeInfo[1] = view.cascadeSplits[1];
    lp.cascadeInfo[2] = view.cascadeSplits[2];
    lp.cascadeInfo[3] = static_cast<float>(view.cascadeCount);
    lp.cameraPos = view.cameraPos;
    lp.fogColor = view.fogColor;
    lp.fogMode = view.fogMode;
    lp.fogDensity = view.fogDensity;
    lp.fogStart = view.fogStart;
    lp.fogEnd = view.fogEnd;
    lp.iblEnabled = pf.iblEnabled; // M38c (透明後段と同判定)
    lp.iblSpecMips = view.iblSpecMips;
    UploadCB(dc, lightCB_.Get(), lp);
    ID3D11Buffer* lightCbs[1] = { lightCB_.Get() };
    dc->PSSetConstantBuffers(0, 1, lightCbs);
    dc->VSSetConstantBuffers(0, 1, lightCbs);
    // 光パスの s0 は IBL 用 LINEAR/CLAMP (deferred_light.hlsl の宣言と一致。
    // LUT を wrap でサンプルすると roughness=1.0 が v=0 に巻き戻るため clamp 必須)
    ID3D11SamplerState* lightSamplers[1] = { iblSampler_.Get() };
    dc->PSSetSamplers(0, 1, lightSamplers);
    // GBuffer t0-3 + シャドウ t4 + IBL t5-7 (M38c、s0=IBL サンプラ / s1=比較サンプラ bind 済み)
    ID3D11ShaderResourceView* gbSrvs[8] = { gbAlbedo_.SRV(),  gbNormal_.SRV(),
                                            gbPosition_.SRV(), gbMaterial_.SRV(),
                                            view.shadowSRV,    view.iblIrradiance,
                                            view.iblPrefiltered, view.iblBrdfLut };
    dc->PSSetShaderResources(0, 8, gbSrvs);
    dc->IASetInputLayout(nullptr);
    dc->VSSetShader(lightProg->vs.Get(), nullptr, 0);
    dc->PSSetShader(lightProg->ps.Get(), nullptr, 0);
    dc->OMSetDepthStencilState(depthDisabled_.Get(), 0);
    dc->Draw(3, 0);
    ID3D11ShaderResourceView* nullSrvs[8] = {};
    dc->PSSetShaderResources(0, 8, nullSrvs); // 次フレームで RT に戻すため解除

    // ---- 2.5) スカイボックス (M29d): clearColor ピクセルを深度 1.0 判定で上書き ----
    skybox_.Render(device, shaders, view);

    // ---- 3) 透明後段 (Forward — マテリアルのシェーダで上描き) ----
    if (!queue.transparent.empty()) {
        dc->OMSetRenderTargets(1, &view.rtv, view.dsv);
        // forward_lit はシャドウ t1 / IBL t3-5 を参照 (M38c)。s0 は光パスで IBL 用に
        // 差し替えたのでマテリアル用 (異方性) に戻す。s2 (IBL) はフレーム頭で bind 済み
        ID3D11ShaderResourceView* fwdSrvs[5] = { view.shadowSRV, nullptr, view.iblIrradiance,
                                                 view.iblPrefiltered, view.iblBrdfLut };
        dc->PSSetShaderResources(1, 5, fwdSrvs);
        ID3D11SamplerState* matSampler[1] = { sampler_.Get() };
        dc->PSSetSamplers(0, 1, matSampler);
        dc->VSSetConstantBuffers(0, 2, cbs);
        dc->PSSetConstantBuffers(0, 2, cbs);
        dc->PSSetConstantBuffers(2, 1, matCbs); // forward_lit の MaterialParams
        dc->OMSetDepthStencilState(depthTransparent_.Get(), 0);
        dc->OMSetBlendState(blendAlpha_.Get(), nullptr, 0xFFFFFFFFu);

        uint64_t boundShader = 0;
        boundMesh = 0;
        boundTexture = 0;
        boundNormal = 0;
        for (const RenderItem& item : queue.transparent) {
            Material* mat = resources.materials.Get(item.material);
            Mesh* mesh = resources.meshes.Get(item.mesh);
            if (!mat || !mesh) {
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
            // forward_lit はノーマルマップを t2 で参照する (無ければ White)
            const AssetID nrmId =
                mat->normalTex.IsNull() ? resources.textures.White() : mat->normalTex;
            if (nrmId.value != boundNormal) {
                Texture* ntex = resources.textures.Get(nrmId);
                ID3D11ShaderResourceView* nsrv = ntex ? ntex->srv.Get() : nullptr;
                dc->PSSetShaderResources(2, 1, &nsrv);
                boundNormal = nrmId.value;
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
            po.baseColor = SrgbToLinear(mat->baseColor); // M38a: authored 色をリニアへ
            UploadCB(dc, perObjectCB_.Get(), po);
            MaterialCB mc = {};
            mc.metallic = mat->metallic;
            mc.roughness = mat->roughness;
            mc.hasNormal = mat->normalTex.IsNull() ? 0 : 1;
            UploadCB(dc, materialCB_.Get(), mc);
            dc->DrawIndexed(mesh->indexCount, 0, 0);
            prof::AddDraw(static_cast<int>(mesh->indexCount / 3));
        }
    } else {
        // パーティクル後段のために RTV+DSV を戻しておく
        dc->OMSetRenderTargets(1, &view.rtv, view.dsv);
    }
    dc->OMSetDepthStencilState(nullptr, 0);
    dc->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
}

} // namespace mye
