//====================================================================================
//                          XpbdSelfTest.cpp
//  MyEngine/ 秋田蓮音                                                      08/27/2026
//                                          XpbdBackend の内容ゲート/被覆/snapshot 往復検査
//====================================================================================
#include "Engine/Engine/Physics/XpbdSelfTest.h"

#include <cstddef>
#include <utility>
#include <vector>

#include <cmath>

#include "Engine/Core/Components.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/Physics/PhysicsSystem.h"
#include "Engine/Engine/Physics/XpbdBackend.h"
#include "Engine/Engine/Replay/SimSnapshot.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene.h"

namespace mye {
namespace {

// 手で組んだ検査用の池 (2 粒子 + 1 距離拘束)。ソルバはまだ無いので値は任意だが、
// 「全フィールドが被覆に入っているか」を 1 フィールドずつ変異で確かめるため、
// 各配列が互いに違う値を持つようにしてある (同値だと px と py の取り違えを検出できない)
XpbdBackend::Pool MakeProbePool(EntityID owner)
{
    XpbdBackend::Pool p;
    p.owner = owner;
    p.kind = static_cast<uint32_t>(XpbdBackend::PoolKind::Rope);
    p.px = { 0.5f, 1.5f };
    p.py = { 2.5f, 3.5f };
    p.pz = { -0.25f, 0.25f };
    p.vx = { 0.125f, -0.125f };
    p.vy = { 4.0f, 5.0f };
    p.vz = { -6.0f, 7.0f };
    p.prevX = { 0.4375f, 1.4375f };
    p.prevY = { 2.4375f, 3.4375f };
    p.prevZ = { -0.3125f, 0.1875f };
    p.invMass = { 0.0f, 2.0f }; // 先頭はピン留め
    p.ca = { 0u };
    p.cb = { 1u };
    p.rest = { 1.0f };
    // M60'd: アタッチの焼き込みも非零で持つ (0 のままだと往復のバイト被覆にならない)
    p.attachValid = 1u;
    p.attachLx = 0.75f;
    p.attachLy = -0.5f;
    p.attachLz = 0.125f;
    return p;
}

// 末端粒子間の距離から「rest からの伸び」を測る (吊り下げ検査用の共通式)
float RopeStretch(const XpbdBackend::Pool& p, float rest)
{
    const size_t a = 0;
    const size_t b = p.px.size() - 1;
    const float dx = p.px[b] - p.px[a];
    const float dy = p.py[b] - p.py[a];
    const float dz = p.pz[b] - p.pz[a];
    return std::sqrt(dx * dx + dy * dy + dz * dz) - rest;
}

// ---- M60'c: ソルバ検査 (解析解 / 決定論 / ピン追従 / Sync の生成・破棄) ----
int SolverChecks()
{
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };
    constexpr float kDt = 1.0f / 60.0f;

    // 1) 吊り下げの静的伸び = compliance × m_端 × g (XPBD の静的解。誤差 10% 以内)
    //    2 粒子 (segmentCount=1)・全質量 2kg → 末端粒子 1kg。env 無し = kGravity 経路
    {
        Scene scene;
        World& w = scene.GetWorld();
        GameObject go = scene.CreateGameObjectTracked("Rope");
        auto* rope = go.AddComponent<RopeComponent>();
        rope->segmentCount = 1;
        rope->length = 1.0f;
        rope->mass = 2.0f;
        rope->compliance = 0.001f;
        rope->damping = 0.2f; // 静定を速める
        w.ApplyStructuralChanges();
        if (auto* t = w.GetComponent<LocalTransform>(go.Id())) {
            t->position = { 0.0f, 10.0f, 0.0f };
        }
        XpbdBackend backend;
        PhysicsSystem phys;
        for (int i = 0; i < 600; ++i) {
            phys.Update(w, kDt, nullptr, &backend);
        }
        check(backend.Pools().size() == 1, "sync builds one pool from the component");
        const float stretch = RopeStretch(backend.Pools()[0], 1.0f);
        // ★減衰は「重力を足した直後の速度」に掛かるので、定常状態の実効重力は
        //   (1 - damping) 倍になる (Rigidbody.linearDamping と同じ意味論)。
        //   実測はこの式と <0.1% で一致する — 10% 幅は反復収束の余裕
        const float expect = 0.001f * 1.0f * 9.81f * (1.0f - 0.2f); // α·m·g·(1-damping)
        MYE_LOG_INFO("  [xpbd] hanging stretch %.6f (analytic %.6f)", stretch, expect);
        check(std::fabs(stretch - expect) < expect * 0.10f,
              "hanging stretch matches compliance*m*g*(1-damping) within 10%");
    }

