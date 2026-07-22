#include "Engine/Engine/Particles/GpuParticleBackend.h"

#include <algorithm>
#include <cmath>

#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/Particles/ParticleCurves.h"
#include "Engine/Renderer/GraphicsDevice.h"
#include "Engine/Renderer/ShaderManager.h"

using namespace DirectX;

namespace mye {
namespace {

constexpr float kPi = 3.14159265358979323846f;

struct GpuParticleCB { // particle_gpu_common.hlsli と一致
    XMFLOAT4 gravityWind; // xyz + dt
    XMFLOAT4 params;      // emitCount, turbulence, sizeEndScale, capacity
    XMFLOAT4 colorBegin;
    XMFLOAT4 colorEnd;
};

struct GpuRenderCB {
    XMFLOAT4X4 viewProj;
    XMFLOAT3 camRight;
    float pad0;
    XMFLOAT3 camUp;
    float pad1;
    float offsetX;
    float pad2[3];
};

struct GpuParticleData { // 48 bytes (シェーダの GpuParticle と一致)
    XMFLOAT3 pos;
    float life;
    XMFLOAT3 vel;
    float invLife;
    float size0;
    XMFLOAT3 pad;
};

bool CreateStructured(ID3D11Device* dev, UINT elemSize, UINT count, const void* initData,
                      UINT uavFlags, Microsoft::WRL::ComPtr<ID3D11Buffer>& buf,
                      Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>* uav,
                      Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>* srv)
{
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = elemSize * count;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = elemSize;
    D3D11_SUBRESOURCE_DATA init = { initData, 0, 0 };
    if (FAILED(dev->CreateBuffer(&bd, initData ? &init : nullptr, buf.GetAddressOf()))) {
        return false;
    }
    if (uav) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = DXGI_FORMAT_UNKNOWN;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = count;
        ud.Buffer.Flags = uavFlags;
        if (FAILED(dev->CreateUnorderedAccessView(buf.Get(), &ud, uav->GetAddressOf()))) {
            return false;
        }
    }
    if (srv) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = DXGI_FORMAT_UNKNOWN;
        sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        sd.Buffer.NumElements = count;
        if (FAILED(dev->CreateShaderResourceView(buf.Get(), &sd, srv->GetAddressOf()))) {
            return false;
        }
    }
    return true;
}

bool CreateConstant(ID3D11Device* dev, UINT size, Microsoft::WRL::ComPtr<ID3D11Buffer>& out)
{
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = size;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(dev->CreateBuffer(&bd, nullptr, out.GetAddressOf()));
}

template <typename T>
void UploadCB(ID3D11DeviceContext* dc, ID3D11Buffer* cb, const T& data)
{
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(dc->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, &data, sizeof(T));
        dc->Unmap(cb, 0);
    }
}

} // namespace

bool GpuParticleBackend::Init(GraphicsDevice& device, ShaderManager& shaders)
{
    device_ = &device;
    shaders_ = &shaders;
    emitCS_ = shaders.LoadCompute("particle_emit.cs");
    simCS_ = shaders.LoadCompute("particle_sim.cs");
    renderShader_ = shaders.Load("particle_render_gpu");

    ID3D11Device* dev = device.Device();
    if (!CreateConstant(dev, sizeof(GpuParticleCB), simCB_)
        || !CreateConstant(dev, sizeof(GpuRenderCB), renderCB_)) {
        return false;
    }

    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(dev->CreateBlendState(&bd, blendAdditive_.GetAddressOf()))) {
        return false;
    }
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    if (FAILED(dev->CreateBlendState(&bd, blendAlpha_.GetAddressOf()))) {
        return false;
    }

    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    if (FAILED(dev->CreateDepthStencilState(&dd, depthNoWrite_.GetAddressOf()))) {
        return false;
    }
    return timer_.Init(device);
}

void GpuParticleBackend::Shutdown()
{
    emitters_.clear();
    simCB_.Reset();
    renderCB_.Reset();
    blendAdditive_.Reset();
    blendAlpha_.Reset();
    depthNoWrite_.Reset();
}

void GpuParticleBackend::Reset()
{
    emitters_.clear();
    stats_ = {};
}

