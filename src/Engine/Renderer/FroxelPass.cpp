#include "Engine/Renderer/FroxelPass.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "Engine/Core/Log.h"
#include "Engine/Renderer/GpuBufferUtil.h"
#include "Engine/Renderer/GraphicsDevice.h"
#include "Engine/Renderer/ShaderManager.h"

using namespace DirectX;

namespace mye {
namespace {

// froxel_inject.cs.hlsl の FroxelInjectCB (b0) とレイアウト一致。
// ★並びを変えたら HLSL 側も同じ順で直すこと — 16 バイト境界だけ合っていれば
//   コンパイルは通り、「霧の色だけがおかしい」形で静かに壊れる
struct FroxelInjectCB {
    XMFLOAT4X4 invView = {}; // transpose(inverse(view))
    XMFLOAT3 cameraPos = { 0, 0, 0 };
    float density = 0.0f;
    uint32_t gridSize[3] = { 0, 0, 0 };
    uint32_t pad0 = 0;
    float nearZ = 0.0f;
    float farZ = 0.0f;
    float invProj00 = 0.0f;
    float invProj11 = 0.0f;
    float anisotropy = 0.0f;
    float scatterAlbedo = 0.0f;
    float heightFalloff = 0.0f;
    float baseHeight = 0.0f;
    XMFLOAT3 ambient = { 0, 0, 0 };
    int32_t lightCount = 0;
    int32_t shadowAtlasEnabled = 0;
    float shadowAtlasTexel = 0.0f;
    float pad1[2] = { 0.0f, 0.0f };
    GpuLight lights[kMaxLights] = {};
    ShadowTileCB shadowTiles[kMaxShadowTiles] = {};
};
static_assert(sizeof(FroxelInjectCB) == 2720, "HLSL の FroxelInjectCB と一致させること");

} // namespace

bool FroxelPass::Init(GraphicsDevice& device, ShaderManager& shaders)
{
    if (inited_) {
        return true;
    }
    injectCS_ = shaders.LoadCompute("froxel_inject.cs");
    ShaderProgram* prog = shaders.Get(injectCS_);
    if (!prog || !prog->valid || !prog->cs) {
        MYE_LOG_ERROR("FroxelPass: froxel_inject.cs のコンパイルに失敗した");
        return false;
    }
    if (!gpubuf::CreateConstant(device.Device(), sizeof(FroxelInjectCB), cb_)) {
        return false;
    }
    // SampleShadowAtlas 用の比較サンプラ。DeferredPath の shadowSampler_ と同じ設定
    // (LESS_EQUAL / CLAMP) — 面の影と霧のビームが別のサンプラで走ると、
    // 「柱の影の縁で霧だけが 1 テクセルずれる」という気付きにくい食い違いになる
    D3D11_SAMPLER_DESC cs = {};
    cs.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    cs.AddressU = cs.AddressV = cs.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    cs.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
    cs.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device.Device()->CreateSamplerState(&cs, shadowSampler_.GetAddressOf()))) {
        return false;
    }
    timer_.Init(device); // 失敗しても 0ms を返すだけ (計測が消えるだけで描画は続く)
    inited_ = true;
    return true;
}

void FroxelPass::Shutdown()
{
    scatter_.Release();
    shadowSampler_.Reset();
    cb_.Reset();
    inited_ = false;
    orthoWarned_ = false;
}