    // 2) compliance=0 は伸びない (< 0.1mm)
    {
        Scene scene;
        World& w = scene.GetWorld();
        GameObject go = scene.CreateGameObjectTracked("StiffRope");
        auto* rope = go.AddComponent<RopeComponent>();
        rope->segmentCount = 1;
        rope->length = 1.0f;
        rope->mass = 2.0f;
        rope->compliance = 0.0f;
        rope->damping = 0.2f;
        w.ApplyStructuralChanges();
        XpbdBackend backend;
        PhysicsSystem phys;
        for (int i = 0; i < 600; ++i) {
            phys.Update(w, kDt, nullptr, &backend);
        }
        check(backend.Pools().size() == 1 && std::fabs(RopeStretch(backend.Pools()[0], 1.0f)) < 1e-4f,
              "zero compliance keeps the rope inextensible");
    }

    // 3) 決定論: snapshot 復元 → 再シムで同じハッシュに着地する (30 tick 先)
    {
        Scene scene;
        World& w = scene.GetWorld();
        GameObject go = scene.CreateGameObjectTracked("ReplayRope");
        auto* rope = go.AddComponent<RopeComponent>();
        rope->segmentCount = 8;
        rope->length = 2.0f;
        rope->compliance = 0.0001f;
        w.ApplyStructuralChanges();
        XpbdBackend backend;
        PhysicsSystem phys;
        SimSources src;
        src.xpbd = &backend;
        SimRefs refs;
        refs.scene = &scene;
        refs.xpbd = &backend;
        for (int i = 0; i < 30; ++i) {
            phys.Update(w, kDt, nullptr, &backend);
        }
        std::vector<std::byte> blob;
        check(CaptureSimSnapshot(refs, blob), "capture mid-flight succeeds");
        for (int i = 0; i < 30; ++i) {
            phys.Update(w, kDt, nullptr, &backend);
        }
        const uint64_t hashA = HashWorld(w, src);
        check(RestoreSimSnapshot(refs, blob.data(), blob.size()), "restore mid-flight succeeds");
        for (int i = 0; i < 30; ++i) {
            phys.Update(w, kDt, nullptr, &backend);
        }
        check(HashWorld(w, src) == hashA,
              "restore + resim lands on the same hash (rope sim is deterministic)");
    }

    // 4) エネルギーが湧かない: 横倒しの振り子ロープの最大速度が上限を超えない。
    //    質点の自由落下上限は √(2gL) ≈ 8.9 m/s だが、鎖の先端は鞭効果でこれを**正当に**
    //    超える (実測 13.2 m/s。falling chain の古典的な性質)。15 は「発散していない」の堰
    {
        Scene scene;
        World& w = scene.GetWorld();
        GameObject go = scene.CreateGameObjectTracked("SwingRope");
        auto* rope = go.AddComponent<RopeComponent>();
        rope->segmentCount = 8;
        rope->length = 4.0f;
        rope->compliance = 0.0f;
        rope->damping = 0.0f;
        w.ApplyStructuralChanges();
        if (auto* t = w.GetComponent<LocalTransform>(go.Id())) {
            t->position = { 0.0f, 10.0f, 0.0f };
            // Z まわり -90 度 = ローカル -Y が -X を向く (ロープが横倒しで始まる)
            t->rotation = { 0.0f, 0.0f, -0.70710678f, 0.70710678f };
        }
        XpbdBackend backend;
        PhysicsSystem phys;
        float maxSpeed = 0.0f;
        bool finite = true;
        for (int i = 0; i < 600; ++i) {
            phys.Update(w, kDt, nullptr, &backend);
            const XpbdBackend::Pool& p = backend.Pools()[0];
            for (size_t k = 0; k < p.vx.size(); ++k) {
                const float s2 =
                    p.vx[k] * p.vx[k] + p.vy[k] * p.vy[k] + p.vz[k] * p.vz[k];
                if (!(s2 >= 0.0f) || std::isnan(p.px[k])) {
                    finite = false;
                }
                if (s2 > maxSpeed * maxSpeed) {
                    maxSpeed = std::sqrt(s2);
                }
            }
        }
        MYE_LOG_INFO("  [xpbd] swing max speed %.3f m/s (bound 15)", maxSpeed);
        check(finite && maxSpeed < 15.0f, "a swinging rope does not gain energy");
    }

