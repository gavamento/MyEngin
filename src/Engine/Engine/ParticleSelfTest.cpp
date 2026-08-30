#include "Engine/Engine/ParticleSelfTest.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator> // M42追補: std::size (ソート網の入力表)
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
#include "Engine/Renderer/RenderTypes.h" // M57追補: froxel::FogHandoffFraction (受け持ち分け)

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

    // ---- (M1) M61e: 非アクティブ化はプール破棄でなく凍結 (再シードしない) ----
    // 対照実験: エンティティ index まで揃えた 2 ワールドで「30 tick → 凍結 10 tick →
    // 解凍 5 tick」と「素の 35 tick」がビット一致することを見る = 凍結が rng/ageTicks/
    // emitAccum/SoA を 1 ビットも進めない (完全に時が止まる) ことの証明
    {
        // 凍結側: ActiveComponent つき + 誕生時から無効の 2 本目 (こちらはプールを持たない)
        Scene s1;
        GameObject g1 = s1.CreateGameObjectTracked("Emitter");
        auto* em1 = g1.AddComponent<ParticleEmitterComponent>();
        em1->seed = 4242u;
        g1.AddComponent<ActiveComponent>(); // enabled=1 (既定)
        GameObject gOff = s1.CreateGameObjectTracked("BornInactive");
        gOff.AddComponent<ParticleEmitterComponent>();
        auto* aOff = gOff.AddComponent<ActiveComponent>();
        aOff->enabled = 0;
        s1.GetWorld().ApplyStructuralChanges();
        World& w1 = s1.GetWorld();
        SetWorldPos(w1, g1.Id(), 1.0f, 0.0f, 0.0f);

        // 対照側: ActiveComponent 無し。g1 と同じ生成順 = 同じ entity index = 同じ rng
        // ストリーム (シードのストリーム値は e.index 由来なので index 一致が前提)
        Scene s2;
        GameObject g2 = s2.CreateGameObjectTracked("Emitter");
        auto* em2 = g2.AddComponent<ParticleEmitterComponent>();
        em2->seed = 4242u;
        s2.GetWorld().ApplyStructuralChanges();
        World& w2 = s2.GetWorld();
        SetWorldPos(w2, g2.Id(), 1.0f, 0.0f, 0.0f);
        check(g1.Id().index == g2.Id().index, "freeze: control world reuses the same entity index");

        CpuParticleBackend frozenSide, control;
        for (int t = 0; t < 30; ++t) {
            frozenSide.Update(w1, kDt);
        }
        check(frozenSide.Pools().size() == 1,
              "freeze: a born-inactive emitter never gets a pool (M10 semantics kept)");
        const uint32_t aliveBefore = frozenSide.Pools()[0].alive;
        const uint64_t rngStateBefore = frozenSide.Pools()[0].rng.State();
        const int32_t ageBefore = frozenSide.Pools()[0].ageTicks;
        const uint64_t hashBefore = HashSimState(frozenSide);
        check(aliveBefore > 0, "freeze: emitter produced particles before deactivation");

        // 凍結: 10 tick の間プールが破棄されず、1 ビットも動かない
        w1.GetComponent<ActiveComponent>(g1.Id())->enabled = 0;
        bool still = true;
        for (int t = 0; t < 10; ++t) {
            frozenSide.Update(w1, kDt);
            still = still && frozenSide.Pools().size() == 1
                && frozenSide.Pools()[0].alive == aliveBefore
                && frozenSide.Pools()[0].rng.State() == rngStateBefore
                && frozenSide.Pools()[0].ageTicks == ageBefore
                && HashSimState(frozenSide) == hashBefore;
        }
        check(still, "freeze: deactivation keeps the pool completely frozen (alive/rng/age/SoA)");
        check(frozenSide.Pools()[0].renderSkip, "freeze: frozen pool raises renderSkip");

        // 解凍: 続きから放出 (再シード・リセット無し)。renderSkip も降りる
        w1.GetComponent<ActiveComponent>(g1.Id())->enabled = 1;
        for (int t = 0; t < 5; ++t) {
            frozenSide.Update(w1, kDt);
        }
        check(frozenSide.Pools()[0].alive > aliveBefore && !frozenSide.Pools()[0].renderSkip,
              "freeze: reactivation resumes emission and clears renderSkip");

        // 対照: 素の 35 tick とビット一致 = 凍結中に時間が 1 tick も流れていない
        for (int t = 0; t < 35; ++t) {
            control.Update(w2, kDt);
        }
        check(HashSimState(frozenSide) == HashSimState(control)
                  && frozenSide.Pools()[0].rng.State() == control.Pools()[0].rng.State()
                  && frozenSide.Pools()[0].ageTicks == control.Pools()[0].ageTicks
                  && frozenSide.Pools()[0].emitAccum == control.Pools()[0].emitAccum,
              "freeze: freeze+resume is bit-identical to an uninterrupted run (no reseed)");

        // 破棄経路は従来どおり: コンポーネント削除 (存在自体の消滅) でプール消滅
        g1.RemoveComponent<ParticleEmitterComponent>();
        w1.ApplyStructuralChanges();
        frozenSide.Update(w1, kDt);
        check(frozenSide.Pools().empty(),
              "freeze: removing the component still destroys the pool");
    }

    // ---- (M2) M61e: プリウォーム — 誕生 tick に prewarmTime ぶんを先回し ----
    {
        Scene s;
        GameObject go = s.CreateGameObjectTracked("Prewarmed");
        auto* em = go.AddComponent<ParticleEmitterComponent>();
        em->seed = 777u;
        em->prewarmTime = 1.0f; // ≒60 tick ぶん
        s.GetWorld().ApplyStructuralChanges();
        World& w = s.GetWorld();
        SetWorldPos(w, go.Id(), 0.0f, 0.0f, 0.0f);

        CpuParticleBackend cpu;
        cpu.Update(w, kDt);
        // 既定 rate=200/s → 約 60 (prewarm) + 1 (通常) tick で ~200 粒。寿命 min=1.2s なので
        // まだ 1 粒も死なない = 放出総数がそのまま alive (float 丸めで ±数粒の幅を持たせる)
        const uint32_t alive1 = cpu.Pools()[0].alive;
        check(alive1 >= 190 && alive1 <= 215,
              "prewarm: first update reaches the prewarmed population (~rate*prewarmTime)");
        check(cpu.Pools()[0].prewarmed == 1, "prewarm: one-shot flag raised after first update");

        cpu.Update(w, kDt);
        const uint32_t alive2 = cpu.Pools()[0].alive;
        check(alive2 >= alive1 + 1 && alive2 <= alive1 + 8,
              "prewarm: second update emits only the normal per-tick amount (no retrigger)");

        // snapshot 復元の等価物: プールを丸ごと写した別バックエンドでも再トリガしない
        // (prewarmed==1 が sim 状態としてプールごと写るため)
        CpuParticleBackend restored;
        restored.PoolsForSnapshot() = cpu.PoolsForSnapshot();
        restored.Update(w, kDt);
        check(restored.Pools()[0].alive >= alive2 + 1 && restored.Pools()[0].alive <= alive2 + 8,
              "prewarm: a restored pool does not prewarm again");

        // playing=0 では発火しない (トリガは prewarmed==0 && prewarmTime>0 && playing)
        Scene sp;
        GameObject gp = sp.CreateGameObjectTracked("Paused");
        auto* emp = gp.AddComponent<ParticleEmitterComponent>();
        emp->prewarmTime = 1.0f;
        emp->playing = 0;
        sp.GetWorld().ApplyStructuralChanges();
        SetWorldPos(sp.GetWorld(), gp.Id(), 0.0f, 0.0f, 0.0f);
        CpuParticleBackend paused;
        paused.Update(sp.GetWorld(), kDt);
        check(!paused.Pools().empty() && paused.Pools()[0].alive == 0,
              "prewarm: playing=0 suppresses the prewarm burst");
    }

    // ==== M61f: GPU 容量再作成 + バースト上限の検証節はこの下へ ====

    // ---- (M61f) GPU 容量追従 + バースト上限 (純関数のみ — D3D 実機は selftest で回せない) ----
    {
        // 容量クランプの境界値 (下限 1024 / 上限 1M / 域内は素通し)
        check(GpuEmitterCapacityFor(0) == 1024u && GpuEmitterCapacityFor(1023) == 1024u,
              "gpu cap: below minimum clamps to 1024");
        check(GpuEmitterCapacityFor(4096) == 4096u, "gpu cap: in-range passes through");
        check(GpuEmitterCapacityFor(2000000) == 1000000u, "gpu cap: above maximum clamps to 1M");

        // 再作成判定: 同値 = 何もしない / 変更は増減どちらも作り直す
        check(!GpuCapacityNeedsRecreate(4096u, GpuEmitterCapacityFor(4096)),
              "gpu cap: unchanged capacity does not recreate");
        check(GpuCapacityNeedsRecreate(4096u, GpuEmitterCapacityFor(8192)),
              "gpu cap: grow triggers recreate");
        check(GpuCapacityNeedsRecreate(8192u, GpuEmitterCapacityFor(4096)),
              "gpu cap: shrink triggers recreate");
        // maxParticles がクランプ域の外で動いても実容量が同じなら再作成しない (毎 tick 再作成の罠)
        check(!GpuCapacityNeedsRecreate(1024u, GpuEmitterCapacityFor(512)),
              "gpu cap: clamped-equal change (512 -> min) does not recreate");

        // バースト上限: 旧 25% クランプの撤廃 — capacity 全量まで通り、超過分だけ切られる
        check(ClampGpuEmitCount(-5, 4096u) == 0, "gpu burst: negative clamps to 0");
        check(ClampGpuEmitCount(4096, 4096u) == 4096,
              "gpu burst: full capacity passes (25% clamp removed)");
        check(ClampGpuEmitCount(5000, 4096u) == 4096,
              "gpu burst: over-capacity clamps to capacity");

        // 放出計画 → クランプの実経路: burstCount=3000 は capacity=4096 でも丸ごと通る
        // (旧クランプなら 4096/4 = 1024 で切られていた値)
        ParticleEmitterComponent d;
        d.rate = 0.0f;
        d.burstCount = 3000;
        int32_t age = 0;
        float acc = 0.0f;
        const int plan = PlanParticleEmission(d, age, acc, kDt);
        check(plan == 3000 && ClampGpuEmitCount(plan, 4096u) == 3000,
              "gpu burst: planned burst passes the old 25% boundary intact");
    }

    // ==== M61g: ローカルシミュレーション空間 (simulationSpace=1) の検証節 ====

    // ---- (M61g-1) ローカル空間: エミッタの移動・回転が sim 状態へ 1 ビットも漏れない ----
    // A=毎 tick 大きく移動 + 90° 回転 / B=原点で静止。同一シードの 2 プールの SoA/rng が
    // 全 tick 完全ビット一致 = 「sim はエミッタ座標系で閉じている」の直接証明
    {
        auto build = [&](Scene& s, int32_t space) {
            GameObject go = s.CreateGameObjectTracked("Emitter");
            auto* em = go.AddComponent<ParticleEmitterComponent>();
            em->rate = 300.0f; // 5 粒/tick
            em->seed = 77u;
            em->shape = 1; // sphere (オフセットつき形状で位置経路も通す)
            em->simulationSpace = space;
            s.GetWorld().ApplyStructuralChanges();
            return go.Id();
        };
        Scene sa, sb;
        const EntityID ia = build(sa, 1);
        (void)build(sb, 1);
        {
            // A には非恒等の基底も与える (90° Y 回転、行ベクトル規約: X→-Z, Z→X) —
            // ローカル空間では M61b の基底適用がスキップされるので放出に効かないはず
            auto* wmA = sa.GetWorld().GetComponent<WorldMatrixComponent>(ia);
            wmA->value._11 = 0.0f; wmA->value._13 = -1.0f;
            wmA->value._31 = 1.0f; wmA->value._33 = 0.0f;
        }
        CpuParticleBackend a, b;
        bool identical = true;
        for (int t = 0; t < 8; ++t) {
            SetWorldPos(sa.GetWorld(), ia, 100.0f + 7.0f * t, 3.0f * t, -5.0f * t);
            a.Update(sa.GetWorld(), kDt);
            b.Update(sb.GetWorld(), kDt);
            identical = identical && (HashSimState(a) == HashSimState(b));
        }
        const auto& pa = a.Pools()[0];
        const auto& pb = b.Pools()[0];
        identical = identical && pa.rng.State() == pb.rng.State() && pa.rng.Inc() == pb.rng.Inc()
                    && pa.emitAccum == pb.emitAccum && pa.ageTicks == pb.ageTicks;
        check(identical,
              "m61g: local-space pools are bit-identical regardless of emitter motion/rotation");
        // 粒子はエミッタ座標系に居る (ワールド位置 100+ が座標へ混入していない)
        bool nearOrigin = pa.alive > 0;
        for (uint32_t i = 0; i < pa.alive; ++i) {
            nearOrigin = nearOrigin && std::fabs(pa.px[i]) < 10.0f;
        }
        check(nearOrigin, "m61g: local-space particles live in the emitter frame (near origin)");

        // 対照実験: ワールド空間 (既定) は従来どおり移動が位置に出る
        Scene sc, sd;
        const EntityID ic = build(sc, 0);
        (void)build(sd, 0);
        CpuParticleBackend c, d;
        c.Update(sc.GetWorld(), kDt);
        d.Update(sd.GetWorld(), kDt);
        SetWorldPos(sc.GetWorld(), ic, 50.0f, 0.0f, 0.0f);
        c.Update(sc.GetWorld(), kDt);
        d.Update(sd.GetWorld(), kDt);
        check(HashSimState(c) != HashSimState(d),
              "m61g: world-space control still reacts to emitter motion");
    }

    // ---- (M61g-2) TransformAabbToWorld: 恒等で素通し / 回転 + 平行移動で 8 頂点包含 ----
    {
        const XMFLOAT4X4 ident = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        const XMFLOAT3 bmin = { -1.0f, -2.0f, -3.0f };
        const XMFLOAT3 bmax = { 4.0f, 5.0f, 6.0f };
        XMFLOAT3 omin = {}, omax = {};
        TransformAabbToWorld(ident, bmin, bmax, omin, omax);
        check(omin.x == bmin.x && omin.y == bmin.y && omin.z == bmin.z && omax.x == bmax.x
                  && omax.y == bmax.y && omax.z == bmax.z,
              "m61g: aabb transform passes identity through bit-exact");

        // 90° Y 回転 + 平行移動 (行ベクトル規約: X→-Z, Z→X)。関数と同じ式で 8 頂点を
        // 変換し、全てが結果 AABB に包含されること (= 保守性) を確認する
        XMFLOAT4X4 rot = {};
        rot._13 = -1.0f;
        rot._22 = 1.0f;
        rot._31 = 1.0f;
        rot._41 = 10.0f; rot._42 = -5.0f; rot._43 = 2.5f;
        rot._44 = 1.0f;
        XMFLOAT3 rmin = {}, rmax = {};
        TransformAabbToWorld(rot, bmin, bmax, rmin, rmax);
        bool contained = true;
        for (int c8 = 0; c8 < 8; ++c8) {
            const float x = (c8 & 1) ? bmax.x : bmin.x;
            const float y = (c8 & 2) ? bmax.y : bmin.y;
            const float z = (c8 & 4) ? bmax.z : bmin.z;
            const float wx = x * rot._11 + y * rot._21 + z * rot._31 + rot._41;
            const float wy = x * rot._12 + y * rot._22 + z * rot._32 + rot._42;
            const float wz = x * rot._13 + y * rot._23 + z * rot._33 + rot._43;
            contained = contained && wx >= rmin.x && wx <= rmax.x && wy >= rmin.y && wy <= rmax.y
                        && wz >= rmin.z && wz <= rmax.z;
        }
        check(contained, "m61g: rotated+translated aabb contains all 8 transformed corners");
    }

    // ---- (M61g-3) 速度継承はローカルでは効かない (係数を立てても静止プールとビット一致) ----
    {
        auto build = [&](Scene& s, float vi) {
            GameObject go = s.CreateGameObjectTracked("Emitter");
            auto* em = go.AddComponent<ParticleEmitterComponent>();
            em->rate = 120.0f;
            em->seed = 13u;
            em->simulationSpace = 1;
            em->velocityInheritance = vi;
            s.GetWorld().ApplyStructuralChanges();
            return go.Id();
        };
        Scene sa, sb;
        const EntityID ia = build(sa, 1.0f); // 係数 on + 毎 tick 急移動
        (void)build(sb, 0.0f);               // 係数 off + 静止
        CpuParticleBackend a, b;
        bool identical = true;
        for (int t = 0; t < 6; ++t) {
            SetWorldPos(sa.GetWorld(), ia, 20.0f * static_cast<float>(t), 0.0f, 0.0f);
            a.Update(sa.GetWorld(), kDt);
            b.Update(sb.GetWorld(), kDt);
            identical = identical && (HashSimState(a) == HashSimState(b));
        }
        identical = identical && a.Pools()[0].rng.State() == b.Pools()[0].rng.State();
        check(identical, "m61g: velocityInheritance has no effect in local space (bit-identical)");
    }

    // ---- (M61g-4) プリウォームはローカルでもそのまま動く (origin=(0,0,0) 固定) ----
    {
        auto build = [&](Scene& s) {
            GameObject go = s.CreateGameObjectTracked("Emitter");
            auto* em = go.AddComponent<ParticleEmitterComponent>();
            em->rate = 600.0f; // 10 粒/tick
            em->seed = 21u;
            em->simulationSpace = 1;
            em->prewarmTime = 0.5f; // 30 tick ぶんの先回し
            s.GetWorld().ApplyStructuralChanges();
            return go.Id();
        };
        Scene sa, sb;
        const EntityID ia = build(sa);
        (void)build(sb);
        SetWorldPos(sa.GetWorld(), ia, 50.0f, -20.0f, 30.0f); // ワールド位置は sim に無関係のはず
        CpuParticleBackend a, b;
        a.Update(sa.GetWorld(), kDt);
        b.Update(sb.GetWorld(), kDt);
        check(a.Pools()[0].alive > 100 && HashSimState(a) == HashSimState(b),
              "m61g: prewarm runs in the local frame independent of the world origin");
    }

    // ---- (M57追補) 霧の係数と合成規則 ----
    // ★検査対象は **C++ ミラー** (ParticleCurves.h)。GPU 描画経路そのものは D3D が要るので
    //   ここでは触れない — 絵の担保は golden (tests\golden\fog.png) が持つ。
    //   ここが守るのは「CPU バックエンドと GPU バックエンドが同じ規則を使う」という一点で、
    //   HLSL (common.hlsli::FogFactor) との一致だけは機械照合できないので式を並べて置く
    {
        // ① off は厳密に 0。加算の (1-f) 倍も alpha の lerp も恒等になる根拠なので、
        //    近似比較ではなく == で見る (1 ULP でも残ると「霧が無いのに絵が動く」)
        check(ParticleFogFactor(-1, 0.5f, 1.0f, 100.0f, 50.0f) == 0.0f,
              "fog: mode<0 returns exactly 0 (identity for both blend modes)");

        // ② 3 モードの定義値。exp / exp2 は dist=0 で厳密に 0
        check(std::fabs(ParticleFogFactor(0, 0.0f, 10.0f, 30.0f, 20.0f) - 0.5f) < 1e-6f,
              "fog: linear is (dist-start)/(end-start)");
        check(std::fabs(ParticleFogFactor(1, 0.05f, 0.0f, 0.0f, 20.0f)
                        - (1.0f - std::exp(-1.0f))) < 1e-6f,
              "fog: exp is 1-e^(-rho*d)");
        check(std::fabs(ParticleFogFactor(2, 0.05f, 0.0f, 0.0f, 20.0f)
                        - (1.0f - std::exp(-1.0f))) < 1e-6f,
              "fog: exp2 is 1-e^(-(rho*d)^2)");
        check(ParticleFogFactor(1, 0.05f, 0.0f, 0.0f, 0.0f) == 0.0f
                  && ParticleFogFactor(2, 0.05f, 0.0f, 0.0f, 0.0f) == 0.0f,
              "fog: exp/exp2 are exactly 0 at dist 0");

        // ③ linear の saturate と退化ガード (start==end で 0 除算しない)
        check(ParticleFogFactor(0, 0.0f, 10.0f, 30.0f, 5.0f) == 0.0f
                  && ParticleFogFactor(0, 0.0f, 10.0f, 30.0f, 99.0f) == 1.0f,
              "fog: linear saturates to [0,1]");
        {
            const float f = ParticleFogFactor(0, 0.0f, 10.0f, 10.0f, 12.0f);
            check(f >= 0.0f && f <= 1.0f, "fog: linear with start==end stays finite in [0,1]");
        }

        // ④ dist について単調非減少 (全モード)
        {
            bool mono = true;
            for (int mode = 0; mode <= 2; ++mode) {
                float prev = -1.0f;
                for (int i = 0; i <= 64; ++i) {
                    const float d = 200.0f * static_cast<float>(i) / 64.0f;
                    const float f = ParticleFogFactor(mode, 0.02f, 0.0f, 150.0f, d);
                    mono = mono && (f >= prev - 1e-7f);
                    prev = f;
                }
            }
            check(mono, "fog: factor is monotonic non-decreasing in distance");
        }

        // ⑤ ★フロクセルとの受け持ち分け。グリッドの中の粒子には解析フォグが 1 ミリも
        //    乗らないこと = 三重計上を避けている主張そのもの。残り区間は乗算で作るので
        //    IEEE でも厳密に 0 になり、そこから先は全モードで f が厳密に 0 になる
        //    (linear は start>=0 のときに限る — forward_lit も同じ性質なので仕様)
        {
            const float farZ = 64.0f;
            bool ok = true;
            for (int i = 1; i <= 32; ++i) {
                const float viewZ = farZ * 0.25f * static_cast<float>(i);
                const float dist = viewZ * 1.3f; // 画面端は斜めなのでワールド距離のほうが長い
                const float remain = dist * (1.0f - froxel::FogHandoffFraction(viewZ, farZ));
                if (viewZ <= farZ) {
                    ok = ok && (remain == 0.0f);
                    for (int mode = 0; mode <= 2; ++mode) {
                        ok = ok && (ParticleFogFactor(mode, 0.02f, 0.0f, 150.0f, remain) == 0.0f);
                    }
                } else {
                    ok = ok && std::fabs(remain / dist - (1.0f - farZ / viewZ)) < 1e-5f;
                }
            }
            check(ok, "fog: inside the froxel grid the analytic fog contributes exactly nothing");
        }

        // ⑥ blendMode → 加算合成か。**CPU 側 CB (blendAdditive) と GPU 側 CB
        //    (fogColorBlend.w) が同じ値を得ることの唯一の機械保証**
        check(ParticleBlendIsAdditive(0) && !ParticleBlendIsAdditive(1)
                  && ParticleBlendIsAdditive(2),
              "fog: blendMode 0/2 are additive, 1 is alpha (shared by both backends)");
    }

    // ---- (N) M42追補: GPU alpha ソートのビットニックネットワーク ----
    // **ここが本追補の要**。GPU 上のソートは D3D 実機が要るので selftest では回せないが、
    // 「どのパスを何本、どの添字どうしを、どの向きで比べるか」という**ネットワークの正しさ**は
    // 純関数だけで完全に証明できる (ParticleCurves.h の ParticleSort* が 3 本の CS の正本)。
    // ソーティングネットワークは「全ての入力で必ず整列する」ことが要件なので、
    // ランダム入力を流して std::sort と突き合わせれば嘘がつけない。
    {
        // (1a) キー写像の単調性: float の大小がそのまま uint の大小になること。
        //      ここが壊れると整数比較が float の順序と食い違い、絵だけが静かに乱れる
        {
            const float zs[] = { -1e9f, -1000.0f, -1.5f, -1e-30f, -0.0f, 0.0f,
                                 1e-30f, 1.5f,    1000.0f, 1e9f };
            bool mono = true;
            for (size_t i = 0; i + 1 < std::size(zs); ++i) {
                const uint32_t a = ParticleSortKeyFromViewZ(zs[i]);
                const uint32_t b = ParticleSortKeyFromViewZ(zs[i + 1]);
                mono = mono && (a <= b);
                if (zs[i] < zs[i + 1]) {
                    mono = mono && (a < b);
                }
            }
            check(mono, "sort: viewZ -> uint key preserves float ordering (incl. -0.0/+0.0)");
        }

        // (1b) キー軸が CPU バックエンドの比較子と同式であること (切り出しの回帰)。
        //      旧式 `x*_13 + y*_23 + z*_33` とのビット一致を要求する
        {
            XMFLOAT4X4 vm;
            XMStoreFloat4x4(&vm, XMMatrixRotationRollPitchYaw(0.3f, -1.1f, 0.7f)
                                     * XMMatrixTranslation(3.0f, -2.0f, 11.0f));
            bool same = true;
            Pcg32 rng;
            rng.Seed(4242u);
            for (int i = 0; i < 64; ++i) {
                const float x = rng.Range(-50.0f, 50.0f);
                const float y = rng.Range(-50.0f, 50.0f);
                const float z = rng.Range(-50.0f, 50.0f);
                const float legacy = x * vm._13 + y * vm._23 + z * vm._33;
                same = same && (ParticleAlphaSortViewZ(x, y, z, vm) == legacy);
            }
            check(same, "sort: ParticleAlphaSortViewZ is bit-identical to the legacy comparator");
        }

        // (2) パス表の本数と並び。setup CS が同じ規則で間接引数を書くので、
        //     ここがずれると「途中から別のネットワーク」になる
        {
            ParticleSortPass passes[kParticleSortMaxPasses];
            const uint32_t n1 =
                ParticleSortBuildPasses(kParticleSortBlock, passes, kParticleSortMaxPasses);
            check(n1 == 1 && passes[0].kind == ParticleSortPassKind::BlockSort,
                  "sort: one block needs exactly one LDS pass (the cheap hybrid path)");
            const uint32_t n2 =
                ParticleSortBuildPasses(kParticleSortBlock * 8, passes, kParticleSortMaxPasses);
            // k = 2B(1 merge + 1 blockmerge) / 4B(2+1) / 8B(3+1) = 9、+ 先頭のブロックソート
            check(n2 == 10, "sort: pass count follows the block-merge schedule");
            const uint32_t nMax =
                ParticleSortBuildPasses(1u << 20, passes, kParticleSortMaxPasses);
            check(nMax <= kParticleSortMaxPasses,
                  "sort: the 1M capacity ceiling stays inside the pass table");
        }

        // (3) **本命**: ネットワークを実際に回した結果が std::sort と完全一致すること。
        //     番兵で 2 冪へ詰め、先頭 pad 件が back-to-front に並ぶことを見る。
        //     2047/2048/2049 は「1 ブロックに収まる/全域マージが起動する」の境界
        {
            const uint32_t aliveCases[] = { 0, 1, 2, 3, 100, 2047, 2048, 2049, 5000, 9000 };
            bool allSorted = true;
            bool anyGlobal = false;
            for (uint32_t alive : aliveCases) {
                const uint32_t sortCapacity = ParticleSortCapacityFor(16384);
                const uint32_t pad = ParticleSortPadFor(alive, sortCapacity);
                std::vector<uint32_t> keys(sortCapacity, 0u);
                std::vector<uint32_t> idx(sortCapacity, 0xFFFFFFFFu);
                Pcg32 rng;
                rng.Seed(90210u + alive);
                for (uint32_t i = 0; i < alive; ++i) { // setup CS と同じ充填
                    keys[i] = ParticleSortKeyFromViewZ(rng.Range(-40.0f, 40.0f));
                    idx[i] = i * 7u + 3u; // スロット番号は連番でない (dead list 由来)
                }
                std::vector<uint32_t> expectK(keys.begin(), keys.begin() + pad);
                std::vector<uint32_t> expectI(idx.begin(), idx.begin() + pad);
                std::vector<uint32_t> order(pad);
                for (uint32_t i = 0; i < pad; ++i) {
                    order[i] = i;
                }
                std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
                    return ParticleSortBefore(expectK[a], expectI[a], expectK[b], expectI[b]);
                });
                std::vector<uint32_t> refK(pad), refI(pad);
                for (uint32_t i = 0; i < pad; ++i) {
                    refK[i] = expectK[order[i]];
                    refI[i] = expectI[order[i]];
                }
                ParticleSortPass passes[kParticleSortMaxPasses];
                const uint32_t passCount =
                    ParticleSortBuildPasses(sortCapacity, passes, kParticleSortMaxPasses);
                for (uint32_t p = 0; p < passCount; ++p) {
                    if (passes[p].kind != ParticleSortPassKind::BlockSort
                        && ParticleSortGroupsFor(passes[p], pad) > 0) {
                        anyGlobal = true;
                    }
                    ParticleSortApplyPass(passes[p], pad, keys.data(), idx.data());
                }
                for (uint32_t i = 0; i < pad; ++i) {
                    allSorted = allSorted && keys[i] == refK[i] && idx[i] == refI[i];
                }
            }
            check(allSorted, "sort: the bitonic network matches std::sort for every alive count");
            check(anyGlobal, "sort: alive beyond one block actually engages the global merge");
        }

        // (4) 同値キーだけの入力 = tie-break が添字昇順に落ちること。
        //     CPU 側 `return a < b;` と同じ規則でないと、同深度で重なった粒子の絵が食い違う
        {
            const uint32_t sortCapacity = ParticleSortCapacityFor(4096);
            const uint32_t alive = 3000;
            const uint32_t pad = ParticleSortPadFor(alive, sortCapacity);
            std::vector<uint32_t> keys(sortCapacity, 0u);
            std::vector<uint32_t> idx(sortCapacity, 0xFFFFFFFFu);
            const uint32_t flat = ParticleSortKeyFromViewZ(-12.5f);
            for (uint32_t i = 0; i < alive; ++i) {
                keys[i] = flat;
                idx[i] = alive - 1 - i; // わざと降順に入れる
            }
            ParticleSortPass passes[kParticleSortMaxPasses];
            const uint32_t passCount =
                ParticleSortBuildPasses(sortCapacity, passes, kParticleSortMaxPasses);
            for (uint32_t p = 0; p < passCount; ++p) {
                ParticleSortApplyPass(passes[p], pad, keys.data(), idx.data());
            }
            bool ascending = true;
            for (uint32_t i = 0; i < alive; ++i) {
                ascending = ascending && (keys[i] == flat) && (idx[i] == i);
            }
            check(ascending, "sort: equal depths fall back to ascending index (CPU tie-break)");
        }

        // (5) 番兵は必ず最後尾へ落ちること = 生存数ぶんだけ描けば正しい粒子が出る。
        //     崩れると「描画数は合っているのに死んだスロットが混ざる」形で壊れる
        {
            const uint32_t sortCapacity = ParticleSortCapacityFor(8192);
            const uint32_t alive = 1500;
            const uint32_t pad = ParticleSortPadFor(alive, sortCapacity);
            std::vector<uint32_t> keys(sortCapacity, 0u);
            std::vector<uint32_t> idx(sortCapacity, 0xFFFFFFFFu);
            Pcg32 rng;
            rng.Seed(5150u);
            for (uint32_t i = 0; i < alive; ++i) {
                keys[i] = ParticleSortKeyFromViewZ(rng.Range(1.0f, 90.0f)); // 番兵 (0) より必ず大
                idx[i] = i;
            }
            ParticleSortPass passes[kParticleSortMaxPasses];
            const uint32_t passCount =
                ParticleSortBuildPasses(sortCapacity, passes, kParticleSortMaxPasses);
            for (uint32_t p = 0; p < passCount; ++p) {
                ParticleSortApplyPass(passes[p], pad, keys.data(), idx.data());
            }
            bool ok = true;
            for (uint32_t i = 0; i < alive; ++i) {
                ok = ok && (idx[i] < alive); // 先頭 alive 件はすべて実スロット
            }
            for (uint32_t i = alive; i < pad; ++i) {
                ok = ok && (keys[i] == 0u) && (idx[i] == 0xFFFFFFFFu);
            }
            check(ok, "sort: sentinels always land behind every live particle");
        }

        // (6) 間接引数が「alive が 1 ブロックに収まる限り実働 1 本」を守ること。
        //     ハイブリッドの主張そのもの — ここが 0 に落ちないと WARP のスクショが重くなる
        {
            ParticleSortPass passes[kParticleSortMaxPasses];
            const uint32_t sortCapacity = ParticleSortCapacityFor(100000); // fog demo の既定容量
            const uint32_t passCount =
                ParticleSortBuildPasses(sortCapacity, passes, kParticleSortMaxPasses);
            const uint32_t pad = ParticleSortPadFor(150, sortCapacity); // 煙の実測生存数くらい
            uint32_t working = 0;
            for (uint32_t p = 0; p < passCount; ++p) {
                if (ParticleSortGroupsFor(passes[p], pad) > 0) {
                    ++working;
                }
            }
            check(passCount == 28 && working == 1,
                  "sort: a small alpha emitter costs exactly one working dispatch of 28 issued");
        }
    }

    if (failCount == 0) {
        MYE_LOG_INFO("==== Particle self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== Particle self test: %d FAILURE(S) ====", failCount);
    return false;
}

} // namespace mye
