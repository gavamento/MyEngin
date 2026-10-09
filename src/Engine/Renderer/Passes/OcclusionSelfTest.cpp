//====================================================================================
//                          OcclusionSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                       10/09/2026
//                                          GPU オクルージョンの回帰テスト実装
//====================================================================================
#include "Engine/Renderer/Passes/OcclusionSelfTest.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <DirectXMath.h>
#include <d3d11.h>
#include <wrl/client.h>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Util/Hash.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/Device/GpuResources.h"
#include "Engine/Renderer/Device/GraphicsDevice.h"
#include "Engine/Renderer/Passes/HzbPass.h"
#include "Engine/Renderer/Passes/OcclusionCullPass.h"
#include "Engine/Renderer/Passes/OcclusionMath.h"
#include "Engine/Renderer/Pipeline/DeferredPath.h"
#include "Engine/Renderer/Pipeline/ForwardPath.h"
#include "Engine/Renderer/Shader/ShaderManager.h"

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

// ---- CPU の参照ピラミッド (HzbReduceSpan の分割規則そのまま) ----
struct CpuPyramid {
    int width = 0;
    int height = 0;
    std::vector<std::vector<float>> mips;
    float At(int mip, int x, int y) const
    {
        return mips[static_cast<size_t>(mip)][static_cast<size_t>(y * HzbMipExtent(width, mip) + x)];
    }
};

CpuPyramid BuildCpuPyramid(const std::vector<float>& base, int w, int h, bool useMax)
{
    CpuPyramid p;
    p.width = w;
    p.height = h;
    p.mips.push_back(base);
    const int count = HzbMipCount(w, h);
    for (int level = 1; level < count; ++level) {
        const int srcW = HzbMipExtent(w, level - 1);
        const int srcH = HzbMipExtent(h, level - 1);
        const int dstW = HzbMipExtent(w, level);
        const int dstH = HzbMipExtent(h, level);
        std::vector<float> dst(static_cast<size_t>(dstW * dstH));
        const std::vector<float>& src = p.mips[static_cast<size_t>(level - 1)];
        for (int y = 0; y < dstH; ++y) {
            for (int x = 0; x < dstW; ++x) {
                int x0 = 0, x1 = 0, y0 = 0, y1 = 0;
                HzbReduceSpan(x, srcW, dstW, x0, x1);
                HzbReduceSpan(y, srcH, dstH, y0, y1);
                float m = useMax ? 0.0f : 1.0f;
                for (int sy = y0; sy <= y1; ++sy) {
                    for (int sx = x0; sx <= x1; ++sx) {
                        const float v = src[static_cast<size_t>(sy * srcW + sx)];
                        m = useMax ? std::max(m, v) : std::min(m, v);
                    }
                }
                dst[static_cast<size_t>(y * dstW + x)] = m;
            }
        }
        p.mips.push_back(std::move(dst));
    }
    return p;
}

// 再現できる疑似乱数 [0,1)
std::vector<float> MakeNoise(int w, int h, uint32_t seed)
{
    std::vector<float> v(static_cast<size_t>(w * h));
    uint32_t s = seed;
    for (float& f : v) {
        s = s * 1664525u + 1013904223u;
        f = static_cast<float>(s >> 8) / 16777216.0f;
    }
    return v;
}

