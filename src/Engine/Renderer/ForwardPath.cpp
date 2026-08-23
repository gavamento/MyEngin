#include "Engine/Renderer/ForwardPath.h"

#include "Engine/Core/Log.h"
#include "Engine/Core/Profiler.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/GraphicsDevice.h"
#include "Engine/Renderer/ShaderManager.h"

using namespace DirectX;

namespace mye {
namespace {

// ボーンパレット最大数は RenderTypes.h の mye::kMaxBones (HLSL の MYE_MAX_BONES と対) を使う。

// HLSL 側は既定の column_major packing のため、書き込み前に転置する。
// forward_lit.hlsl / deferred_gbuffer.hlsl の PerFrame と同一レイアウト。
struct PerFrameCB {
    XMFLOAT4X4 viewProj;
    XMFLOAT3 cameraPos;
    int32_t lightCount;
    XMFLOAT3 ambient;
    float pad0;
    GpuLight lights[kMaxLights];
    XMFLOAT4X4 shadowVP; // transpose(lightView*lightProj)
    float shadowTexel;
    int32_t shadowEnabled;
    float pad1[2];
    // ---- フォグ (M29d、末尾 append = 既存レイアウト不変) ----
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
    XMFLOAT4X4 shadowVP12[2];  // カスケード 1,2
    float cascadeInfo[4];      // xyz = split far 境界 (デバッグ用) / w = カスケード数
    // ---- M43a: ハイトフォグ + 太陽インスキャッタ (末尾 append。既定 = 恒等) ----
    float fogHeightFalloff;
    float fogBaseHeight;
    float fogInscatterIntensity;
    float fogInscatterPower;
    XMFLOAT3 sunDirection;
    float fogPad2;
    XMFLOAT3 sunColor;
    float fogPad3;
    // ---- M54e: 局所ライトのシャドウアトラス (末尾 append。0 = 従来と完全に同一の式) ----
    // ★DeferredPath.cpp の PerFrameCB と**必ず同じ末尾**を持たせること — Deferred の
    //   透明後段は forward_lit.hlsl をそのまま使うので、片方だけ足すと透明メッシュだけが
    //   ゴミ (アトラス以外の CB 内容) をタイル行列として読む
    int32_t shadowAtlasEnabled;
    float shadowAtlasTexel;
    float atlasPad[2];
    ShadowTileCB shadowTiles[kMaxShadowTiles];
    // ---- M57e: フロクセル (末尾 append)。0 = 従来と完全に同一の式。
    //      形は RenderTypes.h の FroxelForwardCB 1 本きりで DeferredPath.cpp と共有する
    //      (上の M54e の轍 = 「片方だけ足す」を型で潰した) ----
    FroxelForwardCB froxel;
};

struct PerObjectCB {
    XMFLOAT4X4 world;
    XMFLOAT4 baseColor;
    // ---- インスタンシング (M38f、末尾 append)。インスタンス版シェーダのみ参照 ----
    int32_t instanceBase;
    float instPad[3];
};

// forward_lit.hlsl の MaterialParams (b2) と一致 (16 バイト)
struct MaterialCB {
    float metallic;
    float roughness;
    int32_t hasNormal; // 0=ノーマルマップ無し
    float emissive;    // M46i: 自己発光の強さ (0 = 発光なし)
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
        || !CreateConstantBuffer(dev, sizeof(PerObjectCB), perObjectCB_)
        || !CreateConstantBuffer(dev, sizeof(MaterialCB), materialCB_)
        || !CreateConstantBuffer(dev, sizeof(XMFLOAT4X4) * kMaxBones, boneCB_)) {
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

    // シャドウ PCF 用の比較サンプラ (M17)
    D3D11_SAMPLER_DESC cs = {};
    cs.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    cs.AddressU = cs.AddressV = cs.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    cs.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
    cs.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateSamplerState(&cs, shadowSampler_.GetAddressOf()))) {
        return false;
    }

