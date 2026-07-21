#include "Engine/Engine/PhysicsSelfTest.h"

#include <cmath>
#include <cstring>

#include "Engine/Core/Components.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/CollisionSystem.h"
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

    // ---- (11) SAT: 45° 回転 box (freezeRotation) が平床に角で立つ (姿勢維持の並進解決) ----
    {
        Scene s;
        MakeGround(s, "G11", 0, -0.5f, 0, 5.0f, 0.5f, 5.0f);
        GameObject box = MakeBox(s, "TiltBox", 0, 3.0f, 0, 0.5f, 0.5f, 0.5f);
        box.GetComponent<LocalTransform>()->rotation = { 0, 0, 0.3826834f, 0.9238795f };
        box.GetComponent<RigidbodyComponent>()->freezeRotation = 1; // M28b: 回転を固定して幾何検証
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
            rb->freezeRotation = 1; // 転がり抜きの純粋な滑り摩擦を検証
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
        rb->freezeRotation = 1;
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
        MakeGround(s, "Trig", 10.0f, 0, 0, 1.0f, 1.0f, 1.0f, /*isTrigger=*/1);
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
        col->isTrigger = 0;
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

    if (failCount == 0) {
        MYE_LOG_INFO("==== Physics self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== Physics self test: %d FAILURE(S) ====", failCount);
    return false;
}

} // namespace mye