// ---- 純関数: max 縮小の取りこぼしゼロ ----
void TestMaxPyramidCoverage()
{
    MYE_LOG_INFO("-- max-Z ピラミッド (CPU): 奇数辺でも取りこぼさない --");
    const int sizes[][2] = { { 15, 7 }, { 37, 23 }, { 31, 31 }, { 64, 48 }, { 129, 3 }, { 1, 9 } };
    bool conservative = true;
    bool topIsGlobalMax = true;
    for (const auto& s : sizes) {
        const int w = s[0];
        const int h = s[1];
        const std::vector<float> base = MakeNoise(w, h, static_cast<uint32_t>(w * 31 + h));
        const CpuPyramid pyr = BuildCpuPyramid(base, w, h, true);
        const int count = HzbMipCount(w, h);
        for (int level = 0; level < count; ++level) {
            const int lw = HzbMipExtent(w, level);
            const int lh = HzbMipExtent(h, level);
            // 画素 (px,py) は floor(px * lw / w) の texel に属する。その texel は必ずその画素以上
            for (int py = 0; py < h; ++py) {
                for (int px = 0; px < w; ++px) {
                    const int tx = std::min(px * lw / w, lw - 1);
                    const int ty = std::min(py * lh / h, lh - 1);
                    if (pyr.At(level, tx, ty) < base[static_cast<size_t>(py * w + px)]) {
                        conservative = false;
                    }
                }
            }
        }
        const float globalMax = *std::max_element(base.begin(), base.end());
        topIsGlobalMax = topIsGlobalMax && pyr.At(count - 1, 0, 0) == globalMax;
    }
    Check(conservative, "どの段の texel も、それに属する全画素の max 以上 (奇数辺の 3 テクセル読みを含む)");
    Check(topIsGlobalMax, "最上段 (1x1) は画像全体の max");

    // 2 のべきの辺は厳密に 2x2 ブロックの max
    const std::vector<float> base = MakeNoise(8, 8, 7);
    const CpuPyramid pyr = BuildCpuPyramid(base, 8, 8, true);
    bool exact = true;
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            const float m = std::max(std::max(base[static_cast<size_t>(2 * y * 8 + 2 * x)],
                                              base[static_cast<size_t>(2 * y * 8 + 2 * x + 1)]),
                                     std::max(base[static_cast<size_t>((2 * y + 1) * 8 + 2 * x)],
                                              base[static_cast<size_t>((2 * y + 1) * 8 + 2 * x + 1)]));
            exact = exact && pyr.At(1, x, y) == m;
        }
    }
    Check(exact, "8x8 の第 1 段は 2x2 ブロックの max に厳密に等しい");
}

// ---- 純関数: 保守的な AABB 判定 ----
void TestConservativeProjection()
{
    MYE_LOG_INFO("-- AABB 判定 (CPU 鏡): 保守的な境界 --");
    constexpr int kW = 128;
    constexpr int kH = 72;
    constexpr float kNear = 0.1f;
    constexpr float kFar = 100.0f;
    XMFLOAT4X4 vp;
    {
        const XMMATRIX v = XMMatrixLookAtLH(XMVectorSet(0, 0, 0, 1), XMVectorSet(0, 0, 1, 1), XMVectorSet(0, 1, 0, 0));
        const XMMATRIX p = XMMatrixPerspectiveFovLH(XMConvertToRadians(60.0f), static_cast<float>(kW) / kH, kNear, kFar);
        XMStoreFloat4x4(&vp, v * p);
    }
    // z = 10 の面が [32,96) x [18,54) 画素を覆っている深度画像
    auto depthAt = [&](float z) { return kFar * (z - kNear) / (z * (kFar - kNear)); };
    const float wallDepth = depthAt(10.0f);
    std::vector<float> base(static_cast<size_t>(kW * kH), 1.0f);
    for (int y = 18; y < 54; ++y) {
        for (int x = 32; x < 96; ++x) {
            base[static_cast<size_t>(y * kW + x)] = wallDepth;
        }
    }
    const CpuPyramid pyr = BuildCpuPyramid(base, kW, kH, true);
    const int mipCount = HzbMipCount(kW, kH);
    auto fetch = [&](int mip, int x, int y) { return pyr.At(mip, x, y); };
    auto visible = [&](float cx, float cy, float cz, float hx, float hy, float hz) {
        const float bmin[3] = { cx - hx, cy - hy, cz - hz };
        const float bmax[3] = { cx + hx, cy + hy, cz + hz };
        return occlusion::IsVisible(vp, bmin, bmax, kW, kH, mipCount, fetch);
    };

    Check(!visible(0, 0, 20, 0.5f, 0.5f, 0.5f), "壁の真裏の小さな箱は隠れている");
    Check(visible(0, 0, 5, 0.5f, 0.5f, 0.5f), "壁の手前の箱は可視");
    Check(visible(12, 0, 20, 0.5f, 0.5f, 0.5f), "壁の輪郭の外にある箱は可視 (HZB が 1.0)");
    Check(visible(10.26f, 0, 20, 1.0f, 0.5f, 0.5f), "壁の縁をまたぐ箱は可視");
    Check(visible(0, 0, 0.05f, 1.0f, 1.0f, 1.0f), "ニア面をまたぐ箱は可視");
    Check(visible(0, 0, -5, 0.5f, 0.5f, 0.5f), "カメラの背後の箱は可視 (w <= 0)");
    Check(visible(0, 0, 20, 30.0f, 30.0f, 0.5f), "画面からはみ出す箱は可視");
    Check(visible(0, 0, 10, 1.0f, 1.0f, 0.0f),
          "壁と同じ深度の厚みの無い箱は自分自身を隠れていると判定しない (深度の丸め余裕)");

    // 深度の甘さの境界 (kDepthBiasSteps 刻みぶんだけ甘く見る)
    occlusion::Probe probe;
    probe.conservativeVisible = false;
    const float maxZ = 0.5f;
    probe.nearestDepth = maxZ + 30.0f * occlusion::kDepthStep;
    Check(!occlusion::IsOccluded(probe, maxZ), "最前面が max-Z の 30 刻み奥は、まだ隠れていると言わない");
    probe.nearestDepth = maxZ + 40.0f * occlusion::kDepthStep;
    Check(occlusion::IsOccluded(probe, maxZ), "40 刻み奥なら隠れている");
    probe.conservativeVisible = true;
    Check(!occlusion::IsOccluded(probe, 0.0f), "conservativeVisible は常に可視");

    // 矩形から選ぶ段は粗いほど max が大きくなる向きにしか動かない (大きな矩形に小さい段を使わない)
    const float bmin[3] = { -3.0f, -2.0f, 19.5f };
    const float bmax[3] = { 3.0f, 2.0f, 20.5f };
    const occlusion::Probe big = occlusion::ProjectAabb(vp, bmin, bmax, kW, kH, mipCount);
    const float smin[3] = { -0.2f, -0.2f, 19.5f };
    const float smax[3] = { 0.2f, 0.2f, 20.5f };
    const occlusion::Probe smallProbe = occlusion::ProjectAabb(vp, smin, smax, kW, kH, mipCount);
    Check(!big.conservativeVisible && !smallProbe.conservativeVisible && big.mip > smallProbe.mip,
          "大きな箱ほど粗い段を使う");
    Check((big.tx1 - big.tx0) <= 2 && (big.ty1 - big.ty0) <= 2, "選んだ段で読むテクセルは各軸 3 個以内");
}

