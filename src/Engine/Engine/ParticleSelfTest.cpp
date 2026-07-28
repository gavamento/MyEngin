#include "Engine/Engine/ParticleSelfTest.h"

#include <cmath>
#include <cstdint>

#include "Engine/Core/Components.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/EffectSystem.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/Particles/CpuParticleBackend.h"
#include "Engine/Engine/Particles/ParticleCurves.h"
#include "Engine/Engine/Scene.h"

using namespace DirectX;

namespace mye {
namespace {

constexpr float kDt = 1.0f / 60.0f;

void SetWorldPos(World& world, EntityID e, float x, float y, float z)
{
    auto* wm = world.GetComponent<WorldMatrixComponent>(e);
    wm->value._41 = x;
    wm->value._42 = y;
    wm->value._43 = z;
}

bool NearF(float a, float b) { return std::fabs(a - b) < 1e-5f; }

// pool の sim 状態 (pos/vel/life) の FNV-1a ハッシュ (SIMD/scalar 比較・決定論比較用)
uint64_t HashSimState(const CpuParticleBackend& cpu)
{
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) {
            h ^= b[i];
            h *= 1099511628211ull;
        }
    };
    for (const CpuParticleBackend::EmitterPool& pool : cpu.Pools()) {
        const uint32_t n = pool.alive;
        mix(&n, sizeof(n));
        if (n == 0) {
            continue;
        }
        mix(pool.px.data(), n * sizeof(float));
        mix(pool.py.data(), n * sizeof(float));
        mix(pool.pz.data(), n * sizeof(float));
        mix(pool.vx.data(), n * sizeof(float));
        mix(pool.vy.data(), n * sizeof(float));
        mix(pool.vz.data(), n * sizeof(float));
        mix(pool.life.data(), n * sizeof(float));
    }
    return h;
}

} // namespace

