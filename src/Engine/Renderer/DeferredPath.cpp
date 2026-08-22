#include "Engine/Renderer/DeferredPath.h"

#include <cmath>

#include "Engine/Core/Log.h"
#include "Engine/Core/Profiler.h"
#include "Engine/Renderer/GpuBufferUtil.h" // M46a: バッファ生成ヘルパ (共通化)
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/GraphicsDevice.h"
#include "Engine/Renderer/RayTracing/RtPasses.h" // M46b: RT デバッグ表示
#include "Engine/Renderer/RayTracing/RtTypes.h"  // M46h: 反射の roughness しきい値
#include "Engine/Renderer/ShaderManager.h"

using namespace DirectX;

namespace mye {
namespace {

// ボーンパレット最大数は RenderTypes.h の mye::kMaxBones (HLSL の MYE_MAX_BONES と対) を使う。

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
    // ---- M43a: ハイトフォグ + 太陽インスキャッタ (末尾 append。既定 = 恒等) ----
    float fogHeightFalloff;
    float fogBaseHeight;
    float fogInscatterIntensity;
    float fogInscatterPower;
    XMFLOAT3 sunDirection;
    float fogPad2;
    XMFLOAT3 sunColor;
    float fogPad3;
    // ---- M54e: 局所ライトのシャドウアトラス (末尾 append)。透明後段の forward_lit が参照。
    //      ★ForwardPath.cpp の PerFrameCB と**同一の末尾**でなければならない ----
    int32_t shadowAtlasEnabled;
    float shadowAtlasTexel;
    float atlasPad[2];
    ShadowTileCB shadowTiles[kMaxShadowTiles];
};

struct PerObjectCB {
    XMFLOAT4X4 world;
    XMFLOAT4 baseColor;
    // ---- インスタンシング (M38f、末尾 append)。インスタンス版シェーダのみ参照 ----
    int32_t instanceBase;
    float instPad[3];
};

// deferred_gbuffer.hlsl / forward_lit.hlsl の MaterialParams (b2) と一致 (16 バイト)
struct MaterialCB {
    float metallic;
    float roughness;
    int32_t hasNormal; // 0=ノーマルマップ無し
    float emissive;    // M46i: 自己発光の強さ (0 = 発光なし)
};

// (M54c の ShadowTileCB は M54e で RenderTypes.h へ引き上げた — Forward の PerFrameCB も
//  同じ形を要求するようになったため。転置の式は FillShadowTilesCB 1 本きり)

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
    // ---- SSAO (M38e、末尾 append) ----
    float screenSize[2];
    int32_t ssaoEnabled;
    float ssaoPad;
    // ---- M43a: ハイトフォグ + 太陽インスキャッタ (末尾 append。既定 = 恒等) ----
    float fogHeightFalloff;
    float fogBaseHeight;
    float fogInscatterIntensity;
    float fogInscatterPower;
    XMFLOAT3 sunDirection;
    float fogPad2;
    XMFLOAT3 sunColor;
    float fogPad3;
    // ---- M46f: RT GI 合成 (末尾 append。0 = 従来と完全に同一の式) ----
    int32_t rtGiEnabled;
    // ---- M46g: RT 影 (末尾 append。0 = 従来どおり CSM をサンプルする) ----
    int32_t rtShadowEnabled;
    // ---- M46h: RT 反射 (末尾 append。0 = スペキュラ環境項は従来どおり IBL のみ) ----
    int32_t rtReflEnabled;
    float rtReflFadeStart;
    float rtReflMaxRough;
    float rtPad[3];
    // ---- M54c: 局所ライトのシャドウアトラス (末尾 append。0 = 従来と完全に同一の式) ----
    int32_t shadowAtlasEnabled;
    float shadowAtlasTexel;
    float atlasPad[2];
    ShadowTileCB shadowTiles[kMaxShadowTiles];
};

// ssao.hlsl の SsaoCB と同一レイアウト
struct SsaoCB {
    XMFLOAT4X4 viewProj; // transpose(view*proj)
    XMFLOAT3 cameraPos;
    float radius;
    float noiseScale[2];
    float intensity;
    float bias;
};

// ssao_blur.hlsl の BlurCB と同一レイアウト
struct SsaoBlurCB {
    float texel[2];
    float pad[2];
};

// M55c: deferred_gbuffer{,_instanced,_skinned}.hlsl の VelocityParams (b4) と同一レイアウト。
// **b4 は GBuffer パス専用** — Forward 系の CB (b0-b3) を 1 バイトも触らずに済ませるための
// 割り切りで、代償は「非インスタンス描画 1 回につき CB 更新が 1 本増える」こと
struct VelocityCB {
    XMFLOAT4X4 prevViewProj; // transpose(前フレームの **非ジッタ** view*proj)
    XMFLOAT4X4 prevWorld;    // transpose(前フレームに実際に描いた world)
    float jitterNdc[2];
    int32_t valid; // 0 = 履歴なし → シェーダは velocity 0 を書く
    float pad;
};

// M56a: decal_project.hlsl の DecalParams (b0) と同一レイアウト。
// ★b0 を使えるのは、デカールパスの前後で b0 が必ず張り替わるから (前 = ジオメトリパスの
//   PerFrame、後 = SSAO / 光パス / 透明後段がそれぞれ自分の CB を張る)。
//   地形パス (M58c) が b4 へ逃げているのは「地形の後に透明後段が来る」ため
struct DecalCB {
    XMFLOAT4X4 viewProj;      // transpose(view*proj)。ジッタ込み
    XMFLOAT4X4 decalWorld;    // transpose(ローカル → ワールド)
    XMFLOAT4X4 decalInvWorld; // transpose(ワールド → ローカル)
    XMFLOAT4 color;           // rgb = リニア tint / a = 不透明度
    XMFLOAT4 uv;              // xy = スケール / zw = オフセット
    XMFLOAT4 proj;            // xyz = 投影方向 / w = 角度フェードの cos
    // ---- M56b (末尾 append)。既定 0 = 法線も roughness も書かない = 恒等 ----
    XMFLOAT4 axisX; // xyz = ローカル +X のワールド向き (正規化) / w = 法線の強さ
    XMFLOAT4 axisY; // xyz = ローカル +Y のワールド向き (正規化) / w = 上書き roughness
    XMFLOAT4 surf;  // x = 粗さの強さ / y = 法線マップ有無 (0/1) / zw = 予約
};

// debug_velocity.hlsl の VelocityDebugCB と同一レイアウト
struct VelocityDebugCB {
    float dstSize[2];
    float pxRange;
    float pad;
};

// M55c: velocity 可視化が色を振り切る速度 [px/frame]。
// 1.0 は「--render-demo の Spinner (30deg/s、半径 ~4、カメラ距離 ~18.6) の刃先が
// ちょうど 1 px/frame 動く」という実測から採った値 — つまりこのデモで刃だけが色付き、
// 静止した床/柱/背景は灰のまま、という絵になる。デバッグ表示専用 (絵には影響しない)
constexpr float kVelocityDebugPxRange = 1.0f;

// M46a: 定数バッファ生成 / CB 更新は GpuBufferUtil.h へ集約 (定義は同一)
using namespace gpubuf;

} // namespace

