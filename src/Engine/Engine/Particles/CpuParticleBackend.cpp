#include "Engine/Engine/Particles/CpuParticleBackend.h"

#include <algorithm>
#include <cmath>

#include <xmmintrin.h>

#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/Particles/ParticleCurves.h"
#include "Engine/Platform/Clock.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/GraphicsDevice.h"
#include "Engine/Renderer/ShaderManager.h"

using namespace DirectX;

namespace mye {
namespace {

struct ParticleInstance {
    XMFLOAT3 pos;
    float size;
    XMFLOAT4 color;
    float age; // [0,1] 寿命係数 (M32b フリップブック用)
    float pad[3];
};

struct ParticleCB {
    XMFLOAT4X4 viewProj; // 転置済み
    XMFLOAT3 camRight;
    float pad0;
    XMFLOAT3 camUp;
    float pad1;
    uint32_t baseIndex;
    uint32_t useTexture; // 0=procedural 円 / 1=テクスチャ (フリップブック)
    int32_t flipTilesX;
    int32_t flipTilesY;
    float flipCycles;
    int32_t blendAdditive; // 1=additive (fog=減光) / 0=alpha (fog=色 lerp)
    int32_t fogMode;       // -1=off / 0=linear 1=exp 2=exp2 (M32c)
    float pad2;
    XMFLOAT3 cameraPos;
    float fogDensity;
    XMFLOAT3 fogColor;
    float fogStart;
    float fogEnd;
    // M42b: ソフトパーティクル (旧 pad3 転用。particle_render.hlsl の ParticleCB と一致)
    float softFade; // 深度フェード距離 (0=off)
    float nearZ;    // 深度線形化用
    float farZ;
    // ---- M57e: フロクセル (末尾 append。0 = 従来と 1 ビットも変わらない) ----
    // ★particle_distort.hlsl は ParticleCB を**手前で切り詰めて**宣言しているので
    //   あちらには触らなくてよい (歪みは色を出さないので霧も要らない)
    int32_t froxelEnabled;
    float froxelNearZ;
    float froxelFarZ;
    float froxelSlices;
    float froxelScreenSize[2]; // SV_Position → uv
    float froxelPad[2];
};

} // namespace

bool CpuParticleBackend::Init(GraphicsDevice& device, ShaderManager& shaders)
{
    shaderId_ = shaders.Load("particle_render");
    distortShaderId_ = shaders.Load("particle_distort"); // M42d: blendMode=2 用

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = sizeof(ParticleCB);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device.Device()->CreateBuffer(&cbd, nullptr, renderCB_.GetAddressOf()))) {
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
    if (FAILED(device.Device()->CreateBlendState(&bd, blendAdditive_.GetAddressOf()))) {
        return false;
    }
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    if (FAILED(device.Device()->CreateBlendState(&bd, blendAlpha_.GetAddressOf()))) {
        return false;
    }

    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; // 深度テストのみ (書き込みなし)
    dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    if (FAILED(device.Device()->CreateDepthStencilState(&dd, depthNoWrite_.GetAddressOf()))) {
        return false;
    }

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device.Device()->CreateSamplerState(&sd, sampler_.GetAddressOf()))) {
        return false;
    }
    return true;
}

void CpuParticleBackend::Shutdown()
{
    pools_.clear();
    instanceBuffer_.Reset();
    instanceSRV_.Reset();
    renderCB_.Reset();
    blendAdditive_.Reset();
    blendAlpha_.Reset();
    depthNoWrite_.Reset();
    sampler_.Reset();
}

void CpuParticleBackend::Reset()
{
    pools_.clear();
    stats_ = {};
}

void CpuParticleBackend::SyncEmitters(World& world)
{
    // 現存エミッタを index 昇順で収集 (決定論)。
    // M61e: 「存在」と「アクティブ」を分けて集める — 非アクティブ化はプール破棄ではなく
    // 凍結保持 (Update 側で完全に時を止め、再アクティブ化で再シードせず続きから動く)。
    // 破棄はエンティティ/コンポーネントの存在自体が消えたときだけ。プールの新規作成は
    // 従来どおりアクティブ時のみ = 「誕生時から無効のエミッタはプールを持たない」という
    // M10 の意味論は保存する
    std::vector<EntityID> existing; // 両コンポーネントを持つ全エンティティ (凍結中も含む)
    std::vector<EntityID> active;   // うちアクティブなもの (プール新規作成の対象)
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

    // 消えたエミッタのプールを除去 (M61e: 非アクティブでは消さない — 凍結して保持)
    std::erase_if(pools_, [&](const EmitterPool& p) {
        return std::find(existing.begin(), existing.end(), p.owner) == existing.end();
    });
    // 新規エミッタのプールを作成 (シードはコンポーネント指定 — spec 7.3)
    for (EntityID e : active) {
        bool exists = false;
        for (const EmitterPool& p : pools_) {
            if (p.owner == e) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            EmitterPool pool;
            pool.owner = e;
            const auto* desc = world.GetComponent<ParticleEmitterComponent>(e);
            pool.rng.Seed(desc ? desc->seed : 1u, static_cast<uint64_t>(e.index) * 2u + 1u);
            pools_.push_back(std::move(pool));
        }
    }
    std::sort(pools_.begin(), pools_.end(),
              [](const EmitterPool& a, const EmitterPool& b) { return a.owner.index < b.owner.index; });
}

