#include "Engine/Engine/Particles/GpuParticleBackend.h"

#include <algorithm>
#include <cmath>

#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/Particles/ParticleCurves.h"
#include "Engine/Renderer/GpuBufferUtil.h" // M46a: バッファ生成ヘルパ (共通化)
#include "Engine/Renderer/GpuResources.h" // M42c: TextureLibrary (フリップブック解決)
#include "Engine/Renderer/GraphicsDevice.h"
#include "Engine/Renderer/ShaderManager.h"

using namespace DirectX;

namespace mye {
namespace {

constexpr float kPi = 3.14159265358979323846f;

// 空 Dispatch 回避の猶予 tick 数。GpuAliveEstimator の丸め差は境界値で ±1 tick なので
// 8 は十分に保守側 (猶予中の Dispatch は従来コストのまま = 早すぎる skip だけが害になる)
constexpr int32_t kGpuIdleGraceTicks = 8;

struct GpuParticleCB { // particle_gpu_common.hlsli と一致
    XMFLOAT4 gravityWind; // xyz + dt
    XMFLOAT4 params;      // emitCount, turbulence, sizeEndScale, capacity
    XMFLOAT4 colorBegin;
    XMFLOAT4 colorEnd;
    XMFLOAT4 params2;     // M42b: softFade, nearZ, farZ / w = 予約
    XMFLOAT4 params3;     // M42c: useTexture, flipTilesX, flipTilesY, flipCycles
    XMFLOAT4X4 collViewProj;    // M42e: transpose 済み
    XMFLOAT4X4 collInvViewProj; // M42e: transpose 済み
    XMFLOAT4 collParams;        // M42e: enabled, restitution, thickness, 予約
    XMFLOAT4 collScreen;        // M42e: 画面 w/h, nearZ, farZ
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

// M46a: CreateStructured / CreateConstant / UploadCB は GpuBufferUtil.h へ集約 (定義は同一)
using namespace gpubuf;

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

    // M42c: フリップブックテクスチャ用サンプラ (CPU バックエンドと同じ linear clamp)
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateSamplerState(&sd, sampler_.GetAddressOf()))) {
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

        // 空 Dispatch 回避: 放出も推定生存も無いエミッタは GPU 作業 (CopyStructureCount×3 +
        // Dispatch + IndirectArgs 更新) を丸ごと省く。sim CS はスレッドが aliveCount で
        // 即 return するとはいえ、群起動だけで 1M 容量 ≒ 3907 グループ/tick かかっていた。
        // 放出計画 (PlanParticleEmission) と推定器の tick は継続する — 凍結すると復帰後の
        // 放出スケジュールと満期処理が狂う。描画専用の最適化 — sim 状態には一切触れない
        em.gpuIdle = StepGpuIdleSkip(emitCount, em.aliveEst.Alive(em.capacity),
                                     kGpuIdleGraceTicks, em.idleTicks);
        if (em.gpuIdle) {
            em.aliveEst.EndTick();
            continue;
        }

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
            em.aliveEst.OnEmit(lifetime, dt); // 寿命は CPU 生成なので死亡 tick を記帳できる
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
        // M42e: 深度衝突 (エミッタ側 depthCollision かつ深度供給済みのときのみ有効)
        const bool collide = (desc->depthCollision != 0) && collValid_ && collDepthSRV_;
        cb.collViewProj = collViewProj_;
        cb.collInvViewProj = collInvViewProj_;
        cb.collParams = { collide ? 1.0f : 0.0f, desc->collisionBounce, 0.0f, 0.0f };
        cb.collScreen = { collScreen_[0], collScreen_[1], collScreen_[2], collScreen_[3] };
        UploadCB(dc, simCB_.Get(), cb);
        ID3D11Buffer* cbs[1] = { simCB_.Get() };
        dc->CSSetConstantBuffers(0, 1, cbs);

        ID3D11UnorderedAccessView* nullUavs[3] = { nullptr, nullptr, nullptr };
        ID3D11ShaderResourceView* nullSrvs[3] = { nullptr, nullptr, nullptr };
        // M42e: t2 = 前フレーム深度 (sim のみ参照。emit は t2 未使用)
        if (collide) {
            ID3D11ShaderResourceView* dsrv = collDepthSRV_.Get();
            dc->CSSetShaderResources(2, 1, &dsrv);
        }

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
        dc->CSSetShaderResources(0, 3, nullSrvs); // M42e: t2 (深度) も解除
        dc->CSSetUnorderedAccessViews(0, 3, nullUavs, nullptr);
        dc->CopyStructureCount(em.indirectArgs.Get(), 4, em.aliveUAV[aliveOut].Get());
        em.aliveCurrent = aliveOut;

        // 生存数は寿命スケジュールの CPU 側推定 (正確な alive は GPU 上にのみ存在するが、
        // 死因は寿命だけなので readback なしでほぼ一致する。飽和時のみ capacity で頭打ち)
        em.aliveEst.EndTick();
        aliveEstimate += em.aliveEst.Alive(em.capacity);
    }

    dc->CSSetShader(nullptr, nullptr, 0);
    timer_.End(*device_);
    stats_.updateMs = timer_.Milliseconds();
    stats_.aliveTotal = aliveEstimate;
}

