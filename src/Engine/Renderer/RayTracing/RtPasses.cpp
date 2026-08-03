#include "Engine/Renderer/RayTracing/RtPasses.h"

#include <algorithm>
#include <cstring>

#include "Engine/Core/Log.h"
#include "Engine/Renderer/GpuBufferUtil.h"
#include "Engine/Renderer/GraphicsDevice.h"
#include "Engine/Renderer/RayTracing/RtTypes.h" // テンポラル蓄積のしきい値 / 履歴長上限
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

// rt_common.hlsli の RtEnvCB (b1) と一致
struct RtEnvCB {
    XMFLOAT3 ambient = { 0, 0, 0 };
    int32_t lightCount = 0;
    XMFLOAT3 skyTop = { 0, 0, 0 };
    int32_t skyMode = -1;
    XMFLOAT3 skyHorizon = { 0, 0, 0 };
    float rayEps = 0.005f;
    XMFLOAT3 skyBottom = { 0, 0, 0 };
    float pad1 = 0.0f;
    GpuLight lights[kMaxLights] = {};
};
static_assert(sizeof(RtEnvCB) == 64 + 64 * kMaxLights, "HLSL の RtEnvCB と一致させること");

// rt_debug.cs.hlsl の RtDebugCB (b2) と一致
struct RtDebugCB {
    XMFLOAT4X4 invViewProj = {};
    XMFLOAT3 cameraPos = { 0, 0, 0 };
    float tMax = 1000.0f;
    float screenSize[2] = { 0, 0 };
    int32_t debugMode = 0;
    float heatScale = 128.0f;
};
static_assert(sizeof(RtDebugCB) == 96, "HLSL の RtDebugCB と一致させること");

// rt_gi.cs.hlsl の RtGiCB (b2) と一致
struct RtGiCB {
    float outSize[2] = { 0, 0 };
    float gbSize[2] = { 0, 0 };
    float tMax = 1000.0f;
    uint32_t frameIndex = 0;
    int32_t bounces = 1;
    int32_t pad = 0;
};
static_assert(sizeof(RtGiCB) == 32, "HLSL の RtGiCB と一致させること");

// rt_temporal.cs.hlsl の RtTemporalCB (b2) と一致
struct RtTemporalCB {
    XMFLOAT4X4 prevViewProj = {}; // 転置済み
    float outSize[2] = { 0, 0 };
    float gbSize[2] = { 0, 0 };
    XMFLOAT3 prevCameraPos = { 0, 0, 0 };
    int32_t histValid = 0;
    XMFLOAT3 cameraPos = { 0, 0, 0 };
    float depthThreshold = kRtTemporalDepthThreshold;
    float normalThreshold = kRtTemporalNormalThreshold;
    float pad[3] = { 0, 0, 0 };
};
static_assert(sizeof(RtTemporalCB) == 128, "HLSL の RtTemporalCB と一致させること");

// rt_blit.hlsl の RtBlitCB (b0) と一致
struct RtBlitCB {
    float dstSize[2] = { 0, 0 };
    int32_t mode = 0;   // 0 = rgb をそのまま / 1 = a を履歴長ヒートマップとして表示
    float param = 1.0f; // mode 1 のスケール (履歴長の上限)
};

} // namespace

bool RtPasses::Init(GraphicsDevice& device, ShaderManager& shaders)
{
    if (inited_) {
        return true;
    }
    ID3D11Device* dev = device.Device();

    debugCS_ = shaders.LoadCompute("rt_debug.cs");
    giCS_ = shaders.LoadCompute("rt_gi.cs");
    temporalCS_ = shaders.LoadCompute("rt_temporal.cs");
    blitShader_ = shaders.Load("rt_blit");

    if (!CreateConstant(dev, sizeof(RtSceneCB), sceneCB_)
        || !CreateConstant(dev, sizeof(RtEnvCB), envCB_)
        || !CreateConstant(dev, sizeof(RtDebugCB), debugCB_)
        || !CreateConstant(dev, sizeof(RtGiCB), giCB_)
        || !CreateConstant(dev, sizeof(RtTemporalCB), temporalCB_)
        || !CreateConstant(dev, sizeof(RtBlitCB), blitCB_)) {
        MYE_LOG_ERROR("RtPasses: constant buffer creation failed");
        return false;
    }

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
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
    if (FAILED(dev->CreateSamplerState(&sd, linearClamp_.GetAddressOf()))
        || FAILED(dev->CreateDepthStencilState(&dsd, depthDisabled_.GetAddressOf()))
        || FAILED(dev->CreateBlendState(&bd, blendOpaque_.GetAddressOf()))
        || FAILED(dev->CreateRasterizerState(&rd, raster_.GetAddressOf()))) {
        MYE_LOG_ERROR("RtPasses: pipeline state creation failed");
        return false;
    }

    debugTimer_.Init(device);
    giTimer_.Init(device);
    temporalTimer_.Init(device);
    inited_ = true;
    return true;
}