void CpuParticleBackend::EmitParticles(EmitterPool& pool, const ParticleEmitterComponent& desc,
                                       const XMFLOAT3& origin, const ParticleEmitBasis& basis,
                                       float dt)
{
    // 放出計画 (playing/duration/loop/burst)。ageTicks/emitAccum を進める。
    // 既定エミッタ (duration=0, burst=0) では従来の emitAccum ロジックとビット同一に縮退する。
    int emit = PlanParticleEmission(desc, pool.ageTicks, pool.emitAccum, dt);
    if (emit <= 0) {
        return;
    }

    const int cap = std::max(0, desc.maxParticles);
    emit = std::min(emit, cap - static_cast<int>(pool.alive)); // burst 優先で cap
    if (emit <= 0) {
        return;
    }

    const size_t needed = pool.alive + static_cast<uint32_t>(emit);
    if (pool.px.size() < needed) {
        const size_t newSize = std::min<size_t>(std::max<size_t>(needed, pool.px.size() * 2 + 64),
                                                static_cast<size_t>(cap));
        pool.px.resize(newSize); pool.py.resize(newSize); pool.pz.resize(newSize);
        pool.vx.resize(newSize); pool.vy.resize(newSize); pool.vz.resize(newSize);
        pool.life.resize(newSize); pool.invLife.resize(newSize); pool.size0.resize(newSize);
    }

    // M61g: ローカルシミュレーション空間 (simulationSpace=1)。呼び出し側 (Update) が
    // origin=(0,0,0) を渡してくるので、ここでは「基底適用のスキップ (ローカル系では恒等)」と
    // 「速度継承の無効化」だけを担う。ガードは全て localSpace 側の分岐 —
    // 既定 (0=ワールド) の演算列は 1 ビットも変えない
    const bool localSpace = (desc.simulationSpace == 1);
    // M61c: エミッタ速度 (prevOrigin 履歴から。プール誕生 tick は履歴なし = 0)。
    // 消費するのは速度継承 (velocityInheritance != 0) とサブフレーム補間だけ —
    // 既定 (係数 0 / subframe 0) では値を読みもしないため従来とビット同一。
    // ローカル空間では prevOrigin も origin も (0,0,0) なので定常的に 0 になる
    const bool subframe = (desc.subframeEmission != 0);
    XMFLOAT3 emitterVel = { 0.0f, 0.0f, 0.0f };
    if (pool.prevOriginValid != 0 && dt > 0.0f) {
        const float invDt = 1.0f / dt;
        emitterVel = { (origin.x - pool.prevOrigin.x) * invDt,
                       (origin.y - pool.prevOrigin.y) * invDt,
                       (origin.z - pool.prevOrigin.z) * invDt };
    }

    for (int n = 0; n < emit; ++n) {
        const uint32_t i = pool.alive++;
        // 乱数の消費順は固定 (決定論): 方向 → 位置 → 速度 → 寿命 → サイズ。
        // (shape, emitFrom) ごとの消費数は SampleParticleShape の表が契約 (M61b で共通化 —
        // emitFrom=0 は旧 switch と同一の演算列・同一の消費列に縮退する)
        ParticleShapeSample smp = SampleParticleShape(desc, pool.rng);
        const float speed = pool.rng.Range(desc.speedMin, desc.speedMax);
        const float lifetime = std::max(0.01f, pool.rng.Range(desc.lifetimeMin, desc.lifetimeMax));
        const float size = pool.rng.Range(desc.sizeMin, desc.sizeMax);

        // M61b: 非恒等の基底 (回転*スケール) だけ方向とオフセットへ適用する。恒等はこの
        // ブロックを丸ごと飛ばす = 従来とビット同一 (基底適用は RNG を消費しない)。
        // M61g: ローカル空間もスキップ — SoA はエミッタ座標系なので放出基底は恒等。
        // エミッタの回転は描画時の renderWorld 変換で全生存粒子へ一括で掛かる
        if (!basis.identity && !localSpace) {
            ParticleBasisRotateDir(basis, smp.dirX, smp.dirY, smp.dirZ);
            if (smp.hasOffset) {
                ParticleBasisTransformOffset(basis, smp.offX, smp.offY, smp.offZ);
            }
        }

        float velX = smp.dirX * speed;
        float velY = smp.dirY * speed;
        float velZ = smp.dirZ * speed;
        if (desc.velocityInheritance != 0.0f && !localSpace) {
            // M61c ③: エミッタ速度の継承。係数 0.0f では演算自体をしない —
            // +0.0f の加算でも -0.0 が +0.0 に化けるため (ビット保存の契約)。
            // M61g: ローカル空間では明示的に無効 — 粒子はエミッタと一緒に動くので
            // 「置いていかれた速度」の概念が成立しない。emitterVel は定常的に 0 だが、
            // +0.0f 加算のビット化けを避ける意味でも係数 on の分岐ごとスキップする
            velX += emitterVel.x * desc.velocityInheritance;
            velY += emitterVel.y * desc.velocityInheritance;
            velZ += emitterVel.z * desc.velocityInheritance;
        }

        float posX = origin.x, posY = origin.y, posZ = origin.z;
        float f = 0.0f;
        if (subframe) {
            // M61c ②: 誕生フラクション f=(n+0.5)/N で tick 区間へ等分散。放出基準点は
            // prevOrigin→origin の補間 (履歴なし = プール誕生 tick は origin のまま)
            f = ParticleSubframeFraction(n, emit);
            if (pool.prevOriginValid != 0) {
                posX = pool.prevOrigin.x + (origin.x - pool.prevOrigin.x) * f;
                posY = pool.prevOrigin.y + (origin.y - pool.prevOrigin.y) * f;
                posZ = pool.prevOrigin.z + (origin.z - pool.prevOrigin.z) * f;
            }
        }
        if (smp.hasOffset) {
            // hasOffset=false のときは加算自体をしない (origin.x + 0.0f は -0.0 を +0.0 に
            // 変えるので、point/cone(apex) の従来経路はここを通らないことがビット保存の条件)
            posX += smp.offX;
            posY += smp.offY;
            posZ += smp.offZ;
        }
        float lifeInit = lifetime;
        if (subframe) {
            // M61c ②: 同 tick の Simulate は全量 dt を積分する — 初期値側で辻褄を合わせる。
            //   誕生後の運動を (1-f)*dt 分にしたい → pos から vel*f*dt を前倒しで引く
            //   寿命消費も (1-f)*dt 分にしたい → life に f*dt を前倒しで足す
            // Simulate 後: pos ≈ 誕生位置 + vel*(1-f)dt (加速度分 O(a*dt^2) は許容)、
            // life = lifetime - (1-f)dt。invLife は本来の lifetime 基準のまま —
            // age 曲線の勾配は変えない (誕生直後の age が僅かに負になるのは描画 clamp が吸収)
            const float fdt = f * dt;
            posX -= velX * fdt;
            posY -= velY * fdt;
            posZ -= velZ * fdt;
            lifeInit = lifetime + fdt;
        }

        pool.px[i] = posX;
        pool.py[i] = posY;
        pool.pz[i] = posZ;
        pool.vx[i] = velX;
        pool.vy[i] = velY;
        pool.vz[i] = velZ;
        pool.life[i] = lifeInit;
        pool.invLife[i] = 1.0f / lifetime;
        pool.size0[i] = size;
    }
}

