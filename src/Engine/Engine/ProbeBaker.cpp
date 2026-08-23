#include "Engine/Engine/ProbeBaker.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include <DirectXPackedVector.h>

#include "Engine/Core/Log.h"
#include "Engine/Renderer/GraphicsDevice.h"
#include "Engine/Renderer/ImageWrite.h"
#include "Engine/Renderer/RenderPath.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

namespace mye {
namespace {

// HDR リニア → 8bit sRGB (診断 PNG 専用)。Reinhard で 1 に漸近させてから符号化する —
// 生の HDR を切り捨てると光溜まりが全部真っ白になって「面が合っているか」が読めない
uint8_t EncodeLdr(float linear)
{
    const float tone = std::max(0.0f, linear) / (1.0f + std::max(0.0f, linear));
    const float srgb = (tone <= 0.0031308f) ? tone * 12.92f
                                            : 1.055f * std::pow(tone, 1.0f / 2.4f) - 0.055f;
    return static_cast<uint8_t>(std::clamp(srgb, 0.0f, 1.0f) * 255.0f + 0.5f);
}

float Luma(float r, float g, float b)
{
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

} // namespace

XMFLOAT3 ProbeFaceDir(int face, float u, float v)
{
    const CubeFaceBasis& b = CubeFace(face);
    const float s = 2.0f * u - 1.0f;
    const float t = 1.0f - 2.0f * v; // v は下向き正 / t は上向き正
    XMVECTOR d = XMVectorAdd(XMLoadFloat3(&b.forward),
                             XMVectorAdd(XMVectorScale(XMLoadFloat3(&b.right), s),
                                         XMVectorScale(XMLoadFloat3(&b.up), t)));
    XMFLOAT3 out;
    XMStoreFloat3(&out, XMVector3Normalize(d));
    return out;
}

bool ProbeDirToFaceUv(const XMFLOAT3& dir, int& face, float& u, float& v)
{
    const float ax = std::fabs(dir.x);
    const float ay = std::fabs(dir.y);
    const float az = std::fabs(dir.z);
    if (ax <= 0.0f && ay <= 0.0f && az <= 0.0f) {
        return false;
    }
    // 支配軸で面を決める。同値のとき (=稜線ちょうど) は X > Y > Z の順で決め打ち —
    // どちらの面を選んでも方向は同じなので、決定性さえあれば良い
    if (ax >= ay && ax >= az) {
        face = (dir.x > 0.0f) ? 0 : 1;
    } else if (ay >= az) {
        face = (dir.y > 0.0f) ? 2 : 3;
    } else {
        face = (dir.z > 0.0f) ? 4 : 5;
    }
    const CubeFaceBasis& b = CubeFace(face);
    // 基底は正規直交 & 軸平行なので、成分は内積そのもの。k = 前方成分で割ると
    // 「forward + s*right + t*up」の s,t が出る
    const float k = dir.x * b.forward.x + dir.y * b.forward.y + dir.z * b.forward.z;
    if (k <= 0.0f) {
        return false;
    }
    const float s = (dir.x * b.right.x + dir.y * b.right.y + dir.z * b.right.z) / k;
    const float t = (dir.x * b.up.x + dir.y * b.up.y + dir.z * b.up.z) / k;
    u = (s + 1.0f) * 0.5f;
    v = (1.0f - t) * 0.5f;
    return true;
}

XMFLOAT4X4 ProbeFaceView(int face, const XMFLOAT3& position)
{
    const CubeFaceBasis& b = CubeFace(face);
    XMFLOAT4X4 out;
    XMStoreFloat4x4(&out, XMMatrixLookToLH(XMVectorSet(position.x, position.y, position.z, 1.0f),
                                           XMLoadFloat3(&b.forward), XMLoadFloat3(&b.up)));
    return out;
}

XMFLOAT4X4 ProbeFaceProj(float nearZ, float farZ)
{
    XMFLOAT4X4 out;
    XMStoreFloat4x4(&out, XMMatrixPerspectiveFovLH(XM_PIDIV2, 1.0f, nearZ, farZ));
    return out;
}

bool ProbeBaker::EnsureDepth(GraphicsDevice& device)
{
    if (depthDsv_) {
        return true;
    }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = kCaptureSize;
    td.Height = kCaptureSize;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_D32_FLOAT;
    td.SampleDesc = { 1, 0 };
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ID3D11Device* dev = device.Device();
    if (FAILED(dev->CreateTexture2D(&td, nullptr, depthTex_.GetAddressOf()))) {
        return false;
    }
    return SUCCEEDED(
        dev->CreateDepthStencilView(depthTex_.Get(), nullptr, depthDsv_.GetAddressOf()));
}

bool ProbeBaker::Bake(World& world, GraphicsDevice& device, IRenderPath& path,
                      ShaderManager& shaders, RenderResources& resources,
                      const XMFLOAT3& position, float nearZ, float farZ, BakedProbe& out)
{
    const auto started = std::chrono::steady_clock::now();
    out.valid = false;
    out.position = position;
    if (!EnsureDepth(device)) {
        MYE_LOG_ERROR("[probe] depth buffer creation failed");
        return false;
    }
    ID3D11Device* dev = device.Device();
    ID3D11DeviceContext* dc = device.Context();

    // ---- キャプチャ用 cube (プローブ 1 個ごとに新規) ----
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = kCaptureSize;
    td.Height = kCaptureSize;
    td.MipLevels = 1;
    td.ArraySize = 6;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; // ★HDR リニアのまま持つ (postFx を切る理由)
    td.SampleDesc = { 1, 0 };
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, out.captureTex.ReleaseAndGetAddressOf()))
        || FAILED(dev->CreateShaderResourceView(out.captureTex.Get(), nullptr,
                                                out.captureSrv.ReleaseAndGetAddressOf()))) {
        MYE_LOG_ERROR("[probe] capture cube creation failed");
        return false;
    }
    for (int f = 0; f < 6; ++f) {
        // 面 1 枚だけの 2D ビュー。ImGui はキューブを描けないのでサムネイルにはこちらが要る
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = td.Format;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        sd.Texture2DArray.MipLevels = 1;
        sd.Texture2DArray.FirstArraySlice = static_cast<UINT>(f);
        sd.Texture2DArray.ArraySize = 1;
        if (FAILED(dev->CreateShaderResourceView(out.captureTex.Get(), &sd,
                                                 out.faceSrv[f].ReleaseAndGetAddressOf()))) {
            MYE_LOG_ERROR("[probe] face SRV creation failed (face %d)", f);
            return false;
        }
    }

    // ---- 6 面を実描画 ----
    // postFx を切るので、パスはトーンマップも OETF も通さず **リニア放射輝度をそのまま**
    // RTV へ書く = プリフィルタがそのまま食える。背景色だけはこちらでリニアへ直す
    // (メイン描画は HDR 中間があるときだけ変換する規約で、直描き経路は変換しない)
    render_.enablePostFx = false;
    render_.assetsRoot = assetsRoot;
    for (int f = 0; f < 6; ++f) {
        D3D11_RENDER_TARGET_VIEW_DESC rd = {};
        rd.Format = td.Format;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        rd.Texture2DArray.MipSlice = 0;
        rd.Texture2DArray.FirstArraySlice = static_cast<UINT>(f);
        rd.Texture2DArray.ArraySize = 1;
        ComPtr<ID3D11RenderTargetView> rtv;
        if (FAILED(dev->CreateRenderTargetView(out.captureTex.Get(), &rd, rtv.GetAddressOf()))) {
            MYE_LOG_ERROR("[probe] face RTV creation failed (face %d)", f);
            return false;
        }
        FrameTarget target;
        target.rtv = rtv.Get();
        target.dsv = depthDsv_.Get();
        target.width = kCaptureSize;
        target.height = kCaptureSize;
        for (int i = 0; i < 3; ++i) {
            target.clearColor[i] = SrgbToLinear(clearColor[i]);
        }
        target.clearColor[3] = clearColor[3];
        // viewKey = 0 = 「履歴を持たないビュー」。ここを 1..3 にするとメイン描画の
        // TAA / モーションブラー / RT テンポラルの前フレーム状態を 6 回踏み潰す
        target.viewKey = 0;

        CameraOverride cam;
        cam.view = ProbeFaceView(f, position);
        cam.position = position;
        cam.fovYDeg = 90.0f; // 正方形 RT なので aspect=1 → ProbeFaceProj と同じ行列になる
        cam.nearZ = nearZ;
        cam.farZ = farZ;
        render_.Render(world, device, path, shaders, resources, target, &cam, nullptr, nullptr);
    }

    // ---- プリフィルタ (EnvMapBaker と完全共有) ----
    // ★ラスタライザを既定へ戻してから渡す。直前に走ったパス (デカールの CULL_FRONT など) が
    //   残っていると、ベイクのフルスクリーン三角形が裏面として消えて焼き上がりが黒になる
    dc->RSSetState(nullptr);
    if (!env_.BakeFrom(device, shaders, out.captureSrv.Get(), out.env)) {
        MYE_LOG_ERROR("[probe] prefilter failed");
        return false;
    }
    out.valid = true;
    lastBakeMs_ = static_cast<float>(
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
            .count());
    MYE_LOG_INFO("[probe] baked at (%.2f, %.2f, %.2f): %d^2 x 6 faces, %.1f ms (CPU)", position.x,
                 position.y, position.z, kCaptureSize, lastBakeMs_);
    return true;
}

