//====================================================================================
//                          OcclusionCullPass.cpp
//  MyEngin/ 秋田蓮音                                                       10/09/2026
//                                          GPU オクルージョンカリング (2 フェーズ、max-Z HZB) 実装
//====================================================================================
#include "Engine/Renderer/Passes/OcclusionCullPass.h"

#include <algorithm>
#include <cstring>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Renderer/Device/GpuBufferUtil.h"
#include "Engine/Renderer/Device/GraphicsDevice.h"
#include "Engine/Renderer/Passes/OcclusionMath.h"
#include "Engine/Renderer/Shader/ShaderManager.h"

namespace mye {
namespace {

using namespace gpubuf;

// occlusion_cull.cs.hlsl の MYE_OCC_FLAG_* と一致
constexpr uint32_t kFlagAlways = 1u;
constexpr uint32_t kFlagNew = 2u;
constexpr uint32_t kFlagNoSlot = 4u;
// occlusion_cull.cs.hlsl の MYE_OCC_MODE_* と一致
constexpr int kModeSelect = 0;
constexpr int kModeTest = 1;
constexpr int kModePass = 2;
// Dispatch の 1 次元あたりのグループ数上限。シェーダの 65535u と対
constexpr uint32_t kMaxGroupsPerDim = 65535u;
constexpr uint32_t kStatsBytes = 16; // phase1 / phase2 / occluded / 予約
constexpr int kStagingRing = 3;
constexpr uint32_t kStatsLagFrames = 2;
// 可視ビットの添字 (entity.index) の上限。これを超える / 無効なエンティティは履歴を持たず常に描く
constexpr uint32_t kMaxHistorySlots = 1u << 22;

bool HasSlot(EntityID e)
{
    return !e.IsNull() && e.index < kMaxHistorySlots;
}

// occlusion_cull.cs.hlsl の OccItem (StructuredBuffer、32 バイト)
struct GpuItem {
    float bmin[3];
    uint32_t slot;
    float bmax[3];
    uint32_t flags;
};
static_assert(sizeof(GpuItem) == 32, "OccItem と一致させる");

// occlusion_cull.cs.hlsl の OccCmd (32 バイト)
struct GpuCmd {
    uint32_t firstItem;
    uint32_t itemCount;
    uint32_t instanceBase;
    uint32_t isInstanced;
    uint32_t indexCount;
    uint32_t startIndex;
    uint32_t pad0;
    uint32_t pad1;
};
static_assert(sizeof(GpuCmd) == 32, "OccCmd と一致させる");

// occlusion_cull.cs.hlsl の OcclusionCB (b0)
struct OcclusionCB {
    DirectX::XMFLOAT4X4 viewProj;
    uint32_t screen[2];
    uint32_t mipCount;
    uint32_t mode;
    uint32_t cmdCount;
    uint32_t argsByteBase;
    uint32_t remapBase;
    uint32_t pad;
};
static_assert(sizeof(OcclusionCB) % 16 == 0, "定数バッファは 16 バイト境界");

// grow-only の DYNAMIC 構造化バッファ。needed が容量以下なら何もしない
bool EnsureDynamicStructured(ID3D11Device* dev, uint32_t stride, uint32_t needed, uint32_t& capacity,
                             Microsoft::WRL::ComPtr<ID3D11Buffer>& buf,
                             Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& srv)
{
    if (buf && needed <= capacity) {
        return true;
    }
    const uint32_t cap = std::max(needed, capacity * 2u + 64u);
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = cap * stride;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = stride;
    Microsoft::WRL::ComPtr<ID3D11Buffer> nb;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> ns;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, nb.GetAddressOf()))) {
        return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = DXGI_FORMAT_UNKNOWN;
    sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    sd.Buffer.NumElements = cap;
    if (FAILED(dev->CreateShaderResourceView(nb.Get(), &sd, ns.GetAddressOf()))) {
        return false;
    }
    buf = std::move(nb);
    srv = std::move(ns);
    capacity = cap;
    return true;
}