void CpuParticleBackend::SimulateScalar(EmitterPool& pool, const XMFLOAT3& accel, float dt,
                                        uint32_t begin, uint32_t end)
{
    // SIMD 本体とレーン毎に同一の演算列 (mul→add の順) — 結果はビット一致する
    const float turb = turb_;
    // M61d: turbulenceMode=1 は位置ベースのカールノイズ場を加速度に使う。専用ループに分離し、
    // mode=0 の既存ループは 1 命令も変えない (既定エミッタのビット保存)。ノイズは位置と時間の
    // 純関数で pool.rng を消費しない — RNG 消費列は放出経路のまま不変。
    // このモードのプールは Simulate が常にスカラー経路へ落とすので SIMD 側の対応は不要
    if (turbMode_ == 1) {
        const float freq = noiseFreq_;
        const float t = noiseTime_ * noiseSpeed_;
        for (uint32_t i = begin; i < end; ++i) {
            const DirectX::XMFLOAT3 curl = EvalCurlNoise(
                { pool.px[i] * freq, pool.py[i] * freq, pool.pz[i] * freq }, t);
            const float ax = accel.x + turb * curl.x;
            const float ay = accel.y + turb * curl.y;
            const float az = accel.z + turb * curl.z;
            pool.vx[i] += ax * dt;
            pool.vy[i] += ay * dt;
            pool.vz[i] += az * dt;
            pool.px[i] += pool.vx[i] * dt;
            pool.py[i] += pool.vy[i] * dt;
            pool.pz[i] += pool.vz[i] * dt;
            pool.life[i] -= dt;
        }
        return;
    }
    for (uint32_t i = begin; i < end; ++i) {
        const float ax = accel.x + turb * (-pool.vz[i]);
        const float ay = accel.y;
        const float az = accel.z + turb * pool.vx[i];
        pool.vx[i] += ax * dt;
        pool.vy[i] += ay * dt;
        pool.vz[i] += az * dt;
        pool.px[i] += pool.vx[i] * dt;
        pool.py[i] += pool.vy[i] * dt;
        pool.pz[i] += pool.vz[i] * dt;
        pool.life[i] -= dt;
    }
}

