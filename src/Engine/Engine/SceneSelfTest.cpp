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
#include "Engine/Engine/Animation.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/Prefab.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Engine/SceneSerializer.h"

#include <cstddef>
#include <filesystem>

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

    // ---- プレハブ (M13) ----
    {
        auto findField = [](ComponentTypeId t, const char* fname) -> const FieldDesc* {
            for (const FieldDesc& f : ComponentRegistry::Get().Desc(t).fields) {
                if (std::string(f.name) == fname) {
                    return &f;
                }
            }
            return nullptr;
        };

        PrefabLibrary lib;
        const std::wstring tmp1 =
            (std::filesystem::temp_directory_path() / L"mye_selftest_a.prefab.json").wstring();

        // ベースサブツリー (root + 子2、子A に MeshRenderer)
        Scene sb0;
        GameObject pr = sb0.CreateGameObjectTracked("PfRoot");
        pr.SetLocalPosition(1.0f, 2.0f, 3.0f);
        GameObject ca = sb0.CreateGameObjectTracked("PfChildA");
        ca.SetParent(pr);
        ca.SetLocalPosition(0.5f, 0.0f, 0.0f);
        auto* mrA = ca.AddComponent<MeshRendererComponent>();
        mrA->mesh = AssetID{ 0xAAull };
        mrA->material = AssetID{ 0xBBull };
        GameObject cb = sb0.CreateGameObjectTracked("PfChildB");
        cb.SetParent(pr);
        sb0.GetWorld().ApplyStructuralChanges();

        const nlohmann::json local = Prefab::ExtractLocal(sb0, pr.Id());
        check(local.is_array() && local.size() == 3, "prefab extract has 3 local entities");
        bool rootLocalOk = false;
        for (const auto& it : local) {
            if (it.value("fileId", 0ull) == 1ull && !it.contains("parent")) {
                rootLocalOk = true;
            }
        }
        check(rootLocalOk, "prefab local root id=1 with no parent");
        const uint64_t hash = lib.Register(tmp1, "a", local);

        // (1) インスタンス化 → save → 2 回ロードのハッシュ一致 (決定論)
        Scene s7;
        const uint64_t r1 = Prefab::Instantiate(s7, lib, hash, 0);
        const uint64_t r2 = Prefab::Instantiate(s7, lib, hash, 0);
        s7.GetWorld().ApplyStructuralChanges();
        check(r1 != 0 && r2 != 0 && r1 != r2, "two instances get distinct root fileIds");
        check(s7.GetWorld().AliveCount() == 6, "two instances = 6 entities");

        // (1b) v7 Instantiate の予約 fileId (M37): forcedRootFileId がルートに使われ、
        // 子は新規採番される。予約 ID は呼び出し側が NextFileId で確保する契約
        {
            const uint64_t reserved = s7.NextFileId();
            const uint64_t r3 = Prefab::Instantiate(s7, lib, hash, 0, reserved);
            s7.GetWorld().ApplyStructuralChanges();
            check(r3 == reserved, "forcedRootFileId: root gets the reserved id");
            GameObject g3 = s7.FindByFileId(reserved);
            check(static_cast<bool>(g3), "forcedRootFileId: root resolvable by reserved id");
            // 子は予約 ID と衝突しない新規 ID (= 予約より後の採番)
            bool childrenOk = true;
            if (g3) {
                if (auto* h = s7.GetWorld().GetComponent<HierarchyComponent>(g3.Id())) {
                    for (EntityID c = h->firstChild; !c.IsNull();) {
                        const uint64_t cf = s7.EnsureFileId(c);
                        if (cf == reserved || cf == 0) {
                            childrenOk = false;
                        }
                        auto* ch = s7.GetWorld().GetComponent<HierarchyComponent>(c);
                        c = ch ? ch->nextSibling : kNullEntity;
                    }
                }
            }
            check(childrenOk, "forcedRootFileId: children get fresh distinct ids");
        }

        const nlohmann::json saved = SceneSerializer::SaveToJson(s7);
        Scene sa;
        SceneSerializer::LoadFromJson(sa, saved);
        Scene sbb;
        SceneSerializer::LoadFromJson(sbb, saved);
        check(HashWorld(sa.GetWorld(), nullptr) == HashWorld(sbb.GetWorld(), nullptr),
              "instantiate->save->load: WorldHash deterministic");
        // ロード後のインスタンスも青文字判定 (PrefabLink) を保持
        {
            const nlohmann::json resaved = SceneSerializer::SaveToJson(sa);
            auto byFid = [](const nlohmann::json& root) {
                std::unordered_map<uint64_t, nlohmann::json> m;
                for (const auto& it : root["entities"]) {
                    m[it.value("fileId", 0ull)] = it;
                }
                return m;
            };
            const auto ma = byFid(saved), mb = byFid(resaved);
            bool same = ma.size() == mb.size();
            for (const auto& [f, it] : ma) {
                auto j = mb.find(f);
                if (j == mb.end() || j->second != it) {
                    same = false;
                    break;
                }
            }
            check(same, "prefab scene save/load roundtrip identical");
        }

        // (2) オーバーライド → Revert
        const FieldDesc* posF = findField(LocalTransform::sTypeId, "position");
        GameObject inst1 = s7.FindByFileId(r1);
        auto* lt = inst1.GetComponent<LocalTransform>();
        check(lt && lt->position.x == 1.0f, "instance root inherits base position");
        check(posF && !Prefab::IsFieldOverridden(s7, lib, inst1.Id(), "LocalTransform", *posF),
              "fresh instance has no override");
        lt->position.x = 99.0f;
        check(posF && Prefab::IsFieldOverridden(s7, lib, inst1.Id(), "LocalTransform", *posF),
              "modified field detected as override");
        Prefab::RevertField(s7, lib, inst1.Id(), "LocalTransform", *posF);
        check(lt->position.x == 1.0f, "revert restores base value");
        check(posF && !Prefab::IsFieldOverridden(s7, lib, inst1.Id(), "LocalTransform", *posF),
              "no override after revert");

        // (3) Apply → 別インスタンスの非オーバーライドへ伝播
        inst1.GetComponent<LocalTransform>()->position.x = 5.0f; // 新ベースにする値
        check(Prefab::ApplyInstance(s7, lib, r1), "apply succeeds");
        s7.GetWorld().ApplyStructuralChanges();
        GameObject inst2 = s7.FindByFileId(r2);
        check(inst2.GetComponent<LocalTransform>()->position.x == 5.0f,
              "apply propagated base change to other instance");
        const PrefabAsset* na = lib.Get(hash);
        bool baseUpdated = false;
        if (na) {
            for (const auto& it : na->entities) {
                if (it.value("fileId", 0ull) == 1ull) {
                    baseUpdated = it["components"]["LocalTransform"]["position"][0].get<float>() == 5.0f;
                }
            }
        }
        check(baseUpdated, "apply updated prefab base asset");
        std::error_code ec;
        std::filesystem::remove(tmp1, ec);

        // (4) EntityRef の 2 段 remap (抽出でローカル id 化 → インスタンス化で実体解決)
        {
            struct PfRef {
                EntityID target = kNullEntity;
            };
            ComponentDesc d;
            d.name = "PfRef";
            d.nameHash = HashStr("PfRef");
            d.size = sizeof(PfRef);
            d.align = alignof(PfRef);
            d.flags = kComponentNone;
            d.construct = [](void* p) { new (p) PfRef(); };
            d.fields = { FieldDesc{ "target", FieldType::EntityRef,
                                    static_cast<uint32_t>(offsetof(PfRef, target)), kFieldNone } };
            const ComponentTypeId pfRefType = ComponentRegistry::Get().Register(std::move(d));

            Scene s8;
            GameObject rp = s8.CreateGameObjectTracked("RP_Root");
            GameObject rc = s8.CreateGameObjectTracked("RP_Child");
            rc.SetParent(rp);
            s8.GetWorld().ApplyStructuralChanges();
            static_cast<PfRef*>(s8.GetWorld().AddComponentRaw(rp.Id(), pfRefType))->target = rc.Id();

            const nlohmann::json local2 = Prefab::ExtractLocal(s8, rp.Id());
            uint64_t refLocal = 0xFFFF;
            for (const auto& it : local2) {
                if (it.value("fileId", 0ull) == 1ull && it.contains("components")
                    && it["components"].contains("PfRef")) {
                    refLocal = it["components"]["PfRef"]["target"].get<uint64_t>();
                }
            }
            check(refLocal == 2ull, "extract remaps EntityRef to local child id (stage 1)");

            const std::wstring tmp2 =
                (std::filesystem::temp_directory_path() / L"mye_selftest_b.prefab.json").wstring();
            const uint64_t hash2 = lib.Register(tmp2, "b", local2);
            Scene s9;
            const uint64_t rr = Prefab::Instantiate(s9, lib, hash2, 0);
            s9.GetWorld().ApplyStructuralChanges();
            GameObject instRoot = s9.FindByFileId(rr);
            auto* p2 = static_cast<PfRef*>(s9.GetWorld().GetComponentRaw(instRoot.Id(), pfRefType));
            auto* h = s9.GetWorld().GetComponent<HierarchyComponent>(instRoot.Id());
            const EntityID refChild = h ? h->firstChild : kNullEntity;
            check(p2 && !refChild.IsNull() && p2->target == refChild,
                  "instantiate remaps EntityRef to instance child (stage 2)");
        }
    }

    // ---- アニメーション (M14) ----
    {
        auto mkkey = [](int t, std::vector<float> v) {
            nlohmann::json k;
            k["t"] = t;
            k["v"] = v;
            return k;
        };
        // position: linear [0,0,0]@0 -> [10,0,0]@10 / scale: step [1,1,1]@0 -> [2,2,2]@5
        nlohmann::json cj;
        cj["name"] = "t";
        cj["lengthTicks"] = 10;
        nlohmann::json tPos;
        tPos["target"] = 0;
        tPos["component"] = "LocalTransform";
        tPos["field"] = "position";
        tPos["interp"] = "linear";
        tPos["keys"] = nlohmann::json::array({ mkkey(0, { 0, 0, 0 }), mkkey(10, { 10, 0, 0 }) });
        nlohmann::json tScl;
        tScl["target"] = 0;
        tScl["component"] = "LocalTransform";
        tScl["field"] = "scale";
        tScl["interp"] = "step";
        tScl["keys"] = nlohmann::json::array({ mkkey(0, { 1, 1, 1 }), mkkey(5, { 2, 2, 2 }) });
        cj["tracks"] = nlohmann::json::array({ tPos, tScl });

        AnimationClipAsset clip;
        check(AnimationLibrary::FromJson(cj, clip), "anim clip FromJson");
        check(clip.tracks.size() == 2, "clip has 2 tracks");
        check(clip.tracks[0].comp == LocalTransform::sTypeId && clip.tracks[0].compCount == 3,
              "position track resolved to LocalTransform (Float3)");

        // (a) roundtrip
        const nlohmann::json j1 = AnimationLibrary::ToJson(clip);
        AnimationClipAsset clip2;
        AnimationLibrary::FromJson(j1, clip2);
        check(j1 == AnimationLibrary::ToJson(clip2), "clip ToJson/FromJson roundtrip identical");

        // (b) 補間の期待値 (linear / step / クランプ)
        LocalTransform lt;
        SampleTrackInto(&lt, clip.tracks[0], 5);
        check(lt.position.x == 5.0f, "linear interp at t=5 -> 5.0");
        SampleTrackInto(&lt, clip.tracks[0], 0);
        check(lt.position.x == 0.0f, "clamp before first key");
        SampleTrackInto(&lt, clip.tracks[0], 100);
        check(lt.position.x == 10.0f, "clamp after last key");
        SampleTrackInto(&lt, clip.tracks[1], 3);
        check(lt.scale.x == 1.0f, "step holds first key at t=3");
        SampleTrackInto(&lt, clip.tracks[1], 7);
        check(lt.scale.x == 2.0f, "step advances at t=7 -> 2.0");

        // (c) 同一クリップを 2 回再生 → 全 tick で WorldHash 一致 (決定論)
        AnimationLibrary lib;
        const std::wstring apath =
            (std::filesystem::temp_directory_path() / L"mye_selftest.anim.json").wstring();
        const uint64_t chash = lib.Register(apath, clip);
        auto buildAnim = [&](Scene& s) {
            GameObject go = s.CreateGameObjectTracked("Animated");
            auto* an = go.AddComponent<AnimatorComponent>();
            an->clip = AssetID{ chash };
            an->timeTicks = 0;
            an->speed = 1;
            an->loop = 1;
            an->playing = 1;
            s.GetWorld().ApplyStructuralChanges();
        };
        Scene sx;
        Scene sy;
        buildAnim(sx);
        buildAnim(sy);
        AnimationSystem sys;
        bool det = true;
        for (int i = 0; i < 30 && det; ++i) {
            sys.Update(sx.GetWorld(), lib);
            sys.Update(sy.GetWorld(), lib);
            if (HashWorld(sx.GetWorld(), nullptr) != HashWorld(sy.GetWorld(), nullptr)) {
                det = false;
            }
        }
        check(det, "same clip played twice: per-tick WorldHash identical");
        // 実際にアニメートされている (position が 0 から動いた)
        GameObject animGo = sx.Find("Animated");
        check(animGo && animGo.GetComponent<LocalTransform>()->position.x != 0.0f,
              "animator advanced the transform");
    }

    if (failCount == 0) {
        MYE_LOG_INFO("==== Scene serializer self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== Scene serializer self test: %d FAILURE(S) ====", failCount);
    return false;
}

} // namespace mye
