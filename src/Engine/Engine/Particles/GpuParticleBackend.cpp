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
    XMFLOAT4 params4;           // M61d: turbulenceMode, noiseFrequency, noiseSpeed, noiseTime
};

struct GpuRenderCB { // particle_render_gpu.hlsl の GpuRenderCB と一致
    XMFLOAT4X4 viewProj;
    XMFLOAT3 camRight;
    float pad0;
    XMFLOAT3 camUp;
    float pad1;
    float offsetX;
    float pad2[3];
    // ---- M61g: ローカルシミュレーション空間 (末尾 append。HLSL 側と両方同時に変更する) ----
    XMFLOAT4X4 emitterWorld; // transpose 済み (mul(float4, M) 規約は viewProj と同じ)
    XMFLOAT4 spaceParams;    // x = simulationSpace (1 = pos を emitterWorld で変換), yzw = 予約
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
    // M61e: CPU 側 (CpuParticleBackend::SyncEmitters) と同じ意味論 — 非アクティブ化は
    // リソース破棄ではなく凍結保持。破棄は存在自体が消えたときだけで、新規作成は従来
    // どおりアクティブ時のみ。erase してしまうと再アクティブ化で RNG 再シード + 全粒子
    // 消失になり、CPU 側の「続きから動く」と食い違う
    std::vector<EntityID> existing; // 両コンポーネントを持つ全エンティティ (凍結中も含む)
    std::vector<EntityID> active;   // うちアクティブなもの (リソース新規作成の対象)
    const ComponentTypeId req[] = { ParticleEmitterComponent::sTypeId,
                                    WorldMatrixComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            existing.push_back(e);
            if (IsEntityActive(world, e)) {
                active.push_back(e);
            }
        }
    });
    std::sort(active.begin(), active.end(),
              [](EntityID a, EntityID b) { return a.index < b.index; });

    std::erase_if(emitters_, [&](const GpuEmitter& em) {
        return std::find(existing.begin(), existing.end(), em.owner) == existing.end();
    });
    for (EntityID e : active) {
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
            // M61f: クランプは Update の容量追従と共通の純関数 (基準がずれると毎 tick 再作成)
            const uint32_t cap = GpuEmitterCapacityFor(desc ? desc->maxParticles : 100000);
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

        // M61e: 凍結 (存在するが非アクティブ)。CPU 側の凍結と同じ「時が止まる」意味論 —
        // gpuIdle が「D3D 作業だけ省いて放出計画 (PlanParticleEmission) と推定器の記帳は
        // 続ける」のと違い、凍結中はその両方も止める。進めてしまうと解凍時に放出
        // スケジュールと寿命満期が凍結時間ぶん先へ飛び、CPU バックエンドの凍結挙動と
        // 食い違う。GPU リソースは保持したまま = 解凍でそのまま続きから動く (再シード無し)
        em.frozen = !IsEntityActive(world, em.owner);
        if (em.frozen) {
            aliveEstimate += em.aliveEst.Alive(em.capacity); // 粒子は保持されたまま (表示専用)
            continue;
        }

        em.descCache = *desc;
        // M61g: Render 用ワールド行列キャッシュ (descCache と同じ扱い。CPU 側 renderWorld の
        // ミラー)。ローカル空間の VS 変換に使う — 常時コピーで空間切り替えに即応する
        em.renderWorld = wm->value;

        // ---- M61f: maxParticles の変更追従 (容量が違えばリソース一式を作り直す) ----
        // 作り直しは一時オブジェクトで行い、成功時のみ move で差し替える — 失敗時 (VRAM 不足等)
        // は旧容量のまま動き続ける (ログのみ。旧リソースは 1 バイトも壊さない)。
        // em.rng は保持する — GPU プールは表示用ベストエフォート (ハッシュ非対象) なので
        // 再シードの義務はなく、放出計画 (ageTicks/emitAccum) との連続性を保つ方を選んだ。
        // 既存粒子は消える (プール/dead list が新品になる容量変更の代償。仕様)
        const uint32_t desiredCap = GpuEmitterCapacityFor(desc->maxParticles);
        if (GpuCapacityNeedsRecreate(em.capacity, desiredCap)) {
            GpuEmitter fresh;
            if (CreateEmitterResources(*device_, fresh, desiredCap)) {
                em.capacity = fresh.capacity;
                em.pool = std::move(fresh.pool);
                em.poolUAV = std::move(fresh.poolUAV);
                em.poolSRV = std::move(fresh.poolSRV);
                em.deadList = std::move(fresh.deadList);
                em.deadUAV = std::move(fresh.deadUAV);
                for (int i = 0; i < 2; ++i) {
                    em.alive[i] = std::move(fresh.alive[i]);
                    em.aliveUAV[i] = std::move(fresh.aliveUAV[i]);
                    em.aliveSRV[i] = std::move(fresh.aliveSRV[i]);
                }
                em.counts = std::move(fresh.counts);
                em.countsSRV = std::move(fresh.countsSRV);
                em.indirectArgs = std::move(fresh.indirectArgs); // InstanceCount=0 で再初期化済み
                // 生存推定と idle 状態は新プール基準へ (旧粒子は消えたので推定 0 から数え直し)
                em.aliveEst = {};
                em.idleTicks = 0;
                em.gpuIdle = false;
                em.aliveCurrent = 0;
            } else {
                MYE_LOG_ERROR("[gpu particles] capacity change failed (cap=%u -> %u), keeping old pool",
                              em.capacity, desiredCap);
            }
        }

        // M61g: ローカルシミュレーション空間 (CPU 側 Update と同じ判定・同じ原点規約 —
        // 放出原点は (0,0,0)、prevOrigin 履歴もローカル原点で回る)
        const bool localSpace = (desc->simulationSpace == 1);
        const XMFLOAT3 origin = localSpace
                                    ? XMFLOAT3{ 0.0f, 0.0f, 0.0f }
                                    : XMFLOAT3{ wm->value._41, wm->value._42, wm->value._43 };
        // M61b: CPU バックエンドと同じ基底 (恒等なら適用スキップ)。表示用ベストエフォート
        // だが放出コードは CPU 側と共有 = 見た目パリティを構造で保つ
        const ParticleEmitBasis basis = MakeParticleEmitBasis(wm->value);

        // M61c: エミッタ速度 (CPU 側 EmitParticles と同式)。履歴の更新は gpuIdle の
        // continue より前に置く — idle 中に止めると起床 tick に古い prevOrigin との差が
        // 巨大な速度スパイクとして出る
        XMFLOAT3 emitterVel = { 0.0f, 0.0f, 0.0f };
        if (em.prevOriginValid != 0 && dt > 0.0f) {
            const float invDt = 1.0f / dt;
            emitterVel = { (origin.x - em.prevOrigin.x) * invDt,
                           (origin.y - em.prevOrigin.y) * invDt,
                           (origin.z - em.prevOrigin.z) * invDt };
        }
        const XMFLOAT3 prevOrigin = em.prevOrigin;          // 今 tick の補間用 (上書き前に退避)
        const uint32_t prevOriginValid = em.prevOriginValid;
        em.prevOrigin = origin;
        em.prevOriginValid = 1;

        // ---- CPU 側で放出データを生成 (決定論 RNG。GPU では乱数を作らない) ----
        // 放出計画は CPU バックエンドと共有 (M32a: playing/duration/loop/burst)。表示用ベストエフォート。
        // M61e: プリウォームは GPU では行わない (spec 7.5 の等価規約に対する明示的な例外)。
        // GPU はハッシュ非対象の表示用ベストエフォートで、数百 tick ぶんの EmitData 生成 +
        // Dispatch を誕生 tick に畳み込む価値がない — 誕生直後の見た目差のみ許容する
        int emitCount = PlanParticleEmission(*desc, em.ageTicks, em.emitAccum, dt);
        // M61f: 旧「capacity/4」の 25% 静黙クランプを撤廃し容量全量まで許可。枯渇分は
        // emit CS の deadCount ガードが捨てる (判断の詳細は ParticleCurves.h::ClampGpuEmitCount)
        emitCount = ClampGpuEmitCount(emitCount, em.capacity);

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

        // M61b: 形状サンプリングは CPU バックエンドと共有 (SampleParticleShape。
        // 旧: ここに CpuParticleBackend::EmitParticles の手写しコピーが重複していた)
        // M61c: 速度継承とサブフレーム補間も CPU 側 EmitParticles と同式のミラー。
        // 唯一の差分: GPU 粒子の invLife は emit CS が 1/life から作るため、subframe の
        // 寿命前倒し (life = lifetime + f*dt) の分だけ age 曲線が最大 1 tick 分ずれる —
        // 表示専用の許容誤差 (EmitData に invLife は載せない)
        const bool subframe = (desc->subframeEmission != 0);
        std::vector<EmitData> emitData(static_cast<size_t>(std::max(emitCount, 0)));
        for (int n = 0; n < emitCount; ++n) {
            ParticleShapeSample smp = SampleParticleShape(*desc, em.rng);
            const float speed = em.rng.Range(desc->speedMin, desc->speedMax);
            const float lifetime = std::max(0.01f, em.rng.Range(desc->lifetimeMin, desc->lifetimeMax));
            const float size = em.rng.Range(desc->sizeMin, desc->sizeMax);
            // M61g: ローカル空間は基底適用をスキップ (ローカル系では恒等。エミッタの回転は
            // 描画時の gEmitterWorld 変換で掛かる) — CPU 側 EmitParticles と同じガード
            if (!basis.identity && !localSpace) {
                ParticleBasisRotateDir(basis, smp.dirX, smp.dirY, smp.dirZ);
                if (smp.hasOffset) {
                    ParticleBasisTransformOffset(basis, smp.offX, smp.offY, smp.offZ);
                }
            }
            float velX = smp.dirX * speed;
            float velY = smp.dirY * speed;
            float velZ = smp.dirZ * speed;
            // M61g: 速度継承はローカル空間では無効 (粒子はエミッタと一緒に動くので
            // 「置いていかれた速度」が成立しない) — CPU 側と同じガード
            if (desc->velocityInheritance != 0.0f && !localSpace) {
                velX += emitterVel.x * desc->velocityInheritance;
                velY += emitterVel.y * desc->velocityInheritance;
                velZ += emitterVel.z * desc->velocityInheritance;
            }
            float posX = origin.x, posY = origin.y, posZ = origin.z;
            float f = 0.0f;
            if (subframe) {
                f = ParticleSubframeFraction(n, emitCount);
                if (prevOriginValid != 0) {
                    posX = prevOrigin.x + (origin.x - prevOrigin.x) * f;
                    posY = prevOrigin.y + (origin.y - prevOrigin.y) * f;
                    posZ = prevOrigin.z + (origin.z - prevOrigin.z) * f;
                }
            }
            if (smp.hasOffset) {
                posX += smp.offX;
                posY += smp.offY;
                posZ += smp.offZ;
            }
            float lifeInit = lifetime;
            if (subframe) {
                const float fdt = f * dt;
                posX -= velX * fdt;
                posY -= velY * fdt;
                posZ -= velZ * fdt;
                lifeInit = lifetime + fdt;
            }
            em.aliveEst.OnEmit(lifeInit, dt); // 寿命は CPU 生成なので死亡 tick を記帳できる
            emitData[static_cast<size_t>(n)] = { { posX, posY, posZ }, lifeInit,
                                                 { velX, velY, velZ }, size };
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
        // M42e: 深度衝突 (エミッタ側 depthCollision かつ深度供給済みのときのみ有効)。
        // M61g: ローカル空間 (simulationSpace=1) では無効 — 衝突判定はワールド座標前提
        // (粒子位置を深度バッファへ投影する) で、ローカル座標をそのまま投影すると無関係な
        // 面と衝突する。sim CS へ渡す collParams.enabled を 0 に落とす (spec 7.5 例外の並び)
        const bool collide = (desc->depthCollision != 0) && (desc->simulationSpace != 1)
                             && collValid_ && collDepthSRV_;
        cb.collViewProj = collViewProj_;
        cb.collInvViewProj = collInvViewProj_;
        cb.collParams = { collide ? 1.0f : 0.0f, desc->collisionBounce, 0.0f, 0.0f };
        cb.collScreen = { collScreen_[0], collScreen_[1], collScreen_[2], collScreen_[3] };
        // M61d: カールノイズ乱流。時間は em.ageTicks (PlanParticleEmission が進めた後の値) 由来
        // — CPU 側 Simulate が見る pool.ageTicks と同じ位相 = パリティ一致。実時間は使わない
        cb.params4 = { static_cast<float>(desc->turbulenceMode), desc->noiseFrequency,
                       desc->noiseSpeed, static_cast<float>(em.ageTicks) * dt };
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
    // M61g: emitterWorld / spaceParams がエミッタ毎に変わるため、アップロードはエミッタ
    // ループ内で毎回行う (共通部は同じ値を書き直すだけ — 描画結果は従来と同一)

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
        if (em.frozen) {
            continue; // M61e: 凍結中は描かない (CPU 側の renderSkip と同じ意味論)
        }
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
        // M61g: ローカル空間はエミッタのワールド行列で VS が pos を変換する (transpose は
        // gViewProj と同じ規約)。ワールド空間 (既定) は flag=0 — 行列は VS が読まない
        XMStoreFloat4x4(&cb.emitterWorld, XMMatrixTranspose(XMLoadFloat4x4(&em.renderWorld)));
        cb.spaceParams = { (em.descCache.simulationSpace == 1) ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f };
        UploadCB(dc, renderCB_.Get(), cb);
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
