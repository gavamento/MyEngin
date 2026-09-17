#include "Engine/Renderer/RayTracing/RtPasses.h"

#include <algorithm>
#include <cstring>

#include "Engine/Core/Log.h"
#include "Engine/Renderer/GpuBufferUtil.h"
#include "Engine/Renderer/GraphicsDevice.h"
#include "Engine/Renderer/RayTracing/RtMath.h"  // M46g: 太陽コーンの cos (CPU が唯一の出所)
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
    // M46h: この信号の履歴長上限 (GI = kRtTemporalMaxHistory / 反射 = kRtReflMaxHistory)。
    // HLSL 側で MYE_RT_TEMPORAL_MAX_HISTORY とのより小さい方に丸められる
    float maxHistory = static_cast<float>(kRtTemporalMaxHistory);
    // M55f: 1 = 履歴 UV を GBuffer RT4 から作る。0 = 前フレーム VP へ射影 (M46d の挙動)。
    // pad を 1 つ潰しているので CB のサイズは 128 のまま
    int32_t useVelocity = 0;
    float pad = 0;
};
static_assert(sizeof(RtTemporalCB) == 128, "HLSL の RtTemporalCB と一致させること");

// rt_variance.cs.hlsl の RtVarianceCB (b2) と一致
struct RtVarianceCB {
    float size[2] = { 0, 0 };
    float historyMin = kRtVarianceHistoryMin;
    int32_t forceSpatial = 0;
    float depthThreshold = kRtTemporalDepthThreshold;
    float normalThreshold = kRtTemporalNormalThreshold;
    float pad[2] = { 0, 0 };
};
static_assert(sizeof(RtVarianceCB) == 32, "HLSL の RtVarianceCB と一致させること");

// rt_atrous.cs.hlsl の RtAtrousCB (b2) と一致
struct RtAtrousCB {
    float size[2] = { 0, 0 };
    int32_t step = 1;
    float sigmaDepth = kRtAtrousSigmaDepth;
    float sigmaNormal = kRtAtrousSigmaNormal;
    float sigmaLuma = kRtAtrousSigmaLuma;
    float pad[2] = { 0, 0 };
};
static_assert(sizeof(RtAtrousCB) == 32, "HLSL の RtAtrousCB と一致させること");

// rt_shadow.cs.hlsl の RtShadowCB (b2) と一致
struct RtShadowCB {
    float size[2] = { 0, 0 };
    float cosThetaMax = 1.0f;
    uint32_t frameIndex = 0;
    XMFLOAT3 cameraPos = { 0, 0, 0 };
    float epsMin = kRtSurfaceEpsMin;
    float epsRel = kRtSurfaceEpsRel;
    float pad[3] = { 0, 0, 0 };
};
static_assert(sizeof(RtShadowCB) == 48, "HLSL の RtShadowCB と一致させること");

// rt_shadow_filter.cs.hlsl の RtShadowFilterCB (b2) と一致
struct RtShadowFilterCB {
    float size[2] = { 0, 0 };
    int32_t step = 1;
    float sigmaDepth = kRtAtrousSigmaDepth;
    XMFLOAT3 cameraPos = { 0, 0, 0 };
    float sigmaNormal = kRtAtrousSigmaNormal;
    int32_t axis[2] = { 1, 0 }; // (1,0) = 水平 / (0,1) = 垂直 (分離型)
    float pad[2] = { 0, 0 };
};
static_assert(sizeof(RtShadowFilterCB) == 48, "HLSL の RtShadowFilterCB と一致させること");

// rt_refl.cs.hlsl の RtReflCB (b2) と一致
struct RtReflCB {
    float outSize[2] = { 0, 0 };
    float gbSize[2] = { 0, 0 };
    XMFLOAT3 cameraPos = { 0, 0, 0 };
    float tMax = 1000.0f;
    uint32_t frameIndex = 0;
    int32_t bounces = 1;
    float maxRoughness = kRtReflMaxRoughness;
    float epsMin = kRtSurfaceEpsMin;
    float epsRel = kRtSurfaceEpsRel;
    float pad[3] = { 0, 0, 0 };
};
static_assert(sizeof(RtReflCB) == 64, "HLSL の RtReflCB と一致させること");

// M67d: assets/shaders/rt_restir_cb.hlsli の RtRestirCB (b3) と一致。
// **並びは HLSL のパッキング規則に合わせてある** — float3 の直後にスカラーを 1 つ置いて
// 16 バイト行を埋め、float4 配列は 16 バイト境界 (M67f で 144 → offset 160) から始める。
// 途中に 1 つ足すと配列の開始がずれて再利用パラメータが丸ごと化けるので、
// 追加は**必ず gRsClass の直前まで**で、サイズの static_assert を必ず更新すること。
// gRsRadiusAlphaRef (M67f) の後ろに **明示パディング float3 を置いて 240 B** にしてある
// (無いと C++ は 148、HLSL は 160 から配列を始めて 12 バイトずれる)
struct RtRestirCB {
    XMFLOAT4X4 prevViewProj = {}; // 転置済み
    XMFLOAT3 prevCameraPos = { 0, 0, 0 };
    int32_t on = 0;
    XMFLOAT3 cameraPos = { 0, 0, 0 };
    int32_t spatialOn = 0;
    float outSize[2] = { 0, 0 };
    float gbSize[2] = { 0, 0 };
    int32_t visRay = 0;
    int32_t histValid = 0;
    int32_t useVelocity = 0;
    int32_t classOverride = -1;
    // M67h: 使っていない枠。**フレーム番号は ReSTIR には混ぜない** — タップ回転に
    // フレーム項を入れると候補集合が毎フレーム入れ替わり、乗り換えがそのままフリッカーになる
    // (M67f の実測でフリッカー 2 倍)。名前だけ pad にして理由を書かないと次の人が
    // 同じ理由で足し直すので、ここに残す。
    // 混ぜたくなったら先に RtTypes.h の kRtRestirTapSeed のコメントを読むこと。
    // 枠を潰さず残しているのは 240 B / offsetof(classTable) == 160 を動かさないため
    uint32_t pad1 = 0;
    float depthThreshold = kRtTemporalDepthThreshold;
    float normalThreshold = kRtTemporalNormalThreshold;
    float jacobianMax = kRtRestirJacobianMax;
    float radiusAlphaRef = kRtRestirRadiusAlphaRef; // M67f
    float pad0[3] = { 0, 0, 0 }; // gRsClass を 16 バイト境界へ揃えるための明示パディング
    RtReflClassParams classTable[kRtReflClassCount] = {};
};
static_assert(sizeof(RtRestirCB) == 240, "HLSL の RtRestirCB と一致させること");
static_assert(offsetof(RtRestirCB, classTable) == 160,
              "gRsClass は 16 バイト境界から始まること (HLSL の配列パッキング)");