void ProbeBaker::Shutdown()
{
    env_.Shutdown();
    depthDsv_.Reset();
    depthTex_.Reset();
}

bool ProbeReadFaces(GraphicsDevice& device, const BakedProbe& probe, std::vector<float>& rgb,
                    int& size)
{
    rgb.clear();
    size = 0;
    if (!probe.valid || !probe.captureTex) {
        return false;
    }
    D3D11_TEXTURE2D_DESC td = {};
    probe.captureTex->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0; // ★TEXTURECUBE を残すと STAGING の生成が弾かれる
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device.Device()->CreateTexture2D(&td, nullptr, staging.GetAddressOf()))) {
        return false;
    }
    ID3D11DeviceContext* dc = device.Context();
    dc->CopyResource(staging.Get(), probe.captureTex.Get());

    const int n = static_cast<int>(td.Width);
    rgb.assign(static_cast<size_t>(6) * n * n * 3, 0.0f);
    for (int f = 0; f < 6; ++f) {
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        // MipLevels=1 なのでサブリソース番号は面番号そのもの
        if (FAILED(dc->Map(staging.Get(), static_cast<UINT>(f), D3D11_MAP_READ, 0, &mapped))) {
            rgb.clear();
            return false;
        }
        const auto* rows = static_cast<const uint8_t*>(mapped.pData);
        for (int y = 0; y < n; ++y) {
            const auto* px = reinterpret_cast<const PackedVector::HALF*>(rows + mapped.RowPitch * y);
            for (int x = 0; x < n; ++x) {
                const size_t o = ((static_cast<size_t>(f) * n + y) * n + x) * 3;
                rgb[o + 0] = PackedVector::XMConvertHalfToFloat(px[x * 4 + 0]);
                rgb[o + 1] = PackedVector::XMConvertHalfToFloat(px[x * 4 + 1]);
                rgb[o + 2] = PackedVector::XMConvertHalfToFloat(px[x * 4 + 2]);
            }
        }
        dc->Unmap(staging.Get(), static_cast<UINT>(f));
    }
    size = n;
    return true;
}