bool UploadStructured(ID3D11DeviceContext* dc, ID3D11Buffer* buf, const void* data, size_t bytes)
{
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(dc->Map(buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return false;
    }
    std::memcpy(mapped.pData, data, bytes);
    dc->Unmap(buf, 0);
    return true;
}

// RAW UAV を持つ DEFAULT バッファ。extraMisc に DRAWINDIRECT_ARGS を足せる
bool CreateRawUav(ID3D11Device* dev, uint32_t bytes, UINT extraMisc,
                  Microsoft::WRL::ComPtr<ID3D11Buffer>& buf,
                  Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>& uav)
{
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = bytes;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    // ★ALLOW_RAW_VIEWS を落とすと UAV の生成だけが E_INVALIDARG で落ちる (GpuParticleBackend と同じ)
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS | extraMisc;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, buf.GetAddressOf()))) {
        return false;
    }
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
    ud.Format = DXGI_FORMAT_R32_TYPELESS;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = bytes / 4;
    ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    return SUCCEEDED(dev->CreateUnorderedAccessView(buf.Get(), &ud, uav.GetAddressOf()));
}

} // namespace

bool OcclusionCuller::Init(GraphicsDevice& device, ShaderManager& shaders)
{
    cullCS_ = shaders.LoadCompute("occlusion_cull.cs");
    if (!CreateConstant(device.Device(), sizeof(OcclusionCB), cb_)) {
        return false;
    }
    selectTimer_.Init(device);
    testTimer_.Init(device);
    disabled_ = false;
    initialized_ = true;
    return true;
}

void OcclusionCuller::Shutdown()
{
    for (ViewState& v : views_) {
        v.pyramid.Shutdown();
        v = ViewState();
    }
    items_.Reset();
    itemsSrv_.Reset();
    cmds_.Reset();
    cmdsSrv_.Reset();
    args_.Reset();
    argsUav_.Reset();
    remap_.Reset();
    remapUav_.Reset();
    remapSrv_.Reset();
    stats_.Reset();
    statsUav_.Reset();
    cb_.Reset();
    selectTimer_.Release();
    testTimer_.Release();
    itemCapacity_ = 0;
    cmdCapacity_ = 0;
    argsCapacity_ = 0;
    remapCapacity_ = 0;
    cmdCount_ = 0;
    worldCount_ = 0;
    // 失敗の記録は復旧 (Init) まで残す。Shutdown→Init で disabled_ が戻る
    initialized_ = false;
}

bool OcclusionCuller::Fail(const char* why)
{
    if (!disabled_) {
        disabled_ = true;
        // 描画は従来の経路で続く。ログは 1 回だけ
        MYE_LOG_ERROR("[occlusion] GPU occlusion culling disabled: %s", why);
    }
    return false;
}

bool OcclusionCuller::EnsureView(GraphicsDevice& device, ShaderManager& shaders, ViewState& vs,
                                 uint32_t slotCapacity)
{
    if (!vs.pyramidInit) {
        if (!vs.pyramid.Init(device, shaders, HzbReduceOp::Max)) {
            return false;
        }
        vs.pyramidInit = true;
    }
    if (!vs.visBuf || slotCapacity > vs.visCapacity) {
        const uint32_t cap = std::max(slotCapacity, vs.visCapacity * 2u + 256u);
        const std::vector<uint32_t> zeros(cap, 0u);
        Microsoft::WRL::ComPtr<ID3D11Buffer> nb;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> nu;
        if (!CreateStructured(device.Device(), 4, cap, zeros.data(), 0, nb, &nu, nullptr)) {
            return false;
        }
        vs.visBuf = std::move(nb);
        vs.visUav = std::move(nu);
        vs.visCapacity = cap;
        vs.valid = false; // 作り直すと可視ビットが 0 に戻る = 履歴を使わない
    }
    if (!vs.staging[0]) {
        for (auto& s : vs.staging) {
            D3D11_BUFFER_DESC bd = {};
            bd.ByteWidth = kStatsBytes;
            bd.Usage = D3D11_USAGE_STAGING;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(device.Device()->CreateBuffer(&bd, nullptr, s.GetAddressOf()))) {
                return false;
            }
        }
    }
    return true;
}