bool FroxelPass::Inject(GraphicsDevice& device, ShaderManager& shaders, const RenderView& view,
                        const SceneLightData& lights, const FroxelSettings& settings)
{
    if (!inited_ || view.width <= 0 || view.height <= 0) {
        return false;
    }
    ShaderProgram* prog = shaders.Get(injectCS_);
    if (!prog || !prog->valid || !prog->cs) {
        return false;
    }
    // ★グリッドは**ジッタ非適用の射影**で組む (M55b の規約)。ジッタ込みで組むと
    //   グリッドがフレーム毎に半セル揺れ、M57c の再投影が自分自身とずれる。
    // ★正射影 (SceneView の Ortho トグル) は視錐台がスラブなので、
    //   「NDC.xy と view 深度から view 座標を復元する」この式が成り立たない。
    //   無理に走らせると霧が視点の後ろへ回り込むので、素直に降りる
    const XMFLOAT4X4& p = view.projNoJitter;
    if (std::fabs(p._34) < 1e-6f || std::fabs(p._11) < 1e-6f || std::fabs(p._22) < 1e-6f) {
        if (!orthoWarned_) {
            orthoWarned_ = true;
            MYE_LOG_WARN("FroxelPass: 透視射影ではないビューなので注入を飛ばす "
                         "(正射影のフロクセルは M57 の対象外)");
        }
        return false;
    }

    scatter_.Resize(device, froxel::kGridX, froxel::kGridY, froxel::kGridZ,
                    DXGI_FORMAT_R16G16B16A16_FLOAT);
    if (!scatter_.UAV()) {
        return false;
    }

    FroxelInjectCB c;
    XMStoreFloat4x4(&c.invView,
                    XMMatrixTranspose(XMMatrixInverse(nullptr, XMLoadFloat4x4(&view.view))));
    c.cameraPos = view.cameraPos;
    c.density = (std::max)(settings.density, 0.0f);
    c.gridSize[0] = static_cast<uint32_t>(scatter_.Width());
    c.gridSize[1] = static_cast<uint32_t>(scatter_.Height());
    c.gridSize[2] = static_cast<uint32_t>(scatter_.Depth());
    // グリッドの奥行きは「カメラの far」ではなく maxDistance でも切る。
    // far=1000m をそのまま 64 スライスへ割ると、指数分布でも近景が粗くなりすぎる
    c.nearZ = (view.nearZ > 1e-3f) ? view.nearZ : 1e-3f;
    const float wanted = (std::min)(view.farZ, (std::max)(settings.maxDistance, 1.0f));
    c.farZ = (wanted > c.nearZ * 1.01f) ? wanted : c.nearZ * 1.01f;
    c.invProj00 = 1.0f / p._11;
    c.invProj11 = 1.0f / p._22;
    c.anisotropy = settings.anisotropy;
    c.scatterAlbedo = (std::max)(settings.scatterAlbedo, 0.0f);
    // M43a のハイトフォグと同じプロファイルを共有する (0 = 一様)。
    // フォグが無効なシーン (fogMode<0) でも高度パラメータ自体は既定 0 なので害はない
    c.heightFalloff = (std::max)(view.fogHeightFalloff, 0.0f);
    c.baseHeight = view.fogBaseHeight;
    c.ambient = lights.ambient;
    c.lightCount = (std::min)(lights.count, static_cast<int32_t>(kMaxLights));
    std::memcpy(c.lights, lights.lights, sizeof(c.lights));
    const bool useAtlas = settings.useLocalShadows && view.shadowAtlasSRV != nullptr
        && view.shadowTileCount > 0;
    c.shadowAtlasEnabled = useAtlas ? 1 : 0;
    c.shadowAtlasTexel = view.shadowAtlasTexel;
    if (useAtlas) {
        FillShadowTilesCB(view, c.shadowTiles); // M54e と同じ転置 1 本きり
    }

    ID3D11DeviceContext* dc = device.Context();
    gpubuf::UploadCB(dc, cb_.Get(), c);

    timer_.Begin(device);
    ID3D11Buffer* cbs[1] = { cb_.Get() };
    ID3D11ShaderResourceView* srvs[1] = { useAtlas ? view.shadowAtlasSRV : nullptr };
    ID3D11SamplerState* samps[1] = { shadowSampler_.Get() };
    ID3D11UnorderedAccessView* uavs[1] = { scatter_.UAV() };
    dc->CSSetShader(prog->cs.Get(), nullptr, 0);
    dc->CSSetConstantBuffers(0, 1, cbs);
    dc->CSSetShaderResources(0, 1, srvs);
    dc->CSSetSamplers(0, 1, samps);
    dc->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    dc->Dispatch(
        static_cast<UINT>(froxel::DispatchGroups(scatter_.Width(), froxel::kGroupSize)),
        static_cast<UINT>(froxel::DispatchGroups(scatter_.Height(), froxel::kGroupSize)),
        static_cast<UINT>(scatter_.Depth()));

    // ★RtPasses::UnbindCompute と同じ理由で必ず剥がす。ここを省くと、この後の
    //   描画パスが同じシャドウアトラスを PS の SRV として張った瞬間に D3D が
    //   「CS の SRV に居るぞ」と警告を出して片方を黙って外す
    ID3D11ShaderResourceView* nullSrvs[1] = {};
    ID3D11UnorderedAccessView* nullUavs[1] = {};
    dc->CSSetShaderResources(0, 1, nullSrvs);
    dc->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);
    dc->CSSetShader(nullptr, nullptr, 0);
    timer_.End(device);
    return true;
}

