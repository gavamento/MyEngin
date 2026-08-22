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
    float sliceJitter = 0.5f; // M57c (0.5 = ジッタ無し = M57b とビット一致)
    float pad1 = 0.0f;
    GpuLight lights[kMaxLights] = {};
    ShadowTileCB shadowTiles[kMaxShadowTiles] = {};
};
static_assert(sizeof(FroxelInjectCB) == 2720, "HLSL の FroxelInjectCB と一致させること");

// M57c: froxel_temporal.cs.hlsl と froxel_integrate.cs.hlsl が**共有する** CB (b0)。
// 2 本に分けても片方は行列を読まないだけで、アップロードが 1 回増えるほかに違いが無い
struct FroxelPostCB {
    XMFLOAT4X4 invView = {};      // transpose(inverse(view))
    XMFLOAT4X4 prevViewProj = {}; // transpose(前フレームの view * projNoJitter)
    uint32_t gridSize[3] = { 0, 0, 0 };
    uint32_t histValid = 0;
    float nearZ = 0.0f;
    float farZ = 0.0f;
    float invProj00 = 0.0f;
    float invProj11 = 0.0f;
    float sliceJitter = 0.5f;
    float feedback = 0.0f;
    float pad[2] = { 0.0f, 0.0f };
};
static_assert(sizeof(FroxelPostCB) == 176, "HLSL の FroxelPostCB と一致させること");

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
    // M57c: テンポラルと積分。**積分が無いと霧は誰にも見えない**ので、こちらが
    // 落ちたら Init 自体を失敗にする (注入だけ走らせても 7MB を捨てるだけ)
    temporalCS_ = shaders.LoadCompute("froxel_temporal.cs");
    integrateCS_ = shaders.LoadCompute("froxel_integrate.cs");
    ShaderProgram* tprog = shaders.Get(temporalCS_);
    ShaderProgram* iprog = shaders.Get(integrateCS_);
    if (!tprog || !tprog->valid || !tprog->cs || !iprog || !iprog->valid || !iprog->cs) {
        MYE_LOG_ERROR("FroxelPass: froxel_temporal.cs / froxel_integrate.cs のコンパイルに失敗した");
        return false;
    }
    if (!gpubuf::CreateConstant(device.Device(), sizeof(FroxelInjectCB), cb_)
        || !gpubuf::CreateConstant(device.Device(), sizeof(FroxelPostCB), postCb_)) {
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
    // M57c: 履歴の再投影用。統合契約 予約 2 の「サンプラは増やさない」は PS のスロットの
    // 話で、CS は別のバインド空間 (上の shadowSampler_ と同じ理屈)
    D3D11_SAMPLER_DESC ls = {};
    ls.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    ls.AddressU = ls.AddressV = ls.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ls.ComparisonFunc = D3D11_COMPARISON_NEVER;
    ls.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device.Device()->CreateSamplerState(&ls, linearClamp_.GetAddressOf()))) {
        return false;
    }
    timer_.Init(device); // 失敗しても 0ms を返すだけ (計測が消えるだけで描画は続く)
    temporalTimer_.Init(device);
    integrateTimer_.Init(device);
    inited_ = true;
    return true;
}

void FroxelPass::Shutdown()
{
    scatter_.Release();
    integrated_.Release();
    for (History& h : hist_) {
        h.vol[0].Release();
        h.vol[1].Release();
        h.write = 0;
        h.hasLast = false;
    }
    shadowSampler_.Reset();
    linearClamp_.Reset();
    postCb_.Reset();
    cb_.Reset();
    inited_ = false;
    orthoWarned_ = false;
    lastHistValid_ = false;
    lastJitter_ = 0.5f;
}

void FroxelPass::ResetHistory()
{
    for (History& h : hist_) {
        h.hasLast = false;
    }
    lastHistValid_ = false;
}

bool FroxelPass::Inject(GraphicsDevice& device, ShaderManager& shaders, const RenderView& view,
                        const SceneLightData& lights, const FroxelSettings& settings,
                        float sliceJitter)
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
    // M57c: テンポラル / 積分 / CPU 参照実装が**同じ深度分割**を使うための控え。
    // ここを取り違えるとスライスの厚みがずれ、透過率だけが静かに間違う
    gridNearZ_ = c.nearZ;
    gridFarZ_ = c.farZ;
    c.sliceJitter = sliceJitter;
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

