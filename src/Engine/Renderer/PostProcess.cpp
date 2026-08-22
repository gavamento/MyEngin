#include "Engine/Renderer/PostProcess.h"

#include <algorithm>
#include <utility>

#include "Engine/Core/Components.h"
#include "Engine/Core/Log.h"
#include "Engine/Renderer/GpuBufferUtil.h" // M46a: バッファ生成ヘルパ (共通化)
#include "Engine/Renderer/GraphicsDevice.h"
#include "Engine/Renderer/PostFxMath.h"
#include "Engine/Renderer/RenderTypes.h"
#include "Engine/Renderer/ShaderManager.h"

namespace mye {

PostProcess::Settings MergeCameraPostFx(const PostProcess::Settings& base,
                                        const CameraPostFxComponent& comp)
{
    PostProcess::Settings s = base; // applyGamma は base を維持
    s.exposure = (comp.exposure >= 0.0f) ? comp.exposure : 0.0f;
    s.tonemap = (comp.tonemapMode >= 0 && comp.tonemapMode <= 2) ? comp.tonemapMode : 1;
    s.bloom = comp.bloomOn != 0;
    s.bloomThreshold = comp.bloomThreshold;
    s.chromAberration = comp.chromAberration; // M32d
    s.vignetteIntensity = comp.vignetteIntensity;
    s.vignetteRadius = comp.vignetteRadius;
    s.saturation = comp.saturation;
    s.contrast = comp.contrast;
    s.colorFilter = comp.colorFilter;
    s.bloomIntensity = comp.bloomIntensity;
    s.fxaa = comp.fxaaOn != 0;
    s.godrayIntensity = comp.godrayIntensity; // M43b
    s.godrayDecay = comp.godrayDecay;
    s.lutTexture = comp.lutTexture; // M44a (lutSRV の解決は RenderSystem)
    s.lutIntensity = comp.lutIntensity;
    s.autoExposure = comp.autoExposure; // M44b (aeInstant は base 維持 — applyGamma と同じ)
    s.aeSpeed = comp.aeSpeed;
    s.aeMin = comp.aeMin;
    s.aeMax = comp.aeMax;
    s.dofFocusDistance = comp.dofFocusDistance; // M44c
    s.dofFocusRange = comp.dofFocusRange;
    s.dofMaxRadius = comp.dofMaxRadius;
    s.motionBlurIntensity = comp.motionBlurIntensity; // M44d
    s.mbMaxPixels = comp.mbMaxPixels;
    s.taaOn = comp.taaOn; // M55d
    s.taaFeedback = comp.taaFeedback;
    return s;
}
namespace {

// postfx_tonemap.hlsl の PostFx cbuffer と一致 (80 バイト)
struct PostFxCB {
    float exposure;
    int32_t tonemap;
    float bloomIntensity;
    int32_t applyGamma;
    float chromAberration; // M32d
    float vignetteIntensity;
    float vignetteRadius;
    float saturation;
    float contrast;
    int32_t distortEnabled; // M42d: 旧 pad[0] 転用。1 で t2 の歪みバッファを UV に加算
    int32_t godrayEnabled;  // M43b: 旧 pad[1] 転用。1 で t3 のゴッドレイを加算
    float pad;
    DirectX::XMFLOAT4 colorFilter;
    // ---- M44a: LUT (末尾 append) / M44b: 自動露出 (旧 lutPad[0] 転用) ----
    float lutIntensity;    // 0 = 無効 (t4 不参照)
    int32_t autoExposure;  // 1 = t5 の露出倍率を gExposure に乗算
    float lutPad[2];
};

// postfx_bright.hlsl の Bright cbuffer (16 バイト)
struct BrightCB {
    float threshold;
    float pad0, pad1, pad2;
};

// postfx_blur.hlsl の Blur cbuffer (16 バイト)
struct BlurCB {
    float texelX, texelY;
    float pad0, pad1;
};

// postfx_fxaa.hlsl の Fxaa cbuffer (16 バイト)
struct FxaaCB {
    float invW, invH;
    float pad0, pad1;
};

// postfx_godray_mask.hlsl の GodrayMask cbuffer (32 バイト)
struct GodrayMaskCB {
    float screenW, screenH; // フル解像度深度の Load 用
    float pad0, pad1;
    DirectX::XMFLOAT3 sunColorFade; // sunColor (リニア・強度込み) × intensity × 画面端フェード
    float pad2;
};

// postfx_godray_blur.hlsl の GodrayBlur cbuffer (16 バイト)
struct GodrayBlurCB {
    float sunU, sunV; // 太陽のスクリーン UV
    float decay;      // タップ毎減衰
    float density;    // 16 タップで太陽までの距離の何割を進むか
};

// postfx_hist.cs.hlsl の HistCB (16 バイト)
struct HistCB {
    float sizeW, sizeH;
    float pad0, pad1;
};

// postfx_hist_reduce.cs.hlsl の AeReduceCB (16 バイト)
struct AeReduceCB {
    float aeSpeed;
    float aeMin;
    float aeMax;
    int32_t aeInstant;
};

// postfx_dof_prefilter/gather/composite.hlsl の DofCB (32 バイト、3 パス共通。
// texel はパス毎の出力ターゲット解像度で詰め直す)
struct DofCB {
    float focusDist;
    float focusRange;
    float nearZ;
    float farZ;
    float maxRadius;
    float texelX;
    float texelY;
    float pad;
};

// postfx_motionblur.hlsl の MotionBlurCB (160 バイト)。
// M55e: useVelocity を末尾 append。CreateConstant は size を丸めないので 16 バイト
// 倍数になるよう pad を明示する (144 + 4 のままだと CreateBuffer が落ちる)
struct MotionBlurCB {
    DirectX::XMFLOAT4X4 invViewProj;  // transpose(inverse(view*proj))
    DirectX::XMFLOAT4X4 prevViewProj; // transpose(前フレームの view*proj)
    float intensity;
    float maxPixels;
    float screenW, screenH;
    int32_t useVelocity; // M55e: 1 = GBuffer RT4 (画面速度) を速度源に使う
    float pad[3];
};

// M46a: 構造化バッファ / 定数バッファ / CB 更新は GpuBufferUtil.h へ集約 (定義は同一)
using namespace gpubuf;

} // namespace

bool PostProcess::Init(GraphicsDevice& device, ShaderManager& shaders)
{
    device_ = &device;
    ID3D11Device* dev = device.Device();

    tonemapShader_ = shaders.Load("postfx_tonemap");
    brightShader_ = shaders.Load("postfx_bright");
    blurShader_ = shaders.Load("postfx_blur");
    fxaaShader_ = shaders.Load("postfx_fxaa");
    godrayMaskShader_ = shaders.Load("postfx_godray_mask"); // M43b
    godrayBlurShader_ = shaders.Load("postfx_godray_blur");
    histCS_ = shaders.LoadCompute("postfx_hist.cs"); // M44b
    histReduceCS_ = shaders.LoadCompute("postfx_hist_reduce.cs");
    dofPrefilterShader_ = shaders.Load("postfx_dof_prefilter"); // M44c
    dofGatherShader_ = shaders.Load("postfx_dof_gather");
    dofCompositeShader_ = shaders.Load("postfx_dof_composite");
    motionBlurShader_ = shaders.Load("postfx_motionblur"); // M44d

    if (!CreateConstant(dev, sizeof(PostFxCB), cb_) || !CreateConstant(dev, sizeof(BrightCB), brightCB_)
        || !CreateConstant(dev, sizeof(BlurCB), blurCB_) || !CreateConstant(dev, sizeof(FxaaCB), fxaaCB_)
        || !CreateConstant(dev, sizeof(GodrayMaskCB), godrayMaskCB_)
        || !CreateConstant(dev, sizeof(GodrayBlurCB), godrayBlurCB_)
        || !CreateConstant(dev, sizeof(HistCB), histCB_)
        || !CreateConstant(dev, sizeof(AeReduceCB), aeReduceCB_)
        || !CreateConstant(dev, sizeof(DofCB), dofCB_)
        || !CreateConstant(dev, sizeof(MotionBlurCB), mbCB_)) {
        MYE_LOG_ERROR("PostProcess: CB creation failed");
        return false;
    }
    resolveTimer_.Init(device); // M44d: 失敗しても致命ではない (計測 0 のまま)
    // M55d: TAA。失敗しても致命ではない (Run が nullptr を返し従来チェーンのまま動く)
    taa_.Init(device, shaders);

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateSamplerState(&sd, linearClamp_.GetAddressOf()))) {
        return false;
    }

    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = FALSE;
    if (FAILED(dev->CreateDepthStencilState(&dd, depthDisabled_.GetAddressOf()))) {
        return false;
    }

    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable = FALSE;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(dev->CreateBlendState(&bd, blendOff_.GetAddressOf()))) {
        return false;
    }

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(dev->CreateRasterizerState(&rd, rasterizer_.GetAddressOf()))) {
        return false;
    }

    ready_ = true;
    return true;
}