bool FroxelPass::ReadbackStats(GraphicsDevice& device, FroxelVolumeStats& out,
                              std::vector<float>* rawOut) const
{
    out = FroxelVolumeStats();
    std::vector<float> local;
    std::vector<float>& cells = rawOut ? *rawOut : local;
    if (!scatter_.ReadbackAll(device, cells) || cells.empty()) {
        return false;
    }
    const int w = scatter_.Width();
    const int h = scatter_.Height();
    const int d = scatter_.Depth();
    out.cells = w * h * d;
    out.minExtinction = cells[3];
    out.maxExtinction = cells[3];
    out.minInscatter = cells[0] + cells[1] + cells[2];

    // ---- 1 周目: 最小値を含む素の集計 ----
    for (int i = 0; i < out.cells; ++i) {
        const float* t = cells.data() + static_cast<size_t>(i) * 4;
        const float lum = t[0] + t[1] + t[2];
        out.sumInscatter += lum;
        out.minInscatter = (std::min)(out.minInscatter, lum);
        out.maxInscatter
            = (std::max)(out.maxInscatter, (std::max)(t[0], (std::max)(t[1], t[2])));
        out.minExtinction = (std::min)(out.minExtinction, t[3]);
        out.maxExtinction = (std::max)(out.maxExtinction, t[3]);
    }

    // ---- 2 周目: 「ライトが届いたセル」を数える ----
    // ★しきい値を 0 にしてはいけない — アンビエント項 (等方散乱の下駄) が全セルに
    //   乗っているので、0 と比べると常に 100% になって何も測れない。
    //   ボリューム内の**最小値**を床にすると、一様密度のシーンではそれがちょうど
    //   アンビエントぶんになり、超えたセル = 局所ライトが届いたセルになる
    const float floorLum = out.minInscatter;
    out.litMin[0] = w;
    out.litMin[1] = h;
    out.litMin[2] = d;
    for (int z = 0; z < d; ++z) {
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const float* t = cells.data() + ((static_cast<size_t>(z) * h + y) * w + x) * 4;
                if (t[0] + t[1] + t[2] > floorLum) {
                    ++out.litCells;
                    const int c[3] = { x, y, z };
                    for (int k = 0; k < 3; ++k) {
                        out.litMin[k] = (std::min)(out.litMin[k], c[k]);
                        out.litMax[k] = (std::max)(out.litMax[k], c[k]);
                    }
                }
            }
        }
    }
    return true;
}

