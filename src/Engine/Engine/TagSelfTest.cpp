//====================================================================================
//                          TagSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                       09/17/2026
//                                          汎用タグ / シェーダキャッシュの回帰テスト
//====================================================================================
#include "Engine/Engine/TagSelfTest.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "Engine/Core/Components.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/Prefab.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Engine/UI/UILayout.h"
#include "Engine/Engine/SceneSerializer.h"
#include "Engine/Engine/Script/EngineApiTable.h"
#include "Engine/Engine/TagNames.h"
#include "Engine/Engine/Tags.h"
#include "Engine/Renderer/MeshInstancing.h"
#include "Engine/Renderer/ShaderCache.h"
#include "Shared/ScriptAPI.h"

#include "nlohmann/json.hpp"

namespace mye {

bool RunTagSelfTest()
{
    MYE_LOG_INFO("==== Tag self test ====");
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

    // ---- ビットとフィルタ ----
    check(Tags::BitOf(0) == 1ull && Tags::BitOf(63) == (1ull << 63), "BitOf: 0 and 63");
    check(Tags::BitOf(-1) == 0ull && Tags::BitOf(64) == 0ull, "BitOf: out of range is 0");
    check(Tags::PassesFilter(0ull, 0ull), "filter 0 = no restriction (even untagged passes)");
    check(!Tags::PassesFilter(0ull, 0x4ull), "filter: untagged fails a non-zero filter");
    check(Tags::PassesFilter(0x6ull, 0x4ull), "filter: any shared bit passes");
    check(!Tags::PassesFilter(0x3ull, 0x4ull), "filter: no shared bit fails");

    // ---- 階層での継承と検索 ----
    //   Root   (tag 2)
    //     Child  (tag 5)
    //   Other  (tag 5)
    //   Plain  (なし)
    Scene scene;
    GameObject root = scene.CreateGameObjectTracked("Root");
    GameObject child = scene.CreateGameObjectTracked("Child");
    GameObject other = scene.CreateGameObjectTracked("Other");
    GameObject plain = scene.CreateGameObjectTracked("Plain");
    child.SetParent(root);
    World& w = scene.GetWorld();
    w.ApplyStructuralChanges();
    root.AddComponent<TagComponent>()->mask = Tags::BitOf(2);
    child.AddComponent<TagComponent>()->mask = Tags::BitOf(5);
    other.AddComponent<TagComponent>()->mask = Tags::BitOf(5);
    w.ApplyStructuralChanges();

    check(Tags::OwnMask(w, child.Id()) == Tags::BitOf(5), "OwnMask: only the entity's own tags");
    check(Tags::EffectiveMask(w, child.Id()) == (Tags::BitOf(5) | Tags::BitOf(2)),
          "EffectiveMask: inherits the parent's tags");
    check(Tags::EffectiveMask(w, plain.Id()) == 0ull, "EffectiveMask: untagged is 0");

    {
        std::vector<EntityID> hits;
        Tags::FindEntitiesWithTag(w, 5, hits);
        const bool sorted = hits.size() == 2 && hits[0].index < hits[1].index;
        const bool members = hits.size() == 2
            && ((hits[0] == child.Id() && hits[1] == other.Id())
                || (hits[0] == other.Id() && hits[1] == child.Id()));
        check(sorted && members, "FindEntitiesWithTag: own tag only, index ascending");
        Tags::FindEntitiesWithTag(w, 2, hits);
        check(hits.size() == 1 && hits[0] == root.Id(),
              "FindEntitiesWithTag: parent tag is not inherited by the search");
        Tags::FindEntitiesWithTag(w, 64, hits);
        check(hits.empty(), "FindEntitiesWithTag: out-of-range index finds nothing");
    }

    // ---- ハッシュ被覆 + シリアライズ往復 ----
    {
        const uint64_t before = HashWorld(w);
        other.GetComponent<TagComponent>()->mask |= Tags::BitOf(9);
        check(HashWorld(w) != before, "hash: a tag change changes the world hash");
        other.GetComponent<TagComponent>()->mask &= ~Tags::BitOf(9);
        check(HashWorld(w) == before, "hash: restoring the tag restores the hash");

        const nlohmann::json saved = SceneSerializer::SaveToJson(scene);
        Scene s2;
        SceneSerializer::LoadFromJson(s2, saved);
        World& w2 = s2.GetWorld();
        const GameObject child2 = s2.Find("Child");
        const auto* t2 = child2 ? w2.GetComponent<TagComponent>(child2.Id()) : nullptr;
        check(t2 != nullptr && t2->mask == Tags::BitOf(5), "serialize: Tag survives a round trip");
        check(HashWorld(w2) == before, "serialize: the round trip is hash-identical");

        // M76k: 保存形式は**エンティティ直下キー "tagMask"**。TagComponent は NoSerialize なので
        // components には出ない (名前 / fileId と同じ「オブジェクト固有の属性」の枠)
        nlohmann::json childItem = nlohmann::json::object();
        nlohmann::json plainItem = nlohmann::json::object();
        for (const nlohmann::json& it : saved["entities"]) {
            if (it.value("name", std::string()) == "Child") {
                childItem = it;
            } else if (it.value("name", std::string()) == "Plain") {
                plainItem = it;
            }
        }
        check(childItem.value("tagMask", 0ull) == Tags::BitOf(5),
              "serialize: tags are written as the entity-level 'tagMask' key");
        check(childItem.contains("components") && !childItem["components"].contains("Tag"),
              "serialize: no 'Tag' entry inside components");
        check(!plainItem.contains("tagMask"),
              "serialize: an untagged entity has no 'tagMask' key at all");

        // ★同値性 (この設計の核心): 「タグを付けて外した」跡がシーンにもハッシュにも残らない。
        //   mask=0 のコンポーネントを残す設計だと、ここの 2 つが両方とも不一致になる
        const std::string beforeDump = SceneSerializer::SaveToJson(scene).dump();
        const uint64_t beforeHash = HashWorld(w);
        Tags::SetOwnMask(w, plain.Id(), Tags::BitOf(11));
        w.ApplyStructuralChanges();
        check(HashWorld(w) != beforeHash, "no-trace: tagging an entity changes the world hash");
        Tags::SetOwnMask(w, plain.Id(), 0ull);
        w.ApplyStructuralChanges();
        check(HashWorld(w) == beforeHash, "no-trace: clearing every tag restores the hash");
        check(SceneSerializer::SaveToJson(scene).dump() == beforeDump,
              "no-trace: clearing every tag restores the scene json byte for byte");

        // 旧形式 (fa37257 の components.Tag.mask) も読む — 開いて保存すれば新形式へ移る
        nlohmann::json legacy = saved;
        for (nlohmann::json& it : legacy["entities"]) {
            if (it.value("name", std::string()) == "Child") {
                it.erase("tagMask");
                it["components"]["Tag"]["mask"] = Tags::BitOf(5);
            }
        }
        Scene s3;
        SceneSerializer::LoadFromJson(s3, legacy);
        const GameObject child3 = s3.Find("Child");
        check(child3 && Tags::OwnMask(s3.GetWorld(), child3.Id()) == Tags::BitOf(5),
              "serialize: the legacy components.Tag.mask form is still read");
    }

    // ---- プレハブ: タグは直下キー "tagMask" として名前と同じ枠で上書き追跡される (M76k) ----
    {
        PrefabLibrary lib;
        uint64_t baseHash = 0;
        {
            Scene sb;
            GameObject enemy = sb.CreateGameObjectTracked("Enemy");
            sb.GetWorld().ApplyStructuralChanges();
            Tags::SetOwnMask(sb.GetWorld(), enemy.Id(), Tags::BitOf(3));
            sb.GetWorld().ApplyStructuralChanges();
            baseHash = lib.Register(L"mye_selftest_tag.prefab.json", "tag_base",
                                    Prefab::ExtractLocal(sb, enemy.Id()));
        }
        Scene si;
        const uint64_t rootFid = Prefab::Instantiate(si, lib, baseHash, 0);
        si.GetWorld().ApplyStructuralChanges();
        World& wi = si.GetWorld();
        const GameObject inst = si.FindByFileId(rootFid);
        check(inst && Tags::OwnMask(wi, inst.Id()) == Tags::BitOf(3),
              "prefab: the base's tags reach the instance");
        check(inst && !Prefab::IsTagMaskOverridden(si, lib, inst.Id()),
              "prefab: an untouched instance reports no tag override");

        if (inst) {
            Tags::SetOwnMask(wi, inst.Id(), Tags::BitOf(3) | Tags::BitOf(8));
            wi.ApplyStructuralChanges();
            Prefab::RecordOverridesSubtree(si, lib, inst.Id()); // エディタ編集直後と同じフック
            const Scene::OverrideSet* rec = si.GetOverrides(rootFid);
            check(rec != nullptr && rec->count("tagMask") != 0,
                  "prefab: an instance tag edit is recorded under the 'tagMask' key");
            check(Prefab::IsTagMaskOverridden(si, lib, inst.Id()),
                  "prefab: the tag override is reported (Inspector's '*')");
            check(rec != nullptr && rec->count("Tag.mask") == 0 && rec->count("+Tag") == 0,
                  "prefab: no component-shaped keys ('Tag.mask' / '+Tag') are produced");

            Prefab::RevertTagMask(si, lib, inst.Id());
            wi.ApplyStructuralChanges();
            check(Tags::OwnMask(wi, inst.Id()) == Tags::BitOf(3),
                  "prefab: RevertTagMask restores the base tags");
            check(!Prefab::IsTagMaskOverridden(si, lib, inst.Id()),
                  "prefab: the override is gone after the revert");
        }
    }

    // ---- UI 専用判定: タグは実体コンポーネントではない (M76k) ----
    {
        Scene su;
        GameObject panel = su.CreateGameObjectTracked("Panel");
        su.GetWorld().ApplyStructuralChanges();
        panel.AddComponent<RectTransformComponent>();
        su.GetWorld().ApplyStructuralChanges();
        check(uilayout::IsUiOnlyEntity(su.GetWorld(), panel.Id()), "ui: a bare UI entity is UI-only");
        Tags::SetOwnMask(su.GetWorld(), panel.Id(), Tags::BitOf(1));
        su.GetWorld().ApplyStructuralChanges();
        check(uilayout::IsUiOnlyEntity(su.GetWorld(), panel.Id()),
              "ui: tagging a UI entity keeps it UI-only (screen UI must not fall to world-follow)");
        panel.AddComponent<MeshRendererComponent>();
        su.GetWorld().ApplyStructuralChanges();
        check(!uilayout::IsUiOnlyEntity(su.GetWorld(), panel.Id()),
              "ui: a real component still makes it a 3D object");
    }

    // ---- ABI v20 の実配線 ----
    {
        ScriptApiContext apiCtx;
        apiCtx.scene = &scene;
        MyeEngineApi api = {};
        BuildEngineApi(api, &apiCtx);
        const auto toShared = [](EntityID e) { return MyeEntityId{ e.index, e.generation }; };

        check(api.HasTag(&apiCtx, toShared(child.Id()), 5) == 1, "ABI HasTag: own tag");
        check(api.HasTag(&apiCtx, toShared(child.Id()), 2) == 0,
              "ABI HasTag: parent's tag is not reported (own tags only)");
        check(api.HasTag(&apiCtx, toShared(child.Id()), 99) == 0, "ABI HasTag: bad index is 0");

        check(api.SetTag(&apiCtx, toShared(plain.Id()), 7, 1) == 1, "ABI SetTag: adds the component");
        w.ApplyStructuralChanges();
        check(api.HasTag(&apiCtx, toShared(plain.Id()), 7) == 1, "ABI SetTag: tag visible afterwards");
        check(api.SetTag(&apiCtx, toShared(plain.Id()), 7, 0) == 1
                  && api.HasTag(&apiCtx, toShared(plain.Id()), 7) == 0,
              "ABI SetTag off: clears the bit");
        check(api.SetTag(&apiCtx, toShared(plain.Id()), -1, 1) == 0, "ABI SetTag: bad index is 0");

        MyeEntityId buf[1] = {};
        const int32_t total = api.FindEntitiesWithTag(&apiCtx, 5, buf, 1);
        check(total == 2, "ABI FindEntitiesWithTag: returns the untruncated total");
        check(api.FindEntitiesWithTag(&apiCtx, 5, nullptr, 0) == 2,
              "ABI FindEntitiesWithTag: null buffer only counts");
    }

    // ---- 名前表 / RT のタグ設定 / CLI 解析 ----
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path dir = fs::temp_directory_path(ec) / L"mye_tag_selftest";
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        {
            // 他キーを置いておき、保存で壊れないことを見る
            std::ofstream f(dir / L"project_settings.json");
            f << R"({"physicsLayers":["Default","Player"],"tags":["Enemy","","Pickup"]})";
        }
        TagNames names;
        names.Load(dir.wstring(), true);
        check(names.IndexOf("Enemy") == 0 && names.IndexOf("Pickup") == 2, "TagNames: load by index");
        check(names.IndexOf("") == -1 && names.IndexOf("enemy") == -1,
              "TagNames: empty and case-mismatched names are not found");
        check(std::string(names.Display(1)) == "Tag 1", "TagNames: blank slot displays as 'Tag N'");

        std::snprintf(names.EditBuffer(1), TagNames::kNameCapacity, "%s", "RtHero");
        check(names.DiffersFromDisk(), "TagNames: edit is detected as unsaved");
        check(names.Save(dir.wstring()), "TagNames: save");
        names.Load(dir.wstring(), true);
        check(names.IndexOf("RtHero") == 1 && !names.DiffersFromDisk(), "TagNames: save/load round trip");

        RtTagSettings rt;
        rt.receiverMask = Tags::BitOf(1) | Tags::BitOf(63);
        rt.sceneMask = Tags::BitOf(0);
        check(SaveRtTagSettings(dir.wstring(), rt), "RtTagSettings: save");
        const RtTagSettings back = LoadRtTagSettings(dir.wstring());
        check(back.receiverMask == rt.receiverMask && back.sceneMask == rt.sceneMask,
              "RtTagSettings: round trip");
        {
            std::ifstream f(dir / L"project_settings.json");
            nlohmann::json j;
            f >> j;
            check(j.contains("physicsLayers") && j["tags"].size() == 3,
                  "RtTagSettings: other keys are preserved");
        }
        check(LoadRtTagSettings((dir / L"missing").wstring()).receiverMask == 0,
              "RtTagSettings: missing file = no filter");
        fs::remove_all(dir, ec);

        uint64_t m = 123;
        check(ParseTagIndexList(L"0,3,63", m) && m == (1ull | 8ull | (1ull << 63)),
              "ParseTagIndexList: basic list");
        check(ParseTagIndexList(L"", m) && m == 0, "ParseTagIndexList: empty = no filter");
        m = 5;
        check(!ParseTagIndexList(L"64", m) && m == 5, "ParseTagIndexList: 64 rejected, out untouched");
        check(!ParseTagIndexList(L"1,,2", m) && !ParseTagIndexList(L"a", m),
              "ParseTagIndexList: malformed rejected");
    }

