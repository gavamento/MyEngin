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

constexpr float kPi = 3.14159265358979323846f;

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
    // 現存エミッタを index 昇順で収集 (決定論)
    struct Found {
        EntityID id;
    };
    std::vector<EntityID> current;
    const ComponentTypeId req[] = { ParticleEmitterComponent::sTypeId,
                                    WorldMatrixComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            if (!IsEntityActive(world, arch.EntityAt(row))) {
                continue; // 無効エミッタはプールを持たない (M10)
            }
            current.push_back(arch.EntityAt(row));
        }
    });
    std::sort(current.begin(), current.end(),
              [](EntityID a, EntityID b) { return a.index < b.index; });

    // 消えたエミッタのプールを除去
    std::erase_if(pools_, [&](const EmitterPool& p) {
        return std::find(current.begin(), current.end(), p.owner) == current.end();
    });
    // 新規エミッタのプールを作成 (シードはコンポーネント指定 — spec 7.3)
    for (EntityID e : current) {
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
                                       const XMFLOAT3& origin, float dt)
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

    for (int n = 0; n < emit; ++n) {
        const uint32_t i = pool.alive++;
        // 乱数の消費順は固定 (決定論): 方向 → 位置 → 速度 → 寿命 → サイズ
        float dirX = 0.0f, dirY = 1.0f, dirZ = 0.0f;
        float posX = origin.x, posY = origin.y, posZ = origin.z;
        switch (desc.shape) {
        case 1: { // sphere (表面から外向き)
            const float z = 1.0f - 2.0f * pool.rng.NextFloat01();
            const float phi = pool.rng.NextFloat01() * 2.0f * kPi;
            const float s = sqrtf(std::max(0.0f, 1.0f - z * z));
            dirX = s * cosf(phi);
            dirY = z;
            dirZ = s * sinf(phi);
            posX += dirX * desc.shapeRadius;
            posY += dirY * desc.shapeRadius;
            posZ += dirZ * desc.shapeRadius;
            break;
        }
        case 2: { // cone (+Y 中心)
            const float cosMax = cosf(desc.coneAngleDeg * kPi / 180.0f);
            const float cosT = 1.0f - pool.rng.NextFloat01() * (1.0f - cosMax);
            const float sinT = sqrtf(std::max(0.0f, 1.0f - cosT * cosT));
            const float phi = pool.rng.NextFloat01() * 2.0f * kPi;
            dirX = sinT * cosf(phi);
            dirY = cosT;
            dirZ = sinT * sinf(phi);
            break;
        }
        case 3: { // box (内部からランダム、上向き)
            posX += (pool.rng.NextFloat01() * 2.0f - 1.0f) * desc.boxExtents.x;
            posY += (pool.rng.NextFloat01() * 2.0f - 1.0f) * desc.boxExtents.y;
            posZ += (pool.rng.NextFloat01() * 2.0f - 1.0f) * desc.boxExtents.z;
            break;
        }
        default: // point
            break;
        }
        const float speed = pool.rng.Range(desc.speedMin, desc.speedMax);
        const float lifetime = std::max(0.01f, pool.rng.Range(desc.lifetimeMin, desc.lifetimeMax));
        const float size = pool.rng.Range(desc.sizeMin, desc.sizeMax);

        pool.px[i] = posX;
        pool.py[i] = posY;
        pool.pz[i] = posZ;
        pool.vx[i] = dirX * speed;
        pool.vy[i] = dirY * speed;
        pool.vz[i] = dirZ * speed;
        pool.life[i] = lifetime;
        pool.invLife[i] = 1.0f / lifetime;
        pool.size0[i] = size;
    }
}

void CpuParticleBackend::SimulateScalar(EmitterPool& pool, const XMFLOAT3& accel, float dt,
                                        uint32_t begin, uint32_t end)
{
    // SIMD 本体とレーン毎に同一の演算列 (mul→add の順) — 結果はビット一致する
    const float turb = turb_;
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
    const XMFLOAT3 accel = { desc.gravity.x + desc.wind.x, desc.gravity.y + desc.wind.y,
                             desc.gravity.z + desc.wind.z };
    turb_ = desc.turbulence;

    if (!simd_ || pool.alive < 8) {
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
        pool.descCache = *desc; // 描画時に World を引かないためのコピー
        const XMFLOAT3 origin = { wm->value._41, wm->value._42, wm->value._43 };
        EmitParticles(pool, *desc, origin, dt);
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
        if (pool.alive == 0
            || (pool.boundsValid
                && !ParticlePoolVisible(frustum, pool.boundsMin, pool.boundsMax,
                                        ParticleBillboardExpand(pool.descCache, pool.maxSize0),
                                        renderOffsetX))) {
            visScratch_[pi] = 0;
            continue;
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
        if (!visScratch_[pi]) { // 空プール or フラスタム外 (上でまとめて判定済み)
            continue;
        }
        const ParticleEmitterComponent& d = pool.descCache;
        const uint32_t base = cursor;

        orderScratch_.resize(pool.alive);
        for (uint32_t i = 0; i < pool.alive; ++i) {
            orderScratch_[i] = i;
        }
        if (d.blendMode == 1) {
            // back-to-front (明示キー: viewZ 降順 → index 昇順。spec 11.2 規則 7)
            std::sort(orderScratch_.begin(), orderScratch_.end(), [&](uint32_t a, uint32_t b) {
                const float za = pool.px[a] * vm._13 + pool.py[a] * vm._23 + pool.pz[a] * vm._33;
                const float zb = pool.px[b] * vm._13 + pool.py[b] * vm._23 + pool.pz[b] * vm._33;
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
            inst.pos = { pool.px[i] + renderOffsetX, pool.py[i], pool.pz[i] };
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
        cbData.blendAdditive = (range.blendMode == 1) ? 0 : 1; // blendMode: 0=additive 1=alpha
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