bool GpuParticleBackend::CreateEmitterResources(GraphicsDevice& device, GpuEmitter& em,
                                                uint32_t capacity)
{
    ID3D11Device* dev = device.Device();
    em.capacity = capacity;

    if (!CreateStructured(dev, sizeof(GpuParticleData), capacity, nullptr, 0, em.pool,
                          &em.poolUAV, &em.poolSRV)) {
        return false;
    }

    // dead list を 0..capacity-1 で初期化
    std::vector<uint32_t> indices(capacity);
    for (uint32_t i = 0; i < capacity; ++i) {
        indices[i] = i;
    }
    if (!CreateStructured(dev, 4, capacity, indices.data(), D3D11_BUFFER_UAV_FLAG_APPEND,
                          em.deadList, &em.deadUAV, nullptr)) {
        return false;
    }
    for (int i = 0; i < 2; ++i) {
        if (!CreateStructured(dev, 4, capacity, nullptr, D3D11_BUFFER_UAV_FLAG_COUNTER,
                              em.alive[i], &em.aliveUAV[i], &em.aliveSRV[i])) {
            return false;
        }
    }
    {
        // CopyStructureCount の宛先は structured buffer 禁止 (D3D 制約) —
        // 通常のバッファ + R32_UINT typed SRV (HLSL 側は Buffer<uint>)
        D3D11_BUFFER_DESC cd = {};
        cd.ByteWidth = 4 * sizeof(uint32_t);
        cd.Usage = D3D11_USAGE_DEFAULT;
        cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev->CreateBuffer(&cd, nullptr, em.counts.GetAddressOf()))) {
            return false;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = DXGI_FORMAT_R32_UINT;
        sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        sd.Buffer.NumElements = 4;
        if (FAILED(dev->CreateShaderResourceView(em.counts.Get(), &sd,
                                                 em.countsSRV.GetAddressOf()))) {
            return false;
        }
    }

    D3D11_BUFFER_DESC ad = {};
    ad.ByteWidth = 4 * sizeof(uint32_t);
    ad.Usage = D3D11_USAGE_DEFAULT;
    ad.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
    const uint32_t args[4] = { 4, 0, 0, 0 }; // VertexCount=4, InstanceCount=0
    D3D11_SUBRESOURCE_DATA ainit = { args, 0, 0 };
    if (FAILED(dev->CreateBuffer(&ad, &ainit, em.indirectArgs.GetAddressOf()))) {
        return false;
    }

    // 隠しカウンタの事前初期化: dead = capacity (全スロット空き)、alive A/B = 0。
    // 初期カウントは UAV バインド時にのみ設定できるため、一度バインドして即解除する
    {
        ID3D11DeviceContext* dc = device.Context();
        ID3D11UnorderedAccessView* uavs[3] = { em.deadUAV.Get(), em.aliveUAV[0].Get(),
                                               em.aliveUAV[1].Get() };
        const UINT inits[3] = { capacity, 0, 0 };
        dc->CSSetUnorderedAccessViews(0, 3, uavs, inits);
        ID3D11UnorderedAccessView* nulls[3] = { nullptr, nullptr, nullptr };
        dc->CSSetUnorderedAccessViews(0, 3, nulls, nullptr);
    }
    em.firstDispatch = false;
    return true;
}

void GpuParticleBackend::SyncEmitters(World& world, GraphicsDevice& device)
{
    std::vector<EntityID> current;
    const ComponentTypeId req[] = { ParticleEmitterComponent::sTypeId,
                                    WorldMatrixComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            if (!IsEntityActive(world, arch.EntityAt(row))) {
                continue; // 無効エミッタは描画しない (M10)
            }
            current.push_back(arch.EntityAt(row));
        }
    });
    std::sort(current.begin(), current.end(),
              [](EntityID a, EntityID b) { return a.index < b.index; });

    std::erase_if(emitters_, [&](const GpuEmitter& em) {
        return std::find(current.begin(), current.end(), em.owner) == current.end();
    });
    for (EntityID e : current) {
        bool exists = false;
        for (const GpuEmitter& em : emitters_) {
            if (em.owner == e) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            GpuEmitter em;
            em.owner = e;
            const auto* desc = world.GetComponent<ParticleEmitterComponent>(e);
            const uint32_t cap = static_cast<uint32_t>(
                std::clamp(desc ? desc->maxParticles : 100000, 1024, 1000000));
            em.rng.Seed(desc ? desc->seed : 1u, static_cast<uint64_t>(e.index) * 2u + 1u);
            if (CreateEmitterResources(device, em, cap)) {
                emitters_.push_back(std::move(em));
            } else {
                MYE_LOG_ERROR("[gpu particles] resource creation failed (cap=%u)", cap);
            }
        }
    }
    std::sort(emitters_.begin(), emitters_.end(),
              [](const GpuEmitter& a, const GpuEmitter& b) { return a.owner.index < b.owner.index; });
}