void FroxelPass::FillPostCB(const RenderView& view, float sliceJitter, float feedback,
                            bool histValid, const XMFLOAT4X4& prevViewProj, void* out) const
{
    FroxelPostCB& c = *static_cast<FroxelPostCB*>(out);
    XMStoreFloat4x4(&c.invView,
                    XMMatrixTranspose(XMMatrixInverse(nullptr, XMLoadFloat4x4(&view.view))));
    XMStoreFloat4x4(&c.prevViewProj, XMMatrixTranspose(XMLoadFloat4x4(&prevViewProj)));
    c.gridSize[0] = static_cast<uint32_t>(scatter_.Width());
    c.gridSize[1] = static_cast<uint32_t>(scatter_.Height());
    c.gridSize[2] = static_cast<uint32_t>(scatter_.Depth());
    c.histValid = histValid ? 1u : 0u;
    c.nearZ = gridNearZ_;
    c.farZ = gridFarZ_;
    // ★ジッタ非適用の射影から引く (M55b の規約)。Inject と同じ値でなければ
    //   再投影が「カメラが半ピクセル動いた」ことになり履歴が毎フレーム外れる
    c.invProj00 = (std::fabs(view.projNoJitter._11) > 1e-6f) ? 1.0f / view.projNoJitter._11 : 0.0f;
    c.invProj11 = (std::fabs(view.projNoJitter._22) > 1e-6f) ? 1.0f / view.projNoJitter._22 : 0.0f;
    c.sliceJitter = sliceJitter;
    c.feedback = feedback;
}

VolumeTexture* FroxelPass::Temporal(GraphicsDevice& device, ShaderManager& shaders,
                                    const RenderView& view, const FroxelSettings& settings,
                                    float sliceJitter, uint32_t frameSerial,
                                    const XMFLOAT4X4& prevViewProj, bool prevValid,
                                    ID3D11ShaderResourceView* currentSRV)
{
    if (!inited_ || currentSRV == nullptr) {
        return nullptr;
    }
    // AssetPreview (viewKey 0) と想定外のキーは履歴を持たない (TaaPass と同じ規則)
    if (view.viewKey == 0 || view.viewKey >= static_cast<uint32_t>(kHistorySlots)) {
        return nullptr;
    }
    ShaderProgram* prog = shaders.Get(temporalCS_);
    if (!prog || !prog->valid || !prog->cs) {
        return nullptr;
    }
    History& h = hist_[view.viewKey];
    for (int i = 0; i < 2; ++i) {
        h.vol[i].Resize(device, scatter_.Width(), scatter_.Height(), scatter_.Depth(),
                        DXGI_FORMAT_R16G16B16A16_FLOAT); // 同寸なら no-op
    }
    if (!h.vol[0].UAV() || !h.vol[1].UAV()) {
        return nullptr;
    }
    // 「前フレームも同じビューが描かれたか」は viewKey 別の描画通番で見る
    // (TaaPass::Run の ④ と同じ判定。RT の on/off で履歴が混ざらない)
    const bool histValid = h.hasLast && (h.lastSerial + 1u == frameSerial) && prevValid;
    VolumeTexture& dst = h.vol[h.write];
    VolumeTexture& src = h.vol[1 - h.write];

    FroxelPostCB c;
    FillPostCB(view, sliceJitter, settings.temporalFeedback, histValid, prevViewProj, &c);
    if (c.feedback > froxel::kMaxTemporalFeedback) {
        c.feedback = froxel::kMaxTemporalFeedback; // 1.0 に張り付くと今フレームが永久に入らない
    }
    if (c.feedback < 0.0f) {
        c.feedback = 0.0f;
    }

    ID3D11DeviceContext* dc = device.Context();
    gpubuf::UploadCB(dc, postCb_.Get(), c);
    temporalTimer_.Begin(device);
    ID3D11Buffer* cbs[1] = { postCb_.Get() };
    ID3D11ShaderResourceView* srvs[2] = { currentSRV, src.SRV() };
    ID3D11SamplerState* samps[1] = { linearClamp_.Get() };
    ID3D11UnorderedAccessView* uavs[1] = { dst.UAV() };
    dc->CSSetShader(prog->cs.Get(), nullptr, 0);
    dc->CSSetConstantBuffers(0, 1, cbs);
    dc->CSSetShaderResources(0, 2, srvs);
    dc->CSSetSamplers(0, 1, samps);
    dc->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    dc->Dispatch(
        static_cast<UINT>(froxel::DispatchGroups(scatter_.Width(), froxel::kGroupSize)),
        static_cast<UINT>(froxel::DispatchGroups(scatter_.Height(), froxel::kGroupSize)),
        static_cast<UINT>(scatter_.Depth()));
    ID3D11ShaderResourceView* nullSrvs[2] = {};
    ID3D11UnorderedAccessView* nullUavs[1] = {};
    dc->CSSetShaderResources(0, 2, nullSrvs);
    dc->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);
    dc->CSSetShader(nullptr, nullptr, 0);
    temporalTimer_.End(device);

    h.write = 1 - h.write;
    h.lastSerial = frameSerial;
    h.hasLast = true;
    lastHistValid_ = histValid;
    return &dst;
}

