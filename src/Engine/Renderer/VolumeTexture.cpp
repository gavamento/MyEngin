#include "Engine/Renderer/VolumeTexture.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <DirectXPackedVector.h>

#include "Engine/Core/Log.h"
#include "Engine/Renderer/FroxelPass.h" // M57b: 注入パスの実測 (プローブの 2 段目)
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
    case DXGI_FORMAT_R8_UNORM: // M65d: 音響の残光 (1 チャンネル・UAV 無し)
        return 1;
    default:
        return 0;
    }
}

} // namespace

bool VolumeTexture::Create(GraphicsDevice& device, int width, int height, int depth,
                           DXGI_FORMAT format, bool withUav)
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
    // M65d: UAV は要求されたときだけ。R8_UNORM のように FL11_0 の typed UAV 必須リストの
    // 外にあるフォーマットは、ここでビットを立てると **ビュー作成が落ちて Create ごと失敗**する
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (withUav) {
        td.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
    }

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

    if (FAILED(dev->CreateShaderResourceView(tex_.Get(), &sd, srv_.GetAddressOf()))) {
        // ★SRV 作成の失敗も必ずログに残す (M65d)。「絵が出ない」だけだと
        //   バインドし忘れと区別が付かない
        MYE_LOG_ERROR("VolumeTexture: SRV creation failed (%dx%dx%d, format %d)", width, height,
                      depth, static_cast<int>(format));
        Release();
        return false;
    }
    if (withUav && FAILED(dev->CreateUnorderedAccessView(tex_.Get(), &ud, uav_.GetAddressOf()))) {
        // ★ここで落ちるのが「FL11_0 の typed 3D UAV が使えない」ケース。
        //   フォーマットを変える (R32_FLOAT 4 枚に分ける等) か withUav=false にする、
        //   という設計変更の合図なので、黙って null を返さずログに残す
        MYE_LOG_ERROR("VolumeTexture: UAV creation failed (%dx%dx%d, format %d は typed 3D UAV "
                      "非対応か)",
                      width, height, depth, static_cast<int>(format));
        Release();
        return false;
    }

    width_ = width;
    height_ = height;
    depth_ = depth;
    format_ = format;
    hasUav_ = withUav;
    return true;
}

void VolumeTexture::Resize(GraphicsDevice& device, int width, int height, int depth,
                           DXGI_FORMAT format, bool withUav)
{
    if (width == width_ && height == height_ && depth == depth_ && format == format_
        && withUav == hasUav_) {
        return;
    }
    Create(device, width, height, depth, format, withUav);
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
    hasUav_ = true;
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
    } else if (format_ == DXGI_FORMAT_R8_UNORM) {
        // M65d: 1 チャンネル。UNORM の復号は「/255」で厳密 (0 と 255 が 0.0 と 1.0)
        out[0] = static_cast<float>(*base) / 255.0f;
        out[1] = out[2] = out[3] = 0.0f;
    } else {
        const float* f = reinterpret_cast<const float*>(base);
        for (int i = 0; i < 4; ++i) {
            out[i] = f[i];
        }
    }
    dc->Unmap(staging.Get(), 0);
    return true;
}