PostProcess::Target* PostProcess::Acquire(GraphicsDevice& device, int width, int height)
{
    if (width <= 0 || height <= 0) {
        return nullptr;
    }
    const uint64_t key =
        (static_cast<uint64_t>(static_cast<uint32_t>(width)) << 32) | static_cast<uint32_t>(height);
    for (size_t i = 0; i < cache_.size(); ++i) {
        if (cache_[i].key == key) {
            if (i != 0) { // move-to-front (簡易 LRU)
                Target tmp = std::move(cache_[i]);
                cache_.erase(cache_.begin() + i);
                cache_.insert(cache_.begin(), std::move(tmp));
            }
            return &cache_[0];
        }
    }
    Target t;
    t.key = key;
    const int bw = std::max(1, width / 2);
    const int bh = std::max(1, height / 2);
    if (!t.scene.Create(device, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, false)
        || !t.bloomA.Create(device, bw, bh, DXGI_FORMAT_R16G16B16A16_FLOAT, false)
        || !t.bloomB.Create(device, bw, bh, DXGI_FORMAT_R16G16B16A16_FLOAT, false)
        || !t.ldr.Create(device, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, false)
        || !t.distort.Create(device, width, height, DXGI_FORMAT_R16G16_FLOAT, false) // M42d
        || !t.godA.Create(device, bw, bh, DXGI_FORMAT_R16G16B16A16_FLOAT, false)     // M43b
        || !t.godB.Create(device, bw, bh, DXGI_FORMAT_R16G16B16A16_FLOAT, false)
        || !t.sceneB.Create(device, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, false) // M44c
        || !t.dofA.Create(device, bw, bh, DXGI_FORMAT_R16G16B16A16_FLOAT, false)
        || !t.dofB.Create(device, bw, bh, DXGI_FORMAT_R16G16B16A16_FLOAT, false)) {
        return nullptr;
    }
    // M44b: 自動露出バッファ (ヒストグラム 256 bin + 露出倍率 1 要素、初期値 1.0)
    const float kInitialExposure = 1.0f;
    if (!CreateStructured(device.Device(), sizeof(uint32_t), 256, nullptr, 0, t.histBuf,
                          &t.histUAV, &t.histSRV)
        || !CreateStructured(device.Device(), sizeof(float), 1, &kInitialExposure, 0,
                             t.exposureBuf, &t.exposureUAV, &t.exposureSRV)) {
        return nullptr;
    }
    cache_.insert(cache_.begin(), std::move(t));
    constexpr size_t kMaxEntries = 4; // SceneView/GameView/backbuffer 想定の上限
    if (cache_.size() > kMaxEntries) {
        cache_.resize(kMaxEntries); // 末尾 (最古) を破棄
    }
    return &cache_[0];
}