bool FroxelPass::Integrate(GraphicsDevice& device, ShaderManager& shaders, const RenderView& view,
                           ID3D11ShaderResourceView* srcSRV)
{
    if (!inited_ || srcSRV == nullptr) {
        return false;
    }
    ShaderProgram* prog = shaders.Get(integrateCS_);
    if (!prog || !prog->valid || !prog->cs) {
        return false;
    }
    integrated_.Resize(device, scatter_.Width(), scatter_.Height(), scatter_.Depth(),
                       DXGI_FORMAT_R16G16B16A16_FLOAT);
    if (!integrated_.UAV()) {
        return false;
    }
    FroxelPostCB c;
    // 積分は行列も feedback も読まない。単位行列を渡しておく (未初期化を送らない)
    XMFLOAT4X4 identity;
    XMStoreFloat4x4(&identity, XMMatrixIdentity());
    FillPostCB(view, 0.5f, 0.0f, false, identity, &c);

    ID3D11DeviceContext* dc = device.Context();
    gpubuf::UploadCB(dc, postCb_.Get(), c);
    integrateTimer_.Begin(device);
    ID3D11Buffer* cbs[1] = { postCb_.Get() };
    ID3D11ShaderResourceView* srvs[1] = { srcSRV };
    ID3D11UnorderedAccessView* uavs[1] = { integrated_.UAV() };
    dc->CSSetShader(prog->cs.Get(), nullptr, 0);
    dc->CSSetConstantBuffers(0, 1, cbs);
    dc->CSSetShaderResources(0, 1, srvs);
    dc->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    // ★Z は割らない (1 スレッドが 1 本の Z 列を手前から舐める)。ここを注入と同じ
    //   「Depth ぶんのグループ」でディスパッチすると、同じ列を 64 回積分し直して
    //   結果は同じなのに 64 倍遅くなる
    dc->Dispatch(
        static_cast<UINT>(froxel::DispatchGroups(scatter_.Width(), froxel::kGroupSize)),
        static_cast<UINT>(froxel::DispatchGroups(scatter_.Height(), froxel::kGroupSize)), 1u);
    ID3D11ShaderResourceView* nullSrvs[1] = {};
    ID3D11UnorderedAccessView* nullUavs[1] = {};
    dc->CSSetShaderResources(0, 1, nullSrvs);
    dc->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);
    dc->CSSetShader(nullptr, nullptr, 0);
    integrateTimer_.End(device);
    return true;
}