// ---- GPU: ピラミッド (min と max) が CPU の参照と 1 ビットも違わない ----
bool ReadPyramid(GraphicsDevice& device, ID3D11Texture2D* tex, int w, int h, std::vector<std::vector<float>>& out)
{
    D3D11_TEXTURE2D_DESC desc = {};
    tex->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device.Device()->CreateTexture2D(&desc, nullptr, staging.GetAddressOf()))) {
        return false;
    }
    ID3D11DeviceContext* dc = device.Context();
    dc->CopyResource(staging.Get(), tex);
    out.clear();
    for (UINT level = 0; level < desc.MipLevels; ++level) {
        D3D11_MAPPED_SUBRESOURCE m = {};
        if (FAILED(dc->Map(staging.Get(), level, D3D11_MAP_READ, 0, &m))) {
            return false;
        }
        const int lw = HzbMipExtent(w, static_cast<int>(level));
        const int lh = HzbMipExtent(h, static_cast<int>(level));
        std::vector<float> mip(static_cast<size_t>(lw * lh));
        for (int y = 0; y < lh; ++y) {
            std::memcpy(&mip[static_cast<size_t>(y * lw)], static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch,
                        static_cast<size_t>(lw) * sizeof(float));
        }
        dc->Unmap(staging.Get(), level);
        out.push_back(std::move(mip));
    }
    return true;
}

void TestGpuPyramid(GraphicsDevice& device, ShaderManager& shaders)
{
    MYE_LOG_INFO("-- HZB (GPU): max 版と min 版が CPU の参照と一致 --");
    const int sizes[][2] = { { 37, 23 }, { 64, 48 }, { 15, 7 }, { 129, 3 } };
    for (const auto& s : sizes) {
        const int w = s[0];
        const int h = s[1];
        const std::vector<float> base = MakeNoise(w, h, static_cast<uint32_t>(w * 131 + h));
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = static_cast<UINT>(w);
        td.Height = static_cast<UINT>(h);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32_FLOAT;
        td.SampleDesc = { 1, 0 };
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init = { base.data(), static_cast<UINT>(w * sizeof(float)), 0 };
        ComPtr<ID3D11Texture2D> src;
        ComPtr<ID3D11ShaderResourceView> srv;
        const bool made = SUCCEEDED(device.Device()->CreateTexture2D(&td, &init, src.GetAddressOf()))
            && SUCCEEDED(device.Device()->CreateShaderResourceView(src.Get(), nullptr, srv.GetAddressOf()));
        Check(made, "入力深度テクスチャを作れる");
        if (!made) {
            continue;
        }
        for (int op = 0; op < 2; ++op) {
            const bool useMax = (op == 1);
            HzbPass pass;
            Check(pass.Init(device, shaders, useMax ? HzbReduceOp::Max : HzbReduceOp::Min), "HzbPass::Init");
            const bool built = pass.Build(device, shaders, srv.Get(), w, h);
            Check(built, "HzbPass::Build");
            std::vector<std::vector<float>> gpu;
            bool equal = built && ReadPyramid(device, pass.Texture(), w, h, gpu);
            const CpuPyramid cpu = BuildCpuPyramid(base, w, h, useMax);
            equal = equal && gpu.size() == cpu.mips.size();
            for (size_t level = 0; equal && level < gpu.size(); ++level) {
                equal = equal && gpu[level] == cpu.mips[level];
            }
            char what[160];
            std::snprintf(what, sizeof(what), "%s ピラミッド %dx%d の全段が CPU の参照とビット一致",
                          useMax ? "max" : "min", w, h);
            Check(equal, what);
            pass.Shutdown();
        }
    }
}

