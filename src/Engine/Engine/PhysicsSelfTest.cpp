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
            envGo.AddComponent<PhysicsEnvironmentComponent>()->gravity = { 0.0f, 0.0f, 0.0f };
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

    if (failCount == 0) {
        MYE_LOG_INFO("==== Physics self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== Physics self test: %d FAILURE(S) ====", failCount);
    return false;
}

} // namespace mye
