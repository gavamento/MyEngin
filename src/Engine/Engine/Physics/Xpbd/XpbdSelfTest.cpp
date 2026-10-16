//====================================================================================
//                          XpbdSelfTest.cpp
//  MyEngine/ 秋田蓮音                                                      08/27/2026
//                                          XpbdBackend の内容ゲート/被覆/snapshot 往復検査
//====================================================================================
#include "Engine/Engine/Physics/Xpbd/XpbdSelfTest.h"

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

#include <cmath>

#include "Engine/Core/Ecs/Components.h"
#include "Engine/Engine/Physics/Collider/ConvexColliderLibrary.h" // M60'e: 形状ごとの非貫通
#include "Engine/Engine/Physics/Collider/ConvexHull.h"
#include "Engine/Engine/Physics/Collider/MeshColliderLibrary.h"
#include "Engine/Engine/Physics/Collider/TerrainColliderLibrary.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/Physics/Rigid/PhysicsSystem.h"
#include "Engine/Engine/Physics/Xpbd/XpbdBackend.h"
#include "Engine/Engine/Replay/SimSnapshot.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene/Scene.h"

namespace mye {
namespace {

// 手で組んだ検査用の池 (2 粒子 + 1 距離拘束)。ソルバには通さないので値は任意だが、
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

// ---- M60'e: 粒子 vs 世界の衝突 (片方向) ----
// ロープを横倒しで落とす共通セットアップ。rotation はロープのローカル -Y をどちらへ
// 倒すか (Z 軸 +90° → +X 方向、X 軸 +90° → -Z 方向)。始端ピンは外す (自由落下させる)
GameObject MakeLyingRope(Scene& scene, float x, float y, float z, float qx, float qy, float qz,
                         float qw, float length)
{
    GameObject go = scene.CreateGameObjectTracked("LyingRope");
    go.AddComponent<RopeComponent>();
    World& w = scene.GetWorld();
    w.ApplyStructuralChanges();
    auto* rope = w.GetComponent<RopeComponent>(go.Id());
    rope->segmentCount = 16;
    rope->length = length;
    rope->mass = 1.0f;
    rope->radius = 0.05f;
    rope->attachStart = false;
    if (auto* t = w.GetComponent<LocalTransform>(go.Id())) {
        t->position = { x, y, z };
        t->rotation = { qx, qy, qz, qw };
    }
    return go;
}

// 静的コライダー (Rigidbody 無し) を 1 個置く。形状ごとの詳細は呼び側が書き足す
ColliderComponent* MakeStaticCollider(Scene& scene, const char* name, float x, float y, float z,
                                      int32_t shape, EntityID& outId)
{
    GameObject go = scene.CreateGameObjectTracked(name);
    go.AddComponent<ColliderComponent>();
    World& w = scene.GetWorld();
    w.ApplyStructuralChanges();
    outId = go.Id();
    if (auto* t = w.GetComponent<LocalTransform>(go.Id())) {
        t->position = { x, y, z };
    }
    auto* col = w.GetComponent<ColliderComponent>(go.Id());
    col->shape = shape;
    col->isTrigger = false;
    return col;
}

// 粒子の最下点 (y − r の最小値) と最大速さ
void PoolLowestAndSpeed(const XpbdBackend::Pool& p, float radius, float& lowest, float& maxSpeed)
{
    lowest = 1e30f;
    maxSpeed = 0.0f;
    for (size_t i = 0; i < p.px.size(); ++i) {
        lowest = std::min(lowest, p.py[i] - radius);
        const float s = std::sqrt(p.vx[i] * p.vx[i] + p.vy[i] * p.vy[i] + p.vz[i] * p.vz[i]);
        maxSpeed = std::max(maxSpeed, s);
    }
}

float PoolMeanX(const XpbdBackend::Pool& p)
{
    float sum = 0.0f;
    for (const float x : p.px) {
        sum += x;
    }
    return sum / static_cast<float>(p.px.size());
}

int ContactChecks()
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
    constexpr float kRadius = 0.05f;
    // 接触は平面へ厳密に射影するので、残る貫通は反復の後の摩擦補正ぶんだけ (≒ 0)
    constexpr float kSlop = 0.002f;
    // Z 軸まわり +90° (ロープを +X へ寝かせる)
    constexpr float kS45 = 0.70710678f;