void FroxelPass::DebugDumpAB(GraphicsDevice& device, ShaderManager& shaders, const RenderView& view,
                             const SceneLightData& lights, const FroxelSettings& settings)
{
    // ★消費者 (積分 = M57c / 合成 = M57e) がまだ居ないサブなので、絵からは
    //   「グリッドに何が入ったか」が 1 画素も分からない。ここで読み戻して数えるのが
    //   唯一の機械的な確認手段になる。A/B (影あり / 影なし) を**同じ実行の中で**
    //   撮るのは、2 回起動すると別プロセスの WARP 差やタイミング差が混ざるため
    // ★GpuTimer は 6 フレームのリングで、結果は同じスロットが一周してから回収される。
    //   撮影が 6 フレームで終わる実行では 1 件も確定せず **0.000 ms が出る** ので、
    //   ここで空回しして必ず 1 件確定させる (0ms を「速い」と読ませないため)
    for (int i = 0; i < 8; ++i) {
        Inject(device, shaders, view, lights, settings);
    }
    FroxelVolumeStats withShadow;
    std::vector<float> a;
    if (!Inject(device, shaders, view, lights, settings)
        || !ReadbackStats(device, withShadow, &a)) {
        MYE_LOG_WARN("Froxel dump: 読み戻しに失敗した (注入が走っていない可能性)");
        return;
    }
    FroxelSettings noShadow = settings;
    noShadow.useLocalShadows = false;
    FroxelVolumeStats without;
    std::vector<float> b;
    const bool abOk = Inject(device, shaders, view, lights, noShadow)
        && ReadbackStats(device, without, &b) && a.size() == b.size();

    MYE_LOG_INFO("Froxel dump: %dx%dx%d = %d cells / inject %.3f ms (GpuTimer) / "
                 "extinction %.5f..%.5f",
                 scatter_.Width(), scatter_.Height(), scatter_.Depth(), withShadow.cells,
                 static_cast<double>(timer_.Milliseconds()),
                 static_cast<double>(withShadow.minExtinction),
                 static_cast<double>(withShadow.maxExtinction));
    MYE_LOG_INFO("Froxel dump:   lit cells %d (%.2f%%) / max inscatter %.5f / mean %.6f / "
                 "lit bbox x[%d..%d] y[%d..%d] z[%d..%d]",
                 withShadow.litCells,
                 withShadow.cells > 0
                     ? 100.0 * withShadow.litCells / static_cast<double>(withShadow.cells)
                     : 0.0,
                 static_cast<double>(withShadow.maxInscatter),
                 withShadow.cells > 0 ? withShadow.sumInscatter / withShadow.cells : 0.0,
                 withShadow.litMin[0], withShadow.litMax[0], withShadow.litMin[1],
                 withShadow.litMax[1], withShadow.litMin[2], withShadow.litMax[2]);
    if (abOk) {
        // 影ありのセルは影なしのセル以下にしかならない。差が出たセル数 = アトラスが
        // 実際に暗くした体積 = 「ビームが SampleShadowAtlas 由来である」ことの証拠
        int darkened = 0;
        double sumA = 0.0;
        double sumB = 0.0;
        for (size_t i = 0; i + 3 < a.size(); i += 4) {
            const double la = static_cast<double>(a[i]) + a[i + 1] + a[i + 2];
            const double lb = static_cast<double>(b[i]) + b[i + 1] + b[i + 2];
            sumA += la;
            sumB += lb;
            if (la < lb) {
                ++darkened;
            }
        }
        MYE_LOG_INFO("Froxel dump:   shadow A/B: %d cells darkened by the atlas (%.2f%%), "
                     "inscatter sum %.1f -> %.1f (%.1f%% remains), lit %d -> %d",
                     darkened,
                     withShadow.cells > 0 ? 100.0 * darkened / withShadow.cells : 0.0, sumB, sumA,
                     (sumB > 0.0) ? 100.0 * sumA / sumB : 0.0, without.litCells,
                     withShadow.litCells);
    }
    // 影ありの状態へ戻して抜ける (この後のフレームが A/B の片割れを掴まないように)
    Inject(device, shaders, view, lights, settings);
}

} // namespace mye