void CpuParticleBackend::Simulate(EmitterPool& pool, const ParticleEmitterComponent& desc, float dt)
{
    // M61g: simulationSpace=1 (ローカル) でも力場は一切変換しない — gravity/wind も
    // 渦/カールノイズも**ローカル系のベクトル/場として解釈する** (v1 仕様。エミッタが
    // 回転すると重力の見た目の向きも一緒に回る)。式が両空間で同一なので分岐自体が不要 =
    // ワールド空間 (既定) の演算列はビット不変
    const XMFLOAT3 accel = { desc.gravity.x + desc.wind.x, desc.gravity.y + desc.wind.y,
                             desc.gravity.z + desc.wind.z };
    turb_ = desc.turbulence;
    // M61d: カールノイズ乱流のパラメータ (SimulateScalar が読む)。時間は pool.ageTicks 由来 —
    // EmitParticles の PlanParticleEmission が進めた後の値で、GPU 側 (Update の CB 充填) も
    // 同じ「Plan 後の ageTicks」を見る = パリティ一致。looping でウィンドウが巻き戻ると
    // ノイズ時間も巻き戻るが、sim 状態のみ由来なので決定論は保たれる
    turbMode_ = desc.turbulenceMode;
    noiseFreq_ = desc.noiseFrequency;
    noiseSpeed_ = desc.noiseSpeed;
    noiseTime_ = static_cast<float>(pool.ageTicks) * dt;

    // M61d: mode=1 はノイズ評価が粒子ごとの散在格子参照になり 4-wide 化しないため、プール単位で
    // スカラー経路へ落とす。desc 由来の分岐 = 全ビルド/全機種で同一判定 = 決定論 OK。
    // mode=0 は従来どおり SIMD (ビット不変)
    if (!simd_ || pool.alive < 8 || desc.turbulenceMode == 1) {
        SimulateScalar(pool, accel, dt, 0, pool.alive);
        return;
    }

    const uint32_t simdCount = pool.alive & ~3u;
    const __m128 dt4 = _mm_set1_ps(dt);
    const __m128 gx4 = _mm_set1_ps(accel.x);
    const __m128 gy4 = _mm_set1_ps(accel.y);
    const __m128 gz4 = _mm_set1_ps(accel.z);
    const __m128 turb4 = _mm_set1_ps(turb_);
    const __m128 zero = _mm_setzero_ps();

    for (uint32_t i = 0; i < simdCount; i += 4) {
        __m128 vx4 = _mm_loadu_ps(&pool.vx[i]);
        __m128 vy4 = _mm_loadu_ps(&pool.vy[i]);
        __m128 vz4 = _mm_loadu_ps(&pool.vz[i]);

        const __m128 ax4 = _mm_add_ps(gx4, _mm_mul_ps(turb4, _mm_sub_ps(zero, vz4)));
        const __m128 az4 = _mm_add_ps(gz4, _mm_mul_ps(turb4, vx4));

        vx4 = _mm_add_ps(vx4, _mm_mul_ps(ax4, dt4));
        vy4 = _mm_add_ps(vy4, _mm_mul_ps(gy4, dt4));
        vz4 = _mm_add_ps(vz4, _mm_mul_ps(az4, dt4));

        __m128 px4 = _mm_loadu_ps(&pool.px[i]);
        __m128 py4 = _mm_loadu_ps(&pool.py[i]);
        __m128 pz4 = _mm_loadu_ps(&pool.pz[i]);
        px4 = _mm_add_ps(px4, _mm_mul_ps(vx4, dt4));
        py4 = _mm_add_ps(py4, _mm_mul_ps(vy4, dt4));
        pz4 = _mm_add_ps(pz4, _mm_mul_ps(vz4, dt4));

        _mm_storeu_ps(&pool.vx[i], vx4);
        _mm_storeu_ps(&pool.vy[i], vy4);
        _mm_storeu_ps(&pool.vz[i], vz4);
        _mm_storeu_ps(&pool.px[i], px4);
        _mm_storeu_ps(&pool.py[i], py4);
        _mm_storeu_ps(&pool.pz[i], pz4);

        __m128 life4 = _mm_loadu_ps(&pool.life[i]);
        _mm_storeu_ps(&pool.life[i], _mm_sub_ps(life4, dt4));
    }
    SimulateScalar(pool, accel, dt, simdCount, pool.alive); // 端数レーン
}

void CpuParticleBackend::KillDead(EmitterPool& pool)
{
    uint32_t i = 0;
    while (i < pool.alive) {
        if (pool.life[i] <= 0.0f) {
            const uint32_t last = --pool.alive;
            pool.px[i] = pool.px[last];
            pool.py[i] = pool.py[last];
            pool.pz[i] = pool.pz[last];
            pool.vx[i] = pool.vx[last];
            pool.vy[i] = pool.vy[last];
            pool.vz[i] = pool.vz[last];
            pool.life[i] = pool.life[last];
            pool.invLife[i] = pool.invLife[last];
            pool.size0[i] = pool.size0[last];
            // 入れ替えた要素を再判定するため i は進めない
        } else {
            ++i;
        }
    }
}