// M44c: DoF。プリフィルタ (scene+depth → dofA 半解像度、α=符号付き CoC) →
// 円盤ギャザー (dofA → dofB) → 合成 (scene+dofB+depth → sceneB フル解像度)。
// 実行後は bloom/トーンマップが sceneB を読む (ボケた高輝度が自然にブルームする順序)
bool PostProcess::RunDof(GraphicsDevice& device, ShaderManager& shaders, Target& t,
                         const Settings& s, const RenderView& view,
                         ID3D11ShaderResourceView* sceneSRV)
{
    if (s.dofMaxRadius <= 0.0f || view.depthSRV == nullptr) {
        return false;
    }
    ShaderProgram* pre = shaders.Get(dofPrefilterShader_);
    ShaderProgram* gather = shaders.Get(dofGatherShader_);
    ShaderProgram* comp = shaders.Get(dofCompositeShader_);
    if (!pre || !pre->valid || !gather || !gather->valid || !comp || !comp->valid) {
        return false;
    }
    ID3D11DeviceContext* dc = device.Context();
    const int hw = t.dofA.Width();
    const int hh = t.dofA.Height();
    const int fw = t.scene.Width();
    const int fh = t.scene.Height();

    // 共通ステート (RunBloom と同じフルスクリーン規約)。t.scene を SRV で読むため RTV を外す
    dc->OMSetRenderTargets(0, nullptr, nullptr);
    dc->IASetInputLayout(nullptr);
    dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dc->OMSetDepthStencilState(depthDisabled_.Get(), 0);
    dc->OMSetBlendState(blendOff_.Get(), nullptr, 0xFFFFFFFFu);
    dc->RSSetState(rasterizer_.Get());
    ID3D11SamplerState* samp[1] = { linearClamp_.Get() };
    dc->PSSetSamplers(0, 1, samp);
    ID3D11ShaderResourceView* nullSrv[3] = { nullptr, nullptr, nullptr };

    DofCB cb = {};
    cb.focusDist = s.dofFocusDistance;
    cb.focusRange = s.dofFocusRange;
    cb.nearZ = view.nearZ;
    cb.farZ = view.farZ;
    cb.maxRadius = s.dofMaxRadius;
    ID3D11Buffer* cbs[1] = { dofCB_.Get() };
    dc->PSSetConstantBuffers(0, 1, cbs);
    D3D11_VIEWPORT vp = {};
    vp.MaxDepth = 1.0f;

    // 1) プリフィルタ: scene(t0)+depth(t1) → dofA (半解像度)
    {
        cb.texelX = 1.0f / static_cast<float>(hw);
        cb.texelY = 1.0f / static_cast<float>(hh);
        UploadCB(dc, dofCB_.Get(), cb);
        vp.Width = static_cast<float>(hw);
        vp.Height = static_cast<float>(hh);
        dc->RSSetViewports(1, &vp);
        ID3D11RenderTargetView* rtv = t.dofA.RTV();
        dc->OMSetRenderTargets(1, &rtv, nullptr);
        ID3D11ShaderResourceView* srvs[2] = { sceneSRV, view.depthSRV }; // M55d: TAA 後なら履歴面
        dc->PSSetShaderResources(0, 2, srvs);
        dc->VSSetShader(pre->vs.Get(), nullptr, 0);
        dc->PSSetShader(pre->ps.Get(), nullptr, 0);
        dc->Draw(3, 0);
        dc->PSSetShaderResources(0, 2, nullSrv);
    }

    // 2) 円盤ギャザー: dofA → dofB (半解像度)
    {
        UploadCB(dc, dofCB_.Get(), cb); // texel は半解像度のまま
        ID3D11RenderTargetView* rtv = t.dofB.RTV();
        dc->OMSetRenderTargets(1, &rtv, nullptr);
        ID3D11ShaderResourceView* srvs[1] = { t.dofA.SRV() };
        dc->PSSetShaderResources(0, 1, srvs);
        dc->VSSetShader(gather->vs.Get(), nullptr, 0);
        dc->PSSetShader(gather->ps.Get(), nullptr, 0);
        dc->Draw(3, 0);
        dc->PSSetShaderResources(0, 1, nullSrv);
    }

    // 3) 合成: scene(t0)+dofB(t1)+depth(t2) → sceneB (フル解像度)
    {
        cb.texelX = 1.0f / static_cast<float>(fw);
        cb.texelY = 1.0f / static_cast<float>(fh);
        UploadCB(dc, dofCB_.Get(), cb);
        vp.Width = static_cast<float>(fw);
        vp.Height = static_cast<float>(fh);
        dc->RSSetViewports(1, &vp);
        ID3D11RenderTargetView* rtv = t.sceneB.RTV();
        dc->OMSetRenderTargets(1, &rtv, nullptr);
        ID3D11ShaderResourceView* srvs[3] = { sceneSRV, t.dofB.SRV(), view.depthSRV };
        dc->PSSetShaderResources(0, 3, srvs);
        dc->VSSetShader(comp->vs.Get(), nullptr, 0);
        dc->PSSetShader(comp->ps.Get(), nullptr, 0);
        dc->Draw(3, 0);
        dc->PSSetShaderResources(0, 3, nullSrv); // 深度 SRV は即解除 (次フレーム DSV bind 対策)
    }
    return true;
}

