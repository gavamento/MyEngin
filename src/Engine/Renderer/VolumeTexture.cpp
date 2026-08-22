#include "Engine/Renderer/VolumeTexture.h"

#include <chrono>
#include <cmath>
#include <cstdio>

#include <DirectXPackedVector.h>

#include "Engine/Core/Log.h"
#include "Engine/Renderer/GpuBufferUtil.h"
#include "Engine/Renderer/GpuTimer.h"
#include "Engine/Renderer/GraphicsDevice.h"
#include "Engine/Renderer/RenderTypes.h"
#include "Engine/Renderer/ShaderManager.h"

namespace mye {
namespace {

// froxel_clear.cs.hlsl の FroxelClearCB (b0) と一致
struct FroxelClearCB {
    uint32_t gridSize[3] = { 0, 0, 0 };
    uint32_t pad = 0;
    float clearValue[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
};
static_assert(sizeof(FroxelClearCB) == 32, "HLSL の FroxelClearCB と一致させること");

// 1 セルのバイト数 (ReadbackTexel が対応するフォーマットのみ)
int BytesPerTexel(DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return 8;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return 16;
    default:
        return 0;
    }
}

} // namespace

bool VolumeTexture::Create(GraphicsDevice& device, int width, int height, int depth,
                           DXGI_FORMAT format)
{
    if (width <= 0 || height <= 0 || depth <= 0) {
        return false;
    }
    Release();

    ID3D11Device* dev = device.Device();

    D3D11_TEXTURE3D_DESC td = {};
    td.Width = static_cast<UINT>(width);
    td.Height = static_cast<UINT>(height);
    td.Depth = static_cast<UINT>(depth);
    td.MipLevels = 1; // フロクセルはミップを持たない (積分は最細のまま Z 列を舐める)
    td.Format = format;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

    if (FAILED(dev->CreateTexture3D(&td, nullptr, tex_.GetAddressOf()))) {
        MYE_LOG_ERROR("VolumeTexture: CreateTexture3D failed (%dx%dx%d, format %d)", width, height,
                      depth, static_cast<int>(format));
        Release();
        return false;
    }

    // SRV: 明示的に TEXTURE3D で作る (null desc でも同じになるが、2D と読み違えないよう明示)
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = format;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
    sd.Texture3D.MipLevels = 1;

    // UAV: WSize は「このビューが覆うスライス数」。-1 は使えないので実寸を入れる
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
    ud.Format = format;
    ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
    ud.Texture3D.FirstWSlice = 0;
    ud.Texture3D.WSize = static_cast<UINT>(depth);

    if (FAILED(dev->CreateShaderResourceView(tex_.Get(), &sd, srv_.GetAddressOf()))
        || FAILED(dev->CreateUnorderedAccessView(tex_.Get(), &ud, uav_.GetAddressOf()))) {
        // ★ここで落ちるのが「FL11_0 の typed 3D UAV が使えない」ケース。
        //   フォーマットを変える (R32_FLOAT 4 枚に分ける等) 設計変更の合図なので、
        //   黙って null を返さずログに残す
        MYE_LOG_ERROR("VolumeTexture: view creation failed (%dx%dx%d, format %d は typed 3D UAV "
                      "非対応か)",
                      width, height, depth, static_cast<int>(format));
        Release();
        return false;
    }

    width_ = width;
    height_ = height;
    depth_ = depth;
    format_ = format;
    return true;
}

void VolumeTexture::Resize(GraphicsDevice& device, int width, int height, int depth,
                           DXGI_FORMAT format)
{
    if (width == width_ && height == height_ && depth == depth_ && format == format_) {
        return;
    }
    Create(device, width, height, depth, format);
}

void VolumeTexture::Release()
{
    uav_.Reset();
    srv_.Reset();
    tex_.Reset();
    width_ = 0;
    height_ = 0;
    depth_ = 0;
    format_ = DXGI_FORMAT_R16G16B16A16_FLOAT;
}

bool VolumeTexture::ReadbackTexel(GraphicsDevice& device, int x, int y, int z, float out[4]) const
{
    if (!tex_ || x < 0 || y < 0 || z < 0 || x >= width_ || y >= height_ || z >= depth_) {
        return false;
    }
    const int texelBytes = BytesPerTexel(format_);
    if (texelBytes == 0) {
        return false;
    }

    ID3D11Device* dev = device.Device();
    ID3D11DeviceContext* dc = device.Context();

    D3D11_TEXTURE3D_DESC td = {};
    tex_->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    Microsoft::WRL::ComPtr<ID3D11Texture3D> staging;
    if (FAILED(dev->CreateTexture3D(&td, nullptr, staging.GetAddressOf()))) {
        return false;
    }
    dc->CopyResource(staging.Get(), tex_.Get());

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(dc->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return false;
    }
    // RowPitch / DepthPitch は要求寸法と一致しない (ドライバがパディングする) ので必ず使う
    const uint8_t* base = static_cast<const uint8_t*>(mapped.pData)
        + static_cast<size_t>(z) * mapped.DepthPitch + static_cast<size_t>(y) * mapped.RowPitch
        + static_cast<size_t>(x) * static_cast<size_t>(texelBytes);
    if (format_ == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        const DirectX::PackedVector::HALF* h
            = reinterpret_cast<const DirectX::PackedVector::HALF*>(base);
        for (int i = 0; i < 4; ++i) {
            out[i] = DirectX::PackedVector::XMConvertHalfToFloat(h[i]);
        }
    } else {
        const float* f = reinterpret_cast<const float*>(base);
        for (int i = 0; i < 4; ++i) {
            out[i] = f[i];
        }
    }
    dc->Unmap(staging.Get(), 0);
    return true;
}

// ---- M57a: WARP 実測プローブ ----
namespace {

// 候補解像度。上から順に「計画が想定した値」「中間」「退避先」。
// 3 点あるのは「遅い/速い」ではなく **セル数に対してどう伸びるか**を見るため —
// 線形なら解像度を落とせば予算に入る、そうでなければディスパッチの固定費が支配的で
// 解像度を落としても救われない (= 設計をやり直す) という判断ができる
struct ProbeCandidate {
    int x;
    int y;
    int z;
};
constexpr ProbeCandidate kCandidates[] = {
    { 160, 90, 64 },
    { 128, 72, 48 },
    { 80, 45, 32 },
};

// 1 回ぶんのディスパッチ (バインドから解除まで)。
// ★RtPasses::UnbindCompute と同じ理由で毎回 UAV / CS を外す — 外さないまま次のパスで
//   同じテクスチャを SRV として読むと D3D が黙ってバインドを剥がす
void DispatchClear(GraphicsDevice& device, ID3D11ComputeShader* cs, ID3D11Buffer* cb,
                   const VolumeTexture& vol)
{
    ID3D11DeviceContext* dc = device.Context();
    ID3D11Buffer* cbs[1] = { cb };
    ID3D11UnorderedAccessView* uavs[1] = { vol.UAV() };
    dc->CSSetShader(cs, nullptr, 0);
    dc->CSSetConstantBuffers(0, 1, cbs);
    dc->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    dc->Dispatch(static_cast<UINT>(froxel::DispatchGroups(vol.Width(), froxel::kGroupSize)),
                 static_cast<UINT>(froxel::DispatchGroups(vol.Height(), froxel::kGroupSize)),
                 static_cast<UINT>(vol.Depth()));
    ID3D11UnorderedAccessView* nullUavs[1] = {};
    dc->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);
    dc->CSSetShader(nullptr, nullptr, 0);
}

} // namespace