void CpuParticleBackend::Update(World& world, float dt)
{
    Clock timer;
    timer.Init();

    SyncEmitters(world);

    uint32_t aliveTotal = 0;
    for (EmitterPool& pool : pools_) {
        const auto* desc = world.GetComponent<ParticleEmitterComponent>(pool.owner);
        const auto* wm = world.GetComponent<WorldMatrixComponent>(pool.owner);
        if (!desc || !wm) {
            continue;
        }

        // M61e: 凍結 (存在するが非アクティブ)。プールは破棄せず完全に時を止める —
        // Emit/Simulate/KillDead/バウンズ/prevOrigin 履歴のどれも進めない (rng も ageTicks も
        // 不変 = ワールドハッシュも不変)。descCache も更新しない (凍結中は描かないので
        // 古いままで害がない)。再アクティブ化はこの分岐を抜けるだけ — 再シードもリセットも
        // 無しで、凍結前の続きからビット同一に動き出す
        const bool frozen = !IsEntityActive(world, pool.owner);
        pool.renderSkip = frozen; // 描画専用フラグ (ハッシュ/スナップショット非対象)
        if (frozen) {
            aliveTotal += pool.alive; // 粒子は保持されたまま止まっている (統計は表示専用)
            continue;
        }

        pool.descCache = *desc; // 描画時に World を引かないためのコピー
        // M61g: renderWorld は descCache と同じ「Render で World を引かないためのキャッシュ」
        // (ハッシュ/スナップショット非対象)。simulationSpace=1 のプールだけが描画変換に使うが、
        // 実行中の空間切り替えで即座に有効になるよう常時コピーする
        pool.renderWorld = wm->value;
        // M61g: ローカルシミュレーション空間 (simulationSpace=1) は SoA (px..vz) を
        // エミッタのローカル系で保持する — 放出原点は常に (0,0,0) で、エミッタの移動・回転は
        // sim に一切入らず、描画時の renderWorld 変換で全生存粒子が剛体追従する。
        // prevOrigin 履歴もローカル原点で更新される (下の履歴更新は同じ origin 変数を使う) ため、
        // サブフレーム補間の位置補間は自然に消え、部分 tick 前進だけがローカル座標で効く。
        // ★実行中に simulationSpace を切り替えると生存粒子の座標解釈が変わって絵が跳ぶ — 仕様
        //   (移行処理は書かない。切り替え直後の 1 tick は prevOrigin も旧空間の値のまま)
        const bool localSpace = (desc->simulationSpace == 1);
        const XMFLOAT3 origin = localSpace
                                    ? XMFLOAT3{ 0.0f, 0.0f, 0.0f }
                                    : XMFLOAT3{ wm->value._41, wm->value._42, wm->value._43 };
        // M61b: 上 3x3 (回転*スケール) を放出に適用する。9 成分が厳密に恒等なら
        // EmitParticles 内で従来経路に縮退 = 既存コンテンツのビット保存
        // (M61g: ローカル空間は非恒等でも EmitParticles 内でスキップされる)
        const ParticleEmitBasis basis = MakeParticleEmitBasis(wm->value);

        // M61e: プリウォーム — プール誕生 tick (prewarmed==0 はここでしか真にならない) に、
        // prewarmTime 秒ぶんの {放出→積分→消滅} を通常処理の前へ同一 tick 内で先回しする。
        // origin は現在値固定 (prevOriginValid==0 のままなので M61c の補間・速度継承も
        // 自然に無効 = 速度 0 扱い)。rng/ageTicks/emitAccum は普通に進む = sim 状態として
        // ハッシュに乗り、どのビルドでも同じ回数だけ回るので決定論は保たれる。上限 600 tick
        // (10 秒 @60Hz) は誤設定の巨大値が 1 tick を丸ごと食い潰す暴走ガード。snapshot 復元後は
        // prewarmed==1 ごと復元されるため再トリガしない (selftest M61e 節で確認)。
        // GPU バックエンドはプリウォームしない (spec 7.5 の等価規約の例外 —
        // GpuParticleBackend::Update のコメント参照)
        if (pool.prewarmed == 0 && desc->prewarmTime > 0.0f && desc->playing != 0) {
            const int prewarmTicks = std::min(600, static_cast<int>(desc->prewarmTime / dt));
            for (int step = 0; step < prewarmTicks; ++step) {
                EmitParticles(pool, *desc, origin, basis, dt);
                Simulate(pool, *desc, dt);
                KillDead(pool);
            }
        }

        EmitParticles(pool, *desc, origin, basis, dt);
        Simulate(pool, *desc, dt);
        KillDead(pool);

        // 描画専用バウンズ (ハッシュ非対象)。シム更新自体は可視性でスキップしない —
        // プールはワールドハッシュ対象なので、更新カリングは即決定論違反になる
        // (ParticlePoolVisible のコメント参照)。O(alive) の min/max 走査のみで分岐なし
        pool.boundsValid = (pool.alive > 0);
        if (pool.boundsValid) {
            float mnx = pool.px[0], mxx = pool.px[0];
            float mny = pool.py[0], mxy = pool.py[0];
            float mnz = pool.pz[0], mxz = pool.pz[0];
            float msz = pool.size0[0];
            for (uint32_t i = 1; i < pool.alive; ++i) {
                mnx = std::min(mnx, pool.px[i]); mxx = std::max(mxx, pool.px[i]);
                mny = std::min(mny, pool.py[i]); mxy = std::max(mxy, pool.py[i]);
                mnz = std::min(mnz, pool.pz[i]); mxz = std::max(mxz, pool.pz[i]);
                msz = std::max(msz, pool.size0[i]);
            }
            pool.boundsMin = { mnx, mny, mnz };
            pool.boundsMax = { mxx, mxy, mxz };
            pool.maxSize0 = msz;
        }
        aliveTotal += pool.alive;

        // M61a: 次 tick のための原点履歴 (消費は M61b/c: サブフレーム補間と速度継承)。
        // prewarmed はプール誕生 tick の検出用 — ここで立てる前 (= 初回 Update の冒頭) だけ 0
        pool.prevOrigin = origin;
        pool.prevOriginValid = 1;
        pool.prewarmed = 1;
    }

    stats_.aliveTotal = aliveTotal;
    stats_.updateMs = static_cast<float>(timer.Now() * 1000.0);
}

