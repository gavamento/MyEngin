#include "Engine/Engine/PhysicsSelfTest.h"

#include <cmath>
#include <cstring>

#include "Engine/Core/Components.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/Physics/PhysicsSystem.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Engine/TransformSystem.h"

namespace mye {
namespace {

// 動的ボックス (Rigidbody + ソリッド aabb コライダー) を生成
GameObject MakeBox(Scene& s, const char* name, float x, float y, float z, float hx, float hy,
                   float hz, float restitution = 0.0f)
{
    GameObject go = s.CreateGameObjectTracked(name);
    go.SetLocalPosition(x, y, z);
    auto* col = go.AddComponent<ColliderComponent>();
    col->shape = 1; // aabb
    col->isTrigger = 0;
    col->halfExtents = { hx, hy, hz };
    auto* rb = go.AddComponent<RigidbodyComponent>();
    rb->mass = 1.0f;
    rb->gravityScale = 1.0f;
    rb->restitution = restitution;
    return go;
}

// 静的な床 (ソリッド aabb コライダーのみ、Rigidbody 無し)
GameObject MakeGround(Scene& s, const char* name, float x, float y, float z, float hx, float hy,
                      float hz, int isTrigger = 0)
{
    GameObject go = s.CreateGameObjectTracked(name);
    go.SetLocalPosition(x, y, z);
    auto* col = go.AddComponent<ColliderComponent>();
    col->shape = 1;
    col->isTrigger = isTrigger;
    col->halfExtents = { hx, hy, hz };
    return go;
}

// ---- M28a 形状拡張の検証用ヘルパ ----

// 動的球 (Rigidbody + ソリッド sphere コライダー)
GameObject MakeSphereBody(Scene& s, const char* name, float x, float y, float z, float r)
{
    GameObject go = s.CreateGameObjectTracked(name);
    go.SetLocalPosition(x, y, z);
    auto* col = go.AddComponent<ColliderComponent>();
    col->shape = 0;
    col->isTrigger = 0;
    col->radius = r;
    auto* rb = go.AddComponent<RigidbodyComponent>();
    rb->mass = 1.0f;
    rb->gravityScale = 1.0f;
    return go;
}

// 動的カプセル (ローカル Y 軸、height は両端球込みの全高)
GameObject MakeCapsuleBody(Scene& s, const char* name, float x, float y, float z, float r,
                           float height)
{
    GameObject go = s.CreateGameObjectTracked(name);
    go.SetLocalPosition(x, y, z);
    auto* col = go.AddComponent<ColliderComponent>();
    col->shape = 2;
    col->isTrigger = 0;
    col->radius = r;
    col->height = height;
    auto* rb = go.AddComponent<RigidbodyComponent>();
    rb->mass = 1.0f;
    rb->gravityScale = 1.0f;
    return go;
}

// 回転付き静的ソリッド box (OBB 検証用)
GameObject MakeStaticBoxRot(Scene& s, const char* name, float x, float y, float z, float hx,
                            float hy, float hz, const DirectX::XMFLOAT4& rot)
{
    GameObject go = MakeGround(s, name, x, y, z, hx, hy, hz);
    go.GetComponent<LocalTransform>()->rotation = rot;
    return go;
}

} // namespace

bool RunPhysicsSelfTest()
{
    MYE_LOG_INFO("==== Physics self test ====");
    RegisterBuiltinComponents(); // sTypeId 解決 (冪等)
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
    PhysicsSystem phys;

    // ---- (1) 重力による自由落下 ----
    {
        Scene s;
        GameObject box = MakeBox(s, "Faller", 0, 0, 0, 0.5f, 0.5f, 0.5f);
        s.GetWorld().ApplyStructuralChanges();
        for (int i = 0; i < 60; ++i) {
            phys.Update(s.GetWorld(), kDt);
        }
        auto* lt = box.GetComponent<LocalTransform>();
        auto* rb = box.GetComponent<RigidbodyComponent>();
        // 1 秒後 ≈ -0.5*g*t^2 ≈ -4.9m、速度 ≈ -9.81 m/s
        check(lt && lt->position.y < -3.0f, "gravity: box fell downward after 60 ticks");
        check(rb && rb->velocity.y < -5.0f, "gravity: downward velocity accumulated");
    }

    // ---- (2) ソリッド床で接地・貫通しない ----
    {
        Scene s;
        MakeGround(s, "Ground", 0, -0.5f, 0, 5.0f, 0.5f, 5.0f); // 上面 y=0
        GameObject box = MakeBox(s, "Rester", 0, 3.0f, 0, 0.5f, 0.5f, 0.5f);
        s.GetWorld().ApplyStructuralChanges();
        for (int i = 0; i < 180; ++i) { // 3 秒
            phys.Update(s.GetWorld(), kDt);
        }
        auto* lt = box.GetComponent<LocalTransform>();
        // 箱の底が床上面 (y=0) に乗る → 中心 y ≈ 0.5
        const float y = lt ? lt->position.y : -999.0f;
        check(y > 0.45f && y < 0.55f, "ground contact: box rests at y~=0.5 (no fall-through)");
        auto* rb = box.GetComponent<RigidbodyComponent>();
        check(rb && std::fabs(rb->velocity.y) < 0.5f, "ground contact: settled (|vy| small)");
    }

    // ---- (3) トリガーコライダーは衝突面でない (透過して落下し続ける) ----
    {
        Scene s;
        MakeGround(s, "TrigGround", 0, -0.5f, 0, 5.0f, 0.5f, 5.0f, /*isTrigger=*/1);
        GameObject box = MakeBox(s, "Passer", 0, 3.0f, 0, 0.5f, 0.5f, 0.5f);
        s.GetWorld().ApplyStructuralChanges();
        for (int i = 0; i < 180; ++i) {
            phys.Update(s.GetWorld(), kDt);
        }
        auto* lt = box.GetComponent<LocalTransform>();
        check(lt && lt->position.y < -1.0f, "trigger collider is not solid (box passes through)");
    }

    // ---- (4) 決定論: 同一シーンを 2 個並走させ per-tick ハッシュ一致 ----
    {
        auto build = [](Scene& s) {
            MakeGround(s, "G", 0, -0.5f, 0, 5.0f, 0.5f, 5.0f);
            MakeBox(s, "B0", -1.0f, 2.0f, 0, 0.5f, 0.5f, 0.5f, 0.2f);
            MakeBox(s, "B1", 1.0f, 4.0f, 0, 0.5f, 0.5f, 0.5f, 0.4f);
            MakeBox(s, "B2", 0.2f, 6.0f, 0.1f, 0.5f, 0.5f, 0.5f, 0.0f);
            s.GetWorld().ApplyStructuralChanges();
        };
        Scene sa, sb;
        build(sa);
        build(sb);
        bool det = true;
        uint64_t finalHash = 0;
        for (int i = 0; i < 240 && det; ++i) {
            phys.Update(sa.GetWorld(), kDt);
            phys.Update(sb.GetWorld(), kDt);
            const uint64_t ha = HashWorld(sa.GetWorld(), nullptr);
            const uint64_t hb = HashWorld(sb.GetWorld(), nullptr);
            if (ha != hb) {
                det = false;
                MYE_LOG_ERROR("  determinism diverged at tick %d: %016llX vs %016llX", i,
                              static_cast<unsigned long long>(ha),
                              static_cast<unsigned long long>(hb));
            }
            finalHash = ha;
        }
        check(det, "determinism: two identical scenes hash-identical for 240 ticks");
        // Debug/Release 間の一致確認用に最終ハッシュを出力 (両構成で同値であること)
        MYE_LOG_INFO("  [phys] stacking scene hash @240 = %016llX",
                     static_cast<unsigned long long>(finalHash));
        // 最終状態の位置/速度ビットパターン (構成間・コミット間の挙動照合用。
        // ワールドハッシュはフィールド追加で変わり得るが、この値は純粋に sim 挙動のみを表す)
        {
            const ComponentTypeId req[] = { RigidbodyComponent::sTypeId, LocalTransform::sTypeId };
            sa.GetWorld().ForEachArchetype(req, [&](Archetype& arch) {
                const int li = arch.FindTypeIndex(LocalTransform::sTypeId);
                const int ri = arch.FindTypeIndex(RigidbodyComponent::sTypeId);
                for (uint32_t row = 0; row < arch.Count(); ++row) {
                    const auto* lt = static_cast<const LocalTransform*>(arch.GetPtr(li, row));
                    const auto* rb = static_cast<const RigidbodyComponent*>(arch.GetPtr(ri, row));
                    uint32_t p[3], v[3];
                    std::memcpy(p, &lt->position, sizeof(p));
                    std::memcpy(v, &rb->velocity, sizeof(v));
                    MYE_LOG_INFO("  [phys] body %u pos %08X %08X %08X vel %08X %08X %08X",
                                 arch.EntityAt(row).index, p[0], p[1], p[2], v[0], v[1], v[2]);
                }
            });
        }
    }

    // ---- (5) Raycast: aabb ヒット (距離 / エンティティ / 法線) ----
    {
        Scene s;
        TransformSystem ts;
        GameObject target = MakeGround(s, "RayBox", 5.0f, 0, 0, 0.5f, 0.5f, 0.5f); // 中心 x=5
        s.GetWorld().ApplyStructuralChanges();
        ts.Update(s.GetWorld()); // WorldMatrix を確定 (Raycast はワールド行列基準)
        MyeRaycastHit hit = {};
        const int ok = RaycastWorld(s.GetWorld(), { 0, 0, 0 }, { 1, 0, 0 }, 100.0f, &hit);
        check(ok == 1, "raycast: +X ray hits aabb");
        check(ok && hit.entity.index == target.Id().index, "raycast: hit reports correct entity");
        check(ok && std::fabs(hit.distance - 4.5f) < 1e-3f, "raycast: distance ~= 4.5 (5 - half)");
        check(ok && hit.normal.x < -0.99f, "raycast: normal faces -X (back toward ray)");
    }

    // ---- (6) Raycast: sphere ヒット ----
    {
        Scene s;
        TransformSystem ts;
        GameObject sph = s.CreateGameObjectTracked("RaySphere");
        sph.SetLocalPosition(0, 0, 10.0f);
        auto* col = sph.AddComponent<ColliderComponent>();
        col->shape = 0; // sphere
        col->radius = 1.0f;
        col->isTrigger = 0;
        s.GetWorld().ApplyStructuralChanges();
        ts.Update(s.GetWorld());
        MyeRaycastHit hit = {};
        const int ok = RaycastWorld(s.GetWorld(), { 0, 0, 0 }, { 0, 0, 2.0f }, 100.0f, &hit);
        check(ok == 1, "raycast: +Z ray hits sphere");
        check(ok && std::fabs(hit.distance - 9.0f) < 1e-3f, "raycast: sphere distance ~= 9.0");
        check(ok && hit.normal.z < -0.99f, "raycast: sphere normal faces -Z");
    }

    // ---- (7) Raycast: ミス ----
    {
        Scene s;
        TransformSystem ts;
        MakeGround(s, "Box", 5.0f, 0, 0, 0.5f, 0.5f, 0.5f);
        s.GetWorld().ApplyStructuralChanges();
        ts.Update(s.GetWorld());
        MyeRaycastHit hit = {};
        const int ok = RaycastWorld(s.GetWorld(), { 0, 0, 0 }, { 0, 1, 0 }, 100.0f, &hit);
        check(ok == 0, "raycast: +Y ray misses (no collider above)");
    }

    // ---- (8) capsule がソリッド床に接地 (中心 y ~= height/2) ----
    {
        Scene s;
        MakeGround(s, "G8", 0, -0.5f, 0, 5.0f, 0.5f, 5.0f);
        GameObject cap = MakeCapsuleBody(s, "Cap8", 0, 3.0f, 0, 0.5f, 2.0f);
        s.GetWorld().ApplyStructuralChanges();
        for (int i = 0; i < 180; ++i) {
            phys.Update(s.GetWorld(), kDt);
        }
        auto* lt = cap.GetComponent<LocalTransform>();
        const float y = lt ? lt->position.y : -999.0f;
        check(y > 0.95f && y < 1.05f, "capsule: rests on ground at y~=1.0 (height/2)");
    }

    // ---- (9) capsule-capsule: 横に重ねた 2 本が押し出しで分離する ----
    {
        Scene s;
        GameObject a = MakeCapsuleBody(s, "CapA", -0.3f, 1.0f, 0, 0.5f, 2.0f);
        GameObject b = MakeCapsuleBody(s, "CapB", 0.3f, 1.0f, 0, 0.5f, 2.0f);
        a.GetComponent<RigidbodyComponent>()->gravityScale = 0.0f;
        b.GetComponent<RigidbodyComponent>()->gravityScale = 0.0f;
        s.GetWorld().ApplyStructuralChanges();
        for (int i = 0; i < 60; ++i) {
            phys.Update(s.GetWorld(), kDt);
        }
        auto* la = a.GetComponent<LocalTransform>();
        auto* lb = b.GetComponent<LocalTransform>();
        const float dx = (la && lb) ? (lb->position.x - la->position.x) : 0.0f;
        check(dx > 0.98f, "capsule-capsule: overlapping pair separates to >= sum of radii");
    }

    // ---- (10) OBB: 45° 斜面 (回転 box) 上の球が斜面に沿って -X へ滑る ----
    {
        Scene s;
        // Z 軸回り +45° (斜面法線 ~= (-0.71, 0.71, 0) → 下りは -X 方向)
        MakeStaticBoxRot(s, "Slope", 0, 0, 0, 5.0f, 0.5f, 5.0f,
                         { 0, 0, 0.3826834f, 0.9238795f });
        GameObject ball = MakeSphereBody(s, "Ball", 0, 3.0f, 0, 0.5f);
        s.GetWorld().ApplyStructuralChanges();
        for (int i = 0; i < 45; ++i) {
            phys.Update(s.GetWorld(), kDt);
        }
        auto* lt = ball.GetComponent<LocalTransform>();
        check(lt && lt->position.x < -0.3f, "obb: sphere slides downhill (-X) on 45deg slope");
        check(lt && lt->position.y > 0.3f, "obb: sphere stays on slope surface (no tunnel)");
    }

    // ---- (11) SAT: 45° 回転した動的 box が平床に角で立つ (並進のみ = 姿勢は維持) ----
    {
        Scene s;
        MakeGround(s, "G11", 0, -0.5f, 0, 5.0f, 0.5f, 5.0f);
        GameObject box = MakeBox(s, "TiltBox", 0, 3.0f, 0, 0.5f, 0.5f, 0.5f);
        box.GetComponent<LocalTransform>()->rotation = { 0, 0, 0.3826834f, 0.9238795f };
        s.GetWorld().ApplyStructuralChanges();
        for (int i = 0; i < 180; ++i) {
            phys.Update(s.GetWorld(), kDt);
        }
        auto* lt = box.GetComponent<LocalTransform>();
        const float y = lt ? lt->position.y : -999.0f;
        // 対角半径 = (hx+hy)/sqrt(2) ~= 0.7071 (SAT 最小軸 = 床の +Y 面)
        check(y > 0.65f && y < 0.78f, "sat: 45deg box rests on edge at y~=0.707");
    }

    // ---- (12) Raycast: 45° OBB (角に当たる → 距離 = 5 - 0.7071) ----
    {
        Scene s;
        TransformSystem ts;
        MakeStaticBoxRot(s, "RayObb", 5.0f, 0, 0, 0.5f, 0.5f, 0.5f,
                         { 0, 0.3826834f, 0, 0.9238795f }); // Y 軸回り 45°
        s.GetWorld().ApplyStructuralChanges();
        ts.Update(s.GetWorld());
        MyeRaycastHit hit = {};
        const int ok = RaycastWorld(s.GetWorld(), { 0, 0, 0 }, { 1, 0, 0 }, 100.0f, &hit);
        check(ok == 1, "raycast obb: +X ray hits rotated box");
        check(ok && std::fabs(hit.distance - (5.0f - 0.70710678f)) < 5e-3f,
              "raycast obb: corner distance ~= 4.293");
    }

    // ---- (13) Raycast: capsule (胴体 / 端球 / ミス) ----
    {
        Scene s;
        TransformSystem ts;
        GameObject cap = s.CreateGameObjectTracked("RayCap");
        cap.SetLocalPosition(0, 0, 10.0f);
        auto* col = cap.AddComponent<ColliderComponent>();
        col->shape = 2;
        col->radius = 1.0f;
        col->height = 4.0f; // 線分半長 = 1
        col->isTrigger = 0;
        s.GetWorld().ApplyStructuralChanges();
        ts.Update(s.GetWorld());
        MyeRaycastHit hit = {};
        int ok = RaycastWorld(s.GetWorld(), { 0, 0, 0 }, { 0, 0, 1 }, 100.0f, &hit);
        check(ok == 1 && std::fabs(hit.distance - 9.0f) < 1e-3f,
              "raycast capsule: cylinder hit at distance ~= 9.0");
        ok = RaycastWorld(s.GetWorld(), { 0, 1.5f, 0 }, { 0, 0, 1 }, 100.0f, &hit);
        check(ok == 1 && std::fabs(hit.distance - (10.0f - 0.8660254f)) < 1e-3f,
              "raycast capsule: upper cap hit at distance ~= 9.134");
        ok = RaycastWorld(s.GetWorld(), { 0, 3.5f, 0 }, { 0, 0, 1 }, 100.0f, &hit);
        check(ok == 0, "raycast capsule: miss above cap");
    }

    // ---- (14) 決定論: capsule / OBB / sphere 混在シーンの 240 tick 並走ハッシュ一致 ----
    {
        auto build = [](Scene& s) {
            MakeStaticBoxRot(s, "SlopeD", 0, 0, 0, 4.0f, 0.5f, 4.0f,
                             { 0, 0, 0.1305262f, 0.9914449f }); // Z 軸回り 15°
            MakeSphereBody(s, "S0", -0.5f, 2.0f, 0.2f, 0.4f);
            MakeCapsuleBody(s, "C0", 0.5f, 3.0f, -0.2f, 0.35f, 1.6f);
            GameObject b = MakeBox(s, "B0", 0.1f, 4.5f, 0, 0.5f, 0.5f, 0.5f, 0.3f);
            b.GetComponent<LocalTransform>()->rotation = { 0, 0.2588190f, 0, 0.9659258f }; // Y 30°
            s.GetWorld().ApplyStructuralChanges();
        };
        Scene sa, sb;
        build(sa);
        build(sb);
        bool det = true;
        uint64_t finalHash = 0;
        for (int i = 0; i < 240 && det; ++i) {
            phys.Update(sa.GetWorld(), kDt);
            phys.Update(sb.GetWorld(), kDt);
            const uint64_t ha = HashWorld(sa.GetWorld(), nullptr);
            const uint64_t hb = HashWorld(sb.GetWorld(), nullptr);
            if (ha != hb) {
                det = false;
                MYE_LOG_ERROR("  mixed-shape determinism diverged at tick %d: %016llX vs %016llX",
                              i, static_cast<unsigned long long>(ha),
                              static_cast<unsigned long long>(hb));
            }
            finalHash = ha;
        }
        check(det, "determinism: mixed-shape scene hash-identical for 240 ticks");
        // Debug/Release 間の一致確認用 (両構成で同値であること)
        MYE_LOG_INFO("  [phys] mixed shapes hash @240 = %016llX",
                     static_cast<unsigned long long>(finalHash));
    }

    if (failCount == 0) {
        MYE_LOG_INFO("==== Physics self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== Physics self test: %d FAILURE(S) ====", failCount);
    return false;
}

} // namespace mye
