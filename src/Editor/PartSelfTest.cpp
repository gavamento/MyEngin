#include "Editor/PartSelfTest.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "Editor/PartTagNames.h"
#include "Engine/Core/Components.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/EntityNaming.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/Parts.h"
#include "Engine/Engine/Prefab.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Engine/SceneSerializer.h"

#include "nlohmann/json.hpp"

namespace mye {

bool RunPartSelfTest()
{
    MYE_LOG_INFO("==== Part (socket) self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    // ---- テスト用の階層 ----
    //   Enemy
    //     Hips
    //       LegL  (Part tag=Foot)
    //       LegR  (Part tag=Foot)
    //     Head    (Part tag=Head)
    Scene scene;
    GameObject enemy = scene.CreateGameObjectTracked("Enemy");
    GameObject hips = scene.CreateGameObjectTracked("Hips");
    GameObject legL = scene.CreateGameObjectTracked("LegL");
    GameObject legR = scene.CreateGameObjectTracked("LegR");
    GameObject head = scene.CreateGameObjectTracked("Head");
    hips.SetParent(enemy);
    legL.SetParent(hips);
    legR.SetParent(hips);
    head.SetParent(enemy);
    World& w = scene.GetWorld();
    w.ApplyStructuralChanges();

    const uint64_t kFoot = Parts::TagOf("Foot");
    const uint64_t kHead = Parts::TagOf("Head");
    legL.AddComponent<PartComponent>()->tag = kFoot;
    legR.AddComponent<PartComponent>()->tag = kFoot;
    {
        auto* p = head.AddComponent<PartComponent>();
        p->tag = kHead;
        std::snprintf(p->joint, sizeof(p->joint), "%s", "head_bone");
    }
    w.ApplyStructuralChanges();

    // ---- タグ ID の定義 ----
    check(kFoot == HashStr("Foot") && kFoot != 0 && Parts::TagOf("") == 0,
          "tag: TagOf is FNV-1a of the name (empty name = 0)");
    check(kFoot != kHead, "tag: different names hash to different ids");

    // ---- FindPart (名前パス降下) ----
    check(Parts::FindPart(w, enemy.Id(), "Hips/LegL") == legL.Id(),
          "FindPart: descends a multi-segment name path");
    check(Parts::FindPart(w, enemy.Id(), "Head") == head.Id(),
          "FindPart: finds a direct child");
    check(Parts::FindPart(w, enemy.Id(), "") == enemy.Id(),
          "FindPart: an empty path is the root itself");
    check(Parts::FindPart(w, enemy.Id(), "/Hips//LegR/") == legR.Id(),
          "FindPart: leading, trailing and repeated separators are skipped");
    check(Parts::FindPart(w, enemy.Id(), "Hips/Nope").IsNull()
              && Parts::FindPart(w, enemy.Id(), "LegL").IsNull(),
          "FindPart: a missing segment (or a non-direct child) returns null");
    check(Parts::FindPart(w, hips.Id(), "LegL") == legL.Id(),
          "FindPart: the path is relative to the given root");

    // ---- FindPartsByTag (サブツリー DFS) ----
    {
        std::vector<EntityID> feet;
        Parts::FindPartsByTag(w, enemy.Id(), kFoot, feet);
        check(feet.size() == 2 && feet[0] == legL.Id() && feet[1] == legR.Id(),
              "FindPartsByTag: collects every match in DFS order");
        std::vector<EntityID> heads;
        Parts::FindPartsByTag(w, enemy.Id(), kHead, heads);
        check(heads.size() == 1 && heads[0] == head.Id(), "FindPartsByTag: filters by tag");
        std::vector<EntityID> none;
        Parts::FindPartsByTag(w, enemy.Id(), Parts::TagOf("Nope"), none);
        check(none.empty(), "FindPartsByTag: an unused tag yields nothing");
        std::vector<EntityID> sub;
        Parts::FindPartsByTag(w, hips.Id(), kFoot, sub);
        check(sub.size() == 2, "FindPartsByTag: the search is scoped to the given root");
    }

    // ---- 兄弟名の一意化との整合 (M48b) ----
    // FindPart は「最初に一致した直子」を返すので、同名の兄弟がいるとどちらが返るか
    // 使う側から見て決まらない。エディタ経由の生成/改名は必ず一意化を通ることで
    // その曖昧さを作らない、という前提が成り立っているかをここで固定する
    {
        const std::string uniq =
            MakeUniqueSiblingName(w, enemy.Id(), "Head", /*exclude=*/kNullEntity);
        check(uniq != "Head", "unique-name: a colliding sibling name gets suffixed (FindPart stays 1:1)");
        GameObject head2 = scene.CreateGameObjectTracked(uniq);
        head2.SetParent(enemy);
        w.ApplyStructuralChanges();
        check(Parts::FindPart(w, enemy.Id(), "Head") == head.Id()
                  && Parts::FindPart(w, enemy.Id(), uniq) == head2.Id(),
              "unique-name: both siblings stay addressable by their own path");
        w.DestroyEntity(head2.Id());
        w.ApplyStructuralChanges();
    }

    // ---- シリアライズ往復 + ハッシュ被覆 ----
    {
        const nlohmann::json saved = SceneSerializer::SaveToJson(scene);
        const uint64_t hashBefore = HashWorld(w, nullptr);
        Scene s2;
        SceneSerializer::LoadFromJson(s2, saved);
        World& w2 = s2.GetWorld();
        const EntityID head2 = Parts::FindPart(w2, s2.Find("Enemy").Id(), "Head");
        auto* p2 = head2.IsNull() ? nullptr : w2.GetComponent<PartComponent>(head2);
        check(p2 && p2->tag == kHead && std::string(p2->joint) == "head_bone",
              "serialize: Part survives a save/load round trip");
        check(HashWorld(w2, nullptr) == hashBefore, "serialize: the round trip is hash-identical");

        // tag は hash 対象 (M48g の PartFollowSystem が sim 入力にする) — 変えたら hash も動く
        w2.GetComponent<PartComponent>(head2)->tag = Parts::TagOf("Other");
        check(HashWorld(w2, nullptr) != hashBefore, "serialize: Part.tag is covered by the world hash");
    }

    // ---- 構造ロック: 「プレハブメンバ かつ 部位」だけ ----
    {
        check(!Parts::IsStructureLocked(w, head.Id()),
              "lock: a part that is not a prefab member is editable");
        PrefabLibrary lib;
        const std::wstring path =
            (std::filesystem::temp_directory_path() / L"mye_selftest_part.actor.json").wstring();
        check(Prefab::CreateAsset(scene, lib, path, enemy.Id()) != 0, "lock: create actor asset");
        w.ApplyStructuralChanges();
        check(Parts::IsStructureLocked(w, head.Id()) && Parts::IsStructureLocked(w, legL.Id()),
              "lock: parts of a prefab instance are structure-locked");
        check(!Parts::IsStructureLocked(w, hips.Id()),
              "lock: a prefab member without a Part is NOT locked (only parts are the asset's API)");
        check(!Parts::IsStructureLocked(w, kNullEntity),
              "lock: a dead entity is not locked (no crash)");

        // ---- 入れ子境界を越えるタグ検索 (中核判断 5: フラット走査) ----
        // 「ボス配下の全弱点」を 1 回で引けることを優先し、内側インスタンスの部位も拾う。
        // ここが境界で止まる実装に変わったら、この check が落ちて気づける
        {
            Scene outer;
            GameObject boss = outer.CreateGameObjectTracked("Boss");
            outer.GetWorld().ApplyStructuralChanges();
            const uint64_t innerRoot =
                Prefab::Instantiate(outer, lib, PrefabLibrary::HashForPath(path),
                                    outer.EnsureFileId(boss.Id()));
            outer.GetWorld().ApplyStructuralChanges();
            std::vector<EntityID> feet;
            Parts::FindPartsByTag(outer.GetWorld(), boss.Id(), kFoot, feet);
            check(innerRoot != 0 && feet.size() == 2,
                  "FindPartsByTag: does NOT stop at a nested prefab boundary (flat by design)");
        }
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    // ---- タグ名テーブル (エディタ表示専用) の往復 ----
    {
        const std::filesystem::path dir =
            std::filesystem::temp_directory_path() / L"mye_selftest_parttags";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const std::filesystem::path settings = dir / L"project_settings.json";
        std::filesystem::remove(settings, ec);

        PartTagNames& pt = PartTagNames::Get();
        pt.Load(dir.wstring(), /*force=*/true);
        check(pt.Count() > 0 && pt.Id(0) == Parts::TagOf(pt.Name(0)),
              "tags: a project without the key gets the seed list, ids are name hashes");

        // 既存キーを壊さない read-modify-write であること
        {
            std::ofstream f(settings);
            f << R"({"physicsLayers":["Default","Enemy"]})";
        }
        pt.Load(dir.wstring(), true);
        pt.SetCount(2);
        std::snprintf(pt.EditBuffer(0), PartTagNames::kNameCapacity, "%s", "Muzzle");
        std::snprintf(pt.EditBuffer(1), PartTagNames::kNameCapacity, "%s", ""); // 空欄は落ちる
        check(pt.Save(dir.wstring()), "tags: save succeeds");
        pt.Load(dir.wstring(), true);
        check(pt.Count() == 1 && std::string(pt.Name(0)) == "Muzzle",
              "tags: empty rows are dropped on save");
        check(pt.NameOf(Parts::TagOf("Muzzle")) != nullptr
                  && pt.NameOf(Parts::TagOf("Nope")) == nullptr && pt.NameOf(0) == nullptr,
              "tags: reverse lookup resolves registered ids only");
        {
            nlohmann::json j;
            std::ifstream f(settings);
            f >> j;
            check(j.contains("physicsLayers") && j["physicsLayers"].size() == 2,
                  "tags: saving part tags does not clobber other project settings keys");
        }
        std::filesystem::remove_all(dir, ec);
    }

    if (failCount == 0) {
        MYE_LOG_INFO("==== Part self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== Part self test: %d FAILURE(S) ====", failCount);
    return false;
}

} // namespace mye
