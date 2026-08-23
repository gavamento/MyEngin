#include "Engine/Engine/PhysicsSelfTest.h"

#include <cmath>
#include <cstring>
#include <vector>

#include "Engine/Core/Components.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/CollisionSystem.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/Physics/MeshColliderLibrary.h"
#include "Engine/Engine/Physics/PhysMatLibrary.h" // M59a2: 材料解決の検証
#include "Engine/Engine/Physics/TerrainColliderLibrary.h" // M59i: 地形コライダー
#include "Engine/Engine/DebugDraw.h"
#include "Engine/Engine/Physics/AeroSampling.h"
#include "Engine/Engine/Physics/PhysicsDebugDraw.h"
#include "Engine/Engine/Physics/PhysicsSystem.h"
#include "Engine/Engine/Physics/Shapes.h"
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
    col->isTrigger = false;
    col->halfExtents = { hx, hy, hz };
    auto* rb = go.AddComponent<RigidbodyComponent>();
    rb->mass = 1.0f;
    rb->gravityScale = 1.0f;
    rb->restitution = restitution;
    return go;
}

// 静的な床 (ソリッド aabb コライダーのみ、Rigidbody 無し)
GameObject MakeGround(Scene& s, const char* name, float x, float y, float z, float hx, float hy,
                      float hz, bool isTrigger = false)
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
    col->isTrigger = false;
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
    col->isTrigger = false;
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
        MakeGround(s, "TrigGround", 0, -0.5f, 0, 5.0f, 0.5f, 5.0f, /*isTrigger=*/true);
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
        col->isTrigger = false;
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

    // ---- (11) SAT: 45° 回転 box (freezeRotation) が平床に角で立つ (姿勢維持の並進解決) ----
    {
        Scene s;
        MakeGround(s, "G11", 0, -0.5f, 0, 5.0f, 0.5f, 5.0f);
        GameObject box = MakeBox(s, "TiltBox", 0, 3.0f, 0, 0.5f, 0.5f, 0.5f);
        box.GetComponent<LocalTransform>()->rotation = { 0, 0, 0.3826834f, 0.9238795f };
        box.GetComponent<RigidbodyComponent>()->freezeRotation = true; // M28b: 回転を固定して幾何検証
        s.GetWorld().ApplyStructuralChanges();
        for (int i = 0; i < 180; ++i) {
            phys.Update(s.GetWorld(), kDt);
        }
        auto* lt = box.GetComponent<LocalTransform>();
        const float y = lt ? lt->position.y : -999.0f;
        // 対角半径 = (hx+hy)/sqrt(2) ~= 0.7071 (SAT 最小軸 = 床の +Y 面)
        check(y > 0.65f && y < 0.78f, "sat: 45deg box rests on edge at y~=0.707");
        // freezeRotation: 姿勢はビット単位で不変 (回転積分・角応答なし)
        const auto& q = lt->rotation;
        check(q.x == 0.0f && q.y == 0.0f && q.z == 0.3826834f && q.w == 0.9238795f,
              "freezeRotation: orientation bits unchanged");
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
        col->isTrigger = false;
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

    // ---- (15) 回転剛体: 40° に傾けた box が倒れて面で静止する (M28b) ----
    {
        Scene s;
        MakeGround(s, "G15", 0, -0.5f, 0, 5.0f, 0.5f, 5.0f);
        GameObject box = MakeBox(s, "Topple", 0, 0.9f, 0, 0.5f, 0.5f, 0.5f);
        // Z 軸回り 40° (バランス点 45° から外す → 重力トルクで倒れる)
        box.GetComponent<LocalTransform>()->rotation = { 0, 0, 0.3420201f, 0.9396926f };
        s.GetWorld().ApplyStructuralChanges();
        for (int i = 0; i < 300; ++i) {
            phys.Update(s.GetWorld(), kDt);
        }
        auto* lt = box.GetComponent<LocalTransform>();
        const float y = lt ? lt->position.y : -999.0f;
        check(y < 0.6f, "rotation: tilted box topples onto a face (y settles ~0.5)");
        // 40° から姿勢が大きく変わっている (回転が起きた証拠)
        const float qz = lt ? std::fabs(lt->rotation.z) : 1.0f;
        check(qz < 0.2f || qz > 0.55f, "rotation: orientation changed from initial 40deg");
    }

    // ---- (16) クーロン摩擦: 20° 斜面で μ 大 = 静止 / μ0 = 滑落 (freezeRotation で純滑り) ----
    {
        auto slopeTest = [&](float mu, float& outDx) {
            Scene s;
            GameObject slope = MakeStaticBoxRot(s, "Slope20", 0, 0, 0, 5.0f, 0.5f, 5.0f,
                                                { 0, 0, 0.1736482f, 0.9848078f }); // Z 20°
            slope.GetComponent<ColliderComponent>()->friction = mu;
            // 斜面上に置いた箱 (斜面法線方向に浮かせて落とす)
            GameObject box = MakeBox(s, "Slider", 0, 1.2f, 0, 0.3f, 0.3f, 0.3f);
            box.GetComponent<LocalTransform>()->rotation = { 0, 0, 0.1736482f, 0.9848078f };
            box.GetComponent<ColliderComponent>()->friction = mu;
            auto* rb = box.GetComponent<RigidbodyComponent>();
            rb->freezeRotation = true; // 転がり抜きの純粋な滑り摩擦を検証
            s.GetWorld().ApplyStructuralChanges();
            float x0 = 0;
            for (int i = 0; i < 150; ++i) {
                phys.Update(s.GetWorld(), kDt);
                if (i == 59) { // 着地・整定後の位置を基準にする
                    x0 = box.GetComponent<LocalTransform>()->position.x;
                }
            }
            outDx = box.GetComponent<LocalTransform>()->position.x - x0;
        };
        float dxHigh = 0, dxZero = 0;
        slopeTest(0.9f, dxHigh);
        slopeTest(0.0f, dxZero);
        // tan20° ~= 0.36 < μ=0.9 → 静止 / μ=0 → 滑落 (-X)
        check(std::fabs(dxHigh) < 0.02f, "friction: box holds still on 20deg slope (mu=0.9)");
        check(dxZero < -0.3f, "friction: box slides down 20deg slope (mu=0)");
    }

    // ---- (17) 転がり: 斜面の球が摩擦で角速度を得る (M28b) ----
    {
        Scene s;
        MakeStaticBoxRot(s, "Slope15", 0, 0, 0, 5.0f, 0.5f, 5.0f,
                         { 0, 0, 0.1305262f, 0.9914449f }); // Z 15°
        GameObject ball = MakeSphereBody(s, "Roller", 0, 1.5f, 0, 0.5f);
        s.GetWorld().ApplyStructuralChanges();
        for (int i = 0; i < 90; ++i) {
            phys.Update(s.GetWorld(), kDt);
        }
        auto* rb = ball.GetComponent<RigidbodyComponent>();
        // -X へ転がる球の回転軸は +Z (接触点の摩擦トルク)
        check(rb && rb->angularVelocity.z > 0.5f, "rolling: sphere gains +Z spin on slope");
    }

    // ---- (18) 3 段箱スタックが 600 tick 安定 (ドリフト < 5mm、常設回帰) ----
    {
        Scene s;
        MakeGround(s, "G18", 0, -0.5f, 0, 5.0f, 0.5f, 5.0f);
        GameObject b0 = MakeBox(s, "St0", 0, 0.5f, 0, 0.5f, 0.5f, 0.5f);
        GameObject b1 = MakeBox(s, "St1", 0, 1.5f, 0, 0.5f, 0.5f, 0.5f);
        GameObject b2 = MakeBox(s, "St2", 0, 2.5f, 0, 0.5f, 0.5f, 0.5f);
        s.GetWorld().ApplyStructuralChanges();
        for (int i = 0; i < 600; ++i) {
            phys.Update(s.GetWorld(), kDt);
        }
        auto drift = [](GameObject& go, float y0) {
            auto* lt = go.GetComponent<LocalTransform>();
            if (!lt) {
                return 999.0f;
            }
            const float dx = std::fabs(lt->position.x);
            const float dz = std::fabs(lt->position.z);
            const float dy = std::fabs(lt->position.y - y0);
            return std::max(dx, std::max(dz, dy));
        };
        MYE_LOG_INFO("  [phys] stack drift: b0=%.4f b1=%.4f b2=%.4f", drift(b0, 0.5f),
                     drift(b1, 1.5f), drift(b2, 2.5f));
        check(drift(b0, 0.5f) < 0.005f && drift(b1, 1.5f) < 0.005f && drift(b2, 2.5f) < 0.005f,
              "stack: 3-box tower drift < 5mm after 600 ticks");
        const auto& q2 = b2.GetComponent<LocalTransform>()->rotation;
        check(std::fabs(q2.x) < 0.05f && std::fabs(q2.y) < 0.05f && std::fabs(q2.z) < 0.05f,
              "stack: top box stays upright (no spin accumulation)");
    }

    // ---- (19) AddTorque (ApplyTorqueWorld): ω += I⁻¹τdt が積分され回転する ----
    {
        Scene s;
        GameObject box = MakeBox(s, "Spinner", 0, 5.0f, 0, 0.5f, 0.5f, 0.5f);
        box.GetComponent<RigidbodyComponent>()->gravityScale = 0.0f;
        s.GetWorld().ApplyStructuralChanges();
        for (int i = 0; i < 60; ++i) {
            ApplyTorqueWorld(s.GetWorld(), box.Id(), { 0, 2.0f, 0 }, kDt);
            phys.Update(s.GetWorld(), kDt);
        }
        auto* rb = box.GetComponent<RigidbodyComponent>();
        auto* lt = box.GetComponent<LocalTransform>();
        check(rb && rb->angularVelocity.y > 1.0f, "torque: +Y torque accumulates +Y spin");
        check(lt && std::fabs(lt->rotation.y) > 0.05f, "torque: orientation integrates over ticks");
        // freezeRotation では拒否される
        rb->freezeRotation = true;
        const int rejected = ApplyTorqueWorld(s.GetWorld(), box.Id(), { 0, 2.0f, 0 }, kDt);
        check(rejected == 0, "torque: rejected when freezeRotation=1");
    }

    // ---- (20) OnCollision イベント: 着地 Enter 1 回 → 静止 Stay → 離脱 Exit (M28c) ----
    {
        Scene s;
        CollisionSystem cols;
        TransformSystem ts;
        MakeGround(s, "G20", 0, -0.5f, 0, 5.0f, 0.5f, 5.0f);
        GameObject box = MakeBox(s, "Faller20", 0, 2.0f, 0, 0.5f, 0.5f, 0.5f);
        s.GetWorld().ApplyStructuralChanges();
        std::vector<SolidContact> contacts;
        int enterCount = 0, stayCount = 0, exitCount = 0;
        int enterTick = -1, exitTick = -1;
        float enterNy = 0;
        for (int i = 0; i < 220; ++i) {
            phys.Update(s.GetWorld(), kDt, &contacts);
            ts.Update(s.GetWorld());
            cols.Update(s.GetWorld(), nullptr, nullptr, &contacts);
            if (!cols.LastCollisionEnter().empty()) {
                ++enterCount;
                if (enterTick < 0) {
                    enterTick = i;
                    enterNy = contacts.empty() ? 0.0f : contacts[0].ny;
                }
            }
            if (!cols.LastCollisionStay().empty()) {
                ++stayCount;
            }
            if (!cols.LastCollisionExit().empty()) {
                ++exitCount;
                exitTick = i;
            }
            if (i == 180) {
                box.GetComponent<RigidbodyComponent>()->velocity.y = 5.0f; // 上向きに離脱
            }
        }
        check(enterCount == 1 && enterTick > 0, "collision events: enter fires exactly once");
        check(stayCount > 100, "collision events: stay fires every resting tick");
        check(exitCount == 1 && exitTick > 180, "collision events: exit fires after leaving");
        // SolidContact.normal は大 index→小 index (小 = 先に作った床)。箱→床 = -Y
        check(enterNy < -0.99f, "collision events: contact normal points toward lower index");
    }

    // ---- (21) トリガーの役割分離 (M28c 是正): ソリッド同士は OnTrigger を出さない ----
    {
        Scene s;
        CollisionSystem cols;
        TransformSystem ts;
        MakeGround(s, "SolidA", 0, 0, 0, 1.0f, 1.0f, 1.0f);
        MakeGround(s, "SolidB", 0.5f, 0, 0, 1.0f, 1.0f, 1.0f);            // ソリッド同士で重なる
        MakeGround(s, "Trig", 10.0f, 0, 0, 1.0f, 1.0f, 1.0f, /*isTrigger=*/true);
        MakeGround(s, "SolidC", 10.5f, 0, 0, 1.0f, 1.0f, 1.0f);           // トリガーと重なる
        s.GetWorld().ApplyStructuralChanges();
        ts.Update(s.GetWorld());
        cols.Update(s.GetWorld(), nullptr, nullptr, nullptr);
        check(cols.LastTriggerEnter().size() == 1,
              "trigger filter: only the trigger-involving pair fires (solid-solid excluded)");
    }

    // ---- (22) OverlapSphere / OverlapBox: ヒット数・index 昇順・切り捨て時の総数 ----
    {
        Scene s;
        TransformSystem ts;
        GameObject a = MakeGround(s, "OvA", 0, 0, 0, 0.5f, 0.5f, 0.5f);
        GameObject b = MakeGround(s, "OvB", 1.5f, 0, 0, 0.5f, 0.5f, 0.5f);
        GameObject c = MakeGround(s, "OvC", 10.0f, 0, 0, 0.5f, 0.5f, 0.5f);
        s.GetWorld().ApplyStructuralChanges();
        ts.Update(s.GetWorld());
        MyeEntityId hits[8] = {};
        int n = OverlapSphereWorld(s.GetWorld(), { 0.75f, 0, 0 }, 1.0f, hits, 8);
        check(n == 2, "overlap sphere: two nearby boxes hit");
        check(n == 2 && hits[0].index == a.Id().index && hits[1].index == b.Id().index,
              "overlap sphere: results are index-ascending");
        n = OverlapSphereWorld(s.GetWorld(), { 0.75f, 0, 0 }, 1.0f, hits, 1);
        check(n == 2, "overlap sphere: truncated call still returns total count");
        n = OverlapBoxWorld(s.GetWorld(), { 10.0f, 0, 0 }, { 1, 1, 1 }, { 0, 0, 0, 1 }, hits, 8);
        check(n == 1 && hits[0].index == c.Id().index, "overlap box: distant box found");
    }

    // ---- (23) SphereCast: 対球の解析解 / 45° OBB の保守的前進 / ミス ----
    {
        Scene s;
        TransformSystem ts;
        GameObject sph = s.CreateGameObjectTracked("ScSphere");
        sph.SetLocalPosition(0, 0, 10.0f);
        auto* col = sph.AddComponent<ColliderComponent>();
        col->shape = 0;
        col->radius = 1.0f;
        col->isTrigger = false;
        MakeStaticBoxRot(s, "ScObb", 5.0f, 0, 0, 0.5f, 0.5f, 0.5f,
                         { 0, 0.3826834f, 0, 0.9238795f }); // Y 軸回り 45°
        s.GetWorld().ApplyStructuralChanges();
        ts.Update(s.GetWorld());
        MyeRaycastHit hit = {};
        int ok = SphereCastWorld(s.GetWorld(), { 0, 0, 0 }, { 0, 0, 1 }, 0.5f, 100.0f, &hit);
        check(ok == 1 && std::fabs(hit.distance - 8.5f) < 1e-3f,
              "spherecast: sphere target analytic hit (10 - 1 - 0.5)");
        // 45° OBB の角 (x = 5 - 0.7071) に半径 0.5 の球が触れる距離
        ok = SphereCastWorld(s.GetWorld(), { 0, 0, 0 }, { 1, 0, 0 }, 0.5f, 100.0f, &hit);
        check(ok == 1 && std::fabs(hit.distance - (5.0f - 0.70710678f - 0.5f)) < 5e-3f,
              "spherecast: obb conservative advancement hit");
        ok = SphereCastWorld(s.GetWorld(), { 0, 50.0f, 0 }, { 1, 0, 0 }, 0.5f, 100.0f, &hit);
        check(ok == 0, "spherecast: miss well above scene");
    }

    // ---- (24) ブロードフェーズ等価性: sort&sweep と総当たりで 240 tick ハッシュ完全一致 ----
    {
        auto build = [](Scene& s) {
            MakeGround(s, "G24", 0, -0.5f, 0, 12.0f, 0.5f, 12.0f);
            // 7x7 の球グリッド + 高さ違い (大量ペアで候補列挙を励起する)
            for (int gz = 0; gz < 7; ++gz) {
                for (int gx = 0; gx < 7; ++gx) {
                    MakeSphereBody(s, "S", -3.0f + gx * 1.0f, 1.0f + ((gx + gz) % 3) * 1.1f,
                                   -3.0f + gz * 1.0f, 0.45f);
                }
            }
            MakeCapsuleBody(s, "C", 0.2f, 6.0f, 0.2f, 0.4f, 1.8f);
            s.GetWorld().ApplyStructuralChanges();
        };
        Scene sa, sb;
        build(sa);
        build(sb);
        bool same = true;
        for (int i = 0; i < 240 && same; ++i) {
            PhysicsSystem::sDisableBroadphaseForTest = false;
            phys.Update(sa.GetWorld(), kDt);
            PhysicsSystem::sDisableBroadphaseForTest = true;
            phys.Update(sb.GetWorld(), kDt);
            PhysicsSystem::sDisableBroadphaseForTest = false;
            if (HashWorld(sa.GetWorld(), nullptr) != HashWorld(sb.GetWorld(), nullptr)) {
                same = false;
                MYE_LOG_ERROR("  broadphase equivalence diverged at tick %d", i);
            }
        }
        check(same, "broadphase: sweep&prune bit-identical to brute force (50 bodies, 240 ticks)");
    }

    // ---- (25) 親子階層: 親オフセット下の子剛体 + 親付き静的コライダー (バグ修正回帰) ----
    {
        Scene s;
        GameObject parent = s.CreateGameObjectTracked("P25");
        parent.SetLocalPosition(5.0f, 0, 0);
        GameObject child = MakeBox(s, "C25", 0, 3.0f, 0, 0.5f, 0.5f, 0.5f); // ローカル (0,3,0)
        s.GetWorld().SetParent(child.Id(), parent.Id());
        // 親付き静的コライダー: 親 (0,-0.5,0) + 床ローカル (0,0,0) → ワールド上面 y=0
        GameObject gp = s.CreateGameObjectTracked("GP25");
        gp.SetLocalPosition(0, -0.5f, 0);
        GameObject ground = MakeGround(s, "G25", 0, 0, 0, 20.0f, 0.5f, 20.0f);
        s.GetWorld().SetParent(ground.Id(), gp.Id());
        s.GetWorld().ApplyStructuralChanges();
        for (int i = 0; i < 180; ++i) {
            phys.Update(s.GetWorld(), kDt);
        }
        auto* lt = child.GetComponent<LocalTransform>();
        // 子はローカル (0, 0.5, 0) に接地 = ワールド (5, 0.5, 0)。親付き床が正しく y=0 上面
        check(lt && std::fabs(lt->position.x) < 0.01f, "hierarchy: child keeps local x under parent");
        check(lt && lt->position.y > 0.45f && lt->position.y < 0.55f,
              "hierarchy: child rests at local y~=0.5 on parented static ground");
    }

    // ---- (26) 親の回転を考慮した子剛体の書き戻し + scalar 合成と TransformSystem の一致 ----
    {
        Scene s;
        TransformSystem ts;
        GameObject parent = s.CreateGameObjectTracked("P26");
        parent.GetComponent<LocalTransform>()->rotation = { 0, 0.7071068f, 0, 0.7071068f }; // Y 90°
        GameObject child = MakeSphereBody(s, "C26", 2.0f, 3.0f, 0, 0.5f); // ローカル (2,3,0)
        s.GetWorld().SetParent(child.Id(), parent.Id());
        MakeGround(s, "G26", 0, -0.5f, 0, 20.0f, 0.5f, 20.0f);
        s.GetWorld().ApplyStructuralChanges();
        for (int i = 0; i < 180; ++i) {
            phys.Update(s.GetWorld(), kDt);
        }
        auto* lt = child.GetComponent<LocalTransform>();
        // ワールド (0,3,-2) から落下 → ワールド (0,0.5,-2) = ローカル (2, 0.5, 0)
        check(lt && std::fabs(lt->position.x - 2.0f) < 0.01f && std::fabs(lt->position.z) < 0.01f,
              "hierarchy: world fall maps back to local through rotated parent");
        check(lt && lt->position.y > 0.45f && lt->position.y < 0.55f,
              "hierarchy: child sphere rests at local y~=0.5");
        // scalar 親チェーン合成と TransformSystem (XMMATRIX) の一致検証
        ts.Update(s.GetWorld());
        const auto* wm = child.GetComponent<WorldMatrixComponent>();
        check(wm && std::fabs(wm->value._41 - 0.0f) < 1e-3f
                  && std::fabs(wm->value._42 - lt->position.y) < 1e-3f
                  && std::fabs(wm->value._43 - (-2.0f)) < 1e-3f,
              "hierarchy: scalar compose matches TransformSystem world matrix");
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

    // ---- (15) ConstantForce: 力 = m·g の上向き定常力でホバリング (M29a) ----
    {
        Scene s;
        GameObject box = MakeBox(s, "Hover", 0, 5.0f, 0, 0.5f, 0.5f, 0.5f);
        auto* rb = box.GetComponent<RigidbodyComponent>();
        rb->mass = 2.0f;
        auto* cf = box.AddComponent<ConstantForceComponent>();
        // 2·9.81 と 1/m=0.5 の積は float で正確に 9.81 → 重力 (−9.81·dt) と毎 tick 完全相殺
        cf->force = { 0.0f, 2.0f * 9.81f, 0.0f };
        s.GetWorld().ApplyStructuralChanges();
        for (int i = 0; i < 120; ++i) {
            phys.Update(s.GetWorld(), kDt);
        }
        auto* lt = box.GetComponent<LocalTransform>();
        check(lt && std::fabs(lt->position.y - 5.0f) < 1e-4f
                  && std::fabs(rb->velocity.y) < 1e-5f,
              "constant force: upward F=m*g hovers exactly against gravity");
    }

    // ---- (16) ConstantForce: トルクによるスピンアップ + relative の局所軸適用 ----
    {
        Scene s;
        GameObject spin = MakeBox(s, "Spinner", 0, 5.0f, 0, 0.5f, 0.5f, 0.5f);
        spin.GetComponent<RigidbodyComponent>()->gravityScale = 0.0f;
        auto* tq = spin.AddComponent<ConstantForceComponent>();
        tq->torque = { 0.0f, 10.0f, 0.0f }; // world Y 軸トルク
        // relative=1 の力: エンティティを Y 軸 90° 回転させ、ローカル +Z の力 → ワールド +X 加速
        GameObject thr = MakeBox(s, "Thruster", 5.0f, 5.0f, 0, 0.5f, 0.5f, 0.5f);
        thr.GetComponent<RigidbodyComponent>()->gravityScale = 0.0f;
        thr.GetComponent<LocalTransform>()->rotation = { 0, 0.7071068f, 0, 0.7071068f }; // Y 90°
        auto* lf = thr.AddComponent<ConstantForceComponent>();
        lf->force = { 0.0f, 0.0f, 4.0f };
        lf->relative = 1;
        s.GetWorld().ApplyStructuralChanges();
        for (int i = 0; i < 60; ++i) {
            phys.Update(s.GetWorld(), kDt);
        }
        const auto* srb = spin.GetComponent<RigidbodyComponent>();
        check(srb && srb->angularVelocity.y > 0.5f,
              "constant force: world torque spins up angular velocity");
        const auto* trb = thr.GetComponent<RigidbodyComponent>();
        // ローカル +Z を Y90° 回転 → ワールド +X。X 速度が支配的で Z はほぼ 0
        check(trb && trb->velocity.x > 1.0f && std::fabs(trb->velocity.z) < 0.1f,
              "constant force: relative force follows entity local axes");
    }

    // ---- (17) SpringJoint: 静的アンカー振り子が平衡長 (restLength + m·g/k) に収束 ----
    {
        Scene s;
        GameObject anchor = s.CreateGameObjectTracked("Anchor");
        anchor.SetLocalPosition(0, 5.0f, 0);
        GameObject bob = MakeBox(s, "Bob", 0, 4.0f, 0, 0.3f, 0.3f, 0.3f);
        auto* sj = bob.AddComponent<SpringJointComponent>();
        sj->connectedEntity = anchor.Id();
        sj->restLength = 2.0f;
        sj->stiffness = 50.0f;
        sj->damping = 5.0f;
        s.GetWorld().ApplyStructuralChanges();
        for (int i = 0; i < 300; ++i) {
            phys.Update(s.GetWorld(), kDt);
        }
        const auto* lt = bob.GetComponent<LocalTransform>();
        const auto* rb = bob.GetComponent<RigidbodyComponent>();
        // 平衡: k·stretch = m·g → 長さ ≈ 2 + 9.81/50 ≈ 2.196 (±5%)
        const float dist = lt ? (5.0f - lt->position.y) : 0.0f;
        const float expect = 2.0f + 9.81f / 50.0f;
        check(lt && rb && std::fabs(dist - expect) < expect * 0.05f
                  && std::fabs(rb->velocity.y) < 0.05f,
              "spring joint: static-anchor pendulum settles at equilibrium length");
    }

    // ---- (18) SpringJoint: 2 体ばねの振動が発散せず運動量を保存する ----
    {
        Scene s;
        // コライダー無しの剛体 (接触を混ぜない純粋なばね力学の検証)
        GameObject a = s.CreateGameObjectTracked("SA");
        a.SetLocalPosition(-2.0f, 5.0f, 0);
        a.AddComponent<RigidbodyComponent>()->gravityScale = 0.0f;
        GameObject b = s.CreateGameObjectTracked("SB");
        b.SetLocalPosition(2.0f, 5.0f, 0);
        b.AddComponent<RigidbodyComponent>()->gravityScale = 0.0f;
        auto* sj = a.AddComponent<SpringJointComponent>();
        sj->connectedEntity = b.Id();
        sj->restLength = 2.0f;
        sj->stiffness = 200.0f;
        sj->damping = 1.0f;
        s.GetWorld().ApplyStructuralChanges();
        bool bounded = true;
        float comDrift = 0.0f;
        for (int i = 0; i < 300 && bounded; ++i) {
            phys.Update(s.GetWorld(), kDt);
            const auto* la = a.GetComponent<LocalTransform>();
            const auto* lb = b.GetComponent<LocalTransform>();
            const float dist = std::fabs(la->position.x - lb->position.x);
            if (!(dist < 4.8f) || std::isnan(dist)) {
                bounded = false; // 初期伸び (dist=4, stretch=2) を超えて発散したら失敗
            }
            const float com = (la->position.x + lb->position.x) * 0.5f;
            if (std::fabs(com) > comDrift) {
                comDrift = std::fabs(com);
            }
        }
        check(bounded, "spring joint: two-body oscillation stays bounded");
        check(comDrift < 1e-3f, "spring joint: two-body impulses conserve momentum (COM fixed)");
    }

    // ---- (19) 決定論: ばね + 定常力シーンの 240 tick 並走ハッシュ一致 (M29a) ----
    {
        auto build = [](Scene& s) {
            MakeGround(s, "G", 0, -0.5f, 0, 6.0f, 0.5f, 6.0f);
            GameObject anchor = s.CreateGameObjectTracked("Anchor");
            anchor.SetLocalPosition(0, 6.0f, 0);
            GameObject bob = MakeBox(s, "Bob", 0.8f, 4.5f, 0.2f, 0.3f, 0.3f, 0.3f);
            auto* sj = bob.AddComponent<SpringJointComponent>();
            sj->connectedEntity = anchor.Id();
            sj->restLength = 1.5f;
            sj->stiffness = 60.0f;
            sj->damping = 2.0f;
            GameObject spin = MakeBox(s, "Spin", -2.0f, 3.0f, 0, 0.5f, 0.5f, 0.5f);
            auto* cf = spin.AddComponent<ConstantForceComponent>();
            cf->force = { 0.0f, 9.0f, 0.0f };
            cf->torque = { 0.0f, 3.0f, 1.0f };
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
                MYE_LOG_ERROR("  spring/force determinism diverged at tick %d: %016llX vs %016llX",
                              i, static_cast<unsigned long long>(ha),
                              static_cast<unsigned long long>(hb));
            }
            finalHash = ha;
        }
        check(det, "determinism: spring/force scene hash-identical for 240 ticks");
        // Debug/Release 間の一致確認用 (両構成で同値であること)
        MYE_LOG_INFO("  [phys] spring/force scene hash @240 = %016llX",
                     static_cast<unsigned long long>(finalHash));
    }

    // ---- (20) CharacterController: 着地・歩行・壁ブロック (M29b) ----
    {
        Scene s;
        MakeGround(s, "G", 0, -0.5f, 0, 8.0f, 0.5f, 8.0f);
        MakeGround(s, "Wall", 3.0f, 2.0f, 0, 0.5f, 2.0f, 2.0f); // 内側面 x=2.5
        GameObject ch = s.CreateGameObjectTracked("Char");
        ch.SetLocalPosition(0, 2.0f, 0);
        ch.AddComponent<CharacterControllerComponent>(); // 既定 r=0.3 h=1.8 → 接地中心 y=0.9
        s.GetWorld().ApplyStructuralChanges();
        auto* cc = ch.GetComponent<CharacterControllerComponent>();
        auto* lt = ch.GetComponent<LocalTransform>();
        for (int i = 0; i < 120; ++i) {
            phys.Update(s.GetWorld(), kDt);
        }
        check(std::fabs(lt->position.y - 0.9f) < 0.02f && cc->isGrounded == 1,
              "character: lands on flat ground at capsule half height");
        cc->moveInput.x = 1.0f;
        for (int i = 0; i < 60; ++i) {
            phys.Update(s.GetWorld(), kDt);
        }
        check(std::fabs(lt->position.x - 1.0f) < 0.02f,
              "character: walks 1m in 60 ticks at 1 m/s");
        float maxX = lt->position.x;
        for (int i = 0; i < 180; ++i) {
            phys.Update(s.GetWorld(), kDt);
            if (lt->position.x > maxX) {
                maxX = lt->position.x;
            }
        }
        // 壁面 2.5 − 半径 0.3 = 2.2 で停止 (貫通しない)
        check(std::fabs(lt->position.x - 2.2f) < 0.05f && maxX < 2.21f,
              "character: blocked by wall at face minus radius");
    }

    // ---- (21) CharacterController: 30° 斜面は登れる / 60° 斜面は y 非増加 ----
    {
        Scene s;
        // 30° 斜面 (Z 軸回り +30° → +X 方向が登り)
        MakeStaticBoxRot(s, "Slope30", 0, 0, 0, 6.0f, 0.5f, 4.0f,
                         { 0, 0, 0.2588190f, 0.9659258f });
        GameObject ch = s.CreateGameObjectTracked("Climber");
        ch.SetLocalPosition(-2.0f, 3.0f, 0);
        ch.AddComponent<CharacterControllerComponent>();
        s.GetWorld().ApplyStructuralChanges();
        auto* cc = ch.GetComponent<CharacterControllerComponent>();
        auto* lt = ch.GetComponent<LocalTransform>();
        for (int i = 0; i < 90; ++i) {
            phys.Update(s.GetWorld(), kDt); // 斜面に着地
        }
        const float y0 = lt->position.y;
        const float x0 = lt->position.x;
        cc->moveInput.x = 1.5f;
        for (int i = 0; i < 120; ++i) {
            phys.Update(s.GetWorld(), kDt);
        }
        check(lt->position.x - x0 > 1.0f && lt->position.y - y0 > 0.4f && cc->isGrounded == 1,
              "character: climbs 30 deg slope (slopeLimit 45)");
    }
    {
        Scene s;
        // 60° 斜面 (slopeLimit 45 超 = 登れない急斜面)。押し続けても y が増えないこと
        MakeStaticBoxRot(s, "Slope60", 0, 0, 0, 6.0f, 0.5f, 4.0f,
                         { 0, 0, 0.5f, 0.8660254f });
        GameObject ch = s.CreateGameObjectTracked("Pusher");
        ch.SetLocalPosition(-1.0f, 4.0f, 0);
        ch.AddComponent<CharacterControllerComponent>();
        s.GetWorld().ApplyStructuralChanges();
        auto* cc = ch.GetComponent<CharacterControllerComponent>();
        auto* lt = ch.GetComponent<LocalTransform>();
        for (int i = 0; i < 60; ++i) {
            phys.Update(s.GetWorld(), kDt); // 接触まで落下
        }
        const float ySettle = lt->position.y;
        cc->moveInput.x = 1.5f; // 登り方向へ押し続ける
        bool noClimb = true;
        for (int i = 0; i < 180; ++i) {
            phys.Update(s.GetWorld(), kDt);
            if (lt->position.y > ySettle + 0.05f) {
                noClimb = false;
            }
        }
        check(noClimb, "character: cannot climb 60 deg slope (y never increases)");
    }

    // ---- (22) CharacterController: 接地ジャンプの頂点 ≈ v²/2g、空中ジャンプは無効 ----
    {
        Scene s;
        MakeGround(s, "G", 0, -0.5f, 0, 8.0f, 0.5f, 8.0f);
        GameObject ch = s.CreateGameObjectTracked("Jumper");
        ch.SetLocalPosition(0, 2.0f, 0);
        ch.AddComponent<CharacterControllerComponent>();
        s.GetWorld().ApplyStructuralChanges();
        auto* cc = ch.GetComponent<CharacterControllerComponent>();
        auto* lt = ch.GetComponent<LocalTransform>();
        for (int i = 0; i < 120; ++i) {
            phys.Update(s.GetWorld(), kDt);
        }
        const float y0 = lt->position.y;
        cc->jumpSpeed = 5.0f;
        float maxY = y0;
        bool airJumpBlocked = true;
        float prevVy = 0.0f;
        for (int i = 0; i < 150; ++i) {
            phys.Update(s.GetWorld(), kDt);
            if (lt->position.y > maxY) {
                maxY = lt->position.y;
            }
            if (i == 30) { // 滞空中 (vy ≈ 0.1) に空中ジャンプを試みる
                if (cc->isGrounded != 0) {
                    airJumpBlocked = false; // 想定外に接地している = テスト前提が崩れている
                }
                prevVy = cc->velocity.y;
                cc->jumpSpeed = 5.0f;
            }
            if (i == 31) {
                // 空中では発火しない: vy は重力で下がるだけ。ただし jumpSpeed は消費される
                if (cc->velocity.y > prevVy || cc->jumpSpeed != 0.0f) {
                    airJumpBlocked = false;
                }
            }
        }
        const float expectApex = y0 + 25.0f / (2.0f * 9.81f);
        check(std::fabs(maxY - expectApex) < 0.15f,
              "character: grounded jump apex matches v^2/2g");
        check(airJumpBlocked, "character: air jump does not fire (jumpSpeed consumed)");
    }

    // ---- (23) 決定論: CC + 剛体混在シーンの 240 tick 並走ハッシュ一致 (M29b) ----
    {
        auto build = [](Scene& s) {
            MakeGround(s, "G", 0, -0.5f, 0, 8.0f, 0.5f, 8.0f);
            MakeStaticBoxRot(s, "Ramp", 4.0f, 0.5f, 0, 3.0f, 0.5f, 3.0f,
                             { 0, 0, 0.1305262f, 0.9914449f });
            GameObject c1 = s.CreateGameObjectTracked("C1");
            c1.SetLocalPosition(-2.0f, 2.0f, 0.5f);
            c1.AddComponent<CharacterControllerComponent>()->moveInput = { 1.2f, 0, 0.3f };
            GameObject c2 = s.CreateGameObjectTracked("C2");
            c2.SetLocalPosition(2.0f, 2.0f, -1.0f);
            c2.AddComponent<CharacterControllerComponent>()->moveInput = { -0.8f, 0, 0 };
            MakeBox(s, "B0", 0.0f, 5.0f, 0, 0.5f, 0.5f, 0.5f, 0.2f); // CC 進路に落ちる剛体
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
                MYE_LOG_ERROR("  character determinism diverged at tick %d: %016llX vs %016llX",
                              i, static_cast<unsigned long long>(ha),
                              static_cast<unsigned long long>(hb));
            }
            finalHash = ha;
        }
        check(det, "determinism: character scene hash-identical for 240 ticks");
        // Debug/Release 間の一致確認用 (両構成で同値であること)
        MYE_LOG_INFO("  [phys] character scene hash @240 = %016llX",
                     static_cast<unsigned long long>(finalHash));
    }

    // ---- (L1) レイヤー/マスク: ソルバペア除外 (M36a) ----
    {
        // 床 = layer0。箱 A = 既定 (全マッチ、床に乗る)。箱 B = mask から layer0 を除外 → 素通り落下
        Scene s;
        MakeGround(s, "LGround", 0, -0.5f, 0, 10.0f, 0.5f, 10.0f);
        GameObject a = MakeBox(s, "LBoxA", -2.0f, 2.0f, 0, 0.5f, 0.5f, 0.5f);
        GameObject b = MakeBox(s, "LBoxB", 2.0f, 2.0f, 0, 0.5f, 0.5f, 0.5f);
        b.GetComponent<ColliderComponent>()->mask = ~1u; // layer0 (床) と衝突しない
        s.GetWorld().ApplyStructuralChanges();
        for (int i = 0; i < 180; ++i) {
            phys.Update(s.GetWorld(), kDt);
        }
        const float ay = a.GetComponent<LocalTransform>()->position.y;
        const float by = b.GetComponent<LocalTransform>()->position.y;
        check(std::fabs(ay - 0.5f) < 0.05f, "layer: default mask box rests on ground");
        check(by < -5.0f, "layer: masked-out box falls through ground");
    }

    // ---- (L2) レイヤー/マスク: 片側除外でも双方向で不衝突 (対称性) ----
    {
        Scene s;
        GameObject g = MakeGround(s, "LG2", 0, -0.5f, 0, 10.0f, 0.5f, 10.0f);
        g.GetComponent<ColliderComponent>()->layer = 3;
        g.GetComponent<ColliderComponent>()->mask = ~2u; // layer1 を拒否
        GameObject b = MakeBox(s, "LB2", 0, 2.0f, 0, 0.5f, 0.5f, 0.5f);
        b.GetComponent<ColliderComponent>()->layer = 1; // mask は既定 (layer3 を許可) — だが床側が拒否
        s.GetWorld().ApplyStructuralChanges();
        for (int i = 0; i < 180; ++i) {
            phys.Update(s.GetWorld(), kDt);
        }
        check(b.GetComponent<LocalTransform>()->position.y < -5.0f,
              "layer: one-sided rejection is bidirectional (falls through)");
    }

    // ---- (L3) レイヤー/マスク: トリガーイベント除外 ----
    {
        Scene s;
        TransformSystem ts;
        CollisionSystem cs;
        GameObject trig = MakeGround(s, "LTrig", 0, 0, 0, 1.0f, 1.0f, 1.0f, true); // isTrigger
        trig.GetComponent<ColliderComponent>()->layer = 2;
        GameObject inA = MakeGround(s, "LInA", 0.5f, 0, 0, 0.5f, 0.5f, 0.5f, 0);
        GameObject inB = MakeGround(s, "LInB", -0.5f, 0, 0, 0.5f, 0.5f, 0.5f, 0);
        inB.GetComponent<ColliderComponent>()->mask = ~4u; // layer2 (トリガー) を拒否
        s.GetWorld().ApplyStructuralChanges();
        ts.Update(s.GetWorld());
        cs.Update(s.GetWorld(), nullptr, nullptr, nullptr);
        int hitA = 0, hitB = 0;
        for (const uint64_t key : cs.LastTriggerEnter()) {
            const uint32_t i0 = static_cast<uint32_t>(key >> 32);
            const uint32_t i1 = static_cast<uint32_t>(key & 0xFFFFFFFFu);
            if (i0 == inA.Id().index || i1 == inA.Id().index) { ++hitA; }
            if (i0 == inB.Id().index || i1 == inB.Id().index) { ++hitB; }
        }
        check(hitA == 1, "layer: unmasked overlap fires trigger enter");
        check(hitB == 0, "layer: masked-out overlap fires no trigger");
    }

    // ---- (L4) レイヤー/マスク: Raycast / Overlap のクエリマスク ----
    {
        Scene s;
        TransformSystem ts;
        GameObject near5 = MakeGround(s, "LRayNear", 5.0f, 0, 0, 0.5f, 0.5f, 0.5f);
        near5.GetComponent<ColliderComponent>()->layer = 4;
        GameObject far10 = MakeGround(s, "LRayFar", 10.0f, 0, 0, 0.5f, 0.5f, 0.5f);
        far10.GetComponent<ColliderComponent>()->layer = 5;
        s.GetWorld().ApplyStructuralChanges();
        ts.Update(s.GetWorld());
        MyeRaycastHit hit = {};
        // 既定マスク: 近い layer4 がヒット
        check(RaycastWorld(s.GetWorld(), { 0, 0, 0 }, { 1, 0, 0 }, 100.0f, &hit) == 1
                  && hit.entity.index == near5.Id().index,
              "layer: raycast default mask hits nearest");
        // layer4 を除外: 奥の layer5 がヒット
        check(RaycastWorld(s.GetWorld(), { 0, 0, 0 }, { 1, 0, 0 }, 100.0f, &hit, ~(1u << 4)) == 1
                  && hit.entity.index == far10.Id().index,
              "layer: raycast mask skips excluded layer");
        // Overlap: layer5 のみ許可 → far だけ
        MyeEntityId ents[8] = {};
        const int n = OverlapSphereWorld(s.GetWorld(), { 7.5f, 0, 0 }, 5.0f, ents, 8, 1u << 5);
        check(n == 1 && ents[0].index == far10.Id().index,
              "layer: overlap mask filters collection");
    }

    // ---- (M41-1) 静的メッシュ: 形状ペアの単体検証 (y=0 の 10x10 クアッド = 三角形 2 枚) ----
    {
        MeshColliderData quad;
        BuildMeshColliderData(
            { { -5, 0, -5 }, { 5, 0, -5 }, { 5, 0, 5 }, { -5, 0, 5 } },
            { 0, 1, 2, 0, 2, 3 }, quad);
        check(quad.TriCount() == 2 && !quad.nodes.empty(), "mesh: BVH built (2 tris)");

        ShapePose mp;
        mp.shape = 3;
        mp.meshData = &quad;

        // 球: 中心 (0,0.3,0)、r=0.5 → 貫通 0.2、法線 = メッシュ→球 = +Y
        ShapePose sp;
        sp.shape = 0;
        sp.py = 0.3f;
        sp.radius = 0.5f;
        float nx = 0, ny = 0, nz = 0, d = 0;
        check(shapes::Collide(sp, mp, nx, ny, nz, d) && ny > 0.99f && std::fabs(d - 0.2f) < 1e-4f,
              "mesh: sphere-tri contact (depth/normal)");
        check(shapes::Collide(mp, sp, nx, ny, nz, d) && ny < -0.99f,
              "mesh: normal flips when mesh is 'a' (b->a convention)");
        sp.py = 0.6f;
        check(!shapes::Collide(sp, mp, nx, ny, nz, d), "mesh: no contact when separated");

        // カプセル: 中心 (1,0.8,1)、halfSeg=0.5、r=0.4 → 底 -0.1 → 貫通 0.1
        ShapePose cp;
        cp.shape = 2;
        cp.px = 1.0f;
        cp.py = 0.8f;
        cp.pz = 1.0f;
        cp.radius = 0.4f;
        cp.halfSeg = 0.5f;
        check(shapes::Collide(cp, mp, nx, ny, nz, d) && ny > 0.99f && std::fabs(d - 0.1f) < 1e-4f,
              "mesh: capsule-tri contact");

        // ボックス: 中心 (0,0.4,0)、h=0.5 → 底 -0.1 → 貫通 0.1、マニフォールドは複数点
        ShapePose bp;
        bp.shape = 1;
        bp.py = 0.4f;
        bp.hx = bp.hy = bp.hz = 0.5f;
        shapes::Manifold m;
        check(shapes::CollideManifold(bp, mp, m) && m.count >= 1 && m.ny > 0.99f
                  && std::fabs(m.pts[0].depth - 0.1f) < 1e-3f,
              "mesh: box-tri manifold (SAT depth/normal)");

        // レイ: (2,3,2) から -Y → t=3、法線 +Y。クアッド外は外れる
        float t = 0;
        check(shapes::Raycast(mp, 2, 3, 2, 0, -1, 0, 10.0f, t, nx, ny, nz)
                  && std::fabs(t - 3.0f) < 1e-4f && ny > 0.99f,
              "mesh: raycast hits quad (t/normal)");
        check(!shapes::Raycast(mp, 20, 3, 0, 0, -1, 0, 10.0f, t, nx, ny, nz),
              "mesh: raycast misses outside quad");

        // 距離 / 最近点 / AABB
        check(std::fabs(shapes::DistanceToShape(mp, 0, 2, 0) - 2.0f) < 1e-4f,
              "mesh: DistanceToShape");
        float qx = 0, qy = 0, qz = 0;
        shapes::ClosestPointOnShape(mp, 1, 1.5f, 1, qx, qy, qz);
        check(std::fabs(qx - 1.0f) < 1e-4f && std::fabs(qy) < 1e-4f && std::fabs(qz - 1.0f) < 1e-4f,
              "mesh: ClosestPointOnShape");
        float mnX = 0, mnY = 0, mnZ = 0, mxX = 0, mxY = 0, mxZ = 0;
        shapes::ComputeAabb(mp, mnX, mnY, mnZ, mxX, mxY, mxZ);
        check(std::fabs(mnX + 5) < 1e-4f && std::fabs(mxX - 5) < 1e-4f && std::fabs(mnY) < 1e-4f,
              "mesh: ComputeAabb from BVH root");

        // スケール: sx=sz=2 で 20x20 相当 → (8,3,8) からのレイがヒット
        mp.sx = 2.0f;
        mp.sz = 2.0f;
        check(shapes::Raycast(mp, 8, 3, 8, 0, -1, 0, 10.0f, t, nx, ny, nz)
                  && std::fabs(t - 3.0f) < 1e-4f,
              "mesh: non-uniform scale applies to triangles");
        mp.sx = mp.sy = mp.sz = 1.0f;

        // meshData 未解決 (null) は全判定が衝突なし (安全なフォールバック)
        ShapePose noMesh = mp;
        noMesh.meshData = nullptr;
        sp.py = 0.3f;
        check(!shapes::Collide(sp, noMesh, nx, ny, nz, d) && !shapes::Overlap(sp, noMesh),
              "mesh: null meshData collides with nothing");
    }

    // ---- (M41-2) ソルバ統合: 球がメッシュ床に静止 + 決定論 (2 回実行で per-tick 一致) ----
    {
        MeshColliderData quad;
        BuildMeshColliderData(
            { { -5, 0, -5 }, { 5, 0, -5 }, { 5, 0, 5 }, { -5, 0, 5 } },
            { 0, 1, 2, 0, 2, 3 }, quad);
        MeshColliderLibrary lib;
        const AssetID meshId{ 0x4D343154ull };
        lib.Register(meshId, std::move(quad));
        meshcol::Install(&lib);

        auto build = [&](Scene& s) {
            GameObject ground = s.CreateGameObjectTracked("MeshGround");
            auto* col = ground.AddComponent<ColliderComponent>();
            col->shape = 3;
            col->isTrigger = false; // ソリッド衝突面 (M51 後続で既定が false になったが明示のまま)
            col->meshAsset = meshId;
            GameObject ball = MakeSphereBody(s, "Ball", 0.5f, 3.0f, 0.5f, 0.5f);
            (void)ball;
            s.GetWorld().ApplyStructuralChanges();
            return ball;
        };
        Scene s1, s2;
        GameObject ball1 = build(s1);
        GameObject ball2 = build(s2);
        bool hashesMatch = true;
        for (int i = 0; i < 180; ++i) {
            phys.Update(s1.GetWorld(), kDt);
            phys.Update(s2.GetWorld(), kDt);
            if (HashWorld(s1.GetWorld(), nullptr) != HashWorld(s2.GetWorld(), nullptr)) {
                hashesMatch = false;
            }
        }
        auto* lt = ball1.GetComponent<LocalTransform>();
        const float y = lt ? lt->position.y : -999.0f;
        check(y > 0.45f && y < 0.55f, "mesh solver: sphere rests on mesh quad at y~=0.5");
        auto* rb = ball1.GetComponent<RigidbodyComponent>();
        check(rb && std::fabs(rb->velocity.y) < 0.5f, "mesh solver: settled (|vy| small)");
        check(hashesMatch, "mesh solver: deterministic (per-tick hash identical)");

        // ワールドレベル Raycast: ボールから離れた位置からメッシュ床へ (WorldMatrix ベース —
        // TransformSystem 未実行でも LocalTransform=ルートなので EnsureFileId 経由の
        // WorldMatrix が無い場合に備え、収集は WorldMatrix 要求 → ここでは transform を回す)
        TransformSystem xform;
        xform.Update(s1.GetWorld());
        MyeRaycastHit hit = {};
        const int rc = RaycastWorld(s1.GetWorld(), { -2.0f, 2.0f, -2.0f }, { 0, -1, 0 }, 10.0f,
                                    &hit);
        check(rc == 1 && std::fabs(hit.distance - 2.0f) < 0.01f && hit.normal.y > 0.99f,
              "mesh: RaycastWorld hits mesh collider (dist/normal)");
        meshcol::Install(nullptr);
    }

    // ---- (M59a2) 物理マテリアル: 解決純関数 / 密度→質量 / 等価並走 / 静的床の反発 ----
    {
        PhysMatLibrary* prevLib = physmat::Library(); // selftest 中は通常 nullptr だが復元する
        PhysMatLibrary lib;

        // -- 純関数: 優先順位 (bit → 既存フィールド / 材料 → .physmat / 未割当 → 既存) --
        {
            PhysMat m;
            m.dynamicFriction = 0.9f;
            m.restitution = 0.8f;
            ColliderComponent col; // friction 既定 0.5
            RigidbodyComponent rb; // restitution 既定 0
            check(SelectFriction(col, &m) == 0.9f, "physmat: friction comes from the material");
            check(SelectFriction(col, nullptr) == 0.5f,
                  "physmat: no material falls back to the collider field");
            col.materialOverrideBits = kPhysMatOverrideFriction;
            check(SelectFriction(col, &m) == 0.5f,
                  "physmat: override bit beats the material (friction)");
            check(SelectRestitution(&col, &rb, &m) == 0.8f,
                  "physmat: restitution comes from the material");
            check(SelectRestitution(&col, nullptr, &m) == 0.8f,
                  "physmat: a static collider can claim restitution (new in M59a2)");
            check(SelectRestitution(&col, nullptr, nullptr) == 0.0f,
                  "physmat: static without material stays 0 (legacy)");
            col.materialOverrideBits |= kPhysMatOverrideRestitution;
            check(SelectRestitution(&col, &rb, &m) == 0.0f,
                  "physmat: override bit beats the material (restitution)");
        }

        // -- 体積と質量導出 (解析値。スケール規約は ApplyScaledExtents と同一) --
        {
            constexpr float kPiT = 3.14159265f;
            ColliderComponent sphere;
            sphere.shape = 0;
            sphere.radius = 0.5f;
            const float vsExpect = (4.0f / 3.0f) * kPiT * 0.125f;
            const float vs = ShapeVolumeWorld(sphere, 1, 1, 1);
            check(std::fabs(vs - vsExpect) < vsExpect * 1e-5f,
                  "physmat: sphere volume = 4/3 pi r^3");
            const float vs2 = ShapeVolumeWorld(sphere, 2.0f, 1, 1); // 球は最大成分 → r=1
            check(std::fabs(vs2 - (4.0f / 3.0f) * kPiT) < 1e-4f,
                  "physmat: sphere volume uses the max scale component");
            ColliderComponent box;
            box.shape = 1;
            box.halfExtents = { 1.0f, 0.5f, 0.25f };
            check(ShapeVolumeWorld(box, 1, 2.0f, 1) == 2.0f,
                  "physmat: box volume scales per component");
            ColliderComponent cap;
            cap.shape = 2;
            cap.radius = 0.5f;
            cap.height = 3.0f; // halfSeg = 1.5 - 0.5 = 1.0
            const float vcExpect = kPiT * 0.25f * 2.0f + (4.0f / 3.0f) * kPiT * 0.125f;
            check(std::fabs(ShapeVolumeWorld(cap, 1, 1, 1) - vcExpect) < vcExpect * 1e-5f,
                  "physmat: capsule volume = cylinder + end spheres");

            PhysMat steel;
            steel.density = 7850.0f;
            RigidbodyComponent rb;
            rb.mass = 2.0f;
            rb.useDensity = true;
            check(std::fabs(ResolveBodyMass(rb, &sphere, &steel, 1, 1, 1) - 7850.0f * vsExpect)
                      < 1.0f,
                  "physmat: mass = density x shape volume");
            rb.useDensity = false;
            check(ResolveBodyMass(rb, &sphere, &steel, 1, 1, 1) == 2.0f,
                  "physmat: useDensity off keeps the mass field");
            rb.useDensity = true;
            check(ResolveBodyMass(rb, &sphere, nullptr, 1, 1, 1) == 2.0f,
                  "physmat: no material keeps the mass field");
            check(ResolveBodyMass(rb, nullptr, &steel, 1, 1, 1) == 2.0f,
                  "physmat: no collider keeps the mass field");
            ColliderComponent degen;
            degen.shape = 0;
            degen.radius = 0.0f; // 体積 0 → 1/m をゼロ除算にしない
            check(ResolveBodyMass(rb, &degen, &steel, 1, 1, 1) == 2.0f,
                  "physmat: zero volume keeps the mass field (no div0)");
        }

        // -- 以降はワールド経由 (physmat::Resolve が要る) --
        physmat::Install(&lib);

        // EffectiveMassWorld: ABI (AddForce/AddImpulse/AddTorque) と同じ入口でスケールが効く
        {
            PhysMat steel;
            steel.density = 7850.0f;
            const uint64_t hs = lib.Register(L"x\\t_steel.physmat.json", steel);
            Scene s;
            GameObject go = MakeSphereBody(s, "M", 0, 0, 0, 0.5f);
            go.GetComponent<LocalTransform>()->scale = { 2.0f, 2.0f, 2.0f }; // r=1 に拡大
            auto* rb = go.GetComponent<RigidbodyComponent>();
            rb->useDensity = true;
            go.GetComponent<ColliderComponent>()->physMaterial = { hs };
            s.GetWorld().ApplyStructuralChanges();
            const float expect = 7850.0f * (4.0f / 3.0f) * 3.14159265f;
            const float m = EffectiveMassWorld(s.GetWorld(), go.Id(), *rb);
            check(std::fabs(m - expect) < expect * 1e-4f,
                  "physmat: EffectiveMassWorld applies the world scale");
        }

        // 剛体軌跡 (pos/rot/vel/angVel) を index 昇順でビット列に吸い出す (以降の並走比較用。
        // ワールドハッシュは physMaterial フィールド自体が載って必ず異なるため使えない)
        auto dumpBodies = [](World& w, std::vector<uint8_t>& out) {
            out.clear();
            const ComponentTypeId req[] = { RigidbodyComponent::sTypeId, LocalTransform::sTypeId };
            w.ForEachArchetype(req, [&](Archetype& arch) {
                const int li = arch.FindTypeIndex(LocalTransform::sTypeId);
                const int ri = arch.FindTypeIndex(RigidbodyComponent::sTypeId);
                for (uint32_t row = 0; row < arch.Count(); ++row) {
                    const auto* lt = static_cast<const LocalTransform*>(arch.GetPtr(li, row));
                    const auto* rb = static_cast<const RigidbodyComponent*>(arch.GetPtr(ri, row));
                    auto push = [&](const void* p, size_t n) {
                        const auto* b = static_cast<const uint8_t*>(p);
                        out.insert(out.end(), b, b + n);
                    };
                    push(&lt->position, sizeof(lt->position));
                    push(&lt->rotation, sizeof(lt->rotation));
                    push(&rb->velocity, sizeof(rb->velocity));
                    push(&rb->angularVelocity, sizeof(rb->angularVelocity));
                }
            });
        };

        // -- 等価並走: 既定同値材料シーン ⇔ 未割当シーンの軌跡ビット一致 (値経路の等価性) --
        {
            PhysMat md; // 動的側の既定同値 (μd=col 既定 0.5 / e=rb に与える 0.2)
            md.dynamicFriction = 0.5f;
            md.restitution = 0.2f;
            PhysMat mg; // 静的側の既定同値 (e=0 が従来の構造的既定)
            mg.dynamicFriction = 0.5f;
            mg.restitution = 0.0f;
            const uint64_t hd = lib.Register(L"x\\t_eq_dyn.physmat.json", md);
            const uint64_t hg = lib.Register(L"x\\t_eq_gnd.physmat.json", mg);
            auto build = [&](Scene& s, bool withMats) {
                GameObject g = MakeGround(s, "G", 0, -0.5f, 0, 5.0f, 0.5f, 5.0f);
                GameObject b0 = MakeBox(s, "B0", -1.0f, 2.0f, 0, 0.5f, 0.5f, 0.5f, 0.2f);
                GameObject b1 = MakeBox(s, "B1", 0.9f, 3.5f, 0.1f, 0.5f, 0.5f, 0.5f, 0.2f);
                GameObject sp = MakeSphereBody(s, "S", 0.1f, 5.0f, 0.05f, 0.5f);
                sp.GetComponent<RigidbodyComponent>()->restitution = 0.2f;
                if (withMats) {
                    g.GetComponent<ColliderComponent>()->physMaterial = { hg };
                    b0.GetComponent<ColliderComponent>()->physMaterial = { hd };
                    b1.GetComponent<ColliderComponent>()->physMaterial = { hd };
                    sp.GetComponent<ColliderComponent>()->physMaterial = { hd };
                }
                s.GetWorld().ApplyStructuralChanges();
            };
            Scene sa, sb;
            build(sa, true);
            build(sb, false);
            bool same = true;
            std::vector<uint8_t> da, db;
            for (int i = 0; i < 240 && same; ++i) {
                phys.Update(sa.GetWorld(), kDt);
                phys.Update(sb.GetWorld(), kDt);
                dumpBodies(sa.GetWorld(), da);
                dumpBodies(sb.GetWorld(), db);
                if (da.empty() || da != db) {
                    same = false;
                    MYE_LOG_ERROR("  physmat equivalence diverged at tick %d", i);
                }
            }
            check(same,
                  "physmat: default-equivalent materials replicate the unassigned trajectory "
                  "bit-exactly (240 ticks)");
        }

        // -- overrideBits: 過激な材料 (μ=0 / e=0.9) を両ビットで殺すと未割当と軌跡ビット一致 --
        {
            PhysMat wild;
            wild.dynamicFriction = 0.0f;
            wild.restitution = 0.9f;
            const uint64_t hw = lib.Register(L"x\\t_wild.physmat.json", wild);
            auto build = [&](Scene& s, bool withMat) {
                MakeGround(s, "G", 0, -0.5f, 0, 8.0f, 0.5f, 8.0f);
                GameObject b = MakeBox(s, "B", -3.0f, 0.6f, 0, 0.5f, 0.5f, 0.5f, 0.0f);
                b.GetComponent<RigidbodyComponent>()->velocity = { 4.0f, 0.0f, 0.0f }; // 摩擦滑走
                if (withMat) {
                    auto* c = b.GetComponent<ColliderComponent>();
                    c->physMaterial = { hw };
                    c->materialOverrideBits =
                        kPhysMatOverrideFriction | kPhysMatOverrideRestitution;
                }
                s.GetWorld().ApplyStructuralChanges();
            };
            Scene sa, sb;
            build(sa, true);
            build(sb, false);
            bool same = true;
            std::vector<uint8_t> da, db;
            for (int i = 0; i < 180 && same; ++i) {
                phys.Update(sa.GetWorld(), kDt);
                phys.Update(sb.GetWorld(), kDt);
                dumpBodies(sa.GetWorld(), da);
                dumpBodies(sb.GetWorld(), db);
                if (da.empty() || da != db) {
                    same = false;
                    MYE_LOG_ERROR("  physmat override diverged at tick %d", i);
                }
            }
            check(same, "physmat: override bits restore the legacy field path bit-exactly");
        }

        // -- 密度→質量がソルバに効く: 鋼球 (m~4110) と 1kg 球の正面弾性衝突 --
        // 解析値 (e=1): 重い側 v1' ~= 2.0 のまま / 軽い側 v2' ~= +6.0 (2m1v1+(m2-m1)v2)/(m1+m2)
        {
            PhysMat steel;
            steel.density = 7850.0f;
            steel.dynamicFriction = 0.5f;
            steel.restitution = 1.0f;
            const uint64_t hs = lib.Register(L"x\\t_steel_e1.physmat.json", steel);
            Scene s;
            GameObject a = MakeSphereBody(s, "A", -2.0f, 0, 0, 0.5f);
            GameObject b = MakeSphereBody(s, "B", 2.0f, 0, 0, 0.5f);
            auto* ra = a.GetComponent<RigidbodyComponent>();
            auto* rbb = b.GetComponent<RigidbodyComponent>();
            ra->gravityScale = 0.0f;
            rbb->gravityScale = 0.0f;
            ra->velocity = { 2.0f, 0.0f, 0.0f };
            rbb->velocity = { -2.0f, 0.0f, 0.0f };
            ra->useDensity = true;
            rbb->restitution = 1.0f; // e = min(材料 1.0, 1.0)
            a.GetComponent<ColliderComponent>()->physMaterial = { hs };
            s.GetWorld().ApplyStructuralChanges();
            for (int i = 0; i < 120; ++i) {
                phys.Update(s.GetWorld(), kDt);
            }
            MYE_LOG_INFO("  [phys] dense-vs-light: heavy vx=%.3f light vx=%.3f (expect ~2 / ~6)",
                         ra->velocity.x, rbb->velocity.x);
            check(rbb->velocity.x > 5.0f,
                  "physmat: light ball rebounds fast off the dense (useDensity) ball");
            check(std::fabs(ra->velocity.x - 2.0f) < 0.1f,
                  "physmat: dense ball keeps its velocity (m1 >> m2)");
        }

        // -- 材料付き静的床は弾む (新規能力): e=0.8 の床で h*e^2 付近まで戻る --
        {
            PhysMat bouncy;
            bouncy.dynamicFriction = 0.5f;
            bouncy.restitution = 0.8f;
            const uint64_t hb = lib.Register(L"x\\t_bouncy.physmat.json", bouncy);
            auto run = [&](bool withMat, float& outApex) {
                Scene s;
                GameObject g = MakeGround(s, "G", 0, -0.5f, 0, 5.0f, 0.5f, 5.0f);
                GameObject ball = MakeSphereBody(s, "Ball", 0, 2.5f, 0, 0.5f); // h = 2.0
                ball.GetComponent<RigidbodyComponent>()->restitution = 0.8f;
                if (withMat) {
                    g.GetComponent<ColliderComponent>()->physMaterial = { hb };
                }
                s.GetWorld().ApplyStructuralChanges();
                auto* lt = ball.GetComponent<LocalTransform>();
                auto* rb = ball.GetComponent<RigidbodyComponent>();
                bool contacted = false;
                outApex = 0.0f;
                for (int i = 0; i < 240; ++i) {
                    phys.Update(s.GetWorld(), kDt);
                    if (!contacted && rb->velocity.y > 0.0f) {
                        contacted = true; // 反発で上向きに転じた
                    }
                    if (contacted && lt->position.y > outApex) {
                        outApex = lt->position.y;
                    }
                }
            };
            float apexMat = 0.0f, apexBare = 0.0f;
            run(true, apexMat);
            run(false, apexBare);
            MYE_LOG_INFO("  [phys] static-floor bounce: material=%.3f bare=%.3f (expect ~%.3f)",
                         apexMat - 0.5f, apexBare - 0.5f, 2.0f * 0.8f * 0.8f);
            check(apexMat - 0.5f > 0.9f && apexMat - 0.5f < 1.5f,
                  "physmat: e=0.8 static floor bounces back to ~h*e^2 (new in M59a2)");
            check(apexBare - 0.5f < 0.2f,
                  "physmat: bare static floor stays inelastic (legacy e=0)");
        }

        physmat::Install(prevLib);
    }

    // ================= M59b: 物理環境 + 等方空力 =================
    // 既存シーンのビット不変は「新コンポーネントは登録順末尾 append = 既存エンティティに
    // 付いていない」で自明に立つ (上の各 `hash @240` ログが変更前後で一致することが
    // 機械的証明)。ここで確かめるのは新経路そのものの正しさ。
    {
        constexpr float kPi = DirectX::XM_PI;

        // -- 基準面積 (Cauchy の平均投影面積) の解析値 --
        {
            ColliderComponent sph;
            sph.shape = 0;
            sph.radius = 0.5f;
            const float aSph = MeanProjectedAreaWorld(&sph, 1.0f, 1.0f, 1.0f);
            check(std::fabs(aSph - kPi * 0.25f) < 1e-6f, "aero area: sphere == pi*r^2");
            // 球は最大成分スケール (ShapeVolumeWorld と同一規約)
            const float aSph2 = MeanProjectedAreaWorld(&sph, 1.0f, 2.0f, 1.0f);
            check(std::fabs(aSph2 - kPi) < 1e-5f, "aero area: sphere scales by max component");

            ColliderComponent box;
            box.shape = 1;
            box.halfExtents = { 0.5f, 0.5f, 0.5f }; // 1 辺 1 の立方体
            const float aBox = MeanProjectedAreaWorld(&box, 1.0f, 1.0f, 1.0f);
            check(std::fabs(aBox - 1.5f) < 1e-6f, "aero area: unit cube == 1.5 (known result)");

            ColliderComponent cap;
            cap.shape = 2;
            cap.radius = 0.5f;
            cap.height = 2.0f; // halfSeg = 0.5
            const float aCap = MeanProjectedAreaWorld(&cap, 1.0f, 1.0f, 1.0f);
            check(std::fabs(aCap - (kPi * 0.25f + kPi * 0.25f)) < 1e-6f,
                  "aero area: capsule == pi*r*halfSeg + pi*r^2");

            // コライダー無し / 動的 mesh は慣性導出と同じ「半径 0.5 の球」既定へ落ちる
            check(std::fabs(MeanProjectedAreaWorld(nullptr, 1.0f, 1.0f, 1.0f) - kPi * 0.25f)
                      < 1e-6f,
                  "aero area: no collider falls back to the r=0.5 sphere default");
            ColliderComponent mesh;
            mesh.shape = 3;
            check(std::fabs(MeanProjectedAreaWorld(&mesh, 3.0f, 3.0f, 3.0f) - kPi * 0.25f) < 1e-6f,
                  "aero area: mesh shape falls back to the same default");
        }

        // -- env: 重力がベクトルになる (横重力で落ちずに横へ加速する) --
        {
            Scene s;
            GameObject envGo = s.CreateGameObjectTracked("Env");
            auto* env = envGo.AddComponent<PhysicsEnvironmentComponent>();
            env->gravity = { 3.0f, 0.0f, 0.0f };
            GameObject ball = MakeSphereBody(s, "Ball", 0, 0, 0, 0.5f);
            s.GetWorld().ApplyStructuralChanges();
            for (int i = 0; i < 60; ++i) {
                phys.Update(s.GetWorld(), kDt);
            }
            auto* rb = ball.GetComponent<RigidbodyComponent>();
            check(rb && std::fabs(rb->velocity.x - 3.0f) < 0.05f,
                  "env: gravity vector accelerates +X (vx ~= 3 after 1s)");
            // gravity.y は +0.0f なので vy += 0.0f * gs * dt = 0 のまま (厳密一致)
            check(rb && rb->velocity.y == 0.0f, "env: zero-Y gravity leaves vy exactly 0");
        }

        // -- env の消費規約: entity.index 最小の active な 1 個 (Skybox/Fog と同じ) --
        {
            auto run = [&](bool disableFirst, float& outVx) {
                Scene s;
                GameObject e0 = s.CreateGameObjectTracked("Env0");
                e0.AddComponent<PhysicsEnvironmentComponent>()->gravity = { 3.0f, 0.0f, 0.0f };
                GameObject e1 = s.CreateGameObjectTracked("Env1");
                e1.AddComponent<PhysicsEnvironmentComponent>()->gravity = { -7.0f, 0.0f, 0.0f };
                if (disableFirst) {
                    e0.AddComponent<ActiveComponent>()->enabled = 0;
                }
                GameObject ball = MakeSphereBody(s, "Ball", 0, 0, 0, 0.5f);
                s.GetWorld().ApplyStructuralChanges();
                for (int i = 0; i < 60; ++i) {
                    phys.Update(s.GetWorld(), kDt);
                }
                outVx = ball.GetComponent<RigidbodyComponent>()->velocity.x;
            };
            float vxFirst = 0.0f, vxSecond = 0.0f;
            run(false, vxFirst);
            run(true, vxSecond);
            check(std::fabs(vxFirst - 3.0f) < 0.05f, "env: lowest entity.index wins");
            check(std::fabs(vxSecond + 7.0f) < 0.05f,
                  "env: an inactive environment is skipped (next index wins)");
        }

        // -- 終端速度が閉形式 v_t = sqrt(mg/k) に一致する (env 無し = 既定の空気で成立) --
        {
            Scene s;
            GameObject ball = MakeSphereBody(s, "Faller", 0, 0, 0, 0.5f);
            ball.AddComponent<AeroComponent>(); // 既定 = 抗力 ON / マグヌス OFF
            s.GetWorld().ApplyStructuralChanges();
            auto* rb = ball.GetComponent<RigidbodyComponent>();
            bool monotone = true;
            float prev = 0.0f;
            for (int i = 0; i < 900; ++i) { // 15 秒 (時定数 v_t/g ~ 0.67s に対して十分)
                phys.Update(s.GetWorld(), kDt);
                if (rb->velocity.y > prev) {
                    monotone = false; // 下向きに単調 = 抗力が符号を反転させていない
                }
                prev = rb->velocity.y;
            }
            const float area = kPi * 0.25f;
            const float k = 0.5f * kDefaultAirDensity * kDefaultDragCoefficient * area;
            const float vt = std::sqrt(9.81f / k); // m = 1
            const float measured = -rb->velocity.y;
            MYE_LOG_INFO("  [phys] aero terminal speed: %.4f (closed form %.4f)", measured, vt);
            check(monotone, "aero: implicit drag never overshoots (speed is monotone)");
            // 離散化 (重力を足してから抗力を掛ける) のぶん閉形式をわずかに下回る:
            // 固定点は v(v + g*dt) = g/k なので dt->0 で閉形式に一致する
            check(measured < vt && measured > vt * 0.95f,
                  "aero: terminal speed matches sqrt(mg/k) within the discretization gap");
        }

        // -- 風: 静止した物体が風速へ漸近する (超えない = implicit の無条件安定) --
        {
            Scene s;
            GameObject envGo = s.CreateGameObjectTracked("Env");
            auto* env = envGo.AddComponent<PhysicsEnvironmentComponent>();
            env->gravity = { 0.0f, 0.0f, 0.0f };
            env->windVelocity = { 5.0f, 0.0f, 0.0f };
            GameObject ball = MakeSphereBody(s, "Leaf", 0, 0, 0, 0.5f);
            auto* aero = ball.AddComponent<AeroComponent>();
            aero->areaScale = 20.0f; // 収束を早める (葉っぱのような大面積小質量)
            s.GetWorld().ApplyStructuralChanges();
            auto* rb = ball.GetComponent<RigidbodyComponent>();
            bool neverExceeds = true;
            for (int i = 0; i < 600; ++i) {
                phys.Update(s.GetWorld(), kDt);
                if (rb->velocity.x >= 5.0f) {
                    neverExceeds = false;
                }
            }
            check(neverExceeds, "aero/wind: velocity never overshoots the wind speed");
            check(rb->velocity.x > 4.95f, "aero/wind: body converges to the wind velocity");
            check(rb->velocity.y == 0.0f && rb->velocity.z == 0.0f,
                  "aero/wind: no drift on the axes the wind does not touch");
        }

        // -- マグヌス: 符号 (+X へ進み +Y 軸で回る球は -Z へ曲がる) と大きさ --
        {
            Scene s;
            GameObject envGo = s.CreateGameObjectTracked("Env");
            auto* menv = envGo.AddComponent<PhysicsEnvironmentComponent>();
            menv->gravity = { 0.0f, 0.0f, 0.0f };
            // ★**1 tick の閉形式と突き合わせる「式の試験」**なのでサブステップを 1 に固定する
            //   (M59g2 の既定 4 だと tick 内で速度ベクトルが回るぶんだけ値がずれ、
            //   マグヌス係数そのものの検証にならない。実測 -0.80144 vs 閉形式 -0.80176)
            menv->substeps = 1;
            GameObject ball = MakeSphereBody(s, "Curve", 0, 0, 0, 0.5f);
            auto* aero = ball.AddComponent<AeroComponent>();
            aero->enableDrag = false;
            aero->enableAngularDrag = false;
            aero->enableMagnus = true;
            s.GetWorld().ApplyStructuralChanges();
            // ★ポインタは**構造変更を確定した後**に取る — AddComponent はアーキタイプを
            //   移すので、それ以前に取った参照は tick の結果が反映されない古い記憶域を指す
            auto* rb = ball.GetComponent<RigidbodyComponent>();
            rb->velocity = { 10.0f, 0.0f, 0.0f };
            rb->angularVelocity = { 0.0f, 20.0f, 0.0f };
            rb->angularDamping = 0.0f; // 非物理の定率減衰を切って空力だけを見る
            phys.Update(s.GetWorld(), kDt);
            // S = magnus * 0.5 * rho * A * r、F = S (omega x v) = S * (0, 0, -omega*vx)
            const float area = kPi * 0.25f;
            const float sMag = 0.5f * kDefaultAirDensity * area * 0.5f;
            const float expectDvz = -sMag * 20.0f * 10.0f * kDt;
            MYE_LOG_INFO("  [phys] magnus 1 tick dvz = %.5f (expect %.5f)", rb->velocity.z,
                         expectDvz);
            check(rb->velocity.z < 0.0f, "magnus: +X motion with +Y spin curves toward -Z");
            check(std::fabs(rb->velocity.z - expectDvz) < 1e-4f,
                  "magnus: magnitude matches S*(omega x v)/m*dt");
            check(rb->velocity.y == 0.0f, "magnus: no force along the spin axis");
            // マグヌス単独では力が常に速度と直交する = 速度ベクトルが等速で回り続ける
            // (角速度 S*omega/m ~ 4.8 rad/s)。長く回すと 1 周してしまうので、
            // 4 分の 1 周に満たない範囲で「軌跡が -Z 側へ曲がっている」ことを見る
            auto* lt = ball.GetComponent<LocalTransform>();
            for (int i = 0; i < 10; ++i) {
                phys.Update(s.GetWorld(), kDt);
            }
            check(lt->position.z < -0.1f, "magnus: the trajectory bends toward -Z");
            // マグヌスは仕事をしない。陽的積分ぶん速さがわずかに増えるだけ
            // (回転の陽的積分の既知の性質。実運用では抗力・角抗力が打ち消す)
            const float sp = std::sqrt(rb->velocity.x * rb->velocity.x
                                       + rb->velocity.z * rb->velocity.z);
            check(sp > 10.0f && sp < 10.5f,
                  "magnus: does no work (speed is preserved up to the explicit-integration drift)");
        }

        // -- 角抗力: 回転が単調減衰し、係数を極端にしても符号が反転しない --
        {
            Scene s;
            GameObject ball = MakeSphereBody(s, "Spinner", 0, 0, 0, 0.5f);
            auto* aero = ball.AddComponent<AeroComponent>();
            aero->enableDrag = false;
            aero->angularDragCoefficient = 1000.0f; // 陽的なら即発散する領域
            s.GetWorld().ApplyStructuralChanges();
            auto* rb = ball.GetComponent<RigidbodyComponent>(); // 構造変更の後に取る
            rb->gravityScale = 0.0f;
            rb->angularVelocity = { 0.0f, 50.0f, 0.0f };
            rb->angularDamping = 0.0f;
            bool monotone = true, positive = true;
            float prev = 50.0f;
            for (int i = 0; i < 600; ++i) {
                phys.Update(s.GetWorld(), kDt);
                const float w = rb->angularVelocity.y;
                if (w > prev) {
                    monotone = false;
                }
                if (w <= 0.0f) {
                    positive = false;
                }
                prev = w;
            }
            check(positive, "angular drag: implicit form never flips the spin sign (Cda=1000)");
            check(monotone, "angular drag: spin decays monotonically");
            check(rb->angularVelocity.y < 1.0f, "angular drag: 50 rad/s is nearly killed in 10s");
        }

        // -- 混在シーン: Aero 非所持ボディの軌跡は 1 ビットも変わらない --
        {
            auto dumpOne = [](GameObject go, std::vector<uint8_t>& out) {
                out.clear();
                const auto* lt = go.GetComponent<LocalTransform>();
                const auto* rb = go.GetComponent<RigidbodyComponent>();
                auto push = [&](const void* p, size_t n) {
                    const auto* b = static_cast<const uint8_t*>(p);
                    out.insert(out.end(), b, b + n);
                };
                push(&lt->position, sizeof(lt->position));
                push(&lt->rotation, sizeof(lt->rotation));
                push(&rb->velocity, sizeof(rb->velocity));
                push(&rb->angularVelocity, sizeof(rb->angularVelocity));
            };
            Scene sa, sb;
            MakeGround(sa, "G", 0, -0.5f, 0, 5.0f, 0.5f, 5.0f);
            GameObject plainA = MakeBox(sa, "Plain", 0, 3.0f, 0, 0.5f, 0.5f, 0.5f, 0.3f);
            sa.GetWorld().ApplyStructuralChanges();
            MakeGround(sb, "G", 0, -0.5f, 0, 5.0f, 0.5f, 5.0f);
            GameObject plainB = MakeBox(sb, "Plain", 0, 3.0f, 0, 0.5f, 0.5f, 0.5f, 0.3f);
            // 接触しない遠方に空力ボディを足す (混在させても非所持側は従来経路のまま)
            GameObject winged = MakeSphereBody(sb, "Winged", 50.0f, 3.0f, 0, 0.5f);
            winged.AddComponent<AeroComponent>()->enableMagnus = true;
            sb.GetWorld().ApplyStructuralChanges();
            // ★水平初速を与える — 自由落下だと omega が v と平行になり omega x v が恒等 0 に
            //   なって「空力が効いた」証明にならない (マグヌスは軸に平行な運動を曲げない)
            auto* wrb = winged.GetComponent<RigidbodyComponent>();
            wrb->velocity = { 0.0f, 0.0f, 8.0f };
            wrb->angularVelocity = { 0.0f, 30.0f, 0.0f };
            bool same = true;
            std::vector<uint8_t> da, db;
            for (int i = 0; i < 240 && same; ++i) {
                phys.Update(sa.GetWorld(), kDt);
                phys.Update(sb.GetWorld(), kDt);
                dumpOne(plainA, da);
                dumpOne(plainB, db);
                if (da.empty() || da != db) {
                    same = false;
                    MYE_LOG_ERROR("  aero mix diverged at tick %d", i);
                }
            }
            check(same,
                  "aero: a body without AeroComponent keeps its legacy trajectory bit-exactly "
                  "even next to aero bodies (240 ticks)");
            // 空力側は実際に動いている (試験が空振りでない証明)
            check(std::fabs(wrb->velocity.x) > 0.01f,
                  "aero: the aero body itself did curve (the mix test is not vacuous)");
        }

        // -- 全項目 OFF の Aero は付けても何も足さない (bool OFF = 項ごとスキップ) --
        {
            auto run = [&](bool withAero, std::vector<uint8_t>& out) {
                Scene s;
                MakeGround(s, "G", 0, -0.5f, 0, 5.0f, 0.5f, 5.0f);
                GameObject b = MakeSphereBody(s, "B", 0.1f, 3.0f, 0.05f, 0.5f);
                b.GetComponent<RigidbodyComponent>()->velocity = { 1.0f, 0.0f, 0.5f };
                if (withAero) {
                    auto* a = b.AddComponent<AeroComponent>();
                    a->enableDrag = false;
                    a->enableAngularDrag = false;
                    a->enableMagnus = false;
                }
                s.GetWorld().ApplyStructuralChanges();
                for (int i = 0; i < 240; ++i) {
                    phys.Update(s.GetWorld(), kDt);
                }
                out.clear();
                const auto* lt = b.GetComponent<LocalTransform>();
                const auto* rb = b.GetComponent<RigidbodyComponent>();
                const auto* p = reinterpret_cast<const uint8_t*>(&lt->position);
                out.insert(out.end(), p, p + sizeof(lt->position));
                const auto* v = reinterpret_cast<const uint8_t*>(&rb->velocity);
                out.insert(out.end(), v, v + sizeof(rb->velocity));
            };
            std::vector<uint8_t> withA, withoutA;
            run(true, withA);
            run(false, withoutA);
            check(!withA.empty() && withA == withoutA,
                  "aero: an all-off AeroComponent is bit-neutral (terms are skipped, not zeroed)");
        }
    }

    // ================= M59b2: 浮力 =================
    {
        constexpr float kPi = DirectX::XM_PI;

        // -- 没水割合と没水重心の解析値 (球冠は多項式のみ) --
        {
            ShapePose sp;
            sp.shape = 0;
            sp.radius = 0.5f;
            sp.px = sp.py = sp.pz = 0.0f;
            float cy = 0.0f;
            check(SubmergedFractionWorld(sp, -1.0f, cy) == 0.0f,
                  "buoyancy: a sphere entirely above the surface is exactly 0 submerged");
            check(SubmergedFractionWorld(sp, 1.0f, cy) == 1.0f,
                  "buoyancy: a fully submerged sphere is exactly 1");
            const float half = SubmergedFractionWorld(sp, 0.0f, cy);
            check(std::fabs(half - 0.5f) < 1e-6f,
                  "buoyancy: a sphere centred on the surface is half submerged");
            // 半球の重心は中心から 3R/8 下
            check(std::fabs(cy + 0.1875f) < 1e-5f,
                  "buoyancy: the hemisphere centroid sits 3R/8 below the centre");

            ShapePose bp;
            bp.shape = 1;
            bp.hx = bp.hy = bp.hz = 0.5f;
            const float bhalf = SubmergedFractionWorld(bp, 0.0f, cy);
            check(std::fabs(bhalf - 0.5f) < 1e-6f, "buoyancy: box slab fraction is the height ratio");
            check(std::fabs(cy + 0.25f) < 1e-6f, "buoyancy: box slab centroid is the slab midpoint");
            check(SubmergedFractionWorld(bp, -0.5f, cy) == 0.0f,
                  "buoyancy: a box touching the surface from above is still 0");
        }

        // 水面 y=0 / 真水の環境をつくる小道具
        auto makeWater = [](Scene& s) {
            GameObject go = s.CreateGameObjectTracked("Water");
            auto* env = go.AddComponent<PhysicsEnvironmentComponent>();
            env->waterPlaneY = 0.0f;
            env->waterDensity = 1000.0f;
            return go;
        };

        // -- 中性浮力: 密度が水の半分の球は中心が水面に来る (解析値) --
        {
            Scene s;
            makeWater(s);
            GameObject ball = MakeSphereBody(s, "Float", 0, 3.0f, 0, 0.5f);
            ball.AddComponent<BuoyancyComponent>()->linearDrag = 5.0f;
            s.GetWorld().ApplyStructuralChanges();
            auto* rb = ball.GetComponent<RigidbodyComponent>();
            // V = 4/3 pi r^3、平衡は V_sub = m / rho_w。m をその半分にすると半没 = 中心が水面
            const float vol = (4.0f / 3.0f) * kPi * 0.125f;
            rb->mass = 1000.0f * vol * 0.5f;
            auto* lt = ball.GetComponent<LocalTransform>();
            for (int i = 0; i < 1800; ++i) { // 30 秒 (減衰比が小さいので長めに沈める)
                phys.Update(s.GetWorld(), kDt);
            }
            MYE_LOG_INFO("  [phys] buoyancy equilibrium y = %.4f (expect ~0)", lt->position.y);
            check(std::fabs(lt->position.y) < 0.05f,
                  "buoyancy: a half-density sphere settles with its centre on the surface");
            check(std::fabs(rb->velocity.y) < 0.05f, "buoyancy: it actually comes to rest");
        }

        // -- 密度が水より大きければ沈み続ける / 小さければ浅く浮く --
        {
            // ★水中抗力は**没水割合で按分される**ので、浅く浮く軽い物体はほとんど減衰
            //   しない (実測: 既定 drag=2 / 没水 5% で減衰比 0.004 = 30 秒でも揺れ続ける)。
            //   物理として正しい振る舞いなので、平衡位置を測る側が drag を上げて対処する
            auto settle = [&](float mass, float drag, int ticks, float& outY) {
                Scene s;
                makeWater(s);
                GameObject ball = MakeSphereBody(s, "B", 0, 1.5f, 0, 0.5f);
                ball.AddComponent<BuoyancyComponent>()->linearDrag = drag;
                s.GetWorld().ApplyStructuralChanges();
                ball.GetComponent<RigidbodyComponent>()->mass = mass;
                for (int i = 0; i < ticks; ++i) {
                    phys.Update(s.GetWorld(), kDt);
                }
                outY = ball.GetComponent<LocalTransform>()->position.y;
            };
            const float vol = (4.0f / 3.0f) * kPi * 0.125f;
            float ySink = 0.0f, yFloat = 0.0f;
            settle(1000.0f * vol * 4.0f, 2.0f, 600, ySink);    // 密度 4000 = 沈む
            settle(1000.0f * vol * 0.05f, 20.0f, 1800, yFloat); // 密度 50 = 5% だけ沈む
            MYE_LOG_INFO("  [phys] buoyancy sink=%.3f float=%.3f", ySink, yFloat);
            check(ySink < -5.0f, "buoyancy: a body denser than water keeps sinking");
            // 没水割合 5% を球冠の式 (0.25t - t^3/3 = -0.075) で解くと t ~= -0.365
            // = 中心は水面より約 0.365 上
            check(yFloat > 0.34f && yFloat < 0.39f,
                  "buoyancy: a light body floats with only its bottom submerged");
        }

        // -- 水面より上では完全 no-op (陸上シーンは Buoyancy を付けても従来経路) --
        {
            auto run = [&](bool withBuoy, std::vector<uint8_t>& out) {
                Scene s;
                makeWater(s);
                // 床を水面より上 (y=2 の上面) に置き、水に触れないまま着地させる
                MakeGround(s, "G", 0, 1.5f, 0, 5.0f, 0.5f, 5.0f);
                GameObject b = MakeSphereBody(s, "B", 0.1f, 5.0f, 0.05f, 0.5f);
                if (withBuoy) {
                    b.AddComponent<BuoyancyComponent>();
                }
                s.GetWorld().ApplyStructuralChanges();
                for (int i = 0; i < 240; ++i) {
                    phys.Update(s.GetWorld(), kDt);
                }
                const auto* lt = b.GetComponent<LocalTransform>();
                const auto* rb = b.GetComponent<RigidbodyComponent>();
                out.clear();
                const auto* p = reinterpret_cast<const uint8_t*>(&lt->position);
                out.insert(out.end(), p, p + sizeof(lt->position));
                const auto* v = reinterpret_cast<const uint8_t*>(&rb->velocity);
                out.insert(out.end(), v, v + sizeof(rb->velocity));
                check(lt->position.y > 0.5f, "buoyancy: the dry-run body never reached the water");
            };
            std::vector<uint8_t> withB, withoutB;
            run(true, withB);
            run(false, withoutB);
            check(!withB.empty() && withB == withoutB,
                  "buoyancy: a body that stays above the surface is bit-identical to no Buoyancy");
        }

        // -- v1 の既知の限界: 傾いて浮かぶ箱に復原モーメントは出ない --
        // (没水重心の水平ずれを高さ比近似が拾えないため。面ごとの圧力積分 = M59c の仕事)
        {
            Scene s;
            makeWater(s);
            GameObject box = MakeBox(s, "Raft", 0, 0.2f, 0, 1.0f, 0.25f, 1.0f);
            box.AddComponent<BuoyancyComponent>();
            s.GetWorld().ApplyStructuralChanges();
            auto* lt = box.GetComponent<LocalTransform>();
            auto* rb = box.GetComponent<RigidbodyComponent>();
            rb->mass = 1000.0f; // 体積 2 m^3 の半分が沈む重さ
            const float a = 0.34906585f * 0.5f; // 20 度の半角
            lt->rotation = { 0.0f, 0.0f, std::sin(a), std::cos(a) };
            const DirectX::XMFLOAT4 tilt0 = lt->rotation;
            for (int i = 0; i < 600; ++i) {
                phys.Update(s.GetWorld(), kDt);
            }
            check(rb->angularVelocity.x == 0.0f && rb->angularVelocity.y == 0.0f
                      && rb->angularVelocity.z == 0.0f,
                  "buoyancy (v1 limit): no righting moment is produced (angular velocity stays 0)");
            check(lt->rotation.z == tilt0.z && lt->rotation.w == tilt0.w,
                  "buoyancy (v1 limit): the tilt is preserved exactly");
            check(lt->position.y > -1.0f && lt->position.y < 1.0f,
                  "buoyancy: the tilted raft still floats near the surface");
        }

        // -- 混在シーン: Buoyancy 非所持ボディの軌跡は 1 ビットも変わらない --
        {
            auto dumpOne = [](GameObject go, std::vector<uint8_t>& out) {
                out.clear();
                const auto* lt = go.GetComponent<LocalTransform>();
                const auto* rb = go.GetComponent<RigidbodyComponent>();
                auto push = [&](const void* p, size_t n) {
                    const auto* b = static_cast<const uint8_t*>(p);
                    out.insert(out.end(), b, b + n);
                };
                push(&lt->position, sizeof(lt->position));
                push(&rb->velocity, sizeof(rb->velocity));
            };
            Scene sa, sb;
            MakeGround(sa, "G", 0, -0.5f, 0, 5.0f, 0.5f, 5.0f);
            GameObject plainA = MakeBox(sa, "Plain", 0, 3.0f, 0, 0.5f, 0.5f, 0.5f, 0.3f);
            // ★**参照側にも同じ env を置く** — M59g2 から PhysicsEnvironment は値の出所だけでなく
            //   **積分スケジュール (substeps) を選ぶ**。env の有無が違うと Buoyancy を持たない
            //   ボディの軌跡まで変わる (重力を h 刻みで N 回足すので float の丸めが違う)。
            //   この試験が見たいのは「**浮いているボディが隣に居ても影響しない**」ことなので、
            //   差分は「浮くボディが居るかどうか」だけにする
            GameObject wgoA = sa.CreateGameObjectTracked("Water");
            wgoA.AddComponent<PhysicsEnvironmentComponent>()->waterPlaneY = 20.0f;
            sa.GetWorld().ApplyStructuralChanges();
            MakeGround(sb, "G", 0, -0.5f, 0, 5.0f, 0.5f, 5.0f);
            GameObject plainB = MakeBox(sb, "Plain", 0, 3.0f, 0, 0.5f, 0.5f, 0.5f, 0.3f);
            // 水面を y=20 に上げて遠方のボディだけを水中に置く (接触しない位置)
            GameObject wgo = sb.CreateGameObjectTracked("Water");
            wgo.AddComponent<PhysicsEnvironmentComponent>()->waterPlaneY = 20.0f;
            GameObject boat = MakeSphereBody(sb, "Boat", 60.0f, 19.0f, 0, 0.5f);
            boat.AddComponent<BuoyancyComponent>();
            sb.GetWorld().ApplyStructuralChanges();
            boat.GetComponent<RigidbodyComponent>()->mass = 100.0f;
            bool same = true;
            std::vector<uint8_t> da, db;
            for (int i = 0; i < 240 && same; ++i) {
                phys.Update(sa.GetWorld(), kDt);
                phys.Update(sb.GetWorld(), kDt);
                dumpOne(plainA, da);
                dumpOne(plainB, db);
                if (da.empty() || da != db) {
                    same = false;
                    MYE_LOG_ERROR("  buoyancy mix diverged at tick %d", i);
                }
            }
            // ★両シーンに同じ env が居るので重力経路も積分スケジュールも同一。
            //   ハッシュ等価ではなく plain 同士の軌跡一致で判定している
            //   (シーン b にはボートが余分に居る = ワールドハッシュは必ず違う)
            check(same,
                  "buoyancy: a body without Buoyancy is unaffected by floating bodies nearby "
                  "(240 ticks bit-identical)");
            check(boat.GetComponent<LocalTransform>()->position.y > 18.5f,
                  "buoyancy: the floating body did float (the mix test is not vacuous)");
        }
    }

    // ================= M59e: SolidContact 拡張 + 物理デバッグ可視化 =================
    {
        // -- 代表接触点と法線インパルスの意味 --
        // 静止した質量 m の箱を支えている接触の法線インパルス合計は m*g*dt になる
        // (重力が毎 tick 与える運動量をソルバがそっくり打ち消しているため)
        {
            Scene s;
            MakeGround(s, "G", 0, -0.5f, 0, 5.0f, 0.5f, 5.0f); // 上面 y=0
            GameObject box = MakeBox(s, "Rester", 0, 0.5f, 0, 0.5f, 0.5f, 0.5f);
            s.GetWorld().ApplyStructuralChanges();
            box.GetComponent<RigidbodyComponent>()->mass = 2.0f;
            std::vector<SolidContact> contacts;
            for (int i = 0; i < 240; ++i) { // 静定させる
                phys.Update(s.GetWorld(), kDt, &contacts);
            }
            check(contacts.size() == 1, "contact: a settled box reports exactly one pair");
            if (contacts.size() == 1) {
                const SolidContact& c = contacts[0];
                // 代表点は箱の底 = 床の上面 (y=0) の近く
                check(std::fabs(c.py) < 0.02f,
                      "contact: the representative point sits on the contact plane");
                check(std::fabs(c.px) < 0.02f && std::fabs(c.pz) < 0.02f,
                      "contact: the representative point is the manifold centroid");
                const float expect = 2.0f * 9.81f * kDt; // m*g*dt
                MYE_LOG_INFO("  [phys] resting contact impulse = %.5f (expect m*g*dt = %.5f)",
                             c.impulse, expect);
                check(std::fabs(c.impulse - expect) < 0.02f,
                      "contact: the accumulated normal impulse equals m*g*dt while resting");
                check(c.ny < -0.9f,
                      "contact: the normal points from the high index to the low index (box->ground)");
            }
        }

        // -- 落下中 (接触なし) は 1 件も出さない / 出力を渡さない呼び方でも同じ挙動 --
        {
            Scene s;
            MakeGround(s, "G", 0, -0.5f, 0, 5.0f, 0.5f, 5.0f);
            MakeBox(s, "Faller", 0, 8.0f, 0, 0.5f, 0.5f, 0.5f);
            s.GetWorld().ApplyStructuralChanges();
            std::vector<SolidContact> contacts;
            phys.Update(s.GetWorld(), kDt, &contacts);
            check(contacts.empty(), "contact: a body in free fall reports no pair");
        }

        // -- 接触情報の収集は sim を変えない (outContacts を渡す/渡さないで並走ハッシュ一致) --
        {
            auto build = [](Scene& s) {
                MakeGround(s, "G", 0, -0.5f, 0, 5.0f, 0.5f, 5.0f);
                MakeBox(s, "B0", -0.6f, 1.2f, 0, 0.5f, 0.5f, 0.5f, 0.2f);
                MakeBox(s, "B1", 0.6f, 2.4f, 0.1f, 0.5f, 0.5f, 0.5f, 0.2f);
                MakeSphereBody(s, "S", 0.0f, 4.0f, 0.05f, 0.5f);
                s.GetWorld().ApplyStructuralChanges();
            };
            Scene sa, sb;
            build(sa);
            build(sb);
            std::vector<SolidContact> contacts;
            bool same = true;
            for (int i = 0; i < 240 && same; ++i) {
                phys.Update(sa.GetWorld(), kDt, &contacts); // 収集あり
                phys.Update(sb.GetWorld(), kDt, nullptr);   // 収集なし
                if (HashWorld(sa.GetWorld(), nullptr) != HashWorld(sb.GetWorld(), nullptr)) {
                    same = false;
                    MYE_LOG_ERROR("  contact collection changed the sim at tick %d", i);
                }
            }
            check(same,
                  "contact: collecting contacts is read-only (hash-identical to not collecting, "
                  "240 ticks)");
        }

        // -- 可視化は World を読むだけ / トグルの意味 --
        {
            Scene s;
            MakeGround(s, "G", 0, -0.5f, 0, 5.0f, 0.5f, 5.0f);
            GameObject box = MakeBox(s, "Rester", 0, 0.5f, 0, 0.5f, 0.5f, 0.5f);
            GameObject flyer = MakeSphereBody(s, "Flyer", 3.0f, 3.0f, 0, 0.5f);
            s.GetWorld().ApplyStructuralChanges();
            flyer.GetComponent<RigidbodyComponent>()->velocity = { 2.0f, 0.0f, 0.0f };
            std::vector<SolidContact> contacts;
            for (int i = 0; i < 120; ++i) {
                phys.Update(s.GetWorld(), kDt, &contacts);
            }
            TransformSystem ts;
            ts.Update(s.GetWorld()); // 速度ベクトルの根元にワールド行列が要る
            const uint64_t before = HashWorld(s.GetWorld(), nullptr);

            std::vector<DebugLineCmd> lines;
            PhysicsDebugFlags off;
            BuildPhysicsDebugLines(s.GetWorld(), contacts, off, lines);
            check(lines.empty(), "phys debug: all flags off emits nothing");

            PhysicsDebugFlags onContacts;
            onContacts.contacts = true;
            BuildPhysicsDebugLines(s.GetWorld(), contacts, onContacts, lines);
            // 接触 1 件につき 十字 3 本 + 法線 1 本
            check(lines.size() == contacts.size() * 4,
                  "phys debug: contacts emit a 3-line cross plus one normal each");

            lines.clear();
            PhysicsDebugFlags onVel;
            onVel.velocities = true;
            BuildPhysicsDebugLines(s.GetWorld(), contacts, onVel, lines);
            // 静止した箱は閾値未満で描かれず、飛んでいる球だけが 1 本出す
            check(lines.size() == 1,
                  "phys debug: only bodies above the speed threshold emit a velocity line");

            check(HashWorld(s.GetWorld(), nullptr) == before,
                  "phys debug: building the lines does not touch the world (hash unchanged)");
        }

        // -- インパルス表示は法線の長さだけを変える (本数は同じ) --
        {
            Scene s;
            MakeGround(s, "G", 0, -0.5f, 0, 5.0f, 0.5f, 5.0f);
            MakeBox(s, "Rester", 0, 0.5f, 0, 0.5f, 0.5f, 0.5f);
            s.GetWorld().ApplyStructuralChanges();
            std::vector<SolidContact> contacts;
            for (int i = 0; i < 120; ++i) {
                phys.Update(s.GetWorld(), kDt, &contacts);
            }
            std::vector<DebugLineCmd> a, b;
            PhysicsDebugFlags f1;
            f1.contacts = true;
            PhysicsDebugFlags f2;
            f2.contacts = true;
            f2.impulses = true;
            BuildPhysicsDebugLines(s.GetWorld(), contacts, f1, a);
            BuildPhysicsDebugLines(s.GetWorld(), contacts, f2, b);
            check(!a.empty() && a.size() == b.size(),
                  "phys debug: the impulse flag changes lengths, not the line count");
            const DebugLineCmd& na = a.back();
            const DebugLineCmd& nb = b.back();
            const float la = std::fabs(na.by - na.ay);
            const float lb = std::fabs(nb.by - nb.ay);
            check(la > 0.0f && lb > 0.0f && std::fabs(la - lb) > 1e-4f,
                  "phys debug: a weak resting contact draws a shorter normal than the fixed length");
        }
    }

    // ================= M59c: 面サンプリングカーネル + 平板空力 =================
    {
        constexpr float kPi = DirectX::XM_PI;

        // -- 一様流中の球: 細かく分割して同じカーネルへ流すと解析 1 発に収束する --
        // (「球 = 解析 1 発」が面積分の閉形式であることの証明。流れは軸に乗せない)
        {
            AeroCoeffs c;
            c.density = 1.225f;
            c.normalCoeff = 0.5f;
            c.tangentCoeff = 0.0f; // 圧力項だけを見る
            const float R = 0.7f;
            const float ux = 6.0f, uy = -3.0f, uz = 2.0f; // 斜めの一様流
            ShapePose sp;
            sp.shape = 0;
            sp.radius = R;
            AeroAccum analytic;
            AccumulateShapeAero(sp, ux, uy, uz, 0, 0, 0, c, analytic);

            // 緯度 x 経度の格子で表面を分割 (試験専用。実行時の経路では使わない)
            AeroAccum summed;
            constexpr int kLat = 128, kLon = 256;
            for (int i = 0; i < kLat; ++i) {
                const float th0 = kPi * static_cast<float>(i) / static_cast<float>(kLat);
                const float th1 = kPi * static_cast<float>(i + 1) / static_cast<float>(kLat);
                const float th = 0.5f * (th0 + th1);
                // 帯の面積 = 2 pi R^2 (cos th0 - cos th1) を経度で割る
                const float band = 2.0f * kPi * R * R * (std::cos(th0) - std::cos(th1));
                const float area = band / static_cast<float>(kLon);
                const float st = std::sin(th), ct = std::cos(th);
                for (int j = 0; j < kLon; ++j) {
                    const float ph = 2.0f * kPi * (static_cast<float>(j) + 0.5f)
                                   / static_cast<float>(kLon);
                    SurfaceElement e;
                    e.nx = st * std::cos(ph);
                    e.ny = ct;
                    e.nz = st * std::sin(ph);
                    e.px = e.nx * R;
                    e.py = e.ny * R;
                    e.pz = e.nz * R;
                    e.area = area;
                    e.vx = ux;
                    e.vy = uy;
                    e.vz = uz;
                    AccumulateSurfaceElement(e, c, 0, 0, 0, summed);
                }
            }
            const float fa = std::sqrt(analytic.fx * analytic.fx + analytic.fy * analytic.fy
                                       + analytic.fz * analytic.fz);
            const float fs = std::sqrt(summed.fx * summed.fx + summed.fy * summed.fy
                                       + summed.fz * summed.fz);
            MYE_LOG_INFO("  [phys] aero sphere: analytic %.4f N vs tessellated %.4f N", fa, fs);
            check(fa > 0.0f && std::fabs(fs - fa) < fa * 0.01f,
                  "aero kernel: a tessellated sphere converges to the closed-form drag (1%)");
            // 抗力は流れ方向へ真っ直ぐ (等方形状なので横力は出ない)
            const float sp2 = std::sqrt(ux * ux + uy * uy + uz * uz);
            check(std::fabs(summed.fx / fs + ux / sp2) < 0.01f
                      && std::fabs(summed.fy / fs + uy / sp2) < 0.01f,
                  "aero kernel: the tessellated sphere force opposes the flow exactly");
        }

        // -- 対称な OBB に正面から流すとトルクは厳密に 0 --
        {
            AeroCoeffs c;
            c.tangentCoeff = 0.02f; // 摩擦を入れても打ち消し合うことまで見る
            ShapePose bp;
            bp.shape = 1;
            bp.hx = 0.5f;
            bp.hy = 0.3f;
            bp.hz = 0.4f;
            AeroAccum acc;
            AccumulateShapeAero(bp, 12.0f, 0.0f, 0.0f, 0, 0, 0, c, acc);
            check(acc.tx == 0.0f && acc.ty == 0.0f && acc.tz == 0.0f,
                  "aero kernel: head-on flow over a symmetric box yields exactly zero torque");
            check(acc.fx < 0.0f && acc.fy == 0.0f && acc.fz == 0.0f,
                  "aero kernel: the force is pure drag along -X");
            // 正対面の抗力は 1/2 rho Cd A u^2 (Cn = Cd/2 の規約)。摩擦ぶん少し上回る
            const float expect = c.normalCoeff * c.density * (4.0f * bp.hy * bp.hz) * 144.0f;
            check(std::fabs(acc.fx) > expect * 0.99f && std::fabs(acc.fx) < expect * 1.3f,
                  "aero kernel: box drag matches Cn*rho*A*u^2 (plus a little skin friction)");
        }

        // -- 平板の揚力: alpha=0 と alpha=90 で厳密 0、間で正 --
        // 板は XZ 面 (法線 = ローカル Y)。速度を (cos a, -sin a, 0) 方向に取ると、
        // 下面が風上になって揚力は +Y 側へ出る
        {
            AeroCoeffs c;
            c.tangentCoeff = 0.0f; // 揚力の符号だけを見る (摩擦は流れ方向にしか効かない)
            ShapePose plate;
            plate.shape = 1;
            plate.hx = 1.0f;
            plate.hy = 0.0f; // 厚さ 0 = 側面の寄与を消した理想平板
            plate.hz = 1.0f;
            auto lift = [&](float cosA, float sinA) {
                AeroAccum acc;
                const float U = 10.0f;
                AccumulateShapeAero(plate, U * cosA, -U * sinA, 0.0f, 0, 0, 0, c, acc);
                // 流れに垂直な成分 (板の上向き側が正)
                return acc.fx * sinA + acc.fy * cosA;
            };
            check(lift(1.0f, 0.0f) == 0.0f, "flat plate: zero lift at alpha = 0 (exactly)");
            check(lift(0.0f, 1.0f) == 0.0f, "flat plate: zero lift at alpha = 90 (exactly)");
            const float l37 = lift(0.8f, 0.6f);  // alpha ~ 36.87 deg
            const float l53 = lift(0.6f, 0.8f);  // alpha ~ 53.13 deg
            MYE_LOG_INFO("  [phys] flat plate lift: 37deg %.3f N / 53deg %.3f N", l37, l53);
            check(l37 > 0.0f && l53 > 0.0f, "flat plate: positive lift between 0 and 90 degrees");
            // sin^2 a * cos a は 54.7 度で最大 → 53 度側が大きい
            check(l53 > l37, "flat plate: lift follows sin^2(a)cos(a) (peaks near 55 degrees)");
        }

        // -- 風見安定の**機構**: 圧力中心が基準点からずれて初めてトルクが出る --
        // ★対称な凸形状に一様流を当てても、幾何中心まわりのトルクは**原理的に厳密 0**。
        //   平らな面にかかる圧力は一様なので合力は必ず面心を通り、面心は法線軸上にある
        //   (r // n // F → 外積 0)。これは近似の粗さではなく物理的に正しい結果で、
        //   実際の風見安定は「圧力中心が質量中心の後ろにある」ことから来る。
        //   本エンジンでは M59d の翼面 (子エンティティに置く = 親の中心からずれる) と
        //   M59f1 の質量中心オフセットがその役を担う。ここではカーネルが
        //   「ずれていればちゃんと復元トルクを出す」ことを直接確かめる
        {
            AeroCoeffs c;
            c.tangentCoeff = 0.0f;
            auto finTorque = [&](float finX) {
                SurfaceElement fin; // 水平尾翼 (法線 +Y)
                fin.px = finX;
                fin.py = 0.0f;
                fin.pz = 0.0f;
                fin.nx = 0.0f;
                fin.ny = 1.0f;
                fin.nz = 0.0f;
                fin.area = 0.2f;
                fin.vx = 20.0f; // +X へ進みながら上へ流れている = 迎角 +
                fin.vy = 2.0f;
                fin.vz = 0.0f;
                AeroAccum acc;
                AccumulateSurfaceElement(fin, c, 0, 0, 0, acc);
                return acc.tz;
            };
            const float tail = finTorque(-1.0f); // 基準点の後ろ = 尾翼
            const float nose = finTorque(1.0f);  // 前 = カナード (不安定化する)
            MYE_LOG_INFO("  [phys] weathercock: tail tz = %.5f / nose tz = %.5f", tail, nose);
            // 機体の +X 軸を流れ (上向きに傾いている) へ合わせるには機首上げ = +Z 回転
            check(tail > 0.0f, "weathercock: a fin behind the reference point restores the axis");
            check(nose < 0.0f, "weathercock: the same fin in front destabilises it (sign flips)");
            check(std::fabs(tail + nose) < 1e-6f,
                  "weathercock: the torque is linear in the lever arm");
        }

        // -- 対称な箱は幾何中心まわりに厳密 0 のトルクしか出さない (上の理由の直接確認) --
        {
            AeroCoeffs c;
            c.tangentCoeff = 0.0f;
            ShapePose dart;
            dart.shape = 1;
            dart.hx = 1.0f;
            dart.hy = 0.05f;
            dart.hz = 0.05f;
            AeroAccum acc;
            AccumulateShapeAero(dart, 20.0f, -2.0f, 0.0f, 0, 0, 0, c, acc);
            check(acc.tx == 0.0f && acc.ty == 0.0f && acc.tz == 0.0f,
                  "aero kernel: a symmetric box has zero torque about its centre at any angle "
                  "(pressure on a flat face is uniform -> the resultant passes through the "
                  "face centre)");
            check(acc.fy > 0.0f,
                  "aero kernel: it still produces lift (the force is what tilts, not the torque)");
        }

        // -- 純関数であること: 2 回実行でビット一致 --
        {
            AeroCoeffs c;
            c.windX = 3.0f;
            ShapePose cap;
            cap.shape = 2;
            cap.radius = 0.4f;
            cap.halfSeg = 0.9f;
            AeroAccum a1, a2;
            AccumulateShapeAero(cap, 5.0f, -2.0f, 1.0f, 0.5f, 1.5f, -0.5f, c, a1);
            AccumulateShapeAero(cap, 5.0f, -2.0f, 1.0f, 0.5f, 1.5f, -0.5f, c, a2);
            check(a1.fx == a2.fx && a1.fy == a2.fy && a1.fz == a2.fz && a1.tx == a2.tx
                      && a1.ty == a2.ty && a1.tz == a2.tz,
                  "aero kernel: the same input yields bit-identical output (pure function)");
        }

        // -- 端の半球の置き換えが軸方向で解析球と一致する (halfSeg=0 のカプセル == 球) --
        {
            AeroCoeffs c;
            c.tangentCoeff = 0.0f;
            const float R = 0.6f;
            ShapePose deg; // 線分長 0 のカプセル = 球
            deg.shape = 2;
            deg.radius = R;
            deg.halfSeg = 0.0f;
            ShapePose sph;
            sph.shape = 0;
            sph.radius = R;
            AeroAccum ac, as;
            AccumulateShapeAero(deg, 0.0f, 9.0f, 0.0f, 0, 0, 0, c, ac); // 軸 (Y) 方向の流れ
            AccumulateShapeAero(sph, 0.0f, 9.0f, 0.0f, 0, 0, 0, c, as);
            check(std::fabs(ac.fy - as.fy) < std::fabs(as.fy) * 0.001f,
                  "aero kernel: the hemisphere-as-disk substitution matches the analytic sphere "
                  "for axial flow");
        }

        // -- ソルバ結線: 面モデルの板は等方近似より遠くまで滑空する --
        {
            auto glide = [&](bool surface, float& outZ, float& outY) {
                Scene s;
                GameObject go = s.CreateGameObjectTracked("Plate");
                go.SetLocalPosition(0, 20.0f, 0);
                auto* col = go.AddComponent<ColliderComponent>();
                col->shape = 1;
                col->isTrigger = false;
                col->halfExtents = { 0.6f, 0.02f, 0.6f };
                auto* rbc = go.AddComponent<RigidbodyComponent>();
                rbc->mass = 0.2f;
                rbc->freezeRotation = true; // 姿勢を固定して「滑空するか」だけを見る
                auto* aero = go.AddComponent<AeroComponent>();
                aero->surfaceModel = surface;
                aero->enableAngularDrag = false;
                s.GetWorld().ApplyStructuralChanges();
                auto* rb = go.GetComponent<RigidbodyComponent>();
                rb->velocity = { 0.0f, 0.0f, 8.0f }; // +Z へ水平に射出
                auto* lt = go.GetComponent<LocalTransform>();
                for (int i = 0; i < 240; ++i) {
                    phys.Update(s.GetWorld(), kDt);
                }
                outZ = lt->position.z;
                outY = lt->position.y;
            };
            float zSurf = 0.0f, ySurf = 0.0f, zIso = 0.0f, yIso = 0.0f;
            glide(true, zSurf, ySurf);
            glide(false, zIso, yIso);
            MYE_LOG_INFO("  [phys] plate glide: surface z=%.2f y=%.2f / isotropic z=%.2f y=%.2f",
                         zSurf, ySurf, zIso, yIso);
            // 水平な板は落下方向 (下面) に大きな面積を向けるので、面モデルの方がよく沈まない
            check(ySurf > yIso, "aero surface: a flat plate falls slower than the isotropic model");
            // 逆に前方投影面積は小さいので前進はよく伸びる
            check(zSurf > zIso, "aero surface: and travels further forward");
        }

        // -- 面モデルでも Aero 非所持ボディはビット不変 --
        {
            auto dumpOne = [](GameObject go, std::vector<uint8_t>& out) {
                out.clear();
                const auto* lt = go.GetComponent<LocalTransform>();
                const auto* rb = go.GetComponent<RigidbodyComponent>();
                const auto* p = reinterpret_cast<const uint8_t*>(&lt->position);
                out.insert(out.end(), p, p + sizeof(lt->position));
                const auto* v = reinterpret_cast<const uint8_t*>(&rb->velocity);
                out.insert(out.end(), v, v + sizeof(rb->velocity));
            };
            Scene sa, sb;
            MakeGround(sa, "G", 0, -0.5f, 0, 5.0f, 0.5f, 5.0f);
            GameObject plainA = MakeBox(sa, "Plain", 0, 3.0f, 0, 0.5f, 0.5f, 0.5f, 0.3f);
            sa.GetWorld().ApplyStructuralChanges();
            MakeGround(sb, "G", 0, -0.5f, 0, 5.0f, 0.5f, 5.0f);
            GameObject plainB = MakeBox(sb, "Plain", 0, 3.0f, 0, 0.5f, 0.5f, 0.5f, 0.3f);
            GameObject wing = MakeBox(sb, "Wing", 40.0f, 6.0f, 0, 1.0f, 0.05f, 1.0f);
            wing.AddComponent<AeroComponent>()->surfaceModel = true;
            sb.GetWorld().ApplyStructuralChanges();
            wing.GetComponent<RigidbodyComponent>()->velocity = { 0.0f, -1.0f, 6.0f };
            bool same = true;
            std::vector<uint8_t> da, db;
            for (int i = 0; i < 240 && same; ++i) {
                phys.Update(sa.GetWorld(), kDt);
                phys.Update(sb.GetWorld(), kDt);
                dumpOne(plainA, da);
                dumpOne(plainB, db);
                if (da.empty() || da != db) {
                    same = false;
                    MYE_LOG_ERROR("  aero surface mix diverged at tick %d", i);
                }
            }
            check(same,
                  "aero surface: bodies without Aero keep their legacy trajectory bit-exactly");
            // 板は対称なのでトルクは出ない (上記) — 「揚力で沈み方が変わった」で空振りでないことを示す
            MYE_LOG_INFO("  [phys] aero mix wing: pos=(%.2f %.2f %.2f) vel=(%.2f %.2f %.2f)",
                         wing.GetComponent<LocalTransform>()->position.x,
                         wing.GetComponent<LocalTransform>()->position.y,
                         wing.GetComponent<LocalTransform>()->position.z,
                         wing.GetComponent<RigidbodyComponent>()->velocity.x,
                         wing.GetComponent<RigidbodyComponent>()->velocity.y,
                         wing.GetComponent<RigidbodyComponent>()->velocity.z);
            // 4 秒間の自由落下なら -39 m/s。面モデルの終端速度 sqrt(mg / (Cn rho A)) ~= 2.9 に
            // 落ち着いていることが「効いている」証拠 (A = 下面 4 m^2)
            check(wing.GetComponent<RigidbodyComponent>()->velocity.y > -4.0f
                      && wing.GetComponent<RigidbodyComponent>()->velocity.y < -2.0f
                      && wing.GetComponent<LocalTransform>()->position.z > 5.0f,
                  "aero surface: the wing reached its own terminal speed (not a vacuous test)");
        }
    }

    // ================= M59d: 翼面 =================
    {
        // 翼を 1 枚だけ持つ機体を作る小道具。panelZ が正 = 重心の前、負 = 後ろ。
        // 姿勢を固定するかどうかを選べる (並進だけ見たいときと復元トルクを見たいときで使い分け)
        struct Craft {
            GameObject body;
            GameObject panel;
        };
        auto makeCraft = [&](Scene& s, float panelZ, float area, bool freezeRot, float vy,
                             float vz) {
            GameObject body = s.CreateGameObjectTracked("Craft");
            body.SetLocalPosition(0, 50.0f, 0);
            auto* col = body.AddComponent<ColliderComponent>();
            col->shape = 1;
            col->isTrigger = false;
            col->halfExtents = { 0.2f, 0.05f, 0.8f };
            auto* rbc = body.AddComponent<RigidbodyComponent>();
            rbc->mass = 0.5f;
            rbc->gravityScale = 0.0f; // 揚力だけを見る (重力は別の試験で確認済み)
            rbc->angularDamping = 0.0f;
            rbc->freezeRotation = freezeRot;
            GameObject panel = s.CreateGameObjectTracked("Panel");
            panel.SetParent(body);
            panel.SetLocalPosition(0.0f, 0.0f, panelZ);
            auto* ps = panel.AddComponent<AeroSurfaceComponent>();
            ps->normal = { 0.0f, 1.0f, 0.0f };
            ps->area = area;
            s.GetWorld().ApplyStructuralChanges();
            auto* rb = body.GetComponent<RigidbodyComponent>();
            rb->velocity = { 0.0f, vy, vz };
            return Craft{ body, panel };
        };

        // -- 揚力: 正の迎角 (前進しながら降下) で上向きの速度を得る --
        {
            Scene s;
            Craft c = makeCraft(s, 0.0f, 0.4f, true, -2.0f, 20.0f);
            auto* rb = c.body.GetComponent<RigidbodyComponent>();
            const float sp0 = std::sqrt(rb->velocity.y * rb->velocity.y
                                        + rb->velocity.z * rb->velocity.z);
            phys.Update(s.GetWorld(), kDt);
            MYE_LOG_INFO("  [phys] wing lift: dvy = %.4f m/s in one tick", rb->velocity.y + 2.0f);
            check(rb->velocity.y > -2.0f, "wing: a positive angle of attack lifts the craft");
            // ★前進成分は**増えうる** — 揚力は流れに垂直なので、前下方から風を受けると
            //   揚力の向きが前へ傾く。これが「降下しながら前へ伸びる」滑空そのもの。
            //   代わりに正しい断言は「揚力は仕事をしない」= 速さは必ず減る
            //   (F.v = q A (cl * 0 - cd |v|) < 0。この試験は重力を切ってある)
            const float sp1 = std::sqrt(rb->velocity.y * rb->velocity.y
                                        + rb->velocity.z * rb->velocity.z);
            check(sp1 < sp0, "wing: lift does no work, so the speed can only drop (drag only)");
        }

        // -- 迎角が逆なら揚力も逆 --
        {
            Scene s;
            Craft c = makeCraft(s, 0.0f, 0.4f, true, 2.0f, 20.0f);
            auto* rb = c.body.GetComponent<RigidbodyComponent>();
            phys.Update(s.GetWorld(), kDt);
            check(rb->velocity.y < 2.0f, "wing: a negative angle of attack pushes the other way");
        }

        // -- 迎角 0 なら揚力 0 (抗力だけ) --
        {
            Scene s;
            Craft c = makeCraft(s, 0.0f, 0.4f, true, 0.0f, 20.0f);
            auto* rb = c.body.GetComponent<RigidbodyComponent>();
            phys.Update(s.GetWorld(), kDt);
            check(rb->velocity.y == 0.0f, "wing: zero angle of attack produces exactly zero lift");
            check(rb->velocity.z < 20.0f, "wing: but still produces parasite drag");
        }

        // -- 失速: 失速角を超えると揚力が落ちる --
        // 速度の大きさを 20 に揃え、sin(alpha) だけを 0.25 (失速前) と 0.40 (失速後) に振る
        {
            auto liftGain = [&](float sinA, float cosA) {
                Scene s;
                Craft c = makeCraft(s, 0.0f, 0.4f, true, -20.0f * sinA, 20.0f * cosA);
                auto* rb = c.body.GetComponent<RigidbodyComponent>();
                const float before = rb->velocity.y;
                phys.Update(s.GetWorld(), kDt);
                return rb->velocity.y - before;
            };
            const float under = liftGain(0.25f, 0.96824584f); // 約 14.5 度 (失速角 15 の手前)
            const float past = liftGain(0.40f, 0.91651514f);  // 約 23.6 度 (失速後)
            MYE_LOG_INFO("  [phys] wing stall: dvy under %.4f -> past %.4f", under, past);
            check(under > 0.0f && past > 0.0f, "wing stall: both angles still lift");
            check(past < under * 0.8f,
                  "wing stall: lift collapses past the stall angle (not the other way around)");
        }

        // -- 風見安定: 尾翼 (重心の後ろ) は姿勢を流れへ戻す / 前翼は逆に振る --
        {
            auto pitchRate = [&](float panelZ) {
                Scene s;
                Craft c = makeCraft(s, panelZ, 0.3f, false, -2.0f, 20.0f);
                auto* rb = c.body.GetComponent<RigidbodyComponent>();
                phys.Update(s.GetWorld(), kDt);
                return rb->angularVelocity.x;
            };
            const float tail = pitchRate(-1.0f);
            const float nose = pitchRate(1.0f);
            MYE_LOG_INFO("  [phys] wing weathercock: tail wx = %.5f / nose wx = %.5f", tail, nose);
            // 機体は +Z へ進みつつ降下中。+X まわりの正回転が機首下げ = 流れへ揃う向き
            check(tail > 0.0f, "wing: a tail panel pitches the craft toward the airflow");
            check(nose < 0.0f, "wing: the same panel in front destabilises it");
        }

        // -- 翼は「最も近い Rigidbody 祖先」に効く (孫でも届く) --
        {
            Scene s;
            GameObject body = s.CreateGameObjectTracked("Body");
            body.SetLocalPosition(0, 50.0f, 0);
            auto* col = body.AddComponent<ColliderComponent>();
            col->shape = 1;
            col->isTrigger = false;
            col->halfExtents = { 0.2f, 0.05f, 0.8f };
            auto* rbc = body.AddComponent<RigidbodyComponent>();
            rbc->mass = 0.5f;
            rbc->gravityScale = 0.0f;
            rbc->freezeRotation = true;
            GameObject mid = s.CreateGameObjectTracked("Pylon");
            mid.SetParent(body);
            GameObject panel = s.CreateGameObjectTracked("Panel");
            panel.SetParent(mid);
            auto* ps = panel.AddComponent<AeroSurfaceComponent>();
            ps->normal = { 0.0f, 1.0f, 0.0f };
            ps->area = 0.4f;
            s.GetWorld().ApplyStructuralChanges();
            auto* rb = body.GetComponent<RigidbodyComponent>();
            rb->velocity = { 0.0f, -2.0f, 20.0f };
            phys.Update(s.GetWorld(), kDt);
            check(rb->velocity.y > -2.0f,
                  "wing: a panel two levels down still drives the nearest rigidbody ancestor");
        }

        // -- 翼を持たないボディは 1 ビットも変わらない --
        {
            auto dumpOne = [](GameObject go, std::vector<uint8_t>& out) {
                out.clear();
                const auto* lt = go.GetComponent<LocalTransform>();
                const auto* rb = go.GetComponent<RigidbodyComponent>();
                const auto* p = reinterpret_cast<const uint8_t*>(&lt->position);
                out.insert(out.end(), p, p + sizeof(lt->position));
                const auto* v = reinterpret_cast<const uint8_t*>(&rb->velocity);
                out.insert(out.end(), v, v + sizeof(rb->velocity));
            };
            Scene sa, sb;
            MakeGround(sa, "G", 0, -0.5f, 0, 5.0f, 0.5f, 5.0f);
            GameObject plainA = MakeBox(sa, "Plain", 0, 3.0f, 0, 0.5f, 0.5f, 0.5f, 0.3f);
            sa.GetWorld().ApplyStructuralChanges();
            MakeGround(sb, "G", 0, -0.5f, 0, 5.0f, 0.5f, 5.0f);
            GameObject plainB = MakeBox(sb, "Plain", 0, 3.0f, 0, 0.5f, 0.5f, 0.5f, 0.3f);
            makeCraft(sb, -1.0f, 0.3f, false, -2.0f, 20.0f); // 遠くない場所でも接触はしない高さ
            bool same = true;
            std::vector<uint8_t> da, db;
            for (int i = 0; i < 240 && same; ++i) {
                phys.Update(sa.GetWorld(), kDt);
                phys.Update(sb.GetWorld(), kDt);
                dumpOne(plainA, da);
                dumpOne(plainB, db);
                if (da.empty() || da != db) {
                    same = false;
                    MYE_LOG_ERROR("  wing mix diverged at tick %d", i);
                }
            }
            check(same, "wing: bodies without AeroSurface keep their legacy trajectory bit-exactly");
        }
    }

    // ================= M59g1: 蓄積インパルスソルバ =================
    {
        // -- 2 体衝突の運動量保存 (解析値) --
        // 反発 e=1 の正面弾性衝突では等質量なら速度が入れ替わる。e=0 なら合体して v/2。
        // どちらでも運動量の総和は保存する — 蓄積インパルスが「押すだけ」を守っている証拠
        {
            auto collide = [&](float e, float& outA, float& outB) {
                Scene s;
                GameObject a = MakeSphereBody(s, "A", -2.0f, 0.0f, 0.0f, 0.5f);
                GameObject b = MakeSphereBody(s, "B", 2.0f, 0.0f, 0.0f, 0.5f);
                s.GetWorld().ApplyStructuralChanges();
                auto* ra = a.GetComponent<RigidbodyComponent>();
                auto* rb = b.GetComponent<RigidbodyComponent>();
                ra->gravityScale = 0.0f;
                rb->gravityScale = 0.0f;
                ra->restitution = e;
                rb->restitution = e;
                ra->velocity = { 4.0f, 0.0f, 0.0f };
                for (int i = 0; i < 120; ++i) {
                    phys.Update(s.GetWorld(), kDt);
                }
                outA = ra->velocity.x;
                outB = rb->velocity.x;
            };
            float ea = 0.0f, eb = 0.0f, ia = 0.0f, ib = 0.0f;
            collide(1.0f, ea, eb);
            collide(0.0f, ia, ib);
            MYE_LOG_INFO("  [phys] 2-body collide: elastic %.4f/%.4f  inelastic %.4f/%.4f", ea,
                         eb, ia, ib);
            check(std::fabs((ea + eb) - 4.0f) < 0.02f,
                  "solver: momentum is conserved in the elastic collision");
            check(std::fabs((ia + ib) - 4.0f) < 0.02f,
                  "solver: momentum is conserved in the inelastic collision");
            check(std::fabs(ea) < 0.2f && std::fabs(eb - 4.0f) < 0.2f,
                  "solver: equal masses exchange velocity when e = 1");
            check(std::fabs(ia - 2.0f) < 0.2f && std::fabs(ib - 2.0f) < 0.2f,
                  "solver: equal masses move together at v/2 when e = 0");
        }

        // -- 高い塔 1200 tick: warm starting を入れるかどうかの計測ゲート (M59h) --
        // 予約事項 1 の「ステートフルバックエンドの箱」を開けるかどうかをここの実測で決める。
        //
        // 実測 (1200 tick 後の最上段 y / 最大水平ドリフト):
        //                        M28b (改装前)          M59g1 (蓄積インパルス)
        //   整列 5 段            4.493 / 0.0000         4.493 / 0.0000   ← 完全一致
        //   整列 10 段           9.416 / 0.0000         9.416 / 0.0000   ← 完全一致
        //   1cm ジグザグ 5 段   -455.3 / 16.38 (貫通)    0.500 / 5.67
        //   1cm ジグザグ 10 段    1.500 / 3.68           0.500 / 5.02
        //
        // ★読み取れること 2 つ:
        //   ① **整列した塔は 10 段でもドリフト厳密 0** で、改装前後で結果が一致する。
        //      つまり M59g1 は「効く場面では何も壊していない」。
        //   ② **1cm ずらすと崩れる**のは改装前からの性質で、しかも旧ソルバは
        //      **床を突き抜けて -455m まで落ちていた**。新ソルバは崩れても床の上に残る。
        //      崩れること自体を直すには warm starting が要る = **M59h でこの箱を開ける**。
        // ここで守るのは「崩れても壊れないこと」(床を抜けない・数値が飛ばない)。
        // M59g1 の途中経過では実際に床を抜けて -975m まで落ちた — 位置補正パスを 8 回
        // 維持することが効いている (4 回に減らすと再現する)
        {
            auto tower = [&](int floors, float jitter, float& outTopY, float& outMaxDrift,
                             float& outMinY) {
                Scene s;
                MakeGround(s, "G", 0, -0.5f, 0, 6.0f, 0.5f, 6.0f);
                std::vector<GameObject> boxes;
                for (int i = 0; i < floors; ++i) {
                    char name[24];
                    std::snprintf(name, sizeof(name), "S%d", i);
                    // わずかにずらして積む (完全に揃えると対称性で不安定さが出ない)
                    boxes.push_back(MakeBox(s, name, jitter * static_cast<float>(i % 3 - 1),
                                            0.5f + 1.0f * static_cast<float>(i), 0.0f, 0.5f, 0.5f,
                                            0.5f));
                }
                s.GetWorld().ApplyStructuralChanges();
                for (int i = 0; i < 1200; ++i) { // 20 秒
                    phys.Update(s.GetWorld(), kDt);
                }
                outMaxDrift = 0.0f;
                outMinY = 1e9f;
                for (int i = 0; i < floors; ++i) {
                    const auto* lt = boxes[static_cast<size_t>(i)].GetComponent<LocalTransform>();
                    const float dx = std::fabs(lt->position.x);
                    const float dz = std::fabs(lt->position.z);
                    const float d = (dx > dz) ? dx : dz;
                    if (d > outMaxDrift) {
                        outMaxDrift = d;
                    }
                    if (lt->position.y < outMinY) {
                        outMinY = lt->position.y;
                    }
                    if (i == floors - 1) {
                        outTopY = lt->position.y;
                    }
                }
            };
            float top5 = 0.0f, drift5 = 0.0f, min5 = 0.0f;
            float top10 = 0.0f, drift10 = 0.0f, min10 = 0.0f;
            float top5j = 0.0f, drift5j = 0.0f, min5j = 0.0f;
            float top10j = 0.0f, drift10j = 0.0f, min10j = 0.0f;
            tower(5, 0.0f, top5, drift5, min5);
            tower(10, 0.0f, top10, drift10, min10);
            tower(5, 0.01f, top5j, drift5j, min5j);
            tower(10, 0.01f, top10j, drift10j, min10j);
            MYE_LOG_INFO("  [phys] tower @1200 aligned : 5-high top %.3f drift %.4f / "
                         "10-high top %.3f drift %.4f  (built at 4.5 / 9.5)",
                         top5, drift5, top10, drift10);
            MYE_LOG_INFO("  [phys] tower @1200 jittered: 5-high top %.3f drift %.4f / "
                         "10-high top %.3f drift %.4f  (1cm zigzag)",
                         top5j, drift5j, top10j, drift10j);
            // 5 段は実用範囲 — ここが崩れたら本物の回帰
            check(top5 > 4.0f, "tower: a 5-high stack is still standing after 1200 ticks");
            check(drift5 < 0.2f, "tower: and has not walked");
            // 10 段は現状の到達点の記録。**崩れること自体は許すが、壊れてはいけない**
            check(min10 > -1.0f && min5j > -1.0f && min10j > -1.0f,
                  "tower (M59h gate): a collapsing stack never tunnels through the floor");
            check(drift10 < 50.0f && drift10j < 50.0f && top10 > -1.0f && top10j > -1.0f,
                  "tower (M59h gate): the collapse stays bounded (no numerical blow-up)");
        }

        // -- 蓄積インパルスの構造的な性質: 接触は押すだけ (負の法線インパルスが出ない) --
        {
            Scene s;
            MakeGround(s, "G", 0, -0.5f, 0, 5.0f, 0.5f, 5.0f);
            MakeBox(s, "B0", 0, 0.5f, 0, 0.5f, 0.5f, 0.5f);
            MakeBox(s, "B1", 0.02f, 1.5f, 0, 0.5f, 0.5f, 0.5f);
            s.GetWorld().ApplyStructuralChanges();
            std::vector<SolidContact> contacts;
            bool everNegative = false;
            for (int i = 0; i < 300; ++i) {
                phys.Update(s.GetWorld(), kDt, &contacts);
                for (const SolidContact& c : contacts) {
                    if (c.impulse < 0.0f) {
                        everNegative = true;
                    }
                }
            }
            check(!everNegative,
                  "solver: the accumulated normal impulse never goes negative (contacts push only)");
            // 2 段タワーの下の接触は 2 箱ぶんの重さを支える = 2 * m * g * dt
            float bottom = 0.0f;
            for (const SolidContact& c : contacts) {
                if ((c.key >> 32) == 0u) { // 床 (index 0) が絡む接触
                    bottom = c.impulse;
                }
            }
            const float expect = 2.0f * 1.0f * 9.81f * kDt;
            MYE_LOG_INFO("  [phys] 2-floor tower bottom impulse = %.5f (expect 2*m*g*dt = %.5f)",
                         bottom, expect);
            // ★これが M59g1 で一番効いた指標。ソルバが収束していないと「支えきれずに
            //   跳ね続ける」状態になり、この値が理論値から大きく外れる (実測: 点毎 Jacobi の
            //   1/count 緩和を残したままだと 8 反復で 88%、緩和を外すと 0.32704 = 100.01%)
            check(std::fabs(bottom - expect) < 0.02f,
                  "solver: the bottom contact carries the weight of both boxes (m*g*dt each)");
            // 静定していること自体も見る (跳ねていれば上の値も揺れる)
            check(std::fabs(s.Find("B1").GetComponent<RigidbodyComponent>()->velocity.y) < 0.02f,
                  "solver: the top box is actually at rest, not bouncing in a limit cycle");
        }
    }

    // ================= M59g2: サブステップ =================
    // 1 tick を substeps 回の「積分 → 制約生成 → 解決 → 位置補正 → 前進」に割る。
    // ★substeps は PhysicsEnvironment のフィールド = **存在ゲートの内側**。env を置いて
    //   いないシーンは 1 固定で M59g1 までと同じ経路を通る。定数にしなかったのは
    //   車両 (M60) が 8 を要求しがちだから (予約事項 4)。
    {
        // -- クランプ: [1, kMaxSubsteps] の外は端へ丸められる --
        {
            auto fall = [&](int substeps) {
                Scene s;
                GameObject envGo = s.CreateGameObjectTracked("Env");
                envGo.AddComponent<PhysicsEnvironmentComponent>()->substeps = substeps;
                GameObject b = MakeBox(s, "F", 0, 10.0f, 0, 0.5f, 0.5f, 0.5f);
                s.GetWorld().ApplyStructuralChanges();
                for (int i = 0; i < 60; ++i) {
                    phys.Update(s.GetWorld(), kDt);
                }
                return b.GetComponent<LocalTransform>()->position.y;
            };
            const float y0 = fall(0), y1 = fall(1), y16 = fall(16), y100 = fall(100);
            check(y0 == y1, "substeps: 0 and below is clamped up to 1 (bit-identical)");
            check(y100 == y16, "substeps: above kMaxSubsteps is clamped down to 16");
            // ★クランプ試験が空転していないこと — 分割すれば自由落下でさえ結果が動く
            //   (半陰的 Euler は h を細かくすると真の解に近づく)
            check(y1 != y16, "substeps: the clamp test is not vacuous (1 and 16 differ)");
        }

        // -- 減衰は「毎 tick の率」: サブステップ数に依らない --
        // ★h ごとに掛けると (1-d)^N になる。substeps を上げただけで減衰が強くなるのは
        //   物理でも意味論でもない — 最初のサブステップで 1 回だけ適用する
        {
            auto damped = [&](int substeps) {
                Scene s;
                GameObject envGo = s.CreateGameObjectTracked("Env");
                auto* e = envGo.AddComponent<PhysicsEnvironmentComponent>();
                e->substeps = substeps;
                e->gravity = { 0.0f, 0.0f, 0.0f };
                GameObject b = MakeBox(s, "D", 0, 0, 0, 0.5f, 0.5f, 0.5f);
                s.GetWorld().ApplyStructuralChanges();
                auto* rb = b.GetComponent<RigidbodyComponent>();
                rb->linearDamping = 0.5f;
                rb->velocity = { 10.0f, 0.0f, 0.0f };
                phys.Update(s.GetWorld(), kDt);
                return rb->velocity.x;
            };
            check(damped(1) == 5.0f && damped(8) == 5.0f && damped(16) == 5.0f,
                  "substeps: linear damping is a per-tick rate, not a per-substep rate");
        }

        // -- 接触インパルスの意味は substeps に依らない (MergeSubstepContacts の契約) --
        // 消費者 (M59e の可視化 / M59k の GetContactInfo) から見た「この tick に入った
        // 法線インパルス」は m*g*dt のまま — サブステップをまたいで**足す**からこうなる。
        // (平均を取ると substeps を上げるだけで報告値が 1/N になり、意味が壊れる)
        {
            auto restingImpulse = [&](int substeps) {
                Scene s;
                GameObject envGo = s.CreateGameObjectTracked("Env");
                auto* renv = envGo.AddComponent<PhysicsEnvironmentComponent>();
                renv->substeps = substeps;
                // ★M59h のスリープを切る — 静止した箱は 60 tick で眠り、眠っている
                //   ペアの報告インパルスは 0 になる (それ自体は正しい)。ここで測りたいのは
                //   「サブステップをまたいで合算しているか」なので、寝かせない
                renv->sleepDelayTicks = 0;
                MakeGround(s, "G", 0, -0.5f, 0, 5.0f, 0.5f, 5.0f);
                MakeBox(s, "B", 0, 0.5f, 0, 0.5f, 0.5f, 0.5f);
                s.GetWorld().ApplyStructuralChanges();
                std::vector<SolidContact> contacts;
                for (int i = 0; i < 120; ++i) {
                    phys.Update(s.GetWorld(), kDt, &contacts);
                }
                return contacts.empty() ? 0.0f : contacts[0].impulse;
            };
            const float i1 = restingImpulse(1), i4 = restingImpulse(4), i16 = restingImpulse(16);
            const float expectI = 9.81f * kDt;
            MYE_LOG_INFO("  [phys] resting impulse vs substeps: 1=%.5f 4=%.5f 16=%.5f "
                         "(expect m*g*dt = %.5f)",
                         i1, i4, i16, expectI);
            check(std::fabs(i1 - expectI) < 0.005f && std::fabs(i4 - expectI) < 0.005f
                      && std::fabs(i16 - expectI) < 0.005f,
                  "substeps: the reported normal impulse stays m*g*dt at any substep count");
        }

        // -- e=1 の反発: 頂点は理想の弾性衝突へ**両側から**近づく --
        // 反発バイアスはマニフォールド生成時の接近速度で 1 回だけ決まる (M59g1)。接触を
        // 検出したときには重力で 1 ステップぶん余計に加速し、貫通も進んでいるので、
        // その速度を丸ごと跳ね返すと**理想より速く**戻る。刻みが細かいほどその過剰が減る。
        // ★実測 (落下 1.5m に対する頂点の回復率): substeps 1 = 108.1% / 4 = 102.7% /
        //   16 = 100.6%。**e=1 で discrete ソルバはエネルギーを足す**のであって引かない —
        //   「跳ね続ける球がだんだん高く上がる」の正体がこれ。判定は保存率そのものではなく
        //   **100% からの乖離が単調に縮むこと**で書く (方向を取り違えると試験が嘘になる)
        {
            // ★**静的コライダーは既存フィールドとしての反発係数を持たない** (M59a2)。
            //   e = min(ea, eb) なので床が 0 のままだと min が 0 になり、球にいくら e=1 を
            //   与えても跳ねない (最初これで頂点が測れず -33% を出した)。材料経由でだけ
            //   静的側が e を主張できる = ここで .physmat を 1 枚仕込む
            PhysMatLibrary* prevBounceLib = physmat::Library();
            PhysMatLibrary bounceLib;
            PhysMat bouncy;
            bouncy.restitution = 1.0f;
            bouncy.dynamicFriction = 0.5f; // Collider 既定と同値 (摩擦は今回の観測対象外)
            const uint64_t hBouncy = bounceLib.Register(L"x\\t_bouncy.physmat.json", bouncy);
            physmat::Install(&bounceLib);
            auto apexRatio = [&](int substeps) {
                Scene s;
                GameObject envGo = s.CreateGameObjectTracked("Env");
                envGo.AddComponent<PhysicsEnvironmentComponent>()->substeps = substeps;
                GameObject ground = MakeGround(s, "G", 0, -0.5f, 0, 5.0f, 0.5f, 5.0f);
                ground.GetComponent<ColliderComponent>()->physMaterial = { hBouncy };
                GameObject ball = MakeSphereBody(s, "Bouncer", 0, 2.0f, 0, 0.5f);
                s.GetWorld().ApplyStructuralChanges();
                auto* rb = ball.GetComponent<RigidbodyComponent>();
                rb->restitution = 1.0f;
                rb->angularDamping = 0.0f;
                const auto* lt = ball.GetComponent<LocalTransform>();
                bool hit = false;
                float apex = 0.0f;
                for (int i = 0; i < 120; ++i) {
                    phys.Update(s.GetWorld(), kDt);
                    if (!hit && rb->velocity.y > 0.0f) {
                        hit = true; // 最初の跳ね返りをとらえてから頂点を探す
                    }
                    if (hit && lt->position.y > apex) {
                        apex = lt->position.y;
                    }
                }
                return (apex - 0.5f) / 1.5f; // 落下高さ 1.5m に対する回復率
            };
            const float r1 = apexRatio(1), r4 = apexRatio(4), r16 = apexRatio(16);
            MYE_LOG_INFO("  [phys] e=1 apex retention: substeps 1=%.1f%% 4=%.1f%% 16=%.1f%%",
                         r1 * 100.0f, r4 * 100.0f, r16 * 100.0f);
            const float e1 = std::fabs(r1 - 1.0f);
            const float e4 = std::fabs(r4 - 1.0f);
            const float e16 = std::fabs(r16 - 1.0f);
            check(e4 < e1 && e16 < e4,
                  "substeps: the e=1 bounce converges monotonically to the ideal elastic collision");
            check(e16 < 0.01f, "substeps: 16 substeps land within 1% of the drop height at e=1");
            physmat::Install(prevBounceLib);
        }

        // -- 剛いバネ: 陽的積分の安定限界 h*omega < 2 をサブステップで買う --
        // k=20000, m=1 なら omega=141 — dt=1/60 では h*omega=2.36 で発散側。
        // substeps=4 (h=1/240) なら 0.59 で安定。これが差分として見える
        // (発散そのものはバネ側の Δv クランプで止まるので、見るのは振幅)
        {
            auto springAmp = [&](int substeps) {
                Scene s;
                GameObject envGo = s.CreateGameObjectTracked("Env");
                auto* e = envGo.AddComponent<PhysicsEnvironmentComponent>();
                e->substeps = substeps;
                e->gravity = { 0.0f, 0.0f, 0.0f };
                GameObject anchor = s.CreateGameObjectTracked("Anchor");
                anchor.SetLocalPosition(0, 0, 0);
                GameObject bob = MakeSphereBody(s, "Bob", 0, -2.1f, 0, 0.3f);
                auto* sj = bob.AddComponent<SpringJointComponent>();
                sj->connectedEntity = anchor.Id();
                sj->restLength = 2.0f;
                sj->stiffness = 20000.0f;
                sj->damping = 0.0f;
                s.GetWorld().ApplyStructuralChanges();
                const auto* lt = bob.GetComponent<LocalTransform>();
                float maxStretch = 0.0f;
                for (int i = 0; i < 300; ++i) {
                    phys.Update(s.GetWorld(), kDt);
                    const float st = std::fabs(-lt->position.y - 2.0f);
                    if (st > maxStretch) {
                        maxStretch = st;
                    }
                }
                return maxStretch;
            };
            const float sa1 = springAmp(1), sa4 = springAmp(4);
            MYE_LOG_INFO("  [phys] stiff spring (k=20000) max stretch: substeps 1=%.4f 4=%.4f "
                         "(initial 0.1)",
                         sa1, sa4);
            check(sa4 < 0.15f, "substeps: 4 substeps keep a k=20000 spring near its initial swing");
            check(sa1 > sa4 * 2.0f, "substeps: the same spring is unusable at 1 substep per tick");
        }

        // -- スタックの定量化 (M59h への引き継ぎ) --
        // M59g1 のゲートで「1cm ジグザグの塔は崩れる」と分かっている。サブステップで
        // 直るのか、warm starting が要るのかをここで測る
        {
            auto tower = [&](int floors, float jitter, int substeps, float& outTopY,
                             float& outMaxDrift, float& outMinY) {
                Scene s;
                GameObject envGo = s.CreateGameObjectTracked("Env");
                envGo.AddComponent<PhysicsEnvironmentComponent>()->substeps = substeps;
                // ★床は M59g1 のゲート (6m) より**大きく** 60m 取る。崩れ方が激しいと箱が
                //   端から転げ落ちて y が -800m まで行き、「床を抜けた」と区別できなくなる
                //   (実測: substeps=4 で水平 93m まで飛んだ)。ここで見たいのは貫通だけ
                MakeGround(s, "G", 0, -0.5f, 0, 60.0f, 0.5f, 60.0f);
                std::vector<GameObject> boxes;
                for (int i = 0; i < floors; ++i) {
                    char name[24];
                    std::snprintf(name, sizeof(name), "S%d", i);
                    boxes.push_back(MakeBox(s, name, jitter * static_cast<float>(i % 3 - 1),
                                            0.5f + 1.0f * static_cast<float>(i), 0.0f, 0.5f, 0.5f,
                                            0.5f));
                }
                s.GetWorld().ApplyStructuralChanges();
                for (int i = 0; i < 1200; ++i) {
                    phys.Update(s.GetWorld(), kDt);
                }
                outMaxDrift = 0.0f;
                outMinY = 1e9f;
                for (int i = 0; i < floors; ++i) {
                    const auto* lt = boxes[static_cast<size_t>(i)].GetComponent<LocalTransform>();
                    const float dx = std::fabs(lt->position.x);
                    const float dz = std::fabs(lt->position.z);
                    const float d = (dx > dz) ? dx : dz;
                    if (d > outMaxDrift) {
                        outMaxDrift = d;
                    }
                    if (lt->position.y < outMinY) {
                        outMinY = lt->position.y;
                    }
                    if (i == floors - 1) {
                        outTopY = lt->position.y;
                    }
                }
            };
            float t1 = 0, d1 = 0, m1 = 0, t4 = 0, d4 = 0, m4 = 0, t8 = 0, d8 = 0, m8 = 0;
            tower(10, 0.01f, 1, t1, d1, m1);
            tower(10, 0.01f, 4, t4, d4, m4);
            tower(10, 0.01f, 8, t8, d8, m8);
            MYE_LOG_INFO("  [phys] jittered 10-high @1200: substeps 1 top %.3f drift %.3f / "
                         "4 top %.3f drift %.3f / 8 top %.3f drift %.3f",
                         t1, d1, t4, d4, t8, d8);
            check(m1 > -1.0f && m4 > -1.0f && m8 > -1.0f,
                  "substeps: a collapsing stack never tunnels through the floor at any count");
            float a1 = 0, ad1 = 0, am1 = 0, a8 = 0, ad8 = 0, am8 = 0;
            tower(10, 0.0f, 1, a1, ad1, am1);
            tower(10, 0.0f, 8, a8, ad8, am8);
            MYE_LOG_INFO("  [phys] aligned 10-high @1200: substeps 1 top %.3f drift %.4f / "
                         "8 top %.3f drift %.4f  (built at 9.5)",
                         a1, ad1, a8, ad8);
            check(a1 > 9.0f && a8 > 9.0f,
                  "substeps: an aligned 10-high stack stands at any substep count");
            check(ad1 == 0.0f && ad8 == 0.0f, "substeps: and does not walk at any substep count");
        }
    }

    // ================= M59f1: ジャイロ項 + 質量中心オフセット =================
    // どちらも Rigidbody の opt-in フィールド。既定 (false / (0,0,0)) では
    // **配線が 1 ビットも動かさない**ことを 2 段階ビルドで確認済み
    // (フィールドだけ足したビルドと配線後のビルドで [phys] ログが完全一致)。
    {
        // ローカルベクトルを姿勢で回す (試験側で重心のワールド位置を出すため)
        auto rotQ = [](const DirectX::XMFLOAT4& q, float vx, float vy, float vz, float& ox, float& oy,
                       float& oz) {
            const float tx = 2.0f * (q.y * vz - q.z * vy);
            const float ty = 2.0f * (q.z * vx - q.x * vz);
            const float tz = 2.0f * (q.x * vy - q.y * vx);
            ox = vx + q.w * tx + (q.y * tz - q.z * ty);
            oy = vy + q.w * ty + (q.z * tx - q.x * tz);
            oz = vz + q.w * tz + (q.x * ty - q.y * tx);
        };

        // -- 球にはジャイロ項が効かない (主軸慣性が全て等しい = J が対角、f が数学的に 0) --
        // 浮動小数の丸めぶんしか動かないことを見る
        {
            Scene s;
            GameObject envGo = s.CreateGameObjectTracked("Env");
            envGo.AddComponent<PhysicsEnvironmentComponent>()->gravity = { 0.0f, 0.0f, 0.0f };
            GameObject ball = MakeSphereBody(s, "G", 0, 0, 0, 0.5f);
            s.GetWorld().ApplyStructuralChanges();
            auto* rb = ball.GetComponent<RigidbodyComponent>();
            rb->gyroscopic = true;
            rb->angularDamping = 0.0f;
            rb->angularVelocity = { 3.0f, 7.0f, -2.0f };
            for (int i = 0; i < 240; ++i) {
                phys.Update(s.GetWorld(), kDt);
            }
            const float dx = rb->angularVelocity.x - 3.0f;
            const float dy = rb->angularVelocity.y - 7.0f;
            const float dz = rb->angularVelocity.z + 2.0f;
            MYE_LOG_INFO("  [phys] gyro sphere drift after 240 ticks: (%.2e, %.2e, %.2e)", dx, dy,
                         dz);
            check(std::fabs(dx) < 1e-4f && std::fabs(dy) < 1e-4f && std::fabs(dz) < 1e-4f,
                  "gyro: a sphere is unaffected (equal principal moments)");
        }

        // -- テニスラケット定理: 中間軸まわりの回転は不安定で符号が反転する --
        // 箱の主軸慣性は I ∝ (hy²+hz², hx²+hz², hx²+hy²)。(0.15, 0.5, 1.0) なら
        // Iz < Iy < Ix なので **Y が中間軸**。わずかな擾乱で軸が反転する
        {
            auto racket = [&](bool gyro) {
                Scene s;
                GameObject envGo = s.CreateGameObjectTracked("Env");
                envGo.AddComponent<PhysicsEnvironmentComponent>()->gravity = { 0.0f, 0.0f, 0.0f };
                GameObject box = MakeBox(s, "R", 0, 0, 0, 0.15f, 0.5f, 1.0f);
                s.GetWorld().ApplyStructuralChanges();
                auto* rb = box.GetComponent<RigidbodyComponent>();
                rb->gyroscopic = gyro;
                rb->angularDamping = 0.0f;
                rb->angularVelocity = { 0.02f, 10.0f, 0.0f }; // 中間軸 + 擾乱
                const auto* lt = box.GetComponent<LocalTransform>();
                bool flipped = false;
                for (int i = 0; i < 900; ++i) {
                    phys.Update(s.GetWorld(), kDt);
                    // ★観測量は**ボディ座標系の** ω₂ — 反転するのは Euler 方程式の
                    //   ω₂ であって、ワールドの y 成分ではない (無トルクなら L は
                    //   ワールドで不動なので、ワールド ω はほとんど符号を変えない)
                    float ax, ay, az;
                    rotQ(lt->rotation, 0.0f, 1.0f, 0.0f, ax, ay, az);
                    const float w2 = rb->angularVelocity.x * ax + rb->angularVelocity.y * ay
                                   + rb->angularVelocity.z * az;
                    if (w2 < 0.0f) {
                        flipped = true;
                    }
                }
                return flipped;
            };
            const bool flipOn = racket(true);
            const bool flipOff = racket(false);
            MYE_LOG_INFO("  [phys] tennis racket: gyro on flipped=%d / off flipped=%d",
                         flipOn ? 1 : 0, flipOff ? 1 : 0);
            check(flipOn, "gyro: the intermediate axis flips (tennis racket theorem)");
            check(!flipOff, "gyro: and stays put with the term off (the test is not vacuous)");
        }

        // -- 角運動量とエネルギー: 陰的形は増えない (陽的だと必ず発散する) --
        // 無トルクなら |L| = |I ω| は保存量。T = 1/2 ω·Iω は陰的形でわずかに減る
        {
            Scene s;
            GameObject envGo = s.CreateGameObjectTracked("Env");
            envGo.AddComponent<PhysicsEnvironmentComponent>()->gravity = { 0.0f, 0.0f, 0.0f };
            GameObject box = MakeBox(s, "L", 0, 0, 0, 0.15f, 0.5f, 1.0f);
            s.GetWorld().ApplyStructuralChanges();
            auto* rb = box.GetComponent<RigidbodyComponent>();
            rb->gyroscopic = true;
            rb->angularDamping = 0.0f;
            rb->angularVelocity = { 0.5f, 9.0f, 0.3f };
            const auto* lt = box.GetComponent<LocalTransform>();
            // I は箱の式そのまま (m=1、h=半辺)
            const float Ii[3] = { (0.5f * 0.5f + 1.0f * 1.0f) / 3.0f,
                                  (0.15f * 0.15f + 1.0f * 1.0f) / 3.0f,
                                  (0.15f * 0.15f + 0.5f * 0.5f) / 3.0f };
            auto measure = [&](float& outL, float& outT) {
                // ワールドの主軸ベクトル (姿勢で回した基底) へ ω を射影して I を掛ける
                float ax[3][3];
                rotQ(lt->rotation, 1, 0, 0, ax[0][0], ax[0][1], ax[0][2]);
                rotQ(lt->rotation, 0, 1, 0, ax[1][0], ax[1][1], ax[1][2]);
                rotQ(lt->rotation, 0, 0, 1, ax[2][0], ax[2][1], ax[2][2]);
                float L[3] = { 0, 0, 0 };
                outT = 0.0f;
                for (int k = 0; k < 3; ++k) {
                    const float wk = rb->angularVelocity.x * ax[k][0]
                                   + rb->angularVelocity.y * ax[k][1]
                                   + rb->angularVelocity.z * ax[k][2];
                    const float Lk = Ii[k] * wk;
                    L[0] += Lk * ax[k][0];
                    L[1] += Lk * ax[k][1];
                    L[2] += Lk * ax[k][2];
                    outT += 0.5f * Ii[k] * wk * wk;
                }
                outL = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
            };
            float l0 = 0, t0 = 0;
            measure(l0, t0);
            float maxT = t0;
            float l240 = 0, t240 = 0;
            for (int i = 0; i < 900; ++i) {
                phys.Update(s.GetWorld(), kDt);
                float lc = 0, tc = 0;
                measure(lc, tc);
                if (tc > maxT) {
                    maxT = tc;
                }
                if (i == 239) {
                    l240 = lc;
                    t240 = tc;
                }
            }
            float l1 = 0, t1 = 0;
            measure(l1, t1);
            MYE_LOG_INFO("  [phys] gyro |L| %.5f -> %.5f @240 -> %.5f @900 (peak T %.5f of %.5f)",
                         l0, l240, l1, maxT, t0);
            // ★陰的**中点**なのでこの系の二次不変量が保たれる。実測の |L| ドリフトは
            //   900 tick (15 秒) で 0.006% — 後退 Euler (1 反復) だと同条件で
            //   **4 秒に 7.3% / 15 秒に 23% 落ちた**ので、桁が 3 つ違う。
            //   シンプレクティックな解法はエネルギーが**振動する**(単調減少ではない)
            //   ので、断言は「増え続けない = 有界」で書く
            check(std::fabs(l240 - l0) < l0 * 0.001f,
                  "gyro: |L| is conserved over 4 s of torque-free tumbling");
            check(std::fabs(l1 - l0) < l0 * 0.001f, "gyro: and still at 15 s (no secular drift)");
            check(maxT <= t0 * 1.001f,
                  "gyro: the rotational energy stays bounded (it oscillates, it does not grow)");
        }

        // -- 混在: gyroscopic を持たないボディの軌跡は 1 ビットも変わらない --
        {
            auto dumpOne = [](GameObject go, std::vector<uint8_t>& out) {
                out.clear();
                const auto* lt = go.GetComponent<LocalTransform>();
                const auto* rb = go.GetComponent<RigidbodyComponent>();
                auto push = [&](const void* p, size_t n) {
                    const auto* b = static_cast<const uint8_t*>(p);
                    out.insert(out.end(), b, b + n);
                };
                push(&lt->position, sizeof(lt->position));
                push(&lt->rotation, sizeof(lt->rotation));
                push(&rb->angularVelocity, sizeof(rb->angularVelocity));
            };
            Scene sa, sb;
            GameObject plainA = MakeBox(sa, "Plain", 0, 3.0f, 0, 0.15f, 0.5f, 1.0f);
            sa.GetWorld().ApplyStructuralChanges();
            plainA.GetComponent<RigidbodyComponent>()->angularVelocity = { 0.02f, 10.0f, 0.0f };
            GameObject plainB = MakeBox(sb, "Plain", 0, 3.0f, 0, 0.15f, 0.5f, 1.0f);
            GameObject spun = MakeBox(sb, "Spun", 40.0f, 3.0f, 0, 0.15f, 0.5f, 1.0f);
            sb.GetWorld().ApplyStructuralChanges();
            plainB.GetComponent<RigidbodyComponent>()->angularVelocity = { 0.02f, 10.0f, 0.0f };
            auto* srb = spun.GetComponent<RigidbodyComponent>();
            srb->gyroscopic = true;
            srb->angularDamping = 0.0f;
            srb->angularVelocity = { 0.02f, 10.0f, 0.0f };
            bool same = true;
            std::vector<uint8_t> da, db;
            for (int i = 0; i < 240 && same; ++i) {
                phys.Update(sa.GetWorld(), kDt);
                phys.Update(sb.GetWorld(), kDt);
                dumpOne(plainA, da);
                dumpOne(plainB, db);
                if (da.empty() || da != db) {
                    same = false;
                    MYE_LOG_ERROR("  gyro mix diverged at tick %d", i);
                }
            }
            check(same, "gyro: a body without the flag keeps its legacy trajectory bit-exactly "
                        "next to a gyroscopic one (240 ticks)");
        }

        // ================= 質量中心オフセット =================

        // -- 回転の中心は形状原点ではなく質量中心 --
        // 重力・接触なしで ω だけ与えると、質量中心は動かず**形状原点がその周りを回る**
        {
            Scene s;
            GameObject envGo = s.CreateGameObjectTracked("Env");
            envGo.AddComponent<PhysicsEnvironmentComponent>()->gravity = { 0.0f, 0.0f, 0.0f };
            GameObject box = MakeBox(s, "C", 0, 0, 0, 0.5f, 0.5f, 0.5f);
            s.GetWorld().ApplyStructuralChanges();
            auto* rb = box.GetComponent<RigidbodyComponent>();
            rb->centerOfMass = { 0.5f, 0.0f, 0.0f };
            rb->angularDamping = 0.0f;
            rb->angularVelocity = { 0.0f, 2.0f, 0.0f };
            const auto* lt = box.GetComponent<LocalTransform>();
            float maxComDrift = 0.0f;
            float minRadErr = 1e9f, maxRadErr = 0.0f;
            float maxOriginX = -1e9f, minOriginX = 1e9f;
            for (int i = 0; i < 240; ++i) {
                phys.Update(s.GetWorld(), kDt);
                float ox, oy, oz;
                rotQ(lt->rotation, 0.5f, 0.0f, 0.0f, ox, oy, oz);
                const float cx = lt->position.x + ox;
                const float cy = lt->position.y + oy;
                const float cz = lt->position.z + oz;
                const float d = std::sqrt((cx - 0.5f) * (cx - 0.5f) + cy * cy + cz * cz);
                if (d > maxComDrift) {
                    maxComDrift = d;
                }
                const float r = std::sqrt(ox * ox + oy * oy + oz * oz);
                const float e = std::fabs(r - 0.5f);
                if (e < minRadErr) {
                    minRadErr = e;
                }
                if (e > maxRadErr) {
                    maxRadErr = e;
                }
                if (lt->position.x > maxOriginX) {
                    maxOriginX = lt->position.x;
                }
                if (lt->position.x < minOriginX) {
                    minOriginX = lt->position.x;
                }
            }
            MYE_LOG_INFO("  [phys] com spin: centre drift %.2e, radius err %.2e, origin x in "
                         "[%.4f, %.4f]",
                         maxComDrift, maxRadErr, minOriginX, maxOriginX);
            check(maxComDrift < 1e-3f, "com: the centre of mass stays put under pure rotation");
            check(maxRadErr < 1e-3f, "com: the shape origin keeps its distance from it");
            // 質量中心は (0.5,0,0) なので形状原点は半径 0.5 の円 = x が [0, 1] を舐める
            check(minOriginX < 0.01f && maxOriginX > 0.99f,
                  "com: and actually swings a full circle around it (not vacuous)");
        }

        // -- 重力は質量中心に効く = オフセットがあっても自由落下は回らない --
        {
            Scene s;
            GameObject box = MakeBox(s, "F", 0, 10.0f, 0, 0.5f, 0.5f, 0.5f);
            s.GetWorld().ApplyStructuralChanges();
            auto* rb = box.GetComponent<RigidbodyComponent>();
            rb->centerOfMass = { 0.4f, -0.3f, 0.2f };
            for (int i = 0; i < 120; ++i) {
                phys.Update(s.GetWorld(), kDt);
            }
            check(rb->angularVelocity.x == 0.0f && rb->angularVelocity.y == 0.0f
                      && rb->angularVelocity.z == 0.0f,
                  "com: gravity acts at the centre of mass, so free fall never spins up");
        }

        // -- 接触インパルスの腕が質量中心から測られる --
        // ★被写体は**球**にする。箱だと 4 点マニフォールドの点毎 Jacobi が不均衡を
        //   打ち消してしまい、腕の効果が正味の回転として出てこない (実測 0.0168 で
        //   符号も直感と逆になった)。球なら接触点が 1 つ = τ = r × j がそのまま出る
        {
            auto landSpin = [&](float comX) {
                Scene s;
                MakeGround(s, "G", 0, -0.5f, 0, 5.0f, 0.5f, 5.0f);
                GameObject ball = MakeSphereBody(s, "B", 0, 2.0f, 0, 0.5f);
                s.GetWorld().ApplyStructuralChanges();
                auto* rb = ball.GetComponent<RigidbodyComponent>();
                rb->centerOfMass = { comX, 0.0f, 0.0f };
                rb->angularDamping = 0.0f;
                std::vector<SolidContact> cs;
                for (int i = 0; i < 120; ++i) {
                    phys.Update(s.GetWorld(), kDt, &cs);
                    if (!cs.empty()) {
                        return rb->angularVelocity.z; // 接触した最初の tick で見る
                    }
                }
                return 0.0f;
            };
            const float w0 = landSpin(0.0f);
            const float wp = landSpin(0.4f);
            const float wn = landSpin(-0.4f);
            MYE_LOG_INFO("  [phys] landing spin wz: com 0 = %.4f / +X = %.4f / -X = %.4f", w0, wp,
                         wn);
            check(w0 == 0.0f, "com: a centred sphere lands without spin (legacy path)");
            check(std::fabs(wp) > 0.01f,
                  "com: an offset centre of mass turns the landing impulse into a spin");
            check(wp == -wn, "com: and the sign follows the offset");
        }

        // -- 浮力の復原モーメント: 重心が低い浮体は傾きを戻す (M59b2 の予告の回収) --
        {
            auto tiltAfter = [&](float comY) {
                Scene s;
                GameObject wgo = s.CreateGameObjectTracked("Water");
                wgo.AddComponent<PhysicsEnvironmentComponent>()->waterPlaneY = 0.0f;
                GameObject raft = MakeBox(s, "Raft", 0, 0.0f, 0, 1.0f, 0.25f, 1.0f);
                raft.AddComponent<BuoyancyComponent>();
                s.GetWorld().ApplyStructuralChanges();
                auto* rb = raft.GetComponent<RigidbodyComponent>();
                rb->mass = 500.0f; // 体積 1.0 m^3 の半分だけ沈む重さ
                rb->centerOfMass = { 0.0f, comY, 0.0f };
                // Z 軸まわりに 20 度傾けて放す
                auto* lt = raft.GetComponent<LocalTransform>();
                lt->rotation = { 0.0f, 0.0f, 0.17364818f, 0.98480775f };
                float maxTilt = 0.17364818f;
                for (int i = 0; i < 900; ++i) {
                    phys.Update(s.GetWorld(), kDt);
                    const float t = std::fabs(lt->rotation.z);
                    if (t > maxTilt) {
                        maxTilt = t;
                    }
                }
                return std::fabs(lt->rotation.z);
            };
            const float flat = tiltAfter(0.0f);
            const float low = tiltAfter(-0.2f);
            MYE_LOG_INFO("  [phys] raft |sin(tilt/2)| after 900: com centred %.5f / lowered %.5f "
                         "(released at 0.17365)",
                         flat, low);
            check(flat == 0.17364818f,
                  "buoyancy: a centred centre of mass still produces no righting moment (M59b2)");
            check(low < 0.17364818f * 0.5f,
                  "buoyancy: lowering the centre of mass rights the raft (M59f1 cashes the note)");
        }

        // -- 面サンプリング空力: 重心オフセットが圧力中心とのずれを作り風見安定が出る --
        // M59c で「対称形状にトルクが出ないのは正しい物理」と結論した件の続き。
        // 圧力中心 (形状中心) と質量中心がずれれば、式を変えずにモーメントが立つ
        {
            auto weathercock = [&](float comZ) {
                Scene s;
                GameObject envGo = s.CreateGameObjectTracked("Env");
                auto* e = envGo.AddComponent<PhysicsEnvironmentComponent>();
                e->gravity = { 0.0f, 0.0f, 0.0f };
                GameObject plate = MakeBox(s, "P", 0, 0, 0, 1.0f, 0.05f, 1.0f);
                auto* aero = plate.AddComponent<AeroComponent>();
                aero->surfaceModel = true;
                s.GetWorld().ApplyStructuralChanges();
                auto* rb = plate.GetComponent<RigidbodyComponent>();
                rb->angularDamping = 0.0f;
                rb->centerOfMass = { 0.0f, 0.0f, comZ };
                rb->velocity = { 0.0f, -6.0f, 0.0f }; // 平板を面で押す流れ
                phys.Update(s.GetWorld(), kDt);
                return rb->angularVelocity.x;
            };
            const float w0 = weathercock(0.0f);
            const float wb = weathercock(-0.8f); // 重心を後ろへ
            const float wf = weathercock(0.8f);
            MYE_LOG_INFO("  [phys] aero weathercock via com: centred %.5f / back %.5f / front %.5f",
                         w0, wb, wf);
            check(w0 == 0.0f, "aero: a symmetric plate about its own centre still gets no torque");
            check(std::fabs(wb) > 1e-3f && wb == -wf,
                  "aero: offsetting the centre of mass produces an equal and opposite moment");
        }
    }

    // ================= M59f2: 静止/動摩擦の分離 + 転がり抵抗 =================
    // どちらも **.physmat 経由でしか有効にならない**。材料未割当のコライダーは
    // μs = μd = Collider.friction / Crr = 0 になり、新しい分岐が従来の 1 本の
    // クランプへ畳まれる (実測でも既存 32 行の [phys] ログが 1 ビットも動かなかった)。
    {
        PhysMatLibrary* prevFricLib = physmat::Library();
        PhysMatLibrary fricLib;

        // -- 純関数の優先順位 (材料 → 既存フィールド → override ビット) --
        {
            ColliderComponent col;
            col.friction = 0.3f;
            PhysMat m;
            m.staticFriction = 0.9f;
            m.dynamicFriction = 0.2f;
            m.rollingResistance = 0.05f;
            check(SelectStaticFriction(col, &m) == 0.9f,
                  "physmat: static friction comes from the material");
            check(SelectRollingResistance(col, &m) == 0.05f,
                  "physmat: rolling resistance comes from the material");
            // ★材料が無ければ μs は **μd と同じ既存フィールド** — これが未割当シーンの
            //   ビット同一の根拠 (μs == μd なら静止/動の分岐が消える)
            check(SelectStaticFriction(col, nullptr) == SelectFriction(col, nullptr),
                  "physmat: without a material the static and dynamic coefficients coincide");
            check(SelectRollingResistance(col, nullptr) == 0.0f,
                  "physmat: without a material there is no rolling resistance");
            col.materialOverrideBits = kPhysMatOverrideFriction;
            check(SelectStaticFriction(col, &m) == 0.3f && SelectFriction(col, &m) == 0.3f,
                  "physmat: the friction override bit covers both coefficients");
            col.materialOverrideBits = kPhysMatOverrideRolling;
            check(SelectRollingResistance(col, &m) == 0.0f,
                  "physmat: the rolling override bit turns it off without dropping the material");
            check(SelectStaticFriction(col, &m) == 0.9f,
                  "physmat: and leaves the friction coefficients alone");
        }

        physmat::Install(&fricLib);

        // -- 斜面のヒステリシス: μd < tanθ < μs --
        // θ = 26.565 度 (tanθ = 0.5)、材料は μs=0.9 / μd=0.2。
        // **静止からは動かないが、いちど滑り出すと止まらない**帯に入る
        {
            PhysMat grip;
            grip.staticFriction = 0.9f;
            grip.dynamicFriction = 0.2f;
            const uint64_t hGrip = fricLib.Register(L"x\\t_grip.physmat.json", grip);
            // Z 軸まわり +26.565 度。斜面法線は (-0.44721, 0.89443, 0)、下りは -X
            const DirectX::XMFLOAT4 q = { 0.0f, 0.0f, 0.22975292f, 0.97325428f };
            auto slide = [&](bool withMaterial, float pushSpeed) {
                Scene s;
                GameObject slope =
                    MakeStaticBoxRot(s, "Slope", 0, 0, 0, 5.0f, 0.5f, 5.0f, q);
                GameObject blk = MakeBox(s, "Blk", -0.33987f, 0.67976f, 0, 0.25f, 0.25f, 0.25f);
                blk.GetComponent<LocalTransform>()->rotation = q;
                if (withMaterial) {
                    slope.GetComponent<ColliderComponent>()->physMaterial = { hGrip };
                    blk.GetComponent<ColliderComponent>()->physMaterial = { hGrip };
                } else {
                    // 対照区: **動摩擦だけ**を材料と同じ 0.2 にした従来経路
                    slope.GetComponent<ColliderComponent>()->friction = 0.2f;
                    blk.GetComponent<ColliderComponent>()->friction = 0.2f;
                }
                s.GetWorld().ApplyStructuralChanges();
                auto* rb = blk.GetComponent<RigidbodyComponent>();
                rb->freezeRotation = true; // 転倒を封じて「滑ったか」だけを見る
                rb->velocity = { -0.89443f * pushSpeed, -0.44721f * pushSpeed, 0.0f };
                const auto* lt = blk.GetComponent<LocalTransform>();
                for (int i = 0; i < 60; ++i) {
                    phys.Update(s.GetWorld(), kDt);
                }
                return lt->position.x - (-0.33987f); // 下り (-X) なら負
            };
            const float legacy = slide(false, 0.0f);
            const float held = slide(true, 0.0f);
            const float pushed = slide(true, 1.0f);
            MYE_LOG_INFO("  [phys] slope dx after 60 ticks: legacy(mu=0.2) %.4f / "
                         "material at rest %.4f / material pushed %.4f",
                         legacy, held, pushed);
            check(legacy < -0.3f, "friction: with only a dynamic coefficient the block slides");
            check(std::fabs(held) < 0.02f,
                  "friction: the static coefficient holds the same block at rest (mu_d < tan < mu_s)");
            check(pushed < -0.5f, "friction: but once it is moving it keeps sliding (hysteresis)");
        }

        // -- 転がり抵抗: 球が完全に止まる (M59h のスリープが要求する前提) --
        // 転がり抵抗は**純粋な角インパルス**なので、まず ω が枯れ、その滑りを摩擦が
        // 受けて v も落ちる。角減衰は 0 にして「非物理の定率減衰」を排除してある
        {
            auto roll = [&](float crr, float& outSpeed, float& outSpin) {
                Scene s;
                MakeGround(s, "G", 0, -0.5f, 0, 30.0f, 0.5f, 5.0f);
                GameObject ball = MakeSphereBody(s, "Roller", -10.0f, 0.5f, 0, 0.5f);
                if (crr > 0.0f) {
                    PhysMat m;
                    m.rollingResistance = crr;
                    char name[64];
                    std::snprintf(name, sizeof(name), "t_roll_%d", static_cast<int>(crr * 1000.0f));
                    wchar_t wname[64];
                    for (int i = 0; i < 64; ++i) {
                        wname[i] = static_cast<wchar_t>(name[i]);
                        if (name[i] == 0) {
                            break;
                        }
                    }
                    ball.GetComponent<ColliderComponent>()->physMaterial = { fricLib.Register(
                        wname, m) };
                }
                s.GetWorld().ApplyStructuralChanges();
                auto* rb = ball.GetComponent<RigidbodyComponent>();
                rb->angularDamping = 0.0f;
                rb->velocity = { 3.0f, 0.0f, 0.0f };
                for (int i = 0; i < 900; ++i) {
                    phys.Update(s.GetWorld(), kDt);
                }
                outSpeed = std::fabs(rb->velocity.x);
                outSpin = std::fabs(rb->angularVelocity.z);
            };
            float v0 = 0, w0 = 0, v1 = 0, w1 = 0;
            roll(0.0f, v0, w0);
            roll(0.05f, v1, w1);
            MYE_LOG_INFO("  [phys] rolling @900: Crr=0 v %.4f w %.4f / Crr=0.05 v %.4f w %.4f",
                         v0, w0, v1, w1);
            check(v0 > 2.0f, "rolling: without resistance a rolling ball keeps its speed");
            check(v1 < 0.05f && w1 < 0.05f,
                  "rolling: resistance brings it to a complete stop (both v and omega)");
        }

        physmat::Install(prevFricLib);
    }

    // ================= M59h: スリープ + アイランド =================
    // ★閾値は PhysicsEnvironment に置いてある = **存在ゲート**。env が無いシーンは
    //   1 体も眠らず、M59f2 までとビット同一の経路を通る。
    // ★状態 (sleepTicks / isSleeping) は Rigidbody 側 = hash / JSON / SimSnapshot /
    //   DLL 移行がフィールド表の既存機構で自動被覆される。
    {
        // 「床 + 箱 n 段」の共通シーン。delay<=0 ならスリープ無効
        auto makeStack = [&](Scene& s, int floors, int delay) {
            GameObject envGo = s.CreateGameObjectTracked("Env");
            auto* e = envGo.AddComponent<PhysicsEnvironmentComponent>();
            e->sleepDelayTicks = delay;
            MakeGround(s, "G", 0, -0.5f, 0, 6.0f, 0.5f, 6.0f);
            std::vector<GameObject> boxes;
            for (int i = 0; i < floors; ++i) {
                char name[24];
                std::snprintf(name, sizeof(name), "B%d", i);
                boxes.push_back(
                    MakeBox(s, name, 0, 0.5f + 1.0f * static_cast<float>(i), 0, 0.5f, 0.5f, 0.5f));
            }
            s.GetWorld().ApplyStructuralChanges();
            return boxes;
        };

        // -- 入眠する / 速度が厳密 0 になる / ワールドハッシュが完全に止まる --
        {
            Scene s;
            auto boxes = makeStack(s, 1, 60);
            auto* rb = boxes[0].GetComponent<RigidbodyComponent>();
            int sleptAt = -1;
            for (int i = 0; i < 300; ++i) {
                phys.Update(s.GetWorld(), kDt);
                if (sleptAt < 0 && rb->isSleeping) {
                    sleptAt = i;
                }
            }
            MYE_LOG_INFO("  [phys] sleep: a resting box fell asleep at tick %d", sleptAt);
            check(sleptAt >= 0, "sleep: a resting box eventually falls asleep");
            // ★厳密 0 を要求する — 「ほぼ 0」だと残留ビットが構成差を運ぶ余地が残る
            check(rb->velocity.x == 0.0f && rb->velocity.y == 0.0f && rb->velocity.z == 0.0f
                      && rb->angularVelocity.x == 0.0f && rb->angularVelocity.y == 0.0f
                      && rb->angularVelocity.z == 0.0f,
                  "sleep: the velocities are written as exact zeros");
            // 眠っているあいだハッシュが 1 ビットも動かない (sleepTicks の据え置きも込み)
            const uint64_t h0 = HashWorld(s.GetWorld(), nullptr);
            bool frozen = true;
            for (int i = 0; i < 120; ++i) {
                phys.Update(s.GetWorld(), kDt);
                if (HashWorld(s.GetWorld(), nullptr) != h0) {
                    frozen = false;
                }
            }
            check(frozen, "sleep: the world hash stays bit-identical for 120 ticks while asleep");
        }

        // -- 閾値 0 = スリープ無効 (導入前と同じ経路) --
        {
            Scene s;
            auto boxes = makeStack(s, 1, 0);
            auto* rb = boxes[0].GetComponent<RigidbodyComponent>();
            for (int i = 0; i < 300; ++i) {
                phys.Update(s.GetWorld(), kDt);
            }
            check(!rb->isSleeping && rb->sleepTicks == 0,
                  "sleep: sleepDelayTicks <= 0 disables the feature outright");
        }

        // -- 島単位: 3 段の塔は**全員そろって**眠る --
        // 1 体ずつ眠らせると、上がまだ動いているのに土台が眠って次 tick に起こされる
        // 「まばたき」が出る。島の最小カウンタで揃えるのが要点
        {
            Scene s;
            auto boxes = makeStack(s, 3, 60);
            int firstSleep[3] = { -1, -1, -1 };
            for (int i = 0; i < 600; ++i) {
                phys.Update(s.GetWorld(), kDt);
                for (int k = 0; k < 3; ++k) {
                    if (firstSleep[k] < 0
                        && boxes[static_cast<size_t>(k)]
                               .GetComponent<RigidbodyComponent>()
                               ->isSleeping) {
                        firstSleep[k] = i;
                    }
                }
            }
            MYE_LOG_INFO("  [phys] sleep island: 3-high stack fell asleep at ticks %d/%d/%d",
                         firstSleep[0], firstSleep[1], firstSleep[2]);
            check(firstSleep[0] >= 0 && firstSleep[1] >= 0 && firstSleep[2] >= 0,
                  "sleep: a 3-high stack falls asleep");
            check(firstSleep[0] == firstSleep[1] && firstSleep[1] == firstSleep[2],
                  "sleep: and the whole island goes down on the same tick");
        }

        // -- 静的な床の隣で眠り続ける (床は起床トリガにならない) --
        {
            Scene s;
            auto boxes = makeStack(s, 1, 60);
            auto* rb = boxes[0].GetComponent<RigidbodyComponent>();
            for (int i = 0; i < 200; ++i) {
                phys.Update(s.GetWorld(), kDt);
            }
            bool stayed = rb->isSleeping;
            for (int i = 0; i < 600; ++i) {
                phys.Update(s.GetWorld(), kDt);
                if (!rb->isSleeping) {
                    stayed = false;
                }
            }
            check(stayed, "sleep: a static floor never wakes the body resting on it");
        }

        // -- 眠っているあいだも接触ペアは報告され続ける (OnCollisionExit の誤発火防止) --
        // ★力積は 0 で報告する — 眠っている tick に交換した力積は本当に無いので
        {
            Scene s;
            auto boxes = makeStack(s, 1, 60);
            auto* rb = boxes[0].GetComponent<RigidbodyComponent>();
            std::vector<SolidContact> cs;
            for (int i = 0; i < 200; ++i) {
                phys.Update(s.GetWorld(), kDt, &cs);
            }
            check(rb->isSleeping, "sleep: (setup) the box is asleep");
            check(cs.size() == 1, "sleep: the contact pair is still reported while asleep");
            check(!cs.empty() && cs[0].impulse == 0.0f,
                  "sleep: and reports exactly zero impulse for that tick");
        }

        // -- 投擲物で起きる --
        {
            Scene s;
            auto boxes = makeStack(s, 1, 60);
            GameObject ball = MakeSphereBody(s, "Ball", -4.0f, 0.5f, 0, 0.3f);
            s.GetWorld().ApplyStructuralChanges();
            // ★ポインタは**最後の構造変更のあと**に取る — 球は箱と同じアーキタイプに
            //   入るので、追加で列が再確保されると先に取った箱のポインタが死ぬ
            //   (最初これで「箱は動いたのに isSleeping が false にならない」を出した)
            auto* rb = boxes[0].GetComponent<RigidbodyComponent>();
            auto* brb = ball.GetComponent<RigidbodyComponent>();
            brb->mass = 5.0f;
            for (int i = 0; i < 200; ++i) {
                phys.Update(s.GetWorld(), kDt);
            }
            const bool wasAsleep = rb->isSleeping;
            brb->isSleeping = false; // 球も眠っているので投げる前に起こす
            brb->velocity = { 12.0f, 0.0f, 0.0f };
            // ★起床は AABB + margin の近接で起きるので、覚醒した瞬間はまだ触れていない。
            //   最後まで回してから変位を見る (早期 break だと「起きたが動いていない」になる)
            bool woke = false;
            for (int i = 0; i < 120; ++i) {
                phys.Update(s.GetWorld(), kDt);
                if (!rb->isSleeping) {
                    woke = true;
                }
            }
            check(wasAsleep, "sleep: (setup) the target was asleep before the throw");
            check(woke, "sleep: a moving body wakes what it touches");
            check(std::fabs(boxes[0].GetComponent<LocalTransform>()->position.x) > 0.01f,
                  "sleep: and the woken body actually moves again");
        }
    }

    // ================= M59i: 地形ハイトフィールドコライダー =================
    // Collider.shape=4。**実体は sim 専用のライブラリ**から引く (描画側の TerrainSystem の
    // キャッシュを読むと「絵を出したかどうか」で sim が変わる)。
    // 三角形の集まりという一点で M41 の静的メッシュと同じ扱いにしてあり、
    // 衝突・マニフォールド・最近点の本体は共有、候補の集め方だけ差し替えている。
    {
        TerrainColliderLibrary* prevTerrLib = terraincol::Library();
        TerrainColliderLibrary terrLib;
        terraincol::Install(&terrLib);

        // 解析ハイトフィールドを組む。h(u,v) は正規化 [0,1] を返す純関数
        auto makeTerrain = [&](uint32_t n, float sizeX, float sizeZ, float base, float scale,
                               float (*h)(float, float)) {
            TerrainAsset::TerrainData d;
            d.heightW = n;
            d.heightH = n;
            d.splatW = 1;
            d.splatH = 1;
            d.worldSizeX = sizeX;
            d.worldSizeZ = sizeZ;
            d.heightBase = base;
            d.heightScale = scale;
            d.heights.resize(static_cast<size_t>(n) * n);
            for (uint32_t z = 0; z < n; ++z) {
                for (uint32_t x = 0; x < n; ++x) {
                    const float u = static_cast<float>(x) / static_cast<float>(n - 1);
                    const float v = static_cast<float>(z) / static_cast<float>(n - 1);
                    float t = h(u, v);
                    if (t < 0.0f) { t = 0.0f; }
                    if (t > 1.0f) { t = 1.0f; }
                    d.heights[static_cast<size_t>(z) * n + x] =
                        static_cast<uint16_t>(t * 65535.0f + 0.5f);
                }
            }
            d.splat.assign(4, 0);
            d.splat[0] = 255;
            return d;
        };
        // 地形エンティティ (静的コライダー only) をシーンへ足す
        auto addTerrain = [&](Scene& s, const char* name, AssetID id, TerrainAsset::TerrainData d) {
            terrLib.Register(id, std::move(d));
            GameObject go = s.CreateGameObjectTracked(name);
            go.SetLocalPosition(0, 0, 0);
            auto* col = go.AddComponent<ColliderComponent>();
            col->shape = 4;
            col->meshAsset = id;
            return go;
        };

        // -- 平坦な地形: 球が高さ h の面に静定する --
        {
            Scene s;
            const AssetID tid{ 0x7e11a10001ull };
            addTerrain(s, "Flat", tid,
                       makeTerrain(9, 20.0f, 20.0f, 0.0f, 10.0f,
                                   [](float, float) { return 0.25f; })); // 高さ 2.5m 一定
            GameObject ball = MakeSphereBody(s, "Ball", 0.7f, 8.0f, -1.3f, 0.5f);
            s.GetWorld().ApplyStructuralChanges();
            const auto* lt = ball.GetComponent<LocalTransform>();
            for (int i = 0; i < 300; ++i) {
                phys.Update(s.GetWorld(), kDt);
            }
            MYE_LOG_INFO("  [phys] terrain flat: ball settled at y = %.4f (expect 3.0)",
                         lt->position.y);
            check(std::fabs(lt->position.y - 3.0f) < 0.02f,
                  "terrain: a sphere settles on the flat heightfield surface");
            check(std::fabs(lt->position.x - 0.7f) < 0.05f
                      && std::fabs(lt->position.z + 1.3f) < 0.05f,
                  "terrain: and does not drift sideways on a flat field");
        }

        // -- 斜面: 球が下り方向へ転がる --
        // h = u (= +X 方向に上る) → 下りは -X
        {
            Scene s;
            const AssetID tid{ 0x7e11a10002ull };
            addTerrain(s, "Slope", tid,
                       makeTerrain(17, 40.0f, 20.0f, 0.0f, 20.0f,
                                   [](float u, float) { return u; }));
            GameObject ball = MakeSphereBody(s, "Ball", 0.0f, 14.0f, 0.0f, 0.5f);
            s.GetWorld().ApplyStructuralChanges();
            const auto* lt = ball.GetComponent<LocalTransform>();
            for (int i = 0; i < 300; ++i) {
                phys.Update(s.GetWorld(), kDt);
            }
            MYE_LOG_INFO("  [phys] terrain slope: ball at (%.2f, %.2f)", lt->position.x,
                         lt->position.y);
            check(lt->position.x < -1.0f, "terrain: a sphere rolls downhill on a sloped field");
            check(lt->position.y > -1.0f, "terrain: and never falls through the surface");
        }

        // -- 谷: 箱が底に落ち着く (対称な地形の中央へ寄る) --
        {
            Scene s;
            const AssetID tid{ 0x7e11a10003ull };
            addTerrain(s, "Valley", tid,
                       makeTerrain(33, 40.0f, 40.0f, 0.0f, 20.0f, [](float u, float) {
                           const float t = (u - 0.5f) * 2.0f;
                           return t * t; // -X/+X 端が高い V 字
                       }));
            GameObject box = MakeBox(s, "Box", 8.0f, 14.0f, 0.0f, 0.5f, 0.5f, 0.5f);
            s.GetWorld().ApplyStructuralChanges();
            const auto* lt = box.GetComponent<LocalTransform>();
            float minY = 1e9f;
            for (int i = 0; i < 900; ++i) {
                phys.Update(s.GetWorld(), kDt);
                if (lt->position.y < minY) {
                    minY = lt->position.y;
                }
            }
            MYE_LOG_INFO("  [phys] terrain valley: box at x = %.2f y = %.2f (min y %.2f)",
                         lt->position.x, lt->position.y, minY);
            check(std::fabs(lt->position.x) < 7.0f, "terrain: a box slides toward the valley floor");
            check(minY > -1.0f, "terrain: and never tunnels below the field");
        }

        // -- Raycast: 真下 / 斜め / 外れ --
        {
            Scene s;
            const AssetID tid{ 0x7e11a10004ull };
            addTerrain(s, "RayField", tid,
                       makeTerrain(17, 40.0f, 40.0f, 0.0f, 20.0f,
                                   [](float u, float) { return u; })); // +X へ 0..20m
            s.GetWorld().ApplyStructuralChanges();
            const auto* col = s.Find("RayField").GetComponent<ColliderComponent>();
            const ShapePose tp = shapes::MakePose(*col, { 0, 0, 0 }, { 0, 0, 0, 1 }, { 1, 1, 1 });
            // 中央 (u=0.5) の高さは 10m
            float t = 0, nx = 0, ny = 0, nz = 0;
            const bool down =
                shapes::Raycast(tp, 0.0f, 30.0f, 0.0f, 0.0f, -1.0f, 0.0f, 100.0f, t, nx, ny, nz);
            MYE_LOG_INFO("  [phys] terrain ray: straight down t = %.4f (expect 20), n.y = %.3f", t,
                         ny);
            check(down && std::fabs(t - 20.0f) < 0.05f,
                  "terrain raycast: a vertical ray hits the surface at the right height");
            check(ny > 0.5f, "terrain raycast: the normal points up");
            // 斜めに長いレイ (DDA が効いていないと候補上限で静かに落ちる)
            float t2 = 0, n2x = 0, n2y = 0, n2z = 0;
            const bool diag = shapes::Raycast(tp, -19.0f, 25.0f, -19.0f, 0.6963f, -0.1741f,
                                              0.6963f, 60.0f, t2, n2x, n2y, n2z);
            MYE_LOG_INFO("  [phys] terrain ray: long diagonal hit = %d t = %.3f", diag ? 1 : 0, t2);
            check(diag, "terrain raycast: a long diagonal ray still finds the surface (DDA)");
            // 地形の外を通るレイは当たらない
            float t3 = 0, n3x = 0, n3y = 0, n3z = 0;
            check(!shapes::Raycast(tp, 100.0f, 30.0f, 0.0f, 0.0f, -1.0f, 0.0f, 100.0f, t3, n3x,
                                   n3y, n3z),
                  "terrain raycast: a ray outside the field misses");
        }

        // -- 縁のクランプ: 負のセル座標で反対側の端へ飛ばない (uint32 wrap の罠) --
        // TerrainSystem.cpp の HeightClamped と同じ理由。高さを左右で大きく変えた地形で、
        // 左端の外側を撃ったときに右端の高さを拾わないことを見る
        {
            Scene s;
            const AssetID tid{ 0x7e11a10005ull };
            addTerrain(s, "Edge", tid,
                       makeTerrain(9, 16.0f, 16.0f, 0.0f, 100.0f,
                                   [](float u, float) { return (u < 0.5f) ? 0.0f : 1.0f; }));
            s.GetWorld().ApplyStructuralChanges();
            const auto* col = s.Find("Edge").GetComponent<ColliderComponent>();
            const ShapePose tp = shapes::MakePose(*col, { 0, 0, 0 }, { 0, 0, 0, 1 }, { 1, 1, 1 });
            // 左半分 (x < 0) は高さ 0、右半分は 100。左端ぎりぎりを撃つ
            float t = 0, nx = 0, ny = 0, nz = 0;
            const bool hit =
                shapes::Raycast(tp, -7.9f, 50.0f, 0.0f, 0.0f, -1.0f, 0.0f, 200.0f, t, nx, ny, nz);
            MYE_LOG_INFO("  [phys] terrain edge: left-edge ray t = %.3f (expect 50)", t);
            check(hit && std::fabs(t - 50.0f) < 0.5f,
                  "terrain: the left edge keeps its own height (no uint32 wrap to the far side)");
        }

        // -- キャラクターコントローラが地形を歩ける --
        {
            Scene s;
            const AssetID tid{ 0x7e11a10006ull };
            addTerrain(s, "CCField", tid,
                       makeTerrain(17, 40.0f, 40.0f, 0.0f, 10.0f,
                                   [](float, float) { return 0.3f; })); // 高さ 3m 一定
            GameObject go = s.CreateGameObjectTracked("Walker");
            go.SetLocalPosition(0, 8.0f, 0);
            auto* cc = go.AddComponent<CharacterControllerComponent>();
            cc->radius = 0.3f;
            cc->height = 1.8f;
            s.GetWorld().ApplyStructuralChanges();
            auto* ccp = go.GetComponent<CharacterControllerComponent>();
            const auto* lt = go.GetComponent<LocalTransform>();
            for (int i = 0; i < 300; ++i) {
                ccp->moveInput = { 1.0f, 0.0f, 0.0f };
                phys.Update(s.GetWorld(), kDt);
            }
            MYE_LOG_INFO("  [phys] terrain CC: walker at (%.2f, %.2f) grounded=%d",
                         lt->position.x, lt->position.y, ccp->isGrounded ? 1 : 0);
            check(ccp->isGrounded, "terrain: a character controller lands on the heightfield");
            check(std::fabs(lt->position.y - (3.0f + 0.9f)) < 0.1f,
                  "terrain: and stands at the surface height");
            check(lt->position.x > 1.0f, "terrain: and can walk across it");
        }

        // -- 地形を持たないシーンは 1 ビットも変わらない (shape=4 は opt-in) --
        {
            const AssetID missing{ 0x7e11a10007ull };
            Scene s;
            GameObject go = s.CreateGameObjectTracked("Broken");
            auto* col = go.AddComponent<ColliderComponent>();
            col->shape = 4;
            col->meshAsset = missing; // 未登録 = 実体 null
            GameObject box = MakeBox(s, "Faller", 0, 5.0f, 0, 0.5f, 0.5f, 0.5f);
            s.GetWorld().ApplyStructuralChanges();
            const auto* lt = box.GetComponent<LocalTransform>();
            for (int i = 0; i < 60; ++i) {
                phys.Update(s.GetWorld(), kDt);
            }
            // 未解決の地形は「衝突なし」に落ちる = 自由落下そのまま。
            // ★期待値は**半陰的 Euler の離散和** g*dt^2*n(n+1)/2 — 連続解 0.5*g*t^2 では
            //   ずれる (実測 0.0133 vs 連続 0.0950)。積分器の形を試験に持ち込むこと
            const float n60 = 60.0f;
            const float freeFall = 5.0f - 9.81f * kDt * kDt * n60 * (n60 + 1.0f) * 0.5f;
            MYE_LOG_INFO("  [phys] terrain unresolved: y = %.4f (free fall %.4f)", lt->position.y,
                         freeFall);
            check(std::fabs(lt->position.y - freeFall) < 0.05f,
                  "terrain: an unresolved terrain asset falls back to no collision");
        }

        terraincol::Install(prevTerrLib);
    }

    if (failCount == 0) {
        MYE_LOG_INFO("==== Physics self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== Physics self test: %d FAILURE(S) ====", failCount);
    return false;
}

} // namespace mye