    // 5) 始端ピンはエンティティへ毎 tick 追従する
    {
        Scene scene;
        World& w = scene.GetWorld();
        GameObject go = scene.CreateGameObjectTracked("PinRope");
        go.AddComponent<RopeComponent>();
        w.ApplyStructuralChanges();
        XpbdBackend backend;
        PhysicsSystem phys;
        phys.Update(w, kDt, nullptr, &backend);
        if (auto* t = w.GetComponent<LocalTransform>(go.Id())) {
            t->position = { 3.0f, 7.0f, -2.0f };
        }
        phys.Update(w, kDt, nullptr, &backend);
        const XpbdBackend::Pool& p = backend.Pools()[0];
        check(p.px[0] == 3.0f && p.py[0] == 7.0f && p.pz[0] == -2.0f,
              "the start pin follows the entity verbatim");
    }

    // 6) Sync の破棄と組み直し (コンポーネント削除 / segmentCount 変更)
    {
        Scene scene;
        World& w = scene.GetWorld();
        GameObject go = scene.CreateGameObjectTracked("SyncRope");
        auto* rope = go.AddComponent<RopeComponent>();
        rope->segmentCount = 4;
        w.ApplyStructuralChanges();
        XpbdBackend backend;
        PhysicsSystem phys;
        phys.Update(w, kDt, nullptr, &backend);
        check(backend.Pools().size() == 1 && backend.Pools()[0].px.size() == 5,
              "sync builds the pool with segmentCount+1 particles");
        if (auto* r = w.GetComponent<RopeComponent>(go.Id())) {
            r->segmentCount = 6; // 粒子数不一致 → 組み直し
        }
        phys.Update(w, kDt, nullptr, &backend);
        check(backend.Pools().size() == 1 && backend.Pools()[0].px.size() == 7,
              "changing segmentCount rebuilds the pool");
        go.RemoveComponent<RopeComponent>();
        w.ApplyStructuralChanges();
        phys.Update(w, kDt, nullptr, &backend);
        check(backend.Pools().empty(), "removing the component destroys the pool");
    }

    return failCount;
}

// ---- M60'd: 終端アタッチ (粒子 ↔ 剛体の双方向連成) ----
int AttachChecks()
{
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };
    constexpr float kDt = 1.0f / 60.0f;
    // ピン (0,10,0) から全長 1 のロープを垂らし、末端 (0,9,0) に剛体を置く共通セットアップ。
    // 末端粒子と剛体中心が一致する = 焼かれるローカルアンカーが (0,0,0) = 純並進の連成。
    // ★フィールドは ApplyStructuralChanges の**後**に再取得して書く — 2 体目の構造変更を
    //   跨いだ AddComponent 直後のポインタへ書かない (アーキタイプ移動で失効しうる)
    auto buildHanging = [](Scene& scene, float boxMass, float damping,
                           float compliance) -> std::pair<GameObject, GameObject> {
        GameObject pivot = scene.CreateGameObjectTracked("AttachPivot");
        pivot.AddComponent<RopeComponent>();
        GameObject box = scene.CreateGameObjectTracked("HangingBody");
        box.AddComponent<RigidbodyComponent>();
        World& w = scene.GetWorld();
        w.ApplyStructuralChanges();
        auto* rope = w.GetComponent<RopeComponent>(pivot.Id());
        rope->segmentCount = 1;
        rope->length = 1.0f;
        // ★軽すぎるロープにしない — 粒子と剛体の逆質量比が数百:1 になると Gauss-Seidel の
        //   伝播が 1 tick に数 % しか進まず、未収束ラグが「余計な定常伸び」として観測される
        //   (0.02kg vs 5kg の初版は解析値の 4 倍に伸びた)。0.4kg = 末端粒子 0.2kg で比 25:1
        rope->mass = 0.4f;
        rope->compliance = compliance;
        rope->damping = damping;
        rope->connectedEntity = box.Id();
        auto* rb = w.GetComponent<RigidbodyComponent>(box.Id());
        rb->mass = boxMass;
        rb->linearDamping = damping;
        rb->freezeRotation = true; // 回転経路はトルク検査が別途持つ
        if (auto* t = w.GetComponent<LocalTransform>(pivot.Id())) {
            t->position = { 0.0f, 10.0f, 0.0f };
        }
        if (auto* t = w.GetComponent<LocalTransform>(box.Id())) {
            t->position = { 0.0f, 9.0f, 0.0f };
        }
        return { pivot, box };
    };