// M44d/M55e: モーションブラー。速度ベクトルの向きへ 8 タップ平均。
// 速度源は画素ごとに 2 通り — ジオメトリ画素は GBuffer RT4 の画面速度 (カメラ +
// **オブジェクト**が合成済み)、背景と Forward パスは従来の深度再投影。
// 行列の逆行列/転置は CPU 側で用意する。選択規則の CPU ミラーは
// PostFxMath.h::motionblur::BlurVector (RenderSelfTest が検証)
bool PostProcess::RunMotionBlur(GraphicsDevice& device, ShaderManager& shaders,
                                const Settings& s, const RenderView& view,
                                ID3D11ShaderResourceView* inputSRV, RenderTexture& dst)
{
    if (s.motionBlurIntensity <= 0.0f || view.depthSRV == nullptr
        || view.prevViewProjValid == 0) {
        return false;
    }
    ShaderProgram* prog = shaders.Get(motionBlurShader_);
    if (!prog || !prog->valid) {
        return false;
    }
    using namespace DirectX;
    ID3D11DeviceContext* dc = device.Context();

    MotionBlurCB cb = {};
    // M55b: 前フレーム側 (prevViewProj) が非ジッタで保存されているので、今フレーム側も
    // 非ジッタで揃える。片方だけジッタが載ると静止画でもサブピクセル速度が出続ける
    const XMMATRIX vpMat = XMLoadFloat4x4(&view.view) * XMLoadFloat4x4(&view.projNoJitter);
    XMVECTOR det;
    const XMMATRIX inv = XMMatrixInverse(&det, vpMat);
    XMStoreFloat4x4(&cb.invViewProj, XMMatrixTranspose(inv));
    XMStoreFloat4x4(&cb.prevViewProj,
                    XMMatrixTranspose(XMLoadFloat4x4(&view.prevViewProj)));
    cb.intensity = std::clamp(s.motionBlurIntensity, 0.0f, 1.0f);
    cb.maxPixels = s.mbMaxPixels;
    cb.screenW = static_cast<float>(dst.Width());
    cb.screenH = static_cast<float>(dst.Height());
    // M55e: 画面速度は GBuffer と同じ画素格子でしか Load できないので、解像度が
    // 一致するときだけ使う (不一致・Forward・AssetPreview は深度再投影へ縮退)
    const bool useVelocity = view.velocitySRV != nullptr && dst.Width() == view.width
        && dst.Height() == view.height;
    cb.useVelocity = useVelocity ? 1 : 0;
    UploadCB(dc, mbCB_.Get(), cb);

    // フルスクリーンパス (RunBloom と同じ規約)。入力 SRV を読むため RTV を先に外す
    dc->OMSetRenderTargets(0, nullptr, nullptr);
    dc->IASetInputLayout(nullptr);
    dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dc->OMSetDepthStencilState(depthDisabled_.Get(), 0);
    dc->OMSetBlendState(blendOff_.Get(), nullptr, 0xFFFFFFFFu);
    dc->RSSetState(rasterizer_.Get());
    ID3D11SamplerState* samp[1] = { linearClamp_.Get() };
    dc->PSSetSamplers(0, 1, samp);
    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(dst.Width());
    vp.Height = static_cast<float>(dst.Height());
    vp.MaxDepth = 1.0f;
    dc->RSSetViewports(1, &vp);
    ID3D11Buffer* cbs[1] = { mbCB_.Get() };
    dc->PSSetConstantBuffers(0, 1, cbs);
    ID3D11RenderTargetView* rtv = dst.RTV();
    dc->OMSetRenderTargets(1, &rtv, nullptr);
    // t2 = 画面速度 (M55e)。useVelocity=0 のときは null のまま = シェーダも参照しない
    ID3D11ShaderResourceView* srvs[3] = { inputSRV, view.depthSRV,
                                          useVelocity ? view.velocitySRV : nullptr };
    dc->PSSetShaderResources(0, 3, srvs);
    dc->VSSetShader(prog->vs.Get(), nullptr, 0);
    dc->PSSetShader(prog->ps.Get(), nullptr, 0);
    dc->Draw(3, 0);
    ID3D11ShaderResourceView* nullSrv[3] = { nullptr, nullptr, nullptr };
    dc->PSSetShaderResources(0, 3, nullSrv); // 深度 SRV は即解除 (次フレーム DSV bind 対策)
    return true;
}