void RtPasses::Shutdown()
{
    debugRt_.Release();
    giRt_.Release();
    for (GiHistory& h : giHist_) {
        for (int i = 0; i < 2; ++i) {
            h.color[i].Release();
            h.geom[i].Release();
        }
        h.write = 0;
        h.w = 0;
        h.h = 0;
        h.lastSerial = 0;
        h.hasLast = false;
    }
    sceneCB_.Reset();
    envCB_.Reset();
    debugCB_.Reset();
    giCB_.Reset();
    temporalCB_.Reset();
    blitCB_.Reset();
    linearClamp_.Reset();
    depthDisabled_.Reset();
    blendOpaque_.Reset();
    raster_.Reset();
    inited_ = false;
}

void RtPasses::BindCommon(GraphicsDevice& device, const RenderView& view, const RtFrameInputs& in)
{
    ID3D11DeviceContext* dc = device.Context();

    RtSceneCB sc = {};
    sc.instanceCount = in.scene->instanceCount;
    UploadCB(dc, sceneCB_.Get(), sc);

    RtEnvCB env = {};
    if (in.lights) {
        env.ambient = in.lights->ambient;
        env.lightCount = (std::min)(in.lights->count, static_cast<int32_t>(kMaxLights));
        std::memcpy(env.lights, in.lights->lights, sizeof(env.lights));
    }
    // キューブマップが無いのに skyMode==1 のままだと真っ黒になるので gradient へ落とす
    env.skyMode = (view.skyMode == 1 && in.skyCube == nullptr) ? 0 : view.skyMode;
    env.skyTop = view.skyTop; // RenderSystem がリニアへ変換済み
    env.skyHorizon = view.skyHorizon;
    env.skyBottom = view.skyBottom;
    // 自己交差回避。G-Buffer のワールド座標が R16F 精度なので余裕を持たせる
    env.rayEps = 0.01f;
    UploadCB(dc, envCB_.Get(), env);

    ID3D11Buffer* cbs[2] = { sceneCB_.Get(), envCB_.Get() };
    dc->CSSetConstantBuffers(0, 2, cbs);
    ID3D11ShaderResourceView* srvs[7] = { in.scene->nodes,     in.scene->tris,
                                          in.scene->attrs,     in.scene->tlas,
                                          in.scene->instances, in.scene->materials,
                                          in.skyCube };
    dc->CSSetShaderResources(0, 7, srvs);
    ID3D11SamplerState* samps[1] = { linearClamp_.Get() };
    dc->CSSetSamplers(0, 1, samps);
}

void RtPasses::UnbindCompute(GraphicsDevice& device)
{
    ID3D11DeviceContext* dc = device.Context();
    // 同じテクスチャを次のパスで SRV / RTV として使うので必ず外す
    // (テンポラルは履歴 ping-pong で「前フレーム書込先」を今フレーム SRV で読むため必須)
    ID3D11ShaderResourceView* nullSrvs[10] = {};
    ID3D11UnorderedAccessView* nullUavs[2] = {};
    dc->CSSetShaderResources(0, 10, nullSrvs);
    dc->CSSetUnorderedAccessViews(0, 2, nullUavs, nullptr);
    dc->CSSetShader(nullptr, nullptr, 0);
}

