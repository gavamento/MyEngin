//====================================================================================
//                          WaterSurfaceSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                     09/24/2026
//                                          M79 sub-05: 水面サーフェス経路の回帰テスト実装
//====================================================================================
#include "Engine/Renderer/WaterSurfaceSelfTest.h"

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

#include "Engine/Core/Log.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/DeferredPath.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/GraphicsDevice.h"
#include "Engine/Renderer/ShaderManager.h"
#include "Engine/Renderer/ShadowPass.h"
#include "Engine/Renderer/WaterPass.h"

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

// IEEE754 half (R16G16_FLOAT が並べるビット列) → float (SurfaceDeferredSelfTest.cpp と同じ実装)
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

// gWaterTime (gTime ではない) 駆動の頂点変位 + MyEngineWater.deepColor をそのまま出す色。
// 「WaterWave と同じ時計 (gWaterTime) を使うと速度・影に反映される」ことの最小フィクスチャ
const char* kWaterProbeSurfaceHlsl = R"HLSL(
#include "MyEngineSurface.hlsli"
struct VSIn { float3 pos : POSITION; };
struct VSOut { float4 pos : SV_Position; };
VSOut VSMain(VSIn v)
{
    VSOut o;
    float3 p = v.pos;
    // 変位は小さく保つ (メッシュの footprint から read-back 画素が外れないように。
    // SurfaceDeferredSelfTest.cpp で実際に踏んだ罠と同じ配慮)
    p.y += gWaterTime * 0.15f;
    float4 worldPos = mul(float4(p, 1.0f), gWorld);
    o.pos = mul(worldPos, gViewProj);
    return o;
}
float4 PSMain(VSOut i) : SV_Target
{
    return gMyeWaterDeepColor;
}
)HLSL";

std::array<uint8_t, 4> ReadRgba(ID3D11DeviceContext* dc, ID3D11Texture2D* tex, ID3D11Texture2D* staging,
                                int px, int py)
{
    dc->CopyResource(staging, tex);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    std::array<uint8_t, 4> out = { 0, 0, 0, 0 };
    if (SUCCEEDED(dc->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        const uint8_t* row =
            static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(py) * mapped.RowPitch;
        std::memcpy(out.data(), row + static_cast<size_t>(px) * 4, 4);
        dc->Unmap(staging, 0);
    }
    return out;
}

XMFLOAT2 ReadVelocity(ID3D11DeviceContext* dc, ID3D11Device* dev, ID3D11ShaderResourceView* velSrv,
                      int px, int py)
{
    XMFLOAT2 out = { 0.0f, 0.0f };
    if (!velSrv) {
        return out;
    }
    ComPtr<ID3D11Resource> velRes;
    velSrv->GetResource(velRes.GetAddressOf());
    ComPtr<ID3D11Texture2D> velTex;
    if (FAILED(velRes.As(&velTex))) {
        return out;
    }
    D3D11_TEXTURE2D_DESC vtd = {};
    velTex->GetDesc(&vtd);
    D3D11_TEXTURE2D_DESC staged = vtd;
    staged.Usage = D3D11_USAGE_STAGING;
    staged.BindFlags = 0;
    staged.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(dev->CreateTexture2D(&staged, nullptr, staging.GetAddressOf()))) {
        return out;
    }
    dc->CopyResource(staging.Get(), velTex.Get());
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(dc->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        const uint8_t* row =
            static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(py) * mapped.RowPitch;
        uint16_t rh = 0;
        uint16_t gh = 0;
        std::memcpy(&rh, row + static_cast<size_t>(px) * 4, 2);
        std::memcpy(&gh, row + static_cast<size_t>(px) * 4 + 2, 2);
        out = { HalfToFloat(rh), HalfToFloat(gh) };
        dc->Unmap(staging.Get(), 0);
    }
    return out;
}