    // 1) 箱の床に落としたロープが貫通せずに静止する
    {
        Scene scene;
        World& w = scene.GetWorld();
        EntityID floorId;
        auto* col = MakeStaticCollider(scene, "Floor", 0.0f, -0.5f, 0.0f, 1, floorId);
        col->halfExtents = { 5.0f, 0.5f, 5.0f };
        MakeLyingRope(scene, -1.0f, 0.5f, 0.0f, 0.0f, 0.0f, kS45, kS45, 2.0f);
        XpbdBackend backend;
        PhysicsSystem phys;
        for (int i = 0; i < 300; ++i) {
            phys.Update(w, kDt, nullptr, &backend);
        }
        float lowest, maxSpeed;
        PoolLowestAndSpeed(backend.Pools()[0], kRadius, lowest, maxSpeed);
        float highest = -1e30f;
        for (const float y : backend.Pools()[0].py) {
            highest = std::max(highest, y - kRadius);
        }
        MYE_LOG_INFO("  [xpbd] rope on box floor: lowest %.6f highest %.6f maxSpeed %.6f",
                     static_cast<double>(lowest), static_cast<double>(highest),
                     static_cast<double>(maxSpeed));
        check(lowest > -kSlop, "a rope dropped on a box floor does not sink into it");
        check(highest < 0.01f && maxSpeed < 0.01f, "and comes to rest lying on the surface");
    }

    // 2) 斜面 30°: μ < tan30° は滑り落ち、μ > tan30° は留まる (クーロン摩擦の閾値)
    //    箱を Z 軸まわり +30° 傾けると上面の法線は (−0.5, 0.866, 0) = +X 側が高く、下り方向は −X。
    //    ロープは Z 方向 (X 軸 +90°) に寝かせ、x=0 の斜面の真上から置く
    auto slide = [&](float mu) {
        Scene scene;
        World& w = scene.GetWorld();
        GameObject env = scene.CreateGameObjectTracked("Env");
        env.AddComponent<PhysicsEnvironmentComponent>();
        EntityID slopeId;
        auto* col = MakeStaticCollider(scene, "Slope", 0.0f, 0.0f, 0.0f, 1, slopeId);
        col->halfExtents = { 5.0f, 0.5f, 5.0f };
        col->friction = mu;
        if (auto* t = w.GetComponent<LocalTransform>(slopeId)) {
            t->rotation = { 0.0f, 0.0f, 0.25881905f, 0.96592583f }; // Z 軸 +30°
        }
        // 上面は (−0.25, 0.433, 0) を通る → x=0 での高さ 0.5774。半径 + 1cm 上から置く
        MakeLyingRope(scene, 0.0f, 0.5774f + kRadius + 0.01f, 1.0f, kS45, 0.0f, 0.0f, kS45, 2.0f);
        XpbdBackend backend;
        PhysicsSystem phys;
        phys.Update(w, kDt, nullptr, &backend);
        const float x0 = PoolMeanX(backend.Pools()[0]);
        for (int i = 0; i < 60; ++i) {
            phys.Update(w, kDt, nullptr, &backend);
        }
        return PoolMeanX(backend.Pools()[0]) - x0;
    };
    {
        const float slipLow = slide(0.2f);
        const float slipHigh = slide(1.0f);
        // μ=0.2: a = g(sin30° − μ cos30°) ≈ 3.2 m/s² → 1 s で −X へ ~1.6 m (着地と静定のぶん少し短い)
        MYE_LOG_INFO("  [xpbd] slope 30deg: slip mu=0.2 %.4f / mu=1.0 %.6f",
                     static_cast<double>(slipLow), static_cast<double>(slipHigh));
        check(slipLow < -1.0f && slipLow > -2.0f,
              "a low-friction rope slides down a 30 degree slope at about g(sin - mu cos)");
        check(std::fabs(slipHigh) < 0.01f, "a high-friction rope stays put on the same slope");
    }