bool OcclusionCuller::EnsureScratch(GraphicsDevice& device, uint32_t itemCount, uint32_t cmdCount,
                                    uint32_t worldCount)
{
    ID3D11Device* dev = device.Device();
    if (!EnsureDynamicStructured(dev, sizeof(GpuItem), itemCount, itemCapacity_, items_, itemsSrv_)
        || !EnsureDynamicStructured(dev, sizeof(GpuCmd), cmdCount, cmdCapacity_, cmds_, cmdsSrv_)) {
        return false;
    }
    if (!args_ || cmdCount > argsCapacity_) {
        const uint32_t cap = std::max(cmdCount, argsCapacity_ * 2u + 64u);
        Microsoft::WRL::ComPtr<ID3D11Buffer> nb;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> nu;
        if (!CreateRawUav(dev, 2u * cap * kArgsStride, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS, nb, nu)) {
            return false;
        }
        args_ = std::move(nb);
        argsUav_ = std::move(nu);
        argsCapacity_ = cap;
    }
    if (!remap_ || worldCount > remapCapacity_) {
        const uint32_t cap = std::max(std::max(worldCount, 1u), remapCapacity_ * 2u + 256u);
        Microsoft::WRL::ComPtr<ID3D11Buffer> nb;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> nu;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> ns;
        if (!CreateStructured(dev, 4, 2u * cap, nullptr, 0, nb, &nu, &ns)) {
            return false;
        }
        remap_ = std::move(nb);
        remapUav_ = std::move(nu);
        remapSrv_ = std::move(ns);
        remapCapacity_ = cap;
    }
    if (!stats_) {
        if (!CreateRawUav(dev, kStatsBytes, 0, stats_, statsUav_)) {
            return false;
        }
    }
    return true;
}