    // 1) 静止張力 = mg: 鎖の伸び = α·(M+m_端)·g·(1-damping)。damping が実効重力を
    //    削るのは SolverChecks 1 と同じ意味論 (剛体側の linearDamping も同じ規約)。
    //    substeps=8 の env で未収束ラグを解析値の 1% 以下へ潰す (sleep は 0 で切る —
    //    眠られると剛体が凍って鎖が荷重を失い、測るものが消える)
    {
        Scene scene;
        World& w = scene.GetWorld();
        GameObject env = scene.CreateGameObjectTracked("Env");
        auto* pe = env.AddComponent<PhysicsEnvironmentComponent>();
        pe->substeps = 8;
        pe->sleepDelayTicks = 0;
        buildHanging(scene, 5.0f, 0.2f, 0.001f);
        XpbdBackend backend;
        PhysicsSystem phys;
        for (int i = 0; i < 600; ++i) {
            phys.Update(w, kDt, nullptr, &backend);
        }
        check(backend.Pools().size() == 1 && backend.Pools()[0].attachValid == 1,
              "the far end resolves and bakes the attach");
        const float stretch = RopeStretch(backend.Pools()[0], 1.0f);
        // ★substeps>1 の実効重力は (1 - d/substeps): 減衰は sub0 の残速度に掛かるが、
        //   ロープに保持された剛体の残速度は「1 サブステップ分の重力」しかない —
        //   tick 全体の重力 g·dt のうち削られるのは 1/substeps だけ (実測と 0.2% で一致)。
        //   substeps=1 の SolverChecks 1 が (1-d) なのはこの式の特殊形
        const float expect = 0.001f * (5.0f + 0.2f) * 9.81f * (1.0f - 0.2f / 8.0f);
        MYE_LOG_INFO("  [xpbd] attach hanging stretch %.6f (analytic %.6f)", stretch, expect);
        check(std::fabs(stretch - expect) < expect * 0.10f,
              "static tension equals the hanging body's weight (stretch = a*M*g within 10%)");
    }

    // 2) 切り離しで自由落下: connectedEntity を外した剛体は g で加速する
    {
        Scene scene;
        World& w = scene.GetWorld();
        auto [pivot, box] = buildHanging(scene, 5.0f, 0.0f, 0.0f);
        XpbdBackend backend;
        PhysicsSystem phys;
        for (int i = 0; i < 60; ++i) {
            phys.Update(w, kDt, nullptr, &backend);
        }
        const float yHanging = w.GetComponent<LocalTransform>(box.Id())->position.y;
        check(yHanging > 8.5f, "the body hangs on the rope before release");
        if (auto* rope = w.GetComponent<RopeComponent>(pivot.Id())) {
            rope->connectedEntity = kNullEntity;
        }
        // Δv で測る — 切り離し瞬間の残速度 (無減衰の吊りは微振動している) に依存させない
        const float v0 = w.GetComponent<RigidbodyComponent>(box.Id())->velocity.y;
        for (int i = 0; i < 60; ++i) {
            phys.Update(w, kDt, nullptr, &backend);
        }
        check(backend.Pools()[0].attachValid == 0, "clearing the target clears the bake");
        const auto* rb = w.GetComponent<RigidbodyComponent>(box.Id());
        MYE_LOG_INFO("  [xpbd] dv after 1s of free fall %.3f m/s (analytic -9.81)",
                     rb->velocity.y - v0);
        check(std::fabs((rb->velocity.y - v0) + 9.81f) < 0.3f,
              "a released body free-falls (dv over 1s equals -g)");
        check(w.GetComponent<LocalTransform>(box.Id())->position.y < yHanging - 3.0f,
              "a released body actually drops");
    }