    // 3) 形状ごとの非貫通: 球 / カプセル / メッシュ / 地形 / 凸包
    //    「表面からの距離 ≥ r − slop」を形状ごとの式で測る
    auto dropOn = [&](Scene& scene, float x, float y, float z, float length) {
        MakeLyingRope(scene, x, y, z, 0.0f, 0.0f, kS45, kS45, length);
        XpbdBackend backend;
        PhysicsSystem phys;
        for (int i = 0; i < 240; ++i) {
            phys.Update(scene.GetWorld(), kDt, nullptr, &backend);
        }
        return backend.Pools()[0];
    };
    {
        // 球 (r=2, 原点): ロープを頂上へ掛ける
        Scene scene;
        EntityID id;
        auto* col = MakeStaticCollider(scene, "Ball", 0.0f, 0.0f, 0.0f, 0, id);
        col->radius = 2.0f;
        const XpbdBackend::Pool p = dropOn(scene, -1.5f, 2.0f + kRadius + 0.2f, 0.0f, 3.0f);
        float minGap = 1e30f;
        for (size_t i = 0; i < p.px.size(); ++i) {
            const float d = std::sqrt(p.px[i] * p.px[i] + p.py[i] * p.py[i] + p.pz[i] * p.pz[i]);
            minGap = std::min(minGap, d - 2.0f - kRadius);
        }
        MYE_LOG_INFO("  [xpbd] rope over sphere: min gap %.6f", static_cast<double>(minGap));
        check(minGap > -kSlop, "a rope draped over a sphere does not sink into it");
    }
    {
        // カプセル (横倒し: Z 軸 +90° で軸が X、r=0.5、全高 4): 線分からの距離で測る
        Scene scene;
        EntityID id;
        auto* col = MakeStaticCollider(scene, "Bar", 0.0f, 0.0f, 0.0f, 2, id);
        col->radius = 0.5f;
        col->height = 4.0f;
        if (auto* t = scene.GetWorld().GetComponent<LocalTransform>(id)) {
            t->rotation = { 0.0f, 0.0f, kS45, kS45 };
        }
        // バーと直交 (Z 方向) に掛ける
        MakeLyingRope(scene, 0.0f, 0.5f + kRadius + 0.2f, 1.5f, kS45, 0.0f, 0.0f, kS45, 3.0f);
        XpbdBackend backend;
        PhysicsSystem phys;
        for (int i = 0; i < 240; ++i) {
            phys.Update(scene.GetWorld(), kDt, nullptr, &backend);
        }
        const XpbdBackend::Pool& p = backend.Pools()[0];
        float minGap = 1e30f;
        for (size_t i = 0; i < p.px.size(); ++i) {
            const float cx = std::clamp(p.px[i], -1.5f, 1.5f); // 線分半長 = 2 − 0.5
            const float dx = p.px[i] - cx;
            const float d = std::sqrt(dx * dx + p.py[i] * p.py[i] + p.pz[i] * p.pz[i]);
            minGap = std::min(minGap, d - 0.5f - kRadius);
        }
        MYE_LOG_INFO("  [xpbd] rope over capsule: min gap %.6f", static_cast<double>(minGap));
        check(minGap > -kSlop, "a rope draped over a capsule does not sink into it");
    }
    {
        // メッシュ (y=0 の 10x10 四角形)
        MeshColliderData quad;
        BuildMeshColliderData({ { -5, 0, -5 }, { 5, 0, -5 }, { 5, 0, 5 }, { -5, 0, 5 } },
                              { 0, 1, 2, 0, 2, 3 }, quad);
        MeshColliderLibrary lib;
        const AssetID meshId{ 0x58504244ull };
        lib.Register(meshId, std::move(quad));
        meshcol::Install(&lib);
        Scene scene;
        EntityID id;
        auto* col = MakeStaticCollider(scene, "MeshFloor", 0.0f, 0.0f, 0.0f, 3, id);
        col->meshAsset = meshId;
        float lowest, maxSpeed;
        PoolLowestAndSpeed(dropOn(scene, -1.0f, 0.5f, 0.0f, 2.0f), kRadius, lowest, maxSpeed);
        MYE_LOG_INFO("  [xpbd] rope on mesh: lowest %.6f maxSpeed %.6f",
                     static_cast<double>(lowest), static_cast<double>(maxSpeed));
        check(lowest > -kSlop && maxSpeed < 0.01f, "a rope rests on a mesh collider (shape=3)");
        meshcol::Install(nullptr);
    }
    {
        // 地形 (平坦な高さ 2.5m)
        TerrainColliderLibrary* prevLib = terraincol::Library();
        TerrainColliderLibrary lib;
        terraincol::Install(&lib);
        TerrainAsset::TerrainData d;
        d.heightW = d.heightH = 9;
        d.splatW = d.splatH = 1;
        d.worldSizeX = d.worldSizeZ = 40.0f;
        d.heightBase = 0.0f;
        d.heightScale = 10.0f;
        d.heights.assign(9 * 9, static_cast<uint16_t>(0.25f * 65535.0f + 0.5f));
        d.splat.assign(4, 0);
        d.splat[0] = 255;
        const AssetID tid{ 0x58504245ull };
        lib.Register(tid, std::move(d));
        Scene scene;
        EntityID id;
        auto* col = MakeStaticCollider(scene, "Terrain", 0.0f, 0.0f, 0.0f, 4, id);
        col->meshAsset = tid;
        float lowest, maxSpeed;
        // 原点近傍 (中心原点でも隅原点でも地形の内側に入る x,z>0 側) に置く
        PoolLowestAndSpeed(dropOn(scene, 0.5f, 3.0f, 0.5f, 2.0f), kRadius, lowest, maxSpeed);
        MYE_LOG_INFO("  [xpbd] rope on terrain: lowest %.6f maxSpeed %.6f",
                     static_cast<double>(lowest), static_cast<double>(maxSpeed));
        check(lowest > 2.5f - kSlop && maxSpeed < 0.01f,
              "a rope rests on a heightfield (shape=4)");
        terraincol::Install(prevLib);
    }
    {
        // 凸包 (10x2x10、上面 y=0)
        ConvexColliderLibrary lib;
        ConvexHullData hull;
        BuildConvexHull({ { -5.0f, -1.0f, -5.0f }, { 5.0f, -1.0f, -5.0f }, { 5.0f, 1.0f, -5.0f },
                          { -5.0f, 1.0f, -5.0f }, { -5.0f, -1.0f, 5.0f }, { 5.0f, -1.0f, 5.0f },
                          { 5.0f, 1.0f, 5.0f }, { -5.0f, 1.0f, 5.0f } },
                        hull);
        const AssetID hid{ 0x58504246ull };
        lib.Register(hid, std::move(hull));
        convexcol::Install(&lib);
        Scene scene;
        EntityID id;
        auto* col = MakeStaticCollider(scene, "HullFloor", 0.0f, -1.0f, 0.0f, 5, id);
        col->meshAsset = hid;
        float lowest, maxSpeed;
        PoolLowestAndSpeed(dropOn(scene, -1.0f, 0.5f, 0.0f, 2.0f), kRadius, lowest, maxSpeed);
        MYE_LOG_INFO("  [xpbd] rope on convex: lowest %.6f maxSpeed %.6f",
                     static_cast<double>(lowest), static_cast<double>(maxSpeed));
        check(lowest > -kSlop && maxSpeed < 0.01f, "a rope rests on a convex hull (shape=5)");
        convexcol::Install(nullptr);
    }