int RunFroxelVolumeProbe(const FroxelProbeOptions& options)
{
    GraphicsDevice device;
    if (!device.Init(options.forceWarp)) {
        std::fprintf(stderr, "[froxel-probe] ERROR: D3D11 device creation failed\n");
        return 2;
    }
    ShaderManager shaders;
    if (!shaders.Init(device, options.shaderDirs)) {
        std::fprintf(stderr, "[froxel-probe] ERROR: ShaderManager init failed\n");
        return 2;
    }
    ShaderProgram* program = shaders.Get(shaders.LoadCompute("froxel_clear.cs"));
    if (!program || !program->valid || !program->cs) {
        std::fprintf(stderr, "[froxel-probe] ERROR: froxel_clear.cs failed to compile\n");
        return 2;
    }

    Microsoft::WRL::ComPtr<ID3D11Buffer> cb;
    if (!gpubuf::CreateConstant(device.Device(), sizeof(FroxelClearCB), cb)) {
        std::fprintf(stderr, "[froxel-probe] ERROR: constant buffer creation failed\n");
        return 2;
    }
    GpuTimer timer;
    const bool timerOk = timer.Init(device);

    const int iterations = (options.iterations > 0) ? options.iterations : 1;
    std::printf("[froxel-probe] adapter = %s (%s), iterations = %d\n",
                device.AdapterName().c_str(), device.IsWarp() ? "WARP" : "hardware", iterations);
    std::printf("[froxel-probe] %-14s %10s %8s %8s %9s %7s  %s\n", "grid", "cells", "ms/disp",
                "gpu ms", "Mcell/s", "MB/vol", "uav store");

    int rc = 0;
    for (const ProbeCandidate& c : kCandidates) {
        VolumeTexture vol;
        if (!vol.Create(device, c.x, c.y, c.z, DXGI_FORMAT_R16G16B16A16_FLOAT)) {
            std::printf("[froxel-probe] %3dx%-3dx%-4d  typed 3D UAV NOT AVAILABLE\n", c.x, c.y,
                        c.z);
            rc = 1;
            continue;
        }

        // ---- 正しさ: CS が本当に 3D UAV へ書けたか ----
        // セルごとに違う値を書けないクリアシェーダなので、識別可能な定数を 1 つ書いて
        // 「先頭 / 末尾 / 中央」の 3 点を読み戻す。末尾を見るのが要点 —
        // WSize や境界判定を間違えると最後のスライスだけが 0 のまま残る
        FroxelClearCB clear;
        clear.gridSize[0] = static_cast<uint32_t>(c.x);
        clear.gridSize[1] = static_cast<uint32_t>(c.y);
        clear.gridSize[2] = static_cast<uint32_t>(c.z);
        clear.clearValue[0] = 0.25f; // half で厳密に表せる値を選ぶ (2 のべき) — 比較を厳密にする
        clear.clearValue[1] = 0.5f;
        clear.clearValue[2] = 1.5f;
        clear.clearValue[3] = 2.0f;
        gpubuf::UploadCB(device.Context(), cb.Get(), clear);
        DispatchClear(device, program->cs.Get(), cb.Get(), vol);

        const int probes[3][3] = {
            { 0, 0, 0 }, { c.x / 2, c.y / 2, c.z / 2 }, { c.x - 1, c.y - 1, c.z - 1 }
        };
        bool storeOk = true;
        for (const int* p : probes) {
            float texel[4] = { 0, 0, 0, 0 };
            if (!vol.ReadbackTexel(device, p[0], p[1], p[2], texel)) {
                storeOk = false;
                break;
            }
            for (int i = 0; i < 4; ++i) {
                if (texel[i] != clear.clearValue[i]) {
                    std::printf("[froxel-probe]   MISMATCH at (%d,%d,%d)[%d]: %g != %g\n", p[0],
                                p[1], p[2], i, texel[i], clear.clearValue[i]);
                    storeOk = false;
                }
            }
        }
        if (!storeOk) {
            rc = 1;
        }

        // ---- 速度: 壁時計 ----
        // ウォームアップで初回のリソース遷移とシェーダの実体化を追い出してから測る。
        // 末尾の ReadbackTexel が Map で GPU を待つので、区間には投げた全ディスパッチの
        // 実行が含まれる (Dispatch だけでは非同期なので「速い」を測ってしまう)
        for (int i = 0; i < 4; ++i) {
            DispatchClear(device, program->cs.Get(), cb.Get(), vol);
        }
        float dummy[4] = { 0, 0, 0, 0 };
        vol.ReadbackTexel(device, 0, 0, 0, dummy);

        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < iterations; ++i) {
            DispatchClear(device, program->cs.Get(), cb.Get(), vol);
        }
        vol.ReadbackTexel(device, 0, 0, 0, dummy);
        const auto t1 = std::chrono::steady_clock::now();
        const double totalMs
            = std::chrono::duration<double, std::milli>(t1 - t0).count();
        const double msPerDispatch = totalMs / static_cast<double>(iterations);

        // ---- 速度: GPU タイムスタンプ ----
        // GpuTimer は 6 フレームのリングで、結果は次に同じスロットへ来たとき回収される。
        // ここでは毎回 Map で完全同期しているので 8 周させれば必ず 1 件は確定する
        float gpuMs = 0.0f;
        if (timerOk) {
            for (int i = 0; i < 8; ++i) {
                timer.Begin(device);
                DispatchClear(device, program->cs.Get(), cb.Get(), vol);
                timer.End(device);
                vol.ReadbackTexel(device, 0, 0, 0, dummy);
            }
            gpuMs = timer.Milliseconds();
        }

        // ボリューム 1 枚の VRAM。M57c のテンポラルは履歴 ping-pong を持つので
        // 実際に載る枚数はこの 3〜4 倍になる (計画が「3D テクスチャは VRAM を増やす」と
        // 名指ししている箇所の根拠になる数字)
        const double cells = static_cast<double>(c.x) * c.y * c.z;
        const double megabytes = cells * BytesPerTexel(DXGI_FORMAT_R16G16B16A16_FLOAT)
            / (1024.0 * 1024.0);
        char grid[32] = {};
        std::snprintf(grid, sizeof(grid), "%dx%dx%d", c.x, c.y, c.z);
        std::printf("[froxel-probe] %-14s %10.0f %8.3f %8.3f %9.1f %7.2f  %s\n", grid, cells,
                    msPerDispatch, static_cast<double>(gpuMs),
                    (msPerDispatch > 0.0) ? cells / msPerDispatch / 1.0e6 * 1000.0 : 0.0,
                    megabytes, storeOk ? "OK" : "BROKEN");
    }

    std::printf("[froxel-probe] %s\n", rc == 0 ? "PASS" : "FAIL");
    return rc;
}

} // namespace mye