// ---- GPU: Deferred の ON/OFF 画素一致 ----
constexpr UINT kViewW = 128;
constexpr UINT kViewH = 72;

struct DeferredRig {
    ComPtr<ID3D11Texture2D> colorTex;
    ComPtr<ID3D11RenderTargetView> colorRtv;
    ComPtr<ID3D11Texture2D> colorStaging;
    ComPtr<ID3D11Texture2D> depthTex;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11ShaderResourceView> depthSrv;

    bool Create(GraphicsDevice& device)
    {
        ID3D11Device* dev = device.Device();
        D3D11_TEXTURE2D_DESC ctd = {};
        ctd.Width = kViewW;
        ctd.Height = kViewH;
        ctd.MipLevels = 1;
        ctd.ArraySize = 1;
        ctd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        ctd.SampleDesc = { 1, 0 };
        ctd.Usage = D3D11_USAGE_DEFAULT;
        ctd.BindFlags = D3D11_BIND_RENDER_TARGET;
        D3D11_TEXTURE2D_DESC staged = ctd;
        staged.Usage = D3D11_USAGE_STAGING;
        staged.BindFlags = 0;
        staged.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        // 深度は SRV でも読めるよう typeless (HZB の入力)。本物のスワップチェーン深度と同じ組み合わせ
        D3D11_TEXTURE2D_DESC dtd = {};
        dtd.Width = kViewW;
        dtd.Height = kViewH;
        dtd.MipLevels = 1;
        dtd.ArraySize = 1;
        dtd.Format = DXGI_FORMAT_R24G8_TYPELESS;
        dtd.SampleDesc = { 1, 0 };
        dtd.Usage = D3D11_USAGE_DEFAULT;
        dtd.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        D3D11_DEPTH_STENCIL_VIEW_DESC dvd = {};
        dvd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        D3D11_SHADER_RESOURCE_VIEW_DESC svd = {};
        svd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        svd.Texture2D.MipLevels = 1;
        return SUCCEEDED(dev->CreateTexture2D(&ctd, nullptr, colorTex.GetAddressOf()))
            && SUCCEEDED(dev->CreateRenderTargetView(colorTex.Get(), nullptr, colorRtv.GetAddressOf()))
            && SUCCEEDED(dev->CreateTexture2D(&staged, nullptr, colorStaging.GetAddressOf()))
            && SUCCEEDED(dev->CreateTexture2D(&dtd, nullptr, depthTex.GetAddressOf()))
            && SUCCEEDED(dev->CreateDepthStencilView(depthTex.Get(), &dvd, dsv.GetAddressOf()))
            && SUCCEEDED(dev->CreateShaderResourceView(depthTex.Get(), &svd, depthSrv.GetAddressOf()));
    }