    // 3) 振り子の周期がオーダー一致: 横倒しスタートの振り子 (L=2、重り M=1)。
    //    小振幅解析値 T = 2π√(L/g) = 2.84s。90 度振幅は ~18% 伸びるので帯は [1.4, 5.7]
    {
        Scene scene;
        World& w = scene.GetWorld();
        GameObject pivot = scene.CreateGameObjectTracked("PendulumPivot");
        pivot.AddComponent<RopeComponent>();
        GameObject bob = scene.CreateGameObjectTracked("PendulumBob");
        bob.AddComponent<RigidbodyComponent>();
        w.ApplyStructuralChanges();
        auto* rope = w.GetComponent<RopeComponent>(pivot.Id());
        rope->segmentCount = 4;
        rope->length = 2.0f;
        rope->mass = 0.02f;
        rope->compliance = 0.0f;
        rope->damping = 0.0f;
        rope->connectedEntity = bob.Id();
        auto* rb = w.GetComponent<RigidbodyComponent>(bob.Id());
        rb->mass = 1.0f;
        rb->linearDamping = 0.0f;
        rb->freezeRotation = true;
        if (auto* t = w.GetComponent<LocalTransform>(pivot.Id())) {
            t->position = { 0.0f, 10.0f, 0.0f };
            // Z まわり -90 度 = ロープが -X へ横倒し (SolverChecks 4 と同じ向き)
            t->rotation = { 0.0f, 0.0f, -0.70710678f, 0.70710678f };
        }
        if (auto* t = w.GetComponent<LocalTransform>(bob.Id())) {
            t->position = { -2.0f, 10.0f, 0.0f };
        }
        XpbdBackend backend;
        PhysicsSystem phys;
        int firstCross = -1, lastCross = -1, crossings = 0;
        float prevX = -2.0f;
        for (int i = 0; i < 600; ++i) {
            phys.Update(w, kDt, nullptr, &backend);
            const float x = w.GetComponent<LocalTransform>(bob.Id())->position.x;
            if ((prevX < 0.0f && x >= 0.0f) || (prevX > 0.0f && x <= 0.0f)) {
                ++crossings;
                if (firstCross < 0) {
                    firstCross = i;
                }
                lastCross = i;
            }
            prevX = x;
        }
        float period = 0.0f;
        if (crossings >= 2) {
            // 隣接する鉛直通過の間隔 = 半周期
            period = 2.0f * static_cast<float>(lastCross - firstCross) * kDt
                   / static_cast<float>(crossings - 1);
        }
        MYE_LOG_INFO("  [xpbd] pendulum period %.2fs over %d crossings (analytic 2.84s)", period,
                     crossings);
        check(crossings >= 2 && period > 1.4f && period < 5.7f,
              "a pendulum on a rope swings with the analytic period (order match)");
    }

