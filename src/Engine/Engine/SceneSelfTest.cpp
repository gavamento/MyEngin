#include "Engine/Engine/SceneSelfTest.h"

#include <algorithm>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

#include "Engine/Core/Components.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/NameUtil.h"
#include "Engine/Core/Profiler.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/Animation.h"
#include "Engine/Engine/EntityNaming.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/Parts.h"
#include "Engine/Engine/Prefab.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Engine/SceneSerializer.h"

#include <cstddef>
#include <filesystem>
#include <fstream>

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

    // ---- remap 一本化: 集合外 EntityRef の扱いが clone と extract で逆であること (M48b) ----
    // 唯一の組み込み serializable EntityRef である SpringJoint.connectedEntity で検証する。
    // **この 2 本が無いと zeroExternal を取り違えてもコンパイルもテストも通ってしまう**
    {
        Scene s;
        GameObject outside = s.CreateGameObjectTracked("Outside");
        GameObject src = s.CreateGameObjectTracked("Jointed");
        s.GetWorld().ApplyStructuralChanges();
        const uint64_t outsideFid = s.GetWorld().GetComponent<FileIdComponent>(outside.Id())->value;
        src.AddComponent<SpringJointComponent>()->connectedEntity = outside.Id();

        const nlohmann::json sub = SceneSerializer::SubtreeToJson(s, src.Id());
        const std::vector<uint64_t> roots = SceneSerializer::CloneSubtree(s, sub);
        s.GetWorld().ApplyStructuralChanges();
        GameObject clone = s.FindByFileId(roots.empty() ? 0 : roots[0]);
        auto* cj = clone ? clone.GetComponent<SpringJointComponent>() : nullptr;
        check(cj && cj->connectedEntity == outside.Id(),
              "clone: an EntityRef pointing outside the set is preserved (zeroExternal=false)");

        // 抽出 (プレハブ化) は集合外参照を 0 にする。
        // **対象コンポーネントを見つけたことを必須にする** — 見つからないまま真を返すと、
        // SpringJoint が出力から消える変異で空振り PASS してしまう
        const nlohmann::json local = Prefab::ExtractLocal(s, src.Id());
        bool found = false;
        bool zeroed = false;
        for (const nlohmann::json& item : local) {
            if (item.contains("components") && item["components"].contains("SpringJoint")) {
                found = true;
                zeroed = item["components"]["SpringJoint"].value("connectedEntity", 1ull) == 0ull;
            }
        }
        check(found && zeroed && outsideFid != 0,
              "extract: an EntityRef pointing outside the set is zeroed (zeroExternal=true)");
    }

    // ---- 兄弟名の一意化 (M48b。エディタ操作専用のヘルパ) ----
    {
        Scene s;
        World& w = s.GetWorld();
        GameObject rootCube = s.CreateGameObjectTracked("Cube");
        GameObject grp = s.CreateGameObjectTracked("Parent");
        GameObject childCube = s.CreateGameObjectTracked("Cube");
        childCube.SetParent(grp);
        w.ApplyStructuralChanges();

        check(MakeUniqueSiblingName(w, kNullEntity, "Sphere", kNullEntity) == "Sphere",
              "unique: a free name is returned unchanged");
        check(MakeUniqueSiblingName(w, kNullEntity, "Cube", kNullEntity) == "Cube (1)",
              "unique: a colliding root name gets ' (1)'");
        check(MakeUniqueSiblingName(w, kNullEntity, "Cube", rootCube.Id()) == "Cube",
              "unique: excluding the entity itself keeps its own name (rename no-op)");
        // 兄弟集合は親ごとに独立 — ルートの "Cube" は parent の子には影響しない
        check(MakeUniqueSiblingName(w, grp.Id(), "Cube", childCube.Id()) == "Cube",
              "unique: sibling sets are per-parent");
        check(MakeUniqueSiblingName(w, grp.Id(), "Cube", kNullEntity) == "Cube (1)",
              "unique: a colliding child name gets ' (1)'");

        // 予算ちょうど (63 バイト) の名前: 素の候補が衝突するので連番を付ける必要があるが、
        // 連番ぶんの領域が無い。base 側を先に詰めるので候補は必ず別文字列になる
        // (詰めずに後置するだけの実装だと切り詰めで元の名前に戻り、永久に衝突が解けない)
        const std::string longName(kMaxEntityNameBytes, 'x');
        SetEntityName(w, rootCube.Id(), longName);
        const std::string uniqLong = MakeUniqueSiblingName(w, kNullEntity, longName, kNullEntity);
        check(uniqLong.size() <= kMaxEntityNameBytes && uniqLong != longName
                  && uniqLong.substr(uniqLong.size() - 4) == " (1)",
              "unique: a name at the exact byte budget still yields a distinct numbered candidate");

        // マルチバイト文字を割らない。**境界をまたぐ予算**で後退ループを実際に働かせる
        // (3 の倍数の予算だと後退が起きず、後退ループを削除しても通ってしまう)
        std::string mb;
        for (int i = 0; i < 5; ++i) {
            mb += "あ"; // 3 バイト × 5 = 15 バイト
        }
        check(nameutil::TruncateUtf8(mb, 7).size() == 6, "TruncateUtf8 steps back to a boundary (7 -> 6)");
        check(nameutil::TruncateUtf8(mb, 8).size() == 6, "TruncateUtf8 steps back to a boundary (8 -> 6)");
        check(nameutil::TruncateUtf8(mb, 9).size() == 9, "TruncateUtf8 keeps an exact boundary (9)");
        check(nameutil::TruncateUtf8(mb, 0).empty() && nameutil::TruncateUtf8(mb, 99).size() == 15,
              "TruncateUtf8 handles zero and oversized budgets");
    }

    // ---- 名前のゼロ埋めと正規化 (M48b) ----
    {
        Scene s;
        World& w = s.GetWorld();
        GameObject g = s.CreateGameObjectTracked("LongEnoughName");
        w.ApplyStructuralChanges();
        auto* nc = w.GetComponent<NameComponent>(g.Id());
        SetEntityName(w, g.Id(), "Ab"); // 短くする: 以前のバイトが残ってはいけない
        bool tailZero = nc != nullptr;
        for (size_t i = 2; nc && i < sizeof(nc->value); ++i) {
            tailZero = tailZero && nc->value[i] == 0;
        }
        check(tailZero, "SetEntityName zero-fills the tail (WorldHash reads all 64 bytes)");

        check(SanitizeEntityName("  Hello  ", "GameObject") == "Hello",
              "sanitize: leading/trailing spaces are trimmed");
        check(SanitizeEntityName("a/b/c", "GameObject") == "abc",
              "sanitize: '/' is stripped (reserved for part paths)");
        check(SanitizeEntityName("   ", "GameObject") == "GameObject",
              "sanitize: an all-blank name falls back");
    }

    // ---- FinishRename: 本番の 2 つのリネーム UI が通る唯一の入口 (M48b) ----
    {
        Scene s;
        World& w = s.GetWorld();
        GameObject dupA = s.CreateGameObjectTracked("Cube");
        GameObject dupB = s.CreateGameObjectTracked("Cube"); // ロード/D&D で実際に起きる同名兄弟
        GameObject slash = s.CreateGameObjectTracked("a/b");
        w.ApplyStructuralChanges();

        // ★変更なし (Esc キャンセル / 無編集): 同名兄弟がいても改名してはいけない
        FinishRename(w, dupB.Id(), "Cube", "Cube");
        check(std::string(w.GetName(dupB.Id())) == "Cube",
              "FinishRename: an unchanged name is never uniquified (Esc / no-edit)");
        FinishRename(w, slash.Id(), "a/b", "a/b");
        check(std::string(w.GetName(slash.Id())) == "a/b",
              "FinishRename: an unchanged name is never sanitized (legacy names survive Esc)");

        // 実際に編集された場合は正規化 + 一意化する
        FinishRename(w, dupB.Id(), "  Cube  ", "Cube");
        check(std::string(w.GetName(dupB.Id())) == "Cube (1)",
              "FinishRename: an edited name is trimmed and uniquified against siblings");
        FinishRename(w, slash.Id(), "x/y", "a/b");
        check(std::string(w.GetName(slash.Id())) == "xy",
              "FinishRename: an edited name has '/' stripped");
        FinishRename(w, slash.Id(), "   ", "xy");
        check(std::string(w.GetName(slash.Id())) == "GameObject",
              "FinishRename: an edited all-blank name falls back");
        check(std::string(w.GetName(dupA.Id())) == "Cube", "FinishRename: siblings are untouched");
    }

    // ---- 同名兄弟のロード冪等性 (M48b の回帰防止の要) ----
    // ロード経路に一意化が混ざると Play/Stop 往復や Undo のたびに名前が育つ。
    // golden.rep は毎回録り直されるため replay_verify では捕まらない = ここで捕まえる
    {
        Scene s;
        for (int i = 0; i < 3; ++i) {
            s.CreateGameObjectTracked("Dup"); // 意図的に兄弟内で重複させる
        }
        s.GetWorld().ApplyStructuralChanges();
        const nlohmann::json save1 = SceneSerializer::SaveToJson(s);

        Scene s2;
        SceneSerializer::LoadFromJson(s2, save1);
        const nlohmann::json save2 = SceneSerializer::SaveToJson(s2);
        Scene s3;
        SceneSerializer::LoadFromJson(s3, save2);
        const nlohmann::json save3 = SceneSerializer::SaveToJson(s3);

        check(save1 == save2 && save2 == save3,
              "load/save is idempotent for duplicate sibling names (no renaming on load)");

        int dupCount = 0;
        EntityID r = s2.GetWorld().FirstRoot();
        while (!r.IsNull()) {
            auto* h = s2.GetWorld().GetComponent<HierarchyComponent>(r);
            if (std::string(s2.GetWorld().GetName(r)) == "Dup") {
                ++dupCount;
            }
            r = h ? h->nextSibling : kNullEntity;
        }
        check(dupCount == 3, "load keeps all three duplicate names as-is");

        // ApplyPartial は Undo/Redo 復元・プレハブ展開・CloneSubtree の共通出口。
        // ここに一意化が混ざると Undo→Redo のたびに " (n)" が積み増され冪等性が消える
        const nlohmann::json sub = SceneSerializer::SubtreeToJson(s2, s2.GetWorld().FirstRoot());
        SceneSerializer::ApplyPartial(s2, sub);
        SceneSerializer::ApplyPartial(s2, sub); // 2 回目も同じ結果でなければならない
        s2.GetWorld().ApplyStructuralChanges();
        check(SceneSerializer::SaveToJson(s2) == save2,
              "ApplyPartial is idempotent for duplicate sibling names (Undo/Redo restore path)");
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
        // M48e: 上書き判定は保存型の override リストが一次情報。ECS を直接書き換えたら
        // エディタが CaptureAfter でやっているのと同じ記録を明示的に行う必要がある
        Prefab::RecordOverrides(s7, lib, inst1.Id());
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

        // ★インスタンスルートの名前は Apply でベースへ焼かない (M48b)。
        //   兄弟名の一意化で 2 個目が "X (1)" になるため、焼くとアセットが汚染され、
        //   続く PropagateBaseChange が 1 個目まで改名して兄弟が両方同名になる
        {
            GameObject i1 = s7.FindByFileId(r1);
            const std::string baseNameBefore = i1 ? std::string(s7.GetWorld().GetName(i1.Id()))
                                                  : std::string();
            const std::string otherBefore = inst2 ? std::string(s7.GetWorld().GetName(inst2.Id()))
                                                  : std::string();
            SetEntityName(s7.GetWorld(), i1.Id(), baseNameBefore + " (1)"); // 一意化を模す
            check(Prefab::ApplyInstance(s7, lib, r1), "apply (renamed root) succeeds");
            s7.GetWorld().ApplyStructuralChanges();

            const PrefabAsset* a2 = lib.Get(hash);
            std::string baseRootName;
            if (a2) {
                for (const auto& it : a2->entities) {
                    if (it.value("fileId", 0ull) == 1ull) {
                        baseRootName = it.value("name", std::string());
                    }
                }
            }
            check(baseRootName == baseNameBefore,
                  "apply does NOT write the instance root name back to the prefab asset");
            check(inst2 && std::string(s7.GetWorld().GetName(inst2.Id())) == otherBefore,
                  "apply does NOT rename other instances via the root name");
        }
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

    // ---- 入れ子プレハブインスタンス (M48c) ----
    // 境界の定義は FindInstanceRoot (最近祖先) ただ 1 つ。抽出 / タグ付け / 列挙 / Apply が
    // すべてその境界に一致していることを確かめる。**内側メンバの localId は内側ベースの
    // ドメイン**なので、外側の連番で上書きすると内側の解決が静かに壊れる
    {
        auto findField = [](ComponentTypeId t, const char* fname) -> const FieldDesc* {
            for (const FieldDesc& f : ComponentRegistry::Get().Desc(t).fields) {
                if (std::string(f.name) == fname) {
                    return &f;
                }
            }
            return nullptr;
        };
        auto firstChild = [](Scene& sc, EntityID e) {
            auto* h = sc.GetWorld().GetComponent<HierarchyComponent>(e);
            return h ? h->firstChild : kNullEntity;
        };
        auto entryOf = [](const nlohmann::json& entities, uint64_t localId) {
            for (const nlohmann::json& it : entities) {
                if (it.value("fileId", 0ull) == localId) {
                    return it;
                }
            }
            return nlohmann::json::object();
        };
        auto hasComp = [](const nlohmann::json& item, const char* name) {
            return item.contains("components") && item["components"].contains(name);
        };
        // 常にオブジェクトを返す (未所持でも .value() が投げないように — 失敗は FAIL 報告させたい)
        auto compOf = [&hasComp](const nlohmann::json& item, const char* name) {
            return hasComp(item, name) ? item["components"][name] : nlohmann::json::object();
        };

        PrefabLibrary lib;
        const std::wstring innerPath =
            (std::filesystem::temp_directory_path() / L"mye_selftest_inner.prefab.json").wstring();
        const std::wstring outerPath =
            (std::filesystem::temp_directory_path() / L"mye_selftest_outer.prefab.json").wstring();

        // 内側ベース: Sword(local 1) > Blade(local 2)
        uint64_t innerHash = 0;
        {
            Scene si;
            GameObject sw = si.CreateGameObjectTracked("Sword");
            GameObject bl = si.CreateGameObjectTracked("Blade");
            bl.SetParent(sw);
            bl.SetLocalPosition(0.0f, 1.0f, 0.0f);
            si.GetWorld().ApplyStructuralChanges();
            innerHash = lib.Register(innerPath, "inner", Prefab::ExtractLocal(si, sw.Id()));
        }

        // 外側シーン: Goblin > Hand > [Sword インスタンス > Blade]
        Scene so;
        GameObject gob = so.CreateGameObjectTracked("Goblin");
        GameObject hand = so.CreateGameObjectTracked("Hand");
        hand.SetParent(gob);
        so.GetWorld().ApplyStructuralChanges();
        const uint64_t swordFid =
            Prefab::Instantiate(so, lib, innerHash, so.EnsureFileId(hand.Id()));
        so.GetWorld().ApplyStructuralChanges();
        check(swordFid != 0, "nested: inner instance placed under Hand");

        const uint64_t outerHash = Prefab::CreateAsset(so, lib, outerPath, gob.Id());
        so.GetWorld().ApplyStructuralChanges();
        const PrefabAsset* outerAsset = lib.Get(outerHash);
        const nlohmann::json outerBase =
            outerAsset ? outerAsset->entities : nlohmann::json::array();
        check(outerBase.size() == 4, "nested: outer asset holds the flattened 4-entity tree");

        // 外側ベース: 1=Goblin 2=Hand 3=Sword(入れ子ルート) 4=Blade(入れ子の配下)
        const nlohmann::json bGoblin = entryOf(outerBase, 1);
        const nlohmann::json bHand = entryOf(outerBase, 2);
        const nlohmann::json bSword = entryOf(outerBase, 3);
        const nlohmann::json bBlade = entryOf(outerBase, 4);
        check(bGoblin.value("name", std::string()) == "Goblin" && !hasComp(bGoblin, "PrefabInstance")
                  && !hasComp(bGoblin, "PrefabLink"),
              "nested: own-level entries carry no prefab tags in the base (tags are added on instantiate)");
        check(bHand.value("name", std::string()) == "Hand" && !hasComp(bHand, "PrefabLink"),
              "nested: own-level child entry carries no prefab tags either");
        check(compOf(bSword, "PrefabInstance").value("prefabHash", 0ull) == innerHash
                  && compOf(bSword, "PrefabInstance").value("outerLocalId", 0ull) == 3ull
                  && compOf(bSword, "PrefabLink").value("localId", 0ull) == 1ull,
              "nested: the inner root keeps its inner identity and records its outer position");
        check(compOf(bBlade, "PrefabLink").value("localId", 0ull) == 2ull
                  && !hasComp(bBlade, "PrefabInstance"),
              "nested: inner member keeps the INNER localId (not the outer 1..N numbering)");

        // CreateAsset がシーン側に付けたタグも同じ規則
        {
            World& wo = so.GetWorld();
            GameObject swGo = so.FindByFileId(swordFid);
            auto* swInst = swGo ? wo.GetComponent<PrefabInstanceComponent>(swGo.Id()) : nullptr;
            auto* swLink = swGo ? wo.GetComponent<PrefabLinkComponent>(swGo.Id()) : nullptr;
            check(swInst && swInst->prefabHash == innerHash && swInst->outerLocalId == 3ull
                      && swLink && swLink->localId == 1ull,
                  "nested: CreateAsset records the inner root's outer position without retagging it");
            auto* gobLink = wo.GetComponent<PrefabLinkComponent>(gob.Id());
            auto* gobInst = wo.GetComponent<PrefabInstanceComponent>(gob.Id());
            check(gobLink && gobLink->localId == 1ull && gobInst && gobInst->prefabHash == outerHash,
                  "nested: CreateAsset still tags the own level as the new instance");
        }

        // 別シーンへインスタンス化 → 入れ子タグが生きていること
        Scene s2;
        const uint64_t rootFid2 = Prefab::Instantiate(s2, lib, outerHash, 0);
        s2.GetWorld().ApplyStructuralChanges();
        GameObject root2 = s2.FindByFileId(rootFid2);
        const EntityID hand2 = root2 ? firstChild(s2, root2.Id()) : kNullEntity;
        const EntityID sword2 = hand2.IsNull() ? kNullEntity : firstChild(s2, hand2);
        const EntityID blade2 = sword2.IsNull() ? kNullEntity : firstChild(s2, sword2);
        check(!blade2.IsNull() && s2.GetWorld().AliveCount() == 4,
              "nested: instantiating the outer asset expands the whole nested tree");
        check(!blade2.IsNull()
                  && Prefab::FindInstanceRoot(s2.GetWorld(), blade2) == sword2
                  && Prefab::FindInstanceRoot(s2.GetWorld(), hand2) == root2.Id(),
              "nested: the instance boundary is the nearest PrefabInstance ancestor");
        {
            auto* bl2Link = s2.GetWorld().GetComponent<PrefabLinkComponent>(blade2);
            auto* sw2Inst = s2.GetWorld().GetComponent<PrefabInstanceComponent>(sword2);
            check(bl2Link && bl2Link->localId == 2ull && sw2Inst
                      && sw2Inst->prefabHash == innerHash && sw2Inst->outerLocalId == 3ull,
                  "nested: instantiate leaves inner tags alone instead of renumbering them");
        }

        // オーバーライド判定が内側ベースに解決されること (壊れていると base=null で常に false)
        const FieldDesc* posF = findField(LocalTransform::sTypeId, "position");
        check(posF && !Prefab::IsFieldOverridden(s2, lib, blade2, "LocalTransform", *posF),
              "nested: a fresh inner member reports no override");
        s2.GetWorld().GetComponent<LocalTransform>(blade2)->position.y = 42.0f;
        Prefab::RecordOverrides(s2, lib, blade2); // M48e: 記録も内側ベース基準で作られること
        check(posF && Prefab::IsFieldOverridden(s2, lib, blade2, "LocalTransform", *posF),
              "nested: an edited inner member is diffed against the INNER base");

        // Revert が境界を越えない
        Prefab::RevertInstance(s2, lib, rootFid2);
        s2.GetWorld().ApplyStructuralChanges();
        check(s2.GetWorld().GetComponent<LocalTransform>(blade2)->position.y == 42.0f,
              "nested: Revert on the outer instance does not reach into the inner one");
        Prefab::RevertInstance(s2, lib, s2.EnsureFileId(sword2));
        s2.GetWorld().ApplyStructuralChanges();
        check(s2.GetWorld().GetComponent<LocalTransform>(blade2)->position.y == 1.0f,
              "nested: Revert on the inner instance restores its own members");

        // 再抽出が元アセットと同型 (= 展開保存モデルの往復が閉じている)
        check(Prefab::ExtractLocal(s2, root2.Id()) == outerBase,
              "nested: re-extracting the instance reproduces the asset exactly");

        // Apply の往復 — ExtractLocalByLinks が内側の localId ドメインと衝突しないこと。
        // 壊れていると Sword/Blade が Goblin/Hand と同じ fileId 1,2 を名乗ってベースが潰れる
        check(Prefab::ApplyInstance(s2, lib, rootFid2), "nested: apply on a nested instance succeeds");
        const PrefabAsset* outerAfter = lib.Get(outerHash);
        check(outerAfter && outerAfter->entities == outerBase,
              "nested: apply round-trips an unchanged nested asset byte-for-byte");

        std::error_code ec;
        std::filesystem::remove(innerPath, ec);
        std::filesystem::remove(outerPath, ec);
    }

    // ---- 3 階層の入れ子 A > B > C (M48c) ----
    // 「外側 ID = 自分が直接所属するインスタンスのドメイン」は再帰的な規約なので、深さに
    // 依存しないことを 2 階層とは別に固定する。C のルートが持つ outerLocalId は **B のドメイン**
    // のままで、A の連番では書き換わらない (書き換えると B 単体の Apply が壊れる)
    {
        auto tmpPath = [](const wchar_t* n) {
            return (std::filesystem::temp_directory_path() / n).wstring();
        };
        auto firstChild = [](Scene& sc, EntityID e) {
            auto* h = sc.GetWorld().GetComponent<HierarchyComponent>(e);
            return h ? h->firstChild : kNullEntity;
        };

        PrefabLibrary lib;
        const std::wstring pathC = tmpPath(L"mye_selftest_n3c.prefab.json");
        const std::wstring pathB = tmpPath(L"mye_selftest_n3b.prefab.json");
        const std::wstring pathA = tmpPath(L"mye_selftest_n3a.prefab.json");

        uint64_t hashC = 0;
        {
            Scene s;
            GameObject gem = s.CreateGameObjectTracked("Gem");
            gem.SetLocalPosition(0.0f, 0.0f, 3.0f);
            s.GetWorld().ApplyStructuralChanges();
            hashC = lib.Register(pathC, "c", Prefab::ExtractLocal(s, gem.Id()));
        }
        uint64_t hashB = 0;
        {
            Scene s;
            GameObject sword = s.CreateGameObjectTracked("Sword");
            GameObject guard = s.CreateGameObjectTracked("Guard");
            guard.SetParent(sword);
            s.GetWorld().ApplyStructuralChanges();
            Prefab::Instantiate(s, lib, hashC, s.EnsureFileId(guard.Id()));
            s.GetWorld().ApplyStructuralChanges();
            hashB = lib.Register(pathB, "b", Prefab::ExtractLocal(s, sword.Id()));
        }

        // A: Hero(1) > Hand(2) > [B: Sword(3) > Guard(4) > [C: Gem(5)]]
        Scene sa;
        GameObject hero = sa.CreateGameObjectTracked("Hero");
        GameObject hand = sa.CreateGameObjectTracked("Hand");
        hand.SetParent(hero);
        sa.GetWorld().ApplyStructuralChanges();
        Prefab::Instantiate(sa, lib, hashB, sa.EnsureFileId(hand.Id()));
        sa.GetWorld().ApplyStructuralChanges();
        const uint64_t hashA = Prefab::CreateAsset(sa, lib, pathA, hero.Id());
        sa.GetWorld().ApplyStructuralChanges();
        const PrefabAsset* assetA = lib.Get(hashA);
        const nlohmann::json baseA = assetA ? assetA->entities : nlohmann::json::array();
        const PrefabAsset* assetB = lib.Get(hashB);
        const nlohmann::json baseB = assetB ? assetB->entities : nlohmann::json::array();
        check(baseA.size() == 5, "3-level: the outermost asset flattens all three levels");

        // 退行時に .value() が type_error を投げて --selftest 全体を落とさないよう、
        // 見つからない場合は object のままにして FAIL 報告に落とす
        nlohmann::json gemEntry = nlohmann::json::object();
        for (const nlohmann::json& it : baseA) {
            if (it.value("name", std::string()) == "Gem") {
                gemEntry = it;
            }
        }
        const nlohmann::json gemInst =
            (gemEntry.contains("components") && gemEntry["components"].contains("PrefabInstance"))
            ? gemEntry["components"]["PrefabInstance"]
            : nlohmann::json::object();
        check(gemEntry.value("fileId", 0ull) == 5ull
                  && gemInst.value("outerLocalId", 0ull) == 3ull,
              "3-level: the innermost root keeps its MIDDLE-domain outerLocalId (not A's numbering)");

        // 別シーンへ展開 → 各階層の境界が独立に立つ
        Scene s2;
        const uint64_t rootA = Prefab::Instantiate(s2, lib, hashA, 0);
        s2.GetWorld().ApplyStructuralChanges();
        World& w2 = s2.GetWorld();
        GameObject heroGo = s2.FindByFileId(rootA);
        const EntityID hand2 = heroGo ? firstChild(s2, heroGo.Id()) : kNullEntity;
        const EntityID sword2 = hand2.IsNull() ? kNullEntity : firstChild(s2, hand2);
        const EntityID guard2 = sword2.IsNull() ? kNullEntity : firstChild(s2, sword2);
        const EntityID gem2 = guard2.IsNull() ? kNullEntity : firstChild(s2, guard2);
        check(!gem2.IsNull() && Prefab::FindInstanceRoot(w2, gem2) == gem2
                  && Prefab::FindInstanceRoot(w2, guard2) == sword2
                  && Prefab::FindInstanceRoot(w2, hand2) == heroGo.Id(),
              "3-level: each level's boundary resolves to its own instance root");
        check(Prefab::ExtractLocal(s2, heroGo.Id()) == baseA,
              "3-level: re-extracting the outermost instance reproduces the asset exactly");

        // Apply を各階層で — 外側 (A) と中間 (B) のどちらのベースも壊れないこと。
        // ExtractLocalByLinks の「最小の空き番号」採番が DFS 連番と一致していないとここで崩れる
        check(Prefab::ApplyInstance(s2, lib, rootA), "3-level: apply on the outermost instance succeeds");
        check(lib.Get(hashA) && lib.Get(hashA)->entities == baseA,
              "3-level: apply round-trips the outermost asset unchanged");
        check(Prefab::ApplyInstance(s2, lib, s2.EnsureFileId(sword2)),
              "3-level: apply on the middle instance succeeds");
        check(lib.Get(hashB) && lib.Get(hashB)->entities == baseB,
              "3-level: apply on the middle instance round-trips ITS own asset unchanged");

        std::error_code ec;
        std::filesystem::remove(pathA, ec);
        std::filesystem::remove(pathB, ec);
        std::filesystem::remove(pathC, ec);
    }

    // ---- 境界の 3 つの穴 (M48c、敵対的レビュー由来の回帰防止) ----
    // (1) 内側インスタンスの配下にユーザーが足した **タグ無しの子** に、外側の localId を
    //     付けてはいけない (FindInstanceRoot は内側を返すので内側ドメインとして誤解決される)
    // (2) 自レベルの兄弟が入れ子の **後ろ** にいる木で、連番が内側配下ぶんも消費すること
    // (3) 削除で空いた localId を内側配下へ再利用してはいけない (他インスタンスがまだ名乗っている)
    {
        auto firstChild = [](Scene& sc, EntityID e) {
            auto* h = sc.GetWorld().GetComponent<HierarchyComponent>(e);
            return h ? h->firstChild : kNullEntity;
        };
        auto tmpPath = [](const wchar_t* n) {
            return (std::filesystem::temp_directory_path() / n).wstring();
        };
        auto nameOfLocal = [](const nlohmann::json& entities, uint64_t localId) {
            for (const nlohmann::json& it : entities) {
                if (it.value("fileId", 0ull) == localId) {
                    return it.value("name", std::string());
                }
            }
            return std::string();
        };

        PrefabLibrary lib;
        const std::wstring innerPath = tmpPath(L"mye_selftest_h_inner.prefab.json");
        const std::wstring outerPath = tmpPath(L"mye_selftest_h_outer.prefab.json");

        uint64_t innerHash = 0;
        {
            Scene s;
            GameObject sw = s.CreateGameObjectTracked("Sword");
            GameObject bl = s.CreateGameObjectTracked("Blade");
            bl.SetParent(sw);
            s.GetWorld().ApplyStructuralChanges();
            innerHash = lib.Register(innerPath, "inner", Prefab::ExtractLocal(s, sw.Id()));
        }

        // Goblin > [ Foot, Hand > [Sword > Blade > Decal] ]
        //   DFS: 1 Goblin / 2 Foot / 3 Hand / 4 Sword / 5 Blade / 6 Decal
        // Foot が入れ子の **前**、Decal は内側配下のユーザー追加 (タグ無し)
        Scene so;
        GameObject gob = so.CreateGameObjectTracked("Goblin");
        GameObject foot = so.CreateGameObjectTracked("Foot");
        GameObject hand = so.CreateGameObjectTracked("Hand");
        foot.SetParent(gob);
        hand.SetParent(gob);
        so.GetWorld().ApplyStructuralChanges();
        const uint64_t swordFid =
            Prefab::Instantiate(so, lib, innerHash, so.EnsureFileId(hand.Id()));
        so.GetWorld().ApplyStructuralChanges();
        GameObject swordGo = so.FindByFileId(swordFid);
        GameObject decal = so.CreateGameObjectTracked("Decal");
        // Blade の子 (= 内側インスタンスの配下) に置く
        decal.SetParent(GameObject(&so.GetWorld(), firstChild(so, swordGo.Id())));
        so.GetWorld().ApplyStructuralChanges();

        const uint64_t outerHash = Prefab::CreateAsset(so, lib, outerPath, gob.Id());
        so.GetWorld().ApplyStructuralChanges();
        const nlohmann::json baseOuter =
            lib.Get(outerHash) ? lib.Get(outerHash)->entities : nlohmann::json::array();
        check(baseOuter.size() == 6 && nameOfLocal(baseOuter, 2) == "Foot"
                  && nameOfLocal(baseOuter, 3) == "Hand" && nameOfLocal(baseOuter, 6) == "Decal",
              "holes: the base numbering counts inner-instance members too (own-level siblings keep DFS order)");
        check(!so.GetWorld().GetComponent<PrefabLinkComponent>(decal.Id()),
              "holes: CreateAsset leaves a user-added child inside an inner instance untagged");

        // 別シーンへ展開 — Decal に外側の PrefabLink が付かないこと (境界判定の代理はここで割れる)
        Scene s2;
        const uint64_t rootFid2 = Prefab::Instantiate(s2, lib, outerHash, 0);
        s2.GetWorld().ApplyStructuralChanges();
        World& w2 = s2.GetWorld();
        GameObject root2 = s2.FindByFileId(rootFid2);
        EntityID decal2 = kNullEntity;
        EntityID sword2 = kNullEntity;
        EntityID foot2 = kNullEntity;
        {
            // Goblin > [Foot, Hand > Sword > Blade > Decal]
            foot2 = root2 ? firstChild(s2, root2.Id()) : kNullEntity;
            auto* fh = foot2.IsNull() ? nullptr : w2.GetComponent<HierarchyComponent>(foot2);
            const EntityID hand2 = fh ? fh->nextSibling : kNullEntity;
            sword2 = hand2.IsNull() ? kNullEntity : firstChild(s2, hand2);
            const EntityID blade2 = sword2.IsNull() ? kNullEntity : firstChild(s2, sword2);
            decal2 = blade2.IsNull() ? kNullEntity : firstChild(s2, blade2);
        }
        check(!decal2.IsNull() && std::string(w2.GetName(decal2)) == "Decal"
                  && !w2.GetComponent<PrefabLinkComponent>(decal2),
              "holes: instantiate does NOT give an outer localId to a tagless child inside an inner instance");

        // CollectInstanceMembers の境界 — 内側ルート自身もメンバに含めないこと
        {
            std::vector<EntityID> members;
            std::vector<EntityID> inners;
            Prefab::CollectInstanceMembers(w2, root2.Id(), members, &inners);
            const bool hasSword =
                std::find(members.begin(), members.end(), sword2) != members.end();
            check(members.size() == 3 && !hasSword && inners.size() == 1 && inners[0] == sword2,
                  "holes: CollectInstanceMembers excludes the inner root itself and reports it separately");
        }

        // 削除で空いた localId を内側配下へ再利用しない (別インスタンスが名乗っているため)
        Scene s3;
        const uint64_t instA = Prefab::Instantiate(s3, lib, outerHash, 0);
        const uint64_t instB = Prefab::Instantiate(s3, lib, outerHash, 0);
        s3.GetWorld().ApplyStructuralChanges();
        World& w3 = s3.GetWorld();
        GameObject rootA = s3.FindByFileId(instA);
        const EntityID footA = rootA ? firstChild(s3, rootA.Id()) : kNullEntity;
        GameObject rootB = s3.FindByFileId(instB);
        const EntityID footB = rootB ? firstChild(s3, rootB.Id()) : kNullEntity;
        w3.DestroyEntity(footA); // A の自レベルメンバ (localId=2) を消す
        w3.ApplyStructuralChanges();
        check(Prefab::ApplyInstance(s3, lib, instA), "holes: apply after deleting an own-level member succeeds");
        s3.GetWorld().ApplyStructuralChanges();
        const nlohmann::json baseAfter =
            lib.Get(outerHash) ? lib.Get(outerHash)->entities : nlohmann::json::array();
        check(nameOfLocal(baseAfter, 2).empty(),
              "holes: the deleted member's localId is left vacant (not recycled for an inner member)");
        check(!footB.IsNull() && w3.IsAlive(footB) && std::string(w3.GetName(footB)) == "Foot",
              "holes: the other instance's member is not renamed into a different entity");

        std::error_code ec;
        std::filesystem::remove(innerPath, ec);
        std::filesystem::remove(outerPath, ec);
    }

    // ---- Unpack Prefab (M50b) ----
    // インスタンスをプレハブから切り離す。規約: 入れ子内 (祖先にルートあり) は拒否 /
    // 内側インスタンスは無傷でシーン直接配置へ / Undo (全量スナップショット +
    // ApplyPartial removeHiddenMissing) で完全に戻る / 外側から順に解ける
    {
        auto firstChild = [](Scene& sc, EntityID e) {
            auto* h = sc.GetWorld().GetComponent<HierarchyComponent>(e);
            return h ? h->firstChild : kNullEntity;
        };
        auto tmpPath = [](const wchar_t* n) {
            return (std::filesystem::temp_directory_path() / n).wstring();
        };
        PrefabLibrary lib;
        const std::wstring innerPath = tmpPath(L"mye_selftest_u_inner.prefab.json");
        const std::wstring outerPath = tmpPath(L"mye_selftest_u_outer.prefab.json");
        uint64_t innerHash = 0;
        {
            Scene si;
            GameObject sw = si.CreateGameObjectTracked("Sword");
            GameObject bl = si.CreateGameObjectTracked("Blade");
            bl.SetParent(sw);
            si.GetWorld().ApplyStructuralChanges();
            innerHash = lib.Register(innerPath, "inner", Prefab::ExtractLocal(si, sw.Id()));
        }
        // Goblin > [ Foot (Part 持ち = 構造ロックの検体), Hand > Sword (内側) > Blade ]
        Scene s;
        World& w = s.GetWorld();
        GameObject gob = s.CreateGameObjectTracked("Goblin");
        GameObject foot = s.CreateGameObjectTracked("Foot");
        GameObject hand = s.CreateGameObjectTracked("Hand");
        foot.SetParent(gob);
        hand.SetParent(gob);
        foot.AddComponent<PartComponent>()->tag = 0x1234;
        w.ApplyStructuralChanges();
        Prefab::Instantiate(s, lib, innerHash, s.EnsureFileId(hand.Id()));
        w.ApplyStructuralChanges();
        const uint64_t outerHash = Prefab::CreateAsset(s, lib, outerPath, gob.Id());
        w.ApplyStructuralChanges();
        const uint64_t rootFid = s.EnsureFileId(gob.Id());
        const EntityID sword = firstChild(s, hand.Id());
        const EntityID blade = sword.IsNull() ? kNullEntity : firstChild(s, sword);
        check(outerHash != 0 && !sword.IsNull() && !blade.IsNull()
                  && Parts::IsStructureLocked(w, foot.Id()),
              "unpack: setup — the part member is structure-locked while packed");

        // (1) 入れ子内 (内側ルート) の Unpack は拒否 — 状態も無傷
        check(!Prefab::UnpackInstance(s, s.EnsureFileId(sword))
                  && w.GetComponent<PrefabInstanceComponent>(sword) != nullptr
                  && w.GetComponent<PrefabLinkComponent>(blade) != nullptr,
              "unpack: refuses inside an outer instance and leaves the tags alone");

        // Undo 相当の全量スナップショット (UndoStack::CaptureBefore と同じ実体)
        const nlohmann::json before = SceneSerializer::SubtreeToJson(s, gob.Id());

        // (2) 本体: 直属タグが剥がれ、部位の構造ロックが外れる
        check(Prefab::UnpackInstance(s, rootFid), "unpack: succeeds on a top-level instance");
        w.ApplyStructuralChanges();
        check(w.GetComponent<PrefabInstanceComponent>(gob.Id()) == nullptr
                  && w.GetComponent<PrefabLinkComponent>(gob.Id()) == nullptr
                  && w.GetComponent<PrefabLinkComponent>(foot.Id()) == nullptr
                  && w.GetComponent<PrefabLinkComponent>(hand.Id()) == nullptr
                  && !Parts::IsStructureLocked(w, foot.Id()),
              "unpack: own-level tags are removed and the part lock is released");

        // (3) 内側は無傷 — タグ温存 + outerLocalId=0 (シーン直接配置扱い) + メンバ無傷
        auto* swordPi = w.GetComponent<PrefabInstanceComponent>(sword);
        check(swordPi != nullptr && swordPi->prefabHash == innerHash && swordPi->outerLocalId == 0
                  && w.GetComponent<PrefabLinkComponent>(sword) != nullptr
                  && w.GetComponent<PrefabLinkComponent>(blade) != nullptr,
              "unpack: the inner instance survives intact and becomes scene-placed");

        // (4) Undo 往復: removeHiddenMissing=true (UndoStack の復元と同じ) でタグとロックが戻る
        check(SceneSerializer::ApplyPartial(s, before, /*removeHiddenMissing=*/true),
              "unpack: the undo snapshot applies");
        w.ApplyStructuralChanges();
        check(w.GetComponent<PrefabInstanceComponent>(gob.Id()) != nullptr
                  && w.GetComponent<PrefabLinkComponent>(foot.Id()) != nullptr
                  && Parts::IsStructureLocked(w, foot.Id()),
              "unpack: undo restores the instance tags and the part lock");

        // (5) Redo 相当で再度解き、外側が消えた今度は内側も Unpack できる (順に解ける規約)
        check(Prefab::UnpackInstance(s, rootFid), "unpack: redo works after undo");
        w.ApplyStructuralChanges();
        check(Prefab::UnpackInstance(s, s.EnsureFileId(sword)),
              "unpack: the inner instance can be unpacked once the outer is gone");
        w.ApplyStructuralChanges();
        check(w.GetComponent<PrefabInstanceComponent>(sword) == nullptr
                  && w.GetComponent<PrefabLinkComponent>(blade) == nullptr,
              "unpack: unpacking the ex-inner instance strips its own tags");

        std::error_code ec;
        std::filesystem::remove(innerPath, ec);
        std::filesystem::remove(outerPath, ec);
    }

    // ---- .actor.json フォーマット v1 + .prefab.json 互換読込 + 複数ルート (M48d) ----
    {
        auto tmpPath = [](const wchar_t* n) {
            return (std::filesystem::temp_directory_path() / n).wstring();
        };
        auto writeText = [](const std::wstring& path, const std::string& text) {
            std::ofstream f(std::filesystem::path(path), std::ios::binary);
            f.write(text.data(), static_cast<std::streamsize>(text.size()));
        };
        auto readJson = [](const std::wstring& path) {
            std::ifstream f(std::filesystem::path(path), std::ios::binary);
            nlohmann::json j;
            if (f) {
                f >> j;
            }
            return j;
        };
        auto entityDoc = [](const char* declKey, const char* name, const nlohmann::json& ents) {
            nlohmann::json d;
            d["engine"] = "MyEngine";
            d[declKey] = 1;
            d["name"] = name;
            d["entities"] = ents;
            return d.dump(2);
        };
        auto mkEntity = [](uint64_t fileId, const char* name, uint64_t parent, uint32_t childIndex) {
            nlohmann::json e;
            e["fileId"] = fileId;
            e["name"] = name;
            e["childIndex"] = childIndex;
            e["components"] = nlohmann::json::object();
            if (parent != 0) {
                e["parent"] = parent;
            }
            return e;
        };

        PrefabLibrary lib;
        const std::wstring actorPath = tmpPath(L"mye_selftest_fmt.actor.json");
        const std::wstring prefabPath = tmpPath(L"mye_selftest_fmt.prefab.json");
        const std::wstring badPath = tmpPath(L"mye_selftest_fmt_bad.actor.json");
        const std::wstring multiPath = tmpPath(L"mye_selftest_fmt_multi.actor.json");

        // (1) actor:1 を受理し、宣言キーを覚える
        writeText(actorPath,
                  entityDoc("actor", "FmtActor",
                            nlohmann::json::array({ mkEntity(1, "ActorRoot", 0, 0) })));
        const uint64_t actorHash = lib.LoadFromFile(actorPath);
        check(actorHash != 0 && lib.Get(actorHash) && lib.Get(actorHash)->actorFormat,
              "format: \"actor\":1 loads and is remembered as actor format");

        // (2) prefab:1 を部分集合として受理する (互換読込)
        writeText(prefabPath,
                  entityDoc("prefab", "FmtPrefab",
                            nlohmann::json::array({ mkEntity(1, "PrefabRoot", 0, 0) })));
        const uint64_t prefabHash = lib.LoadFromFile(prefabPath);
        check(prefabHash != 0 && lib.Get(prefabHash) && !lib.Get(prefabHash)->actorFormat,
              "format: \"prefab\":1 loads as the compatible subset (prefab format)");

        // (3) どちらのキーも無い .json は弾く (従来は entities だけ見て素通ししていた)
        writeText(badPath, R"({"engine":"MyEngine","sceneName":"x","entities":[]})");
        check(lib.LoadFromFile(badPath) == 0,
              "format: a json with neither actor:1 nor prefab:1 is rejected");

        check(PrefabLibrary::IsComposePath(L"a\\b.ACTOR.JSON")
                  && PrefabLibrary::IsComposePath(L"a\\b.prefab.json")
                  && !PrefabLibrary::IsComposePath(L"a\\b.scene.json"),
              "format: IsComposePath accepts both suffixes case-insensitively");

        // (4) 書き戻しで宣言キーを維持する (強制移行しない)
        {
            Scene s;
            const uint64_t r = Prefab::Instantiate(s, lib, prefabHash, 0);
            s.GetWorld().ApplyStructuralChanges();
            check(Prefab::ApplyInstance(s, lib, r), "format: apply on a .prefab.json instance succeeds");
            const nlohmann::json back = readJson(prefabPath);
            check(back.value("prefab", 0) == 1 && !back.contains("actor"),
                  "format: apply keeps the original \"prefab\":1 key (no forced migration)");
        }
        {
            Scene s;
            const uint64_t r = Prefab::Instantiate(s, lib, actorHash, 0);
            s.GetWorld().ApplyStructuralChanges();
            check(Prefab::ApplyInstance(s, lib, r), "format: apply on a .actor.json instance succeeds");
            const nlohmann::json back = readJson(actorPath);
            check(back.value("actor", 0) == 1 && !back.contains("prefab"),
                  "format: apply keeps the \"actor\":1 key");
        }

        // (5) 複数ルート (ミニシーン型) はラッパーで包む
        writeText(multiPath,
                  entityDoc("actor", "MiniScene",
                            // DFS 順 + 正しい childIndex = ExtractLocal が吐く形。
                            // 手書きで順序や childIndex が崩れていても読めるが、Apply で正規化される
                            nlohmann::json::array({ mkEntity(1, "RootA", 0, 0),
                                                    mkEntity(3, "ChildOfA", 1, 0),
                                                    mkEntity(2, "RootB", 0, 1) })));
        const uint64_t multiHash = lib.LoadFromFile(multiPath);
        Scene sm;
        const uint64_t wrapFid = Prefab::Instantiate(sm, lib, multiHash, 0);
        sm.GetWorld().ApplyStructuralChanges();
        World& wm = sm.GetWorld();
        GameObject wrap = sm.FindByFileId(wrapFid);
        check(wrap && std::string(wm.GetName(wrap.Id())) == "MiniScene",
              "multi-root: instantiation is wrapped in a group named after the asset");
        check(wm.AliveCount() == 4 && wm.GetParent(wrap.Id()).IsNull(),
              "multi-root: the wrapper is the single root of the instance (3 members + wrapper)");
        {
            auto* wl = wrap ? wm.GetComponent<PrefabLinkComponent>(wrap.Id()) : nullptr;
            auto* wi = wrap ? wm.GetComponent<PrefabInstanceComponent>(wrap.Id()) : nullptr;
            check(wl && wl->localId == 0 && wi && wi->prefabHash == multiHash,
                  "multi-root: the wrapper links to localId 0 (= no counterpart in the base)");
            // 両ルートがラッパーの子になり、どちらも自分の localId を保つ
            std::vector<std::string> kids;
            auto* h = wrap ? wm.GetComponent<HierarchyComponent>(wrap.Id()) : nullptr;
            for (EntityID c = h ? h->firstChild : kNullEntity; !c.IsNull();) {
                kids.push_back(wm.GetName(c));
                auto* ch = wm.GetComponent<HierarchyComponent>(c);
                c = ch ? ch->nextSibling : kNullEntity;
            }
            check(kids.size() == 2 && kids[0] == "RootA" && kids[1] == "RootB",
                  "multi-root: every base root becomes a child of the wrapper (order preserved)");
        }
        // ラッパーはベース対応物が無いので diff / Revert / Apply から自然に外れる
        {
            std::vector<EntityID> members;
            Prefab::CollectInstanceMembers(wm, wrap.Id(), members);
            check(members.size() == 4, "multi-root: the wrapper is still a member for enumeration");
            check(Prefab::ApplyInstance(sm, lib, wrapFid), "multi-root: apply succeeds");
            // 手書きの最小ファイルは初回 Apply で正規化される (WriteEntity が実体の
            // コンポーネントを書き出す) ので、ここで見るのは「ラッパーが焼かれていないか」
            const nlohmann::json baseNorm =
                lib.Get(multiHash) ? lib.Get(multiHash)->entities : nlohmann::json::array();
            bool wrapperBaked = false;
            int rootCount = 0;
            for (const nlohmann::json& it : baseNorm) {
                if (it.value("name", std::string()) == "MiniScene") {
                    wrapperBaked = true;
                }
                if (!it.contains("parent")) {
                    ++rootCount;
                }
            }
            check(baseNorm.size() == 3 && !wrapperBaked && rootCount == 2,
                  "multi-root: apply keeps the base multi-rooted and does not bake the wrapper in");

            // 正規化後は wrap → unwrap → wrap が安定する (ID も配列順も動かない)
            Scene sm2;
            const uint64_t wrap2 = Prefab::Instantiate(sm2, lib, multiHash, 0);
            sm2.GetWorld().ApplyStructuralChanges();
            check(Prefab::ApplyInstance(sm2, lib, wrap2), "multi-root: re-apply succeeds");
            check(lib.Get(multiHash) && lib.Get(multiHash)->entities == baseNorm,
                  "multi-root: a normalized multi-root asset round-trips byte-for-byte");
        }

        std::error_code ec;
        for (const std::wstring& p : { actorPath, prefabPath, badPath, multiPath }) {
            std::filesystem::remove(p, ec);
        }
    }

    // ---- シーン override リスト + ロード時ベース更新 (M48e) ----
    // M13 のライブ diff は「シーンを閉じている間にベースが変わった」ケースを誤判定する
    // (ユーザーが触っていないフィールドまで上書き扱いになり、二度とベース更新に追随しない)。
    // 保存型の override リストでそこを直したので、以下 4 点を機械的に押さえる:
    //   (1) 上書きが overrides キーで往復する  (2) 再ロードで非 override だけベース最新値へ
    //   (3) レガシー (キー無し) シーンはビット不変ロード  (4) Play/Stop 往復でリスト不変
    {
        PrefabLibrary lib;
        const std::wstring ovrPath =
            (std::filesystem::temp_directory_path() / L"mye_selftest_ovr.actor.json").wstring();
        const std::string kPosKey = "LocalTransform.position";

        // ベース: Root(pos 1,2,3 / scale 1,1,1) + Child(pos 0.5,0,0)
        Scene srcScene;
        GameObject ovrRoot = srcScene.CreateGameObjectTracked("OvrRoot");
        ovrRoot.SetLocalPosition(1.0f, 2.0f, 3.0f);
        GameObject ovrKid = srcScene.CreateGameObjectTracked("OvrChild");
        ovrKid.SetParent(ovrRoot);
        ovrKid.SetLocalPosition(0.5f, 0.0f, 0.0f);
        srcScene.GetWorld().ApplyStructuralChanges();
        const uint64_t ovrHash = Prefab::CreateAsset(srcScene, lib, ovrPath, ovrRoot.Id());

        Scene s;
        const uint64_t rootFid = Prefab::Instantiate(s, lib, ovrHash, 0);
        s.GetWorld().ApplyStructuralChanges();
        GameObject inst = s.FindByFileId(rootFid);
        uint64_t kidFid = 0;
        if (auto* h = s.GetWorld().GetComponent<HierarchyComponent>(inst.Id())) {
            kidFid = s.EnsureFileId(h->firstChild);
        }
        check(s.HasOverrideRecord(rootFid) && s.GetOverrides(rootFid)->empty()
                  && s.HasOverrideRecord(kidFid),
              "override: a fresh instance is recorded as new-format with an empty list");

        // ルートの position だけ上書き (エディタの CaptureAfter に相当する記録を明示的に行う)
        inst.GetComponent<LocalTransform>()->position.x = 99.0f;
        Prefab::RecordOverrides(s, lib, inst.Id());
        check(s.GetOverrides(rootFid) && *s.GetOverrides(rootFid) == std::set<std::string>{ kPosKey },
              "override: editing one field records exactly that leaf key");

        // (1) 保存往復 — overrides キーがソート済み配列で出て、ロードで戻る
        const nlohmann::json saved = SceneSerializer::SaveToJson(s);
        {
            bool wrote = false;
            for (const nlohmann::json& it : saved["entities"]) {
                if (it.value("fileId", 0ull) == rootFid) {
                    wrote = it.contains("overrides")
                        && it["overrides"] == nlohmann::json::array({ kPosKey });
                }
            }
            check(wrote, "override: SaveToJson emits the list as an \"overrides\" array");
            Scene sl;
            SceneSerializer::LoadFromJson(sl, saved);
            check(sl.GetOverrides(rootFid)
                      && *sl.GetOverrides(rootFid) == std::set<std::string>{ kPosKey },
                  "override: LoadFromJson restores the list");
        }

        // ---- ベースを「シーンを閉じている間に」書き換える ----
        // Root: position (7,8,9) + scale (2,2,2) / Child: position (0.25,0,0) + 改名
        {
            const PrefabAsset* ovrAsset = lib.Get(ovrHash);
            nlohmann::json newBase = ovrAsset ? ovrAsset->entities : nlohmann::json::array();
            const std::string assetName = ovrAsset ? ovrAsset->name : std::string();
            for (nlohmann::json& it : newBase) {
                nlohmann::json& lt = it["components"]["LocalTransform"];
                if (it.value("fileId", 0ull) == 1ull) {
                    lt["position"] = nlohmann::json::array({ 7.0f, 8.0f, 9.0f });
                    lt["scale"] = nlohmann::json::array({ 2.0f, 2.0f, 2.0f });
                } else {
                    lt["position"] = nlohmann::json::array({ 0.25f, 0.0f, 0.0f });
                    it["name"] = "RenamedChild";
                }
            }
            lib.Register(ovrPath, assetName, newBase);
        }

        // (2) 再ロード + refresh: override は据え置き、非 override だけ新ベースへ追随する
        {
            Scene s2;
            SceneSerializer::LoadFromJson(s2, saved);
            Prefab::RefreshNonOverridden(s2, lib);
            GameObject i2 = s2.FindByFileId(rootFid);
            GameObject k2 = s2.FindByFileId(kidFid);
            const LocalTransform* rlt = i2 ? i2.GetComponent<LocalTransform>() : nullptr;
            const LocalTransform* klt = k2 ? k2.GetComponent<LocalTransform>() : nullptr;
            check(rlt && rlt->position.x == 99.0f && rlt->position.y == 2.0f,
                  "override: the overridden field survives a base change (whole leaf field)");
            check(rlt && rlt->scale.x == 2.0f,
                  "override: a non-overridden field of the same component follows the new base");
            check(klt && klt->position.x == 0.25f,
                  "override: a non-overridden member follows the new base");
            check(k2 && std::string(s2.GetWorld().GetName(k2.Id())) == "RenamedChild",
                  "override: a non-overridden name follows the new base");
            check(s2.GetOverrides(rootFid)
                      && *s2.GetOverrides(rootFid) == std::set<std::string>{ kPosKey },
                  "override: refresh does not disturb the stored list");
        }

        // (3) レガシー (overrides キー無し) はビット不変ロード + ライブ diff からの移行
        {
            nlohmann::json legacy = saved;
            for (nlohmann::json& it : legacy["entities"]) {
                it.erase("overrides");
            }
            Scene s3;
            SceneSerializer::LoadFromJson(s3, legacy);
            Prefab::RefreshNonOverridden(s3, lib);
            nlohmann::json after = SceneSerializer::SaveToJson(s3);
            for (nlohmann::json& it : after["entities"]) {
                it.erase("overrides"); // 移行で付いたキーだけを外して値を比較する
            }
            check(after == legacy,
                  "override: a legacy scene loads bit-invariant (refresh must not touch it)");
            check(s3.GetOverrides(rootFid) && s3.GetOverrides(rootFid)->count(kPosKey) == 1,
                  "override: a legacy instance is migrated from the live diff at load");
        }

        // (4) Play/Stop (SaveToJson → LoadFromJson) の往復でリストが変わらないこと
        {
            Scene s4;
            SceneSerializer::LoadFromJson(s4, saved);
            const nlohmann::json snap = SceneSerializer::SaveToJson(s4);
            SceneSerializer::LoadFromJson(s4, snap);
            check(s4.GetOverrides(rootFid)
                      && *s4.GetOverrides(rootFid) == std::set<std::string>{ kPosKey },
                  "override: the list survives the Play/Stop round trip");
        }

        // (5) Revert はリストからも消す / 抽出したベースには自レベルの overrides を残さない
        {
            const FieldDesc* pf = nullptr;
            for (const FieldDesc& f : ComponentRegistry::Get().Desc(LocalTransform::sTypeId).fields) {
                if (std::string(f.name) == "position") {
                    pf = &f;
                }
            }
            Prefab::RevertField(s, lib, inst.Id(), "LocalTransform", *pf);
            check(s.GetOverrides(rootFid) && s.GetOverrides(rootFid)->empty(),
                  "override: RevertField drops the key from the list");
            inst.GetComponent<LocalTransform>()->position.x = 55.0f;
            Prefab::RecordOverrides(s, lib, inst.Id());
            bool leaked = false;
            for (const nlohmann::json& it : Prefab::ExtractLocal(s, inst.Id())) {
                leaked = leaked || it.contains("overrides");
            }
            check(!leaked, "override: extracting a new base strips own-level override lists");
        }

        std::error_code ec;
        std::filesystem::remove(ovrPath, ec);
    }

    // ---- Undo 復元でプレハブタグが消えること (M48c) ----
    // UndoStack は before/after のサブツリー全量スナップショットなので JSON がタグの正解。
    // 隠しコンポーネントを消せないままだと「Create Prefab → Undo」でタグだけ残り、
    // アセットに紐づいていないのに Inspector がプレハブバーを出す偽インスタンスになる
    {
        PrefabLibrary lib;
        const std::wstring p =
            (std::filesystem::temp_directory_path() / L"mye_selftest_undo.prefab.json").wstring();
        Scene s;
        GameObject undoRoot = s.CreateGameObjectTracked("UndoRoot");
        GameObject undoChild = s.CreateGameObjectTracked("UndoChild");
        undoChild.SetParent(undoRoot);
        s.GetWorld().ApplyStructuralChanges();
        const nlohmann::json before = SceneSerializer::SubtreeToJson(s, undoRoot.Id());

        check(Prefab::CreateAsset(s, lib, p, undoRoot.Id()) != 0, "undo: create prefab succeeded");
        s.GetWorld().ApplyStructuralChanges();
        check(s.GetWorld().GetComponent<PrefabInstanceComponent>(undoRoot.Id()) != nullptr,
              "undo: the subtree is tagged before undo");

        SceneSerializer::ApplyPartial(s, before, /*removeHiddenMissing=*/false);
        s.GetWorld().ApplyStructuralChanges();
        check(s.GetWorld().GetComponent<PrefabInstanceComponent>(undoRoot.Id()) != nullptr,
              "undo: the default ApplyPartial keeps hidden tags (load path stays bit-invariant)");

        SceneSerializer::ApplyPartial(s, before, /*removeHiddenMissing=*/true);
        s.GetWorld().ApplyStructuralChanges();
        check(s.GetWorld().GetComponent<PrefabInstanceComponent>(undoRoot.Id()) == nullptr
                  && s.GetWorld().GetComponent<PrefabLinkComponent>(undoChild.Id()) == nullptr,
              "undo: removeHiddenMissing strips the tags the snapshot does not have");

        std::error_code ec;
        std::filesystem::remove(p, ec);
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

    // ---- ミニシーン編集モード (M48k) ----
    // 「アセットを専用 Scene に展開して編集し、書き戻す」経路の往復不変条件を固定する。
    // ここが崩れると、配置済みインスタンスの PrefabLink.localId が別メンバを指す
    {
        Scene s;
        PrefabLibrary lib;
        GameObject root = s.CreateGameObjectTracked("Rig");
        GameObject arm = s.CreateGameObjectTracked("Arm");
        GameObject hand = s.CreateGameObjectTracked("Hand");
        arm.SetParent(root);
        hand.SetParent(arm);
        s.GetWorld().ApplyStructuralChanges();
        arm.GetComponent<LocalTransform>()->position = { 1.0f, 2.0f, 3.0f };

        const std::wstring path =
            (std::filesystem::temp_directory_path() / L"mye_selftest_edit.actor.json").wstring();
        const uint64_t hash = Prefab::CreateAsset(s, lib, path, root.Id());
        check(hash != 0, "actor-edit: create the asset to edit");
        const nlohmann::json base = lib.Get(hash)->entities; // 編集前のベース

        // ---- 展開 ----
        Scene mini;
        const nlohmann::json doc = Prefab::MakeEditDocument(*lib.Get(hash));
        check(SceneSerializer::LoadFromJson(mini, doc), "actor-edit: the asset expands into a scene");
        mini.GetWorld().ApplyStructuralChanges();
        uint64_t maxLocal = 0;
        for (const nlohmann::json& it : base) {
            maxLocal = std::max(maxLocal, it.value("fileId", 0ull));
        }
        check(doc.value("nextFileId", 0ull) == maxLocal + 1,
              "actor-edit: nextFileId starts past the highest localId (new entities cannot collide)");
        GameObject miniHand = mini.Find("Hand");
        check(mini.Find("Rig") && mini.Find("Arm") && miniHand,
              "actor-edit: every member is present in the mini scene");

        // ---- 無変更で保存 → ベースがビット単位で同じ (往復不変) ----
        check(Prefab::SaveEdited(mini, lib, path, "Rig", /*actorFormat=*/true),
              "actor-edit: saving an untouched mini scene succeeds");
        check(lib.Get(hash)->entities == base,
              "actor-edit: an untouched round trip reproduces the base exactly");

        // ---- 編集して保存 → 値が入り、localId は据え置き ----
        miniHand.GetComponent<LocalTransform>()->position = { 9.0f, 0.0f, 0.0f };
        const uint64_t handLocal = mini.EnsureFileId(miniHand.Id());
        GameObject added = mini.CreateGameObjectTracked("Extra");
        added.SetParent(mini.Find("Rig"));
        mini.GetWorld().ApplyStructuralChanges();
        const uint64_t addedLocal = mini.EnsureFileId(added.Id());
        check(addedLocal > maxLocal,
              "actor-edit: an entity added while editing gets a fresh localId");
        check(Prefab::SaveEdited(mini, lib, path, "Rig", true), "actor-edit: save the edit");
        {
            const nlohmann::json& saved = lib.Get(hash)->entities;
            bool handOk = false;
            bool extraOk = false;
            for (const nlohmann::json& it : saved) {
                if (it.value("fileId", 0ull) == handLocal) {
                    handOk = it["components"]["LocalTransform"]["position"][0].get<float>() == 9.0f;
                }
                if (it.value("fileId", 0ull) == addedLocal) {
                    extraOk = it.value("name", std::string()) == "Extra";
                }
            }
            check(handOk, "actor-edit: the edit lands on the member that kept its localId");
            check(extraOk, "actor-edit: the added entity is written with its new localId");
            check(saved.size() == base.size() + 1, "actor-edit: exactly one member was added");
        }

        // ---- 空保存の拒否 (配置済みインスタンスのベースを消させない) ----
        Scene empty;
        check(!Prefab::SaveEdited(empty, lib, path, "Rig", true),
              "actor-edit: saving an empty mini scene is refused");
        check(lib.Get(hash)->entities.size() == base.size() + 1,
              "actor-edit: the refused save left the asset untouched");
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    if (failCount == 0) {
        MYE_LOG_INFO("==== Scene serializer self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== Scene serializer self test: %d FAILURE(S) ====", failCount);
    return false;
}

} // namespace mye