bool ProbeSeamCheck(const std::vector<float>& rgb, int size, ProbeSeamStats& out)
{
    out = {};
    if (size <= 1 || rgb.size() != static_cast<size_t>(6) * size * size * 3) {
        return false;
    }
    const float inv = 1.0f / static_cast<float>(size);
    double lumaSum = 0.0;
    for (size_t i = 0; i + 2 < rgb.size(); i += 3) {
        lumaSum += Luma(rgb[i], rgb[i + 1], rgb[i + 2]);
    }
    out.meanLuma = static_cast<float>(lumaSum / (6.0 * size * size));

    auto texel = [&](int f, int x, int y, float* c) {
        const size_t o = ((static_cast<size_t>(f) * size + y) * size + x) * 3;
        c[0] = rgb[o];
        c[1] = rgb[o + 1];
        c[2] = rgb[o + 2];
    };
    auto diff = [](const float* a, const float* b) {
        return (std::fabs(a[0] - b[0]) + std::fabs(a[1] - b[1]) + std::fabs(a[2] - b[2])) / 3.0f;
    };

    // ---- 分母: 面の内側で 1 テクセル進んだときの差 (継ぎ目と同じ歩幅) ----
    double interior = 0.0;
    int interiorN = 0;
    for (int f = 0; f < 6; ++f) {
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                float a[3] = {}, b[3] = {};
                texel(f, x, y, a);
                if (x + 1 < size) {
                    texel(f, x + 1, y, b);
                    interior += diff(a, b);
                    ++interiorN;
                }
                if (y + 1 < size) {
                    texel(f, x, y + 1, b);
                    interior += diff(a, b);
                    ++interiorN;
                }
            }
        }
    }
    out.meanInteriorDiff = (interiorN > 0) ? static_cast<float>(interior / interiorN) : 0.0f;

    double acc = 0.0;
    for (int f = 0; f < 6; ++f) {
        for (int edge = 0; edge < 4; ++edge) {
            for (int i = 0; i < size; ++i) {
                const float c = (static_cast<float>(i) + 0.5f) * inv;
                int x = 0, y = 0;
                float ou = 0.0f, ov = 0.0f; // 面の 1/2 テクセルぶん外側 = 隣の面の縁
                switch (edge) {
                case 0: x = 0;        y = i;        ou = -0.5f * inv;        ov = c; break;
                case 1: x = size - 1; y = i;        ou = 1.0f + 0.5f * inv;  ov = c; break;
                case 2: x = i;        y = 0;        ou = c; ov = -0.5f * inv;        break;
                default: x = i;       y = size - 1; ou = c; ov = 1.0f + 0.5f * inv;  break;
                }
                int f2 = 0;
                float u2 = 0.0f, v2 = 0.0f;
                if (!ProbeDirToFaceUv(ProbeFaceDir(f, ou, ov), f2, u2, v2) || f2 == f) {
                    continue;
                }
                const int x2 = std::clamp(static_cast<int>(u2 * size), 0, size - 1);
                const int y2 = std::clamp(static_cast<int>(v2 * size), 0, size - 1);
                float a[3] = {}, b[3] = {};
                texel(f, x, y, a);
                texel(f2, x2, y2, b);
                const float d = diff(a, b);
                acc += d;
                out.maxSeamDiff = std::max(out.maxSeamDiff, d);
                ++out.samples;
            }
        }
    }
    if (out.samples == 0) {
        return false;
    }
    out.meanSeamDiff = static_cast<float>(acc / out.samples);
    // 完全な単色キューブ (= 内側の差が 0) では比が定義できない。継ぎ目も 0 なら
    // 「継ぎ目は面の中と同じ」= 1 と答えるのが素直 (0/0 を無限大にすると偽陽性になる)
    out.seamRatio = (out.meanInteriorDiff > 1e-9f)
        ? out.meanSeamDiff / out.meanInteriorDiff
        : ((out.meanSeamDiff > 1e-9f) ? 1e9f : 1.0f);
    return true;
}