bool OcclusionCuller::Begin(GraphicsDevice& device, ShaderManager& shaders, const FrameDesc& desc,
                            const std::vector<OcclusionItemIn>& items,
                            const std::vector<OcclusionCmdIn>& cmds)
{
    if (disabled_ || !initialized_ || desc.viewKey == 0
        || desc.viewKey >= static_cast<uint32_t>(kViewSlots) || items.empty()
        || cmds.empty() || desc.width <= 0 || desc.height <= 0) {
        return false;
    }
    if (injectFailure_) {
        return Fail("injected resource creation failure");
    }
    ShaderProgram* cs = shaders.Get(cullCS_);
    if (cs == nullptr || !cs->valid || !cs->cs) {
        return Fail("occlusion_cull.cs failed to compile");
    }

    ViewState& vs = views_[desc.viewKey];
    uint32_t maxIndex = 0;
    for (const OcclusionItemIn& it : items) {
        if (HasSlot(it.entity)) {
            maxIndex = std::max(maxIndex, it.entity.index);
        }
    }
    if (!EnsureView(device, shaders, vs, maxIndex + 1u)
        || !EnsureScratch(device, static_cast<uint32_t>(items.size()), static_cast<uint32_t>(cmds.size()),
                          desc.worldCount)) {
        return Fail("resource creation failed");
    }

    // 履歴が使えるか: 前回の通番の次で、同じサイズ (PrevRenderWorldStore::Begin と同じ判定)
    const bool usable = vs.valid && vs.width == desc.width && vs.height == desc.height
        && vs.lastSerial + 1u == desc.serial;
    const uint32_t prevSerial = desc.serial - 1u;
    vs.lastSerial = desc.serial;
    vs.width = desc.width;
    vs.height = desc.height;
    vs.valid = true;
    if (vs.generation.size() <= maxIndex) {
        vs.generation.resize(maxIndex + 1u, 0u);
        vs.slotSerial.resize(maxIndex + 1u, 0u);
    }

    std::vector<GpuItem> gpuItems(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        const OcclusionItemIn& in = items[i];
        GpuItem& g = gpuItems[i];
        std::memcpy(g.bmin, in.bmin, sizeof(g.bmin));
        std::memcpy(g.bmax, in.bmax, sizeof(g.bmax));
        if (!HasSlot(in.entity)) {
            // 可視ビットを持てない項目 (エンティティの無い手組みの項目) は判定せず常に描く
            g.slot = 0;
            g.flags = kFlagAlways | kFlagNew | kFlagNoSlot;
            continue;
        }
        const bool seenLastFrame = usable && vs.generation[in.entity.index] == in.entity.generation + 1u
            && vs.slotSerial[in.entity.index] == prevSerial;
        g.slot = in.entity.index;
        g.flags = (in.alwaysDraw ? kFlagAlways : 0u) | (seenLastFrame ? 0u : kFlagNew);
        vs.generation[in.entity.index] = in.entity.generation + 1u;
        vs.slotSerial[in.entity.index] = desc.serial;
    }
    std::vector<GpuCmd> gpuCmds(cmds.size());
    for (size_t i = 0; i < cmds.size(); ++i) {
        const OcclusionCmdIn& in = cmds[i];
        GpuCmd& g = gpuCmds[i];
        g.firstItem = in.firstItem;
        g.itemCount = in.itemCount;
        g.instanceBase = in.instanceBase;
        g.isInstanced = in.isInstanced ? 1u : 0u;
        g.indexCount = in.indexCount;
        g.startIndex = in.startIndex;
        g.pad0 = 0;
        g.pad1 = 0;
    }
    ID3D11DeviceContext* dc = device.Context();
    if (!UploadStructured(dc, items_.Get(), gpuItems.data(), gpuItems.size() * sizeof(GpuItem))
        || !UploadStructured(dc, cmds_.Get(), gpuCmds.data(), gpuCmds.size() * sizeof(GpuCmd))) {
        return Fail("upload failed");
    }

    const uint32_t zeros[4] = {};
    dc->ClearUnorderedAccessViewUint(statsUav_.Get(), zeros);

    activeView_ = desc.viewKey;
    frame_ = desc;
    cmdCount_ = static_cast<uint32_t>(cmds.size());
    worldCount_ = desc.worldCount;
    return true;
}

void OcclusionCuller::Dispatch(GraphicsDevice& device, ID3D11ComputeShader* cs, int mode, int phase,
                               ID3D11ShaderResourceView* hzb, ViewState& vs)
{
    ID3D11DeviceContext* dc = device.Context();
    OcclusionCB c = {};
    c.viewProj = frame_.viewProjT;
    c.screen[0] = static_cast<uint32_t>(frame_.width);
    c.screen[1] = static_cast<uint32_t>(frame_.height);
    c.mipCount = static_cast<uint32_t>(std::max(vs.pyramid.MipCount(), 1));
    c.mode = static_cast<uint32_t>(mode);
    c.cmdCount = cmdCount_;
    c.argsByteBase = static_cast<uint32_t>(phase) * cmdCount_ * kArgsStride;
    c.remapBase = RemapRegion(phase);
    UploadCB(dc, cb_.Get(), c);

    dc->CSSetShader(cs, nullptr, 0);
    ID3D11Buffer* cbs[1] = { cb_.Get() };
    dc->CSSetConstantBuffers(0, 1, cbs);
    ID3D11ShaderResourceView* srvs[3] = { itemsSrv_.Get(), cmdsSrv_.Get(), hzb };
    dc->CSSetShaderResources(0, 3, srvs);
    ID3D11UnorderedAccessView* uavs[4] = { argsUav_.Get(), remapUav_.Get(), vs.visUav.Get(),
                                           statsUav_.Get() };
    dc->CSSetUnorderedAccessViews(0, 4, uavs, nullptr);

    const UINT gx = std::min(cmdCount_, kMaxGroupsPerDim);
    const UINT gy = (cmdCount_ + kMaxGroupsPerDim - 1u) / kMaxGroupsPerDim;
    dc->Dispatch(gx, gy, 1);

    // remap / args は後で VS の SRV / 間接引数として読むので UAV を必ず外す
    ID3D11ShaderResourceView* nullSrvs[3] = {};
    dc->CSSetShaderResources(0, 3, nullSrvs);
    ID3D11UnorderedAccessView* nullUavs[4] = {};
    dc->CSSetUnorderedAccessViews(0, 4, nullUavs, nullptr);
    dc->CSSetShader(nullptr, nullptr, 0);
}