void PostProcess::RunBloom(GraphicsDevice& device, ShaderManager& shaders, Target& t,
                           const Settings& s, ID3D11ShaderResourceView* sceneSRV)
{
    ShaderProgram* bright = shaders.Get(brightShader_);
    ShaderProgram* blur = shaders.Get(blurShader_);
    if (!bright || !bright->valid || !blur || !blur->valid) {
        return; // シェーダ未コンパイル — Resolve 側で bloom 無効にフォールバック
    }
    ID3D11DeviceContext* dc = device.Context();
    const int bw = t.bloomA.Width();
    const int bh = t.bloomA.Height();

    // 共通ステート (フルスクリーン、深度/ブレンド無し、半解像度ビューポート)
    dc->IASetInputLayout(nullptr);
    dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dc->OMSetDepthStencilState(depthDisabled_.Get(), 0);
    dc->OMSetBlendState(blendOff_.Get(), nullptr, 0xFFFFFFFFu);
    dc->RSSetState(rasterizer_.Get());
    ID3D11SamplerState* samp[1] = { linearClamp_.Get() };
    dc->PSSetSamplers(0, 1, samp);
    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(bw);
    vp.Height = static_cast<float>(bh);
    vp.MaxDepth = 1.0f;
    dc->RSSetViewports(1, &vp);
    ID3D11ShaderResourceView* nullSrv[1] = { nullptr };

    // 1) bright-pass: scene(full) → bloomA(half)。linear sample がダウンサンプルも兼ねる
    {
        BrightCB cb = {};
        cb.threshold = s.bloomThreshold;
        UploadCB(dc, brightCB_.Get(), cb);
        ID3D11Buffer* cbs[1] = { brightCB_.Get() };
        dc->PSSetConstantBuffers(0, 1, cbs);
        ID3D11RenderTargetView* rtv = t.bloomA.RTV();
        dc->OMSetRenderTargets(1, &rtv, nullptr);
        ID3D11ShaderResourceView* srv[1] = { sceneSRV }; // M44c: DoF 後は sceneB
        dc->PSSetShaderResources(0, 1, srv);
        dc->VSSetShader(bright->vs.Get(), nullptr, 0);
        dc->PSSetShader(bright->ps.Get(), nullptr, 0);
        dc->Draw(3, 0);
        dc->PSSetShaderResources(0, 1, nullSrv);
    }

    // 2) 分離ガウスブラー (H, V) を 2 回 (グロー幅を確保)
    dc->VSSetShader(blur->vs.Get(), nullptr, 0);
    dc->PSSetShader(blur->ps.Get(), nullptr, 0);
    for (int iter = 0; iter < 2; ++iter) {
        // H: bloomA → bloomB
        {
            BlurCB cb = {};
            cb.texelX = 1.0f / static_cast<float>(bw);
            UploadCB(dc, blurCB_.Get(), cb);
            ID3D11Buffer* cbs[1] = { blurCB_.Get() };
            dc->PSSetConstantBuffers(0, 1, cbs);
            ID3D11RenderTargetView* rtv = t.bloomB.RTV();
            dc->OMSetRenderTargets(1, &rtv, nullptr);
            ID3D11ShaderResourceView* srv[1] = { t.bloomA.SRV() };
            dc->PSSetShaderResources(0, 1, srv);
            dc->Draw(3, 0);
            dc->PSSetShaderResources(0, 1, nullSrv);
        }
        // V: bloomB → bloomA
        {
            BlurCB cb = {};
            cb.texelY = 1.0f / static_cast<float>(bh);
            UploadCB(dc, blurCB_.Get(), cb);
            ID3D11Buffer* cbs[1] = { blurCB_.Get() };
            dc->PSSetConstantBuffers(0, 1, cbs);
            ID3D11RenderTargetView* rtv = t.bloomA.RTV();
            dc->OMSetRenderTargets(1, &rtv, nullptr);
            ID3D11ShaderResourceView* srv[1] = { t.bloomB.SRV() };
            dc->PSSetShaderResources(0, 1, srv);
            dc->Draw(3, 0);
            dc->PSSetShaderResources(0, 1, nullSrv);
        }
    }
}