    // IBL 用の LINEAR/CLAMP サンプラ (s2、M38c — LUT の端で wrap しないこと)
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
    rd.FrontCounterClockwise = FALSE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(dev->CreateRasterizerState(&rd, rasterizer_.GetAddressOf()))) {
        return false;
    }
    // SceneView Wireframe (M40b)。CULL_NONE = 裏面の線も見せる
    rd.FillMode = D3D11_FILL_WIREFRAME;
    rd.CullMode = D3D11_CULL_NONE;
    if (FAILED(dev->CreateRasterizerState(&rd, rasterizerWire_.GetAddressOf()))) {
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

    // スキンメッシュ用のシェーダをプリロード (BLENDINDICES 入力レイアウトもここで構築される)
    skinnedShader_ = shaders.Load("forward_skinned");
    // インスタンシング (M38f)。litShader_ は run 判定用 (これ以外のシェーダは差し替え不可)
    litShader_ = shaders.Load("forward_lit");
    litInstancedShader_ = shaders.Load("forward_lit_instanced");
    // スカイボックス (M29d)。失敗しても続行 (空が clearColor になるだけ)
    skybox_.Init(device, shaders);
    // 地形 (M58c)。失敗しても続行 (地形が描かれないだけ = 従来の絵)
    terrain_.Init(device, shaders);
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
    instanceBuf_.Reset();
    terrain_.Shutdown(); // M58c
}