RtGiResult RtPasses::RenderGi(GraphicsDevice& device, ShaderManager& shaders,
                              const RenderView& view, const RtFrameInputs& in)
{
    RtGiResult result;
    if (!inited_ || !in.scene || !in.scene->IsValid() || !in.gbNormal || !in.gbPosition
        || !in.gbAlbedo || view.width <= 0 || view.height <= 0) {
        return result;
    }
    ShaderProgram* cs = shaders.Get(giCS_);
    if (!cs || !cs->valid || !cs->cs) {
        return result; // コンパイル失敗時は GI 無しで進む
    }

    const float scale = std::clamp(view.rtResolutionScale, 0.25f, 1.0f);
    const int gw = (std::max)(1, static_cast<int>(static_cast<float>(view.width) * scale));
    const int gh = (std::max)(1, static_cast<int>(static_cast<float>(view.height) * scale));
    giRt_.Resize(device, gw, gh, DXGI_FORMAT_R16G16B16A16_FLOAT, /*withDepth=*/false,
                 /*withUav=*/true);
    if (!giRt_.UAV()) {
        return result;
    }

    ID3D11DeviceContext* dc = device.Context();
    giTimer_.Begin(device);
    BindCommon(device, view, in);

    RtGiCB gi = {};
    gi.outSize[0] = static_cast<float>(gw);
    gi.outSize[1] = static_cast<float>(gh);
    gi.gbSize[0] = static_cast<float>(view.width);
    gi.gbSize[1] = static_cast<float>(view.height);
    gi.tMax = (view.farZ > 0.0f) ? view.farZ : 1000.0f;
    gi.frameIndex = view.rtFrameIndex;
    gi.bounces = (std::max)(1, view.rtBounces);
    UploadCB(dc, giCB_.Get(), gi);
    ID3D11Buffer* giCbs[1] = { giCB_.Get() };
    dc->CSSetConstantBuffers(2, 1, giCbs);

    ID3D11ShaderResourceView* gbuf[3] = { in.gbNormal, in.gbPosition, in.gbAlbedo };
    dc->CSSetShaderResources(7, 3, gbuf);
    ID3D11UnorderedAccessView* uavs[1] = { giRt_.UAV() };
    dc->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    dc->CSSetShader(cs->cs.Get(), nullptr, 0);
    dc->Dispatch(static_cast<UINT>((gw + 7) / 8), static_cast<UINT>((gh + 7) / 8), 1);

    UnbindCompute(device);
    giTimer_.End(device);

    // M46d: 履歴と混ぜる。off / シェーダ未コンパイルなら 1spp のまま返す
    result.raw = giRt_.SRV();
    ID3D11ShaderResourceView* acc = nullptr;
    if (view.rtTemporal != 0) {
        acc = Accumulate(device, shaders, view, in, gw, gh);
    } else {
        // 蓄積を切ったら履歴は連続しない — 次に入れたときは 1spp から積み直す
        const uint32_t key =
            (view.rtViewKey < static_cast<uint32_t>(kHistorySlots)) ? view.rtViewKey : 0u;
        giHist_[key].hasLast = false;
    }
    result.accumulated = (acc != nullptr) ? acc : result.raw;
    return result;
}

ID3D11ShaderResourceView* RtPasses::Accumulate(GraphicsDevice& device, ShaderManager& shaders,
                                               const RenderView& view, const RtFrameInputs& in,
                                               int gw, int gh)
{
    ShaderProgram* cs = shaders.Get(temporalCS_);
    if (!cs || !cs->valid || !cs->cs) {
        return nullptr; // コンパイル失敗時は 1spp のまま (絵は荒れるが壊れない)
    }
    const uint32_t key =
        (view.rtViewKey < static_cast<uint32_t>(kHistorySlots)) ? view.rtViewKey : 0u;
    GiHistory& h = giHist_[key];

    if (h.w != gw || h.h != gh) {
        for (int i = 0; i < 2; ++i) {
            h.color[i].Resize(device, gw, gh, DXGI_FORMAT_R16G16B16A16_FLOAT,
                              /*withDepth=*/false, /*withUav=*/true);
            h.geom[i].Resize(device, gw, gh, DXGI_FORMAT_R16G16B16A16_FLOAT,
                             /*withDepth=*/false, /*withUav=*/true);
        }
        h.w = gw;
        h.h = gh;
        h.write = 0;
        h.hasLast = false; // リサイズで履歴は捨てる
    }
    const int wr = h.write;
    const int rd = 1 - wr;
    if (!h.color[wr].UAV() || !h.geom[wr].UAV() || !h.color[rd].SRV() || !h.geom[rd].SRV()) {
        return nullptr;
    }

    // 履歴が使えるのは「同じビューが前フレームも描かれた」ときだけ。
    // lastSerial+1 == 今フレームの通番 で連続性を見る (RT を途中で on/off しても混ざらない)
    const bool histValid =
        h.hasLast && (h.lastSerial + 1u == view.rtViewSerial) && view.prevViewProjValid != 0;

    ID3D11DeviceContext* dc = device.Context();
    temporalTimer_.Begin(device);

    RtTemporalCB tc = {};
    XMStoreFloat4x4(&tc.prevViewProj, XMMatrixTranspose(XMLoadFloat4x4(&view.prevViewProj)));
    tc.outSize[0] = static_cast<float>(gw);
    tc.outSize[1] = static_cast<float>(gh);
    tc.gbSize[0] = static_cast<float>(view.width);
    tc.gbSize[1] = static_cast<float>(view.height);
    tc.prevCameraPos = view.prevCameraPos;
    tc.histValid = histValid ? 1 : 0;
    tc.cameraPos = view.cameraPos;
    tc.depthThreshold = kRtTemporalDepthThreshold;
    tc.normalThreshold = kRtTemporalNormalThreshold;
    UploadCB(dc, temporalCB_.Get(), tc);
    ID3D11Buffer* cbs[1] = { temporalCB_.Get() };
    dc->CSSetConstantBuffers(2, 1, cbs);

    ID3D11ShaderResourceView* srvs[6] = { giRt_.SRV(),   h.color[rd].SRV(), h.geom[rd].SRV(),
                                          in.gbNormal,   in.gbPosition,     in.gbAlbedo };
    dc->CSSetShaderResources(0, 6, srvs);
    ID3D11UnorderedAccessView* uavs[2] = { h.color[wr].UAV(), h.geom[wr].UAV() };
    dc->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
    dc->CSSetShader(cs->cs.Get(), nullptr, 0);
    dc->Dispatch(static_cast<UINT>((gw + 7) / 8), static_cast<UINT>((gh + 7) / 8), 1);

    UnbindCompute(device);
    temporalTimer_.End(device);

    h.lastSerial = view.rtViewSerial;
    h.hasLast = true;
    h.write = rd; // 次フレームは今書いた面を読む
    return h.color[wr].SRV();
}