// M43b: スクリーンスペースゴッドレイ (放射ブラー方式)。
// 空ピクセル (depth>=0.9999) だけを太陽色で塗ったマスク (半解像度) を作り、
// 太陽のスクリーン位置へ向けて 16 タップ × 2 パスの放射ブラー。結果は t.godA。
// v1 制限: 遮蔽マスクは空のみ (発光体のレイ非対応)。太陽が背面/画面外は端フェードで消す
bool PostProcess::RunGodray(GraphicsDevice& device, ShaderManager& shaders, Target& t,
                            const Settings& s, const RenderView& view)
{
    if (s.godrayIntensity <= 0.0f || view.depthSRV == nullptr) {
        return false;
    }
    // 平行光が無い (sunColor 黒) なら描いても見えない — パスごとスキップ
    if (view.sunColor.x <= 0.0f && view.sunColor.y <= 0.0f && view.sunColor.z <= 0.0f) {
        return false;
    }
    ShaderProgram* mask = shaders.Get(godrayMaskShader_);
    ShaderProgram* blur = shaders.Get(godrayBlurShader_);
    if (!mask || !mask->valid || !blur || !blur->valid) {
        return false;
    }
    float sunU = 0.5f, sunV = 0.5f;
    // M55b: 光芒の中心は非ジッタ側で決める (揺らすと放射方向がフレーム毎に振れる)
    const float fade =
        ComputeSunScreenPos(view.view, view.projNoJitter, view.sunDirection, sunU, sunV);
    if (fade <= 0.0f) {
        return false; // 太陽が背面または画面から遠すぎる
    }

    ID3D11DeviceContext* dc = device.Context();
    const int gw = t.godA.Width();
    const int gh = t.godA.Height();

    // 共通ステート (RunBloom と同じフルスクリーン規約、半解像度ビューポート)
    dc->IASetInputLayout(nullptr);
    dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dc->OMSetDepthStencilState(depthDisabled_.Get(), 0);
    dc->OMSetBlendState(blendOff_.Get(), nullptr, 0xFFFFFFFFu);
    dc->RSSetState(rasterizer_.Get());
    ID3D11SamplerState* samp[1] = { linearClamp_.Get() };
    dc->PSSetSamplers(0, 1, samp);
    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(gw);
    vp.Height = static_cast<float>(gh);
    vp.MaxDepth = 1.0f;
    dc->RSSetViewports(1, &vp);
    ID3D11ShaderResourceView* nullSrv[1] = { nullptr };

    // 1) 空マスク: 深度 (フル解像度、t0) → godA。深度 SRV は Resolve 冒頭で DSV が
    //    外れている (OMSetRenderTargets が null DSV) ためハザード無し
    {
        GodrayMaskCB cb = {};
        cb.screenW = static_cast<float>(view.width);
        cb.screenH = static_cast<float>(view.height);
        const float k = s.godrayIntensity * fade;
        cb.sunColorFade = { view.sunColor.x * k, view.sunColor.y * k, view.sunColor.z * k };
        UploadCB(dc, godrayMaskCB_.Get(), cb);
        ID3D11Buffer* cbs[1] = { godrayMaskCB_.Get() };
        dc->PSSetConstantBuffers(0, 1, cbs);
        ID3D11RenderTargetView* rtv = t.godA.RTV();
        dc->OMSetRenderTargets(1, &rtv, nullptr);
        ID3D11ShaderResourceView* srv[1] = { view.depthSRV };
        dc->PSSetShaderResources(0, 1, srv);
        dc->VSSetShader(mask->vs.Get(), nullptr, 0);
        dc->PSSetShader(mask->ps.Get(), nullptr, 0);
        dc->Draw(3, 0);
        dc->PSSetShaderResources(0, 1, nullSrv); // 深度 SRV は即解除 (次フレームの DSV bind 対策)
    }

    // 2) 放射ブラー 2 パス (godA → godB → godA)。1 パス目は短く 2 パス目で伸ばす =
    //    16 タップ × 2 で実効 256 タップ相当の滑らかさ
    dc->VSSetShader(blur->vs.Get(), nullptr, 0);
    dc->PSSetShader(blur->ps.Get(), nullptr, 0);
    const float densities[2] = { 0.5f, 1.0f };
    RenderTexture* src = &t.godA;
    RenderTexture* dstRt = &t.godB;
    for (int pass = 0; pass < 2; ++pass) {
        GodrayBlurCB cb = {};
        cb.sunU = sunU;
        cb.sunV = sunV;
        cb.decay = s.godrayDecay;
        cb.density = densities[pass];
        UploadCB(dc, godrayBlurCB_.Get(), cb);
        ID3D11Buffer* cbs[1] = { godrayBlurCB_.Get() };
        dc->PSSetConstantBuffers(0, 1, cbs);
        ID3D11RenderTargetView* rtv = dstRt->RTV();
        dc->OMSetRenderTargets(1, &rtv, nullptr);
        ID3D11ShaderResourceView* srv[1] = { src->SRV() };
        dc->PSSetShaderResources(0, 1, srv);
        dc->Draw(3, 0);
        dc->PSSetShaderResources(0, 1, nullSrv);
        std::swap(src, dstRt);
    }
    return true; // 結果は t.godA (2 パスで A→B→A)
}