void GpuParticleBackend::Update(World& world, float dt)
{
    if (!device_ || !shaders_) {
        return;
    }
    ShaderProgram* emitProg = shaders_->Get(emitCS_);
    ShaderProgram* simProg = shaders_->Get(simCS_);
    if (!emitProg || !emitProg->valid || !simProg || !simProg->valid) {
        return; // コンパイル失敗中はスキップ (ホットリロードでの復旧待ち)
    }
    ID3D11ComputeShader* emitShader = emitProg->cs.Get();
    ID3D11ComputeShader* simShader = simProg->cs.Get();

    SyncEmitters(world, *device_);
    ID3D11DeviceContext* dc = device_->Context();

    timer_.Begin(*device_);

    uint32_t aliveEstimate = 0;
    for (GpuEmitter& em : emitters_) {
        const auto* desc = world.GetComponent<ParticleEmitterComponent>(em.owner);
        const auto* wm = world.GetComponent<WorldMatrixComponent>(em.owner);
        if (!desc || !wm) {
            continue;
        }
        em.descCache = *desc;
        const XMFLOAT3 origin = { wm->value._41, wm->value._42, wm->value._43 };

        // ---- CPU 側で放出データを生成 (決定論 RNG。GPU では乱数を作らない) ----
        // 放出計画は CPU バックエンドと共有 (M32a: playing/duration/loop/burst)。表示用ベストエフォート。
        int emitCount = PlanParticleEmission(*desc, em.ageTicks, em.emitAccum, dt);
        emitCount = std::min(emitCount, static_cast<int>(em.capacity / 4)); // 1tick 暴発ガード
        emitCount = std::max(emitCount, 0);

        std::vector<EmitData> emitData(static_cast<size_t>(std::max(emitCount, 0)));
        for (int n = 0; n < emitCount; ++n) {
            float dirX = 0.0f, dirY = 1.0f, dirZ = 0.0f;
            float posX = origin.x, posY = origin.y, posZ = origin.z;
            switch (desc->shape) {
            case 1: {
                const float z = 1.0f - 2.0f * em.rng.NextFloat01();
                const float phi = em.rng.NextFloat01() * 2.0f * kPi;
                const float s = sqrtf(std::max(0.0f, 1.0f - z * z));
                dirX = s * cosf(phi);
                dirY = z;
                dirZ = s * sinf(phi);
                posX += dirX * desc->shapeRadius;
                posY += dirY * desc->shapeRadius;
                posZ += dirZ * desc->shapeRadius;
                break;
            }
            case 2: {
                const float cosMax = cosf(desc->coneAngleDeg * kPi / 180.0f);
                const float cosT = 1.0f - em.rng.NextFloat01() * (1.0f - cosMax);
                const float sinT = sqrtf(std::max(0.0f, 1.0f - cosT * cosT));
                const float phi = em.rng.NextFloat01() * 2.0f * kPi;
                dirX = sinT * cosf(phi);
                dirY = cosT;
                dirZ = sinT * sinf(phi);
                break;
            }
            case 3:
                posX += (em.rng.NextFloat01() * 2.0f - 1.0f) * desc->boxExtents.x;
                posY += (em.rng.NextFloat01() * 2.0f - 1.0f) * desc->boxExtents.y;
                posZ += (em.rng.NextFloat01() * 2.0f - 1.0f) * desc->boxExtents.z;
                break;
            default:
                break;
            }
            const float speed = em.rng.Range(desc->speedMin, desc->speedMax);
            const float lifetime = std::max(0.01f, em.rng.Range(desc->lifetimeMin, desc->lifetimeMax));
            const float size = em.rng.Range(desc->sizeMin, desc->sizeMax);
            emitData[static_cast<size_t>(n)] = { { posX, posY, posZ }, lifetime,
                                                 { dirX * speed, dirY * speed, dirZ * speed }, size };
        }

        // 放出バッファ (動的) を確保・充填
        if (emitCount > 0) {
            if (em.emitCapacity < static_cast<uint32_t>(emitCount)) {
                em.emitCapacity = std::max<uint32_t>(static_cast<uint32_t>(emitCount) * 2, 256);
                D3D11_BUFFER_DESC bd = {};
                bd.ByteWidth = em.emitCapacity * sizeof(EmitData);
                bd.Usage = D3D11_USAGE_DYNAMIC;
                bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
                bd.StructureByteStride = sizeof(EmitData);
                if (FAILED(device_->Device()->CreateBuffer(&bd, nullptr,
                                                           em.emitBuffer.ReleaseAndGetAddressOf()))) {
                    continue;
                }
                D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
                sd.Format = DXGI_FORMAT_UNKNOWN;
                sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
                sd.Buffer.NumElements = em.emitCapacity;
                device_->Device()->CreateShaderResourceView(em.emitBuffer.Get(), &sd,
                                                            em.emitSRV.ReleaseAndGetAddressOf());
            }
            D3D11_MAPPED_SUBRESOURCE mapped = {};
            if (SUCCEEDED(dc->Map(em.emitBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                memcpy(mapped.pData, emitData.data(), emitData.size() * sizeof(EmitData));
                dc->Unmap(em.emitBuffer.Get(), 0);
            }
        }

        const int aliveIn = em.aliveCurrent;
        const int aliveOut = 1 - aliveIn;

        GpuParticleCB cb = {};
        cb.gravityWind = { desc->gravity.x + desc->wind.x, desc->gravity.y + desc->wind.y,
                           desc->gravity.z + desc->wind.z, dt };
        cb.params = { static_cast<float>(emitCount), desc->turbulence, desc->sizeEndScale,
                      static_cast<float>(em.capacity) };
        cb.colorBegin = desc->colorBegin;
        cb.colorEnd = desc->colorEnd;
        UploadCB(dc, simCB_.Get(), cb);
        ID3D11Buffer* cbs[1] = { simCB_.Get() };
        dc->CSSetConstantBuffers(0, 1, cbs);

        ID3D11UnorderedAccessView* nullUavs[3] = { nullptr, nullptr, nullptr };
        ID3D11ShaderResourceView* nullSrvs[2] = { nullptr, nullptr };

        // カウントバッファ更新: [0]=deadCount, [1]=aliveInCount。
        // 重要: counts が CS の SRV にバインドされたままコピーしない (ハザードで落ちる)
        dc->CSSetShaderResources(0, 2, nullSrvs);
        dc->CopyStructureCount(em.counts.Get(), 0, em.deadUAV.Get());
        dc->CopyStructureCount(em.counts.Get(), 4, em.aliveUAV[aliveIn].Get());

        // ---- 放出パス: aliveIn に追記 (カウンタ保持 = -1) ----
        if (emitCount > 0) {
            ID3D11UnorderedAccessView* uavs[3] = { em.poolUAV.Get(), em.deadUAV.Get(),
                                                   em.aliveUAV[aliveIn].Get() };
            const UINT inits[3] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
            dc->CSSetUnorderedAccessViews(0, 3, uavs, inits);
            ID3D11ShaderResourceView* srvs[2] = { em.emitSRV.Get(), em.countsSRV.Get() };
            dc->CSSetShaderResources(0, 2, srvs);
            dc->CSSetShader(emitShader, nullptr, 0);
            dc->Dispatch((static_cast<UINT>(emitCount) + 63) / 64, 1, 1);

            // 放出後の aliveIn カウントを再取得 (SRV/UAV を外してから)
            dc->CSSetShaderResources(0, 2, nullSrvs);
            dc->CSSetUnorderedAccessViews(0, 3, nullUavs, nullptr);
            dc->CopyStructureCount(em.counts.Get(), 4, em.aliveUAV[aliveIn].Get());
        }

        // ---- 更新パス: aliveIn → aliveOut (圧縮)。aliveOut カウンタは 0 リセット ----
        {
            ID3D11UnorderedAccessView* uavs[3] = { em.poolUAV.Get(), em.deadUAV.Get(),
                                                   em.aliveUAV[aliveOut].Get() };
            const UINT inits[3] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0 };
            dc->CSSetUnorderedAccessViews(0, 3, uavs, inits);
            ID3D11ShaderResourceView* srvs[2] = { em.aliveSRV[aliveIn].Get(), em.countsSRV.Get() };
            dc->CSSetShaderResources(0, 2, srvs);
            dc->CSSetShader(simShader, nullptr, 0);
            dc->Dispatch((em.capacity + 255) / 256, 1, 1);
        }

        // ---- 描画用: InstanceCount を GPU 上で確定 (リードバックなし) ----
        dc->CSSetShaderResources(0, 2, nullSrvs);
        dc->CSSetUnorderedAccessViews(0, 3, nullUavs, nullptr);
        dc->CopyStructureCount(em.indirectArgs.Get(), 4, em.aliveUAV[aliveOut].Get());
        em.aliveCurrent = aliveOut;

        aliveEstimate += em.capacity; // 概算 (正確な alive は GPU 上にのみ存在)
    }

    dc->CSSetShader(nullptr, nullptr, 0);
    timer_.End(*device_);
    stats_.updateMs = timer_.Milliseconds();
    stats_.aliveTotal = aliveEstimate;
}

void GpuParticleBackend::Render(GraphicsDevice& device, const RenderView& view,
                                ShaderManager& shaders, RenderResources& resources,
                                float renderOffsetX)
{
    (void)resources; // GPU 側テクスチャは将来対応 (CPU バックエンドが主。M32b)
    ShaderProgram* prog = shaders.Get(renderShader_);
    if (!prog || !prog->valid) {
        return;
    }
    ID3D11DeviceContext* dc = device.Context();

    GpuRenderCB cb = {};
    const XMMATRIX v = XMLoadFloat4x4(&view.view);
    const XMMATRIX p = XMLoadFloat4x4(&view.proj);
    XMStoreFloat4x4(&cb.viewProj, XMMatrixTranspose(XMMatrixMultiply(v, p)));
    cb.camRight = { view.view._11, view.view._21, view.view._31 };
    cb.camUp = { view.view._12, view.view._22, view.view._32 };
    cb.offsetX = renderOffsetX;
    UploadCB(dc, renderCB_.Get(), cb);

    dc->IASetInputLayout(nullptr);
    dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    dc->VSSetShader(prog->vs.Get(), nullptr, 0);
    dc->PSSetShader(prog->ps.Get(), nullptr, 0);
    dc->OMSetDepthStencilState(depthNoWrite_.Get(), 0);

    for (GpuEmitter& em : emitters_) {
        // sim CB (b0: sizeEndScale/color) はエミッタ毎に更新
        GpuParticleCB simCb = {};
        simCb.params = { 0, em.descCache.turbulence, em.descCache.sizeEndScale,
                         static_cast<float>(em.capacity) };
        simCb.colorBegin = em.descCache.colorBegin;
        simCb.colorEnd = em.descCache.colorEnd;
        UploadCB(dc, simCB_.Get(), simCb);
        ID3D11Buffer* cbs[2] = { simCB_.Get(), renderCB_.Get() };
        dc->VSSetConstantBuffers(0, 2, cbs);

        ID3D11ShaderResourceView* srvs[2] = { em.poolSRV.Get(),
                                              em.aliveSRV[em.aliveCurrent].Get() };
        dc->VSSetShaderResources(0, 2, srvs);
        dc->OMSetBlendState(em.descCache.blendMode == 1 ? blendAlpha_.Get() : blendAdditive_.Get(),
                            nullptr, 0xFFFFFFFFu);
        dc->DrawInstancedIndirect(em.indirectArgs.Get(), 0);
    }

    ID3D11ShaderResourceView* nullSrvs[2] = { nullptr, nullptr };
    dc->VSSetShaderResources(0, 2, nullSrvs);
    dc->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
    dc->OMSetDepthStencilState(nullptr, 0);
}

} // namespace mye