ID3D11ShaderResourceView* FroxelPass::Render(GraphicsDevice& device, ShaderManager& shaders,
                                             const RenderView& view, const SceneLightData& lights,
                                             const FroxelSettings& settings)
{
    // ★ジッタと履歴は必ずセットで決める。履歴を持てないビュー (AssetPreview) で
    //   ジッタだけ載せると、代表点がフレーム毎に動くのに混ぜる相手が居ない =
    //   霧が奥行き方向に脈打つだけになる (M55d のカメラジッタと TAA の関係と同じ)
    const bool useTemporal = settings.temporal && view.viewKey > 0
        && view.viewKey < static_cast<uint32_t>(kHistorySlots);
    const float jitter = useTemporal ? froxel::SliceJitter(view.viewFrameIndex) : 0.5f;
    lastJitter_ = jitter;
    lastHistValid_ = false;
    if (!Inject(device, shaders, view, lights, settings, jitter)) {
        return nullptr;
    }
    ID3D11ShaderResourceView* src = scatter_.SRV();
    if (useTemporal) {
        VolumeTexture* blended = Temporal(device, shaders, view, settings, jitter,
                                          view.viewFrameIndex, view.prevViewProj,
                                          view.prevViewProjValid != 0, src);
        if (blended != nullptr) {
            src = blended->SRV();
        }
    }
    if (!Integrate(device, shaders, view, src)) {
        return nullptr;
    }
    return integrated_.SRV();
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
        Inject(device, shaders, view, lights, settings, 0.5f);
    }
    FroxelVolumeStats withShadow;
    std::vector<float> a;
    // ★A/B はジッタ 0.5 (= M57b と同じ代表点) で撮る。ジッタ付きで撮ると、
    //   影の効きを測っているのかジッタの揺れを測っているのか分からなくなる
    if (!Inject(device, shaders, view, lights, settings, 0.5f)
        || !ReadbackStats(device, withShadow, &a)) {
        MYE_LOG_WARN("Froxel dump: 読み戻しに失敗した (注入が走っていない可能性)");
        return;
    }
    FroxelSettings noShadow = settings;
    noShadow.useLocalShadows = false;
    FroxelVolumeStats without;
    std::vector<float> b;
    const bool abOk = Inject(device, shaders, view, lights, noShadow, 0.5f)
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
    // ---- M57c ①: 前方積分の値検査 ----
    // ★「透過率が 1 未満になった」で済ませない (M57b の教訓)。読み戻した注入結果から
    //   CPU で**同じ前方積分**を組み、GPU の出力と 1 セルずつ突き合わせる。
    //   スライスの厚みを 1 枚ぶんずらす / 解析積分を厚みの素掛けにする / 透過率を
    //   掛ける順序を間違える — どれも「それらしい絵」になるので値でしか落とせない
    if (Inject(device, shaders, view, lights, settings, 0.5f)
        && Integrate(device, shaders, view, scatter_.SRV())) {
        std::vector<float> src;
        std::vector<float> gpu;
        if (scatter_.ReadbackAll(device, src) && integrated_.ReadbackAll(device, gpu)
            && src.size() == gpu.size() && !src.empty()) {
            const int w = scatter_.Width();
            const int h = scatter_.Height();
            const int d = scatter_.Depth();
            double maxTransErr = 0.0;
            double maxScatterErr = 0.0;
            int badCells = 0;
            double sumFinalT = 0.0;
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    float acc[3] = { 0.0f, 0.0f, 0.0f };
                    float trans = 1.0f;
                    float prevDepth = gridNearZ_;
                    for (int z = 0; z < d; ++z) {
                        const size_t i = ((static_cast<size_t>(z) * h + y) * w + x) * 4;
                        const float sigmaT = (src[i + 3] > 0.0f) ? src[i + 3] : 0.0f;
                        const float depth = froxel::SliceToViewDepth(static_cast<float>(z + 1), d,
                                                                     gridNearZ_, gridFarZ_);
                        const float thickness = depth - prevDepth;
                        prevDepth = depth;
                        const float k = froxel::IntegratedSliceScatter(sigmaT, thickness);
                        for (int ch = 0; ch < 3; ++ch) {
                            acc[ch] += trans * src[i + ch] * k;
                        }
                        trans *= froxel::SliceTransmittance(sigmaT, thickness);
                        // half 格納 (相対 5e-4) が 64 スライスぶん積み上がるので
                        // 1% + 絶対 1e-3 を許す。式の取り違えはこの桁では隠れない
                        const double te = std::fabs(static_cast<double>(gpu[i + 3]) - trans);
                        maxTransErr = (std::max)(maxTransErr, te);
                        bool bad = te > 1e-3;
                        for (int ch = 0; ch < 3; ++ch) {
                            const double e = std::fabs(static_cast<double>(gpu[i + ch]) - acc[ch]);
                            const double rel = e / (std::max)(std::fabs(acc[ch]), 1e-4f);
                            maxScatterErr = (std::max)(maxScatterErr, rel);
                            bad = bad || (e > 1e-3 && rel > 0.01);
                        }
                        if (bad) {
                            ++badCells;
                        }
                    }
                    sumFinalT += trans;
                }
            }
            // 一様密度なら全列の最終透過率は e^{-σ_t(far-near)} に一致する
            // (厚みの総和が far-near ちょうどであることの検算にもなる)
            const double uniformT = std::exp(-static_cast<double>(settings.density)
                                             * (gridFarZ_ - gridNearZ_));
            MYE_LOG_INFO("Froxel dump:   integrate vs CPU: %d/%d cells out of tolerance / "
                         "max |dT| %.6f / max rel scatter err %.4f%% / mean final T %.5f "
                         "(uniform-density reference %.5f) / %.3f ms (GpuTimer)",
                         badCells, withShadow.cells, maxTransErr, 100.0 * maxScatterErr,
                         sumFinalT / (static_cast<double>(w) * h), uniformT,
                         static_cast<double>(integrateTimer_.Milliseconds()));
        }
    }

    // ---- M57c ②: テンポラルの恒等性と再投影 ----
    // ジッタを止めた (常に 0.5) 状態でカメラが 1 ミリも動いていない前フレームを渡すと、
    // **再投影は各セルを自分自身へ写す**はずなので、履歴を 0.9 混ぜても結果は
    // 注入結果と一致しなければならない。ここが割れる = 再投影の式か格納規約がずれている。
    // 「霧が少しぼやける」形でしか絵に出ないので、値で落とすしかない
    if (view.viewKey > 0 && view.viewKey < static_cast<uint32_t>(kHistorySlots)) {
        XMFLOAT4X4 still;
        XMStoreFloat4x4(&still,
                        XMLoadFloat4x4(&view.view) * XMLoadFloat4x4(&view.projNoJitter));
        ResetHistory();
        std::vector<float> injected;
        std::vector<float> blended;
        double maxDelta = 0.0;
        int moved = 0;
        bool ok = true;
        for (uint32_t f = 0; f < 6u && ok; ++f) {
            ok = Inject(device, shaders, view, lights, settings, 0.5f);
            VolumeTexture* out =
                ok ? Temporal(device, shaders, view, settings, 0.5f, f, still, true, scatter_.SRV())
                   : nullptr;
            ok = ok && out != nullptr;
            if (ok && f + 1u == 6u) {
                ok = scatter_.ReadbackAll(device, injected) && out->ReadbackAll(device, blended)
                    && injected.size() == blended.size();
            }
        }
        if (ok) {
            for (size_t i = 0; i < injected.size(); ++i) {
                const double e = std::fabs(static_cast<double>(blended[i]) - injected[i]);
                maxDelta = (std::max)(maxDelta, e);
                if (e > 1e-3) {
                    ++moved;
                }
            }
            MYE_LOG_INFO("Froxel dump:   temporal reprojection (static camera, no jitter): "
                         "%d/%zu components differ / max |delta| %.6f / histValid %d / %.3f ms",
                         moved, injected.size(), maxDelta, lastHistValid_ ? 1 : 0,
                         static_cast<double>(temporalTimer_.Milliseconds()));
        } else {
            MYE_LOG_WARN("Froxel dump: テンポラルの恒等性検査を走らせられなかった");
        }

        // ---- M57c ③: ジッタ列を回すと本当に別の値へ収束するか ----
        // ②が「動かない」ことの検査なので、対に「動く」ことも見ておく。
        // ここが 0 だと、ジッタが CB に載っていない / 履歴が毎フレーム捨てられている
        ResetHistory();
        std::vector<float> single;
        std::vector<float> accum;
        bool ok3 = true;
        for (uint32_t f = 0; f < froxel::kJitterSequenceLength && ok3; ++f) {
            const float j = froxel::SliceJitter(f);
            ok3 = Inject(device, shaders, view, lights, settings, j);
            VolumeTexture* out =
                ok3 ? Temporal(device, shaders, view, settings, j, f, still, true, scatter_.SRV())
                    : nullptr;
            ok3 = ok3 && out != nullptr;
            if (ok3 && f + 1u == froxel::kJitterSequenceLength) {
                ok3 = scatter_.ReadbackAll(device, single) && out->ReadbackAll(device, accum)
                    && single.size() == accum.size();
            }
        }
        if (ok3) {
            double sum = 0.0;
            double maxJitterDelta = 0.0;
            for (size_t i = 0; i < single.size(); ++i) {
                const double e = std::fabs(static_cast<double>(accum[i]) - single[i]);
                sum += e;
                maxJitterDelta = (std::max)(maxJitterDelta, e);
            }
            MYE_LOG_INFO("Froxel dump:   temporal jitter accumulation over %u frames: "
                         "mean |delta| %.8f / max |delta| %.6f (jitter %.4f..%.4f)",
                         froxel::kJitterSequenceLength, sum / single.size(), maxJitterDelta,
                         static_cast<double>(froxel::SliceJitter(0)),
                         static_cast<double>(
                             froxel::SliceJitter(froxel::kJitterSequenceLength - 1u)));
        }
        ResetHistory(); // 検査で作った履歴を本番フレームへ持ち越さない
    }

    // 影ありの状態へ戻して抜ける (この後のフレームが A/B の片割れを掴まないように)
    Inject(device, shaders, view, lights, settings, 0.5f);
}

} // namespace mye