// ---- (a): DeferredPath のフォワード段が RenderView::water から MyEngineWater の値を読み、
//      gWaterTime の前後差 (curWaterTime/prevWaterTime) が gbVelocity に反映されることを確認する。
//      view.water が無効なフレームでは両方が厳密に 0 に落ちることも確認する (spec §4.1) ----
void TestDeferredForwardStageReadsWaterCb(GraphicsDevice& device, const std::wstring& engineShaderDir)
{
    MYE_LOG_INFO("-- DeferredPath フォワード段: MyEngineWater の値と gWaterTime 駆動の速度 --");
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / L"mye_water_surface_selftest_deferred";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    WriteFile(dir / L"WaterProbe.surface.hlsl", kWaterProbeSurfaceHlsl);
    WriteFile(dir / L"WaterProbe.mat.json", "{\"shader\":\"WaterProbe.surface\"}");

    ShaderManager shaders;
    Check(shaders.Init(device, { dir.wstring(), engineShaderDir }), "shader manager init (deferred)");

    RenderResources resources;
    resources.Init(device);
    const AssetID matId = resources.materials.LoadFromFile((dir / L"WaterProbe.mat.json").wstring(),
                                                            resources.textures, dir.wstring());
    Check(!matId.IsNull(), "WaterProbe.mat.json が読み込める");

    DeferredPath dp;
    Check(dp.Init(device, shaders), "DeferredPath::Init");

    const AssetID quad = resources.meshes.Quad();

    constexpr UINT kSize = 32;
    ID3D11Device* dev = device.Device();
    ID3D11DeviceContext* dc = device.Context();

    D3D11_TEXTURE2D_DESC ctd = {};
    ctd.Width = kSize;
    ctd.Height = kSize;
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
    D3D11_TEXTURE2D_DESC colorStagedDesc = ctd;
    colorStagedDesc.Usage = D3D11_USAGE_STAGING;
    colorStagedDesc.BindFlags = 0;
    colorStagedDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> colorStaging;
    ok = ok && SUCCEEDED(dev->CreateTexture2D(&colorStagedDesc, nullptr, colorStaging.GetAddressOf()));

    D3D11_TEXTURE2D_DESC dtd = {};
    dtd.Width = kSize;
    dtd.Height = kSize;
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
    Check(ok, "RTV/DSV/staging を作成できる (deferred)");
    if (!ok) {
        fs::remove_all(dir, ec);
        return;
    }

    RenderView view;
    XMStoreFloat4x4(&view.view, XMMatrixLookAtLH(XMVectorSet(0, 0, 0, 1), XMVectorSet(0, 0, 1, 1),
                                                 XMVectorSet(0, 1, 0, 0)));
    XMStoreFloat4x4(&view.proj, XMMatrixOrthographicLH(4.0f, 4.0f, 0.1f, 100.0f));
    XMStoreFloat4x4(&view.projNoJitter, XMLoadFloat4x4(&view.proj));
    view.width = static_cast<int>(kSize);
    view.height = static_cast<int>(kSize);
    view.rtv = colorRtv.Get();
    view.dsv = dsv.Get();
    view.instancingEnabled = 0;
    XMStoreFloat4x4(&view.prevViewProj,
                    XMMatrixMultiply(XMLoadFloat4x4(&view.view), XMLoadFloat4x4(&view.projNoJitter)));
    view.prevViewProjValid = 1;
    view.viewFrameIndex = 30;

    SceneLightData lights;
    lights.ambient = { 0.0f, 0.0f, 0.0f };
    lights.count = 0;

    RenderItem item;
    item.mesh = quad;
    item.material = matId;
    XMStoreFloat4x4(&item.world, XMMatrixTranslation(0.0f, 0.0f, 5.0f));
    item.prevWorld = item.world; // ワールド位置は動かさない — 速度はシェーダ変位だけの寄与

    RenderQueue queue;
    queue.opaque = { item };

    // ---- 水面 active: gWaterTime が cur=1.0 / prev=0.2 で異なる → 速度が非 0 になるはず ----
    WaterDrawData water;
    water.active = true;
    water.curWaterTime = 1.0f;
    water.prevWaterTime = 0.2f;
    water.surfaceCb.enabled = 1;
    water.surfaceCb.deepColor = { 0.25f, 0.75f, 0.10f, 1.0f };
    view.water = &water;

    dp.Render(device, view, queue, lights, resources, shaders);

    const std::array<uint8_t, 4> pixelActive =
        ReadRgba(dc, colorTex.Get(), colorStaging.Get(), kSize / 2, kSize / 2);
    auto closeTo = [](uint8_t a, uint8_t b) { return std::abs(static_cast<int>(a) - static_cast<int>(b)) <= 3; };
    // 0.25/0.75/0.10 の sRGB 量子化 (UNORM そのまま、リニア変換なし) ≈ 64/191/26
    Check(closeTo(pixelActive[0], 64) && closeTo(pixelActive[1], 191) && closeTo(pixelActive[2], 26),
          "(値) MyEngineWater.deepColor が view.water 経由でそのまま HDR シーンへ出る");
    if (!(closeTo(pixelActive[0], 64) && closeTo(pixelActive[1], 191) && closeTo(pixelActive[2], 26))) {
        MYE_LOG_ERROR("    pixelActive=(%d,%d,%d,%d)", pixelActive[0], pixelActive[1], pixelActive[2],
                     pixelActive[3]);
    }

    ID3D11ShaderResourceView* velSrv = dp.VelocitySRV();
    Check(velSrv != nullptr, "VelocitySRV が取得できる");
    const XMFLOAT2 velActive = ReadVelocity(dc, dev, velSrv, kSize / 2, kSize / 2);
    const float velActiveMag = std::sqrt(velActive.x * velActive.x + velActive.y * velActive.y);
    Check(velActiveMag > 0.0005f,
          "(速度) 水面 active なフレームは gWaterTime の前後差 (cur!=prev) で gbVelocity が非 0");
    if (!(velActiveMag > 0.0005f)) {
        MYE_LOG_ERROR("    velActive=(%f,%f) mag=%f", velActive.x, velActive.y, velActiveMag);
    }

    // ---- 対照: view.water が無効 (null) → gWaterTime は前後とも 0 → 速度も色 (deepColor) も 0 ----
    view.water = nullptr;
    dp.Render(device, view, queue, lights, resources, shaders);
    const std::array<uint8_t, 4> pixelInactive =
        ReadRgba(dc, colorTex.Get(), colorStaging.Get(), kSize / 2, kSize / 2);
    Check(pixelInactive[0] == 0 && pixelInactive[1] == 0 && pixelInactive[2] == 0,
          "(値) view.water が無効なら MyEngineWater は全 0 (gMyeWaterDeepColor も黒)");
    const XMFLOAT2 velInactive = ReadVelocity(dc, dev, velSrv, kSize / 2, kSize / 2);
    Check(std::fabs(velInactive.x) < 1e-6f && std::fabs(velInactive.y) < 1e-6f,
          "(速度) view.water が無効なら gWaterTime は前後とも 0 → gbVelocity は厳密に 0");

    dp.Shutdown();
    fs::remove_all(dir, ec);
}

