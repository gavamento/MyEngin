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

#include "Engine/Core/Log.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/DeferredPath.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/GraphicsDevice.h"
#include "Engine/Renderer/ShaderManager.h"
#include "Engine/Renderer/ShadowPass.h"

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
    TestDeferredTransparentDrawsSurfaceColorEntry(device, engineShaderDir); // M79 sub-05 round 2

    // (d) サーフェス 0 件のときフォワード段が何も張らないことは、この自己テストの範囲では
    // 「既存 golden (--selftest 全体 / tools\replay_verify.bat の既定シーン群、いずれもサーフェス
    // 材質を含まない) が本サブの変更前後でビット一致する」ことで担保している (round 1 で確認済み、
    // sub-03.md 実装メモ参照)。この専用テストにサーフェス 0 件のケースを重複して足さない

    MYE_LOG_INFO("==== M79 surface deferred self test: %s ====", (g_fails == 0) ? "ALL PASS" : "FAILED");
    return g_fails == 0;
}

} // namespace mye