    // ---- インスタンス run は RT を受けるかの違いで切れる ----
    {
        std::vector<RenderItem> items(4);
        for (RenderItem& it : items) {
            it.mesh = AssetID{ 11 };
            it.material = AssetID{ 22 };
        }
        const std::vector<uint8_t> can(items.size(), 1);
        std::vector<MeshInstanceRun> runs;
        std::vector<DirectX::XMFLOAT4X4> worlds;
        BuildInstanceRuns(items, can, runs, worlds);
        check(runs.size() == 1 && runs[0].count == 4, "instancing: same receiver flag = one run");
        items[2].rtReceiver = 0.0f;
        items[3].rtReceiver = 0.0f;
        BuildInstanceRuns(items, can, runs, worlds);
        check(runs.size() == 2 && runs[0].count == 2 && runs[1].first == 2,
              "instancing: receiver flag change splits the run");
    }

    // ---- シェーダのバイトコードキャッシュの形式 ----
    {
        ShaderCacheEntry e;
        e.configKey = ShaderCacheConfigKey(true, 0x800u);
        e.isCompute = true;
        e.sourceHash = 0x1234ull;
        e.deps.push_back({ "rt_common.hlsli", "c:\\x\\rt_common.hlsli", 0x55ull });
        e.blobs.push_back({ 1, 2, 3, 4, 5 });
        std::vector<uint8_t> bytes = EncodeShaderCacheEntry(e);
        ShaderCacheEntry d;
        check(DecodeShaderCacheEntry(bytes, d) && d.configKey == e.configKey && d.isCompute
                  && d.sourceHash == e.sourceHash && d.deps.size() == 1
                  && d.deps[0].resolvedPath == e.deps[0].resolvedPath
                  && d.deps[0].contentHash == 0x55ull && d.blobs == e.blobs,
              "shader cache: encode/decode round trip");
        bytes[bytes.size() / 2] ^= 0xFF;
        check(!DecodeShaderCacheEntry(bytes, d), "shader cache: corruption is rejected (checksum)");
        bytes.resize(bytes.size() - 3);
        check(!DecodeShaderCacheEntry(bytes, d), "shader cache: truncation is rejected");

        ShaderCacheEntry vsps = e;
        vsps.isCompute = false; // VS+PS なのに 1 本 = 形式違反
        check(!DecodeShaderCacheEntry(EncodeShaderCacheEntry(vsps), d),
              "shader cache: blob count must match the program kind");

        check(ShaderCacheConfigKey(true, 0x800u) != ShaderCacheConfigKey(false, 0x800u)
                  && ShaderCacheConfigKey(true, 0x800u) != ShaderCacheConfigKey(true, 0x801u),
              "shader cache: config key depends on kind and flags");
        const std::wstring a = ShaderCacheFileName(L"c:\\p\\assets\\shaders\\rt_gi.cs.hlsl", true);
        const std::wstring b = ShaderCacheFileName(L"c:\\e\\assets\\shaders\\rt_gi.cs.hlsl", true);
        check(a != b && a == ShaderCacheFileName(L"c:\\p\\assets\\shaders\\rt_gi.cs.hlsl", true),
              "shader cache: file name is stable and differs between roots");
    }

    if (failCount == 0) {
        MYE_LOG_INFO("Tag self test: ALL PASS");
        return true;
    }
    MYE_LOG_ERROR("Tag self test: %d failure(s)", failCount);
    return false;
}

} // namespace mye