// ---- (b): ShadowPass の影エントリが ShadowPass::Render の water 引数から gMyeCurWaterTime を
//      読み、curWaterTime の値によって CSM シャドウマップの深度 (= gWaterTime 変位) が変わることを
//      確認する。同じ viewFrameIndex (= gTime) で curWaterTime だけを変えるので、
//      「gTime ではなく water 引数 (gWaterTime) が効いている」ことの直接証拠になる ----
void TestShadowPassDepthReflectsWaterTime(GraphicsDevice& device, const std::wstring& engineShaderDir)
{
    MYE_LOG_INFO("-- ShadowPass: 影エントリの深度が ShadowPass::Render の water 引数 (gWaterTime) で変わるか --");
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / L"mye_water_surface_selftest_shadow";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    WriteFile(dir / L"WaterProbe.surface.hlsl", kWaterProbeSurfaceHlsl);
    WriteFile(dir / L"WaterProbe.mat.json", "{\"shader\":\"WaterProbe.surface\"}");

    ShaderManager shaders;
    Check(shaders.Init(device, { dir.wstring(), engineShaderDir }), "shader manager init (shadow)");

    RenderResources resources;
    resources.Init(device);
    const AssetID matId = resources.materials.LoadFromFile((dir / L"WaterProbe.mat.json").wstring(),
                                                            resources.textures, dir.wstring());
    Check(!matId.IsNull(), "WaterProbe.mat.json が読み込める (shadow)");

    ShadowPass sp;
    constexpr int kRes = 128;
    Check(sp.Init(device, shaders, kRes), "ShadowPass::Init");

    // 真上から見下ろす正射影ライト (SurfaceDeferredSelfTest.cpp と同じ理由で Cube を使う:
    // Quad は真上からの光で footprint が線に潰れる)
    const AssetID cube = resources.meshes.Cube();
    RenderItem item;
    item.mesh = cube;
    item.material = matId;
    XMStoreFloat4x4(&item.world, XMMatrixTranslation(0.0f, 0.0f, 0.0f));
    item.prevWorld = item.world;
    RenderQueue queue;
    queue.opaque = { item };

    const XMMATRIX lightView =
        XMMatrixLookToLH(XMVectorSet(0, 10, 0, 1), XMVectorSet(0, -1, 0, 0), XMVectorSet(0, 0, 1, 0));
    const XMMATRIX lightProj = XMMatrixOrthographicLH(4.0f, 4.0f, 0.1f, 20.0f);
    XMFLOAT4X4 lightViewProj;
    XMStoreFloat4x4(&lightViewProj, XMMatrixMultiply(lightView, lightProj));

    auto readCenterDepth = [&]() -> float {
        ID3D11ShaderResourceView* shadowSrv = sp.SRV();
        if (!shadowSrv) {
            return 0.0f;
        }
        ComPtr<ID3D11Resource> shadowRes;
        shadowSrv->GetResource(shadowRes.GetAddressOf());
        ComPtr<ID3D11Texture2D> shadowTex;
        bool okr = SUCCEEDED(shadowRes.As(&shadowTex));
        D3D11_TEXTURE2D_DESC td = {};
        if (okr) {
            shadowTex->GetDesc(&td);
        }
        D3D11_TEXTURE2D_DESC staged = td;
        staged.Format = DXGI_FORMAT_R32_FLOAT; // TYPELESS → R32_FLOAT として読み替え
        staged.Usage = D3D11_USAGE_STAGING;
        staged.BindFlags = 0;
        staged.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        okr = okr && SUCCEEDED(device.Device()->CreateTexture2D(&staged, nullptr, staging.GetAddressOf()));
        if (!okr) {
            return 0.0f;
        }
        ID3D11DeviceContext* dc = device.Context();
        dc->CopyResource(staging.Get(), shadowTex.Get());
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        float v = 0.0f;
        if (SUCCEEDED(dc->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            const uint8_t* row = static_cast<const uint8_t*>(mapped.pData)
                + static_cast<size_t>(kRes / 2) * mapped.RowPitch;
            std::memcpy(&v, row + static_cast<size_t>(kRes / 2) * 4, 4);
            dc->Unmap(staging.Get(), 0);
        }
        return v;
    };

    // viewFrameIndex は 3 回とも同じ (0) — gTime は差が付かない。curWaterTime だけを変える
    WaterDrawData waterZero;
    waterZero.active = true;
    waterZero.curWaterTime = 0.0f;
    waterZero.surfaceCb.enabled = 1;
    sp.Render(device, shaders, queue, resources, &lightViewProj, 1, /*viewFrameIndex=*/0, true, &waterZero);
    const float depthZero = readCenterDepth();

    WaterDrawData waterMoved;
    waterMoved.active = true;
    waterMoved.curWaterTime = 3.0f;
    waterMoved.surfaceCb.enabled = 1;
    sp.Render(device, shaders, queue, resources, &lightViewProj, 1, /*viewFrameIndex=*/0, true, &waterMoved);
    const float depthMoved = readCenterDepth();

    sp.Render(device, shaders, queue, resources, &lightViewProj, 1, /*viewFrameIndex=*/0, true, nullptr);
    const float depthNull = readCenterDepth();

    Check(depthZero != 0.0f && depthMoved != 0.0f && depthNull != 0.0f,
          "3 回とも影を落としている (深度が既定の 0 のままではない)");
    Check(std::fabs(depthMoved - depthZero) > 1e-4f,
          "water 引数の curWaterTime を変えると CSM シャドウマップの深度が変わる (影エントリが gWaterTime を反映する)");
    if (!(std::fabs(depthMoved - depthZero) > 1e-4f)) {
        MYE_LOG_ERROR("    depthZero=%f depthMoved=%f", depthZero, depthMoved);
    }
    Check(std::fabs(depthNull - depthZero) < 1e-6f,
          "water=nullptr は curWaterTime=0 のときと同じ深度になる (無効時は gWaterTime=0 に落ちる)");

    fs::remove_all(dir, ec);
}

// ---- (c): WaterPass::Render は useSurfaceRoute のとき描画を一切しない (二重描画の回避)。
//      「描画したかどうか」を OM の render target 束縛状態の変化だけで判定する — WaterPass の
//      実際の見た目 (ライティング等) には依存しない、最も壊れにくい判定方法 ----
void TestWaterPassSkipsWhenSurfaceRouteActive(GraphicsDevice& device, const std::wstring& engineShaderDir)
{
    MYE_LOG_INFO("-- WaterPass::Render: useSurfaceRoute のとき何も描かない (二重描画の回避) --");
    ShaderManager shaders;
    Check(shaders.Init(device, { engineShaderDir }), "shader manager init (water pass)");

    RenderResources resources;
    resources.Init(device);

    WaterPass wp;
    Check(wp.Init(device, shaders), "WaterPass::Init");

    constexpr UINT kSize = 8;
    ID3D11Device* dev = device.Device();
    ID3D11DeviceContext* dc = device.Context();

    D3D11_TEXTURE2D_DESC ctd = {};
    ctd.Width = kSize;
    ctd.Height = kSize;
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

    D3D11_TEXTURE2D_DESC dtd = {};
    dtd.Width = kSize;
    dtd.Height = kSize;
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
    Check(ok, "RTV/DSV を作成できる (water pass)");
    if (!ok) {
        return;
    }
    dc->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

    RenderView view;
    XMStoreFloat4x4(&view.view, XMMatrixLookAtLH(XMVectorSet(0, 10, 0, 1), XMVectorSet(0, 0, 0, 1),
                                                 XMVectorSet(0, 0, 1, 0)));
    XMStoreFloat4x4(&view.proj, XMMatrixOrthographicLH(4.0f, 4.0f, 0.1f, 100.0f));
    view.width = static_cast<int>(kSize);
    view.height = static_cast<int>(kSize);
    view.rtv = colorRtv.Get();
    view.dsv = dsv.Get();

    WaterDrawData water;
    water.active = true;
    view.water = &water;

    auto boundAfterRender = [&](bool useSurfaceRoute) {
        water.useSurfaceRoute = useSurfaceRoute;
        dc->OMSetRenderTargets(0, nullptr, nullptr); // 束縛状態をいったん白紙に戻す
        wp.Render(device, shaders, view, resources, nullptr, nullptr);
        ID3D11RenderTargetView* gotRtv = nullptr;
        ID3D11DepthStencilView* gotDsv = nullptr;
        dc->OMGetRenderTargets(1, &gotRtv, &gotDsv);
        const bool bound = gotRtv != nullptr;
        if (gotRtv) {
            gotRtv->Release();
        }
        if (gotDsv) {
            gotDsv->Release();
        }
        return bound;
    };

    Check(!boundAfterRender(true),
          "useSurfaceRoute=true: WaterPass::Render は OM ステートに一切触れず即座に return する");
    Check(boundAfterRender(false),
          "useSurfaceRoute=false: WaterPass::Render は従来どおり render target を束縛して描く");

    wp.Shutdown();
}

} // namespace

bool RunWaterSurfaceSelfTest()
{
    MYE_LOG_INFO("==== M79 water surface self test (sub-05) ====");
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

    TestDeferredForwardStageReadsWaterCb(device, engineShaderDir);
    TestShadowPassDepthReflectsWaterTime(device, engineShaderDir);
    TestWaterPassSkipsWhenSurfaceRouteActive(device, engineShaderDir);

    MYE_LOG_INFO("==== M79 water surface self test: %s ====", (g_fails == 0) ? "ALL PASS" : "FAILED");
    return g_fails == 0;
}

} // namespace mye