    // 4) アンカーオフセットのトルク: 中心から外れた点で吊った箱は、アンカーがピンの
    //    真下へ来るまで**回転して**釣り合う (outT 経路 = 回転の実効質量の直接検査)
    {
        Scene scene;
        World& w = scene.GetWorld();
        GameObject pivot = scene.CreateGameObjectTracked("TorquePivot");
        pivot.AddComponent<RopeComponent>();
        GameObject box = scene.CreateGameObjectTracked("TorqueBox");
        box.AddComponent<RigidbodyComponent>();
        box.AddComponent<ColliderComponent>();
        w.ApplyStructuralChanges();
        auto* rope = w.GetComponent<RopeComponent>(pivot.Id());
        rope->segmentCount = 1;
        rope->length = 1.0f;
        rope->mass = 0.02f;
        rope->compliance = 0.0005f;
        rope->damping = 0.2f;
        rope->connectedEntity = box.Id();
        auto* rb = w.GetComponent<RigidbodyComponent>(box.Id());
        rb->mass = 1.0f;
        rb->linearDamping = 0.2f;
        rb->angularDamping = 0.2f;
        w.GetComponent<ColliderComponent>(box.Id())->shape = 1; // box (慣性がフルに出る)
        if (auto* t = w.GetComponent<LocalTransform>(pivot.Id())) {
            t->position = { 0.0f, 10.0f, 0.0f };
        }
        if (auto* t = w.GetComponent<LocalTransform>(box.Id())) {
            t->position = { 0.4f, 8.9f, 0.0f }; // 中心を横へ外す → アンカーは角寄り
        }
        XpbdBackend backend;
        PhysicsSystem phys;
        for (int i = 0; i < 900; ++i) {
            phys.Update(w, kDt, nullptr, &backend);
        }
        const XpbdBackend::Pool& p = backend.Pools()[0];
        check(p.attachValid == 1, "the offset anchor resolves and bakes");
        // アンカーのワールド位置 = 剛体姿勢 × 焼いたローカル (テスト側で再構成)
        const auto* t = w.GetComponent<LocalTransform>(box.Id());
        const float qx = t->rotation.x, qy = t->rotation.y, qz = t->rotation.z,
                    qw = t->rotation.w;
        const float tx = 2.0f * (qy * p.attachLz - qz * p.attachLy);
        const float ty = 2.0f * (qz * p.attachLx - qx * p.attachLz);
        const float tz = 2.0f * (qx * p.attachLy - qy * p.attachLx);
        const float ax = t->position.x + p.attachLx + qw * tx + (qy * tz - qz * ty);
        const float az = t->position.z + p.attachLz + qw * tz + (qx * ty - qy * tx);
        const float tilt = std::fabs(qz) + std::fabs(qx) + std::fabs(qy);
        MYE_LOG_INFO("  [xpbd] offset anchor rests at x=%.3f z=%.3f, |q.xyz|=%.3f", ax, az, tilt);
        check(std::fabs(ax) < 0.1f && std::fabs(az) < 0.1f,
              "the anchor settles plumb under the pin (torque coupling works)");
        check(tilt > 0.2f, "the box actually rotates to reach equilibrium");
    }

    // 5) 眠った剛体はロープに引かれて起きる (env が要る — sleep は env の存在ゲートの内側)
    {
        Scene scene;
        World& w = scene.GetWorld();
        GameObject env = scene.CreateGameObjectTracked("Env");
        auto* pe = env.AddComponent<PhysicsEnvironmentComponent>();
        pe->sleepDelayTicks = 30;
        auto [pivot, box] = buildHanging(scene, 1.0f, 0.2f, 0.0f);
        XpbdBackend backend;
        PhysicsSystem phys;
        for (int i = 0; i < 600; ++i) {
            phys.Update(w, kDt, nullptr, &backend);
        }
        const auto* rb = w.GetComponent<RigidbodyComponent>(box.Id());
        check(rb->isSleeping != 0, "a body hanging at rest falls asleep");
        const float ySleep = w.GetComponent<LocalTransform>(box.Id())->position.y;
        if (auto* t = w.GetComponent<LocalTransform>(pivot.Id())) {
            t->position.y += 2.0f; // ピンを引き上げる → 鎖がロープ経由で剛体を引く
        }
        // ★起床の判定は引いた**直後**に見る — 引き上げが数十 tick で静定すると、
        //   sleepDelayTicks=30 の再入眠が 60 tick 後の判定を正しく塗り潰す (再入眠は正常)
        for (int i = 0; i < 5; ++i) {
            phys.Update(w, kDt, nullptr, &backend);
        }
        check(w.GetComponent<RigidbodyComponent>(box.Id())->isSleeping == 0,
              "pulling the rope wakes the sleeping body");
        for (int i = 0; i < 55; ++i) {
            phys.Update(w, kDt, nullptr, &backend);
        }
        check(w.GetComponent<LocalTransform>(box.Id())->position.y > ySleep + 0.5f,
              "the woken body is hoisted by the rope");
    }

    // 6) 決定論: アタッチ込みの snapshot 復元 → 再シムが同じハッシュに着地する
    {
        Scene scene;
        World& w = scene.GetWorld();
        buildHanging(scene, 2.0f, 0.0f, 0.0001f);
        XpbdBackend backend;
        PhysicsSystem phys;
        SimSources src;
        src.xpbd = &backend;
        SimRefs refs;
        refs.scene = &scene;
        refs.xpbd = &backend;
        for (int i = 0; i < 30; ++i) {
            phys.Update(w, kDt, nullptr, &backend);
        }
        check(backend.Pools()[0].attachValid == 1, "the attach is live in the replay probe");
        std::vector<std::byte> blob;
        check(CaptureSimSnapshot(refs, blob), "capture with a live attach succeeds");
        for (int i = 0; i < 30; ++i) {
            phys.Update(w, kDt, nullptr, &backend);
        }
        const uint64_t hashA = HashWorld(w, src);
        check(RestoreSimSnapshot(refs, blob.data(), blob.size()),
              "restore with a live attach succeeds");
        for (int i = 0; i < 30; ++i) {
            phys.Update(w, kDt, nullptr, &backend);
        }
        check(HashWorld(w, src) == hashA,
              "restore + resim lands on the same hash (attach coupling is deterministic)");
    }