bool VolumeTexture::ReadbackBytes(GraphicsDevice& device, std::vector<uint8_t>& out) const
{
    if (!tex_ || format_ != DXGI_FORMAT_R8_UNORM) {
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
    out.assign(static_cast<size_t>(width_) * height_ * depth_, 0u);
    for (int z = 0; z < depth_; ++z) {
        for (int y = 0; y < height_; ++y) {
            // ★RowPitch / DepthPitch はドライバのパディング込み。要求寸法で歩かないこと
            const uint8_t* row = static_cast<const uint8_t*>(mapped.pData)
                + static_cast<size_t>(z) * mapped.DepthPitch
                + static_cast<size_t>(y) * mapped.RowPitch;
            std::memcpy(out.data() + (static_cast<size_t>(z) * height_ + y) * width_, row,
                        static_cast<size_t>(width_));
        }
    }
    dc->Unmap(staging.Get(), 0);
    return true;
}

bool VolumeTexture::ReadbackAll(GraphicsDevice& device, std::vector<float>& out) const
{
    if (!tex_) {
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
    out.assign(static_cast<size_t>(width_) * height_ * depth_ * 4, 0.0f);
    for (int z = 0; z < depth_; ++z) {
        for (int y = 0; y < height_; ++y) {
            // RowPitch / DepthPitch はドライバのパディング込み。要求寸法で歩かないこと
            const uint8_t* row = static_cast<const uint8_t*>(mapped.pData)
                + static_cast<size_t>(z) * mapped.DepthPitch
                + static_cast<size_t>(y) * mapped.RowPitch;
            float* dst = out.data()
                + ((static_cast<size_t>(z) * height_ + y) * static_cast<size_t>(width_)) * 4;
            if (format_ == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                const DirectX::PackedVector::HALF* h
                    = reinterpret_cast<const DirectX::PackedVector::HALF*>(row);
                for (int i = 0; i < width_ * 4; ++i) {
                    dst[i] = DirectX::PackedVector::XMConvertHalfToFloat(h[i]);
                }
            } else {
                std::memcpy(dst, row, static_cast<size_t>(width_) * 4 * sizeof(float));
            }
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

// ---- M57b: 注入パスの実測 + 正しさ確認 ----
//
// M57a が測ったのは「空の CS」= 下限であって、注入 (ライト 16 本ループ +
// SampleShadowAtlas) のコストではなかった。ここが M57c の設計
// (テンポラルを足せるか / golden を CI に載せるか) の入力になる。
//
// 正しさの確認も兼ねる。**消費者が居ないサブなので絵からは何も分からない**ぶん、
// 「CB のレイアウトが HLSL と合っているか」までここで潰しておく — レイアウトが
// ずれると密度が乱数になるので、消散係数の厳密比較 1 本で全部引っかかる。
// 戻り値: 0 = OK / 1 = 値が食い違った / 2 = 計測に至らなかった
int RunFroxelInjectProbe(GraphicsDevice& device, ShaderManager& shaders, int iterations)
{
    FroxelPass pass;
    if (!pass.Init(device, shaders)) {
        std::fprintf(stderr, "[froxel-probe] ERROR: froxel_inject.cs failed to compile\n");
        return 2;
    }

    // 裸の RenderView を手で組む (シーンも ECS も無い)。射影は決定的撮影と同じ 960x540。
    // ★projNoJitter を埋めないと FroxelPass が「正射影」と判定して降りる —
    //   ここを空のままにすると 0ms が出て「速い」と誤読する
    RenderView view;
    view.width = 960;
    view.height = 540;
    view.nearZ = 0.1f;
    view.farZ = 200.0f;
    view.cameraPos = { 0.0f, 0.0f, 0.0f };
    DirectX::XMStoreFloat4x4(&view.view, DirectX::XMMatrixIdentity()); // 原点から +Z を向く
    DirectX::XMStoreFloat4x4(
        &view.projNoJitter,
        DirectX::XMMatrixPerspectiveFovLH(DirectX::XM_PI / 3.0f, 960.0f / 540.0f, view.nearZ,
                                          view.farZ));
    view.proj = view.projNoJitter;

    // ライトは光軸上に等間隔で 16 本。**位置を決定論的に並べる**のは、
    // 本数を変えたときに「同じ空間を何本が照らすか」だけが変わるようにするため
    SceneLightData lights;
    lights.ambient = { 0.1f, 0.1f, 0.1f };
    for (int i = 0; i < kMaxLights; ++i) {
        GpuLight& g = lights.lights[i];
        g.type = 1; // 点光源 (スポットは分岐が 1 本増えるだけなので上限の測定にはならない)
        g.position = { 0.0f, 0.0f, 8.0f + 2.0f * static_cast<float>(i) };
        g.range = 6.0f;
        g.color = { 1.0f, 1.0f, 1.0f };
        g.intensity = 2.0f;
    }

    FroxelSettings settings; // 既定 = 予約表の froxelDensity / froxelAnisotropy と同値
    const float sigmaS = settings.density * settings.scatterAlbedo;
    const float ambientFloor = lights.ambient.x * sigmaS; // ライトが届かないセルの値

    int rc = 0;
    lights.count = 1;
    // M57c: 値の照合はジッタ 0.5 (= スライス中心) で撮る。CPU 期待値側の
    // SliceCenterViewDepth と代表点を揃えるため
    if (!pass.Inject(device, shaders, view, lights, settings, 0.5f)) {
        std::fprintf(stderr, "[froxel-probe] ERROR: inject dispatch was skipped\n");
        return 2;
    }

    // ---- 正しさ (ライト 1 本) ----
    // ★「0 より大きいか」では検査にならない — 係数を 10 倍間違えても通ってしまう。
    //   CPU 側で**同じ式**を組んで期待値を出し、half の丸めぶんの相対誤差で照合する。
    //   これ 1 本で「CB のレイアウト」「逆射影によるセル中心の復元」「減衰」「位相関数」が
    //   まとめて検査される (どれが崩れても数値が合わなくなる)
    const int slices = pass.Volume().Depth();
    const float gridFar = (std::min)(view.farZ, settings.maxDistance);
    auto expectedInscatter = [&](int cx, int cy, int cz) {
        const float viewZ = froxel::SliceCenterViewDepth(cz, slices, view.nearZ, gridFar);
        const float ndcX
            = ((static_cast<float>(cx) + 0.5f) / static_cast<float>(pass.Volume().Width())) * 2.0f
            - 1.0f;
        const float ndcY = 1.0f
            - ((static_cast<float>(cy) + 0.5f) / static_cast<float>(pass.Volume().Height()))
                * 2.0f;
        // view 行列が単位 = ワールド座標と view 座標が一致する構成にしてある
        const float px = ndcX * viewZ / view.projNoJitter._11;
        const float py = ndcY * viewZ / view.projNoJitter._22;
        const float pz = viewZ;
        const float camLen = std::sqrt(px * px + py * py + pz * pz);
        const float cam[3] = { -px / camLen, -py / camLen, -pz / camLen }; // セル → カメラ (原点)
        float sum = lights.ambient.x;
        for (int i = 0; i < lights.count; ++i) {
            const GpuLight& g = lights.lights[i];
            const float tx = g.position.x - px;
            const float ty = g.position.y - py;
            const float tz = g.position.z - pz;
            const float dist = std::sqrt(tx * tx + ty * ty + tz * tz);
            const float dx = tx / dist;
            const float dy = ty / dist;
            const float dz = tz / dist;
            float d = 1.0f - dist / g.range;
            d = (d < 0.0f) ? 0.0f : ((d > 1.0f) ? 1.0f : d);
            const float atten = d * d;
            const float cosTheta = -dx * cam[0] - dy * cam[1] - dz * cam[2];
            sum += g.color.x * g.intensity * atten
                * froxel::HenyeyGreenstein(cosTheta, settings.anisotropy);
        }
        return sum * sigmaS;
    };
    const int litZ = static_cast<int>(
        froxel::ViewDepthToSlice(lights.lights[0].position.z, slices, view.nearZ, gridFar));
    const int probes[2][3] = { { pass.Volume().Width() / 2, pass.Volume().Height() / 2, litZ },
                               { 0, 0, 0 } };
    float lit[4] = {};
    float dark[4] = {};
    if (!pass.Volume().ReadbackTexel(device, probes[0][0], probes[0][1], probes[0][2], lit)
        || !pass.Volume().ReadbackTexel(device, probes[1][0], probes[1][1], probes[1][2], dark)) {
        std::fprintf(stderr, "[froxel-probe] ERROR: inject readback failed\n");
        return 2;
    }
    const float litExpect = expectedInscatter(probes[0][0], probes[0][1], probes[0][2]);
    const float darkExpect = expectedInscatter(probes[1][0], probes[1][1], probes[1][2]);
    // ① 消散係数は一様密度 = 全セルで σ_t。**CB のレイアウトが 1 バイトでもずれると
    //    ここが真っ先に壊れる**
    const bool extOk = std::fabs(lit[3] - settings.density) < settings.density * 0.01f
        && std::fabs(dark[3] - settings.density) < settings.density * 0.01f;
    // ② ライトの居るセルが CPU の期待値と一致する (= 減衰と位相関数が効いている)
    const bool litOk = std::fabs(lit[0] - litExpect) < litExpect * 0.02f;
    // ③ 射程外のセルはアンビエントちょうど (= ライトの寄与が全セルに漏れていない)
    const bool darkOk = std::fabs(dark[0] - ambientFloor) < ambientFloor * 0.02f
        && std::fabs(darkExpect - ambientFloor) < ambientFloor * 1e-4f;
    if (!extOk || !litOk || !darkOk) {
        std::printf("[froxel-probe]   INJECT MISMATCH: lit=%g (expected %g) / dark=%g "
                    "(expected %g) / sigmaT=%g,%g (expected %g)\n",
                    lit[0], litExpect, dark[0], ambientFloor, lit[3], dark[3], settings.density);
        rc = 1;
    } else {
        std::printf("[froxel-probe]   inject values OK: lit %g vs CPU %g / floor %g / sigmaT %g\n",
                    lit[0], litExpect, dark[0], lit[3]);
    }

    // ---- 速度 (ライト本数を変えて) ----
    // 注入のコストは「セル数 × ライト本数」で伸びるはずで、そこが線形なら
    // M57c は解像度でも本数でも予算を作れる。折れていたら別の支配項がある
    std::printf("[froxel-probe] inject %dx%dx%d (%s):\n", pass.Volume().Width(),
                pass.Volume().Height(), pass.Volume().Depth(), extOk && litOk && darkOk
                    ? "values OK"
                    : "VALUES BROKEN");
    // lights 0 = セルあたりの固定費 (逆射影 + 高度密度 + 正規化) の切り出し。
    // これが無いと「本数を増やすと重い」だけが分かって、どちらを削ればよいか決まらない
    for (const int lc : { 0, 1, 4, 16 }) {
        lights.count = lc;
        for (int i = 0; i < 2; ++i) { // ウォームアップ
            pass.Inject(device, shaders, view, lights, settings, 0.5f);
        }
        float dummy[4] = {};
        pass.Volume().ReadbackTexel(device, 0, 0, 0, dummy); // Map で完全同期
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < iterations; ++i) {
            pass.Inject(device, shaders, view, lights, settings, 0.5f);
        }
        pass.Volume().ReadbackTexel(device, 0, 0, 0, dummy);
        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count()
            / static_cast<double>(iterations);
        std::printf("[froxel-probe]   lights %2d: %8.3f ms/dispatch (gpu %.3f ms)\n", lc, ms,
                    static_cast<double>(pass.InjectGpuMs()));
    }

    // ---- M57c: テンポラル + 前方積分 ----
    // ★viewKey を 2 にしておく — 0 (AssetPreview) は履歴を持たない規約なので、
    //   そのままだとテンポラルが降りて「積分だけ」を測ってしまう
    view.viewKey = 2;
    view.prevViewProjValid = 1;
    DirectX::XMStoreFloat4x4(&view.prevViewProj,
                             DirectX::XMLoadFloat4x4(&view.view)
                                 * DirectX::XMLoadFloat4x4(&view.projNoJitter));
    lights.count = 4;
    uint32_t serial = 0;
    for (int i = 0; i < 4; ++i) { // ウォームアップ (履歴を「有効」にしてから測る)
        view.viewFrameIndex = serial++;
        pass.Render(device, shaders, view, lights, settings);
    }
    float dummy[4] = {};
    pass.Integrated().ReadbackTexel(device, 0, 0, 0, dummy);
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iterations; ++i) {
        view.viewFrameIndex = serial++;
        pass.Render(device, shaders, view, lights, settings);
    }
    pass.Integrated().ReadbackTexel(device, 0, 0, 0, dummy);
    const auto t1 = std::chrono::steady_clock::now();
    const double fullMs = std::chrono::duration<double, std::milli>(t1 - t0).count()
        / static_cast<double>(iterations);
    std::printf("[froxel-probe] full frame (inject+temporal+integrate, 4 lights): "
                "%8.3f ms/frame (gpu %.3f + %.3f + %.3f ms)\n",
                fullMs, static_cast<double>(pass.InjectGpuMs()),
                static_cast<double>(pass.TemporalGpuMs()),
                static_cast<double>(pass.IntegrateGpuMs()));

    // ---- M57c: 積分の解析検算 ----
    // 一様媒質 (heightFalloff = 0) なので、**最奥スライスの透過率は厳密に
    // e^{-σ_t·(far-near)}** になる。スライスの厚みを 1 枚ずらす / 中心差で刻む /
    // 手前と奥を取り違える、どの間違いもこの 1 本で落ちる (絵では絶対に分からない)
    float deep[4] = {};
    const float gridFarZ = (std::min)(view.farZ, settings.maxDistance);
    if (pass.Integrated().ReadbackTexel(device, 0, 0, pass.Integrated().Depth() - 1, deep)) {
        const float expectT = std::exp(-settings.density * (gridFarZ - view.nearZ));
        const bool transOk = std::fabs(deep[3] - expectT) < expectT * 0.01f;
        // 内向き散乱は「単調非減少で 0 より大きい」ことだけを見る (値そのものは
        // --froxel-dump が全セルを CPU 参照と突き合わせる)
        const bool scatterOk = deep[0] > 0.0f;
        if (!transOk || !scatterOk) {
            std::printf("[froxel-probe]   INTEGRATE MISMATCH: farT=%g (expected %g) / "
                        "inscatter=%g\n",
                        deep[3], expectT, deep[0]);
            rc = 1;
        } else {
            std::printf("[froxel-probe]   integrate values OK: far transmittance %g vs "
                        "analytic %g / accumulated inscatter %g\n",
                        deep[3], expectT, deep[0]);
        }
    } else {
        std::fprintf(stderr, "[froxel-probe] ERROR: integrate readback failed\n");
        return 2;
    }
    return rc;
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

    // M57b: 注入パスの実測 (M57a の clear は下限でしかなかった)。
    // 計測に至らなかった (rc=2) 場合はそのまま返す — 「0ms だから速い」と読ませない
    const int injectRc = RunFroxelInjectProbe(device, shaders, iterations);
    if (injectRc == 2) {
        return 2;
    }
    rc = (rc != 0) ? rc : injectRc;

    std::printf("[froxel-probe] %s\n", rc == 0 ? "PASS" : "FAIL");
    return rc;
}

} // namespace mye
