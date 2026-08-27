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

    // ---- (M61c-1) 既定 (係数 0 / subframe 0) は移動エミッタでも従来とビット同一 ----
    // A=移動 / B=静止 / C=速度継承 on / D=subframe on を同一シードで並走させ、
    // 「既定は履歴 (prevOrigin) をどこにも効かせない」「on は実際に変わる」を同時に見る
    {
        auto build = [&](Scene& s, float vi, int32_t sub) {
            GameObject go = s.CreateGameObjectTracked("Emitter");
            auto* em = go.AddComponent<ParticleEmitterComponent>();
            em->rate = 60.0f; // 1 粒/tick
            em->seed = 99u;
            em->shape = 2; // cone (既定角。従来式の手計算対象)
            em->velocityInheritance = vi;
            em->subframeEmission = sub;
            s.GetWorld().ApplyStructuralChanges();
            return go.Id();
        };
        Scene sa, sb, sc, sd;
        const EntityID ia = build(sa, 0.0f, 0);
        const EntityID ib = build(sb, 0.0f, 0);
        const EntityID ic = build(sc, 1.0f, 0);
        const EntityID idd = build(sd, 0.0f, 1);
        CpuParticleBackend a, b, c, d;
        // tick1: 全員 (0,0,0)
        a.Update(sa.GetWorld(), kDt);
        b.Update(sb.GetWorld(), kDt);
        c.Update(sc.GetWorld(), kDt);
        d.Update(sd.GetWorld(), kDt);
        // tick2: B 以外は (0.5,0,0) へ移動してから更新
        SetWorldPos(sa.GetWorld(), ia, 0.5f, 0.0f, 0.0f);
        SetWorldPos(sc.GetWorld(), ic, 0.5f, 0.0f, 0.0f);
        SetWorldPos(sd.GetWorld(), idd, 0.5f, 0.0f, 0.0f);
        a.Update(sa.GetWorld(), kDt);
        b.Update(sb.GetWorld(), kDt);
        c.Update(sc.GetWorld(), kDt);
        d.Update(sd.GetWorld(), kDt);

        const auto& pa = a.Pools()[0];
        const auto& pb = b.Pools()[0];
        check(pa.alive == 2 && pb.alive == 2, "m61c: both emitters made one particle per tick");
        bool sameAsStatic = true;
        for (uint32_t i = 0; i < 2; ++i) {
            // 速度・寿命・(移動していない軸の) 位置がビット一致 = 履歴はどこにも効いていない。
            // x 位置だけが誕生時 origin の平行移動差
            sameAsStatic = sameAsStatic && pa.vx[i] == pb.vx[i] && pa.vy[i] == pb.vy[i]
                           && pa.vz[i] == pb.vz[i] && pa.life[i] == pb.life[i]
                           && pa.size0[i] == pb.size0[i] && pa.py[i] == pb.py[i]
                           && pa.pz[i] == pb.pz[i];
        }
        check(sameAsStatic, "m61c: defaults ignore emitter motion (bit-identical to static)");

        // 従来式の手計算: tick2 に生まれた粒 (index 1) を旧コードの式で逐語再現
        {
            Pcg32 r;
            r.Seed(99u, static_cast<uint64_t>(ia.index) * 2u + 1u);
            // p0 (tick1) 分の RNG 消費だけ合わせる (cone 2 回 + speed/寿命/サイズ 3 回)
            (void)r.NextFloat01();
            (void)r.NextFloat01();
            (void)r.Range(2.0f, 3.5f);
            (void)r.Range(1.2f, 2.2f);
            (void)r.Range(0.10f, 0.22f);
            const float pi = 3.14159265358979323846f;
            const float cosMax = cosf(20.0f * pi / 180.0f);
            const float cosT = 1.0f - r.NextFloat01() * (1.0f - cosMax);
            const float sinT = sqrtf(std::max(0.0f, 1.0f - cosT * cosT));
            const float phi = r.NextFloat01() * 2.0f * pi;
            const float dirX = sinT * cosf(phi);
            const float dirY = cosT;
            const float dirZ = sinT * sinf(phi);
            const float speed = r.Range(2.0f, 3.5f);
            const float lifetime = std::max(0.01f, r.Range(1.2f, 2.2f));
            float vx = dirX * speed, vy = dirY * speed, vz = dirZ * speed;
            float px = 0.5f, py = 0.0f, pz = 0.0f; // tick2 の origin (移動後)
            const float ax = (0.0f + 0.0f) + 0.0f * (-vz);
            const float ay = 1.5f + 0.0f; // gravity 既定 {0,1.5,0} + wind 0
            const float az = (0.0f + 0.0f) + 0.0f * vx;
            vx += ax * kDt;
            vy += ay * kDt;
            vz += az * kDt;
            px += vx * kDt;
            py += vy * kDt;
            pz += vz * kDt;
            check(pa.px[1] == px && pa.py[1] == py && pa.pz[1] == pz && pa.vx[1] == vx
                      && pa.vy[1] == vy && pa.vz[1] == vz && pa.life[1] == lifetime - kDt,
                  "m61c: moving emitter with defaults matches the legacy formula bit-exact");
        }

        // on 側は実際に変わる (継承は初速へ、subframe は誕生位置へ)
        check(c.Pools()[0].vx[1] != pa.vx[1], "m61c: velocityInheritance=1 changes the initial velocity");
        check(std::fabs(d.Pools()[0].px[1] - pa.px[1]) > 0.1f,
              "m61c: subframeEmission=1 moves the birth position onto the trajectory");
    }

    // ---- (M61c-2) 速度継承の値検証 (静止 → 急移動) ----
    {
        Scene s;
        GameObject go = s.CreateGameObjectTracked("Emitter");
        auto* em = go.AddComponent<ParticleEmitterComponent>();
        em->rate = 60.0f;
        em->seed = 5u;
        em->shape = 2;
        em->coneAngleDeg = 0.0f; // dir = (±0,1,±0) — 継承分だけが vx に出る
        em->speedMin = 2.0f;
        em->speedMax = 2.0f;
        em->gravity = { 0.0f, 0.0f, 0.0f };
        em->velocityInheritance = 0.5f;
        s.GetWorld().ApplyStructuralChanges();
        World& w = s.GetWorld();
        SetWorldPos(w, go.Id(), 0.0f, 0.0f, 0.0f);

        CpuParticleBackend cpu;
        cpu.Update(w, kDt); // tick1: 静止 (プール誕生 tick は履歴なし = 継承 0)
        check(cpu.Pools()[0].alive == 1 && cpu.Pools()[0].vx[0] == 0.0f,
              "m61c: stationary emitter inherits nothing");

        SetWorldPos(w, go.Id(), 3.0f, 0.0f, 0.0f); // 急移動 (+3 を 1 tick で)
        cpu.Update(w, kDt);
        // EmitParticles と同一の演算列: ((origin-prev) * (1/dt)) * 係数
        const float expected = ((3.0f - 0.0f) * (1.0f / kDt)) * 0.5f;
        const auto& pool = cpu.Pools()[0];
        check(pool.alive == 2 && pool.vx[1] == expected && pool.vy[1] == 2.0f,
              "m61c: sudden move adds emitterVel * coefficient to the initial velocity");
    }

    // ---- (M61c-3) サブフレーム放出: 等分散・寿命の前倒し・軌跡上の分布 ----
    {
        auto build = [&](Scene& s, int32_t sub) {
            GameObject go = s.CreateGameObjectTracked("Emitter");
            auto* em = go.AddComponent<ParticleEmitterComponent>();
            em->rate = 600.0f; // 10 粒/tick
            em->seed = 21u;
            em->shape = 0; // point (オフセット 0 = 誕生位置が補間点そのもの)
            em->speedMin = 0.0f;
            em->speedMax = 0.0f;
            em->gravity = { 0.0f, 0.0f, 0.0f };
            em->lifetimeMin = 1.0f;
            em->lifetimeMax = 1.0f; // 寿命 1.0s ちょうど (前倒し量の検証用)
            em->subframeEmission = sub;
            s.GetWorld().ApplyStructuralChanges();
            return go.Id();
        };
        Scene sa, sb;
        const EntityID ia = build(sa, 1);
        const EntityID ib = build(sb, 0);
        CpuParticleBackend on, off;
        on.Update(sa.GetWorld(), kDt); // tick1: 原点で誕生
        off.Update(sb.GetWorld(), kDt);
        {
            // 寿命の前倒し: life = (lifetime + f*dt) - dt (invLife は本来の lifetime 基準)
            const auto& pool = on.Pools()[0];
            bool lifeStaggered = (pool.alive == 10);
            for (uint32_t n = 0; n < pool.alive && lifeStaggered; ++n) {
                const float expected =
                    (1.0f + ParticleSubframeFraction(static_cast<int>(n), 10) * kDt) - kDt;
                lifeStaggered = pool.life[n] == expected && pool.invLife[n] == 1.0f;
            }
            check(lifeStaggered, "m61c: subframe pre-consumes lifetime by birth fraction");
        }
        // tick2-4: 等速移動 (0.6/tick)。誕生位置が prevOrigin→origin の補間点に散る
        for (int t = 1; t <= 3; ++t) {
            SetWorldPos(sa.GetWorld(), ia, 0.6f * t, 0.0f, 0.0f);
            SetWorldPos(sb.GetWorld(), ib, 0.6f * t, 0.0f, 0.0f);
            on.Update(sa.GetWorld(), kDt);
            off.Update(sb.GetWorld(), kDt);
        }
        auto maxGap = [](const CpuParticleBackend& cpu) {
            std::vector<float> xs;
            const auto& pool = cpu.Pools()[0];
            for (uint32_t i = 0; i < pool.alive; ++i) {
                xs.push_back(pool.px[i]);
            }
            std::sort(xs.begin(), xs.end());
            float gap = 0.0f;
            for (size_t i = 1; i < xs.size(); ++i) {
                gap = std::max(gap, xs[i] - xs[i - 1]);
            }
            return gap;
        };
        const float gapOn = maxGap(on);
        const float gapOff = maxGap(off);
        check(on.Pools()[0].alive == 40 && gapOn < 0.1f,
              "m61c: subframe spreads births along the trajectory (gap << 1 tick of motion)");
        check(gapOff > 0.5f && gapOn < gapOff,
              "m61c: without subframe the same motion leaves 1-tick clumps");
    }

    // ==== M61d: 乱流ノイズの検証節はこの下へ ====

    // ---- (M) M61d: カールノイズ乱流 (turbulenceMode=1) ----
    // (M1) EvalCurlNoise は同一入力で決定論的 (隠れ状態も RNG も持たない純関数の確認)
    {
        const XMFLOAT3 a = EvalCurlNoise({ 1.234f, -5.678f, 9.1011f }, 12.13f);
        const XMFLOAT3 b = EvalCurlNoise({ 1.234f, -5.678f, 9.1011f }, 12.13f);
        check(a.x == b.x && a.y == b.y && a.z == b.z,
              "curl: same input twice -> bit-identical output");
        check(std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z),
              "curl: output is finite");
    }

    // (M2) 数値発散がほぼ 0 (カール場の性質)。差分刻みを内部の kCurlNoiseEps と揃えると
    // 混合中心差分が厳密に可換 (同一 4 点の同一係数和) になり、残るのは丸め誤差だけ。
    // 刻みを変えると O(h^2) の打ち切り残差が出て閾値の意味が変わるので揃えること
    {
        const XMFLOAT3 samples[5] = { { 0.3f, 0.7f, -1.2f },
                                      { 5.5f, -2.25f, 3.75f },
                                      { -8.1f, 4.4f, 0.6f },
                                      { 12.0f, -6.5f, -9.25f },
                                      { 0.5f, 0.5f, 0.5f } };
        const float times[5] = { 0.0f, 1.7f, 0.33f, 2.6f, 0.05f };
        const float h = kCurlNoiseEps;
        float maxDiv = 0.0f;
        float maxMag = 0.0f;
        for (int i = 0; i < 5; ++i) {
            const XMFLOAT3& p = samples[i];
            const float t = times[i];
            const XMFLOAT3 xp = EvalCurlNoise({ p.x + h, p.y, p.z }, t);
            const XMFLOAT3 xm = EvalCurlNoise({ p.x - h, p.y, p.z }, t);
            const XMFLOAT3 yp = EvalCurlNoise({ p.x, p.y + h, p.z }, t);
            const XMFLOAT3 ym = EvalCurlNoise({ p.x, p.y - h, p.z }, t);
            const XMFLOAT3 zp = EvalCurlNoise({ p.x, p.y, p.z + h }, t);
            const XMFLOAT3 zm = EvalCurlNoise({ p.x, p.y, p.z - h }, t);
            const float div = (xp.x - xm.x + yp.y - ym.y + zp.z - zm.z) / (2.0f * h);
            maxDiv = std::max(maxDiv, std::fabs(div));
            const XMFLOAT3 c = EvalCurlNoise(p, t);
            maxMag = std::max(maxMag, std::fabs(c.x) + std::fabs(c.y) + std::fabs(c.z));
        }
        check(maxDiv < 1e-3f, "curl: numerical divergence ~ 0 (divergence-free field)");
        check(maxMag > 0.02f, "curl: field is non-trivial (not a zero field)");
    }

    // (M3) mode=0 (既定) は従来の渦式のまま — 1 tick を渦式の手計算と突き合わせてビット一致。
    // 改変前バイナリとの直接比較はできないので、SimulateScalar の mode=0 ループと同一の
    // 演算列 (mul→add の順) をここへ書き写して同値性を検証する
    {
        Scene s;
        GameObject go = s.CreateGameObjectTracked("VortexEmitter");
        auto* em = go.AddComponent<ParticleEmitterComponent>();
        em->rate = 60.0f; // 1 粒/tick
        em->seed = 777u;
        em->turbulence = 0.7f;
        em->gravity = { 0.3f, 1.5f, -0.2f };
        em->wind = { 0.1f, 0.0f, 0.05f };
        s.GetWorld().ApplyStructuralChanges();
        World& w = s.GetWorld();
        SetWorldPos(w, go.Id(), 2.0f, 0.5f, -1.0f);

        CpuParticleBackend backend;
        backend.Update(w, kDt); // tick1: 粒子 0 が生まれ渦式で 1 回積分済み
        check(!backend.Pools().empty() && backend.Pools()[0].alive == 1,
              "vortex: one particle after first tick");
        float evx = 0.0f, evy = 0.0f, evz = 0.0f;
        float epx = 0.0f, epy = 0.0f, epz = 0.0f;
        float elife = 0.0f;
        {
            const CpuParticleBackend::EmitterPool& pool = backend.Pools()[0];
            const float accelX = em->gravity.x + em->wind.x;
            const float accelY = em->gravity.y + em->wind.y;
            const float accelZ = em->gravity.z + em->wind.z;
            const float turb = em->turbulence;
            evx = pool.vx[0];
            evy = pool.vy[0];
            evz = pool.vz[0];
            epx = pool.px[0];
            epy = pool.py[0];
            epz = pool.pz[0];
            elife = pool.life[0];
            const float ax = accelX + turb * (-evz);
            const float ay = accelY;
            const float az = accelZ + turb * evx;
            evx += ax * kDt;
            evy += ay * kDt;
            evz += az * kDt;
            epx += evx * kDt;
            epy += evy * kDt;
            epz += evz * kDt;
            elife -= kDt;
        }
        backend.Update(w, kDt); // tick2 (粒子 1 が増えるが粒子 0 の index は不変)
        const CpuParticleBackend::EmitterPool& pool2 = backend.Pools()[0];
        check(pool2.vx[0] == evx && pool2.vy[0] == evy && pool2.vz[0] == evz
                  && pool2.px[0] == epx && pool2.py[0] == epy && pool2.pz[0] == epz
                  && pool2.life[0] == elife,
              "vortex (mode=0): tick2 matches hand-computed vortex step bit-exact");
    }

    // (M4) mode=1 は SIMD 設定 on/off でビット一致 ((C) 節方式)。mode=1 のプールは Simulate が
    // 常にスカラーへ落とすので、強制が壊れると simd 側だけ渦式 (SIMD 本体) が走って割れる
    {
        Scene s;
        GameObject go = s.CreateGameObjectTracked("CurlEmitter");
        auto* em = go.AddComponent<ParticleEmitterComponent>();
        em->rate = 300.0f;
        em->seed = 4242u;
        em->shape = 1; // sphere — 全軸に初速が散る
        em->turbulence = 0.8f;
        em->turbulenceMode = 1;
        em->noiseFrequency = 1.7f;
        em->noiseSpeed = 0.9f;
        em->gravity = { 0.0f, 0.6f, 0.0f };
        s.GetWorld().ApplyStructuralChanges();
        World& w = s.GetWorld();
        SetWorldPos(w, go.Id(), 0.5f, 1.0f, -0.75f);

        CpuParticleBackend simd, scalar;
        simd.SetSimdEnabled(true);
        scalar.SetSimdEnabled(false);
        bool matched = true;
        for (int t = 0; t < 120 && matched; ++t) {
            simd.Update(w, kDt);
            scalar.Update(w, kDt);
            matched = (HashSimState(simd) == HashSimState(scalar));
        }
        check(matched, "curl (mode=1): simd on/off bit-identical (scalar path forced)");
        check(!simd.Pools().empty() && simd.Pools()[0].alive >= 8,
              "curl: pool grew past the simd threshold (forcing actually exercised)");
    }

    // (M5) mode=1 は mode=0 と挙動が変わる (実際に場が効いている) が、RNG ストリームは
    // 消費しない — 放出列が同一なので rng 状態と alive は両モードで完全一致するはず
    {
        auto makeScene = [](Scene& s, int32_t mode) -> GameObject {
            GameObject go = s.CreateGameObjectTracked("TurbEmitter");
            auto* em = go.AddComponent<ParticleEmitterComponent>();
            em->rate = 180.0f;
            em->seed = 1313u;
            em->shape = 1;
            em->turbulence = 0.9f;
            em->turbulenceMode = mode;
            s.GetWorld().ApplyStructuralChanges();
            return go;
        };
        Scene s0, s1;
        GameObject g0 = makeScene(s0, 0);
        GameObject g1 = makeScene(s1, 1);
        SetWorldPos(s0.GetWorld(), g0.Id(), 1.0f, 2.0f, 3.0f);
        SetWorldPos(s1.GetWorld(), g1.Id(), 1.0f, 2.0f, 3.0f);

        CpuParticleBackend vortex, curl;
        for (int t = 0; t < 20; ++t) {
            vortex.Update(s0.GetWorld(), kDt);
            curl.Update(s1.GetWorld(), kDt);
        }
        check(HashSimState(vortex) != HashSimState(curl),
              "curl vs vortex: mode=1 actually changes the trajectories");
        const CpuParticleBackend::EmitterPool& pv = vortex.Pools()[0];
        const CpuParticleBackend::EmitterPool& pc = curl.Pools()[0];
        check(pv.rng.State() == pc.rng.State() && pv.rng.Inc() == pc.rng.Inc()
                  && pv.alive == pc.alive,
              "curl: noise consumes no rng (emission stream identical across modes)");
    }

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
