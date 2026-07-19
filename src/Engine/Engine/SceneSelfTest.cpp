#include "Engine/Engine/SceneSelfTest.h"

#include <new>
#include <string>
#include <unordered_map>
#include <vector>

#include "Engine/Core/Components.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Profiler.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Engine/SceneSerializer.h"

namespace mye {

bool RunSceneSerializerSelfTest()
{
    MYE_LOG_INFO("==== Scene serializer self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    // ---- テストシーン構築 ----
    Scene scene;
    scene.SetName("SerializerTest");

    GameObject parent = scene.CreateGameObject("Parent");
    parent.SetLocalPosition(1.0f, 2.0f, 3.0f);
    GameObject child = scene.CreateGameObject("Child");
    child.SetParent(parent);
    child.SetLocalPosition(0.5f, 0.0f, 0.0f);
    auto* mr = child.AddComponent<MeshRendererComponent>();
    mr->mesh = AssetID{ 0xABCDull };
    mr->material = AssetID{ 0x1234ull };
    GameObject cam = scene.CreateGameObject("Camera");
    auto* cc = cam.AddComponent<CameraComponent>();
    cc->fovYDeg = 42.0f;
    scene.GetWorld().ApplyStructuralChanges(); // SetParent 反映

    // ---- 保存 → 読込 → 再保存 ----
    const nlohmann::json first = SceneSerializer::SaveToJson(scene);
    check(first["entities"].size() == 3, "3 entities saved");

    check(SceneSerializer::LoadFromJson(scene, first), "load succeeds");
    check(scene.GetWorld().AliveCount() == 3, "3 entities after load");

    GameObject child2 = scene.Find("Child");
    check(static_cast<bool>(child2), "Child found after load");
    if (child2) {
        auto* lt = child2.GetComponent<LocalTransform>();
        check(lt && lt->position.x == 0.5f, "Child position restored");
        auto* mr2 = child2.GetComponent<MeshRendererComponent>();
        check(mr2 && mr2->mesh.value == 0xABCDull, "Child MeshRenderer restored");
        GameObject parent2 = scene.Find("Parent");
        check(parent2 && child2.GetWorld()->GetParent(child2.Id()) == parent2.Id(),
              "parent relation restored");
    }
    GameObject cam2 = scene.Find("Camera");
    if (cam2) {
        auto* cc2 = cam2.GetComponent<CameraComponent>();
        check(cc2 && cc2->fovYDeg == 42.0f, "Camera fov restored");
    }

    // ---- ラウンドトリップ一致 (fileId 単位で比較 — 配列順はアーキタイプ順に依存するため) ----
    const nlohmann::json second = SceneSerializer::SaveToJson(scene);
    auto mapByFileId = [](const nlohmann::json& root) {
        std::unordered_map<uint64_t, nlohmann::json> map;
        for (const auto& item : root["entities"]) {
            map[item.value("fileId", 0ull)] = item;
        }
        return map;
    };
    const auto a = mapByFileId(first);
    const auto b = mapByFileId(second);
    bool identical = (a.size() == b.size());
    if (identical) {
        for (const auto& [fid, item] : a) {
            auto it = b.find(fid);
            if (it == b.end() || it->second != item) {
                identical = false;
                MYE_LOG_ERROR("  roundtrip mismatch at fileId=%llu",
                              static_cast<unsigned long long>(fid));
                break;
            }
        }
    }
    check(identical, "save -> load -> save roundtrip identical");

    // ---- EntityRef の fileId 往復 (M8) ----
    // 組み込みには serializable な EntityRef フィールドが無いため、
    // テスト専用コンポーネントを登録して汎用 EntityRef 経路を検証する (--selftest 限りの登録)
    {
        struct RefProbe {
            EntityID target = kNullEntity;
        };
        ComponentDesc d;
        d.name = "RefProbe";
        d.nameHash = HashStr("RefProbe");
        d.size = sizeof(RefProbe);
        d.align = alignof(RefProbe);
        d.flags = kComponentNone;
        d.construct = [](void* p) { new (p) RefProbe(); };
        d.fields = { FieldDesc{ "target", FieldType::EntityRef,
                                static_cast<uint32_t>(offsetof(RefProbe, target)), kFieldNone } };
        const ComponentTypeId probeType = ComponentRegistry::Get().Register(std::move(d));

        Scene s2;
        GameObject ra = s2.CreateGameObjectTracked("Ref_A");
        GameObject rb = s2.CreateGameObjectTracked("Ref_B");
        auto* probe = static_cast<RefProbe*>(s2.GetWorld().AddComponentRaw(ra.Id(), probeType));
        probe->target = rb.Id();
        const uint64_t rbFid = s2.GetWorld().GetComponent<FileIdComponent>(rb.Id())->value;

        const nlohmann::json j = SceneSerializer::SaveToJson(s2);
        check(SceneSerializer::LoadFromJson(s2, j), "EntityRef scene reload");

        GameObject ra2 = s2.Find("Ref_A");
        GameObject rb2 = s2.FindByFileId(rbFid);
        bool refOk = false;
        if (ra2 && rb2) {
            auto* p2 = static_cast<RefProbe*>(s2.GetWorld().GetComponentRaw(ra2.Id(), probeType));
            refOk = p2 && p2->target == rb2.Id() && !rb2.Id().IsNull();
        }
        check(refOk, "EntityRef restored by fileId across save/load");
    }

    // ---- 兄弟順の保存/復元 (M8) ----
    {
        auto childNames = [](Scene& sc, GameObject parent) {
            std::vector<std::string> out;
            World& w = sc.GetWorld();
            auto* h = w.GetComponent<HierarchyComponent>(parent.Id());
            EntityID ch = h ? h->firstChild : kNullEntity;
            while (!ch.IsNull()) {
                out.push_back(w.GetName(ch));
                auto* chh = w.GetComponent<HierarchyComponent>(ch);
                ch = chh ? chh->nextSibling : kNullEntity;
            }
            return out;
        };

        Scene s3;
        GameObject p = s3.CreateGameObjectTracked("SibParent");
        GameObject c0 = s3.CreateGameObjectTracked("Sib0");
        GameObject c1 = s3.CreateGameObjectTracked("Sib1");
        GameObject c2 = s3.CreateGameObjectTracked("Sib2");
        c0.SetParent(p);
        c1.SetParent(p);
        c2.SetParent(p);
        s3.GetWorld().ApplyStructuralChanges();
        s3.GetWorld().SetSiblingIndex(c2.Id(), 0); // Sib2 を先頭へ
        s3.GetWorld().ApplyStructuralChanges();

        const std::vector<std::string> before = childNames(s3, p);
        check(before.size() == 3 && before[0] == "Sib2" && before[1] == "Sib0"
                  && before[2] == "Sib1",
              "sibling reorder applied (Sib2, Sib0, Sib1)");

        const nlohmann::json j = SceneSerializer::SaveToJson(s3);
        SceneSerializer::LoadFromJson(s3, j);
        const std::vector<std::string> after = childNames(s3, s3.Find("SibParent"));
        check(before == after, "sibling order round-trips through save/load");
    }

    // ---- CloneSubtree: 複製で新 fileId 採番 + 値一致 (M10) ----
    {
        Scene s4;
        GameObject p = s4.CreateGameObjectTracked("CloneParent");
        p.SetLocalPosition(5.0f, 0.0f, 0.0f);
        GameObject c = s4.CreateGameObjectTracked("CloneChild");
        c.SetParent(p);
        c.SetLocalPosition(1.0f, 2.0f, 3.0f);
        s4.GetWorld().ApplyStructuralChanges();
        const uint64_t pFid = s4.GetWorld().GetComponent<FileIdComponent>(p.Id())->value;
        const uint32_t beforeCount = s4.GetWorld().AliveCount();

        const nlohmann::json sub = SceneSerializer::SubtreeToJson(s4, p.Id());
        const std::vector<uint64_t> roots = SceneSerializer::CloneSubtree(s4, sub);

        check(s4.GetWorld().AliveCount() == beforeCount + 2, "clone added parent+child (2)");
        check(roots.size() == 1 && roots[0] != 0 && roots[0] != pFid, "clone root has new fileId");
        GameObject clone = s4.FindByFileId(roots.empty() ? 0 : roots[0]);
        bool cloneOk = static_cast<bool>(clone);
        if (cloneOk) {
            auto* lt = clone.GetComponent<LocalTransform>();
            cloneOk = lt && lt->position.x == 5.0f;
            auto* ch = s4.GetWorld().GetComponent<HierarchyComponent>(clone.Id());
            if (ch && !ch->firstChild.IsNull()) {
                GameObject childGo(&s4.GetWorld(), ch->firstChild);
                auto* clt = childGo.GetComponent<LocalTransform>();
                cloneOk = cloneOk && clt && clt->position.y == 2.0f;
            } else {
                cloneOk = false;
            }
        }
        check(cloneOk, "clone parent+child values match original");
    }

    // ---- ActiveComponent: 有効/無効 + シリアライズ往復 (M10) ----
    {
        Scene s5;
        GameObject g = s5.CreateGameObjectTracked("ActiveTest");
        s5.GetWorld().ApplyStructuralChanges();
        check(IsEntityActive(s5.GetWorld(), g.Id()), "no ActiveComponent = active");
        auto* act = g.AddComponent<ActiveComponent>();
        act->enabled = 0;
        check(!IsEntityActive(s5.GetWorld(), g.Id()), "enabled=0 = inactive");
        act->enabled = 1;
        check(IsEntityActive(s5.GetWorld(), g.Id()), "enabled=1 = active");

        act->enabled = 0;
        const nlohmann::json j = SceneSerializer::SaveToJson(s5);
        SceneSerializer::LoadFromJson(s5, j);
        GameObject g2 = s5.Find("ActiveTest");
        check(static_cast<bool>(g2) && !IsEntityActive(s5.GetWorld(), g2.Id()),
              "ActiveComponent (enabled=0) survives save/load");
    }

    // ---- メモリフック: new/delete カウンタ (M12) ----
    {
        const prof::MemStats before = prof::GetMemoryStats();
        constexpr int N = 100;
        std::vector<int*> ptrs;
        ptrs.reserve(N);
        for (int i = 0; i < N; ++i) {
            ptrs.push_back(new int(i));
        }
        const prof::MemStats mid = prof::GetMemoryStats();
        for (int* p : ptrs) {
            delete p;
        }
        const prof::MemStats after = prof::GetMemoryStats();
        check(mid.totalAllocs - before.totalAllocs >= N, "new increments alloc counter");
        check(mid.totalBytes - before.totalBytes >= N * sizeof(int), "new adds byte counter");
        check(after.totalFrees - mid.totalFrees >= N, "delete increments free counter");
        int* np = nullptr;
        delete np; // 落ちないこと
        check(true, "delete nullptr is safe");
    }

    if (failCount == 0) {
        MYE_LOG_INFO("==== Scene serializer self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== Scene serializer self test: %d FAILURE(S) ====", failCount);
    return false;
}

} // namespace mye