bool RunParticleSelfTest()
{
    MYE_LOG_INFO("==== Particle self test ====");
    RegisterBuiltinComponents();
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    // ---- (A) 放出計画 (純関数、World 不要) ----
    {
        // 既定 (rate 連続、duration=0): 毎 tick 放出 + age 前進
        ParticleEmitterComponent d;
        d.rate = 120.0f;
        int32_t age = 0;
        float acc = 0.0f;
        const int e0 = PlanParticleEmission(d, age, acc, kDt);
        check(e0 >= 1 && age == 1, "plan: continuous rate emits and advances age");

        // burst: ウィンドウ先頭で burstCount、以降は再発火しない (非ループ)
        ParticleEmitterComponent b;
        b.rate = 0.0f;
        b.burstCount = 100;
        b.durationTicks = 0;
        int32_t bage = 0;
        float bacc = 0.0f;
        check(PlanParticleEmission(b, bage, bacc, kDt) == 100, "plan: burst fires burstCount at start");
        check(PlanParticleEmission(b, bage, bacc, kDt) == 0, "plan: burst does not refire (no loop)");

        // duration + 非ループ: ウィンドウ内のみ放出、以降停止
        ParticleEmitterComponent du;
        du.rate = 600.0f;
        du.durationTicks = 3;
        du.looping = 0;
        int32_t dage = 0;
        float dacc = 0.0f;
        int inWindow = 0, afterWindow = 0;
        for (int t = 0; t < 6; ++t) {
            const int e = PlanParticleEmission(du, dage, dacc, kDt);
            (t < 3 ? inWindow : afterWindow) += e;
        }
        check(inWindow > 0 && afterWindow == 0, "plan: duration window stops emission (no loop)");

        // duration + ループ: ウィンドウ毎に burst 再発火 (3 ウィンドウ = 30)
        ParticleEmitterComponent lo;
        lo.rate = 0.0f;
        lo.durationTicks = 3;
        lo.looping = 1;
        lo.burstCount = 10;
        int32_t lage = 0;
        float lacc = 0.0f;
        int total = 0;
        for (int t = 0; t < 7; ++t) {
            total += PlanParticleEmission(lo, lage, lacc, kDt);
        }
        check(total == 30, "plan: looping refires burst each window (3 -> 30)");

        // playing=0: 連続放出停止 + age/accum 凍結。ただし pendingBurst は通す
        ParticleEmitterComponent pa;
        pa.rate = 600.0f;
        pa.playing = 0;
        pa.pendingBurst = 5;
        int32_t page = 7;
        float pacc = 0.9f;
        const int pe = PlanParticleEmission(pa, page, pacc, kDt);
        check(pe == 5 && page == 7 && NearF(pacc, 0.9f),
              "plan: playing=0 freezes age/accum, pendingBurst still fires");
    }

    // ---- (B) グラデーション評価 ----
    {
        ParticleEmitterComponent d;
        d.colorBegin = { 1, 0, 0, 1 };
        d.colorEnd = { 0, 0, 1, 0 };
        const XMFLOAT4 c = EvalParticleColor(d, 0.5f); // 中間キー無し = 2 点線形
        check(NearF(c.x, 0.5f) && NearF(c.y, 0.0f) && NearF(c.z, 0.5f) && NearF(c.w, 0.5f),
              "gradient: no mid key equals begin->end lerp");

        d.colorMid1 = { 0, 1, 0, 1 };
        d.colorMidT1 = 0.5f;
        const XMFLOAT4 cm = EvalParticleColor(d, 0.5f); // 中間キーにちょうど到達
        check(NearF(cm.x, 0.0f) && NearF(cm.y, 1.0f) && NearF(cm.z, 0.0f),
              "gradient: mid key hit exactly at its T");

        ParticleEmitterComponent s;
        s.sizeEndScale = 0.0f;
        s.sizeMidT = 0.0f;
        check(NearF(EvalParticleSizeScale(s, 0.0f), 1.0f)
                  && NearF(EvalParticleSizeScale(s, 1.0f), 0.0f),
              "gradient: size no mid equals 1->end lerp");
        s.sizeMidScale = 2.0f;
        s.sizeMidT = 0.5f;
        check(NearF(EvalParticleSizeScale(s, 0.5f), 2.0f), "gradient: size mid key hit exactly");
    }

    // ---- (B2) ソフトパーティクルのフェード係数 (M42b、HLSL と同一式のミラー) ----
    {
        // fadeDist=0 (既定) は無効 = 常に 1.0 (従来とビット同一)
        check(SoftFadeFactor(5.0f, 10.0f, 0.0f) == 1.0f, "softfade: disabled returns 1");
        // シーンが粒子よりちょうど fadeDist 奥 -> 1.0 (フェード完了)
        check(NearF(SoftFadeFactor(12.0f, 10.0f, 2.0f), 1.0f), "softfade: full at fade distance");
        // 中間 (奥行差 = fadeDist/2) -> 0.5
        check(NearF(SoftFadeFactor(11.0f, 10.0f, 2.0f), 0.5f), "softfade: half at mid distance");
        // 粒子がシーンより奥 (差が負) -> 0 (完全に消える)
        check(SoftFadeFactor(8.0f, 10.0f, 2.0f) == 0.0f, "softfade: behind scene clamps to 0");
        // 深度線形化: d=0 -> near, d=1 -> far (float で正確に表せる値を使う —
        // near=0.1/far=1000 だと far-(far-near) の桁落ちで ~0.02% ずれて NearF を外れる)
        check(NearF(LinearizeParticleDepth(0.0f, 1.0f, 100.0f), 1.0f)
                  && NearF(LinearizeParticleDepth(1.0f, 1.0f, 100.0f), 100.0f),
              "softfade: depth linearization endpoints");
    }

    // ---- (C) SIMD と スカラーの Simulate がビット一致 (120 tick) ----
    {
        Scene s;
        GameObject go = s.CreateGameObjectTracked("Emitter");
        auto* em = go.AddComponent<ParticleEmitterComponent>();
        em->rate = 300.0f;
        em->seed = 999u;
        em->shape = 2;
        em->turbulence = 0.7f;
        em->gravity = { 0.0f, 2.0f, 0.0f };
        s.GetWorld().ApplyStructuralChanges();
        World& w = s.GetWorld();
        SetWorldPos(w, go.Id(), 1.0f, 2.0f, 3.0f);

        CpuParticleBackend simd, scalar;
        simd.SetSimdEnabled(true);
        scalar.SetSimdEnabled(false);
        bool matched = true;
        for (int t = 0; t < 120 && matched; ++t) {
            simd.Update(w, kDt);
            scalar.Update(w, kDt);
            matched = (HashSimState(simd) == HashSimState(scalar));
        }
        check(matched, "SIMD and scalar simulate are bit-identical (120 ticks)");
        check(!simd.Pools().empty() && simd.Pools()[0].alive > 0, "emitter produced live particles");
    }

    // ---- (D) 同一エミッタ 2 個の per-tick sim ハッシュ一致 (burst+duration+loop の決定論) ----
    {
        auto build = [](Scene& s) {
            GameObject go = s.CreateGameObjectTracked("E");
            auto* em = go.AddComponent<ParticleEmitterComponent>();
            em->rate = 200.0f;
            em->seed = 7u;
            em->durationTicks = 30;
            em->looping = 1;
            em->burstCount = 40;
            s.GetWorld().ApplyStructuralChanges();
            return go.Id();
        };
        Scene sa, sb;
        const EntityID ea = build(sa);
        const EntityID eb = build(sb);
        SetWorldPos(sa.GetWorld(), ea, 0.0f, 0.0f, 0.0f);
        SetWorldPos(sb.GetWorld(), eb, 0.0f, 0.0f, 0.0f);
        CpuParticleBackend a, b;
        bool deterministic = true;
        for (int t = 0; t < 120 && deterministic; ++t) {
            a.Update(sa.GetWorld(), kDt);
            b.Update(sb.GetWorld(), kDt);
            deterministic = (HashSimState(a) == HashSimState(b));
        }
        check(deterministic, "two identical emitters stay bit-identical for 120 ticks");
    }

    // ---- (E) EffectSystem: duration→停止 / linger→自動破棄 (M32e) ----
    {
        Scene s;
        GameObject root = s.CreateGameObjectTracked("FX");
        auto* fx = root.AddComponent<EffectComponent>();
        fx->durationTicks = 5;
        fx->lingerTicks = 3;
        fx->autoDestroy = 1;
        fx->looping = 0;
        GameObject child = s.CreateGameObjectTracked("Emitter");
        child.AddComponent<ParticleEmitterComponent>();
        World& w = s.GetWorld();
        w.SetParent(child.Id(), root.Id());
        w.ApplyStructuralChanges();

        EffectSystem fxsys;
        for (int t = 0; t < 5; ++t) { // duration=5 で子エミッタ停止
            fxsys.Update(w);
            w.ApplyStructuralChanges();
        }
        auto* em = w.GetComponent<ParticleEmitterComponent>(child.Id());
        check(em && em->playing == 0, "effect: child emitter stops at duration");
        check(w.IsAlive(root.Id()), "effect: root alive during linger");

        for (int t = 0; t < 3; ++t) { // linger=3 で duration+linger=8 到達 → 破棄
            fxsys.Update(w);
            w.ApplyStructuralChanges();
        }
        check(!w.IsAlive(root.Id()), "effect: root auto-destroyed after linger");
        check(!w.IsAlive(child.Id()), "effect: child destroyed with root (subtree)");
    }

    // ---- (F) EffectSystem: looping で巻き戻し + 破棄しない ----
    {
        Scene s;
        GameObject root = s.CreateGameObjectTracked("FXLoop");
        auto* fx = root.AddComponent<EffectComponent>();
        fx->durationTicks = 4;
        fx->looping = 1;
        fx->autoDestroy = 1;
        GameObject child = s.CreateGameObjectTracked("Emitter");
        child.AddComponent<ParticleEmitterComponent>();
        World& w = s.GetWorld();
        w.SetParent(child.Id(), root.Id());
        w.ApplyStructuralChanges();

        EffectSystem fxsys;
        for (int t = 0; t < 4; ++t) {
            fxsys.Update(w);
            w.ApplyStructuralChanges();
        }
        auto* efx = w.GetComponent<EffectComponent>(root.Id());
        auto* em = w.GetComponent<ParticleEmitterComponent>(child.Id());
        check(efx && efx->elapsedTicks == 0, "effect: looping rewinds elapsed");
        check(em && em->playing == 1, "effect: looping keeps child emitting");
        check(w.IsAlive(root.Id()), "effect: looping never auto-destroys");
    }

    // ---- (G) EffectSystem::RestartEffect (M32f ABI RestartEffect の共有ロジック) ----
    {
        Scene s;
        GameObject root = s.CreateGameObjectTracked("FXRestart");
        auto* fx = root.AddComponent<EffectComponent>();
        fx->durationTicks = 3;
        fx->looping = 0;
        fx->autoDestroy = 0;
        GameObject child = s.CreateGameObjectTracked("Emitter");
        child.AddComponent<ParticleEmitterComponent>();
        World& w = s.GetWorld();
        w.SetParent(child.Id(), root.Id());
        w.ApplyStructuralChanges();

        EffectSystem fxsys;
        for (int t = 0; t < 3; ++t) { // 停止まで進める
            fxsys.Update(w);
            w.ApplyStructuralChanges();
        }
        check(w.GetComponent<ParticleEmitterComponent>(child.Id())->playing == 0,
              "effect: child stopped before restart");

        EffectSystem::RestartEffect(w, root.Id());
        auto* efx = w.GetComponent<EffectComponent>(root.Id());
        auto* em = w.GetComponent<ParticleEmitterComponent>(child.Id());
        check(efx->elapsedTicks == 0 && efx->playing == 1 && em->playing == 1,
              "effect: RestartEffect rewinds + re-enables child emission");
    }

    if (failCount == 0) {
        MYE_LOG_INFO("==== Particle self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== Particle self test: %d FAILURE(S) ====", failCount);
    return false;
}

} // namespace mye
