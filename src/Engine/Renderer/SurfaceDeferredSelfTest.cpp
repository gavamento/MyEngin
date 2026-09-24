//====================================================================================
//                          SurfaceDeferredSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                     09/24/2026
//                                          M79 sub-03: Deferred サーフェス段・CSM 影の回帰テスト実装
//====================================================================================
#include "Engine/Renderer/SurfaceDeferredSelfTest.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include <DirectXMath.h>
#include <d3d11.h>
#include <wrl/client.h>

#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/DeferredPath.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/GraphicsDevice.h"
#include "Engine/Renderer/ShaderManager.h"
#include "Engine/Renderer/ShadowPass.h"
#include "Engine/Renderer/WaterPass.h" // review-1 #1 補強: WaterDrawData (水面が絡む確認用)

using namespace DirectX;
using Microsoft::WRL::ComPtr;

namespace mye {
namespace {

int g_fails = 0;

void Check(bool cond, const char* what)
{
    if (cond) {
        MYE_LOG_INFO("  PASS: %s", what);
    } else {
        MYE_LOG_ERROR("  FAIL: %s", what);
        ++g_fails;
    }
}

void WriteFile(const std::filesystem::path& path, const std::string& content)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << content;
}

// IEEE754 half (D3D の R16G16_FLOAT が並べるビット列) → float。sign/exp/mantissa の素直な展開
float HalfToFloat(uint16_t h)
{
    const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
    uint32_t exponent = (h & 0x7C00u) >> 10;
    uint32_t mantissa = (h & 0x03FFu);
    uint32_t bits;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            exponent = 1;
            while ((mantissa & 0x0400u) == 0) {
                mantissa <<= 1;
                --exponent;
            }
            mantissa &= 0x03FFu;
            bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
        }
    } else if (exponent == 0x1F) {
        bits = sign | 0x7F800000u | (mantissa << 13);
    } else {
        bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
    }
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

// gTime に応じて頂点を +Y へ動かすだけの最小サーフェスシェーダ。Properties なし。
// PSMain は固定色 (0.2,0.8,0.3) を返す — GBuffer 経由の反射/環境光計算を一切通らないので、
// HDR シーンの読み戻り値がそのまま「色エントリ (実体は速度エントリの SV_Target0) が
// GBuffer をバイパスして直接 HDR に出た」ことの証拠になる
const char* kDisplaceSurface = R"HLSL(
#include "MyEngineSurface.hlsli"
struct VSIn { float3 pos : POSITION; };
struct VSOut { float4 pos : SV_Position; };
VSOut VSMain(VSIn v)
{
    VSOut o;
    float3 p = v.pos;
    // 変位は小さく保つ (メッシュ自身のスクリーン footprint から読み戻しピクセルが
    // 外れない範囲。速度は非 0 であることだけを見るので振幅は問わない)
    p.y += gTime * 0.15f;
    float4 worldPos = mul(float4(p, 1.0f), gWorld);
    o.pos = mul(worldPos, gViewProj);
    return o;
}
float4 PSMain(VSOut i) : SV_Target
{
    return float4(0.2f, 0.8f, 0.3f, 1.0f);
}
)HLSL";

// 上と同一だが頂点変位を行わない (比較用の「剛体」版)
const char* kRigidSurface = R"HLSL(
#include "MyEngineSurface.hlsli"
struct VSIn { float3 pos : POSITION; };
struct VSOut { float4 pos : SV_Position; };
VSOut VSMain(VSIn v)
{
    VSOut o;
    float4 worldPos = mul(float4(v.pos, 1.0f), gWorld);
    o.pos = mul(worldPos, gViewProj);
    return o;
}
float4 PSMain(VSOut i) : SV_Target
{
    return float4(0.2f, 0.8f, 0.3f, 1.0f);
}
)HLSL";

// M79 sub-05 round 2: Deferred の透明段が色エントリを描けることを確認する最小フィクスチャ。
// 半透明 (alpha=0.5) の固定色を返すだけ — 変位・予約 CB の値は問わない
const char* kTransparentColorSurface = R"HLSL(
#include "MyEngineSurface.hlsli"
struct VSIn { float3 pos : POSITION; };
struct VSOut { float4 pos : SV_Position; };
VSOut VSMain(VSIn v)
{
    VSOut o;
    float4 worldPos = mul(float4(v.pos, 1.0f), gWorld);
    o.pos = mul(worldPos, gViewProj);
    return o;
}
float4 PSMain(VSOut i) : SV_Target
{
    return float4(0.0f, 0.8f, 0.0f, 0.5f);
}
)HLSL";

// review-1 #1 の再現用: VS で Texture2D (_HeightTex) を読む。名前解決で VS の SRV スロットへ
// 張られるため、後続の instanced run が期待する t0 (インスタンス行列バッファ) と衝突しうる
// (rv1\vtexA_def.png の再現)。Properties の既定テクスチャ ("white") で足りるので .mat.json 側は
// properties を書かなくてよい
const char* kVTexSurface = R"HLSL(
/*@MyEngineProperties
_HeightTex ("Height", 2D) = "white" {}
@*/
#include "MyEngineSurface.hlsli"
Texture2D _HeightTex;
struct VSIn { float3 pos : POSITION; };
struct VSOut { float4 pos : SV_Position; };
VSOut VSMain(VSIn v)
{
    VSOut o;
    float3 p = v.pos;
    p.y += _HeightTex.SampleLevel(gSampler, float2(0.5f, 0.5f), 0.0f).r * 0.01f;
    float4 worldPos = mul(float4(p, 1.0f), gWorld);
    o.pos = mul(worldPos, gViewProj);
    return o;
}
float4 PSMain(VSOut i) : SV_Target
{
    return float4(1.0f, 0.0f, 1.0f, 1.0f);
}
)HLSL";