// M44b: 自動露出。HDR シーンの輝度ヒストグラム (log2 [-10,+6]、256 bin) → 加重平均 →
// 目標露出 0.18/avgLum を指数平滑で t.exposureBuf[0] に反映する。GPU 内完結・リードバック
// 無し = WorldHash 非干渉。aeInstant (決定的スクショ) は 1 フレーム収束
bool PostProcess::RunAutoExposure(GraphicsDevice& device, ShaderManager& shaders, Target& t,
                                  const Settings& s, ID3D11ShaderResourceView* sceneSRV)
{
    if (s.autoExposure == 0) {
        return false;
    }
    ShaderProgram* hist = shaders.Get(histCS_);
    ShaderProgram* reduce = shaders.Get(histReduceCS_);
    if (!hist || !hist->valid || !hist->cs || !reduce || !reduce->valid || !reduce->cs) {
        return false;
    }
    if (!t.histUAV || !t.exposureUAV || !t.exposureSRV) {
        return false;
    }
    ID3D11DeviceContext* dc = device.Context();
    // t.scene が RTV に残っている可能性がある (bloom/godray スキップ時) — CS の SRV 読みの前に外す
    dc->OMSetRenderTargets(0, nullptr, nullptr);
    const UINT clear[4] = { 0, 0, 0, 0 };
    dc->ClearUnorderedAccessViewUint(t.histUAV.Get(), clear);

    // 1) ヒストグラム収集: scene (フル解像度) → histBuf
    HistCB hcb = {};
    hcb.sizeW = static_cast<float>(t.scene.Width());
    hcb.sizeH = static_cast<float>(t.scene.Height());
    UploadCB(dc, histCB_.Get(), hcb);
    ID3D11Buffer* cbs[1] = { histCB_.Get() };
    dc->CSSetConstantBuffers(0, 1, cbs);
    ID3D11ShaderResourceView* srv[1] = { sceneSRV }; // M44c: DoF 後は sceneB
    dc->CSSetShaderResources(0, 1, srv);
    ID3D11UnorderedAccessView* uav[1] = { t.histUAV.Get() };
    dc->CSSetUnorderedAccessViews(0, 1, uav, nullptr);
    dc->CSSetShader(hist->cs.Get(), nullptr, 0);
    dc->Dispatch((t.scene.Width() + 15) / 16, (t.scene.Height() + 15) / 16, 1);

    // 2) 縮約: histBuf (SRV に持ち替え) → exposureBuf[0] 更新
    ID3D11UnorderedAccessView* nullUav[1] = { nullptr };
    dc->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
    AeReduceCB rcb = {};
    rcb.aeSpeed = s.aeSpeed;
    rcb.aeMin = s.aeMin;
    rcb.aeMax = s.aeMax;
    rcb.aeInstant = s.aeInstant ? 1 : 0;
    UploadCB(dc, aeReduceCB_.Get(), rcb);
    cbs[0] = aeReduceCB_.Get();
    dc->CSSetConstantBuffers(0, 1, cbs);
    srv[0] = t.histSRV.Get();
    dc->CSSetShaderResources(0, 1, srv);
    uav[0] = t.exposureUAV.Get();
    dc->CSSetUnorderedAccessViews(0, 1, uav, nullptr);
    dc->CSSetShader(reduce->cs.Get(), nullptr, 0);
    dc->Dispatch(1, 1, 1);

    // 後始末: exposureBuf は直後にトーンマップの t5 (PS SRV) になるため UAV を必ず外す
    dc->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
    ID3D11ShaderResourceView* nullSrv[1] = { nullptr };
    dc->CSSetShaderResources(0, 1, nullSrv);
    return true;
}