bool RtPasses::Blit(GraphicsDevice& device, ShaderManager& shaders, const RenderView& view,
                    ID3D11ShaderResourceView* src, int mode)
{
    ShaderProgram* blit = shaders.Get(blitShader_);
    if (!blit || !blit->valid || src == nullptr) {
        return false;
    }
    ID3D11DeviceContext* dc = device.Context();

    RtBlitCB bc = {};
    bc.dstSize[0] = static_cast<float>(view.width);
    bc.dstSize[1] = static_cast<float>(view.height);
    bc.mode = mode;
    bc.param = static_cast<float>(kRtTemporalMaxHistory);
    UploadCB(dc, blitCB_.Get(), bc);

    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(view.width);
    vp.Height = static_cast<float>(view.height);
    vp.MaxDepth = 1.0f;
    ID3D11RenderTargetView* rtvs[1] = { view.rtv };
    dc->OMSetRenderTargets(1, rtvs, nullptr);
    dc->RSSetViewports(1, &vp);
    dc->RSSetState(raster_.Get());
    dc->OMSetDepthStencilState(depthDisabled_.Get(), 0);
    dc->OMSetBlendState(blendOpaque_.Get(), nullptr, 0xFFFFFFFFu);
    dc->IASetInputLayout(nullptr);
    dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11Buffer* cbs[1] = { blitCB_.Get() };
    dc->PSSetConstantBuffers(0, 1, cbs);
    ID3D11SamplerState* samps[1] = { linearClamp_.Get() };
    dc->PSSetSamplers(0, 1, samps);
    ID3D11ShaderResourceView* srvs[1] = { src };
    dc->PSSetShaderResources(0, 1, srvs);
    dc->VSSetShader(blit->vs.Get(), nullptr, 0);
    dc->PSSetShader(blit->ps.Get(), nullptr, 0);
    dc->Draw(3, 0);
    ID3D11ShaderResourceView* nullSrv[1] = {};
    dc->PSSetShaderResources(0, 1, nullSrv);
    return true;
}

bool RtPasses::RenderDebug(GraphicsDevice& device, ShaderManager& shaders, const RenderView& view,
                           const RtFrameInputs& in, const RtGiResult& gi)
{
    if (!inited_ || view.rtDebugMode == 0 || !in.scene || !in.scene->IsValid()
        || view.rtv == nullptr || view.width <= 0 || view.height <= 0) {
        return false;
    }

    // モード 4-6 は GI バッファをそのまま拡大表示する (CS は RenderGi で実行済み)
    if (view.rtDebugMode == 4) {
        return Blit(device, shaders, view, gi.raw);
    }
    if (view.rtDebugMode == 5) {
        return Blit(device, shaders, view, gi.accumulated);
    }
    if (view.rtDebugMode == 6) { // 履歴長 (a) のヒートマップ
        return Blit(device, shaders, view, gi.accumulated, /*mode=*/1);
    }

    ShaderProgram* cs = shaders.Get(debugCS_);
    if (!cs || !cs->valid || !cs->cs) {
        return false;
    }
    debugRt_.Resize(device, view.width, view.height, DXGI_FORMAT_R16G16B16A16_FLOAT,
                    /*withDepth=*/false, /*withUav=*/true);
    if (!debugRt_.UAV()) {
        return false;
    }

    ID3D11DeviceContext* dc = device.Context();
    debugTimer_.Begin(device);
    BindCommon(device, view, in);

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
    ID3D11Buffer* dbgCbs[1] = { debugCB_.Get() };
    dc->CSSetConstantBuffers(2, 1, dbgCbs);

    ID3D11UnorderedAccessView* uavs[1] = { debugRt_.UAV() };
    dc->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    dc->CSSetShader(cs->cs.Get(), nullptr, 0);
    dc->Dispatch(static_cast<UINT>((view.width + 7) / 8), static_cast<UINT>((view.height + 7) / 8),
                 1);
    UnbindCompute(device);
    debugTimer_.End(device);

    return Blit(device, shaders, view, debugRt_.SRV());
}

} // namespace mye
