//====================================================================================
//                          DistanceCullSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                       11/11/2026
//                                          距離カリングの判定と RenderCulling コンポーネントの回帰テストの実装
//====================================================================================
#include "Engine/Engine/Rendering/DistanceCullSelfTest.h"

#include <cmath>
#include <filesystem>
#include <limits>
#include <string>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/ComponentRegistry.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Rendering/CullingSettings.h"
#include "Engine/Engine/Rendering/DistanceCull.h"
#include "Engine/Renderer/Mesh/MeshLod.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/Scene/Prefab.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Engine/Scene/SceneSerializer.h"

namespace mye {
namespace {

using DirectX::XMFLOAT3;
namespace dc = distancecull;

// 64 要素のタグ表 (全部 0 = 設定なし)
struct TagTable {
    float d[kCullingTagCount] = {};
};

constexpr uint64_t Bit(int tag)
{
    return 1ull << tag;
}

} // namespace

bool RunDistanceCullSelfTest()
{
    MYE_LOG_INFO("==== DistanceCull self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };
    RegisterBuiltinComponents();

    // ---- (1) 実効距離: 3 つの出所と最小値の規則 ----
    {
        TagTable t;
        t.d[3] = 30.0f;
        t.d[7] = 20.0f;
        t.d[9] = 5.0f; // 物がこのタグを持たなければ効かない
        t.d[63] = 70.0f;
        check(dc::EffectiveMaxDrawDistance(0.0f, 0, TagTable{}.d, 0.0f) == 0.0f,
              "distance: nothing set anywhere = 0 (no distance culling)");
        check(dc::EffectiveMaxDrawDistance(50.0f, 0, TagTable{}.d, 0.0f) == 50.0f, "distance: per-object only");
        check(dc::EffectiveMaxDrawDistance(0.0f, 0, TagTable{}.d, 80.0f) == 80.0f, "distance: project default only");
        check(dc::EffectiveMaxDrawDistance(0.0f, Bit(3), t.d, 0.0f) == 30.0f, "distance: tag only");
        check(dc::EffectiveMaxDrawDistance(0.0f, Bit(63), t.d, 0.0f) == 70.0f, "distance: tag 63 (top bit) is read");
        check(dc::EffectiveMaxDrawDistance(50.0f, Bit(3) | Bit(7), t.d, 80.0f) == 20.0f,
              "distance: the smallest of object / tags / default wins (tag 7)");
        check(dc::EffectiveMaxDrawDistance(50.0f, Bit(3), t.d, 80.0f) == 30.0f, "distance: tag 3 beats object 50 and default 80");
        check(dc::EffectiveMaxDrawDistance(10.0f, Bit(3), t.d, 80.0f) == 10.0f, "distance: per-object 10 beats tag 30");
        check(dc::EffectiveMaxDrawDistance(50.0f, 0, t.d, 40.0f) == 40.0f, "distance: default 40 beats per-object 50");
        check(dc::EffectiveMaxDrawDistance(0.0f, Bit(3), t.d, 80.0f) == 30.0f,
              "distance: a zero per-object value is skipped, not taken as the minimum");
        check(dc::EffectiveMaxDrawDistance(-5.0f, 0, TagTable{}.d, 80.0f) == 80.0f,
              "distance: a negative value is ignored");
        check(dc::EffectiveMaxDrawDistance(std::numeric_limits<float>::quiet_NaN(), 0, TagTable{}.d, 80.0f) == 80.0f,
              "distance: NaN is ignored");
    }

    // ---- (2) AABB までの距離 ----
    {
        const XMFLOAT3 lo = { -1.0f, -1.0f, -1.0f };
        const XMFLOAT3 hi = { 1.0f, 1.0f, 1.0f };
        check(dc::DistanceToAabb({ 0.0f, 0.0f, 0.0f }, lo, hi) == 0.0f, "aabb: the camera inside the box = 0");
        check(dc::DistanceToAabb({ 1.0f, 0.0f, 0.5f }, lo, hi) == 0.0f, "aabb: on the surface = 0");
        check(dc::DistanceToAabb({ 11.0f, 0.0f, 0.0f }, lo, hi) == 10.0f, "aabb: nearest point on a face (not the centre)");
        check(dc::DistanceToAabb({ -3.0f, 5.0f, 1.0f }, { 0.0f, 0.0f, 0.0f }, { 1.0f, 1.0f, 1.0f })
                  == 5.0f,
              "aabb: nearest point is the corner (3-4-5)");
    }

    // ---- (3) 閾値と境界 ----
    {
        const XMFLOAT3 lo = { -1.0f, -1.0f, -1.0f };
        const XMFLOAT3 hi = { 1.0f, 1.0f, 1.0f };
        const XMFLOAT3 cam = { 11.0f, 0.0f, 0.0f }; // 距離ちょうど 10
        check(!dc::ExceedsMaxDistance(cam, lo, hi, 10.0f), "threshold: d == Dmax is kept");
        check(dc::ExceedsMaxDistance(cam, lo, hi, 9.999f), "threshold: d > Dmax is dropped");
        check(!dc::ExceedsMaxDistance(cam, lo, hi, 10.001f), "threshold: d < Dmax is kept");
        check(!dc::ExceedsMaxDistance(cam, lo, hi, 0.0f), "threshold: Dmax 0 never drops");
        check(!dc::ExceedsMaxDistance({ 0.0f, 0.0f, 0.0f }, lo, hi, 0.001f),
              "threshold: a camera inside the box never drops it");
        // 箱の大きさで距離が縮む: 同じ中心でも大きい箱は近い
        check(!dc::ExceedsMaxDistance({ 20.0f, 0.0f, 0.0f }, { 4.0f, -1.0f, -1.0f }, { 14.0f, 1.0f, 1.0f }, 10.0f),
              "threshold: the distance is to the nearest point of the box, not its centre");
    }

    // ---- (4) 掛けるビュー ----
    {
        check(dc::AppliesToView(1, false) && dc::AppliesToView(1, true), "view: Runtime (1) always");
        check(dc::AppliesToView(3, false) && dc::AppliesToView(3, true), "view: Game View (3) always");
        check(!dc::AppliesToView(2, false), "view: Scene View (2) is not dropped by default");
        check(dc::AppliesToView(2, true), "view: Scene View (2) is dropped when the session toggle is on");
        check(!dc::AppliesToView(0, false) && !dc::AppliesToView(0, true), "view: previews (0) are never dropped");
        check(!dc::AppliesToView(4, true), "view: an out-of-range key is never dropped");
    }

    // ---- (4b) 画面サイズ: 閾値・ヒステリシス・除外・履歴 ----
    {
        constexpr float kMin = 0.02f;
        const auto cull = [&](float s, int32_t prev, bool exempt = false, bool always = false) {
            return dc::CullsByScreenSize(s, kMin, prev, exempt, always);
        };
        check(cull(0.019f, -1) && !cull(0.021f, -1), "size: below the threshold is dropped, above is kept (no history)");
        check(!cull(kMin, -1), "size: exactly the threshold is kept");
        check(!cull(0.0201f, 0) && cull(0.0199f, 0), "size: a kept object follows the plain threshold");
        // 前フレームに落とした物は 閾値 x 1.0〜1.1 の帯で落としたまま、1.1 倍以上で戻る
        check(cull(0.0201f, 1) && cull(0.0219f, 1), "size: a dropped object stays dropped inside the 10% band");
        check(!cull(0.0221f, 1), "size: a dropped object returns above threshold x 1.1");
        check(!cull(0.0201f, 0), "size: a kept object is not dropped inside the band (no flicker)");
        check(!cull(0.001f, 1, true) && !cull(0.001f, -1, true), "size: exemptFromSizeCulling is never dropped");
        check(!cull(0.001f, -1, false, true), "size: alwaysVisible (no box) is never dropped");
        check(!dc::CullsByScreenSize(0.0f, 0.0f, -1, false, false), "size: a threshold of 0 never drops");
        check(!cull(std::numeric_limits<float>::quiet_NaN(), -1), "size: NaN is kept");

        // 履歴: Get は Set 前と Clear 後で -1 (= 履歴なし)。ResetRenderHistory はこれを呼ぶ
        LodHistory history;
        const EntityID e{ 5, 0 };
        check(history.Get(e) < 0, "size history: none before the first write");
        history.Set(e, 1);
        check(cull(0.0205f, history.Get(e)), "size history: a recorded drop holds the object inside the band");
        history.Clear();
        check(!cull(0.0205f, history.Get(e)), "size history: after a reset the plain threshold decides");
    }

    // ---- (5) RenderCulling コンポーネント: 登録・既定値・NoHash ----
    {
        const ComponentTypeId id = RenderCullingComponent::sTypeId;
        check(id == 100 && ComponentRegistry::Get().FindByName("RenderCulling") == id,
              "component: registered as RenderCulling with TypeId 100");
        const ComponentDesc& desc = ComponentRegistry::Get().Desc(id);
        check((desc.flags & kComponentNoHash) != 0, "component: RenderCulling is kComponentNoHash");
        RenderCullingComponent d;
        check(d.maxDrawDistance == 0.0f && !d.exemptFromSizeCulling, "component: defaults are 0 / false");
        bool rangeOk = false;
        for (const FieldDesc& f : desc.fields) {
            if (std::string(f.name) == "maxDrawDistance") {
                rangeOk = f.minVal == 0.0f && f.maxVal == 100000.0f;
            }
        }
        check(rangeOk, "component: maxDrawDistance is limited to 0..100000");

        Scene scene;
        GameObject host = scene.CreateGameObjectTracked("Host");
        World& w = scene.GetWorld();
        w.ApplyStructuralChanges();
        const uint64_t before = HashWorld(w);
        auto* rc = w.AddComponent<RenderCullingComponent>(host.Id());
        w.ApplyStructuralChanges();
        check(rc != nullptr, "component: can be added to an entity");
        if (rc != nullptr) {
            rc->maxDrawDistance = 123.5f;
            rc->exemptFromSizeCulling = true;
        }
        check(HashWorld(w) == before, "component: adding RenderCulling never moves the world hash");

        // ---- (6) シーン保存 → 読み込み ----
        const uint64_t hostFileId = scene.EnsureFileId(host.Id());
        const nlohmann::json saved = SceneSerializer::SaveToJson(scene);
        Scene loaded;
        check(SceneSerializer::LoadFromJson(loaded, saved), "scene: load succeeds");
        const GameObject again = loaded.FindByFileId(hostFileId);
        const auto* lrc = again ? loaded.GetWorld().GetComponent<RenderCullingComponent>(again.Id()) : nullptr;
        check(lrc != nullptr && lrc->maxDrawDistance == 123.5f && lrc->exemptFromSizeCulling,
              "scene: RenderCulling values survive save and load");
        check(HashWorld(loaded.GetWorld()) == HashWorld(w), "scene: the loaded world hash equals the saved world's");
    }

    // ---- (6b) 階層: タグと RenderCulling は祖先から継承する ----
    {
        Scene scene;
        World& w = scene.GetWorld();
        GameObject root = scene.CreateGameObjectTracked("Root");
        GameObject child = scene.CreateGameObjectTracked("Child");
        GameObject grand = scene.CreateGameObjectTracked("Grand");
        GameObject lone = scene.CreateGameObjectTracked("Lone");
        child.SetParent(root);
        grand.SetParent(child);
        w.ApplyStructuralChanges();
        TagTable t;
        t.d[4] = 25.0f;

        check(dc::ResolveMaxDrawDistance(w, grand.Id(), true, t.d, 0.0f) == 0.0f
                  && dc::ResolveMaxDrawDistance(w, lone.Id(), false, t.d, 0.0f) == 0.0f,
              "hierarchy: nothing anywhere = 0");
        check(dc::ResolveMaxDrawDistance(w, grand.Id(), false, t.d, 90.0f) == 90.0f, "hierarchy: only the project default");

        Tags::SetOwnMask(w, root.Id(), Bit(4));
        w.ApplyStructuralChanges();
        check(dc::ResolveMaxDrawDistance(w, grand.Id(), true, t.d, 0.0f) == 25.0f,
              "hierarchy: a tag on the root reaches a grandchild mesh");
        check(dc::ResolveMaxDrawDistance(w, lone.Id(), true, t.d, 0.0f) == 0.0f,
              "hierarchy: an unrelated entity does not get the tag");

        root.AddComponent<RenderCullingComponent>()->maxDrawDistance = 40.0f;
        w.ApplyStructuralChanges();
        check(dc::ResolveMaxDrawDistance(w, grand.Id(), false, t.d, 0.0f) == 40.0f,
              "hierarchy: RenderCulling on the root reaches a grandchild");
        check(dc::ResolveMaxDrawDistance(w, grand.Id(), true, t.d, 0.0f) == 25.0f,
              "hierarchy: the smallest of the inherited object value and the tag wins");

        bool exempt = true;
        child.AddComponent<RenderCullingComponent>()->maxDrawDistance = 90.0f;
        w.ApplyStructuralChanges();
        check(dc::ResolveMaxDrawDistance(w, grand.Id(), false, t.d, 0.0f, &exempt) == 90.0f && !exempt,
              "hierarchy: a child's RenderCulling replaces the ancestor's wholesale (the nearest one wins)");
        check(dc::ResolveMaxDrawDistance(w, root.Id(), false, t.d, 0.0f) == 40.0f,
              "hierarchy: the root keeps its own value");
    }

    // ---- (7) プレハブ上書き ----
    {
        auto findField = [](const char* fname) -> const FieldDesc* {
            for (const FieldDesc& f : ComponentRegistry::Get().Desc(RenderCullingComponent::sTypeId).fields) {
                if (std::string(f.name) == fname) {
                    return &f;
                }
            }
            return nullptr;
        };
        PrefabLibrary lib;
        const std::wstring path = (std::filesystem::temp_directory_path() / L"mye_selftest_distcull.prefab.json").wstring();
        Scene baseScene;
        GameObject root = baseScene.CreateGameObjectTracked("Tree");
        root.AddComponent<RenderCullingComponent>()->maxDrawDistance = 50.0f;
        baseScene.GetWorld().ApplyStructuralChanges();
        const uint64_t hash = lib.Register(path, "distcull", Prefab::ExtractLocal(baseScene, root.Id()));

        Scene s;
        const uint64_t fid = Prefab::Instantiate(s, lib, hash, 0);
        s.GetWorld().ApplyStructuralChanges();
        GameObject inst = s.FindByFileId(fid);
        auto* rc = inst ? inst.GetComponent<RenderCullingComponent>() : nullptr;
        const FieldDesc* dist = findField("maxDrawDistance");
        check(rc != nullptr && rc->maxDrawDistance == 50.0f, "prefab: the instance inherits the base distance");
        if (rc != nullptr && dist != nullptr) {
            check(!Prefab::IsFieldOverridden(s, lib, inst.Id(), "RenderCulling", *dist), "prefab: no override at first");
            rc->maxDrawDistance = 99.0f;
            Prefab::RecordOverrides(s, lib, inst.Id());
            check(Prefab::IsFieldOverridden(s, lib, inst.Id(), "RenderCulling", *dist),
                  "prefab: a changed distance is recorded as an override");
            Prefab::RevertField(s, lib, inst.Id(), "RenderCulling", *dist);
            check(rc->maxDrawDistance == 50.0f, "prefab: revert restores the base distance");
        } else {
            check(false, "prefab: the field and the component are found");
        }
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    MYE_LOG_INFO("==== DistanceCull self test: %s ====", failCount == 0 ? "PASS" : "FAIL");
    return failCount == 0;
}

} // namespace mye