bool DeferredPath::Init(GraphicsDevice& device, ShaderManager& shaders)
{
    ID3D11Device* dev = device.Device();

    gbufferShader_ = shaders.Load("deferred_gbuffer");
    lightShader_ = shaders.Load("deferred_light");
    // スキンメッシュ用の GBuffer シェーダをプリロード (BLENDINDICES 入力レイアウトもここで構築)
    gbufferSkinnedShader_ = shaders.Load("deferred_gbuffer_skinned");
    // インスタンシング (M38f)
    gbufferInstancedShader_ = shaders.Load("deferred_gbuffer_instanced");
    // スカイボックス (M29d)。失敗しても続行 (空が clearColor になるだけ)
    skybox_.Init(device, shaders);
    // 地形 (M58c)。失敗しても続行 (地形が描かれないだけ = 従来の絵)
    terrain_.Init(device, shaders);

    // M55c: velocity の可視化 (デバッグ表示。既定 off なので失敗しても描画は続く)
    velocityDebugShader_ = shaders.Load("debug_velocity");
    // M56a: デカール。失敗しても続行 (デカールが描かれないだけ = 従来の絵)
    decalShader_ = shaders.Load("decal_project");

    if (!CreateConstant(dev, sizeof(PerFrameCB), perFrameCB_)
        || !CreateConstant(dev, sizeof(PerObjectCB), perObjectCB_)
        || !CreateConstant(dev, sizeof(MaterialCB), materialCB_)
        || !CreateConstant(dev, sizeof(LightPassCB), lightCB_)
        || !CreateConstant(dev, sizeof(VelocityCB), velocityCB_)           // M55c (b4)
        || !CreateConstant(dev, sizeof(VelocityDebugCB), velocityDebugCB_) // M55c
        || !CreateConstant(dev, sizeof(DecalCB), decalCB_)                 // M56a
        || !CreateConstant(dev, sizeof(XMFLOAT4X4) * kMaxBones, boneCB_)) {
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

    // ---- SSAO (M38e) ----
    ssaoShader_ = shaders.Load("ssao");
    ssaoBlurShader_ = shaders.Load("ssao_blur");
    if (!CreateConstant(dev, sizeof(SsaoCB), ssaoCB_) || !CreateConstant(dev, sizeof(SsaoBlurCB), ssaoBlurCB_)) {
        return false;
    }
    D3D11_SAMPLER_DESC ps = {};
    ps.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    ps.AddressU = ps.AddressV = ps.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ps.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateSamplerState(&ps, pointClamp_.GetAddressOf()))) {
        return false;
    }
    ps.AddressU = ps.AddressV = ps.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    if (FAILED(dev->CreateSamplerState(&ps, pointWrap_.GetAddressOf()))) {
        return false;
    }
    {
        // 4x4 ランダム回転ノイズ (固定テーブル = 再現的。xy に単位ベクトル、z=0)
        constexpr float kAngles[16] = { 0.13f, 2.71f, 5.02f, 1.37f, 3.88f, 0.94f, 5.71f, 2.15f,
                                        4.42f, 1.83f, 0.55f, 3.27f, 5.44f, 2.93f, 1.11f, 4.05f };
        uint8_t pixels[16 * 4];
        for (int i = 0; i < 16; ++i) {
            const float x = std::cos(kAngles[i]) * 0.5f + 0.5f;
            const float y = std::sin(kAngles[i]) * 0.5f + 0.5f;
            pixels[i * 4 + 0] = static_cast<uint8_t>(x * 255.0f);
            pixels[i * 4 + 1] = static_cast<uint8_t>(y * 255.0f);
            pixels[i * 4 + 2] = 128; // z=0
            pixels[i * 4 + 3] = 255;
        }
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = 4;
        td.Height = 4;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc = { 1, 0 };
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd2 = {};
        sd2.pSysMem = pixels;
        sd2.SysMemPitch = 16;
        if (FAILED(dev->CreateTexture2D(&td, &sd2, noiseTex_.GetAddressOf()))
            || FAILED(dev->CreateShaderResourceView(noiseTex_.Get(), nullptr,
                                                    noiseSrv_.GetAddressOf()))) {
            return false;
        }
    }

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_BACK;
    rd.DepthClipEnable = TRUE;
    if (FAILED(dev->CreateRasterizerState(&rd, rasterizer_.GetAddressOf()))) {
        return false;
    }
    // SceneView Wireframe (M40b)。CULL_NONE = 裏面の線も見せる (GBuffer パスのみ使用 —
    // フルスクリーン解決系は常に solid)
    rd.FillMode = D3D11_FILL_WIREFRAME;
    rd.CullMode = D3D11_CULL_NONE;
    if (FAILED(dev->CreateRasterizerState(&rd, rasterizerWire_.GetAddressOf()))) {
        return false;
    }
    // M56a: デカールの投影ボックス。**表面ではなく裏面を描く** — 表面 (CULL_BACK) だと
    // カメラが箱の中に入った瞬間にデカールが丸ごと消える。深度テストは切ってあるので
    // 「箱が壁の裏に完全に隠れている」場合もラスタライズはされるが、受け面のワールド座標が
    // 箱の外に出るので PS 側の OBB 判定で必ず捨てられる。
    // DepthClipEnable=FALSE は「箱が near/far を跨いだときに面が欠ける」対策
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_FRONT;
    rd.DepthClipEnable = FALSE;
    if (FAILED(dev->CreateRasterizerState(&rd, rasterizerDecal_.GetAddressOf()))) {
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

    // ---- M56b: デカール専用ブレンド (MRT ごとに別設定) ----
    // ★**IndependentBlendEnable がこのサブの肝**。3 枚を 1 draw で書くのに、
    //   ・RT1 (法線) は「デカールの法線へどれだけ寄せるか」= SV_Target1.a
    //   ・RT3 (material) は「roughness をどれだけ上書きするか」= SV_Target3.a
    //   と **別々の係数**が要る。D3D11 は独立ブレンドのとき RT n の SRC_ALPHA を
    //   「RT n 向けの PS 出力のアルファ」から取るので、強度をそのままそこへ載せれば
    //   「強度 0 → src*0 + dst*1 = dst を厳密に維持」がハードウェア側の性質になる。
    //   シェーダに「書かない」分岐を持たせるより強い (分岐は書き忘れると壊れる)。
    // ★**RT3 の書込マスクは GREEN だけ**。metallic (r) / emissive (b) をデカールが
    //   触ると「色を貼っただけなのに金属になる」が静かに起きる。
    // ★RT1 は RGB のみ (R10G10B10A2 の 2bit アルファは誰も読まない。
    //   ここを書くと 2bit へ丸められた値が入るだけで害しかない)。
    // ★RT2 (ワールド座標) と RT4 (画面速度 = TAA の入力) は BlendEnable=FALSE +
    //   マスク 0。そもそも RTV も張らないし PS も SV_Target2/4 を持たないが、
    //   3 重に塞いでおく (ここが緩むと TAA の履歴 UV が壊れる = 気付きにくい)
    D3D11_BLEND_DESC dbd = {};
    dbd.IndependentBlendEnable = TRUE;
    for (int rt = 0; rt < 5; ++rt) {
        dbd.RenderTarget[rt].BlendEnable = FALSE;
        dbd.RenderTarget[rt].RenderTargetWriteMask = 0;
    }
    auto setAlphaBlend = [&](int rt, UINT8 mask) {
        dbd.RenderTarget[rt].BlendEnable = TRUE;
        dbd.RenderTarget[rt].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        dbd.RenderTarget[rt].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        dbd.RenderTarget[rt].BlendOp = D3D11_BLEND_OP_ADD;
        dbd.RenderTarget[rt].SrcBlendAlpha = D3D11_BLEND_ONE;
        dbd.RenderTarget[rt].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        dbd.RenderTarget[rt].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        dbd.RenderTarget[rt].RenderTargetWriteMask = mask;
    };
    setAlphaBlend(0, D3D11_COLOR_WRITE_ENABLE_ALL); // albedo (a = ジオメトリ有りマークを維持)
    setAlphaBlend(1, D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN
                         | D3D11_COLOR_WRITE_ENABLE_BLUE); // 法線
    setAlphaBlend(3, D3D11_COLOR_WRITE_ENABLE_GREEN);       // roughness だけ
    if (FAILED(dev->CreateBlendState(&dbd, blendDecal_.GetAddressOf()))) {
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
    gbVelocity_.Release(); // M55c
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
    // SSAO (M38e)
    ssaoRaw_.Release();
    ssaoBlur_.Release();
    ssaoCB_.Reset();
    ssaoBlurCB_.Reset();
    pointClamp_.Reset();
    pointWrap_.Reset();
    noiseTex_.Reset();
    noiseSrv_.Reset();
    // インスタンシング (M38f)
    instanceBuf_.Reset();
    // M55c: velocity
    prevInstanceBuf_.Reset();
    velocityCB_.Reset();
    velocityDebugCB_.Reset();
    // M56a: デカール
    decalCB_.Reset();
    rasterizerDecal_.Reset();
    // M56b
    blendDecal_.Reset();
    gbNormalCopy_.Reset();
    gbNormalCopySrv_.Reset();
    normalCopyW_ = 0;
    normalCopyH_ = 0;
    terrain_.Shutdown(); // M58c
}

// M56b: 受け面の法線 (RT1) を読みながら RT1 へ書くための読み取り用コピー。
// **フォーマットとサイズは gbNormal_ から取る** — CopyResource は寸法・フォーマットが
// 完全一致でないと黙って何もしないので、ここで数値を二重管理しない
bool DeferredPath::EnsureNormalCopy(GraphicsDevice& device)
{
    if (gbNormal_.SRV() == nullptr) {
        return false;
    }
    Microsoft::WRL::ComPtr<ID3D11Resource> res;
    gbNormal_.SRV()->GetResource(res.GetAddressOf());
    Microsoft::WRL::ComPtr<ID3D11Texture2D> srcTex;
    if (FAILED(res.As(&srcTex))) {
        return false;
    }
    D3D11_TEXTURE2D_DESC td = {};
    srcTex->GetDesc(&td);
    if (td.Width == 0 || td.Height == 0) {
        return false;
    }
    if (gbNormalCopySrv_ && normalCopyW_ == static_cast<int>(td.Width)
        && normalCopyH_ == static_cast<int>(td.Height)) {
        return true;
    }
    gbNormalCopy_.Reset();
    gbNormalCopySrv_.Reset();
    normalCopyW_ = 0;
    normalCopyH_ = 0;
    td.MipLevels = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE; // RTV は要らない (読むだけ)
    td.CPUAccessFlags = 0;
    td.MiscFlags = 0;
    ID3D11Device* dev = device.Device();
    if (FAILED(dev->CreateTexture2D(&td, nullptr, gbNormalCopy_.GetAddressOf()))
        || FAILED(dev->CreateShaderResourceView(gbNormalCopy_.Get(), nullptr,
                                                gbNormalCopySrv_.GetAddressOf()))) {
        gbNormalCopy_.Reset();
        gbNormalCopySrv_.Reset();
        return false;
    }
    normalCopyW_ = static_cast<int>(td.Width);
    normalCopyH_ = static_cast<int>(td.Height);
    return true;
}

// ---- M56a/M56b: デカール (投影ボックス。albedo + 法線 + roughness) ----
void DeferredPath::RenderDecals(GraphicsDevice& device, ShaderManager& shaders,
                                const RenderView& view, RenderResources& resources,
                                const XMFLOAT4X4& viewProjT)
{
    // デカール 0 個なら 1 命令も発行しない。**これが「デカールを置いていない golden は
    // 1 バイトも動かない」という受け入れ基準の根拠**なので、この早期 return より前に
    // 状態を触る行を足さないこと
    if (view.decals == nullptr || view.decals->items.empty()) {
        return;
    }
    ShaderProgram* prog = shaders.Get(decalShader_);
    if (!prog || !prog->valid) {
        return;
    }
    ID3D11DeviceContext* dc = device.Context();

    // M56b: このフレームに 1 枚でも法線 / roughness を書くデカールが居るか。
    // 居なければ M56a と 1 命令も違わない経路 (RT0 だけ / コピー無し) に落ちる
    bool writesSurface = false;
    for (const DecalRenderItem& d : view.decals->items) {
        writesSurface = writesSurface || DecalWritesSurface(d);
    }

    // ★**GBuffer の RT は「書くものだけ」張り直す。**
    //   計画は「IndependentBlendEnable=TRUE で RT2 (position) と RT4 (velocity) を
    //   RenderTargetWriteMask=0 で塞ぐ」を想定していたが、
    //     ・ワールド座標 (RT2) は**この場で SRV として読む**ので、そもそも RTV に
    //       残したままには出来ない (同一リソースの読み書き二重バインド)。
    //     ・書込マスク 0 は「PS が値を出さなかった RT の内容は未定義」という D3D の規則を
    //       消してくれるが、**bind しない方がそれより強い**。
    //   → M56b で足したのは RT1 (法線) と RT3 (material) だけ。
    //     **RT2 (SSAO / RT / SSR の入力) と RT4 (TAA の入力) は nullptr のまま据え置き**
    //     (RT4 を 1 バイトでも書くと TAA の履歴 UV が壊れる)。
    // ★法線 (RT1) は角度フェードのために**読みながら書く**ので、RTV に張る前に
    //   コピーを取って SRV 側はコピーを見る。順序が命 — RTV を外してからでないと
    //   CopyResource は「まだ RTV に bind されているリソース」に当たる
    dc->OMSetRenderTargets(0, nullptr, nullptr);
    ID3D11ShaderResourceView* normalSrv = gbNormal_.SRV();
    if (writesSurface && EnsureNormalCopy(device)) {
        Microsoft::WRL::ComPtr<ID3D11Resource> src;
        gbNormal_.SRV()->GetResource(src.GetAddressOf());
        dc->CopyResource(gbNormalCopy_.Get(), src.Get());
        normalSrv = gbNormalCopySrv_.Get();
    } else {
        writesSurface = false; // コピーが作れなかった = 法線を読めない → albedo だけに縮退
    }
    ID3D11RenderTargetView* rtv[4] = { gbAlbedo_.RTV(), nullptr, nullptr, nullptr };
    if (writesSurface) {
        rtv[1] = gbNormal_.RTV();
        rtv[3] = gbMaterial_.RTV();
    }
    // 深度は外す (深度テストは使わない)
    dc->OMSetRenderTargets(writesSurface ? 4 : 1, rtv, nullptr);

    ID3D11Buffer* cb[1] = { decalCB_.Get() };
    dc->VSSetConstantBuffers(0, 1, cb);
    dc->PSSetConstantBuffers(0, 1, cb);
    // s0 = LINEAR/CLAMP (光パスの IBL 用サンプラを流用 = サンプラは 1 つも増やさない)。
    // CLAMP なのはデカールの縁で反対側の画素がにじまないようにするため
    ID3D11SamplerState* samp[1] = { iblSampler_.Get() };
    dc->PSSetSamplers(0, 1, samp);
    dc->IASetInputLayout(nullptr); // 頂点は SV_VertexID から組む (VB / IB を持たない)
    dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dc->VSSetShader(prog->vs.Get(), nullptr, 0);
    dc->PSSetShader(prog->ps.Get(), nullptr, 0);
    dc->RSSetState(rasterizerDecal_.Get());
    dc->OMSetDepthStencilState(depthDisabled_.Get(), 0);
    // M56b: MRT ごとに書込マスクとブレンド係数が違う専用ステート (Init のコメント参照)。
    // RT1 / RT3 を張らないフレームでも RT0 の設定は blendAlpha_ と同一なので絵は変わらない
    dc->OMSetBlendState(blendDecal_.Get(), nullptr, 0xFFFFFFFFu);

    uint64_t boundTexture = 0;
    uint64_t boundNormalTex = 0;
    for (const DecalRenderItem& d : view.decals->items) {
        const AssetID texId = d.texture.IsNull() ? resources.textures.White() : d.texture;
        // 法線マップ非対応 / 未指定は白 1x1 を張っておく (シェーダは gDecalSurf.y で
        // サンプルするかを決めるので、中身は読まれない。null SRV を残さないためだけ)
        const bool hasNormalTex = writesSurface && !d.normalTexture.IsNull();
        const AssetID nrmId = hasNormalTex ? d.normalTexture : resources.textures.White();
        if (texId.value != boundTexture || nrmId.value != boundNormalTex) {
            Texture* tex = resources.textures.Get(texId);
            Texture* nrm = resources.textures.Get(nrmId);
            // t0 = ワールド座標 / t1 = 受け面の法線 (コピー) / t2 = デカール画像 /
            // t3 = デカールの法線マップ。t0/t1 は毎回同じだが、テクスチャが変わるたびに
            // 4 本まとめて張り直す方が「後から t2 だけ張って t0/t1 を張り忘れる」事故が起きない
            ID3D11ShaderResourceView* srvs[4] = { gbPosition_.SRV(), normalSrv,
                                                  tex ? tex->srv.Get() : nullptr,
                                                  nrm ? nrm->srv.Get() : nullptr };
            dc->PSSetShaderResources(0, 4, srvs);
            boundTexture = texId.value;
            boundNormalTex = nrmId.value;
        }
        DecalCB dc0 = {};
        dc0.viewProj = viewProjT;
        XMStoreFloat4x4(&dc0.decalWorld, XMMatrixTranspose(XMLoadFloat4x4(&d.world)));
        XMStoreFloat4x4(&dc0.decalInvWorld, XMMatrixTranspose(XMLoadFloat4x4(&d.invWorld)));
        dc0.color = { d.color.x, d.color.y, d.color.z, d.opacity };
        dc0.uv = { d.uvScale[0], d.uvScale[1], d.uvOffset[0], d.uvOffset[1] };
        dc0.proj = { d.projDir.x, d.projDir.y, d.projDir.z, d.angleFadeCos };
        // M56b: RT1 / RT3 を張っていないフレームは強度を 0 に潰す。
        // **張っていない RT へ書いても捨てられるだけ**だが、CB を見たときに
        // 「効いていない値が入っている」状態を残さない (デバッグの誤読を減らす)
        dc0.axisX = { d.axisX.x, d.axisX.y, d.axisX.z, writesSurface ? d.normalStrength : 0.0f };
        dc0.axisY = { d.axisY.x, d.axisY.y, d.axisY.z, d.roughness };
        dc0.surf = { writesSurface ? d.roughnessStrength : 0.0f, hasNormalTex ? 1.0f : 0.0f,
                     0.0f, 0.0f };
        UploadCB(dc, decalCB_.Get(), dc0);
        dc->Draw(36, 0); // 立方体 12 三角形
        prof::AddDraw(12);
    }

    // GBuffer を SRV で読んだまま残さない。加えて **ラスタライザを必ず戻す** —
    // rasterizerDecal_ は CULL_FRONT なので、後段のフルスクリーン三角形が
    // 丸ごと裏面として消える (SSAO を off にした経路で最初に踏む)
    ID3D11ShaderResourceView* nullSrvs[4] = {};
    dc->PSSetShaderResources(0, 4, nullSrvs);
    // ★M56b: **RTV も明示的に外す**。RT1 (法線) / RT3 (material) を RTV に残したまま
    //   後段 (SSAO / 光パス) が同じリソースを SRV で読むと、ドライバが暗黙に片方を
    //   外して「AO だけが真っ黒」のような読みにくい壊れ方をする
    dc->OMSetRenderTargets(0, nullptr, nullptr);
    dc->RSSetState(rasterizer_.Get());
    dc->OMSetBlendState(blendOpaque_.Get(), nullptr, 0xFFFFFFFFu);
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
    // M55c: 画面速度 (RT4)。UV 変位は ±1 に収まる小さな符号付き量なので R16G16_FLOAT で足りる
    gbVelocity_.Resize(device, view.width, view.height, DXGI_FORMAT_R16G16_FLOAT, false);
    if (!gbAlbedo_.IsValid() || !gbNormal_.IsValid() || !gbPosition_.IsValid()
        || !gbMaterial_.IsValid() || !gbVelocity_.IsValid()) {
        return;
    }

    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(view.width);
    vp.Height = static_cast<float>(view.height);
    vp.MaxDepth = 1.0f;

    // ---- 1) ジオメトリパス ----
    // M55c: MRT は 5 本 (RT4 = velocity)。blendOpaque_ は IndependentBlendEnable=FALSE なので
    // RT0 の設定 (ブレンド無効・全チャンネル書込) がそのまま 5 本すべてに適用される
    ID3D11RenderTargetView* gbufs[5] = { gbAlbedo_.RTV(), gbNormal_.RTV(), gbPosition_.RTV(),
                                         gbMaterial_.RTV(), gbVelocity_.RTV() };
    dc->OMSetRenderTargets(5, gbufs, view.dsv);
    dc->RSSetViewports(1, &vp);
    const float zero[4] = { 0, 0, 0, 0 };
    dc->ClearRenderTargetView(gbAlbedo_.RTV(), zero);
    dc->ClearRenderTargetView(gbNormal_.RTV(), zero);
    dc->ClearRenderTargetView(gbPosition_.RTV(), zero);
    dc->ClearRenderTargetView(gbMaterial_.RTV(), zero);
    dc->ClearRenderTargetView(gbVelocity_.RTV(), zero); // 背景 = 速度 0
    if (view.dsv) {
        dc->ClearDepthStencilView(view.dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    }

    // SceneView 表示モード (M40b): Unlit/Wireframe はライト白差替 + 影/IBL/SSAO/フォグ無効
    const bool unlit = view.debugViewMode != 0;
    const bool wire = view.debugViewMode == 2;
    SceneLightData unlitLights;
    unlitLights.ambient = { 1.0f, 1.0f, 1.0f };
    unlitLights.count = 0;
    const SceneLightData& L = unlit ? unlitLights : lights;

    PerFrameCB pf = {};
    const XMMATRIX v = XMLoadFloat4x4(&view.view);
    const XMMATRIX p = XMLoadFloat4x4(&view.proj);
    XMStoreFloat4x4(&pf.viewProj, XMMatrixTranspose(XMMatrixMultiply(v, p)));
    pf.cameraPos = view.cameraPos;
    pf.lightCount = L.count;
    pf.ambient = L.ambient;
    memcpy(pf.lights, L.lights, sizeof(pf.lights));
    pf.shadowVP = view.lightViewProj[0]; // 透明後段の forward_lit 用
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
    // IBL (M38c): 透明後段の forward_lit / 光パスの deferred_light が参照
    const bool ibl = !unlit && view.iblIrradiance != nullptr && view.iblPrefiltered != nullptr
        && view.iblBrdfLut != nullptr;
    pf.iblEnabled = ibl ? 1 : 0;
    pf.iblSpecMips = view.iblSpecMips;
    // M54e: 透明後段 (forward_lit) 用の局所シャドウアトラス。判定式は光パスの
    // lp.shadowAtlasEnabled と同一 — 不透明と透明で影の有無が食い違わないようにする
    pf.shadowAtlasEnabled =
        (!unlit && view.shadowAtlasSRV != nullptr && view.shadowTileCount > 0) ? 1 : 0;
    pf.shadowAtlasTexel = view.shadowAtlasTexel;
    FillShadowTilesCB(view, pf.shadowTiles);
    UploadCB(dc, perFrameCB_.Get(), pf);

    ID3D11Buffer* cbs[2] = { perFrameCB_.Get(), perObjectCB_.Get() };
    dc->VSSetConstantBuffers(0, 2, cbs);
    dc->PSSetConstantBuffers(0, 2, cbs);
    ID3D11Buffer* matCbs[1] = { materialCB_.Get() };
    dc->PSSetConstantBuffers(2, 1, matCbs);
    // ---- M55c: velocity 用 CB (b4)。VS が prevWorld/prevViewProj を、PS が jitter/valid を読む ----
    // prevViewProj は **非ジッタ** (RenderSystem が projNoJitter で保存している)。
    // 履歴が無いフレームは valid=0 → シェーダは velocity 0 を書く (行列は使われないが、
    // 未初期化を渡さないよう今フレームの非ジッタ VP で埋めておく)
    VelocityCB vel = {};
    if (view.prevViewProjValid != 0) {
        XMStoreFloat4x4(&vel.prevViewProj, XMMatrixTranspose(XMLoadFloat4x4(&view.prevViewProj)));
        vel.valid = 1;
    } else {
        const XMMATRIX nj = v * XMLoadFloat4x4(&view.projNoJitter);
        XMStoreFloat4x4(&vel.prevViewProj, XMMatrixTranspose(nj));
        vel.valid = 0;
    }
    XMStoreFloat4x4(&vel.prevWorld, XMMatrixIdentity()); // 非インスタンス描画が毎回上書きする
    vel.jitterNdc[0] = view.jitterNdc[0];
    vel.jitterNdc[1] = view.jitterNdc[1];
    UploadCB(dc, velocityCB_.Get(), vel);
    ID3D11Buffer* velCbs[1] = { velocityCB_.Get() };
    dc->VSSetConstantBuffers(4, 1, velCbs);
    dc->PSSetConstantBuffers(4, 1, velCbs);
    ID3D11SamplerState* samplers[3] = { sampler_.Get(), shadowSampler_.Get(), iblSampler_.Get() };
    dc->PSSetSamplers(0, 3, samplers);
    dc->RSSetState(wire ? rasterizerWire_.Get() : rasterizer_.Get()); // M40b
    dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dc->OMSetDepthStencilState(depthOpaque_.Get(), 0);
    dc->OMSetBlendState(blendOpaque_.Get(), nullptr, 0xFFFFFFFFu);

    dc->IASetInputLayout(gbProg->inputLayout.Get());
    dc->VSSetShader(gbProg->vs.Get(), nullptr, 0);
    dc->PSSetShader(gbProg->ps.Get(), nullptr, 0);

    // インスタンス run 検出 (M38f): 非スキン opaque の同一 (material,mesh) 連続 run。
    // GBuffer は全マテリアルが同一シェーダなので Forward と違いシェーダ一致判定は不要
    runs_.clear();
    worlds_.clear();
    ShaderProgram* gbInstProg = shaders.Get(gbufferInstancedShader_);
    if (view.instancingEnabled != 0 && gbInstProg && gbInstProg->valid) {
        canInstance_.resize(queue.opaque.size());
        for (size_t i = 0; i < queue.opaque.size(); ++i) {
            const RenderItem& it = queue.opaque[i];
            canInstance_[i] = (it.bones == nullptr && resources.materials.Get(it.material)
                               && resources.meshes.Get(it.mesh))
                ? 1
                : 0;
        }
        BuildInstanceRuns(queue.opaque, canInstance_, runs_, worlds_);
        // M55c: 前フレーム world を worlds_ と同じ並びで積む。BuildInstanceRuns は
        // Forward/Shadow も呼ぶ共有関数なので触らず、runs_ から組み直す (並びは定義上一致)
        prevWorlds_.clear();
        prevWorlds_.reserve(worlds_.size());
        for (const MeshInstanceRun& r : runs_) {
            for (uint32_t k = 0; k < r.count; ++k) {
                prevWorlds_.push_back(queue.opaque[r.first + k].prevWorld);
            }
        }
        if (!worlds_.empty() && instanceBuf_.Upload(device, worlds_)
            && prevInstanceBuf_.Upload(device, prevWorlds_)) {
            ID3D11ShaderResourceView* isrvs[2] = { instanceBuf_.SRV(), prevInstanceBuf_.SRV() };
            dc->VSSetShaderResources(0, 2, isrvs);
        } else {
            runs_.clear();
        }
    }

    uint64_t boundMesh = 0;
    uint64_t boundTexture = 0;
    uint64_t boundNormal = 0;
    uint64_t boundGbShader = gbufferShader_.value; // 上で gbProg を bind 済み
    size_t nextRun = 0;
    for (size_t idx = 0; idx < queue.opaque.size(); ++idx) {
        const RenderItem& item = queue.opaque[idx];
        Material* mat = resources.materials.Get(item.material);
        Mesh* mesh = resources.meshes.Get(item.mesh);
        if (!mat || !mesh) {
            continue;
        }
        // インスタンス run の先頭なら一括描画 (M38f)
        if (nextRun < runs_.size() && runs_[nextRun].first == idx) {
            const MeshInstanceRun& run = runs_[nextRun];
            ++nextRun;
            if (gbufferInstancedShader_.value != boundGbShader) {
                dc->IASetInputLayout(gbInstProg->inputLayout.Get());
                dc->VSSetShader(gbInstProg->vs.Get(), nullptr, 0);
                dc->PSSetShader(gbInstProg->ps.Get(), nullptr, 0);
                boundGbShader = gbufferInstancedShader_.value;
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
            po.world = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 }; // 未使用
            po.baseColor = SrgbToLinear(mat->baseColor);
            po.instanceBase = static_cast<int32_t>(run.base);
            UploadCB(dc, perObjectCB_.Get(), po);
            MaterialCB imc = {};
            imc.metallic = mat->metallic;
            imc.roughness = mat->roughness;
            imc.hasNormal = mat->normalTex.IsNull() ? 0 : 1;
            imc.emissive = mat->emissiveIntensity;
            UploadCB(dc, materialCB_.Get(), imc);
            dc->DrawIndexedInstanced(mesh->indexCount, run.count, 0, 0, 0);
            prof::AddDraw(static_cast<int>(mesh->indexCount / 3 * run.count));
            idx += run.count - 1; // for の ++idx と合わせて run 全体を飛ばす
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
        // M55c: この描画の「前フレームに実際に描いた world」。b4 の他のフィールドは
        // フレーム頭で埋めた値をそのまま持ち回る
        XMStoreFloat4x4(&vel.prevWorld, XMMatrixTranspose(XMLoadFloat4x4(&item.prevWorld)));
        UploadCB(dc, velocityCB_.Get(), vel);
        MaterialCB mc = {};
        mc.metallic = mat->metallic;
        mc.roughness = mat->roughness;
        mc.hasNormal = mat->normalTex.IsNull() ? 0 : 1;
        mc.emissive = mat->emissiveIntensity;
        UploadCB(dc, materialCB_.Get(), mc);
        dc->DrawIndexed(mesh->indexCount, 0, 0);
        prof::AddDraw(static_cast<int>(mesh->indexCount / 3));
    }

    // インスタンス SRV を外す (次フレームの Map と競合させない、M38f。M55c で 2 本に)
    ID3D11ShaderResourceView* nullVsSrvs[2] = {};
    dc->VSSetShaderResources(0, 2, nullVsSrvs);

    // ---- 1.1) 地形 (M58c): 同じ GBuffer + 同じ深度へ専用シェーダで書く。
    //      RT / ビューポート / ラスタライザ (Wireframe 込み) / 深度 / ブレンドは
    //      上の設定をそのまま使う。CB は b4 なので b0-b3 は張り替わらない =
    //      この後の透明後段 (forward_lit) は何も張り直さなくてよい。
    //      地形が無いフレームは TerrainPass が即 return する = 従来とビット一致 ----
    terrain_.RenderGBuffer(device, shaders, view, resources);

    // Wireframe (M40b) は GBuffer パスのみ — フルスクリーン解決系は solid に戻す
    if (wire) {
        dc->RSSetState(rasterizer_.Get());
    }

    // ---- 1.2) デカール (M56a/M56b): ジオメトリパス (地形込み) の直後・SSAO の前。
    //      「もう GBuffer に書かれた面」の albedo / 法線 / roughness を投影ボックスで
    //      上描きするので、
    //      **地形の後**でなければ地形に貼れない。SSAO / RT / 光パスの**前**でなければ
    //      デカールの色がライティングにも AO にも乗らない。
    //      Wireframe (M40b) では線の画素しか GBuffer に無く投影しても意味が無いので飛ばす ----
    if (!wire) {
        RenderDecals(device, shaders, view, resources, pf.viewProj);
    }

    // ---- 1.5) SSAO (M38e): worldpos + normal → 半解像度 AO → 4x4 ブラー ----
    // (Unlit/Wireframe では環境項が定数 1 のためスキップ、M40b)
    ShaderProgram* ssaoProg = shaders.Get(ssaoShader_);
    ShaderProgram* ssaoBlurProg = shaders.Get(ssaoBlurShader_);
    const bool ssaoOn = view.ssaoEnabled != 0 && !unlit && ssaoProg && ssaoProg->valid
        && ssaoBlurProg && ssaoBlurProg->valid;
    if (ssaoOn) {
        const int hw = (view.width > 1) ? view.width / 2 : 1;
        const int hh = (view.height > 1) ? view.height / 2 : 1;
        ssaoRaw_.Resize(device, hw, hh, DXGI_FORMAT_R8_UNORM, /*withDepth=*/false);
        ssaoBlur_.Resize(device, hw, hh, DXGI_FORMAT_R8_UNORM, /*withDepth=*/false);

        SsaoCB sc = {};
        sc.viewProj = pf.viewProj; // 既に transpose 済み
        sc.cameraPos = view.cameraPos;
        sc.radius = (view.ssaoRadius > 0.01f) ? view.ssaoRadius : 0.8f; // M40d: CameraPostFx
        sc.noiseScale[0] = static_cast<float>(hw) / 4.0f;
        sc.noiseScale[1] = static_cast<float>(hh) / 4.0f;
        sc.intensity = view.ssaoIntensity;
        sc.bias = 0.03f;
        UploadCB(dc, ssaoCB_.Get(), sc);

        D3D11_VIEWPORT hvp = {};
        hvp.Width = static_cast<float>(hw);
        hvp.Height = static_cast<float>(hh);
        hvp.MaxDepth = 1.0f;
        dc->RSSetViewports(1, &hvp);
        dc->IASetInputLayout(nullptr);
        dc->OMSetDepthStencilState(depthDisabled_.Get(), 0);
        dc->OMSetBlendState(blendOpaque_.Get(), nullptr, 0xFFFFFFFFu);

        // AO 生成 (t0=position t1=normal t2=noise / s0=point clamp s1=point wrap)
        ID3D11RenderTargetView* aoRtv[1] = { ssaoRaw_.RTV() };
        dc->OMSetRenderTargets(1, aoRtv, nullptr);
        ID3D11Buffer* aoCbs[1] = { ssaoCB_.Get() };
        dc->PSSetConstantBuffers(0, 1, aoCbs);
        ID3D11SamplerState* aoSamps[2] = { pointClamp_.Get(), pointWrap_.Get() };
        dc->PSSetSamplers(0, 2, aoSamps);
        ID3D11ShaderResourceView* aoSrvs[3] = { gbPosition_.SRV(), gbNormal_.SRV(),
                                                noiseSrv_.Get() };
        dc->PSSetShaderResources(0, 3, aoSrvs);
        dc->VSSetShader(ssaoProg->vs.Get(), nullptr, 0);
        dc->PSSetShader(ssaoProg->ps.Get(), nullptr, 0);
        dc->Draw(3, 0);

        // ブラー (raw → blur)
        SsaoBlurCB bc = {};
        bc.texel[0] = 1.0f / static_cast<float>(hw);
        bc.texel[1] = 1.0f / static_cast<float>(hh);
        UploadCB(dc, ssaoBlurCB_.Get(), bc);
        ID3D11RenderTargetView* blurRtv[1] = { ssaoBlur_.RTV() };
        dc->OMSetRenderTargets(1, blurRtv, nullptr);
        ID3D11Buffer* blurCbs[1] = { ssaoBlurCB_.Get() };
        dc->PSSetConstantBuffers(0, 1, blurCbs);
        ID3D11SamplerState* blurSamps[1] = { iblSampler_.Get() }; // linear clamp
        dc->PSSetSamplers(0, 1, blurSamps);
        ID3D11ShaderResourceView* rawSrv[1] = { ssaoRaw_.SRV() };
        dc->PSSetShaderResources(0, 1, rawSrv);
        dc->VSSetShader(ssaoBlurProg->vs.Get(), nullptr, 0);
        dc->PSSetShader(ssaoBlurProg->ps.Get(), nullptr, 0);
        dc->Draw(3, 0);

        ID3D11ShaderResourceView* aoNull[3] = {};
        dc->PSSetShaderResources(0, 3, aoNull); // 光パスで再バインドする前に解除
    }

    // ---- 1.7) レイトレ拡散 GI (M46f) / RT 影 (M46g) / RT 反射 (M46h): ライトパスの前に撃つ。
    //      デバッグ表示 (mode 4-8 = GI / 9 = 影 / 10-11 = 反射) も**この結果を使い回す** —
    //      1 フレームに 2 回撃つとテンポラル履歴が二重に進んで蓄積が壊れるため。
    //      Unlit/Wireframe は環境項が定数・影も無効なので撃たない (SSAO/IBL と同じ扱い) ----
    const bool rtAvailable = view.rtPasses != nullptr && view.rtScene != nullptr;
    const bool rtGiOn = rtAvailable && !unlit && view.rtGiEnabled != 0;
    const bool rtShadowOn = rtAvailable && !unlit && view.rtShadowEnabled != 0;
    const bool rtReflOn = rtAvailable && !unlit && view.rtReflEnabled != 0;
    RtFrameInputs rtIn;
    RtGiResult rtGi;
    RtReflResult rtRefl;
    ID3D11ShaderResourceView* rtShadowSrv = nullptr;
    if (rtAvailable) {
        rtIn.scene = view.rtScene;
        rtIn.lights = &lights;
        rtIn.gbNormal = gbNormal_.SRV();
        rtIn.gbPosition = gbPosition_.SRV();
        rtIn.gbAlbedo = gbAlbedo_.SRV();
        rtIn.gbMaterial = gbMaterial_.SRV(); // M46h: metallic / roughness
        // M55f: 画面速度 (RT4)。テンポラル蓄積が履歴 UV に使う。velocity を書けなかった
        // フレーム (vel.valid==0 = 履歴なし) は渡さない — 全画素 0 の RT4 を「動いていない」と
        // 読むと、カメラが動いた初回フレームの履歴を取り違える
        rtIn.gbVelocity = (vel.valid != 0) ? gbVelocity_.SRV() : nullptr;
        rtIn.skyCube = view.skyCubemap;
        const bool needGi = rtGiOn || (view.rtDebugMode >= 4 && view.rtDebugMode <= 8);
        const bool needShadow = rtShadowOn || view.rtDebugMode == 9;
        const bool needRefl = rtReflOn || view.rtDebugMode == 10 || view.rtDebugMode == 11;
        if (needGi || needShadow || needRefl) {
            // GBuffer を CS の SRV で読むので RTV を先に外す
            // (SSAO off の経路では GBuffer が RTV に残ったままなので必須。M44b と同じ罠)
            dc->OMSetRenderTargets(0, nullptr, nullptr);
            if (needGi) {
                rtGi = view.rtPasses->RenderGi(device, shaders, view, rtIn);
            }
            if (needShadow) {
                rtShadowSrv = view.rtPasses->RenderShadow(device, shaders, view, rtIn);
            }
            if (needRefl) {
                rtRefl = view.rtPasses->RenderReflection(device, shaders, view, rtIn);
            }
        }
    }
    const bool rtGiBound = rtGiOn && rtGi.filtered != nullptr;
    const bool rtShadowBound = rtShadowOn && rtShadowSrv != nullptr;
    const bool rtReflBound = rtReflOn && rtRefl.filtered != nullptr;

    // ---- 2) ライティングパス (フルスクリーン解決) ----
    dc->OMSetRenderTargets(1, &view.rtv, nullptr); // GBuffer を SRV で読むため depth も外す
    dc->RSSetViewports(1, &vp);
    LightPassCB lp = {};
    lp.ambient = L.ambient; // M40b: Unlit は白定数
    lp.lightCount = L.count;
    lp.clearColor = { view.clearColor[0], view.clearColor[1], view.clearColor[2],
                      view.clearColor[3] };
    memcpy(lp.lights, L.lights, sizeof(lp.lights));
    lp.shadowVP = view.lightViewProj[0];
    lp.shadowTexel = view.shadowTexelSize;
    lp.shadowEnabled = (!unlit && view.shadowSRV != nullptr) ? 1 : 0;
    lp.shadowVP12[0] = view.lightViewProj[1]; // M38d CSM
    lp.shadowVP12[1] = view.lightViewProj[2];
    lp.cascadeInfo[0] = view.cascadeSplits[0];
    lp.cascadeInfo[1] = view.cascadeSplits[1];
    lp.cascadeInfo[2] = view.cascadeSplits[2];
    lp.cascadeInfo[3] = static_cast<float>(view.cascadeCount);
    lp.cameraPos = view.cameraPos;
    lp.fogColor = view.fogColor;
    lp.fogMode = unlit ? -1 : view.fogMode;
    lp.fogDensity = view.fogDensity;
    lp.fogStart = view.fogStart;
    lp.fogEnd = view.fogEnd;
    lp.iblEnabled = pf.iblEnabled; // M38c (透明後段と同判定)
    lp.iblSpecMips = view.iblSpecMips;
    lp.screenSize[0] = static_cast<float>(view.width); // M38e
    lp.screenSize[1] = static_cast<float>(view.height);
    lp.ssaoEnabled = ssaoOn ? 1 : 0;
    lp.fogHeightFalloff = view.fogHeightFalloff; // M43a
    lp.fogBaseHeight = view.fogBaseHeight;
    lp.fogInscatterIntensity = view.fogInscatterIntensity;
    lp.fogInscatterPower = view.fogInscatterPower;
    lp.sunDirection = view.sunDirection;
    lp.sunColor = view.sunColor;
    lp.rtGiEnabled = rtGiBound ? 1 : 0;         // M46f
    lp.rtShadowEnabled = rtShadowBound ? 1 : 0; // M46g
    lp.rtReflEnabled = rtReflBound ? 1 : 0;     // M46h
    lp.rtReflFadeStart = kRtReflFadeStart;      // しきい値の出所は RtTypes.h
    lp.rtReflMaxRough = kRtReflMaxRoughness;
    // M54c: 局所ライトのシャドウアトラス。SRV が null (Forward / AssetPreview / 影を投げる
    // 局所ライトが 1 本も無いシーン) なら 0 = 従来と 1 ビットも変わらない経路へ落ちる
    lp.shadowAtlasEnabled =
        (!unlit && view.shadowAtlasSRV != nullptr && view.shadowTileCount > 0) ? 1 : 0;
    lp.shadowAtlasTexel = view.shadowAtlasTexel;
    FillShadowTilesCB(view, lp.shadowTiles);
    UploadCB(dc, lightCB_.Get(), lp);
    ID3D11Buffer* lightCbs[1] = { lightCB_.Get() };
    dc->PSSetConstantBuffers(0, 1, lightCbs);
    dc->VSSetConstantBuffers(0, 1, lightCbs);
    // 光パスの s0 = IBL 用 LINEAR/CLAMP (LUT を wrap で引くと roughness=1.0 が v=0 に
    // 巻き戻るため clamp 必須)、s1 = シャドウ比較サンプラ。**SSAO パス (M38e) が s1 を
    // point-wrap で上書きするため両方を明示的に張り直す** (張り忘れると影が全消えする)
    ID3D11SamplerState* lightSamplers[2] = { iblSampler_.Get(), shadowSampler_.Get() };
    dc->PSSetSamplers(0, 2, lightSamplers);
    // GBuffer t0-3 + シャドウ t4 + IBL t5-7 (M38c) + SSAO t8 (M38e) + RT GI t9 (M46f)
    // + RT 影 t10 (M46g) + RT 反射 t11 (M46h) + シャドウアトラス t12 (M54c)。
    // s0=IBL サンプラ / s1=比較サンプラ bind 済み (アトラスも s1 を共有する = サンプラ増やさず)
    ID3D11ShaderResourceView* gbSrvs[13] = { gbAlbedo_.SRV(),     gbNormal_.SRV(),
                                             gbPosition_.SRV(),   gbMaterial_.SRV(),
                                             view.shadowSRV,      view.iblIrradiance,
                                             view.iblPrefiltered, view.iblBrdfLut,
                                             ssaoOn ? ssaoBlur_.SRV() : nullptr,
                                             rtGiBound ? rtGi.filtered : nullptr,
                                             rtShadowBound ? rtShadowSrv : nullptr,
                                             rtReflBound ? rtRefl.filtered : nullptr,
                                             view.shadowAtlasSRV };
    dc->PSSetShaderResources(0, 13, gbSrvs);
    dc->IASetInputLayout(nullptr);
    dc->OMSetBlendState(blendOpaque_.Get(), nullptr, 0xFFFFFFFFu);
    dc->VSSetShader(lightProg->vs.Get(), nullptr, 0);
    dc->PSSetShader(lightProg->ps.Get(), nullptr, 0);
    dc->OMSetDepthStencilState(depthDisabled_.Get(), 0);
    dc->Draw(3, 0);
    // 統合契約 予約 2: 最終的に [16] になる (M54 が [13]、M56 が [15]、M57 が [16])
    ID3D11ShaderResourceView* nullSrvs[13] = {};
    dc->PSSetShaderResources(0, 13, nullSrvs); // 次フレームで RT に戻すため解除

    // ---- 2.5) スカイボックス (M29d): clearColor ピクセルを深度 1.0 判定で上書き ----
    // (Wireframe はフルスクリーン三角形が線になるためスキップ、M40b)
    if (!wire) {
        skybox_.Render(device, shaders, view);
    }

    // ---- 3) 透明後段 (Forward — マテリアルのシェーダで上描き) ----
    if (!queue.transparent.empty()) {
        dc->OMSetRenderTargets(1, &view.rtv, view.dsv);
        if (wire) {
            dc->RSSetState(rasterizerWire_.Get()); // M40b: 透明メッシュもワイヤ表示
        }
        // forward_lit はシャドウ t1 / IBL t3-5 / 局所シャドウアトラス t6 を参照
        // (M38c + M54e)。s0 は光パスで IBL 用に差し替えたのでマテリアル用 (異方性) に戻す。
        // s1 (比較サンプラ = アトラスと CSM で共有) と s2 (IBL) はフレーム頭で bind 済み。
        // ★t6 を足したら**本数も 5 → 6 へ**増やすこと (増やし忘れると透明メッシュだけが
        //   前段の光パスが t6 に残したもの、または null を読む = 影が出ない/ゴミが出る)
        ID3D11ShaderResourceView* fwdSrvs[6] = { view.shadowSRV,      nullptr,
                                                 view.iblIrradiance,  view.iblPrefiltered,
                                                 view.iblBrdfLut,     view.shadowAtlasSRV };
        dc->PSSetShaderResources(1, 6, fwdSrvs);
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
            mc.emissive = mat->emissiveIntensity;
            UploadCB(dc, materialCB_.Get(), mc);
            dc->DrawIndexed(mesh->indexCount, 0, 0);
            prof::AddDraw(static_cast<int>(mesh->indexCount / 3));
        }
    } else {
        // パーティクル後段のために RTV+DSV を戻しておく
        // (M42a: パーティクル直前に RenderSystem が read-only DSV へ差し替える)
        dc->OMSetRenderTargets(1, &view.rtv, view.dsv);
    }
    dc->OMSetDepthStencilState(nullptr, 0);
    dc->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
    // Wireframe (M40b) はメッシュ描画のみ — 後段 (パーティクル/ポスプロ) は solid に戻す
    if (wire) {
        dc->RSSetState(rasterizer_.Get());
    }

    // ---- 4) RT デバッグ表示 (M46b): BVH の検証用に画面を丸ごと差し替える。
    //      既定 (rtDebugMode==0 / rtScene==null) では何も起きない ----
    if (view.rtDebugMode != 0 && rtAvailable) {
        // GI 系表示 (4=生 / 5=蓄積 / 6=履歴長 / 7=SVGF / 8=分散) と影 (9=可視率)、
        // 反射 (10=生 / 11=デノイズ後) の入力は 1.7 で撃った結果。
        // ここで撃ち直すと同じフレームで履歴が 2 回進むので絶対に呼ばない
        view.rtPasses->RenderDebug(device, shaders, view, rtIn, rtGi, rtShadowSrv, rtRefl);
        // パーティクル後段のために RTV+DSV を戻す (ブリットが RTV のみに差し替えたため)
        dc->OMSetRenderTargets(1, &view.rtv, view.dsv);
        dc->OMSetDepthStencilState(nullptr, 0);
        dc->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
    }

    // ---- 5) velocity の可視化 (M55c)。既定 (velocityDebug==0) では 1 命令も走らない。
    //      RT4 を読む本番の消費者は M55d/M55e/M55f まで居ないので、ここが唯一の目視口 ----
    if (view.velocityDebug != 0) {
        ShaderProgram* velDbg = shaders.Get(velocityDebugShader_);
        if (velDbg && velDbg->valid) {
            VelocityDebugCB vd = {};
            vd.dstSize[0] = static_cast<float>(view.width);
            vd.dstSize[1] = static_cast<float>(view.height);
            vd.pxRange = kVelocityDebugPxRange;
            UploadCB(dc, velocityDebugCB_.Get(), vd);
            // GBuffer を SRV で読むので深度は外す (光パスと同じ作法)
            dc->OMSetRenderTargets(1, &view.rtv, nullptr);
            dc->RSSetViewports(1, &vp);
            ID3D11Buffer* vdCbs[1] = { velocityDebugCB_.Get() };
            dc->PSSetConstantBuffers(0, 1, vdCbs);
            ID3D11ShaderResourceView* velSrv[1] = { gbVelocity_.SRV() };
            dc->PSSetShaderResources(0, 1, velSrv);
            dc->IASetInputLayout(nullptr);
            dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            dc->OMSetDepthStencilState(depthDisabled_.Get(), 0);
            dc->OMSetBlendState(blendOpaque_.Get(), nullptr, 0xFFFFFFFFu);
            dc->VSSetShader(velDbg->vs.Get(), nullptr, 0);
            dc->PSSetShader(velDbg->ps.Get(), nullptr, 0);
            dc->Draw(3, 0);
            ID3D11ShaderResourceView* velNull[1] = {};
            dc->PSSetShaderResources(0, 1, velNull); // 次フレームで RTV に戻すため解除
            // パーティクル後段のために RTV+DSV と状態を戻す (RT デバッグ表示と同じ後始末)
            dc->OMSetRenderTargets(1, &view.rtv, view.dsv);
            dc->OMSetDepthStencilState(nullptr, 0);
            dc->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
        }
    }
}

} // namespace mye