void PostProcess::Resolve(GraphicsDevice& device, ShaderManager& shaders, Target& t,
                          ID3D11RenderTargetView* dst, int width, int height, const Settings& s,
                          const RenderView& view, bool distortionActive)
{
    ID3D11DeviceContext* dc = device.Context();
    ShaderProgram* prog = shaders.Get(tonemapShader_);
    if (!prog || !prog->valid || dst == nullptr) {
        return; // 解決不能 (シェーダ未コンパイル等)。呼び出し側は既に HDR に描画済み
    }
    resolveTimer_.Begin(device); // M44d: Resolve 全体の GPU 時間 (ProfilerWindow "postfx" 行)

    // M55d: TAA はチェーンの **先頭** (DoF より前)。ボケや速度スミアを掛けた後の絵を
    // 積むと履歴が二重にぼやけるので、素の HDR に対して先に解決する。
    // 出力は TaaPass 内の履歴面 (viewKey 別) — 走らなければ nullptr で従来どおり t.scene。
    // ★t.scene へ書き戻さないのは、書き戻しには全画面コピーが 1 回要るのと、
    //   次フレームの読み元 (t.scene = 素の描画) を汚さないため
    ID3D11ShaderResourceView* sceneSRV = t.scene.SRV();
    if (s.taaOn != 0) {
        if (ID3D11ShaderResourceView* taaOut =
                taa_.Run(device, shaders, view, sceneSRV, s.taaFeedback)) {
            sceneSRV = taaOut;
        }
    }

    // M44c: DoF (scene → sceneB)。実行後は bloom/AE/トーンマップが sceneB を読む
    // (ボケた高輝度が自然にブルームする順序)。off/不成立時は従来どおり sceneSRV。
    // M44d: モーションブラーは DoF 出力と scene のピンポン (DoF off なら → sceneB)
    RenderTexture* mbDst = &t.sceneB;
    if (RunDof(device, shaders, t, s, view, sceneSRV)) {
        sceneSRV = t.sceneB.SRV();
        mbDst = &t.scene;
    }
    if (RunMotionBlur(device, shaders, s, view, sceneSRV, *mbDst)) {
        sceneSRV = mbDst->SRV();
    }

    ID3D11ShaderResourceView* bloomSRV = sceneSRV; // プレースホルダ (intensity 0 で不参照)
    float bloomIntensity = 0.0f;
    if (s.bloom) {
        RunBloom(device, shaders, t, s, sceneSRV);
        ShaderProgram* bright = shaders.Get(brightShader_);
        ShaderProgram* blur = shaders.Get(blurShader_);
        if (bright && bright->valid && blur && blur->valid) {
            bloomSRV = t.bloomA.SRV();
            bloomIntensity = s.bloomIntensity;
        }
    }

    // M43b: ゴッドレイ (結果 = t.godA)。off/不成立時は t3 に null = 従来とビット同一
    const bool godrayActive = RunGodray(device, shaders, t, s, view);

    // M44b: 自動露出 (結果 = t.exposureBuf[0])。off/不成立時は t5 に null = 従来とビット同一
    const bool aeActive = RunAutoExposure(device, shaders, t, s, sceneSRV);

    // FXAA 有効時はトーンマップを LDR 中間 (t.ldr) に描き、その後 FXAA で dst へ。
    ShaderProgram* fxaa = shaders.Get(fxaaShader_);
    const bool useFxaa = s.fxaa && fxaa && fxaa->valid && t.ldr.IsValid();
    ID3D11RenderTargetView* tonemapDst = useFxaa ? t.ldr.RTV() : dst;

    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(width);
    vp.Height = static_cast<float>(height);
    vp.MaxDepth = 1.0f;

    // ---- トーンマップ: scene(t0) + bloom(t1) → tonemapDst ----
    dc->OMSetRenderTargets(1, &tonemapDst, nullptr);
    dc->RSSetViewports(1, &vp);
    dc->RSSetState(rasterizer_.Get());

    PostFxCB cb = {};
    cb.exposure = s.exposure;
    cb.tonemap = s.tonemap;
    cb.bloomIntensity = bloomIntensity;
    cb.applyGamma = s.applyGamma ? 1 : 0;
    cb.chromAberration = s.chromAberration;
    cb.vignetteIntensity = s.vignetteIntensity;
    cb.vignetteRadius = s.vignetteRadius;
    cb.saturation = s.saturation;
    cb.contrast = s.contrast;
    cb.distortEnabled = (distortionActive && t.distort.IsValid()) ? 1 : 0; // M42d
    cb.godrayEnabled = godrayActive ? 1 : 0;                              // M43b
    cb.colorFilter = s.colorFilter;
    cb.lutIntensity = (s.lutSRV != nullptr) ? s.lutIntensity : 0.0f; // M44a: SRV 無しは強制 off
    cb.autoExposure = aeActive ? 1 : 0;                              // M44b
    UploadCB(dc, cb_.Get(), cb);
    ID3D11Buffer* cbs[1] = { cb_.Get() };
    dc->PSSetConstantBuffers(0, 1, cbs);

    // M42d: t2 = 歪みバッファ / M43b: t3 = ゴッドレイ / M44a: t4 = LUT / M44b: t5 = 露出
    // (無効時は null — enabled/intensity 0 なら不参照)。t0 は DoF 後なら sceneB (M44c)
    ID3D11ShaderResourceView* srvs[6] = { sceneSRV, bloomSRV,
                                          cb.distortEnabled ? t.distort.SRV() : nullptr,
                                          godrayActive ? t.godA.SRV() : nullptr,
                                          (cb.lutIntensity > 0.0f) ? s.lutSRV : nullptr,
                                          aeActive ? t.exposureSRV.Get() : nullptr };
    dc->PSSetShaderResources(0, 6, srvs);
    ID3D11SamplerState* samps[1] = { linearClamp_.Get() };
    dc->PSSetSamplers(0, 1, samps);

    dc->IASetInputLayout(nullptr);
    dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dc->VSSetShader(prog->vs.Get(), nullptr, 0);
    dc->PSSetShader(prog->ps.Get(), nullptr, 0);
    dc->OMSetDepthStencilState(depthDisabled_.Get(), 0);
    dc->OMSetBlendState(blendOff_.Get(), nullptr, 0xFFFFFFFFu);
    dc->Draw(3, 0);

    ID3D11ShaderResourceView* nulls[6] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
    dc->PSSetShaderResources(0, 6, nulls); // t.scene / t.distort / t.godA / 露出を次フレームのため解除

    // ---- FXAA: t.ldr → dst ----
    if (useFxaa) {
        FxaaCB fcb = {};
        fcb.invW = 1.0f / static_cast<float>(width);
        fcb.invH = 1.0f / static_cast<float>(height);
        UploadCB(dc, fxaaCB_.Get(), fcb);
        ID3D11Buffer* fcbs[1] = { fxaaCB_.Get() };
        dc->PSSetConstantBuffers(0, 1, fcbs);
        dc->OMSetRenderTargets(1, &dst, nullptr);
        dc->RSSetViewports(1, &vp);
        ID3D11ShaderResourceView* fsrv[1] = { t.ldr.SRV() };
        dc->PSSetShaderResources(0, 1, fsrv);
        dc->VSSetShader(fxaa->vs.Get(), nullptr, 0);
        dc->PSSetShader(fxaa->ps.Get(), nullptr, 0);
        dc->Draw(3, 0);
        ID3D11ShaderResourceView* fnull[1] = { nullptr };
        dc->PSSetShaderResources(0, 1, fnull);
    }

    dc->OMSetDepthStencilState(nullptr, 0);
    dc->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
    resolveTimer_.End(device); // M44d
}

} // namespace mye