void ForwardPath::Render(GraphicsDevice& device, const RenderView& view, const RenderQueue& queue,
                         const SceneLightData& lights, RenderResources& resources,
                         ShaderManager& shaders)
{
    ID3D11DeviceContext* dc = device.Context();

    // RT はここでバインドしたまま VFX/パーティクル後段まで引き継ぐ
    // (M42a: パーティクル直前に RenderSystem が read-only DSV へ差し替える)
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

    // SceneView 表示モード (M40b): Unlit/Wireframe はライトを白定数に差し替え、
    // 影/IBL/フォグも切る (シェーダ追加なしで albedo 素通し)
    const bool unlit = view.debugViewMode != 0;
    const bool wire = view.debugViewMode == 2;
    SceneLightData unlitLights;
    unlitLights.ambient = { 1.0f, 1.0f, 1.0f };
    unlitLights.count = 0;
    const SceneLightData& L = unlit ? unlitLights : lights;

    // フレーム共通 CB
    PerFrameCB pf = {};
    const XMMATRIX v = XMLoadFloat4x4(&view.view);
    const XMMATRIX p = XMLoadFloat4x4(&view.proj);
    XMStoreFloat4x4(&pf.viewProj, XMMatrixTranspose(XMMatrixMultiply(v, p)));
    pf.cameraPos = view.cameraPos;
    pf.lightCount = L.count;
    pf.ambient = L.ambient;
    memcpy(pf.lights, L.lights, sizeof(pf.lights));
    pf.shadowVP = view.lightViewProj[0];
    pf.shadowTexel = view.shadowTexelSize;
    pf.shadowEnabled = (!unlit && view.shadowSRV != nullptr) ? 1 : 0;
    pf.shadowVP12[0] = view.lightViewProj[1]; // M38d CSM
    pf.shadowVP12[1] = view.lightViewProj[2];
    pf.cascadeInfo[0] = view.cascadeSplits[0];
    pf.cascadeInfo[1] = view.cascadeSplits[1];
    pf.cascadeInfo[2] = view.cascadeSplits[2];
    pf.cascadeInfo[3] = static_cast<float>(view.cascadeCount);
    pf.fogColor = view.fogColor;
    pf.fogMode = unlit ? -1 : view.fogMode;
    pf.fogDensity = view.fogDensity;
    pf.fogStart = view.fogStart;
    pf.fogEnd = view.fogEnd;
    pf.fogHeightFalloff = view.fogHeightFalloff; // M43a
    pf.fogBaseHeight = view.fogBaseHeight;
    pf.fogInscatterIntensity = view.fogInscatterIntensity;
    pf.fogInscatterPower = view.fogInscatterPower;
    pf.sunDirection = view.sunDirection;
    pf.sunColor = view.sunColor;
    // IBL (M38c): SRV 3 点が揃っている時のみ有効 (無ければ従来の定数アンビエント)
    const bool ibl = !unlit && view.iblIrradiance != nullptr && view.iblPrefiltered != nullptr
        && view.iblBrdfLut != nullptr;
    pf.iblEnabled = ibl ? 1 : 0;
    pf.iblSpecMips = view.iblSpecMips;
    // M54e: 局所ライトのシャドウアトラス。SRV が null (影を投げる局所ライトが 1 本も無い
    // シーン / AssetPreviewCache の enableShadows=false / Unlit・Wireframe) なら 0 =
    // 従来と 1 ビットも変わらない経路へ落ちる。**Deferred 光パスと同じ判定式**
    pf.shadowAtlasEnabled =
        (!unlit && view.shadowAtlasSRV != nullptr && view.shadowTileCount > 0) ? 1 : 0;
    pf.shadowAtlasTexel = view.shadowAtlasTexel;
    FillShadowTilesCB(view, pf.shadowTiles);
    // M57e: フロクセル。判定は FroxelIsBound 1 本 (Deferred / スカイ / パーティクルと共有)。
    // SRV が null なら 0 = 従来の ApplyFog へ落ちる = 1 ビットも変わらない
    const bool froxelBound = FroxelIsBound(view);
    pf.froxel = MakeFroxelForwardCB(view, froxelBound);
    UploadCB(dc, perFrameCB_.Get(), pf);

    ID3D11Buffer* cbs[2] = { perFrameCB_.Get(), perObjectCB_.Get() };
    dc->VSSetConstantBuffers(0, 2, cbs);
    dc->PSSetConstantBuffers(0, 2, cbs);
    ID3D11Buffer* matCbs[1] = { materialCB_.Get() };
    dc->PSSetConstantBuffers(2, 1, matCbs);
    ID3D11SamplerState* samplers[3] = { sampler_.Get(), shadowSampler_.Get(), iblSampler_.Get() };
    dc->PSSetSamplers(0, 3, samplers);
    // シャドウマップを t1 に (マテリアルの albedo は t0)、IBL を t3-5 に (M38c)、
    // 局所ライトのアトラスを t6 に (M54e。統合契約 予約 2)、フロクセルの積分結果を
    // t7 に (M57e。統合契約 予約 2)。
    // アトラス用のサンプラは増やさず s1 の比較サンプラを共有する (CSM と同じ設定でよい)。
    // froxel は s2 (IBL 用 LINEAR/CLAMP) を流用する = サンプラは 1 つも増えない
    ID3D11ShaderResourceView* frameSrvs[7] = { view.shadowSRV,      nullptr,
                                               view.iblIrradiance,  view.iblPrefiltered,
                                               view.iblBrdfLut,     view.shadowAtlasSRV,
                                               froxelBound ? view.froxelSRV : nullptr };
    static_assert(froxel::kForwardSrvSlot == 7, "froxel の Forward SRV は統合契約 予約 2 の t7");
    dc->PSSetShaderResources(1, 7, frameSrvs);
    dc->RSSetState(wire ? rasterizerWire_.Get() : rasterizer_.Get());
    dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // インスタンス run 検出 (M38f): forward_lit マテリアルの非スキン opaque 連続 run のみ。
    // シェーダ未ロード時や無効化時は runs 空 = 全て従来の per-item 描画
    runs_.clear();
    worlds_.clear();
    ShaderProgram* instProg = shaders.Get(litInstancedShader_);
    if (view.instancingEnabled != 0 && instProg && instProg->valid) {
        canInstance_.resize(queue.opaque.size());
        for (size_t i = 0; i < queue.opaque.size(); ++i) {
            const RenderItem& it = queue.opaque[i];
            bool can = (it.bones == nullptr);
            if (can) {
                Material* m = resources.materials.Get(it.material);
                can = m && m->shader.value == litShader_.value
                    && resources.meshes.Get(it.mesh) != nullptr;
            }
            canInstance_[i] = can ? 1 : 0;
        }
        BuildInstanceRuns(queue.opaque, canInstance_, runs_, worlds_);
        if (!worlds_.empty() && instanceBuf_.Upload(device, worlds_)) {
            ID3D11ShaderResourceView* isrv = instanceBuf_.SRV();
            dc->VSSetShaderResources(0, 1, &isrv);
        } else {
            runs_.clear();
        }
    }

    // 不透明
    dc->OMSetDepthStencilState(depthOpaque_.Get(), 0);
    dc->OMSetBlendState(blendOpaque_.Get(), nullptr, 0xFFFFFFFFu);
    DrawItems(device, queue.opaque, view, resources, shaders, runs_.empty() ? nullptr : &runs_);

    // インスタンス SRV を外す (次フレームの Map と競合させない)
    ID3D11ShaderResourceView* nullVsSrv = nullptr;
    dc->VSSetShaderResources(0, 1, &nullVsSrv);

    // 地形 (M58c): 不透明メッシュの直後・スカイボックスの前。深度を書くので
    // 「地形の向こうの空が塗られない」が成立する。CB は b4 なので b0-b2 は張り替わらず、
    // 透明段の DrawItems はシェーダを張り直すだけでよい。
    // 地形が無いフレームは TerrainPass が即 return する = 従来とビット一致
    terrain_.RenderForward(device, shaders, view, resources);

    // スカイボックス (M29d): 不透明後・透明前。深度 1.0 のピクセルだけ塗る。
    // PS の b3 のみ使うので b0-b2 / トポロジは不変 (透明段は DrawItems がシェーダ再バインド)。
    // Wireframe (M40b) はフルスクリーン三角形が線になってしまうためスキップ
    if (!wire) {
        skybox_.Render(device, shaders, view);
    }

    // 半透明 (インスタンシング対象外)
    if (!queue.transparent.empty()) {
        // M57e: スカイボックスの cubemap 経路が s0 を LINEAR/CLAMP へ差し替えたままなので
        // マテリアル用 (異方性 WRAP) へ戻す。M38b からの潜在バグで、フロクセルとは
        // 独立に効く (Deferred の透明後段は前から同じことをしている)
        ID3D11SamplerState* samplers2[3] = { sampler_.Get(), shadowSampler_.Get(),
                                             iblSampler_.Get() };
        dc->PSSetSamplers(0, 3, samplers2);
        dc->OMSetDepthStencilState(depthTransparent_.Get(), 0);
        dc->OMSetBlendState(blendAlpha_.Get(), nullptr, 0xFFFFFFFFu);
        DrawItems(device, queue.transparent, view, resources, shaders, nullptr);
    }

    // Wireframe (M40b) はメッシュ描画のみ — 後段 (パーティクル/ポスプロ) は solid に戻す
    if (wire) {
        dc->RSSetState(rasterizer_.Get());
    }

    // ---- M57e: t1-t7 を剥がす。**t7 (フロクセル積分結果) を残してはいけない** ----
    // 残すと次フレームの積分パスが同じテクスチャを UAV に取った瞬間に D3D が
    // 片方を黙って外す (M57d が Deferred の t15 で踏んだのと同じ罠)
    ID3D11ShaderResourceView* fwdNull[7] = {};
    dc->PSSetShaderResources(1, 7, fwdNull);
}

