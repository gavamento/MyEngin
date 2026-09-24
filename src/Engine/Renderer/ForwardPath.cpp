#include "Engine/Renderer/ForwardPath.h"

#include "Engine/Core/Log.h"
#include "Engine/Core/Profiler.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/GraphicsDevice.h"
#include "Engine/Renderer/MeshBind.h"
#include "Engine/Renderer/ShaderManager.h"
#include "Engine/Renderer/SurfaceDrawBind.h" // M79 sub-02
#include "Engine/Renderer/SurfaceProgram.h"
#include "Engine/Renderer/SurfaceShaderTypes.h"

using namespace DirectX;

namespace mye {
namespace {

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
    //      (型が 1 本なので「片方だけ足す」が起きない) ----
    FroxelForwardCB froxel;
    // ---- M65e: 音響の残光 (末尾 append)。同上 — 形は RenderTypes.h の AcousticCB
    //      1 本きりで DeferredPath.cpp と共有する ----
    AcousticCB acoustic;
};

// PerObjectCB / MaterialCB は MeshBind.h (DeferredPath と共有する 1 本)

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
        || !CreateConstantBuffer(dev, sizeof(XMFLOAT4X4) * kMaxBones, boneCB_)
        // M79 sub-02: サーフェスシェーダーの予約 CB (forward_lit の b0-b2 とは別バッファ)
        || !CreateConstantBuffer(dev, sizeof(MyEnginePerFrameCB), surfacePerFrameCB_)
        || !CreateConstantBuffer(dev, sizeof(MyEngineSurfaceFrameCB), surfaceFrameCB_)
        || !CreateConstantBuffer(dev, sizeof(MyEnginePerObjectCB), surfacePerObjectCB_)
        || !CreateConstantBuffer(dev, sizeof(MyEngineWaterCB), surfaceWaterCB_)) {
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
    // M79 sub-06: doubleSided なサーフェス用 (Solid のまま Cull だけ外す)
    rd.CullMode = D3D11_CULL_NONE;
    if (FAILED(dev->CreateRasterizerState(&rd, rasterizerCullNone_.GetAddressOf()))) {
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

    // M79 sub-02: サーフェス失敗時のマゼンタ代替。他の *.surface.hlsl と同じ LoadSurface 経路
    surfaceErrorId_ = shaders.LoadSurface("surface_error");
    // スキンメッシュ用のシェーダをプリロード (BLENDINDICES 入力レイアウトもここで構築される)
    skinnedShader_ = shaders.Load("forward_skinned");
    // インスタンシング (M38f)。litShader_ は run 判定用 (これ以外のシェーダは差し替え不可)
    litShader_ = shaders.Load("forward_lit");
    litInstancedShader_ = shaders.Load("forward_lit_instanced");
    // スカイボックス (M29d)。失敗しても続行 (空が clearColor になるだけ)
    skybox_.Init(device, shaders);
    // 地形 (M58c)。失敗しても続行 (地形が描かれないだけ = 従来の絵)
    terrain_.Init(device, shaders);
    // 水面。失敗しても続行 (水面が描かれないだけ = 従来の絵)
    water_.Init(device, shaders);
    return true;
}

void ForwardPath::Shutdown()
{
    perFrameCB_.Reset();
    perObjectCB_.Reset();
    sampler_.Reset();
    rasterizer_.Reset();
    rasterizerCullNone_.Reset();
    depthOpaque_.Reset();
    depthTransparent_.Reset();
    blendOpaque_.Reset();
    blendAlpha_.Reset();
    instanceBuf_.Reset();
    terrain_.Shutdown(); // M58c
    water_.Shutdown();
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
    // M65e: 音響の残光。**SRV が null なら params が全部 0 = w も 0** で
    // シェーダは分岐に一度も入らない (= 従来の絵とビット恒等)
    const bool acousticBound = AcousticIsBound(view);
    pf.acoustic = MakeAcousticCB(view, acousticBound);
    UploadCB(dc, perFrameCB_.Get(), pf);

    // ---- M79 sub-02: サーフェスシェーダーの予約 CB (forward_lit の b0-b2 とは別バッファ)。
    //      内容は pf/view から詰め、シェーダごとの実バインドは DrawSurfaceItem が
    //      D3DReflect の名前解決で行う (register 位置は作者ソースごとに変わりうる) ----
    MyEnginePerFrameCB spf = {};
    spf.cameraPos = pf.cameraPos;
    spf.lightCount = pf.lightCount;
    spf.ambient = pf.ambient;
    memcpy(spf.lights, pf.lights, sizeof(spf.lights));
    spf.shadowVP = pf.shadowVP;
    spf.shadowTexel = pf.shadowTexel;
    spf.shadowEnabled = pf.shadowEnabled;
    spf.fogColor = pf.fogColor;
    spf.fogMode = pf.fogMode;
    spf.fogDensity = pf.fogDensity;
    spf.fogStart = pf.fogStart;
    spf.fogEnd = pf.fogEnd;
    spf.iblEnabled = pf.iblEnabled;
    spf.iblSpecMips = pf.iblSpecMips;
    spf.shadowVP12[0] = pf.shadowVP12[0];
    spf.shadowVP12[1] = pf.shadowVP12[1];
    memcpy(spf.cascadeInfo, pf.cascadeInfo, sizeof(spf.cascadeInfo));
    spf.fogHeightFalloff = pf.fogHeightFalloff;
    spf.fogBaseHeight = pf.fogBaseHeight;
    spf.fogInscatterIntensity = pf.fogInscatterIntensity;
    spf.fogInscatterPower = pf.fogInscatterPower;
    spf.sunDirection = pf.sunDirection;
    spf.sunColor = pf.sunColor;
    spf.shadowAtlasEnabled = pf.shadowAtlasEnabled;
    spf.shadowAtlasTexel = pf.shadowAtlasTexel;
    memcpy(spf.shadowTiles, pf.shadowTiles, sizeof(spf.shadowTiles));
    spf.froxel = pf.froxel;
    spf.acoustic = pf.acoustic;
    UploadCB(dc, surfacePerFrameCB_.Get(), spf);

    // MyEngineSurfaceFrame: 今/前の ViewProj・時刻。速度/影エントリは Deferred (sub-03) 専用だが、
    // 予約 CB は 1 本を全パスで共有するので Forward でも正しい値を詰めておく
    MyEngineSurfaceFrameCB sf = {};
    XMStoreFloat4x4(&sf.curViewProj, XMMatrixTranspose(XMMatrixMultiply(v, p)));
    if (view.prevViewProjValid != 0) {
        XMStoreFloat4x4(&sf.prevViewProj, XMMatrixTranspose(XMLoadFloat4x4(&view.prevViewProj)));
        sf.historyValid = 1;
    } else {
        const XMMATRIX nj = XMMatrixMultiply(v, XMLoadFloat4x4(&view.projNoJitter));
        XMStoreFloat4x4(&sf.prevViewProj, XMMatrixTranspose(nj));
        sf.historyValid = 0;
    }
    sf.shadowViewProj = view.lightViewProj[0]; // 色エントリは未使用 (影エントリは sub-03)
    sf.curTime = static_cast<float>(view.viewFrameIndex) * (1.0f / 60.0f); // spec §2: 時計
    sf.prevTime = (view.viewFrameIndex > 0)
        ? static_cast<float>(view.viewFrameIndex - 1) * (1.0f / 60.0f)
        : 0.0f;
    // M79 sub-05: 水面が有効なフレームだけ実値を渡す (無効 = 0、spec §4.1)
    const bool waterActive = view.water != nullptr && view.water->active;
    sf.curWaterTime = waterActive ? view.water->curWaterTime : 0.0f;
    sf.prevWaterTime = waterActive ? view.water->prevWaterTime : 0.0f;
    sf.jitterNdc = { view.jitterNdc[0], view.jitterNdc[1] };
    sf.screenSize = { static_cast<float>(view.width), static_cast<float>(view.height) };
    UploadCB(dc, surfaceFrameCB_.Get(), sf);

    // MyEngineWater: 水面が有効なフレームだけ波パラメータを渡す (無効 = 全 0 + enabled=0)
    const MyEngineWaterCB water = waterActive ? view.water->surfaceCb : MyEngineWaterCB{};
    UploadCB(dc, surfaceWaterCB_.Get(), water);

    ID3D11Buffer* cbs[2] = { perFrameCB_.Get(), perObjectCB_.Get() };
    dc->VSSetConstantBuffers(0, 2, cbs);
    dc->PSSetConstantBuffers(0, 2, cbs);
    ID3D11Buffer* matCbs[1] = { materialCB_.Get() };
    dc->PSSetConstantBuffers(2, 1, matCbs);
    ID3D11SamplerState* samplers[3] = { sampler_.Get(), shadowSampler_.Get(), iblSampler_.Get() };
    dc->PSSetSamplers(0, 3, samplers);
    // シャドウマップを t1 に (マテリアルの albedo は t0)、IBL を t3-5 に (M38c)、
    // 局所ライトのアトラスを t6 に (M54e)、フロクセルの積分結果を t7 に (M57e)、
    // 音響の残光を t8 に (M65e)、見通しビットの 3D テクスチャを t9 に。本数は 9
    // (DeferredPath の透明後段も同じ 9 本)。
    // アトラス用のサンプラは増やさず s1 の比較サンプラを共有する (CSM と同じ設定でよい)。
    // froxel は s2 (IBL 用 LINEAR/CLAMP) を流用する = サンプラは 1 つも増えない
    ID3D11ShaderResourceView* frameSrvs[9] = { view.shadowSRV,      nullptr,
                                               view.iblIrradiance,  view.iblPrefiltered,
                                               view.iblBrdfLut,     view.shadowAtlasSRV,
                                               froxelBound ? view.froxelSRV : nullptr,
                                               acousticBound ? view.acousticSRV : nullptr,
                                               acousticBound ? view.acousticFrontSRV : nullptr };
    static_assert(froxel::kForwardSrvSlot == 7, "froxel の Forward SRV は統合契約 予約 2 の t7");
    static_assert(acoustic::kGlowForwardSrvSlot == 8, "音響の Forward SRV は t8 (M65e で 7->8)");
    static_assert(acoustic::kFrontForwardSrvSlot == 9, "解析的な波面の Forward SRV は t9 (8->9)");
    dc->PSSetShaderResources(1, 9, frameSrvs);
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

    // 水面 (スカイボックス後・透明メッシュ前)
    water_.Render(device, shaders, view, resources, perFrameCB_.Get());

    // 半透明 (インスタンシング対象外)
    if (!queue.transparent.empty()) {
        // M57e: スカイボックスの cubemap 経路が s0 を LINEAR/CLAMP へ差し替えたままなので
        // マテリアル用 (異方性 WRAP) へ戻す。フロクセルの有無とは関係なく要る
        // (Deferred の透明後段も同じことをしている)
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

    // ---- M57e: t1-t8 を剥がす。**t7 (フロクセル積分結果) を残してはいけない** ----
    // 残すと次フレームの積分パスが同じテクスチャを UAV に取った瞬間に D3D が
    // 片方を黙って外す (Deferred 光パスの t15 と同じ罠)
    // ★t8 (残光) も剥がす本数に入れること。剥がし忘れると次フレームまで生き残る。
    //   t9 (見通しビット) はここでは剥がしていない — SRV 専用のテクスチャで UAV と衝突しないため
    ID3D11ShaderResourceView* fwdNull[8] = {};
    dc->PSSetShaderResources(1, 8, fwdNull);
}

void ForwardPath::DrawItems(GraphicsDevice& device, const std::vector<RenderItem>& items,
                            const RenderView& view, RenderResources& resources,
                            ShaderManager& shaders, const std::vector<MeshInstanceRun>* runs)
{
    ID3D11DeviceContext* dc = device.Context();

    uint64_t boundShader = 0;
    MeshBindState bound;
    size_t nextRun = 0;

    // M79 sub-02: DrawSurfaceItem は forward_lit の固定スロット (b0-b2 / VS t0 / PS t0-t9 / s0-s2) を
    // 名前解決で自由に張り替えるので、直後にこの一式へ戻す。「次のアイテムが forward_lit の
    // ときにバインド前提を壊さない」(sub-02.md 受け入れ条件 5) を、サーフェス→通常のどの
    // 境目でも成立させるための唯一の復元経路
    auto restoreForwardLitBindings = [&]() {
        ID3D11Buffer* cbs2[2] = { perFrameCB_.Get(), perObjectCB_.Get() };
        dc->VSSetConstantBuffers(0, 2, cbs2);
        dc->PSSetConstantBuffers(0, 2, cbs2);
        ID3D11Buffer* matCbs[1] = { materialCB_.Get() };
        dc->PSSetConstantBuffers(2, 1, matCbs);
        ID3D11SamplerState* samplers[3] = { sampler_.Get(), shadowSampler_.Get(), iblSampler_.Get() };
        dc->PSSetSamplers(0, 3, samplers);
        const bool froxelBound = FroxelIsBound(view);
        const bool acousticBound = AcousticIsBound(view);
        ID3D11ShaderResourceView* frameSrvs[9] = { view.shadowSRV,      nullptr,
                                                   view.iblIrradiance,  view.iblPrefiltered,
                                                   view.iblBrdfLut,     view.shadowAtlasSRV,
                                                   froxelBound ? view.froxelSRV : nullptr,
                                                   acousticBound ? view.acousticSRV : nullptr,
                                                   acousticBound ? view.acousticFrontSRV : nullptr };
        dc->PSSetShaderResources(1, 9, frameSrvs);
        // forward_lit_instanced.hlsl は VS 側 t0 に StructuredBuffer<MeshInstance> を持つ
        // (PS の t0 = アルベドとは独立のスロット空間)。サーフェスの VS が名前解決で VS t0 に
        // Texture2D 等を張ると、次の instanced run が型不一致で丸ごと消える (review-1 #2)
        ID3D11ShaderResourceView* instSrv = runs ? instanceBuf_.SRV() : nullptr;
        dc->VSSetShaderResources(0, 1, &instSrv);
        dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        // M79 sub-06: doubleSided (Cull None) はサーフェス描画のときだけ張るので、
        // 通常描画の前提 (rasterizer_/rasterizerWire_) へ必ず戻す
        const bool wire = view.debugViewMode == 2;
        dc->RSSetState(wire ? rasterizerWire_.Get() : rasterizer_.Get());
        boundShader = 0; // 次の通常アイテムに VS/PS/InputLayout を再バインドさせる
        bound = MeshBindState{}; // t0/normal/メッシュ VB・IB も再バインドさせる
    };

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
            BindMaterialTextures(dc, resources.textures, *mat, kForwardNormalSlot, bound);
            BindMeshBuffers(dc, *mesh, item.mesh, bound);
            PerObjectCB po = {};
            po.world = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 }; // 未使用
            po.baseColor = SrgbToLinear(mat->baseColor);
            po.instanceBase = static_cast<int32_t>(run.base);
            UploadCB(dc, perObjectCB_.Get(), po);
            UploadCB(dc, materialCB_.Get(), MakeMaterialCB(*mat));
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

        // M79 sub-02: shader が "*.surface" のマテリアルはサーフェスプログラムの色エントリで描く。
        // スキン+サーフェスは初版未対応 (spec §2) — WARN 1 回だけ出して下の従来経路へ落ちる
        // (skinnedShader_ が使われ、mat->shader は無視される。既存の分岐と同じ)
        SurfaceMaterialState* surf =
            resources.materials.GetOrBuildSurfaceState(item.material, shaders, resources.textures, device);
        if (surf && surf->isSurfaceShader) {
            if (skinned) {
                if (skinnedSurfaceWarned_.insert(item.material.value).second) {
                    MYE_LOG_WARN(
                        "surface material on skinned mesh is not supported yet - using skinned shader (material=0x%llx)",
                        static_cast<unsigned long long>(item.material.value));
                }
                // 従来のスキン経路へフォールスルー (下の shaderId 解決へ)
            } else {
                DrawSurfaceItem(device, item, *mat, *mesh, *surf, shaders, resources, view);
                restoreForwardLitBindings();
                continue;
            }
        }

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
        BindMaterialTextures(dc, resources.textures, *mat, kForwardNormalSlot, bound);
        BindMeshBuffers(dc, *mesh, item.mesh, bound);

        PerObjectCB po = {};
        XMStoreFloat4x4(&po.world, XMMatrixTranspose(XMLoadFloat4x4(&item.world)));
        po.baseColor = SrgbToLinear(mat->baseColor); // M38a: authored 色をリニアへ
        UploadCB(dc, perObjectCB_.Get(), po);
        UploadCB(dc, materialCB_.Get(), MakeMaterialCB(*mat));

        dc->DrawIndexed(mesh->indexCount, 0, 0);
        prof::AddDraw(static_cast<int>(mesh->indexCount / 3));
    }
}