// viewKey → 履歴スロット (範囲外は 0 へ丸める)。GI と反射で同じ写像を使う
uint32_t HistorySlot(uint32_t viewKey, int slots)
{
    return (viewKey < static_cast<uint32_t>(slots)) ? viewKey : 0u;
}

// rt_blit.hlsl の RtBlitCB (b0) と一致
struct RtBlitCB {
    float dstSize[2] = { 0, 0 };
    // 0 = rgb / 1 = a を履歴長 / 2 = a を分散のヒートマップ / 3 = r をグレースケール /
    // 4 = a を ReflectionClass の色として表示 (M67d)
    int32_t mode = 0;
    float param = 1.0f; // ヒートマップの正規化スケール
};

} // namespace

bool RtPasses::Init(GraphicsDevice& device, ShaderManager& shaders)
{
    if (inited_) {
        return true;
    }
    ID3D11Device* dev = device.Device();

    // ★シェーダはここでは読まない。各パスが初めて走ったときに Program() が読む —
    //   ここで 10 本まとめて読むと、影だけ on にしても GI / 反射 / ReSTIR / デバッグの
    //   コンパイル (fxc 実測で計 6.6 秒) を払っていた
    (void)shaders;

    if (!CreateConstant(dev, sizeof(RtSceneCB), sceneCB_)
        || !CreateConstant(dev, sizeof(RtEnvCB), envCB_)
        || !CreateConstant(dev, sizeof(RtDebugCB), debugCB_)
        || !CreateConstant(dev, sizeof(RtGiCB), giCB_)
        || !CreateConstant(dev, sizeof(RtTemporalCB), temporalCB_)
        || !CreateConstant(dev, sizeof(RtVarianceCB), varianceCB_)
        || !CreateConstant(dev, sizeof(RtAtrousCB), atrousCB_)
        || !CreateConstant(dev, sizeof(RtShadowCB), shadowCB_)
        || !CreateConstant(dev, sizeof(RtShadowFilterCB), shadowFilterCB_)
        || !CreateConstant(dev, sizeof(RtReflCB), reflCB_)
        || !CreateConstant(dev, sizeof(RtRestirCB), restirCB_) // M67d
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
    // M67d: デバッグ 12 / 14 用の点サンプラ。クラス番号 (整数) を線形補間すると
    // 境界に「隣り合う 2 クラスの中間の番号」= 存在しないクラスの色が出る
    D3D11_SAMPLER_DESC pd = sd;
    pd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    if (FAILED(dev->CreateSamplerState(&sd, linearClamp_.GetAddressOf()))
        || FAILED(dev->CreateSamplerState(&pd, pointClamp_.GetAddressOf()))
        || FAILED(dev->CreateDepthStencilState(&dsd, depthDisabled_.GetAddressOf()))
        || FAILED(dev->CreateBlendState(&bd, blendOpaque_.GetAddressOf()))
        || FAILED(dev->CreateRasterizerState(&rd, raster_.GetAddressOf()))) {
        MYE_LOG_ERROR("RtPasses: pipeline state creation failed");
        return false;
    }

    debugTimer_.Init(device);
    giTimer_.Init(device);
    temporalTimer_.Init(device);
    svgfTimer_.Init(device);
    shadowTimer_.Init(device);
    shadowFilterTimer_.Init(device);
    reflTimer_.Init(device);
    reflTemporalTimer_.Init(device);
    reflSvgfTimer_.Init(device);
    restirTimer_.Init(device); // M67d
    inited_ = true;
    return true;
}

void RtPasses::Shutdown()
{
    debugRt_.Release();
    giRt_.Release();
    reflRt_.Release();
    reflRestirRt_.Release(); // M67d
    for (RenderTexture& rt : svgfRt_) {
        rt.Release();
    }
    for (RenderTexture& rt : shadowRt_) {
        rt.Release();
    }
    for (RenderTexture& rt : reflSvgfRt_) {
        rt.Release();
    }
    for (RtHistory* set : { giHist_, reflHist_ }) {
        for (int k = 0; k < kHistorySlots; ++k) {
            RtHistory& h = set[k];
            for (int i = 0; i < 2; ++i) {
                h.color[i].Release();
                h.geom[i].Release();
                h.moments[i].Release();
            }
            h.write = 0;
            h.w = 0;
            h.h = 0;
            h.lastSerial = 0;
            h.hasLast = false;
        }
    }
    // M67d: reservoir (一度も ReSTIR を使っていなければ全部空のまま)
    for (RtReservoirSlot& slot : reservoirs_) {
        for (RtReservoirSet& set : slot.set) {
            set.pos.Release();
            set.rad.Release();
            set.nrm.Release();
            set.geom.Release();
            set.rpos.Release();
        }
        slot.w = 0;
        slot.h = 0;
        slot.lastSerial = 0;
        slot.hasLast = false;
    }
    sceneCB_.Reset();
    envCB_.Reset();
    debugCB_.Reset();
    giCB_.Reset();
    temporalCB_.Reset();
    varianceCB_.Reset();
    atrousCB_.Reset();
    shadowCB_.Reset();
    shadowFilterCB_.Reset();
    reflCB_.Reset();
    restirCB_.Reset(); // M67d
    blitCB_.Reset();
    linearClamp_.Reset();
    pointClamp_.Reset(); // M67d
    depthDisabled_.Reset();
    blendOpaque_.Reset();
    raster_.Reset();
    inited_ = false;
}

ShaderProgram* RtPasses::Program(ShaderManager& shaders, AssetID& id, const char* name)
{
    if (id.IsNull()) {
        // LoadCompute は同名を 2 度コンパイルしない (ID が既にあれば返すだけ) ので、
        // 失敗したシェーダも毎フレーム再試行にはならない = 壊れた .hlsl でフレームが止まらない
        id = shaders.LoadCompute(name);
    }
    return shaders.Get(id);
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
    // (テンポラルは履歴 ping-pong で「前フレーム書込先」を今フレーム SRV で読むため必須。
    //  SVGF も ping-pong で書いた面を次の反復で読むので同様)。
    // 上限は ReSTIR が使う t16 (M67e = 画面速度) / u5 まで。**ここを伸ばし忘れると
    // 「同じ reservoir を SRV と UAV で同時に張った」で D3D が片方を黙って外す**
    ID3D11ShaderResourceView* nullSrvs[17] = {};
    ID3D11UnorderedAccessView* nullUavs[6] = {};
    dc->CSSetShaderResources(0, 17, nullSrvs);
    dc->CSSetUnorderedAccessViews(0, 6, nullUavs, nullptr);
    dc->CSSetShader(nullptr, nullptr, 0);
}

RtGiResult RtPasses::RenderGi(GraphicsDevice& device, ShaderManager& shaders,
                              const RenderView& view, const RtFrameInputs& in)
{
    RtGiResult result;
    // gbMaterial は必須 (a = RT を受ける面か。null を張ると Load が 0 = 全画素「受けない」になる)
    if (!inited_ || !in.scene || !in.scene->IsValid() || !in.gbNormal || !in.gbPosition
        || !in.gbAlbedo || !in.gbMaterial || view.width <= 0 || view.height <= 0) {
        return result;
    }
    ShaderProgram* cs = Program(shaders, giCS_, "rt_gi.cs");
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

    ID3D11ShaderResourceView* gbuf[4] = { in.gbNormal, in.gbPosition, in.gbAlbedo, in.gbMaterial };
    dc->CSSetShaderResources(7, 4, gbuf);
    ID3D11UnorderedAccessView* uavs[1] = { giRt_.UAV() };
    dc->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    dc->CSSetShader(cs->cs.Get(), nullptr, 0);
    dc->Dispatch(static_cast<UINT>((gw + 7) / 8), static_cast<UINT>((gh + 7) / 8), 1);

    UnbindCompute(device);
    giTimer_.End(device);

    // M46d: 履歴と混ぜる。off / シェーダ未コンパイルなら 1spp のまま返す
    result.raw = giRt_.SRV();
    result.accumulated = result.raw;
    result.filtered = result.raw;
    if (view.rtTemporal != 0) {
        const AccumResult acc =
            Accumulate(device, shaders, view, in, gw, gh, giRt_.SRV(),
                       giHist_[HistorySlot(view.rtViewKey, kHistorySlots)],
                       static_cast<float>(kRtTemporalMaxHistory), temporalTimer_);
        if (acc.color != nullptr) {
            result.accumulated = acc.color;
            result.filtered = acc.color;
            // M46e: 分散推定 + A-Trous。幾何バッファが蓄積パスの副産物なので順序は固定
            if (view.rtSvgf != 0) {
                ID3D11ShaderResourceView* f =
                    Denoise(device, shaders, view, acc, gw, gh, svgfRt_, kRtAtrousIterations,
                            kRtAtrousSigmaLuma, svgfTimer_);
                if (f != nullptr) {
                    result.filtered = f;
                }
            }
        }
    } else {
        // 蓄積を切ったら履歴は連続しない — 次に入れたときは 1spp から積み直す
        giHist_[HistorySlot(view.rtViewKey, kHistorySlots)].hasLast = false;
    }
    return result;
}

RtPasses::AccumResult RtPasses::Accumulate(GraphicsDevice& device, ShaderManager& shaders,
                                           const RenderView& view, const RtFrameInputs& in, int gw,
                                           int gh, ID3D11ShaderResourceView* src, RtHistory& h,
                                           float maxHistory, GpuTimer& timer)
{
    AccumResult out;
    ShaderProgram* cs = Program(shaders, temporalCS_, "rt_temporal.cs");
    if (!cs || !cs->valid || !cs->cs || src == nullptr) {
        return out; // コンパイル失敗時は 1spp のまま (絵は荒れるが壊れない)
    }

    if (h.w != gw || h.h != gh) {
        for (int i = 0; i < 2; ++i) {
            h.color[i].Resize(device, gw, gh, DXGI_FORMAT_R16G16B16A16_FLOAT,
                              /*withDepth=*/false, /*withUav=*/true);
            h.geom[i].Resize(device, gw, gh, DXGI_FORMAT_R16G16B16A16_FLOAT,
                             /*withDepth=*/false, /*withUav=*/true);
            h.moments[i].Resize(device, gw, gh, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                /*withDepth=*/false, /*withUav=*/true);
        }
        h.w = gw;
        h.h = gh;
        h.write = 0;
        h.hasLast = false; // リサイズで履歴は捨てる
    }
    const int wr = h.write;
    const int rd = 1 - wr;
    if (!h.color[wr].UAV() || !h.geom[wr].UAV() || !h.moments[wr].UAV() || !h.color[rd].SRV()
        || !h.geom[rd].SRV() || !h.moments[rd].SRV()) {
        return out;
    }

    // 履歴が使えるのは「同じビューが前フレームも描かれた」ときだけ。
    // lastSerial+1 == 今フレームの通番 で連続性を見る (RT を途中で on/off しても混ざらない)
    const bool histValid =
        h.hasLast && (h.lastSerial + 1u == view.rtViewSerial) && view.prevViewProjValid != 0;

    ID3D11DeviceContext* dc = device.Context();
    timer.Begin(device);

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
    tc.maxHistory = maxHistory; // M46h: 反射は GI より短く積む (鏡面のラグを避けるため)
    // M55f: 画面速度で履歴 UV を作れるのは「RT4 が張れている かつ 履歴が有効」なときだけ。
    // histValid には prevViewProjValid が含まれており、それは GBuffer 側の gVelocityValid と
    // **同じ条件** — つまり velocity が全画素 0 で書かれたフレームをここで読むことはない
    tc.useVelocity = (in.gbVelocity != nullptr && histValid) ? 1 : 0;
    UploadCB(dc, temporalCB_.Get(), tc);
    ID3D11Buffer* cbs[1] = { temporalCB_.Get() };
    dc->CSSetConstantBuffers(2, 1, cbs);

    ID3D11ShaderResourceView* srvs[9] = { src,
                                          h.color[rd].SRV(),
                                          h.geom[rd].SRV(),
                                          in.gbNormal,
                                          in.gbPosition,
                                          in.gbAlbedo,
                                          h.moments[rd].SRV(),
                                          in.gbVelocity,   // M55f: t7
                                          in.gbMaterial }; // 汎用タグ: t8 (a = RT を受ける面か)
    dc->CSSetShaderResources(0, 9, srvs);
    ID3D11UnorderedAccessView* uavs[3] = { h.color[wr].UAV(), h.geom[wr].UAV(),
                                           h.moments[wr].UAV() };
    dc->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
    dc->CSSetShader(cs->cs.Get(), nullptr, 0);
    dc->Dispatch(static_cast<UINT>((gw + 7) / 8), static_cast<UINT>((gh + 7) / 8), 1);

    UnbindCompute(device);
    timer.End(device);

    h.lastSerial = view.rtViewSerial;
    h.hasLast = true;
    h.write = rd; // 次フレームは今書いた面を読む
    out.color = h.color[wr].SRV();
    out.geom = h.geom[wr].SRV();
    out.moments = h.moments[wr].SRV();
    return out;
}

// M46e: SVGF の空間側。分散推定 → A-Trous を刻み幅 1,2,4 と倍化しながら iterations 回。
// ping-pong (pp) はビュー間で共有する — 同じフレーム内で使い切るので履歴と違い分ける必要はない。
// ただし**信号間 (GI と反射) では分けること**: GI の結果はライトパスまで t9 で生きている
ID3D11ShaderResourceView* RtPasses::Denoise(GraphicsDevice& device, ShaderManager& shaders,
                                            const RenderView& view, const AccumResult& acc, int gw,
                                            int gh, RenderTexture (&pp)[2], int iterations,
                                            float sigmaLuma, GpuTimer& timer)
{
    ShaderProgram* varCs = Program(shaders, varianceCS_, "rt_variance.cs");
    ShaderProgram* atrousCs = Program(shaders, atrousCS_, "rt_atrous.cs");
    if (!varCs || !varCs->valid || !varCs->cs || !atrousCs || !atrousCs->valid || !atrousCs->cs) {
        return nullptr; // コンパイル失敗時は蓄積結果のまま (ノイズは残るが壊れない)
    }
    for (RenderTexture& rt : pp) {
        rt.Resize(device, gw, gh, DXGI_FORMAT_R16G16B16A16_FLOAT, /*withDepth=*/false,
                  /*withUav=*/true);
        if (!rt.UAV() || !rt.SRV()) {
            return nullptr;
        }
    }

    ID3D11DeviceContext* dc = device.Context();
    const UINT gx = static_cast<UINT>((gw + 7) / 8);
    const UINT gy = static_cast<UINT>((gh + 7) / 8);
    timer.Begin(device);

    // ---- 1) 分散推定 → pp[0] (rgb = 色, a = 分散) ----
    RtVarianceCB vc = {};
    vc.size[0] = static_cast<float>(gw);
    vc.size[1] = static_cast<float>(gh);
    vc.historyMin = kRtVarianceHistoryMin;
    // シード凍結中は毎フレーム同じサンプル = テンポラル分散が 0 に潰れるので空間推定を使う
    vc.forceSpatial = (view.rtFreezeSeed != 0) ? 1 : 0;
    vc.depthThreshold = kRtTemporalDepthThreshold;
    vc.normalThreshold = kRtTemporalNormalThreshold;
    UploadCB(dc, varianceCB_.Get(), vc);
    ID3D11Buffer* varCbs[1] = { varianceCB_.Get() };
    dc->CSSetConstantBuffers(2, 1, varCbs);
    ID3D11ShaderResourceView* varSrvs[3] = { acc.color, acc.moments, acc.geom };
    dc->CSSetShaderResources(0, 3, varSrvs);
    ID3D11UnorderedAccessView* varUavs[1] = { pp[0].UAV() };
    dc->CSSetUnorderedAccessViews(0, 1, varUavs, nullptr);
    dc->CSSetShader(varCs->cs.Get(), nullptr, 0);
    dc->Dispatch(gx, gy, 1);
    UnbindCompute(device);

    // ---- 2) A-Trous (刻み幅を倍化しながら ping-pong) ----
    int src = 0;
    for (int i = 0; i < iterations; ++i) {
        const int dst = 1 - src;
        RtAtrousCB ac = {};
        ac.size[0] = static_cast<float>(gw);
        ac.size[1] = static_cast<float>(gh);
        ac.step = 1 << i;
        ac.sigmaDepth = kRtAtrousSigmaDepth;
        ac.sigmaNormal = kRtAtrousSigmaNormal;
        ac.sigmaLuma = sigmaLuma;
        UploadCB(dc, atrousCB_.Get(), ac);
        ID3D11Buffer* atCbs[1] = { atrousCB_.Get() };
        dc->CSSetConstantBuffers(2, 1, atCbs);
        ID3D11ShaderResourceView* atSrvs[2] = { pp[src].SRV(), acc.geom };
        dc->CSSetShaderResources(0, 2, atSrvs);
        ID3D11UnorderedAccessView* atUavs[1] = { pp[dst].UAV() };
        dc->CSSetUnorderedAccessViews(0, 1, atUavs, nullptr);
        dc->CSSetShader(atrousCs->cs.Get(), nullptr, 0);
        dc->Dispatch(gx, gy, 1);
        UnbindCompute(device);
        src = dst;
    }

    timer.End(device);
    return pp[src].SRV();
}

// M46g: 太陽の可視率 (フル解像度 R8)。1 画素 1 レイの any-hit + スカラー空間フィルタ。
// GI と違いテンポラル履歴を持たない — 動く物体の影がゴーストするのを避けるため
// (太陽コーンが狭いので 1spp でも半影の数画素にしかノイズが出ない)
ID3D11ShaderResourceView* RtPasses::RenderShadow(GraphicsDevice& device, ShaderManager& shaders,
                                                 const RenderView& view, const RtFrameInputs& in)
{
    if (!inited_ || !in.scene || !in.scene->IsValid() || !in.gbNormal || !in.gbPosition
        || !in.gbAlbedo || !in.gbMaterial || view.width <= 0 || view.height <= 0) {
        return nullptr; // gbMaterial は必須 (RenderGi と同じ理由)
    }
    ShaderProgram* cs = Program(shaders, shadowCS_, "rt_shadow.cs");
    if (!cs || !cs->valid || !cs->cs) {
        return nullptr; // コンパイル失敗時は影なしで進む (ライトパスは CSM のまま)
    }
    for (RenderTexture& rt : shadowRt_) {
        rt.Resize(device, view.width, view.height, DXGI_FORMAT_R8_UNORM, /*withDepth=*/false,
                  /*withUav=*/true);
        if (!rt.UAV() || !rt.SRV()) {
            return nullptr;
        }
    }

    ID3D11DeviceContext* dc = device.Context();
    const UINT gx = static_cast<UINT>((view.width + 7) / 8);
    const UINT gy = static_cast<UINT>((view.height + 7) / 8);
    shadowTimer_.Begin(device);
    BindCommon(device, view, in);

    // ---- 1) 影レイ (1spp、太陽コーンサンプル) → shadowRt_[0] ----
    RtShadowCB sc = {};
    sc.size[0] = static_cast<float>(view.width);
    sc.size[1] = static_cast<float>(view.height);
    sc.cosThetaMax = RtConeCosMax(kRtShadowSunAngleDeg);
    sc.frameIndex = view.rtFrameIndex;
    sc.cameraPos = view.cameraPos;
    sc.epsMin = kRtSurfaceEpsMin;
    sc.epsRel = kRtSurfaceEpsRel;
    UploadCB(dc, shadowCB_.Get(), sc);
    ID3D11Buffer* shCbs[1] = { shadowCB_.Get() };
    dc->CSSetConstantBuffers(2, 1, shCbs);
    ID3D11ShaderResourceView* gbuf[4] = { in.gbNormal, in.gbPosition, in.gbAlbedo, in.gbMaterial };
    dc->CSSetShaderResources(7, 4, gbuf);
    ID3D11UnorderedAccessView* uavs[1] = { shadowRt_[0].UAV() };
    dc->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    dc->CSSetShader(cs->cs.Get(), nullptr, 0);
    dc->Dispatch(gx, gy, 1);
    UnbindCompute(device);

    shadowTimer_.End(device);

    // ---- 2) 空間フィルタ (分離型: 水平 → 垂直 で 1 反復。刻み幅を倍化しながら ping-pong) ----
    int src = 0;
    ShaderProgram* filterCs = Program(shaders, shadowFilterCS_, "rt_shadow_filter.cs");
    if (filterCs && filterCs->valid && filterCs->cs) {
        shadowFilterTimer_.Begin(device);
        for (int i = 0; i < kRtShadowFilterIterations * 2; ++i) {
            const int dst = 1 - src;
            RtShadowFilterCB fc = {};
            fc.size[0] = static_cast<float>(view.width);
            fc.size[1] = static_cast<float>(view.height);
            fc.step = 1 << (i / 2);
            fc.sigmaDepth = kRtAtrousSigmaDepth;
            fc.cameraPos = view.cameraPos;
            fc.sigmaNormal = kRtAtrousSigmaNormal;
            fc.axis[0] = (i % 2 == 0) ? 1 : 0; // 偶数 = 水平、奇数 = 垂直
            fc.axis[1] = (i % 2 == 0) ? 0 : 1;
            UploadCB(dc, shadowFilterCB_.Get(), fc);
            ID3D11Buffer* fCbs[1] = { shadowFilterCB_.Get() };
            dc->CSSetConstantBuffers(2, 1, fCbs);
            ID3D11ShaderResourceView* fSrvs[4] = { shadowRt_[src].SRV(), in.gbNormal,
                                                   in.gbPosition, in.gbMaterial };
            dc->CSSetShaderResources(0, 4, fSrvs);
            ID3D11UnorderedAccessView* fUavs[1] = { shadowRt_[dst].UAV() };
            dc->CSSetUnorderedAccessViews(0, 1, fUavs, nullptr);
            dc->CSSetShader(filterCs->cs.Get(), nullptr, 0);
            dc->Dispatch(gx, gy, 1);
            UnbindCompute(device);
            src = dst;
        }
        shadowFilterTimer_.End(device);
    }
    return shadowRt_[src].SRV();
}

// M46h: 鏡面反射 (内部解像度)。GGX VNDF で 1 本撃ち、GI と同じ 2 段のデノイズを掛ける。
// GI と違い履歴上限を短く (kRtReflMaxHistory) 取り、A-Trous も反復を減らして
// 輝度エッジ停止を厳しくする — 反射像は「本物のディテール」なので均しすぎると溶ける
RtReflResult RtPasses::RenderReflection(GraphicsDevice& device, ShaderManager& shaders,
                                        const RenderView& view, const RtFrameInputs& in)
{
    RtReflResult result;
    if (!inited_ || !in.scene || !in.scene->IsValid() || !in.gbNormal || !in.gbPosition
        || !in.gbAlbedo || !in.gbMaterial || view.width <= 0 || view.height <= 0) {
        return result;
    }
    ShaderProgram* cs = Program(shaders, reflCS_, "rt_refl.cs");
    if (!cs || !cs->valid || !cs->cs) {
        return result; // コンパイル失敗時は反射無しで進む (合成側は IBL のまま)
    }

    const float scale = std::clamp(view.rtResolutionScale, 0.25f, 1.0f);
    const int gw = (std::max)(1, static_cast<int>(static_cast<float>(view.width) * scale));
    const int gh = (std::max)(1, static_cast<int>(static_cast<float>(view.height) * scale));
    reflRt_.Resize(device, gw, gh, DXGI_FORMAT_R16G16B16A16_FLOAT, /*withDepth=*/false,
                   /*withUav=*/true);
    if (!reflRt_.UAV()) {
        return result;
    }

    // M67d: ReSTIR。**ここを通らない限り reservoir は 1 バイトも確保しない**。
    // シェーダのコンパイルに失敗したら現行経路へ黙って縮退する (絵は ReSTIR off と同じ)
    RtReservoirSlot& slot = reservoirs_[HistorySlot(view.rtViewKey, kHistorySlots)];
    // ReSTIR が off なら spatial のシェーダは読みもしない (遅延ロードの効き目がここで出る)
    ShaderProgram* restirCs = (view.rtReflRestir != 0)
        ? Program(shaders, restirCS_, "rt_refl_restir_spatial.cs")
        : nullptr;
    const bool restirOn = view.rtReflRestir != 0 && restirCs != nullptr && restirCs->valid
        && restirCs->cs && EnsureReservoirs(device, slot, gw, gh);
    if (!restirOn) {
        // ReSTIR を切ったフレームは履歴を捨てる (on/off を跨いで混ざらない)
        slot.hasLast = false;
    }

    ID3D11DeviceContext* dc = device.Context();
    reflTimer_.Begin(device);
    BindCommon(device, view, in);

    RtReflCB rc = {};
    rc.outSize[0] = static_cast<float>(gw);
    rc.outSize[1] = static_cast<float>(gh);
    rc.gbSize[0] = static_cast<float>(view.width);
    rc.gbSize[1] = static_cast<float>(view.height);
    rc.cameraPos = view.cameraPos;
    rc.tMax = (view.farZ > 0.0f) ? view.farZ : 1000.0f;
    rc.frameIndex = view.rtFrameIndex;
    rc.bounces = (std::max)(1, view.rtBounces);
    rc.maxRoughness = kRtReflMaxRoughness;
    rc.epsMin = kRtSurfaceEpsMin;
    rc.epsRel = kRtSurfaceEpsRel;
    UploadCB(dc, reflCB_.Get(), rc);
    ID3D11Buffer* reflCbs[1] = { reflCB_.Get() };
    dc->CSSetConstantBuffers(2, 1, reflCbs);

    // M67d: 履歴が使えるのは「同じビューが前フレームも ReSTIR で描かれた」ときだけ。
    // 判定は Accumulate と同じ規約 (lastSerial+1 == 今フレームの通番 かつ prevViewProj 有効)
    const bool rsHistValid =
        restirOn && slot.hasLast && (slot.lastSerial + 1u == view.rtViewSerial)
        && view.prevViewProjValid != 0;
    // ★**off でも必ず上げて張る** — UnbindCompute は SRV/UAV しか外さないので、
    //   「off にしたら b3 を張らない」にすると前フレームの gRsOn = 1 が残ったままになり、
    //   トグルを切った次のフレームがまだ ReSTIR 経路を走る (UAV は張られていないので
    //   書き込みは捨てられるが、無駄な計算をしたうえで挙動が状態依存になる)。
    //   off のときの中身は on = 0 だけが意味を持つ
    RtRestirCB rs = {};
    XMStoreFloat4x4(&rs.prevViewProj, XMMatrixTranspose(XMLoadFloat4x4(&view.prevViewProj)));
    rs.prevCameraPos = view.prevCameraPos;
    rs.on = restirOn ? 1 : 0;
    rs.cameraPos = view.cameraPos;
    rs.spatialOn = (view.rtReflRestirParams.spatial != 0) ? 1 : 0;
    rs.outSize[0] = static_cast<float>(gw);
    rs.outSize[1] = static_cast<float>(gh);
    rs.gbSize[0] = static_cast<float>(view.width);
    rs.gbSize[1] = static_cast<float>(view.height);
    rs.visRay = (view.rtReflRestirParams.visRay != 0) ? 1 : 0;
    rs.histValid = rsHistValid ? 1 : 0;
    // M55f と同じ条件 — velocity が全画素 0 のフレームを「動いていない」と読まない
    rs.useVelocity = (in.gbVelocity != nullptr && rsHistValid) ? 1 : 0;
    rs.classOverride = view.rtReflRestirParams.classOverride;
    rs.depthThreshold = kRtTemporalDepthThreshold;
    rs.normalThreshold = kRtTemporalNormalThreshold;
    rs.jacobianMax = kRtRestirJacobianMax;
    // M67f: 半径を受け側の α で縮める基準。チューニング UI が実行中に触るので
    // 定数ではなく params から取る (既定は kRtRestirRadiusAlphaRef)。
    // 0 以下を入れられると HLSL 側が「タップ 0」に倒れるだけなので下限だけ締める
    rs.radiusAlphaRef = std::clamp(view.rtReflRestirParams.radiusAlphaRef, 0.01f, 1.0f);
    for (int i = 0; i < kRtReflClassCount; ++i) {
        rs.classTable[i] = view.rtReflRestirParams.classTable[i];
    }
    UploadCB(dc, restirCB_.Get(), rs);
    ID3D11Buffer* rsCbs[1] = { restirCB_.Get() };
    dc->CSSetConstantBuffers(3, 1, rsCbs);

    // t7-t9 は GI/影と同じ並び、t10 に metallic/roughness を足す
    ID3D11ShaderResourceView* gbuf[4] = { in.gbNormal, in.gbPosition, in.gbAlbedo,
                                          in.gbMaterial };
    dc->CSSetShaderResources(7, 4, gbuf);
    // ★off のときは reservoir を 1 枚も張らない (t11-t15 / u1-u5 は UnbindCompute が
    //   前のパスで null にしてある) = 現行と同じ「UAV 1 本だけ」のバインド
    if (restirOn) {
        // M67f: ping-pong。前フレームに書いた面 (1-write) を t11-t15 で読み、
        // 今フレームの面 (write) へ書く。読む面と書く面は常に別テクスチャ
        RtReservoirSet& a = slot.set[1 - slot.write];
        RtReservoirSet& b = slot.set[slot.write];
        ID3D11ShaderResourceView* prev[5] = { a.pos.SRV(), a.rad.SRV(), a.nrm.SRV(),
                                              a.geom.SRV(), a.rpos.SRV() };
        dc->CSSetShaderResources(11, 5, prev);
        // M67e: 画面速度 (t16)。**null もそのまま張る** — rs.useVelocity が同じ条件
        // (gbVelocity && histValid) で 0 になるので、シェーダは読まない。
        // 「張らない」にすると前のパスの残りが t16 に居座りうる (UnbindCompute が
        // 外すのでこの経路では起きないが、依存を持たせない方が安い)
        ID3D11ShaderResourceView* vel[1] = { in.gbVelocity };
        dc->CSSetShaderResources(16, 1, vel);
        ID3D11UnorderedAccessView* uavs[6] = { reflRt_.UAV(),  b.pos.UAV(),  b.rad.UAV(),
                                               b.nrm.UAV(),    b.geom.UAV(), b.rpos.UAV() };
        dc->CSSetUnorderedAccessViews(0, 6, uavs, nullptr);
    } else {
        ID3D11UnorderedAccessView* uavs[1] = { reflRt_.UAV() };
        dc->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    }
    dc->CSSetShader(cs->cs.Get(), nullptr, 0);
    dc->Dispatch(static_cast<UINT>((gw + 7) / 8), static_cast<UINT>((gh + 7) / 8), 1);

    UnbindCompute(device);
    reflTimer_.End(device);

    result.raw = reflRt_.SRV();
    result.filtered = result.raw;
    // M67d: 2 パス目 (空間再利用 + resolve)。ここから先の入力は reflRestirRt_ に変わる
    const bool restirRan =
        restirOn && RenderRestirSpatial(device, shaders, view, in, slot, gw, gh);
    ID3D11ShaderResourceView* denoiseSrc = reflRt_.SRV();
    if (restirRan) {
        denoiseSrc = reflRestirRt_.SRV();
        result.filtered = denoiseSrc;
        // デバッグ 12 / 14 が読むのは**今フレームの面** = rt_refl が書いた reservoir。
        // spatial は書き戻さないので、ここ以外に「今フレームの reservoir」は無い
        result.reservoirM = slot.set[slot.write].rad.SRV();
        result.reservoirCls = slot.set[slot.write].nrm.SRV();
        slot.lastSerial = view.rtViewSerial;
        slot.hasLast = true;
        slot.write = 1 - slot.write; // 次フレームは今書いた面を読む (RtHistory と同じ)
    } else if (restirOn) {
        // spatial を走らせられなかった = この絵は reservoir を経由していないので、
        // 次フレームが「連続している」と思い込まないよう履歴を捨てる (flip もしない)
        slot.hasLast = false;
    }
    if (view.rtTemporal != 0) {
        // ReSTIR 後段の SVGF は既定値が M46h と同値なので、on にしただけでは何も変わらない。
        // スライダ (M67f) が壊れた値を入れても内部で潰れないよう範囲はここで締める
        const float maxHistory = restirRan
            ? std::clamp(view.rtReflRestirParams.svgfHistory, 1.0f,
                         static_cast<float>(kRtTemporalMaxHistory))
            : kRtReflMaxHistory;
        const int atrous = restirRan
            ? std::clamp(view.rtReflRestirParams.atrousIterations, 0, 4)
            : kRtReflAtrousIterations;
        const AccumResult acc =
            Accumulate(device, shaders, view, in, gw, gh, denoiseSrc,
                       reflHist_[HistorySlot(view.rtViewKey, kHistorySlots)], maxHistory,
                       reflTemporalTimer_);
        if (acc.color != nullptr) {
            result.filtered = acc.color;
            if (view.rtSvgf != 0) {
                ID3D11ShaderResourceView* f =
                    Denoise(device, shaders, view, acc, gw, gh, reflSvgfRt_, atrous,
                            kRtReflSigmaLuma, reflSvgfTimer_);
                if (f != nullptr) {
                    result.filtered = f;
                }
            }
        }
    } else {
        reflHist_[HistorySlot(view.rtViewKey, kHistorySlots)].hasLast = false;
    }
    return result;
}

// M67d: reservoir 5 枚 × 2 組を確保する。**ReSTIR を一度も使わないなら呼ばれない** =
// 既定の描画では 1 バイトも増えない。内部解像度が変わったら履歴は捨てる
// (RtHistory::Resize と同型 — 落とし忘れると SceneView と GameView が混線する)
bool RtPasses::EnsureReservoirs(GraphicsDevice& device, RtReservoirSlot& slot, int gw, int gh)
{
    if (slot.w != gw || slot.h != gh) {
        for (RtReservoirSet& set : slot.set) {
            set.pos.Resize(device, gw, gh, DXGI_FORMAT_R32G32B32A32_FLOAT, /*withDepth=*/false,
                           /*withUav=*/true);
            set.rad.Resize(device, gw, gh, DXGI_FORMAT_R16G16B16A16_FLOAT, /*withDepth=*/false,
                           /*withUav=*/true);
            set.nrm.Resize(device, gw, gh, DXGI_FORMAT_R16G16B16A16_FLOAT, /*withDepth=*/false,
                           /*withUav=*/true);
            set.geom.Resize(device, gw, gh, DXGI_FORMAT_R16G16B16A16_FLOAT, /*withDepth=*/false,
                            /*withUav=*/true);
            set.rpos.Resize(device, gw, gh, DXGI_FORMAT_R32G32B32A32_FLOAT, /*withDepth=*/false,
                            /*withUav=*/true);
        }
        slot.w = gw;
        slot.h = gh;
        slot.write = 0;
        slot.hasLast = false; // リサイズで履歴は捨てる
    }
    for (RtReservoirSet& set : slot.set) {
        const RenderTexture* rts[5] = { &set.pos, &set.rad, &set.nrm, &set.geom, &set.rpos };
        for (const RenderTexture* rt : rts) {
            if (!rt->UAV() || !rt->SRV()) {
                return false;
            }
        }
    }
    return true;
}

// M67d: ReSTIR の 2 パス目。今フレームの reservoir (t11-t15) を読んで、解決した
// 反射放射輝度を reflRestirRt_ (u0) へ出す。
// **M67f: reservoir は書き戻さない** (u1-u5 を張らない) — 履歴は rt_refl の出力だけ。
// 読む面は UAV に張っていないので、同じフレーム内で SRV/UAV の衝突が起きない
bool RtPasses::RenderRestirSpatial(GraphicsDevice& device, ShaderManager& shaders,
                                   const RenderView& view, const RtFrameInputs& in,
                                   RtReservoirSlot& slot, int gw, int gh)
{
    ShaderProgram* cs = Program(shaders, restirCS_, "rt_refl_restir_spatial.cs");
    if (!cs || !cs->valid || !cs->cs) {
        return false;
    }
    reflRestirRt_.Resize(device, gw, gh, DXGI_FORMAT_R16G16B16A16_FLOAT, /*withDepth=*/false,
                         /*withUav=*/true);
    if (!reflRestirRt_.UAV() || !reflRestirRt_.SRV()) {
        return false;
    }

    ID3D11DeviceContext* dc = device.Context();
    restirTimer_.Begin(device);
    BindCommon(device, view, in); // 可視レイ (M67f) が BVH を引くので同じ土台を張る
    ID3D11Buffer* rsCbs[1] = { restirCB_.Get() }; // b3 は反射パスで上げたものをそのまま使う
    dc->CSSetConstantBuffers(3, 1, rsCbs);

    ID3D11ShaderResourceView* gbuf[4] = { in.gbNormal, in.gbPosition, in.gbAlbedo,
                                          in.gbMaterial };
    dc->CSSetShaderResources(7, 4, gbuf);
    RtReservoirSet& b = slot.set[slot.write]; // rt_refl がこのフレームに書いた面
    ID3D11ShaderResourceView* src[5] = { b.pos.SRV(), b.rad.SRV(), b.nrm.SRV(), b.geom.SRV(),
                                         b.rpos.SRV() };
    dc->CSSetShaderResources(11, 5, src);
    ID3D11UnorderedAccessView* uavs[1] = { reflRestirRt_.UAV() };
    dc->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    dc->CSSetShader(cs->cs.Get(), nullptr, 0);
    dc->Dispatch(static_cast<UINT>((gw + 7) / 8), static_cast<UINT>((gh + 7) / 8), 1);

    UnbindCompute(device);
    restirTimer_.End(device);
    return true;
}

bool RtPasses::Blit(GraphicsDevice& device, ShaderManager& shaders, const RenderView& view,
                    ID3D11ShaderResourceView* src, int mode, float param)
{
    if (blitShader_.IsNull()) {
        blitShader_ = shaders.Load("rt_blit"); // VS+PS なので Program() ではなくこちら
    }
    ShaderProgram* blit = shaders.Get(blitShader_);
    if (!blit || !blit->valid || src == nullptr) {
        return false;
    }
    ID3D11DeviceContext* dc = device.Context();

    RtBlitCB bc = {};
    bc.dstSize[0] = static_cast<float>(view.width);
    bc.dstSize[1] = static_cast<float>(view.height);
    bc.mode = mode;
    bc.param = (param > 0.0f) ? param : static_cast<float>(kRtTemporalMaxHistory);
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
    // s0 = 線形 (従来のモード)、s1 = 点 (M67d のクラス表示。整数を補間させない)
    ID3D11SamplerState* samps[2] = { linearClamp_.Get(), pointClamp_.Get() };
    dc->PSSetSamplers(0, 2, samps);
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
                           const RtFrameInputs& in, const RtGiResult& gi,
                           ID3D11ShaderResourceView* shadow, const RtReflResult& refl)
{
    if (!inited_ || view.rtDebugMode == rtdebug::kOff || !in.scene || !in.scene->IsValid()
        || view.rtv == nullptr || view.width <= 0 || view.height <= 0) {
        return false;
    }

    // GI / 影 / 反射 / reservoir の表示は、1.7 で撃ったパスの結果バッファをそのまま拡大表示する
    // (CS は各パスで実行済み)。一次レイを撃つ表示 (kBvhHeat / kHitNormal / kInstanceId /
    // kPrimaryClass) だけが default を抜けて下の rt_debug.cs へ進む
    switch (view.rtDebugMode) {
    case rtdebug::kGiRaw:
        return Blit(device, shaders, view, gi.raw);
    case rtdebug::kGiAccumulated:
        return Blit(device, shaders, view, gi.accumulated);
    case rtdebug::kGiHistory: // 履歴長 (a) のヒートマップ
        return Blit(device, shaders, view, gi.accumulated, /*mode=*/1);
    case rtdebug::kGiSvgf:
        return Blit(device, shaders, view, gi.filtered);
    case rtdebug::kGiVariance: // 推定分散 (a) のヒートマップ。緑 = 収束
        // 標準偏差 0.25 で赤に振り切る (GI の輝度スケールに合わせた表示用の定数)
        return Blit(device, shaders, view, gi.filtered, /*mode=*/2, /*param=*/4.0f);
    case rtdebug::kShadowVisibility: // 白 = 照らされる / 黒 = 影
        return Blit(device, shaders, view, shadow, /*mode=*/3);
    case rtdebug::kReflRaw: // roughness 超過は黒
        return Blit(device, shaders, view, refl.raw);
    case rtdebug::kReflDenoised:
        return Blit(device, shaders, view, refl.filtered);
    case rtdebug::kReservoirM: // 赤 = 1 本 → 緑 = 上限まで再利用
        return Blit(device, shaders, view, refl.reservoirM, /*mode=*/1,
                    /*param=*/kRtRestirMaxM);
    case rtdebug::kReflClass: // 反射像側の ReflectionClass (空 = 黒 / スカイ = 黒)
        return Blit(device, shaders, view, refl.reservoirCls, /*mode=*/4);
    default:
        break;
    }

    ShaderProgram* cs = Program(shaders, debugCS_, "rt_debug.cs");
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
    // M55b: RT の一次光線はラスタライズ結果を読まない独立描画なので非ジッタ側で撃つ。
    // テンポラル蓄積 (Accumulate) が使う prevViewProj も非ジッタなので出所が揃う
    const XMMATRIX vp = XMLoadFloat4x4(&view.view) * XMLoadFloat4x4(&view.projNoJitter);
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
