#include "Engine/Engine/ParticleSelfTest.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "Engine/Core/Components.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/EffectSystem.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/Particles/CpuParticleBackend.h"
#include "Engine/Engine/Particles/GpuAliveEstimator.h"
#include "Engine/Engine/Particles/ParticleCurves.h"
#include "Engine/Engine/Replay/SimSnapshot.h"
#include "Engine/Engine/Replay/WorldHasher.h"
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

    // ---- (B3) GPU 深度衝突の座標変換/反射 (M42e、particle_sim.cs.hlsl のミラー) ----
    {
        float u = -1.0f, v = -1.0f;
        // クリップ原点 (画面中央) -> uv (0.5, 0.5)
        check(ParticleClipToUv(0.0f, 0.0f, 1.0f, u, v) && NearF(u, 0.5f) && NearF(v, 0.5f),
              "collision: clip origin maps to uv center");
        // クリップ右上 (+w, +w) -> uv (1, 0) (y は上下反転)
        check(ParticleClipToUv(2.0f, 2.0f, 2.0f, u, v) && NearF(u, 1.0f) && NearF(v, 0.0f),
              "collision: clip corner maps with y flip");
        // 背面 (w<=0) は判定しない
        check(!ParticleClipToUv(0.0f, 0.0f, -1.0f, u, v), "collision: behind camera rejected");

        // 真下落下 (0,-4,0) を y-up 法線で反射、反発 0.5 -> (0, +2, 0)
        const XMFLOAT3 r =
            ReflectWithRestitution({ 0.0f, -4.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, 0.5f);
        check(NearF(r.x, 0.0f) && NearF(r.y, 2.0f) && NearF(r.z, 0.0f),
              "collision: straight-down reflect with restitution");
        // 斜め入射 (1,-1,0)、反発 1 -> (1, +1, 0) (接線成分保存)
        const XMFLOAT3 r2 =
            ReflectWithRestitution({ 1.0f, -1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, 1.0f);
        check(NearF(r2.x, 1.0f) && NearF(r2.y, 1.0f) && NearF(r2.z, 0.0f),
              "collision: oblique reflect preserves tangent");
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
        // 非ループ Animator の子: ループ 1 周中に自然終了した状態を作っておく
        // (AnimationSystem は回さないので終了状態は手で再現する)
        GameObject anim = s.CreateGameObjectTracked("Anim");
        {
            auto* an = anim.AddComponent<AnimatorComponent>();
            an->loop = 0;
            an->playing = 0; // = Animation.cpp AdvanceTime が末尾で止めた状態
            an->timeTicks = 7;
        }
        World& w = s.GetWorld();
        w.SetParent(child.Id(), root.Id());
        w.SetParent(anim.Id(), root.Id());
        w.ApplyStructuralChanges();

        EffectSystem fxsys;
        for (int t = 0; t < 4; ++t) {
            fxsys.Update(w);
            w.ApplyStructuralChanges();
        }
        auto* efx = w.GetComponent<EffectComponent>(root.Id());
        auto* em = w.GetComponent<ParticleEmitterComponent>(child.Id());
        auto* an = w.GetComponent<AnimatorComponent>(anim.Id());
        check(efx && efx->elapsedTicks == 0, "effect: looping rewinds elapsed");
        check(em && em->playing == 1, "effect: looping keeps child emitting");
        check(an && an->timeTicks == 0 && an->playing == 1,
              "effect: looping restarts finished non-loop animator");
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

    // ---- (H) RestartEffect: 自然終了した非ループ Animator を再開する ----
    // timeTicks の巻き戻しだけでは AnimationSystem が !playing で continue し続けて
    // 二度と動かない (エフェクトのループ再生で片道アニメが 1 周目しか出ないバグの再現)
    {
        Scene s;
        GameObject root = s.CreateGameObjectTracked("FXAnim");
        auto* fx = root.AddComponent<EffectComponent>();
        fx->durationTicks = 3;
        fx->looping = 0;
        fx->autoDestroy = 0;
        GameObject anim = s.CreateGameObjectTracked("Anim");
        {
            auto* an = anim.AddComponent<AnimatorComponent>();
            an->loop = 0;
            an->playing = 0; // = Animation.cpp AdvanceTime が末尾で止めた状態
            an->timeTicks = 7;
        }
        World& w = s.GetWorld();
        w.SetParent(anim.Id(), root.Id());
        w.ApplyStructuralChanges();

        EffectSystem::RestartEffect(w, root.Id());
        auto* an = w.GetComponent<AnimatorComponent>(anim.Id());
        check(an && an->timeTicks == 0 && an->playing == 1,
              "effect: RestartEffect restarts finished non-loop animator");
    }

    // ---- (I) GpuAliveEstimator: GPU 生存数の CPU 側推定 (readback なし) ----
    {
        // 定常状態: 毎 tick 8 粒 × 寿命 30 tick 分。放出 tick に sim も走り life が
        // dt 減るため、1 粒が描画される tick 数は 30 ではなく 29 (放出 tick 分を消費)
        GpuAliveEstimator est;
        for (int t = 0; t < 120; ++t) {
            for (int n = 0; n < 8; ++n) {
                est.OnEmit(0.5f, kDt); // 0.5s = 30 tick
            }
            est.EndTick();
        }
        check(est.Alive(100000) == 8 * 29, "alive estimator converges to rate*(life-1)");

        // 放出停止後: 寿命分の tick で 0 に戻る (容量合計を返す旧実装との違いの核)
        for (int t = 0; t < 30; ++t) {
            est.EndTick();
        }
        check(est.Alive(100000) == 0, "alive estimator drains to zero after emission stops");

        // 容量飽和: GPU 側は dead list 枯渇で放出が落ちるため、推定は capacity で頭打ち
        GpuAliveEstimator sat;
        for (int t = 0; t < 10; ++t) {
            for (int n = 0; n < 100; ++n) {
                sat.OnEmit(10.0f, kDt);
            }
            sat.EndTick();
        }
        check(sat.Alive(256) == 256, "alive estimator clamps at capacity");

        // 寿命 <= dt の粒は放出と同 tick で死亡 (aliveOut に入らず描画もされない)
        GpuAliveEstimator tiny;
        tiny.OnEmit(0.01f, kDt);
        tiny.EndTick();
        check(tiny.Alive(100) == 0, "life <= dt dies in the emit tick");
    }

    // ---- (J) 描画専用バウンズ + フラスタムカリング判定 ----
    // シム更新はカリングしない (プールはハッシュ対象) — テストするのは
    // 「Update が撮る AABB が全生存粒子を包む」ことと、純関数の可視判定のみ
    {
        Scene s;
        GameObject go = s.CreateGameObjectTracked("Emitter");
        auto* em = go.AddComponent<ParticleEmitterComponent>();
        em->rate = 300.0f;
        em->seed = 42u;
        em->shape = 1; // sphere (全方向に散る = AABB が退化しない)
        s.GetWorld().ApplyStructuralChanges();
        World& w = s.GetWorld();
        SetWorldPos(w, go.Id(), 5.0f, -2.0f, 8.0f);

        CpuParticleBackend cpu;
        for (int t = 0; t < 30; ++t) {
            cpu.Update(w, kDt);
        }
        {
            const CpuParticleBackend::EmitterPool& pool = cpu.Pools()[0];
            check(pool.boundsValid && pool.alive > 0, "bounds: valid after update with live particles");
            bool inside = true;
            float maxSize = 0.0f;
            for (uint32_t i = 0; i < pool.alive; ++i) {
                inside = inside && pool.px[i] >= pool.boundsMin.x && pool.px[i] <= pool.boundsMax.x
                         && pool.py[i] >= pool.boundsMin.y && pool.py[i] <= pool.boundsMax.y
                         && pool.pz[i] >= pool.boundsMin.z && pool.pz[i] <= pool.boundsMax.z;
                maxSize = std::max(maxSize, pool.size0[i]);
            }
            check(inside, "bounds: AABB contains every live particle");
            check(NearF(pool.maxSize0, maxSize), "bounds: max size tracks live particles");
        }

        // 放出停止 + 寿命切れで空になったら invalid (= カリングしない側へ倒れる)
        w.GetComponent<ParticleEmitterComponent>(go.Id())->playing = 0;
        for (int t = 0; t < 240; ++t) { // lifetimeMax 2.2s = 132 tick を掃き切る
            cpu.Update(w, kDt);
        }
        check(cpu.Pools()[0].alive == 0 && !cpu.Pools()[0].boundsValid,
              "bounds: empty pool invalidates bounds");

        // フラスタム判定 (純関数): view=単位行列 + 90° 透視 → z=10 で半幅 10 の錐台
        XMFLOAT4X4 vp;
        XMStoreFloat4x4(&vp, XMMatrixPerspectiveFovLH(XM_PIDIV2, 1.0f, 0.1f, 100.0f));
        const Frustum fr = BuildFrustum(vp);
        check(ParticlePoolVisible(fr, { -1, -1, 9 }, { 1, 1, 11 }, 0.5f, 0.0f),
              "cull: box ahead of camera is visible");
        check(!ParticlePoolVisible(fr, { 50, -1, 9 }, { 52, 1, 11 }, 0.5f, 0.0f),
              "cull: box far off to the side is culled");
        check(ParticlePoolVisible(fr, { 50, -1, 9 }, { 52, 1, 11 }, 0.5f, -50.0f),
              "cull: render offset shifts the box back into view");
        check(!ParticlePoolVisible(fr, { -1, -1, -11 }, { 1, 1, -9 }, 0.5f, 0.0f),
              "cull: box behind camera is culled");

        // 拡張量: 半幅 × サイズカーブ最大倍率 × 1.5 (ビルボード対角 √2 の保守値)
        ParticleEmitterComponent bd;
        bd.sizeMidScale = 1.0f;
        bd.sizeEndScale = 3.0f;
        check(NearF(ParticleBillboardExpand(bd, 2.0f), 2.0f * 3.0f * 1.5f),
              "cull: billboard expand uses max curve scale");
    }

    // ---- (K) GPU 空 Dispatch 回避の遅延判定 (純関数) ----
    {
        constexpr int32_t kGrace = 8;
        int32_t idle = 0;
        // 推定生存 >0 / 放出あり の間はスキップしない (+ 計数リセット)
        check(!StepGpuIdleSkip(0, 5, kGrace, idle) && idle == 0, "idle: alive estimate keeps gpu on");
        check(!StepGpuIdleSkip(3, 0, kGrace, idle) && idle == 0, "idle: emission keeps gpu on");
        // 空 tick が grace 回続くまでは Dispatch を続ける (GPU 側の残存粒子を死なせ切る猶予)
        bool skippedEarly = false;
        for (int t = 0; t < kGrace; ++t) {
            skippedEarly = skippedEarly || StepGpuIdleSkip(0, 0, kGrace, idle);
        }
        check(!skippedEarly, "idle: grace ticks keep dispatching");
        check(StepGpuIdleSkip(0, 0, kGrace, idle), "idle: skips after grace");
        // 放出再開で即復帰
        check(!StepGpuIdleSkip(1, 0, kGrace, idle) && idle == 0, "idle: wakes on new emission");
    }

    // ---- (L) M61a: 原点履歴とスナップショット往復 ----
    // (L1) Update が prevOrigin/prevOriginValid/prewarmed を毎 tick 進めること
    {
        // プール生成は Update 内 (SyncEmitters) なので「生成直後・初回 Update 前」は外から
        // 観測できない — 生成コード (EmitterPool pool; pool.owner=e; ...) は既定構築の値を
        // そのまま使うため、既定構築のプールで「誕生時は 0」を代弁させる
        CpuParticleBackend::EmitterPool fresh;
        check(fresh.prewarmed == 0 && fresh.prevOriginValid == 0,
              "history: a fresh pool starts with prewarmed=0 and no origin history");

        Scene s;
        GameObject go = s.CreateGameObjectTracked("Emitter");
        go.AddComponent<ParticleEmitterComponent>();
        s.GetWorld().ApplyStructuralChanges();
        World& w = s.GetWorld();
        SetWorldPos(w, go.Id(), 1.0f, 2.0f, 3.0f);

        CpuParticleBackend cpu;
        cpu.Update(w, kDt);
        {
            const CpuParticleBackend::EmitterPool& pool = cpu.Pools()[0];
            check(pool.prevOrigin.x == 1.0f && pool.prevOrigin.y == 2.0f
                      && pool.prevOrigin.z == 3.0f && pool.prevOriginValid == 1
                      && pool.prewarmed == 1,
                  "history: first update records the origin and raises both flags");
        }
        SetWorldPos(w, go.Id(), 4.0f, 5.0f, 6.0f);
        cpu.Update(w, kDt);
        check(cpu.Pools()[0].prevOrigin.x == 4.0f && cpu.Pools()[0].prevOrigin.y == 5.0f
                  && cpu.Pools()[0].prevOrigin.z == 6.0f,
              "history: origin history follows the emitter every tick");
    }

    // (L2) スナップショット往復のビット保存 + ハッシュ感度 (XpbdSelfTest のプローブ池パターン)
    {
        Scene s;
        GameObject owner = s.CreateGameObjectTracked("PoolOwner");
        World& w = s.GetWorld();
        w.ApplyStructuralChanges();

        // 手で組んだプローブ池。M61a の新フィールドは全て非デフォルト値にする —
        // デフォルトのままだと Write/Read の書き忘れがあっても一致してしまい検出できない
        CpuParticleBackend backend;
        {
            CpuParticleBackend::EmitterPool probe;
            probe.owner = owner.Id();
            probe.alive = 2;
            probe.emitAccum = 0.625f;
            probe.ageTicks = 42;
            probe.rng.Seed(777u, 5u);
            probe.px = { 0.5f, 1.5f };
            probe.py = { 2.5f, 3.5f };
            probe.pz = { -0.25f, 0.25f };
            probe.vx = { 0.125f, -0.125f };
            probe.vy = { 4.0f, 5.0f };
            probe.vz = { -6.0f, 7.0f };
            probe.life = { 1.0f, 0.5f };
            probe.invLife = { 1.0f, 2.0f };
            probe.size0 = { 0.1f, 0.2f };
            probe.prevOrigin = { 1.25f, -2.5f, 3.75f };
            probe.prevOriginValid = 1;
            probe.prewarmed = 1;
            probe.descCache.velocityInheritance = 0.5f;
            probe.descCache.simulationSpace = 1;
            probe.descCache.prewarmTime = 0.75f;
            probe.descCache.subframeEmission = 1;
            probe.descCache.turbulenceMode = 1;
            probe.descCache.noiseFrequency = 2.0f;
            probe.descCache.noiseSpeed = 0.25f;
            probe.descCache.emitFrom = 2;
            backend.PoolsForSnapshot().push_back(std::move(probe));
        }

        SimSources src;
        src.particles = &backend;
        const uint64_t hashPool = HashWorld(w, src);

        SimRefs refs;
        refs.scene = &s;
        refs.particles = &backend;
        uint64_t tick = 60;
        refs.tickIndex = &tick;

        std::vector<std::byte> blob;
        check(CaptureSimSnapshot(refs, blob), "snapshot: capture succeeds with a probe pool");

        // 徹底的に変異させてから戻す。descCache はハッシュ非対象なので、ハッシュ復帰の
        // 確認だけでは往復被覆にならない — 復元後に直接ビット比較する
        {
            CpuParticleBackend::EmitterPool& pool = backend.PoolsForSnapshot()[0];
            pool.prevOrigin = { 9.0f, 9.0f, 9.0f };
            pool.prevOriginValid = 0;
            pool.prewarmed = 0;
            pool.descCache.velocityInheritance = 9.0f;
            pool.descCache.emitFrom = 0;
        }
        check(HashWorld(w, src) != hashPool, "snapshot: the pool really diverged before restore");

        check(RestoreSimSnapshot(refs, blob.data(), blob.size()), "snapshot: restore succeeds");
        check(HashWorld(w, src) == hashPool, "snapshot: restore returns the captured hash");
        const CpuParticleBackend::EmitterPool& rp = backend.Pools()[0];
        check(rp.prevOrigin.x == 1.25f && rp.prevOrigin.y == -2.5f && rp.prevOrigin.z == 3.75f
                  && rp.prevOriginValid == 1 && rp.prewarmed == 1,
              "snapshot: M61a pool fields survive the round trip bit-exact");
        check(rp.descCache.velocityInheritance == 0.5f && rp.descCache.simulationSpace == 1
                  && rp.descCache.prewarmTime == 0.75f && rp.descCache.subframeEmission == 1
                  && rp.descCache.turbulenceMode == 1 && rp.descCache.noiseFrequency == 2.0f
                  && rp.descCache.noiseSpeed == 0.25f && rp.descCache.emitFrom == 2,
              "snapshot: descCache A-group fields survive the round trip");

        // ハッシュ被覆: 変異 → 割れる → 復元 → 戻る、を 1 か所で回す (XpbdSelfTest と同形)
        auto mutateCheck = [&](const char* what, auto&& mutate, auto&& restore) {
            mutate();
            const bool moved = HashWorld(w, src) != hashPool;
            restore();
            const bool restored = HashWorld(w, src) == hashPool;
            check(moved && restored, what);
        };
        // prevOrigin.x は「1 bit だけ」動かす — HashBytes のビットパターン畳み込みが
        // 仮数最下位まで効いていることの確認 (float 比較経由だと丸めで消えうる差)
        auto flipBit = [](float& f) {
            uint32_t u = 0;
            std::memcpy(&u, &f, sizeof(u));
            u ^= 1u;
            std::memcpy(&f, &u, sizeof(f));
        };
        CpuParticleBackend::EmitterPool& lp = backend.PoolsForSnapshot()[0];
        mutateCheck("hash: prevOrigin.x is covered (1-bit sensitivity)",
                    [&] { flipBit(lp.prevOrigin.x); }, [&] { flipBit(lp.prevOrigin.x); });
        mutateCheck("hash: prevOriginValid is covered", [&] { lp.prevOriginValid = 0; },
                    [&] { lp.prevOriginValid = 1; });
        mutateCheck("hash: prewarmed is covered", [&] { lp.prewarmed = 0; },
                    [&] { lp.prewarmed = 1; });
    }

    // ==== M61b: 回転 + 形状サンプリングの検証節はこの下へ ====

    // ---- (M61b-1) 基底の純関数: 恒等判定 (9 成分の完全一致) と行ベクトル規約の適用 ----
    {
        XMFLOAT4X4 m;
        XMStoreFloat4x4(&m, XMMatrixIdentity());
        m._41 = 5.0f; // 平行移動は上 3x3 の恒等判定に影響しない
        m._42 = -3.0f;
        m._43 = 2.0f;
        check(MakeParticleEmitBasis(m).identity, "m61b: translation-only matrix is identity");

        // Z 軸まわり -90° (行ベクトル規約: +Y → +X)
        XMFLOAT4X4 r = m;
        r._11 = 0.0f; r._12 = -1.0f;
        r._21 = 1.0f; r._22 = 0.0f;
        const ParticleEmitBasis rb = MakeParticleEmitBasis(r);
        check(!rb.identity, "m61b: rotation matrix is not identity");
        float dx = 0.0f, dy = 1.0f, dz = 0.0f;
        ParticleBasisRotateDir(rb, dx, dy, dz);
        check(NearF(dx, 1.0f) && NearF(dy, 0.0f) && NearF(dz, 0.0f),
              "m61b: +Y rotates to +X (row-vector convention)");

        // スケールは非恒等。方向は正規化されるがオフセットはスケールを保つ
        XMFLOAT4X4 sc = m;
        sc._11 = 2.0f;
        const ParticleEmitBasis sb = MakeParticleEmitBasis(sc);
        check(!sb.identity, "m61b: scale matrix is not identity");
        float sx = 1.0f, sy = 0.0f, sz = 0.0f;
        ParticleBasisRotateDir(sb, sx, sy, sz);
        check(NearF(sx, 1.0f) && NearF(sy, 0.0f) && NearF(sz, 0.0f),
              "m61b: scaled direction is renormalized");
        float ox = 1.0f, oy = 1.0f, oz = 1.0f;
        ParticleBasisTransformOffset(sb, ox, oy, oz);
        check(NearF(ox, 2.0f) && NearF(oy, 1.0f) && NearF(oz, 1.0f),
              "m61b: offset keeps scale (no normalize)");

        // 退化した方向 (長さ ~0) は (0,1,0) フォールバック
        float zx = 0.0f, zy = 0.0f, zz = 0.0f;
        ParticleBasisRotateDir(rb, zx, zy, zz);
        check(zx == 0.0f && zy == 1.0f && zz == 0.0f,
              "m61b: degenerate direction falls back to +Y");
    }

    // ---- (M61b-2) 恒等基底 + emitFrom=0 は従来式の手計算とビット一致 ----
    // (改変前の EmitParticles + SimulateScalar 1 step の逐語再現と比較する —
    //  「恒等パスで旧経路が温存されている」ことの直接検証)
    {
        Scene s;
        GameObject go = s.CreateGameObjectTracked("Emitter");
        auto* em = go.AddComponent<ParticleEmitterComponent>();
        em->rate = 60.0f; // ちょうど 1 粒/tick
        em->seed = 4242u;
        em->shape = 2; // cone (既定角 20°)
        s.GetWorld().ApplyStructuralChanges();
        World& w = s.GetWorld();
        SetWorldPos(w, go.Id(), 2.0f, 1.0f, -3.0f);

        CpuParticleBackend cpu;
        cpu.Update(w, kDt);

        // 手計算: 旧コードと同じ式・同じ順で 1 粒を再現 (シードはプール生成と同じ規則)
        Pcg32 r;
        r.Seed(4242u, static_cast<uint64_t>(go.Id().index) * 2u + 1u);
        const float pi = 3.14159265358979323846f; // 旧 kPi と同値
        const float cosMax = cosf(20.0f * pi / 180.0f);
        const float cosT = 1.0f - r.NextFloat01() * (1.0f - cosMax);
        const float sinT = sqrtf(std::max(0.0f, 1.0f - cosT * cosT));
        const float phi = r.NextFloat01() * 2.0f * pi;
        const float dirX = sinT * cosf(phi);
        const float dirY = cosT;
        const float dirZ = sinT * sinf(phi);
        const float speed = r.Range(2.0f, 3.5f);                     // speedMin/Max 既定
        const float lifetime = std::max(0.01f, r.Range(1.2f, 2.2f)); // lifetime 既定
        const float size = r.Range(0.10f, 0.22f);                    // size 既定
        float px = 2.0f, py = 1.0f, pz = -3.0f;
        float vx = dirX * speed, vy = dirY * speed, vz = dirZ * speed;
        // SimulateScalar 1 step (accel = gravity 既定 {0,1.5,0} + wind 0、turb 0)
        const float ax = (0.0f + 0.0f) + 0.0f * (-vz);
        const float ay = 1.5f + 0.0f;
        const float az = (0.0f + 0.0f) + 0.0f * vx;
        vx += ax * kDt;
        vy += ay * kDt;
        vz += az * kDt;
        px += vx * kDt;
        py += vy * kDt;
        pz += vz * kDt;

        const CpuParticleBackend::EmitterPool& pool = cpu.Pools()[0];
        check(pool.alive == 1, "m61b: identity emitter emitted exactly one particle");
        check(pool.px[0] == px && pool.py[0] == py && pool.pz[0] == pz && pool.vx[0] == vx
                  && pool.vy[0] == vy && pool.vz[0] == vz && pool.life[0] == lifetime - kDt
                  && pool.size0[0] == size,
              "m61b: identity path is bit-identical to the legacy formula");
    }

    // ---- (M61b-3) 90° 回転で cone 軸が +Y → +X に回る ----
    {
        Scene s;
        GameObject go = s.CreateGameObjectTracked("Emitter");
        auto* em = go.AddComponent<ParticleEmitterComponent>();
        em->rate = 300.0f;
        em->seed = 7u;
        em->shape = 2;
        em->coneAngleDeg = 0.0f; // 軸ぴったり (dir = (±0,1,±0) が厳密に出る)
        em->speedMin = 3.0f;     // レンジ退化で speed = 3.0 ちょうど
        em->speedMax = 3.0f;
        em->gravity = { 0.0f, 0.0f, 0.0f };
        s.GetWorld().ApplyStructuralChanges();
        World& w = s.GetWorld();
        auto* wm = w.GetComponent<WorldMatrixComponent>(go.Id());
        wm->value._11 = 0.0f; wm->value._12 = -1.0f; // Z 軸まわり -90°: コーン軸 +Y → +X
        wm->value._21 = 1.0f; wm->value._22 = 0.0f;

        CpuParticleBackend cpu;
        cpu.Update(w, kDt);
        const CpuParticleBackend::EmitterPool& pool = cpu.Pools()[0];
        bool axisRotated = pool.alive > 0;
        for (uint32_t i = 0; i < pool.alive; ++i) {
            axisRotated = axisRotated && pool.vx[i] == 3.0f && pool.vy[i] == 0.0f
                          && pool.vz[i] == 0.0f;
        }
        check(axisRotated, "m61b: 90-degree rotation turns the cone axis from +Y to +X");
    }

    // ---- (M61b-4) emitFrom の幾何レンジ (speed=0 で位置分布だけを観察する) ----
    {
        auto makeEmitter = [&](Scene& s, int32_t shape, int32_t emitFrom) {
            GameObject go = s.CreateGameObjectTracked("Emitter");
            auto* em = go.AddComponent<ParticleEmitterComponent>();
            em->rate = 0.0f;
            em->burstCount = 64;
            em->seed = 11u;
            em->shape = shape;
            em->emitFrom = emitFrom;
            em->shapeRadius = 1.0f;
            em->boxExtents = { 1.0f, 2.0f, 3.0f };
            em->speedMin = 0.0f;
            em->speedMax = 0.0f;
            em->gravity = { 0.0f, 0.0f, 0.0f };
            s.GetWorld().ApplyStructuralChanges();
            return go.Id();
        };

        // sphere 体積: 半径内に収まり、内部にもサンプルが出る (表面限定ではない)
        {
            Scene s;
            makeEmitter(s, 1, 1);
            CpuParticleBackend cpu;
            cpu.Update(s.GetWorld(), kDt);
            const auto& pool = cpu.Pools()[0];
            bool inRange = (pool.alive == 64);
            float minR = 1e9f;
            for (uint32_t i = 0; i < pool.alive; ++i) {
                const float r2 = pool.px[i] * pool.px[i] + pool.py[i] * pool.py[i]
                                 + pool.pz[i] * pool.pz[i];
                inRange = inRange && r2 <= 1.0f + 1e-4f;
                minR = std::min(minR, sqrtf(r2));
            }
            check(inRange, "m61b: sphere volume stays inside the radius");
            check(minR < 0.9f, "m61b: sphere volume has interior samples");
        }

        // sphere 表面 (emitFrom=2) = 従来 (0) とビット同一 (消費列も同一の契約)
        {
            Scene sa, sb;
            makeEmitter(sa, 1, 0);
            makeEmitter(sb, 1, 2);
            CpuParticleBackend a, b;
            bool same = true;
            for (int t = 0; t < 30 && same; ++t) {
                a.Update(sa.GetWorld(), kDt);
                b.Update(sb.GetWorld(), kDt);
                same = (HashSimState(a) == HashSimState(b));
            }
            check(same, "m61b: sphere emitFrom=2 (surface) is bit-identical to legacy");
        }

        // box 体積 (emitFrom=1) = 従来 (0) とビット同一 (消費列も同一の契約)
        {
            Scene sa, sb;
            makeEmitter(sa, 3, 0);
            makeEmitter(sb, 3, 1);
            CpuParticleBackend a, b;
            bool same = true;
            for (int t = 0; t < 30 && same; ++t) {
                a.Update(sa.GetWorld(), kDt);
                b.Update(sb.GetWorld(), kDt);
                same = (HashSimState(a) == HashSimState(b));
            }
            check(same, "m61b: box emitFrom=1 (volume) is bit-identical to legacy");
        }

        // box 表面: どれか 1 軸が ±extent に張り付き、全軸が extent 内
        {
            Scene s;
            makeEmitter(s, 3, 2);
            CpuParticleBackend cpu;
            cpu.Update(s.GetWorld(), kDt);
            const auto& pool = cpu.Pools()[0];
            bool onSurface = (pool.alive == 64);
            for (uint32_t i = 0; i < pool.alive; ++i) {
                const float axf = std::fabs(pool.px[i]);
                const float ayf = std::fabs(pool.py[i]);
                const float azf = std::fabs(pool.pz[i]);
                const bool inBox = axf <= 1.0f + 1e-4f && ayf <= 2.0f + 1e-4f
                                   && azf <= 3.0f + 1e-4f;
                const bool pinned = std::fabs(axf - 1.0f) < 1e-5f
                                    || std::fabs(ayf - 2.0f) < 1e-5f
                                    || std::fabs(azf - 3.0f) < 1e-5f;
                onSurface = onSurface && inBox && pinned;
            }
            check(onSurface, "m61b: box emitFrom=2 pins one axis to a face");
        }

        // cone 円盤 (emitFrom=1): y=0 の底円盤 (半径 shapeRadius) 内に散る
        {
            Scene s;
            const EntityID id = makeEmitter(s, 2, 1);
            s.GetWorld().GetComponent<ParticleEmitterComponent>(id)->coneAngleDeg = 0.0f;
            CpuParticleBackend cpu;
            cpu.Update(s.GetWorld(), kDt);
            const auto& pool = cpu.Pools()[0];
            bool onDisk = (pool.alive == 64);
            float maxR = 0.0f;
            for (uint32_t i = 0; i < pool.alive; ++i) {
                const float rr = sqrtf(pool.px[i] * pool.px[i] + pool.pz[i] * pool.pz[i]);
                onDisk = onDisk && pool.py[i] == 0.0f && rr <= 1.0f + 1e-4f;
                maxR = std::max(maxR, rr);
            }
            check(onDisk, "m61b: cone emitFrom=1 emits from the base disk (y=0)");
            check(maxR > 0.2f, "m61b: cone disk emission has radial spread");
        }
    }

    // ---- (M61b-5) 回転 + emitFrom つきでも SIMD/scalar がビット一致 ((C) 節の回転版) ----
    {
        Scene s;
        GameObject go = s.CreateGameObjectTracked("Emitter");
        auto* em = go.AddComponent<ParticleEmitterComponent>();
        em->rate = 300.0f;
        em->seed = 999u;
        em->shape = 1;
        em->emitFrom = 1;
        em->turbulence = 0.7f;
        em->gravity = { 0.0f, 2.0f, 0.0f };
        s.GetWorld().ApplyStructuralChanges();
        World& w = s.GetWorld();
        SetWorldPos(w, go.Id(), 1.0f, 2.0f, 3.0f);
        auto* wm = w.GetComponent<WorldMatrixComponent>(go.Id());
        wm->value._11 = 0.0f; wm->value._12 = -2.0f; // 回転 + スケール 2 (非恒等経路)
        wm->value._21 = 2.0f; wm->value._22 = 0.0f;
        wm->value._33 = 2.0f;

        CpuParticleBackend simd, scalar;
        simd.SetSimdEnabled(true);
        scalar.SetSimdEnabled(false);
        bool matched = true;
        for (int t = 0; t < 60 && matched; ++t) {
            simd.Update(w, kDt);
            scalar.Update(w, kDt);
            matched = (HashSimState(simd) == HashSimState(scalar));
        }
        check(matched, "m61b: SIMD and scalar stay bit-identical with rotation + emitFrom");
    }

    // ==== M61c: サブフレーム補間 + 速度継承の検証節はこの下へ ====

    // ==== M61d: 乱流ノイズの検証節はこの下へ ====

    // ==== M61e: プリウォーム + 非アクティブ凍結の検証節はこの下へ ====

    // ==== M61f: GPU 容量再作成 + バースト上限の検証節はこの下へ ====

    if (failCount == 0) {
        MYE_LOG_INFO("==== Particle self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== Particle self test: %d FAILURE(S) ====", failCount);
    return false;
}

} // namespace mye