    // 4) 除外とレイヤー: 持ち主自身のコライダーは押し出さない / マスクで外した床は素通り
    {
        // 持ち主に箱コライダー (半辺 0.5) を付けて始端ピンで吊る。除外が無いと上の
        // 粒子が箱の外へ押し出されて鎖が曲がる
        Scene scene;
        World& w = scene.GetWorld();
        GameObject go = scene.CreateGameObjectTracked("RopeHolder");
        go.AddComponent<RopeComponent>();
        go.AddComponent<ColliderComponent>();
        w.ApplyStructuralChanges();
        w.GetComponent<ColliderComponent>(go.Id())->shape = 1;
        auto* rope = w.GetComponent<RopeComponent>(go.Id());
        rope->segmentCount = 8;
        rope->length = 2.0f;
        if (auto* t = w.GetComponent<LocalTransform>(go.Id())) {
            t->position = { 0.0f, 10.0f, 0.0f };
        }
        XpbdBackend backend;
        PhysicsSystem phys;
        for (int i = 0; i < 120; ++i) {
            phys.Update(w, kDt, nullptr, &backend);
        }
        const XpbdBackend::Pool& p = backend.Pools()[0];
        float maxSide = 0.0f;
        for (size_t i = 0; i < p.px.size(); ++i) {
            maxSide = std::max(maxSide, std::max(std::fabs(p.px[i]), std::fabs(p.pz[i])));
        }
        check(maxSide < 1e-4f, "the owner's own collider does not push its rope out");
    }
    {
        Scene scene;
        World& w = scene.GetWorld();
        EntityID floorId;
        auto* col = MakeStaticCollider(scene, "GhostFloor", 0.0f, -0.5f, 0.0f, 1, floorId);
        col->halfExtents = { 5.0f, 0.5f, 5.0f };
        col->mask = ~1u; // レイヤー 0 (粒子) を相手にしない
        MakeLyingRope(scene, -1.0f, 0.5f, 0.0f, 0.0f, 0.0f, kS45, kS45, 2.0f);
        XpbdBackend backend;
        PhysicsSystem phys;
        for (int i = 0; i < 60; ++i) {
            phys.Update(w, kDt, nullptr, &backend);
        }
        float lowest, maxSpeed;
        PoolLowestAndSpeed(backend.Pools()[0], kRadius, lowest, maxSpeed);
        check(lowest < -2.0f, "a floor whose mask excludes layer 0 lets the rope fall through");
    }