void GpuParticleBackend::SetSceneDepth(ID3D11ShaderResourceView* depthSRV,
                                       const XMFLOAT4X4& view, const XMFLOAT4X4& proj, int width,
                                       int height, float nearZ, float farZ)
{
    collDepthSRV_ = depthSRV; // ComPtr 保持 (リサイズ後も stale-but-safe)
    collValid_ = (depthSRV != nullptr) && width > 0 && height > 0;
    if (!collValid_) {
        return;
    }
    const XMMATRIX v = XMLoadFloat4x4(&view);
    const XMMATRIX p = XMLoadFloat4x4(&proj);
    const XMMATRIX vp = XMMatrixMultiply(v, p);
    XMStoreFloat4x4(&collViewProj_, XMMatrixTranspose(vp));
    XMStoreFloat4x4(&collInvViewProj_, XMMatrixTranspose(XMMatrixInverse(nullptr, vp)));
    collScreen_[0] = static_cast<float>(width);
    collScreen_[1] = static_cast<float>(height);
    collScreen_[2] = nearZ;
    collScreen_[3] = farZ;
}

void GpuParticleBackend::Render(GraphicsDevice& device, const RenderView& view,
                                ShaderManager& shaders, RenderResources& resources,
                                float renderOffsetX)
{
    ShaderProgram* prog = shaders.Get(renderShader_);
    if (!prog || !prog->valid) {
        return;
    }
    ID3D11DeviceContext* dc = device.Context();

    // M42c: フリップブックテクスチャの白フォールバック (CPU バックエンドと同じ)
    Texture* whiteTex = resources.textures.Get(resources.textures.White());
    ID3D11ShaderResourceView* whiteSrv = whiteTex ? whiteTex->srv.Get() : nullptr;

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

    // M42b: シーン深度を PS t2 へ (VS の t0/t1 とはステージ別で無衝突)。
    // depthSRV が無いビューでは softFade=0 に強制されるのでバインド不要
    if (view.depthSRV != nullptr) {
        dc->PSSetShaderResources(2, 1, &view.depthSRV);
    }

    for (GpuEmitter& em : emitters_) {
        if (em.gpuIdle) {
            continue; // 空 Dispatch 回避中 (最後の実 Dispatch が InstanceCount=0 を確定済み)
        }
        if (em.descCache.blendMode == 2) {
            continue; // M42d: 歪みは CPU バックエンド限定 (v1 制限。GPU は描画スキップ)
        }
        // sim CB (b0: sizeEndScale/color) はエミッタ毎に更新
        GpuParticleCB simCb = {};
        simCb.params = { 0, em.descCache.turbulence, em.descCache.sizeEndScale,
                         static_cast<float>(em.capacity) };
        simCb.colorBegin = em.descCache.colorBegin;
        simCb.colorEnd = em.descCache.colorEnd;
        // M42b: ソフトフェード (深度が読めるビューのみ有効)
        simCb.params2 = { (view.depthSRV != nullptr) ? em.descCache.softFadeDistance : 0.0f,
                          view.nearZ, view.farZ, 0.0f };
        // M42c: テクスチャ解決 (空なら procedural 円へフォールバック。CPU 側 :504-510 と同型)
        ID3D11ShaderResourceView* texSrv = nullptr;
        if (em.descCache.texture.value != 0) {
            if (Texture* t = resources.textures.Get(em.descCache.texture)) {
                texSrv = t->srv.Get();
            }
        }
        simCb.params3 = { texSrv ? 1.0f : 0.0f,
                          static_cast<float>(std::max(1, em.descCache.flipTilesX)),
                          static_cast<float>(std::max(1, em.descCache.flipTilesY)),
                          em.descCache.flipCycles };
        UploadCB(dc, simCB_.Get(), simCb);
        ID3D11Buffer* cbs[2] = { simCB_.Get(), renderCB_.Get() };
        dc->VSSetConstantBuffers(0, 2, cbs);
        dc->PSSetConstantBuffers(0, 2, cbs); // M42b: PS もソフトフェードで b0 を参照

        ID3D11ShaderResourceView* srvs[2] = { em.poolSRV.Get(),
                                              em.aliveSRV[em.aliveCurrent].Get() };
        dc->VSSetShaderResources(0, 2, srvs);
        ID3D11ShaderResourceView* psTex = texSrv ? texSrv : whiteSrv;
        dc->PSSetShaderResources(3, 1, &psTex); // M42c: t3
        ID3D11SamplerState* samp = sampler_.Get();
        dc->PSSetSamplers(0, 1, &samp);
        dc->OMSetBlendState(em.descCache.blendMode == 1 ? blendAlpha_.Get() : blendAdditive_.Get(),
                            nullptr, 0xFFFFFFFFu);
        dc->DrawInstancedIndirect(em.indirectArgs.Get(), 0);
    }

    ID3D11ShaderResourceView* nullSrvs[3] = { nullptr, nullptr, nullptr };
    dc->VSSetShaderResources(0, 2, nullSrvs);
    dc->PSSetShaderResources(2, 2, nullSrvs); // M42b/c: t2=深度, t3=テクスチャを解除
    dc->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
    dc->OMSetDepthStencilState(nullptr, 0);
}

} // namespace mye