bool ProbeWriteFacesPng(const std::vector<float>& rgb, int size, const std::wstring& path)
{
    if (size <= 0 || rgb.size() != static_cast<size_t>(6) * size * size * 3) {
        return false;
    }
    // 十字配置。**この並びは隣り合う面が画像上でも隣り合う** — 中段の 4 枚は水平に
    // 一周するパノラマになり、上下は +Z の上端 / 下端と繋がる。面の向きが壊れていれば
    // 継ぎ目の段差として一目で分かる (-1 = 空きセル)
    const int kCell[3][4] = {
        { -1, 2, -1, -1 },  // +Y
        { 1, 4, 0, 5 },     // -X +Z +X -Z
        { -1, 3, -1, -1 },  // -Y
    };
    const int w = size * 4;
    const int h = size * 3;
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4, 0);
    for (int cy = 0; cy < 3; ++cy) {
        for (int cx = 0; cx < 4; ++cx) {
            const int f = kCell[cy][cx];
            for (int y = 0; y < size; ++y) {
                for (int x = 0; x < size; ++x) {
                    const size_t d = ((static_cast<size_t>(cy) * size + y) * w + cx * size + x) * 4;
                    if (f < 0) {
                        px[d + 0] = px[d + 1] = px[d + 2] = 24; // 空きセルは暗い灰
                    } else {
                        const size_t o = ((static_cast<size_t>(f) * size + y) * size + x) * 3;
                        px[d + 0] = EncodeLdr(rgb[o + 0]);
                        px[d + 1] = EncodeLdr(rgb[o + 1]);
                        px[d + 2] = EncodeLdr(rgb[o + 2]);
                    }
                    px[d + 3] = 255;
                }
            }
        }
    }
    return WritePngRGBA(path, px.data(), w, h, w * 4);
}

} // namespace mye