// M79 sub-02: shader が "*.surface" のアイテムをサーフェスプログラムの色エントリで描く。
// 予約 CB / 予約テクスチャ・サンプラ / MyEnginePerMaterial / 作者 Texture2D は
// すべて D3DReflect の名前解決でバインドする (register 位置は作者ソースごとに変わりうる)。
// surf->ready が false のときは surface_error (マゼンタ、変位なし) を代わりに使う
void ForwardPath::DrawSurfaceItem(GraphicsDevice& device, const RenderItem& item,
                                  const Material& mat, const Mesh& mesh, SurfaceMaterialState& surf,
                                  ShaderManager& shaders, RenderResources& resources,
                                  const RenderView& view)
{
    ID3D11DeviceContext* dc = device.Context();
    const AssetID programId = surf.useErrorFallback ? surfaceErrorId_ : surf.surfaceProgramId;
    SurfaceProgram* prog = shaders.GetSurface(programId);
    if (!prog || !prog->valid) {
        return; // surface_error 自体が壊れている異常系。描画せず諦める (クラッシュしない)
    }

    dc->IASetInputLayout(prog->colorInputLayout.Get());
    dc->VSSetShader(prog->colorVS.Get(), nullptr, 0);
    dc->PSSetShader(prog->colorPS.Get(), nullptr, 0);
    // M79 sub-06: doubleSided (.mat.json) は色・速度・影の全エントリを Cull None で描く。
    // 呼び出し側 (DrawItems::restoreForwardLitBindings) が描画直後に既定のラスタライザへ戻す
    if (resources.materials.GetSurfaceDoubleSided(item.material)) {
        dc->RSSetState(rasterizerCullNone_.Get());
    }

    const SurfaceEntryReflection& vsRefl = prog->colorVSReflect;
    const SurfaceEntryReflection& psRefl = prog->colorPSReflect;

    BindSurfaceNamedCB(dc, vsRefl, psRefl, surface::kPerFrameCB, surfacePerFrameCB_.Get());
    BindSurfaceNamedCB(dc, vsRefl, psRefl, surface::kSurfaceFrameCB, surfaceFrameCB_.Get());
    BindSurfaceNamedCB(dc, vsRefl, psRefl, surface::kWaterCB, surfaceWaterCB_.Get());
    BindSurfaceNamedCB(dc, vsRefl, psRefl, surface::kPerMaterialCB, surf.perMaterialGpuCB.Get());

    MyEnginePerObjectCB po = {};
    XMStoreFloat4x4(&po.world, XMMatrixTranspose(XMLoadFloat4x4(&item.world)));
    XMStoreFloat4x4(&po.prevWorld, XMMatrixTranspose(XMLoadFloat4x4(&item.prevWorld)));
    po.baseColor = SrgbToLinear(mat.baseColor);
    UploadCB(dc, surfacePerObjectCB_.Get(), po);
    BindSurfaceNamedCB(dc, vsRefl, psRefl, surface::kPerObjectCB, surfacePerObjectCB_.Get());

    // 予約テクスチャ / サンプラ (Forward の既存スロット構成と同じ SRV を名前で引く)
    const bool froxelBound = FroxelIsBound(view);
    BindSurfaceNamedSRV(dc, vsRefl, psRefl, "gShadowMap", view.shadowSRV);
    BindSurfaceNamedSRV(dc, vsRefl, psRefl, "gIblIrradiance", view.iblIrradiance);
    BindSurfaceNamedSRV(dc, vsRefl, psRefl, "gIblPrefiltered", view.iblPrefiltered);
    BindSurfaceNamedSRV(dc, vsRefl, psRefl, "gIblBrdfLut", view.iblBrdfLut);
    BindSurfaceNamedSRV(dc, vsRefl, psRefl, "gFroxelVolume", froxelBound ? view.froxelSRV : nullptr);
    BindSurfaceNamedSampler(dc, vsRefl, psRefl, "gSampler", sampler_.Get());
    BindSurfaceNamedSampler(dc, vsRefl, psRefl, "gShadowSampler", shadowSampler_.Get());
    BindSurfaceNamedSampler(dc, vsRefl, psRefl, "gIblSampler", iblSampler_.Get());

    // 作者 Texture2D プロパティ (失敗時は surf.textures が空なので何もバインドしない)
    for (const auto& [texName, texId] : surf.textures) {
        Texture* tex = resources.textures.Get(texId);
        ID3D11ShaderResourceView* srv = tex ? tex->srv.Get() : nullptr;
        BindSurfaceNamedSRV(dc, vsRefl, psRefl, texName.c_str(), srv);
    }

    const UINT stride = sizeof(MeshVertex);
    const UINT offset = 0;
    ID3D11Buffer* vb = mesh.vb.Get();
    dc->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    dc->IASetIndexBuffer(mesh.ib.Get(), DXGI_FORMAT_R32_UINT, 0);

    dc->DrawIndexed(mesh.indexCount, 0, 0);
    prof::AddDraw(static_cast<int>(mesh.indexCount / 3));
}

} // namespace mye