    std::vector<uint8_t> ReadColor(GraphicsDevice& device)
    {
        ID3D11DeviceContext* dc = device.Context();
        dc->CopyResource(colorStaging.Get(), colorTex.Get());
        std::vector<uint8_t> out(static_cast<size_t>(kViewW * kViewH * 4));
        D3D11_MAPPED_SUBRESOURCE m = {};
        if (SUCCEEDED(dc->Map(colorStaging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
            for (UINT y = 0; y < kViewH; ++y) {
                std::memcpy(&out[static_cast<size_t>(y * kViewW * 4)],
                            static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch, kViewW * 4);
            }
            dc->Unmap(colorStaging.Get(), 0);
        }
        return out;
    }
};

struct BoxSpec {
    AssetID mesh;
    AssetID material;
    float cx, cy, cz;
    float sx, sy, sz;
    uint8_t lod = 0; // LOD の段 (段表を持つメッシュだけ意味がある)
};

RenderItem MakeItem(RenderResources& resources, const BoxSpec& b, uint32_t entityIndex)
{
    RenderItem item;
    item.mesh = b.mesh;
    item.lod = b.lod;
    item.material = b.material;
    item.entity = EntityID{ entityIndex, 1 };
    XMStoreFloat4x4(&item.world, XMMatrixScaling(b.sx, b.sy, b.sz) * XMMatrixTranslation(b.cx, b.cy, b.cz));
    item.prevWorld = item.world;
    if (const Mesh* m = resources.meshes.Get(b.mesh)) {
        item.worldAabbMin = { b.cx + m->aabbMin.x * b.sx, b.cy + m->aabbMin.y * b.sy, b.cz + m->aabbMin.z * b.sz };
        item.worldAabbMax = { b.cx + m->aabbMax.x * b.sx, b.cy + m->aabbMax.y * b.sy, b.cz + m->aabbMax.z * b.sz };
        item.hasWorldAabb = 1;
    }
    return item;
}

// 判定対象外 (フェーズ 1 で常に描かれる) サーフェス項目の材質。Forward だけが持つ経路を通す
struct SurfaceProbe {
    std::wstring dir;     // *.surface.hlsl と *.mat.json の置き場
    std::wstring matPath; // サーフェス材質の .mat.json
};

// Deferred / Forward 共通の ON/OFF 一致テスト。Path は Init / Render / InjectOcclusionFailureForTest /
// OcclusionDisabled / OcclusionStatsForTest / Shutdown を持つ描画経路。
// surface が非 null なら、サーフェス材質の項目を 1 個混ぜる (Forward のみ。判定対象外の分岐を通す)
template <class Path>
void TestPathAB(const char* label, GraphicsDevice& device, ShaderManager& shaders, const SurfaceProbe* surface)
{
    MYE_LOG_INFO("-- %s: オクルージョン ON/OFF の画素一致 (履歴なし / 定常 / カメラカット) と失敗の局所化 --", label);
    RenderResources resources;
    resources.Init(device);
    Path dp;
    Check(dp.Init(device, shaders), "Path::Init");
    DeferredRig rig;
    const bool rigOk = rig.Create(device);
    Check(rigOk, "RTV/DSV(typeless)/staging を作れる");
    if (!rigOk) {
        return;
    }

    auto makeMat = [&](const char* name, float r, float g, float b) {
        Material m;
        m.shader = AssetID{ HashStr("forward_lit") };
        m.texture = resources.textures.White();
        m.baseColor = { r, g, b, 1.0f };
        return resources.materials.Register(name, m);
    };
    const AssetID matWall = makeMat("occtest_wall", 0.8f, 0.8f, 0.7f);
    const AssetID matA = makeMat("occtest_a", 0.9f, 0.3f, 0.2f);
    const AssetID matB = makeMat("occtest_b", 0.2f, 0.7f, 0.3f);
    const AssetID cube = resources.meshes.Cube();
    const AssetID sphere = resources.meshes.Sphere();
    AssetID matSurface = {};
    if (surface != nullptr) {
        matSurface = resources.materials.LoadFromFile(surface->matPath, resources.textures, surface->dir);
        Check(!matSurface.IsNull(), "サーフェス材質を読み込める");
    }

    // 壁 (z=10) の裏に同じ (材質, メッシュ) のインスタンス run、壁の外に別の run、単発の球
    std::vector<BoxSpec> boxes;
    boxes.push_back({ cube, matWall, 0, 0, 10, 12, 8, 1 });
    for (int i = 0; i < 6; ++i) {
        boxes.push_back({ cube, matA, -2.5f + static_cast<float>(i), 0.0f, 22.0f, 0.8f, 0.8f, 0.8f }); // 壁の裏
    }
    for (int i = 0; i < 4; ++i) {
        boxes.push_back({ cube, matA, 14.0f + static_cast<float>(i) * 1.5f, 0.0f, 24.0f, 0.8f, 0.8f, 0.8f }); // 壁の外
    }
    boxes.push_back({ sphere, matB, 0.0f, 0.0f, 30.0f, 1.2f, 1.2f, 1.2f });  // 壁の裏の単発
    boxes.push_back({ sphere, matB, -9.0f, 0.0f, 16.0f, 1.2f, 1.2f, 1.2f }); // 壁の外の単発

    // LOD 付きメッシュ (LOD1 = 三角形を 1 つおきに間引いた粗い段)。段ごとの index 範囲が
    // フェーズ 1 / 2 の間接引数に正しく載ることを、カット直後も含めて ON/OFF 比較で確かめる
    AssetID lodSphere = {};
    {
        const Mesh* base = resources.meshes.Get(sphere);
        std::vector<MeshVertex> verts;
        std::vector<uint32_t> coarse;
        if (base != nullptr) {
            verts.resize(base->positions.size());
            for (size_t i = 0; i < verts.size(); ++i) {
                verts[i].position = base->positions[i];
                verts[i].normal = base->normals[i];
                verts[i].uv = base->uvs[i];
            }
            for (size_t t = 0; t + 2 < base->indices.size(); t += 6) {
                coarse.insert(coarse.end(), base->indices.begin() + t, base->indices.begin() + t + 3);
            }
            const MeshLodLevel lod1{ static_cast<uint32_t>(base->indices.size()),
                                     static_cast<uint32_t>(coarse.size()), 0.5f };
            lodSphere = resources.meshes.Register("occtest_lod_sphere", verts, base->indices, coarse,
                                                  std::span<const MeshLodLevel>(&lod1, 1));
        }
        const Mesh* lm = resources.meshes.Get(lodSphere);
        Check(lm != nullptr && lm->LodCount() == 2 && lm->LodRange(1).indexCount < lm->LodRange(0).indexCount,
              "LOD 付きの検査用メッシュを登録できる");
    }
    if (!lodSphere.IsNull()) {
        for (int i = 0; i < 4; ++i) {
            boxes.push_back({ lodSphere, matB, -4.8f + 2.4f * static_cast<float>(i), 0.0f, 36.0f, 2.0f, 2.0f, 2.0f,
                              1 }); // 壁の裏の LOD1 の run
        }
        boxes.push_back({ lodSphere, matB, 5.5f, 1.0f, 36.0f, 2.0f, 2.0f, 2.0f, 2 }); // 段数を超える指定 (最も粗い段)
        for (int i = 0; i < 3; ++i) {
            boxes.push_back({ lodSphere, matB, 17.0f + 1.5f * static_cast<float>(i), 0.0f, 26.0f, 1.2f, 1.2f,
                              1.2f, 1 }); // 壁の外の LOD1 の run
        }
        boxes.push_back({ lodSphere, matA, -14.0f, 2.0f, 18.0f, 1.2f, 1.2f, 1.2f, 0 }); // 壁の外の LOD0
    }
    if (!matSurface.IsNull()) {
        boxes.push_back({ cube, matSurface, 3.0f, -1.5f, 5.0f, 0.6f, 0.6f, 0.6f }); // 壁の手前 (判定対象外)
    }

    // 壁の裏の物はカメラ A では隠れ、カメラ B (横から) では見える
    // forceLod >= 0 は全項目の段をその値にする (LOD の差が絵に出ることの確認用)
    auto render = [&](bool occlusionOn, uint32_t serial, bool cameraB, int forceLod = -1) {
        RenderView view;
        const XMVECTOR eye = cameraB ? XMVectorSet(26.0f, 3.0f, 8.0f, 1.0f) : XMVectorSet(0, 0, 0, 1);
        const XMVECTOR at = cameraB ? XMVectorSet(0.0f, 0.0f, 24.0f, 1.0f) : XMVectorSet(0, 0, 1, 1);
        XMStoreFloat4x4(&view.view, XMMatrixLookAtLH(eye, at, XMVectorSet(0, 1, 0, 0)));
        XMStoreFloat4x4(&view.proj,
                        XMMatrixPerspectiveFovLH(XMConvertToRadians(60.0f), static_cast<float>(kViewW) / kViewH, 0.1f, 100.0f));
        view.projNoJitter = view.proj;
        XMStoreFloat3(&view.cameraPos, eye);
        view.width = static_cast<int>(kViewW);
        view.height = static_cast<int>(kViewH);
        view.rtv = rig.colorRtv.Get();
        view.dsv = rig.dsv.Get();
        view.depthSRV = rig.depthSrv.Get();
        view.instancingEnabled = 1;
        view.viewKey = 1;
        view.viewFrameIndex = serial;
        view.occlusionEnabled = occlusionOn ? 1 : 0;
        SceneLightData lights;
        lights.ambient = { 1.0f, 1.0f, 1.0f };
        lights.count = 0;
        RenderQueue queue;
        for (size_t i = 0; i < boxes.size(); ++i) {
            RenderItem item = MakeItem(resources, boxes[i], static_cast<uint32_t>(i));
            if (forceLod >= 0) {
                item.lod = static_cast<uint8_t>(forceLod);
            }
            queue.opaque.push_back(item);
        }
        queue.Sort();
        dp.Render(device, view, queue, lights, resources, shaders);
        return rig.ReadColor(device);
    };

    // 背景だけの絵と区別できること (壁の画素に色が出ている)
    const std::vector<uint8_t> baselineA = render(false, 0, false);
    const std::vector<uint8_t> baselineB = render(false, 0, true);
    Check(baselineA != baselineB, "カメラ A と B で絵が違う (検査が意味を持つ)");
    Check(render(false, 0, true, 0) != baselineB, "LOD0 と LOD1 で絵が違う (LOD の検査が意味を持つ)");
    size_t nonBackground = 0;
    for (size_t i = 0; i < baselineA.size(); i += 4) {
        nonBackground += (baselineA[i] != baselineA[0] || baselineA[i + 1] != baselineA[1]) ? 1 : 0;
    }
    Check(nonBackground > 200, "OFF の絵に物が描かれている");

    uint32_t serial = 100;
    bool allEqual = true;
    // フレーム 0 は履歴なし。以降は定常 (壁の裏の物は前フレーム不可視)
    for (int frame = 0; frame < 5; ++frame) {
        const std::vector<uint8_t> on = render(true, serial++, false);
        const bool same = (on == baselineA);
        allEqual = allEqual && same;
        if (!same) {
            MYE_LOG_ERROR("    frame %d: ON と OFF の絵が違う", frame);
        }
    }
    Check(allEqual, "カメラ A の 5 フレーム (履歴なし → 定常) で ON と OFF の絵が全画素一致");

    // カメラカット: 履歴の可視ビットが全部外れる
    const std::vector<uint8_t> cutOn = render(true, serial++, true);
    Check(cutOn == baselineB, "カメラカット直後のフレームで ON と OFF の絵が全画素一致 (欠けない)");
    const std::vector<uint8_t> cutOn2 = render(true, serial++, true);
    Check(cutOn2 == baselineB, "カット後の定常フレームも一致");
    const std::vector<uint8_t> backOn = render(true, serial++, false);
    Check(backOn == baselineA, "カット前のカメラへ戻った直後も一致");

    // 通番が飛んだ (履歴を捨てる) フレームでも欠けない
    const std::vector<uint8_t> gapOn = render(true, serial + 50, false);
    Check(gapOn == baselineA, "通番が飛んだフレーム (履歴を捨てる) でも一致");

    // 失敗の局所化: 作成失敗を注入してもオクルージョンだけが OFF になり、描画は続く
    Check(!dp.OcclusionDisabled(), "注入前はオクルージョンが有効");
    dp.InjectOcclusionFailureForTest(true);
    const std::vector<uint8_t> failOn = render(true, serial + 60, false);
    Check(dp.OcclusionDisabled(), "リソース作成失敗でオクルージョンが無効になる");
    Check(failOn == baselineA, "失敗したフレームも描画は続き、絵は OFF と一致する");
    dp.InjectOcclusionFailureForTest(false);
    const std::vector<uint8_t> afterFail = render(true, serial + 61, true);
    Check(afterFail == baselineB && dp.OcclusionDisabled(), "失敗後は従来の経路で描き続ける (毎フレーム再試行しない)");

    dp.Shutdown();
}

// 履歴・統計のあるビュー 1 本の遅延統計: 壁の裏の物が数えられる
template <class Path>
void TestStats(const char* label, GraphicsDevice& device, ShaderManager& shaders, bool waitForStats = true)
{
    MYE_LOG_INFO("-- %s 統計 (読み戻しで%s): 2 フレーム遅れの GPU カウント --", label,
                 waitForStats ? "待つ" : "待たない");
    RenderResources resources;
    resources.Init(device);
    Path dp;
    Check(dp.Init(device, shaders), "Path::Init (stats)");
    DeferredRig rig;
    if (!rig.Create(device)) {
        Check(false, "rig");
        return;
    }
    Material m;
    m.shader = AssetID{ HashStr("forward_lit") };
    m.texture = resources.textures.White();
    const AssetID mat = resources.materials.Register("occtest_stats", m);
    const AssetID cube = resources.meshes.Cube();
    // 壁 1 + 裏の単発 3 (別のスケール = 同じメッシュでも run になる 3 個)
    std::vector<BoxSpec> boxes = { { cube, mat, 0, 0, 10, 12, 8, 1 },
                                   { cube, mat, -1, 0, 22, 0.8f, 0.8f, 0.8f },
                                   { cube, mat, 0, 0, 22, 0.8f, 0.8f, 0.8f },
                                   { cube, mat, 1, 0, 22, 0.8f, 0.8f, 0.8f } };
    int occluded = -1;
    int phase1 = -1;
    for (uint32_t frame = 0; frame < 6; ++frame) {
        RenderView view;
        XMStoreFloat4x4(&view.view, XMMatrixLookAtLH(XMVectorSet(0, 0, 0, 1), XMVectorSet(0, 0, 1, 1), XMVectorSet(0, 1, 0, 0)));
        XMStoreFloat4x4(&view.proj,
                        XMMatrixPerspectiveFovLH(XMConvertToRadians(60.0f), static_cast<float>(kViewW) / kViewH, 0.1f, 100.0f));
        view.projNoJitter = view.proj;
        view.width = static_cast<int>(kViewW);
        view.height = static_cast<int>(kViewH);
        view.rtv = rig.colorRtv.Get();
        view.dsv = rig.dsv.Get();
        view.depthSRV = rig.depthSrv.Get();
        view.instancingEnabled = 1;
        view.viewKey = 2;
        view.viewFrameIndex = frame;
        view.occlusionEnabled = 1;
        view.occlusionStatsWait = waitForStats ? 1 : 0;
        SceneLightData lights;
        lights.ambient = { 1.0f, 1.0f, 1.0f };
        RenderQueue queue;
        for (size_t i = 0; i < boxes.size(); ++i) {
            queue.opaque.push_back(MakeItem(resources, boxes[i], static_cast<uint32_t>(i)));
        }
        queue.Sort();
        dp.Render(device, view, queue, lights, resources, shaders);
        const OcclusionStats s = dp.OcclusionStatsForTest(2);
        if (s.valid) {
            occluded = s.occluded;
            phase1 = s.phase1Draws;
        }
    }
    if (waitForStats) {
        Check(occluded == 3, "壁の裏の 3 個が「隠れた物」として数えられる");
        Check(phase1 == 1, "定常フレームのフェーズ 1 は壁 1 個だけ (隠れた物は描かない)");
    } else {
        // 待たない読み方は、どのフレームの値が最後に読めるかが GPU の進み次第なので範囲だけ見る
        // (読めなかったフレームは前の値のまま = 一度も読めなければ -1)
        Check(occluded >= -1 && occluded <= 3, "待たない読み戻しの隠れた数が範囲内 (壁の裏は 3 個まで)");
        Check(phase1 >= -1 && phase1 <= 4, "待たない読み戻しのフェーズ 1 の数が範囲内 (全 4 個まで)");
    }
    dp.Shutdown();
}

} // namespace

bool RunOcclusionSelfTest()
{
    MYE_LOG_INFO("==== GPU occlusion culling self test ====");
    g_fails = 0;
    TestMaxPyramidCoverage();
    TestConservativeProjection();

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
    // サーフェス項目用の最小シェーダと材質 (一時ディレクトリ)
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path probeDir = fs::temp_directory_path(ec) / L"mye_occlusion_selftest";
    fs::remove_all(probeDir, ec);
    fs::create_directories(probeDir, ec);
    {
        std::ofstream hlsl(probeDir / L"OccProbe.surface.hlsl", std::ios::binary);
        hlsl << R"HLSL(#include "MyEngineSurface.hlsli"
struct VSIn { float3 pos : POSITION; };
struct VSOut { float4 pos : SV_Position; };
VSOut VSMain(VSIn v)
{
    VSOut o;
    o.pos = mul(mul(float4(v.pos, 1.0f), gWorld), gViewProj);
    return o;
}
float4 PSMain(VSOut i) : SV_Target
{
    return float4(0.2f, 0.8f, 0.3f, 1.0f);
}
)HLSL";
        std::ofstream mat(probeDir / L"OccProbe.mat.json", std::ios::binary);
        mat << "{\"shader\":\"OccProbe.surface\"}";
    }
    SurfaceProbe surfaceProbe;
    surfaceProbe.dir = probeDir.wstring();
    surfaceProbe.matPath = (probeDir / L"OccProbe.mat.json").wstring();
    ShaderManager shaders;
    Check(shaders.Init(device, { surfaceProbe.dir, engineShaderDir }), "shader manager init");
    TestGpuPyramid(device, shaders);
    TestPathAB<DeferredPath>("Deferred", device, shaders, nullptr);
    TestStats<DeferredPath>("Deferred", device, shaders);
    TestStats<DeferredPath>("Deferred", device, shaders, false);
    TestPathAB<ForwardPath>("Forward", device, shaders, &surfaceProbe);
    TestStats<ForwardPath>("Forward", device, shaders);

    MYE_LOG_INFO("==== GPU occlusion culling self test: %s ====", g_fails == 0 ? "PASS" : "FAIL");
    return g_fails == 0;
}

} // namespace mye