void ForwardPath::DrawItems(GraphicsDevice& device, const std::vector<RenderItem>& items,
                            const RenderView& view, RenderResources& resources,
                            ShaderManager& shaders, const std::vector<MeshInstanceRun>* runs)
{
    (void)view;
    ID3D11DeviceContext* dc = device.Context();

    uint64_t boundShader = 0;
    uint64_t boundTexture = 0;
    uint64_t boundNormal = 0;
    uint64_t boundMesh = 0;
    size_t nextRun = 0;

    for (size_t idx = 0; idx < items.size(); ++idx) {
        const RenderItem& item = items[idx];

        // インスタンス run の先頭なら一括描画 (M38f)。run 判定時に mat/mesh/シェーダの
        // 有効性は確認済み (Render の canInstance_ 構築を参照)
        if (runs && nextRun < runs->size() && (*runs)[nextRun].first == idx) {
            const MeshInstanceRun& run = (*runs)[nextRun];
            ++nextRun;
            Material* mat = resources.materials.Get(item.material);
            Mesh* mesh = resources.meshes.Get(item.mesh);
            ShaderProgram* prog = shaders.Get(litInstancedShader_);
            if (litInstancedShader_.value != boundShader) {
                dc->IASetInputLayout(prog->inputLayout.Get());
                dc->VSSetShader(prog->vs.Get(), nullptr, 0);
                dc->PSSetShader(prog->ps.Get(), nullptr, 0);
                boundShader = litInstancedShader_.value;
            }
            const AssetID texId =
                mat->texture.IsNull() ? resources.textures.White() : mat->texture;
            if (texId.value != boundTexture) {
                Texture* tex = resources.textures.Get(texId);
                ID3D11ShaderResourceView* srv = tex ? tex->srv.Get() : nullptr;
                dc->PSSetShaderResources(0, 1, &srv);
                boundTexture = texId.value;
            }
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
            po.world = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 }; // 未使用
            po.baseColor = SrgbToLinear(mat->baseColor);
            po.instanceBase = static_cast<int32_t>(run.base);
            UploadCB(dc, perObjectCB_.Get(), po);
            MaterialCB mc = {};
            mc.metallic = mat->metallic;
            mc.roughness = mat->roughness;
            mc.hasNormal = mat->normalTex.IsNull() ? 0 : 1;
            mc.emissive = mat->emissiveIntensity;
            UploadCB(dc, materialCB_.Get(), mc);
            dc->DrawIndexedInstanced(mesh->indexCount, run.count, 0, 0, 0);
            prof::AddDraw(static_cast<int>(mesh->indexCount / 3 * run.count));
            idx += run.count - 1; // for の ++idx と合わせて run 全体を飛ばす
            continue;
        }

        Material* mat = resources.materials.Get(item.material);
        if (!mat) {
            continue;
        }
        Mesh* mesh = resources.meshes.Get(item.mesh);
        if (!mesh) {
            continue;
        }
        // スキンメッシュはマテリアルのシェーダではなくスキニング版に差し替える (M18)
        const bool skinned = (item.bones != nullptr && item.boneCount > 0);
        const AssetID shaderId = skinned ? skinnedShader_ : mat->shader;
        ShaderProgram* prog = shaders.Get(shaderId);
        if (!prog || !prog->valid) {
            continue;
        }

        if (shaderId.value != boundShader) {
            dc->IASetInputLayout(prog->inputLayout.Get());
            dc->VSSetShader(prog->vs.Get(), nullptr, 0);
            dc->PSSetShader(prog->ps.Get(), nullptr, 0);
            boundShader = shaderId.value;
        }
        if (skinned) {
            // ボーンパレットを b3 (VS) にアップロード
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
        // ノーマルマップを t2 に (無ければ White。シェーダは gHasNormal で使用可否を判定)
        const AssetID nrmId = mat->normalTex.IsNull() ? resources.textures.White() : mat->normalTex;
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
        mc.emissive = mat->emissiveIntensity;
        UploadCB(dc, materialCB_.Get(), mc);

        dc->DrawIndexed(mesh->indexCount, 0, 0);
        prof::AddDraw(static_cast<int>(mesh->indexCount / 3));
    }
}

} // namespace mye