void OcclusionCuller::SelectPhase1(GraphicsDevice& device, ShaderManager& shaders)
{
    ShaderProgram* cs = shaders.Get(cullCS_);
    if (cs == nullptr || !cs->cs) {
        return; // Begin が通った直後なので起きない (引数が未書込みのまま描くことになるため防御)
    }
    selectTimer_.Begin(device);
    Dispatch(device, cs->cs.Get(), kModeSelect, 0, nullptr, views_[activeView_]);
    selectTimer_.End(device);
}

void OcclusionCuller::TestPhase2(GraphicsDevice& device, ShaderManager& shaders,
                                 ID3D11ShaderResourceView* depthSRV)
{
    ViewState& vs = views_[activeView_];
    ID3D11DeviceContext* dc = device.Context();
    ShaderProgram* cs = shaders.Get(cullCS_);
    if (cs == nullptr || !cs->cs) {
        return;
    }
    const bool built = depthSRV != nullptr
        && vs.pyramid.Build(device, shaders, depthSRV, frame_.width, frame_.height);
    if (!built) {
        // 判定せず全項目を可視とみなして今フレームは描き切る。次フレームからは従来の経路に落とす
        Fail("max-Z pyramid could not be built");
    }
    testTimer_.Begin(device);
    Dispatch(device, cs->cs.Get(), built ? kModeTest : kModePass, 1, built ? vs.pyramid.SRV() : nullptr,
             vs);
    testTimer_.End(device);

    // 統計をステージングへ写す。読むのは 2 フレーム遅れ (GPU を待たない)
    ID3D11Buffer* dst = vs.staging[vs.writeCount % kStagingRing].Get();
    dc->CopyResource(dst, stats_.Get());
    ++vs.writeCount;
    ReadStats(device, vs);
}

void OcclusionCuller::ReadStats(GraphicsDevice& device, ViewState& vs)
{
    if (vs.writeCount <= kStatsLagFrames) {
        return;
    }
    ID3D11Buffer* src = vs.staging[(vs.writeCount - 1u - kStatsLagFrames) % kStagingRing].Get();
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(device.Context()->Map(src, 0, D3D11_MAP_READ, 0, &mapped))) {
        return;
    }
    uint32_t v[4] = {};
    std::memcpy(v, mapped.pData, sizeof(v));
    device.Context()->Unmap(src, 0);
    vs.stats.valid = true;
    vs.stats.phase1Draws = static_cast<int>(v[0]);
    vs.stats.phase2Draws = static_cast<int>(v[1]);
    vs.stats.occluded = static_cast<int>(v[2]);
}

OcclusionStats OcclusionCuller::Stats(uint32_t viewKey) const
{
    return (viewKey < static_cast<uint32_t>(kViewSlots)) ? views_[viewKey].stats : OcclusionStats{};
}

float OcclusionCuller::GpuMs() const
{
    return selectTimer_.Milliseconds() + views_[activeView_].pyramid.GpuMs() + testTimer_.Milliseconds();
}

} // namespace mye