    return failCount;
}

} // namespace

bool RunXpbdSelfTest()
{
    MYE_LOG_INFO("==== Xpbd backend (M60'b) self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    Scene scene;
    GameObject owner = scene.CreateGameObjectTracked("XpbdOwner");
    World& w = scene.GetWorld();
    w.ApplyStructuralChanges();

    // ---- 1. 内容ゲート: 空の池は配線してもハッシュを 1 ビットも動かさない ----
    XpbdBackend backend;
    SimSources srcXpbd;
    srcXpbd.xpbd = &backend;
    const uint64_t hashPlain = HashWorld(w);
    check(HashWorld(w, srcXpbd) == hashPlain,
          "an empty backend folds nothing (content gate keeps the hash intact)");

    // ---- 2. 池の全フィールドが被覆に入っている ----
    backend.PoolsForSnapshot().push_back(MakeProbePool(owner.Id()));
    const uint64_t hashPool = HashWorld(w, srcXpbd);
    check(hashPool != hashPlain, "a populated pool changes the hash");

    // 1 フィールドずつ変異させて必ず割れることを確かめる。戻し忘れ防止のため
    // 「変異 → 比較 → 復元 → 復元確認」を 1 か所で回す
    auto mutateCheck = [&](const char* what, auto&& mutate, auto&& restore) {
        mutate();
        const bool moved = HashWorld(w, srcXpbd) != hashPool;
        restore();
        const bool restored = HashWorld(w, srcXpbd) == hashPool;
        check(moved && restored, what);
    };
    XpbdBackend::Pool& pool = backend.PoolsForSnapshot()[0];
    mutateCheck("px is hash-covered", [&] { pool.px[0] += 1.0f; }, [&] { pool.px[0] -= 1.0f; });
    mutateCheck("py is hash-covered", [&] { pool.py[1] += 1.0f; }, [&] { pool.py[1] -= 1.0f; });
    mutateCheck("pz is hash-covered", [&] { pool.pz[0] += 1.0f; }, [&] { pool.pz[0] -= 1.0f; });
    mutateCheck("vx is hash-covered", [&] { pool.vx[0] += 1.0f; }, [&] { pool.vx[0] -= 1.0f; });
    mutateCheck("vy is hash-covered", [&] { pool.vy[1] += 1.0f; }, [&] { pool.vy[1] -= 1.0f; });
    mutateCheck("vz is hash-covered", [&] { pool.vz[0] += 1.0f; }, [&] { pool.vz[0] -= 1.0f; });
    mutateCheck("prevX is hash-covered", [&] { pool.prevX[0] += 1.0f; },
                [&] { pool.prevX[0] -= 1.0f; });
    mutateCheck("prevY is hash-covered", [&] { pool.prevY[1] += 1.0f; },
                [&] { pool.prevY[1] -= 1.0f; });
    mutateCheck("prevZ is hash-covered", [&] { pool.prevZ[0] += 1.0f; },
                [&] { pool.prevZ[0] -= 1.0f; });
    mutateCheck("invMass is hash-covered", [&] { pool.invMass[1] += 1.0f; },
                [&] { pool.invMass[1] -= 1.0f; });
    mutateCheck("ca is hash-covered", [&] { pool.ca[0] = 1u; }, [&] { pool.ca[0] = 0u; });
    mutateCheck("cb is hash-covered", [&] { pool.cb[0] = 0u; }, [&] { pool.cb[0] = 1u; });
    mutateCheck("rest is hash-covered", [&] { pool.rest[0] += 0.5f; },
                [&] { pool.rest[0] -= 0.5f; });
    mutateCheck("attachValid is hash-covered", [&] { pool.attachValid = 0u; },
                [&] { pool.attachValid = 1u; });
    mutateCheck("attachLx is hash-covered", [&] { pool.attachLx += 1.0f; },
                [&] { pool.attachLx -= 1.0f; });
    mutateCheck("attachLy is hash-covered", [&] { pool.attachLy += 1.0f; },
                [&] { pool.attachLy -= 1.0f; });
    mutateCheck("attachLz is hash-covered", [&] { pool.attachLz += 1.0f; },
                [&] { pool.attachLz -= 1.0f; });
    mutateCheck("kind is hash-covered",
                [&] { pool.kind = static_cast<uint32_t>(XpbdBackend::PoolKind::Cloth); },
                [&] { pool.kind = static_cast<uint32_t>(XpbdBackend::PoolKind::Rope); });
    mutateCheck("owner is hash-covered", [&] { ++pool.owner.generation; },
                [&] { --pool.owner.generation; });

    // ---- 3. SimSnapshot v4 の往復 ----
    SimRefs refs;
    refs.scene = &scene;
    refs.xpbd = &backend;
    uint64_t tick = 60;
    refs.tickIndex = &tick;

    std::vector<std::byte> blob;
    check(CaptureSimSnapshot(refs, blob), "capture succeeds with a populated pool");

    // 池を徹底的に壊してから戻す (要素数まで変える = resize 経路も通す)
    backend.PoolsForSnapshot().clear();
    backend.PoolsForSnapshot().push_back(MakeProbePool(owner.Id()));
    backend.PoolsForSnapshot()[0].px = { 9.0f };
    backend.PoolsForSnapshot()[0].kind = static_cast<uint32_t>(XpbdBackend::PoolKind::SoftBody);
    backend.PoolsForSnapshot().push_back(MakeProbePool(owner.Id()));
    check(HashWorld(w, srcXpbd) != hashPool, "the pools really diverged before restore");

    check(RestoreSimSnapshot(refs, blob.data(), blob.size()), "restore succeeds");
    check(HashWorld(w, srcXpbd) == hashPool,
          "restore is bit-identical (hash returns to the captured value)");
    check(backend.Pools().size() == 1 && backend.Pools()[0].px.size() == 2,
          "restore rebuilds the pool shape (count and array sizes)");

    // ---- 4. blob レイアウトは refs の構成に依らない ----
    // null と「空 backend」で同じ blob になること = 「節ごとの参照が null なら空の節」の契約。
    // これが崩れると、クラッシュ .rep が「どの構成で撮ったか」に縛られてしまう
    backend.Reset();
    std::vector<std::byte> blobWithEmpty;
    check(CaptureSimSnapshot(refs, blobWithEmpty), "capture succeeds with an empty backend");
    SimRefs refsNoXpbd = refs;
    refsNoXpbd.xpbd = nullptr;
    std::vector<std::byte> blobNoXpbd;
    check(CaptureSimSnapshot(refsNoXpbd, blobNoXpbd), "capture succeeds without a backend");
    check(blobWithEmpty == blobNoXpbd,
          "a null backend and an empty backend write the same blob (layout independence)");

    // ---- 5. refs に池が無い構成は節を読み捨てる ----
    // (blob には池入りの節があるが、受け手が居なければ黙って捨てて他の節は復元する)
    backend.PoolsForSnapshot().push_back(MakeProbePool(owner.Id()));
    std::vector<std::byte> blobWithPool;
    check(CaptureSimSnapshot(refs, blobWithPool), "capture succeeds for the discard probe");
    backend.PoolsForSnapshot().clear();
    check(RestoreSimSnapshot(refsNoXpbd, blobWithPool.data(), blobWithPool.size()),
          "restore succeeds when the refs have no backend");
    check(backend.Pools().empty(), "the xpbd section is discarded, not force-applied");

    // ---- M60'c: ソルバと Sync の検査 ----
    failCount += SolverChecks();

    // ---- M60'd: 終端アタッチの検査 ----
    failCount += AttachChecks();

    if (failCount == 0) {
        MYE_LOG_INFO("==== Xpbd backend self test: ALL PASS ====");
    } else {
        MYE_LOG_ERROR("==== Xpbd backend self test: %d FAILED ====", failCount);
    }
    return failCount == 0;
}

} // namespace mye