// ---- (a)(b): DeferredPath のフォワード段。gTime 変位アイテムは gbVelocity が非 0、
//      剛体アイテムは 0。どちらも GBuffer/光パスを経由せず HDR シーンへ固定色がそのまま出る ----
void TestDeferredForwardStepVelocityAndBypass(GraphicsDevice& device, const std::wstring& engineShaderDir)
{
    MYE_LOG_INFO("-- DeferredPath: フォワード段の速度 (gTime 変位) と GBuffer バイパス --");
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / L"mye_surface_deferred_selftest";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    WriteFile(dir / L"DisplaceProbe.surface.hlsl", kDisplaceSurface);
    WriteFile(dir / L"RigidProbe.surface.hlsl", kRigidSurface);

    auto writeMat = [&](const wchar_t* fileName, const std::string& shader) {
        std::string j = "{\"shader\":\"" + shader + "\"}";
        WriteFile(dir / fileName, j);
    };
    writeMat(L"Displace.mat.json", "DisplaceProbe.surface");
    writeMat(L"Rigid.mat.json", "RigidProbe.surface");

    ShaderManager shaders;
    Check(shaders.Init(device, { dir.wstring(), engineShaderDir }), "shader manager init");

    RenderResources resources;
    resources.Init(device);
    const AssetID displaceMatId = resources.materials.LoadFromFile(
        (dir / L"Displace.mat.json").wstring(), resources.textures, dir.wstring());
    const AssetID rigidMatId = resources.materials.LoadFromFile(
        (dir / L"Rigid.mat.json").wstring(), resources.textures, dir.wstring());
    Check(!displaceMatId.IsNull() && !rigidMatId.IsNull(), "2 本の .mat.json が読み込める");

    DeferredPath dp;
    Check(dp.Init(device, shaders), "DeferredPath::Init");
    Check(dp.WritesVelocity(), "DeferredPath は velocity を書く (WritesVelocity)");

    const AssetID quad = resources.meshes.Quad();

    constexpr UINT kWidth = 96;
    constexpr UINT kHeight = 32;
    ID3D11Device* dev = device.Device();
    ID3D11DeviceContext* dc = device.Context();

    // HDR シーン (色読み戻し用。R8G8B8A8 で十分 — 固定色の一致を見るだけ)
    D3D11_TEXTURE2D_DESC ctd = {};
    ctd.Width = kWidth;
    ctd.Height = kHeight;
    ctd.MipLevels = 1;
    ctd.ArraySize = 1;
    ctd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    ctd.SampleDesc = { 1, 0 };
    ctd.Usage = D3D11_USAGE_DEFAULT;
    ctd.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> colorTex;
    ComPtr<ID3D11RenderTargetView> colorRtv;
    bool ok = SUCCEEDED(dev->CreateTexture2D(&ctd, nullptr, colorTex.GetAddressOf()));
    ok = ok && SUCCEEDED(dev->CreateRenderTargetView(colorTex.Get(), nullptr, colorRtv.GetAddressOf()));
    D3D11_TEXTURE2D_DESC staged = ctd;
    staged.Usage = D3D11_USAGE_STAGING;
    staged.BindFlags = 0;
    staged.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> colorStaging;
    ok = ok && SUCCEEDED(dev->CreateTexture2D(&staged, nullptr, colorStaging.GetAddressOf()));

    // 深度 (Deferred が要求する DSV。実 D3D の要求どおり本物を用意する)
    D3D11_TEXTURE2D_DESC dtd = {};
    dtd.Width = kWidth;
    dtd.Height = kHeight;
    dtd.MipLevels = 1;
    dtd.ArraySize = 1;
    dtd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dtd.SampleDesc = { 1, 0 };
    dtd.Usage = D3D11_USAGE_DEFAULT;
    dtd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> depthTex;
    ComPtr<ID3D11DepthStencilView> dsv;
    ok = ok && SUCCEEDED(dev->CreateTexture2D(&dtd, nullptr, depthTex.GetAddressOf()));
    ok = ok && SUCCEEDED(dev->CreateDepthStencilView(depthTex.Get(), nullptr, dsv.GetAddressOf()));
    Check(ok, "RTV/DSV/staging を作成できる");
    if (!ok) {
        fs::remove_all(dir, ec);
        return;
    }

    RenderView view;
    XMStoreFloat4x4(&view.view,
                    XMMatrixLookAtLH(XMVectorSet(0, 0, 0, 1), XMVectorSet(0, 0, 1, 1),
                                     XMVectorSet(0, 1, 0, 0)));
    XMStoreFloat4x4(&view.proj, XMMatrixOrthographicLH(6.0f, 3.0f, 0.1f, 100.0f));
    XMStoreFloat4x4(&view.projNoJitter, XMLoadFloat4x4(&view.proj));
    view.width = static_cast<int>(kWidth);
    view.height = static_cast<int>(kHeight);
    view.rtv = colorRtv.Get();
    view.dsv = dsv.Get();
    view.instancingEnabled = 0;
    // 前フレーム履歴あり・カメラ静止 (速度は純粋にシェーダの gTime 変位だけに由来させる)
    XMStoreFloat4x4(&view.prevViewProj,
                    XMMatrixMultiply(XMLoadFloat4x4(&view.view), XMLoadFloat4x4(&view.projNoJitter)));
    view.prevViewProjValid = 1;
    view.viewFrameIndex = 30; // gTime = 30/60 = 0.5, prevTime = 29/60 (spec §2)

    // ambient を真っ黒にする — もしサーフェスが GBuffer 経由で光パスへ回っていたら
    // albedo * (ambient + lights) = 0 になり、固定色 (0.2,0.8,0.3) は絶対に出ない
    SceneLightData lights;
    lights.ambient = { 0.0f, 0.0f, 0.0f };
    lights.count = 0;

    auto makeItem = [&](AssetID mat, float x) {
        RenderItem item;
        item.mesh = quad;
        item.material = mat;
        XMStoreFloat4x4(&item.world, XMMatrixTranslation(x, 0.0f, 5.0f));
        item.prevWorld = item.world; // ワールド位置は動かさない — 速度はシェーダ変位だけの寄与
        return item;
    };

    RenderQueue queue;
    queue.opaque = { makeItem(displaceMatId, -2.0f), makeItem(rigidMatId, 2.0f) };
    dp.Render(device, view, queue, lights, resources, shaders);

    auto readColor = [&](int px, int py) {
        dc->CopyResource(colorStaging.Get(), colorTex.Get());
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        std::array<uint8_t, 4> out = { 0, 0, 0, 0 };
        if (SUCCEEDED(dc->Map(colorStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            const uint8_t* row =
                static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(py) * mapped.RowPitch;
            std::memcpy(out.data(), row + static_cast<size_t>(px) * 4, 4);
            dc->Unmap(colorStaging.Get(), 0);
        }
        return out;
    };
    auto closeTo = [](uint8_t a, uint8_t b) { return std::abs(static_cast<int>(a) - static_cast<int>(b)) <= 3; };

    const std::array<uint8_t, 4> displacePixel = readColor(16, 16);
    const std::array<uint8_t, 4> rigidPixel = readColor(80, 16);
    Check(closeTo(displacePixel[0], 51) && closeTo(displacePixel[1], 204) && closeTo(displacePixel[2], 76),
          "(b) 変位ありサーフェスの HDR 色が固定色 (0.2,0.8,0.3) のまま = GBuffer/光パスを経由していない"
          " (ambient=0 でも暗くならない)");
    Check(closeTo(rigidPixel[0], 51) && closeTo(rigidPixel[1], 204) && closeTo(rigidPixel[2], 76),
          "(b) 剛体サーフェスも同様に GBuffer をバイパスする");
    if (!(closeTo(displacePixel[0], 51) && closeTo(displacePixel[1], 204) && closeTo(displacePixel[2], 76))) {
        MYE_LOG_ERROR("    displacePixel=(%d,%d,%d,%d)", displacePixel[0], displacePixel[1], displacePixel[2],
                     displacePixel[3]);
    }

    // ---- (a) gbVelocity 読み戻し (R16G16_FLOAT = 4 バイト/テクセル、half float) ----
    ComPtr<ID3D11Resource> velRes;
    ID3D11ShaderResourceView* velSrv = dp.VelocitySRV();
    Check(velSrv != nullptr, "VelocitySRV が取得できる");
    std::array<float, 2> velDisplace = { 0, 0 };
    std::array<float, 2> velRigid = { 0, 0 };
    if (velSrv) {
        velSrv->GetResource(velRes.GetAddressOf());
        ComPtr<ID3D11Texture2D> velTex;
        ok = SUCCEEDED(velRes.As(&velTex));
        D3D11_TEXTURE2D_DESC vtd = {};
        if (ok) {
            velTex->GetDesc(&vtd);
        }
        D3D11_TEXTURE2D_DESC vStaged = vtd;
        vStaged.Usage = D3D11_USAGE_STAGING;
        vStaged.BindFlags = 0;
        vStaged.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> velStaging;
        ok = ok && SUCCEEDED(dev->CreateTexture2D(&vStaged, nullptr, velStaging.GetAddressOf()));
        Check(ok, "gbVelocity staging を作成できる");
        if (ok) {
            dc->CopyResource(velStaging.Get(), velTex.Get());
            D3D11_MAPPED_SUBRESOURCE mapped = {};
            auto readVel = [&](int px, int py) -> std::array<float, 2> {
                std::array<float, 2> out = { 0, 0 };
                if (SUCCEEDED(dc->Map(velStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                    const uint8_t* row = static_cast<const uint8_t*>(mapped.pData)
                        + static_cast<size_t>(py) * mapped.RowPitch;
                    uint16_t rh, gh;
                    std::memcpy(&rh, row + static_cast<size_t>(px) * 4, 2);
                    std::memcpy(&gh, row + static_cast<size_t>(px) * 4 + 2, 2);
                    out = { HalfToFloat(rh), HalfToFloat(gh) };
                    dc->Unmap(velStaging.Get(), 0);
                }
                return out;
            };
            velDisplace = readVel(16, 16);
            velRigid = readVel(80, 16);
        }
    }
    const float velDisplaceMag = std::sqrt(velDisplace[0] * velDisplace[0] + velDisplace[1] * velDisplace[1]);
    const float velRigidMag = std::sqrt(velRigid[0] * velRigid[0] + velRigid[1] * velRigid[1]);
    Check(velDisplaceMag > 0.0005f,
          "(a) gTime 駆動の頂点変位アイテムは gbVelocity が非 0 (world は動いていないのでシェーダ変位のみに由来)");
    Check(velRigidMag < 1e-6f, "(a) 変位なしアイテムは gbVelocity が厳密に 0");
    if (!(velDisplaceMag > 0.0005f)) {
        MYE_LOG_ERROR("    velDisplace=(%f,%f) mag=%f", velDisplace[0], velDisplace[1], velDisplaceMag);
    }

    dp.Shutdown();
    fs::remove_all(dir, ec);
}

// ---- (c): ShadowPass の影エントリ。gTime 変位ありのサーフェスと剛体のサーフェスとで
//      CSM シャドウマップの深度値が異なることを、真上から見下ろす正射影ライトで確認する ----
void TestShadowPassDepthReflectsDisplacement(GraphicsDevice& device, const std::wstring& engineShaderDir)
{
    MYE_LOG_INFO("-- ShadowPass: 影エントリの深度が gTime 変位で変わるか --");
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / L"mye_surface_shadow_selftest";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    WriteFile(dir / L"DisplaceProbe.surface.hlsl", kDisplaceSurface);
    WriteFile(dir / L"RigidProbe.surface.hlsl", kRigidSurface);
    WriteFile(dir / L"Displace.mat.json", "{\"shader\":\"DisplaceProbe.surface\"}");
    WriteFile(dir / L"Rigid.mat.json", "{\"shader\":\"RigidProbe.surface\"}");

    ShaderManager shaders;
    Check(shaders.Init(device, { dir.wstring(), engineShaderDir }), "shader manager init (shadow)");

    RenderResources resources;
    resources.Init(device);
    const AssetID displaceMatId = resources.materials.LoadFromFile(
        (dir / L"Displace.mat.json").wstring(), resources.textures, dir.wstring());
    const AssetID rigidMatId = resources.materials.LoadFromFile(
        (dir / L"Rigid.mat.json").wstring(), resources.textures, dir.wstring());
    Check(!displaceMatId.IsNull() && !rigidMatId.IsNull(), "2 本の .mat.json が読み込める (shadow)");

    ShadowPass sp;
    constexpr int kRes = 128;
    Check(sp.Init(device, shaders, kRes), "ShadowPass::Init");

    // Quad (Z 面) は真上からの正射影ライトだと厚み 0 の板が縁だけを見せてしまい
    // (footprint がほぼ 1 直線になる)、shadow map 上でほぼ何も覆わない。
    // Cube なら X/Z にも実際の厚みがあるので、真上から見ても正しい footprint が乗る
    const AssetID cube = resources.meshes.Cube();
    RenderQueue queue;
    auto makeItem = [&](AssetID mat, float x) {
        RenderItem item;
        item.mesh = cube;
        item.material = mat;
        XMStoreFloat4x4(&item.world, XMMatrixTranslation(x, 0.0f, 0.0f));
        item.prevWorld = item.world;
        return item;
    };
    queue.opaque = { makeItem(displaceMatId, -1.0f), makeItem(rigidMatId, 1.0f) };

    // 真上 (y=10) から -Y を見下ろす正射影ライト。up は +Z (dir と平行でない軸を選ぶ)。
    // これで NDC 深度は世界の Y 位置だけに単調対応し、gTime 変位の有無が深度差として直接読める
    const XMMATRIX lightView =
        XMMatrixLookToLH(XMVectorSet(0, 10, 0, 1), XMVectorSet(0, -1, 0, 0), XMVectorSet(0, 0, 1, 0));
    const XMMATRIX lightProj = XMMatrixOrthographicLH(4.0f, 4.0f, 0.1f, 20.0f);
    XMFLOAT4X4 lightViewProj;
    XMStoreFloat4x4(&lightViewProj, XMMatrixMultiply(lightView, lightProj));

    sp.Render(device, shaders, queue, resources, &lightViewProj, /*count=*/1, /*viewFrameIndex=*/30,
              /*instancing=*/true);

    ID3D11ShaderResourceView* shadowSrv = sp.SRV();
    Check(shadowSrv != nullptr, "ShadowPass::SRV が取得できる");
    float depthDisplace = 0.0f;
    float depthRigid = 0.0f;
    if (shadowSrv) {
        ComPtr<ID3D11Resource> shadowRes;
        shadowSrv->GetResource(shadowRes.GetAddressOf());
        ComPtr<ID3D11Texture2D> shadowTex;
        bool ok = SUCCEEDED(shadowRes.As(&shadowTex));
        D3D11_TEXTURE2D_DESC td = {};
        if (ok) {
            shadowTex->GetDesc(&td);
        }
        // シャドウテクスチャは TYPELESS (R32_TYPELESS) — ステージングは R32_FLOAT に読み替えて Copy する
        D3D11_TEXTURE2D_DESC staged = td;
        staged.Format = DXGI_FORMAT_R32_FLOAT;
        staged.Usage = D3D11_USAGE_STAGING;
        staged.BindFlags = 0;
        staged.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        ok = ok && SUCCEEDED(device.Device()->CreateTexture2D(&staged, nullptr, staging.GetAddressOf()));
        Check(ok, "シャドウマップの staging を作成できる");
        if (ok) {
            ID3D11DeviceContext* dc = device.Context();
            dc->CopyResource(staging.Get(), shadowTex.Get());
            D3D11_MAPPED_SUBRESOURCE mapped = {};
            // カスケード 0 (先頭スライス) だけを読む。x=-1/+1 は ortho width=4 の中心付近 →
            // 画素 32 (u=0.25) / 96 (u=0.75)、v は両アイテムとも z=0 で同じ (中心 = 64)
            auto readDepth = [&](int px, int py) {
                float v = 0.0f;
                if (SUCCEEDED(dc->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                    const uint8_t* row = static_cast<const uint8_t*>(mapped.pData)
                        + static_cast<size_t>(py) * mapped.RowPitch;
                    std::memcpy(&v, row + static_cast<size_t>(px) * 4, 4);
                    dc->Unmap(staging.Get(), 0);
                }
                return v;
            };
            const int u1 = static_cast<int>((-1.0f / 2.0f * 0.5f + 0.5f) * kRes); // x=-1 -> 32
            const int u2 = static_cast<int>((1.0f / 2.0f * 0.5f + 0.5f) * kRes);  // x=+1 -> 96
            depthDisplace = readDepth(u1, kRes / 2);
            depthRigid = readDepth(u2, kRes / 2);
        }
    }
    Check(depthDisplace != 0.0f && depthRigid != 0.0f, "2 本とも影を落としている (深度が既定の 0 のままではない)");
    Check(std::fabs(depthDisplace - depthRigid) > 1e-4f,
          "(c) CSM シャドウマップの深度が gTime 変位あり/なしで異なる (影エントリが VSMain の変位を反映する)");
    // 変位ありは y=+0.5 (光源に近い) なので、標準的な LH 正射影 (近=0,遠=1) では深度が小さくなるはず。
    // 符号が逆でも「異なる」こと自体は上のチェックで確認済みなので、ここは参考ログに留める
    if (depthDisplace >= depthRigid) {
        MYE_LOG_WARN("    符号が期待と逆: depthDisplace=%f depthRigid=%f (差異自体は検出済み)",
                     depthDisplace, depthRigid);
    }

    fs::remove_all(dir, ec);
}

// ---- review-1 #1 (M79c-fix): ShadowPass の混在順序回帰。サーフェスの影エントリは名前解決で
//      VS の CB/SRV を任意スロットへ張るため、直後に描く非サーフェス (通常 + instanced run) が
//      期待する固定スロット (VS b0 = objectCB_、VS t0 = instance SRV) を戻さないと、その非サーフェス
//      アイテムが壊れた行列/SRV を読んで影を落とせなくなる (再現: rv1\shadowA.png / vtexA_def.png)。
//      「サーフェス→非サーフェス」「非サーフェス→サーフェス」の 2 順序で、非サーフェス側
//      (instanced run) の深度が完全一致することを確認する ----
void TestShadowPassFixedSlotsSurviveSurfaceEntry(GraphicsDevice& device, const std::wstring& engineShaderDir)
{
    MYE_LOG_INFO("-- ShadowPass: サーフェス描画後も VS b0(objectCB_)/t0(instance SRV) が非サーフェスへ戻るか --");
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / L"mye_surface_shadow_order_selftest";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    WriteFile(dir / L"VTexProbe.surface.hlsl", kVTexSurface);
    WriteFile(dir / L"VTexProbe.mat.json", "{\"shader\":\"VTexProbe.surface\"}");

    ShaderManager shaders;
    Check(shaders.Init(device, { dir.wstring(), engineShaderDir }), "shader manager init (shadow order)");

    RenderResources resources;
    resources.Init(device);
    const AssetID surfMatId = resources.materials.LoadFromFile(
        (dir / L"VTexProbe.mat.json").wstring(), resources.textures, dir.wstring());
    Check(!surfMatId.IsNull(), "VTexProbe.mat.json が読み込める");

    // 非サーフェスの通常マテリアル。ShadowPass はマテリアルの中身を読まないので実体は不要
    // (SurfaceMaterialSelfTest の「forward_lit マテリアルは対象外」と同じ組み方)
    Material plain;
    plain.shader = AssetID{ HashStr("forward_lit") };
    const AssetID plainMatId = resources.materials.Register("shadow_order_plain", plain);

    ShadowPass sp;
    constexpr int kRes = 128;
    Check(sp.Init(device, shaders, kRes), "ShadowPass::Init (order)");

    const AssetID cube = resources.meshes.Cube();
    auto makeItem = [&](AssetID mat, float x) {
        RenderItem item;
        item.mesh = cube;
        item.material = mat;
        XMStoreFloat4x4(&item.world, XMMatrixTranslation(x, 0.0f, 0.0f));
        item.prevWorld = item.world;
        return item;
    };

    // 真上 (y=10) から見下ろす正射影ライト (TestShadowPassDepthReflectsDisplacement と同じ組み方)
    const XMMATRIX lightView =
        XMMatrixLookToLH(XMVectorSet(0, 10, 0, 1), XMVectorSet(0, -1, 0, 0), XMVectorSet(0, 0, 1, 0));
    const XMMATRIX lightProj = XMMatrixOrthographicLH(8.0f, 8.0f, 0.1f, 20.0f);
    XMFLOAT4X4 lightViewProj;
    XMStoreFloat4x4(&lightViewProj, XMMatrixMultiply(lightView, lightProj));

    auto readDepthAt = [&](float worldX) -> float {
        ID3D11ShaderResourceView* shadowSrv = sp.SRV();
        if (!shadowSrv) {
            return -1.0f;
        }
        ComPtr<ID3D11Resource> shadowRes;
        shadowSrv->GetResource(shadowRes.GetAddressOf());
        ComPtr<ID3D11Texture2D> shadowTex;
        if (FAILED(shadowRes.As(&shadowTex))) {
            return -1.0f;
        }
        D3D11_TEXTURE2D_DESC td = {};
        shadowTex->GetDesc(&td);
        D3D11_TEXTURE2D_DESC staged = td;
        staged.Format = DXGI_FORMAT_R32_FLOAT; // シャドウテクスチャは TYPELESS
        staged.Usage = D3D11_USAGE_STAGING;
        staged.BindFlags = 0;
        staged.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(device.Device()->CreateTexture2D(&staged, nullptr, staging.GetAddressOf()))) {
            return -1.0f;
        }
        ID3D11DeviceContext* dc = device.Context();
        dc->CopyResource(staging.Get(), shadowTex.Get());
        // ortho width=8 中心 0 → u = worldX/width + 0.5 (TestShadowPassDepthReflectsDisplacement と同じ式)
        const int px = static_cast<int>((worldX / 8.0f + 0.5f) * kRes);
        const int py = kRes / 2;
        float v = -1.0f;
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (SUCCEEDED(dc->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            const uint8_t* row =
                static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(py) * mapped.RowPitch;
            std::memcpy(&v, row + static_cast<size_t>(px) * 4, 4);
            dc->Unmap(staging.Get(), 0);
        }
        return v;
    };

    constexpr float kClearDepth = 1.0f; // ShadowPass::Render の ClearDepthStencilView と同じ既定値

    // 順序 A: サーフェス → 非サーフェス instanced run 2 個 (rv1\shadowA.png の再現方向)
    RenderQueue queueA;
    queueA.opaque = { makeItem(surfMatId, -3.0f), makeItem(plainMatId, 0.0f), makeItem(plainMatId, 3.0f) };
    sp.Render(device, shaders, queueA, resources, &lightViewProj, /*count=*/1, /*viewFrameIndex=*/30,
              /*instancing=*/true);
    const float depthAfterA0 = readDepthAt(0.0f);
    const float depthAfterA3 = readDepthAt(3.0f);

    // 順序 B: 非サーフェス instanced run 2 個 → サーフェス (rv1\shadowB.png の再現方向、基準値)
    RenderQueue queueB;
    queueB.opaque = { makeItem(plainMatId, 0.0f), makeItem(plainMatId, 3.0f), makeItem(surfMatId, -3.0f) };
    sp.Render(device, shaders, queueB, resources, &lightViewProj, /*count=*/1, /*viewFrameIndex=*/30,
              /*instancing=*/true);
    const float depthBeforeB0 = readDepthAt(0.0f);
    const float depthBeforeB3 = readDepthAt(3.0f);

    Check(std::fabs(depthBeforeB0 - kClearDepth) > 1e-4f && std::fabs(depthBeforeB3 - kClearDepth) > 1e-4f,
          "順序 B (サーフェスが後) では非サーフェス instanced run の影が正しく落ちる (基準値)");
    Check(std::fabs(depthAfterA0 - kClearDepth) > 1e-4f && std::fabs(depthAfterA3 - kClearDepth) > 1e-4f,
          "(review-1 #1) 順序 A (サーフェスが先) でも非サーフェス instanced run の影が消えない");
    Check(std::fabs(depthAfterA0 - depthBeforeB0) < 1e-6f && std::fabs(depthAfterA3 - depthBeforeB3) < 1e-6f,
          "(review-1 #1) 順序 A と順序 B で非サーフェス instanced run の深度が完全一致する"
          " (b0/objectCB_・t0/instance SRV がサーフェス描画後に復元されている)");
    if (!(std::fabs(depthAfterA0 - depthBeforeB0) < 1e-6f && std::fabs(depthAfterA3 - depthBeforeB3) < 1e-6f)) {
        MYE_LOG_ERROR("    depthAfterA0=%f depthBeforeB0=%f depthAfterA3=%f depthBeforeB3=%f", depthAfterA0,
                     depthBeforeB0, depthAfterA3, depthBeforeB3);
    }

    fs::remove_all(dir, ec);
}

// ---- review-1 #1 の補強: DeferredPath のサーフェス段 (2.65 RenderSurfaceForward) の後に続く
//      水面 (2.7 water_.Render) / 透明後段 (3 RenderTransparent) は、どちらも自分の描画に要る
//      固定スロットを呼び出しの都度自分で張り直す設計 (WaterPass.cpp の perFrameCB 再バインド、
//      DeferredPath.cpp の bindForwardLitFixed) だが、それを「サーフェス段の有無で水面・透明の
//      絵が変わらない」という観測可能な形で固定する。画面上の別々の位置に
//      distractor サーフェス (VS テクスチャ付き) / 透明アイテム / 水面のみの読み取り点を置き、
//      distractor の有無で後 2 者の画素が完全一致することを確認する (絶対色は問わない) ----
void TestDeferredWaterAndTransparentUnaffectedBySurfaceForwardStep(GraphicsDevice& device,
                                                                   const std::wstring& engineShaderDir)
{
    MYE_LOG_INFO("-- DeferredPath: サーフェス段 (2.65) の後でも水面 (2.7) / 透明後段 (3) の絵が変わらないか --");
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / L"mye_surface_deferred_water_selftest";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    WriteFile(dir / L"VTexProbe.surface.hlsl", kVTexSurface);
    WriteFile(dir / L"VTexProbe.mat.json", "{\"shader\":\"VTexProbe.surface\"}");

    ShaderManager shaders;
    Check(shaders.Init(device, { dir.wstring(), engineShaderDir }), "shader manager init (water order)");
    const AssetID litShaderId = shaders.Load("forward_lit");
    Check(shaders.Get(litShaderId) != nullptr && shaders.Get(litShaderId)->valid, "forward_lit がロードできる");

    RenderResources resources;
    resources.Init(device);
    const AssetID surfMatId = resources.materials.LoadFromFile(
        (dir / L"VTexProbe.mat.json").wstring(), resources.textures, dir.wstring());
    Check(!surfMatId.IsNull(), "VTexProbe.mat.json が読み込める (water order)");

    Material transMat;
    transMat.shader = litShaderId;
    transMat.texture = resources.textures.White();
    transMat.baseColor = { 0.1f, 0.2f, 0.9f, 0.5f };
    const AssetID transMatId = resources.materials.Register("water_order_transparent", transMat);

    DeferredPath dp;
    Check(dp.Init(device, shaders), "DeferredPath::Init (water order)");

    const AssetID cube = resources.meshes.Cube();

    constexpr UINT kWidth = 128;
    constexpr UINT kHeight = 128;
    ID3D11Device* dev = device.Device();
    ID3D11DeviceContext* dc = device.Context();

    D3D11_TEXTURE2D_DESC ctd = {};
    ctd.Width = kWidth;
    ctd.Height = kHeight;
    ctd.MipLevels = 1;
    ctd.ArraySize = 1;
    ctd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    ctd.SampleDesc = { 1, 0 };
    ctd.Usage = D3D11_USAGE_DEFAULT;
    ctd.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> colorTex;
    ComPtr<ID3D11RenderTargetView> colorRtv;
    bool ok = SUCCEEDED(dev->CreateTexture2D(&ctd, nullptr, colorTex.GetAddressOf()));
    ok = ok && SUCCEEDED(dev->CreateRenderTargetView(colorTex.Get(), nullptr, colorRtv.GetAddressOf()));
    D3D11_TEXTURE2D_DESC staged = ctd;
    staged.Usage = D3D11_USAGE_STAGING;
    staged.BindFlags = 0;
    staged.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> colorStaging;
    ok = ok && SUCCEEDED(dev->CreateTexture2D(&staged, nullptr, colorStaging.GetAddressOf()));

    D3D11_TEXTURE2D_DESC dtd = {};
    dtd.Width = kWidth;
    dtd.Height = kHeight;
    dtd.MipLevels = 1;
    dtd.ArraySize = 1;
    dtd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dtd.SampleDesc = { 1, 0 };
    dtd.Usage = D3D11_USAGE_DEFAULT;
    dtd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> depthTex;
    ComPtr<ID3D11DepthStencilView> dsv;
    ok = ok && SUCCEEDED(dev->CreateTexture2D(&dtd, nullptr, depthTex.GetAddressOf()));
    ok = ok && SUCCEEDED(dev->CreateDepthStencilView(depthTex.Get(), nullptr, dsv.GetAddressOf()));
    Check(ok, "RTV/DSV/staging を作成できる (water order)");
    if (!ok) {
        fs::remove_all(dir, ec);
        return;
    }

    // 真上 (y=8) から見下ろす正射影カメラ。x=-4 に distractor サーフェス、x=0 に透明キューブ、
    // x=+4 は何も置かず水面だけを読む (TestShadowPassFixedSlotsSurviveSurfaceEntry と同じ発想)
    RenderView view;
    XMStoreFloat4x4(&view.view,
                    XMMatrixLookToLH(XMVectorSet(0, 8, 0, 1), XMVectorSet(0, -1, 0, 0), XMVectorSet(0, 0, 1, 0)));
    XMStoreFloat4x4(&view.proj, XMMatrixOrthographicLH(12.0f, 12.0f, 0.1f, 50.0f));
    XMStoreFloat4x4(&view.projNoJitter, XMLoadFloat4x4(&view.proj));
    view.width = static_cast<int>(kWidth);
    view.height = static_cast<int>(kHeight);
    view.rtv = colorRtv.Get();
    view.dsv = dsv.Get();
    view.instancingEnabled = 0;
    view.viewFrameIndex = 30;
    XMStoreFloat4x4(&view.prevViewProj,
                    XMMatrixMultiply(XMLoadFloat4x4(&view.view), XMLoadFloat4x4(&view.projNoJitter)));
    view.prevViewProjValid = 1;

    SceneLightData lights;
    lights.ambient = { 0.3f, 0.3f, 0.3f };
    lights.count = 0;

    WaterDrawData water;
    water.active = true;
    water.useSurfaceRoute = false;
    water.curWaterTime = 0.5f;
    water.prevWaterTime = 0.4f;
    view.water = &water;

    auto makeCubeItem = [&](AssetID mat, float x) {
        RenderItem item;
        item.mesh = cube;
        item.material = mat;
        XMStoreFloat4x4(&item.world, XMMatrixTranslation(x, 0.5f, 0.0f));
        item.prevWorld = item.world;
        return item;
    };

    auto readColor = [&](int px, int py) {
        dc->CopyResource(colorStaging.Get(), colorTex.Get());
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        std::array<uint8_t, 4> out = { 0, 0, 0, 0 };
        if (SUCCEEDED(dc->Map(colorStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            const uint8_t* row =
                static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(py) * mapped.RowPitch;
            std::memcpy(out.data(), row + static_cast<size_t>(px) * 4, 4);
            dc->Unmap(colorStaging.Get(), 0);
        }
        return out;
    };
    // u = worldX/width + 0.5 (TestShadowPassFixedSlotsSurviveSurfaceEntry と同じ式、width=12)
    const int pxTransparent = static_cast<int>((0.0f / 12.0f + 0.5f) * kWidth);
    const int pxWaterOnly = static_cast<int>((4.0f / 12.0f + 0.5f) * kWidth);
    const int py = static_cast<int>(kHeight) / 2;

    // 順序 A (基準値): distractor サーフェスなし — 透明キューブ + 水面のみ
    RenderQueue queueBase;
    queueBase.transparent = { makeCubeItem(transMatId, 0.0f) };
    dp.Render(device, view, queueBase, lights, resources, shaders);
    const std::array<uint8_t, 4> transparentBase = readColor(pxTransparent, py);
    const std::array<uint8_t, 4> waterBase = readColor(pxWaterOnly, py);

    // 順序 B: 同じシーンに distractor サーフェス (x=-4、画面上の別位置) を追加
    RenderQueue queueWith;
    queueWith.opaque = { makeCubeItem(surfMatId, -4.0f) };
    queueWith.transparent = { makeCubeItem(transMatId, 0.0f) };
    dp.Render(device, view, queueWith, lights, resources, shaders);
    const std::array<uint8_t, 4> transparentWith = readColor(pxTransparent, py);
    const std::array<uint8_t, 4> waterWith = readColor(pxWaterOnly, py);

    Check(waterBase[0] != 0 || waterBase[1] != 0 || waterBase[2] != 0,
          "水面のみの画素が黒でない (WaterPass::Render が実際に描いている)");
    Check(transparentBase != waterBase,
          "透明キューブの画素は水面のみの画素と異なる (透明キューブが実際に描かれている)");
    Check(transparentWith == transparentBase,
          "(review-1 #1 補強) distractor サーフェスの有無で透明後段の画素が変わらない");
    Check(waterWith == waterBase,
          "(review-1 #1 補強) distractor サーフェスの有無で水面の画素が変わらない");
    if (transparentWith != transparentBase || waterWith != waterBase) {
        MYE_LOG_ERROR("    transparentBase=(%d,%d,%d,%d) transparentWith=(%d,%d,%d,%d)", transparentBase[0],
                     transparentBase[1], transparentBase[2], transparentBase[3], transparentWith[0],
                     transparentWith[1], transparentWith[2], transparentWith[3]);
        MYE_LOG_ERROR("    waterBase=(%d,%d,%d,%d) waterWith=(%d,%d,%d,%d)", waterBase[0], waterBase[1],
                     waterBase[2], waterBase[3], waterWith[0], waterWith[1], waterWith[2], waterWith[3]);
    }

    dp.Shutdown();
    fs::remove_all(dir, ec);
}

// ---- (e): M79 sub-05 round 2 — Deferred の透明段がサーフェスマテリアルの色エントリを描くこと。
//      未修正時は `shaders.Get(mat->shader)` が `*.surface` 短名を解決できず `continue` で
//      黙って消えていた (round 1 VERDICT の指摘)。ここでは
//      (1) 正常なサーフェス透明マテリアルの色が背景と正しくアルファブレンドされること
//      (2) 存在しないシェーダを参照する透明マテリアルは surface_error (マゼンタ、alpha=1) に
//          フォールバックし、黙って消えないこと
//      を read-back で確認する ----
void TestDeferredTransparentDrawsSurfaceColorEntry(GraphicsDevice& device, const std::wstring& engineShaderDir)
{
    MYE_LOG_INFO("-- DeferredPath: 透明段がサーフェスの色エントリを描く (M79 sub-05 round 2) --");
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / L"mye_surface_deferred_transparent_selftest";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    WriteFile(dir / L"TransparentGood.surface.hlsl", kTransparentColorSurface);
    WriteFile(dir / L"TransparentGood.mat.json", "{\"shader\":\"TransparentGood.surface\",\"transparent\":true}");
    // 対応する .hlsl を置かない = LoadSurface が失敗し surface_error へフォールバックするはずの材質
    WriteFile(dir / L"TransparentBroken.mat.json",
             "{\"shader\":\"NoSuchShader.surface\",\"transparent\":true}");

    ShaderManager shaders;
    Check(shaders.Init(device, { dir.wstring(), engineShaderDir }), "shader manager init (transparent)");

    RenderResources resources;
    resources.Init(device);
    const AssetID goodMatId = resources.materials.LoadFromFile(
        (dir / L"TransparentGood.mat.json").wstring(), resources.textures, dir.wstring());
    const AssetID brokenMatId = resources.materials.LoadFromFile(
        (dir / L"TransparentBroken.mat.json").wstring(), resources.textures, dir.wstring());
    Check(!goodMatId.IsNull() && !brokenMatId.IsNull(), "2 本の透明 .mat.json が読み込める");

    DeferredPath dp;
    Check(dp.Init(device, shaders), "DeferredPath::Init (transparent)");

    const AssetID quad = resources.meshes.Quad();

    constexpr UINT kWidth = 96;
    constexpr UINT kHeight = 32;
    ID3D11Device* dev = device.Device();
    ID3D11DeviceContext* dc = device.Context();

    D3D11_TEXTURE2D_DESC ctd = {};
    ctd.Width = kWidth;
    ctd.Height = kHeight;
    ctd.MipLevels = 1;
    ctd.ArraySize = 1;
    ctd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    ctd.SampleDesc = { 1, 0 };
    ctd.Usage = D3D11_USAGE_DEFAULT;
    ctd.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> colorTex;
    ComPtr<ID3D11RenderTargetView> colorRtv;
    bool ok = SUCCEEDED(dev->CreateTexture2D(&ctd, nullptr, colorTex.GetAddressOf()));
    ok = ok && SUCCEEDED(dev->CreateRenderTargetView(colorTex.Get(), nullptr, colorRtv.GetAddressOf()));
    D3D11_TEXTURE2D_DESC staged = ctd;
    staged.Usage = D3D11_USAGE_STAGING;
    staged.BindFlags = 0;
    staged.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> colorStaging;
    ok = ok && SUCCEEDED(dev->CreateTexture2D(&staged, nullptr, colorStaging.GetAddressOf()));

    D3D11_TEXTURE2D_DESC dtd = {};
    dtd.Width = kWidth;
    dtd.Height = kHeight;
    dtd.MipLevels = 1;
    dtd.ArraySize = 1;
    dtd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dtd.SampleDesc = { 1, 0 };
    dtd.Usage = D3D11_USAGE_DEFAULT;
    dtd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> depthTex;
    ComPtr<ID3D11DepthStencilView> dsv;
    ok = ok && SUCCEEDED(dev->CreateTexture2D(&dtd, nullptr, depthTex.GetAddressOf()));
    ok = ok && SUCCEEDED(dev->CreateDepthStencilView(depthTex.Get(), nullptr, dsv.GetAddressOf()));
    Check(ok, "RTV/DSV/staging を作成できる (transparent)");
    if (!ok) {
        fs::remove_all(dir, ec);
        return;
    }

    RenderView view;
    XMStoreFloat4x4(&view.view, XMMatrixLookAtLH(XMVectorSet(0, 0, 0, 1), XMVectorSet(0, 0, 1, 1),
                                                 XMVectorSet(0, 1, 0, 0)));
    XMStoreFloat4x4(&view.proj, XMMatrixOrthographicLH(6.0f, 3.0f, 0.1f, 100.0f));
    XMStoreFloat4x4(&view.projNoJitter, XMLoadFloat4x4(&view.proj));
    view.width = static_cast<int>(kWidth);
    view.height = static_cast<int>(kHeight);
    view.rtv = colorRtv.Get();
    view.dsv = dsv.Get();
    view.instancingEnabled = 0;

    SceneLightData lights;
    lights.ambient = { 0.0f, 0.0f, 0.0f };
    lights.count = 0;

    auto makeItem = [&](AssetID mat, float x) {
        RenderItem item;
        item.mesh = quad;
        item.material = mat;
        XMStoreFloat4x4(&item.world, XMMatrixTranslation(x, 0.0f, 5.0f));
        item.prevWorld = item.world;
        return item;
    };

    RenderQueue queue;
    queue.transparent = { makeItem(goodMatId, -2.0f), makeItem(brokenMatId, 2.0f) };
    dp.Render(device, view, queue, lights, resources, shaders);

    auto readColor = [&](int px, int py) {
        dc->CopyResource(colorStaging.Get(), colorTex.Get());
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        std::array<uint8_t, 4> out = { 0, 0, 0, 0 };
        if (SUCCEEDED(dc->Map(colorStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            const uint8_t* row =
                static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(py) * mapped.RowPitch;
            std::memcpy(out.data(), row + static_cast<size_t>(px) * 4, 4);
            dc->Unmap(colorStaging.Get(), 0);
        }
        return out;
    };
    auto closeTo = [](int a, int b) { return std::abs(a - b) <= 4; };

    const std::array<uint8_t, 4> backgroundPixel = readColor(48, 16); // 2 アイテムの中間 (どちらにも覆われない)
    const std::array<uint8_t, 4> goodPixel = readColor(16, 16);
    const std::array<uint8_t, 4> brokenPixel = readColor(80, 16);

    // 期待値 = src*srcA + dst*(1-srcA) (blendAlpha_ の SRC_ALPHA/INV_SRC_ALPHA、DeferredPath.cpp Init 参照)。
    // 背景の実測値を dst として使うので、背景色そのもの (スカイ/ambient の式) には依存しない
    const float srcA = 0.5f;
    const int expectedR = static_cast<int>(0.0f * 255.0f * srcA + backgroundPixel[0] * (1.0f - srcA));
    const int expectedG = static_cast<int>(0.8f * 255.0f * srcA + backgroundPixel[1] * (1.0f - srcA));
    const int expectedB = static_cast<int>(0.0f * 255.0f * srcA + backgroundPixel[2] * (1.0f - srcA));
    Check(closeTo(goodPixel[0], expectedR) && closeTo(goodPixel[1], expectedG) && closeTo(goodPixel[2], expectedB),
          "(1) 透明サーフェスの色エントリが背景と正しくアルファブレンドされる (黙って消えない)");
    if (!(closeTo(goodPixel[0], expectedR) && closeTo(goodPixel[1], expectedG) && closeTo(goodPixel[2], expectedB))) {
        MYE_LOG_ERROR("    goodPixel=(%d,%d,%d) expected=(%d,%d,%d) background=(%d,%d,%d)", goodPixel[0],
                     goodPixel[1], goodPixel[2], expectedR, expectedG, expectedB, backgroundPixel[0],
                     backgroundPixel[1], backgroundPixel[2]);
    }

    Check(closeTo(brokenPixel[0], 255) && brokenPixel[1] <= 4 && closeTo(brokenPixel[2], 255),
          "(2) 存在しないシェーダを参照する透明サーフェスは surface_error (マゼンタ、alpha=1) になる");
    if (!(closeTo(brokenPixel[0], 255) && brokenPixel[1] <= 4 && closeTo(brokenPixel[2], 255))) {
        MYE_LOG_ERROR("    brokenPixel=(%d,%d,%d)", brokenPixel[0], brokenPixel[1], brokenPixel[2]);
    }

    dp.Shutdown();
    fs::remove_all(dir, ec);
}

} // namespace

bool RunSurfaceDeferredSelfTest()
{
    MYE_LOG_INFO("==== M79 surface deferred self test (sub-03) ====");
    g_fails = 0;

    GraphicsDevice device;
    if (!device.Init(true)) {
        MYE_LOG_ERROR("  FAIL: WARP device init");
        return false;
    }
    const std::wstring engineShaderDir = FindEngineShaderDir();
    Check(!engineShaderDir.empty(), "engine shader dir found");
    if (engineShaderDir.empty()) {
        return false;
    }

    TestDeferredForwardStepVelocityAndBypass(device, engineShaderDir);
    TestShadowPassDepthReflectsDisplacement(device, engineShaderDir);
    TestShadowPassFixedSlotsSurviveSurfaceEntry(device, engineShaderDir); // review-1 #1 (M79c-fix)
    TestDeferredWaterAndTransparentUnaffectedBySurfaceForwardStep(device, engineShaderDir); // review-1 #1 補強
    TestDeferredTransparentDrawsSurfaceColorEntry(device, engineShaderDir); // M79 sub-05 round 2

    // (d) サーフェス 0 件のときフォワード段が何も張らないことは、この自己テストの範囲では
    // 「既存 golden (--selftest 全体 / tools\replay_verify.bat の既定シーン群、いずれもサーフェス
    // 材質を含まない) が本サブの変更前後でビット一致する」ことで担保している (round 1 で確認済み、
    // sub-03.md 実装メモ参照)。この専用テストにサーフェス 0 件のケースを重複して足さない

    MYE_LOG_INFO("==== M79 surface deferred self test: %s ====", (g_fails == 0) ? "ALL PASS" : "FAILED");
    return g_fails == 0;
}

} // namespace mye