    // 5) 決定論: 斜面を滑っている最中の snapshot 復元 → 再シムが同じハッシュに着地する
    {
        Scene scene;
        World& w = scene.GetWorld();
        EntityID slopeId;
        auto* col = MakeStaticCollider(scene, "Slope", 0.0f, 0.0f, 0.0f, 1, slopeId);
        col->halfExtents = { 5.0f, 0.5f, 5.0f };
        col->friction = 0.3f;
        if (auto* t = w.GetComponent<LocalTransform>(slopeId)) {
            t->rotation = { 0.0f, 0.0f, 0.25881905f, 0.96592583f };
        }
        MakeLyingRope(scene, 0.0f, 0.5774f + kRadius + 0.01f, 1.0f, kS45, 0.0f, 0.0f, kS45, 2.0f);
        XpbdBackend backend;
        PhysicsSystem phys;
        SimSources src;
        src.xpbd = &backend;
        SimRefs refs;
        refs.scene = &scene;
        refs.xpbd = &backend;
        for (int i = 0; i < 20; ++i) {
            phys.Update(w, kDt, nullptr, &backend);
        }
        std::vector<std::byte> blob;
        check(CaptureSimSnapshot(refs, blob), "capture while sliding succeeds");
        for (int i = 0; i < 30; ++i) {
            phys.Update(w, kDt, nullptr, &backend);
        }
        const uint64_t hashA = HashWorld(w, src);
        check(RestoreSimSnapshot(refs, blob.data(), blob.size()), "restore while sliding succeeds");
        for (int i = 0; i < 30; ++i) {
            phys.Update(w, kDt, nullptr, &backend);
        }
        check(HashWorld(w, src) == hashA,
              "restore + resim lands on the same hash (contacts and friction are stateless)");
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

    // ---- M60'e: 粒子 vs 世界の衝突の検査 ----
    failCount += ContactChecks();

    if (failCount == 0) {
        MYE_LOG_INFO("==== Xpbd backend self test: ALL PASS ====");
    } else {
        MYE_LOG_ERROR("==== Xpbd backend self test: %d FAILED ====", failCount);
    }
    return failCount == 0;
}

} // namespace mye