void CpuParticleBackend::Render(GraphicsDevice& device, const RenderView& view,
                                ShaderManager& shaders, RenderResources& resources,
                                float renderOffsetX)
{
    ShaderProgram* prog = shaders.Get(shaderId_);
    if (!prog || !prog->valid) {
        return;
    }

    // プール単位フラスタムカリング (描画専用)。AABB は Update が撮ったもの — Render は
    // sim を進めないので同 tick 内では正確。invalid (スナップショット復元直後など) は
    // 「描画する」側へ倒す。判定はビュー毎 (SceneView / Game / プレビューで各々正しく落ちる)
    DirectX::XMFLOAT4X4 viewProjRaw;
    {
        const XMMATRIX xv = XMLoadFloat4x4(&view.view);
        const XMMATRIX xp = XMLoadFloat4x4(&view.proj);
        XMStoreFloat4x4(&viewProjRaw, XMMatrixMultiply(xv, xp));
    }
    const Frustum frustum = BuildFrustum(viewProjRaw);
    visScratch_.assign(pools_.size(), 1u);
    uint32_t total = 0;
    for (size_t pi = 0; pi < pools_.size(); ++pi) {
        const EmitterPool& pool = pools_[pi];
        // M61e: renderSkip = 凍結中 (owner 非アクティブ)。粒子は保持されたままだが描かない
        if (pool.renderSkip || pool.alive == 0) {
            visScratch_[pi] = 0;
            continue;
        }
        if (pool.boundsValid) {
            // M61g: ローカル空間プールのバウンズはローカル AABB — renderWorld の 8 頂点変換で
            // 保守的なワールド AABB にしてから既存の判定に掛ける (回転で膨らむが包含は保存)。
            // ワールド空間 (既定) はコピーを渡すだけで判定値は従来と同一
            XMFLOAT3 bmin = pool.boundsMin;
            XMFLOAT3 bmax = pool.boundsMax;
            if (pool.descCache.simulationSpace == 1) {
                TransformAabbToWorld(pool.renderWorld, pool.boundsMin, pool.boundsMax, bmin, bmax);
            }
            if (!ParticlePoolVisible(frustum, bmin, bmax,
                                     ParticleBillboardExpand(pool.descCache, pool.maxSize0),
                                     renderOffsetX)) {
                visScratch_[pi] = 0;
                continue;
            }
        }
        total += pool.alive;
    }
    if (total == 0) {
        return;
    }

    ID3D11Device* dev = device.Device();
    ID3D11DeviceContext* dc = device.Context();

    // インスタンスバッファを必要に応じて拡張
    if (instanceCapacity_ < total) {
        instanceCapacity_ = std::max(total, instanceCapacity_ * 2 + 1024);
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = instanceCapacity_ * sizeof(ParticleInstance);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = sizeof(ParticleInstance);
        if (FAILED(dev->CreateBuffer(&bd, nullptr, instanceBuffer_.ReleaseAndGetAddressOf()))) {
            return;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = DXGI_FORMAT_UNKNOWN;
        sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        sd.Buffer.NumElements = instanceCapacity_;
        if (FAILED(dev->CreateShaderResourceView(instanceBuffer_.Get(), &sd,
                                                 instanceSRV_.ReleaseAndGetAddressOf()))) {
            return;
        }
    }

    // インスタンスデータ充填 (エミッタ順。アルファは back-to-front ソート — 描画専用処理で
    // シミュレーション状態 (ハッシュ対象) には触れない)
    const XMFLOAT4X4& vm = view.view;
    struct DrawRange {
        uint32_t base;
        uint32_t count;
        int32_t blendMode;
        AssetID texture;
        int32_t flipTilesX;
        int32_t flipTilesY;
        float flipCycles;
        float softFade; // M42b: エミッタ毎の深度フェード距離
    };
    std::vector<DrawRange> ranges;

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(dc->Map(instanceBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return;
    }
    auto* out = static_cast<ParticleInstance*>(mapped.pData);
    uint32_t cursor = 0;
    for (size_t pi = 0; pi < pools_.size(); ++pi) {
        EmitterPool& pool = pools_[pi];
        if (!visScratch_[pi]) { // 凍結 or 空プール or フラスタム外 (上でまとめて判定済み)
            continue;
        }
        const ParticleEmitterComponent& d = pool.descCache;
        const uint32_t base = cursor;

        // M61g: ローカル空間プールは位置を renderWorld でワールドへ変換してから詰める
        // (renderOffsetX はその後に加算)。alpha ソートの viewZ も変換後の位置で計る。
        // ワールド空間 (既定) はベースポインタの差し替えだけ = 従来と同一の値・同一の演算列。
        // ビルボードサイズにはスケールを適用しない (v1 制限 — renderWorld は位置にだけ効く)
        const float* sx = pool.px.data();
        const float* sy = pool.py.data();
        const float* sz = pool.pz.data();
        if (d.simulationSpace == 1) {
            wxScratch_.resize(pool.alive);
            wyScratch_.resize(pool.alive);
            wzScratch_.resize(pool.alive);
            const XMFLOAT4X4& m = pool.renderWorld;
            for (uint32_t i = 0; i < pool.alive; ++i) {
                const float lx = pool.px[i], ly = pool.py[i], lz = pool.pz[i];
                wxScratch_[i] = lx * m._11 + ly * m._21 + lz * m._31 + m._41;
                wyScratch_[i] = lx * m._12 + ly * m._22 + lz * m._32 + m._42;
                wzScratch_[i] = lx * m._13 + ly * m._23 + lz * m._33 + m._43;
            }
            sx = wxScratch_.data();
            sy = wyScratch_.data();
            sz = wzScratch_.data();
        }

        orderScratch_.resize(pool.alive);
        for (uint32_t i = 0; i < pool.alive; ++i) {
            orderScratch_[i] = i;
        }
        if (d.blendMode == 1) {
            // back-to-front (明示キー: viewZ 降順 → index 昇順。spec 11.2 規則 7)
            std::sort(orderScratch_.begin(), orderScratch_.end(), [&](uint32_t a, uint32_t b) {
                const float za = sx[a] * vm._13 + sy[a] * vm._23 + sz[a] * vm._33;
                const float zb = sx[b] * vm._13 + sy[b] * vm._23 + sz[b] * vm._33;
                if (za != zb) {
                    return za > zb;
                }
                return a < b;
            });
        }

        for (uint32_t k = 0; k < pool.alive; ++k) {
            const uint32_t i = orderScratch_[k];
            float age = 1.0f - pool.life[i] * pool.invLife[i];
            age = std::clamp(age, 0.0f, 1.0f);
            ParticleInstance& inst = out[cursor++];
            inst.pos = { sx[i] + renderOffsetX, sy[i], sz[i] };
            // 多点グラデーション (中間キー未使用なら従来の 2 点線形と同値)
            inst.size = pool.size0[i] * EvalParticleSizeScale(d, age);
            inst.color = EvalParticleColor(d, age);
            inst.age = age;
        }
        ranges.push_back({ base, pool.alive, d.blendMode, d.texture,
                           std::max(1, d.flipTilesX), std::max(1, d.flipTilesY), d.flipCycles,
                           d.softFadeDistance });
    }
    dc->Unmap(instanceBuffer_.Get(), 0);

    // ---- 描画 ----
    dc->IASetInputLayout(nullptr);
    dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    dc->VSSetShader(prog->vs.Get(), nullptr, 0);
    dc->PSSetShader(prog->ps.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srv = instanceSRV_.Get();
    dc->VSSetShaderResources(0, 1, &srv);
    dc->OMSetDepthStencilState(depthNoWrite_.Get(), 0);
    ID3D11Buffer* cb = renderCB_.Get();
    dc->VSSetConstantBuffers(0, 1, &cb);
    dc->PSSetConstantBuffers(0, 1, &cb); // フリップブック分岐を PS でも参照
    ID3D11SamplerState* samp = sampler_.Get();
    dc->PSSetSamplers(0, 1, &samp);

    // フリップブックテクスチャの白フォールバック
    Texture* whiteTex = resources.textures.Get(resources.textures.White());
    ID3D11ShaderResourceView* whiteSrv = whiteTex ? whiteTex->srv.Get() : nullptr;

    using namespace DirectX;
    ParticleCB cbData = {};
    // viewProj はカリングで計算済みのものを転置して使う (二重計算を避ける)
    XMStoreFloat4x4(&cbData.viewProj, XMMatrixTranspose(XMLoadFloat4x4(&viewProjRaw)));
    cbData.camRight = { vm._11, vm._21, vm._31 };
    cbData.camUp = { vm._12, vm._22, vm._32 };
    // フォグ (M32c): RenderView が CollectEnvironment から埋めた値を粒子にも適用する
    cbData.fogMode = view.fogMode;
    cbData.cameraPos = view.cameraPos;
    cbData.fogDensity = view.fogDensity;
    cbData.fogColor = view.fogColor;
    cbData.fogStart = view.fogStart;
    cbData.fogEnd = view.fogEnd;
    cbData.nearZ = view.nearZ; // M42b: ソフトフェードの深度線形化用
    cbData.farZ = view.farZ;

    // M42b: シーン深度を t2 へ。depthSRV が無いビュー (AssetPreview 等) は softFade=0 に
    // 強制してフェードを無効化する (read-only DSV バインド中のみ SRV 読みが合法 — M42a)
    if (view.depthSRV != nullptr) {
        dc->PSSetShaderResources(2, 1, &view.depthSRV);
    }

    // M57e: フロクセルの積分結果を t3 へ。**適用しないと霧の中で粒子だけが浮く** —
    // 加算合成は背景の減衰を受けないので、周囲が霞むほど粒子だけがくっきり残る。
    // 判定は FroxelIsBound 1 本 (Deferred / Forward / スカイと共有) = 「サムネイル
    // (AssetPreviewCache の別 RenderSystem) だけがゴミを読む」を構造的に潰してある
    const bool froxelBound = FroxelIsBound(view);
    cbData.froxelEnabled = froxelBound ? 1 : 0;
    cbData.froxelNearZ = view.froxelNearZ;
    cbData.froxelFarZ = view.froxelFarZ;
    cbData.froxelSlices = static_cast<float>(view.froxelSlices);
    cbData.froxelScreenSize[0] = static_cast<float>(view.width);
    cbData.froxelScreenSize[1] = static_cast<float>(view.height);
    {
        ID3D11ShaderResourceView* froxelSrv[1] = { froxelBound ? view.froxelSRV : nullptr };
        static_assert(froxel::kParticleSrvSlot == 3, "パーティクルのフロクセル SRV は t3");
        dc->PSSetShaderResources(froxel::kParticleSrvSlot, 1, froxelSrv);
    }

    bool anyDistortion = false; // M42d
    for (const DrawRange& range : ranges) {
        if (range.blendMode == 2) { // M42d: 歪みは後段の専用パスで描く
            anyDistortion = true;
            continue;
        }
        cbData.baseIndex = range.base;
        // M57追補: 規則を ParticleCurves.h へ寄せた (GPU バックエンドと共有。値は同一)
        cbData.blendAdditive = ParticleBlendIsAdditive(range.blendMode) ? 1 : 0;
        // テクスチャ解決 (空なら procedural 円へフォールバック)
        ID3D11ShaderResourceView* texSrv = nullptr;
        if (range.texture.value != 0) {
            if (Texture* t = resources.textures.Get(range.texture)) {
                texSrv = t->srv.Get();
            }
        }
        cbData.useTexture = texSrv ? 1u : 0u;
        cbData.flipTilesX = range.flipTilesX;
        cbData.flipTilesY = range.flipTilesY;
        cbData.flipCycles = range.flipCycles;
        cbData.softFade = (view.depthSRV != nullptr) ? range.softFade : 0.0f; // M42b
        D3D11_MAPPED_SUBRESOURCE cbMapped = {};
        if (SUCCEEDED(dc->Map(renderCB_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &cbMapped))) {
            memcpy(cbMapped.pData, &cbData, sizeof(cbData));
            dc->Unmap(renderCB_.Get(), 0);
        }
        ID3D11ShaderResourceView* psSrv = texSrv ? texSrv : whiteSrv;
        dc->PSSetShaderResources(1, 1, &psSrv);
        dc->OMSetBlendState(range.blendMode == 1 ? blendAlpha_.Get() : blendAdditive_.Get(),
                            nullptr, 0xFFFFFFFFu);
        dc->DrawInstanced(4, range.count, 0, 0);
    }

    // ---- M42d: 歪みパス (blendMode=2) — 歪みバッファへ加算描画 ----
    // RenderSystem が「歪みエミッタあり && HDR 経路」のときだけ distortionRTV を配線する。
    // 深度テストは read-only DSV で継続 = 遮蔽された粒子は歪まない
    if (anyDistortion && view.distortionRTV != nullptr) {
        ShaderProgram* dprog = shaders.Get(distortShaderId_);
        if (dprog && dprog->valid) {
            dc->OMSetRenderTargets(1, &view.distortionRTV,
                                   view.dsvReadOnly ? view.dsvReadOnly : nullptr);
            dc->VSSetShader(dprog->vs.Get(), nullptr, 0);
            dc->PSSetShader(dprog->ps.Get(), nullptr, 0);
            dc->OMSetBlendState(blendAdditive_.Get(), nullptr, 0xFFFFFFFFu); // 加算で累積
            for (const DrawRange& range : ranges) {
                if (range.blendMode != 2) {
                    continue;
                }
                cbData.baseIndex = range.base;
                cbData.softFade = 0.0f;
                D3D11_MAPPED_SUBRESOURCE cbMapped = {};
                if (SUCCEEDED(
                        dc->Map(renderCB_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &cbMapped))) {
                    memcpy(cbMapped.pData, &cbData, sizeof(cbData));
                    dc->Unmap(renderCB_.Get(), 0);
                }
                dc->DrawInstanced(4, range.count, 0, 0);
            }
            // シーン RT へ戻す (M42a: パーティクル区間は read-only DSV)
            dc->OMSetRenderTargets(1, &view.rtv,
                                   view.dsvReadOnly ? view.dsvReadOnly : view.dsv);
        }
    }

    // SRV を外す (次フレームの Map と競合させない。t2=深度は RTV/DSV 戻し前の解除 — M42a 流儀)。
    // ★t3 (M57e のフロクセル) も必ず外す — 残すと次フレームの積分パスが同じテクスチャを
    //   UAV に取った瞬間に D3D が片方を黙って外す
    ID3D11ShaderResourceView* nullSrv = nullptr;
    dc->VSSetShaderResources(0, 1, &nullSrv);
    dc->PSSetShaderResources(1, 1, &nullSrv);
    dc->PSSetShaderResources(2, 1, &nullSrv);
    dc->PSSetShaderResources(3, 1, &nullSrv);
    dc->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
    dc->OMSetDepthStencilState(nullptr, 0);
}

} // namespace mye
