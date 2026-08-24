#include "Editor/PartSelfTest.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <DirectXMath.h>

#include "Editor/PartTagNames.h"
#include "Engine/Core/Components.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/EntityNaming.h"
#include "Engine/Engine/FbxLoader.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/ModelLoader.h"
#include "Engine/Engine/PartFollowSystem.h"
#include "Engine/Engine/Parts.h"
#include "Engine/Engine/Prefab.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Engine/SceneSerializer.h"
#include "Engine/Engine/Script/EngineApiTable.h" // v9 部位スロットの実配線 (M48h)
#include "Engine/Engine/TransformSystem.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/ShaderManager.h"
#include "Engine/Renderer/Skeleton.h"
#include "Shared/ScriptAPI.h" // MyePartTag / MyeFindPartByTag / MyeAttachToPart (糖衣)

using namespace DirectX;

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

    // ---- 骨の供給元の解決 (M48i で Parts:: へ 1 本化) ----
    check(Parts::ResolvePartSource(w, head.Id(), kNullEntity).IsNull(),
          "source: a part with no skinned mesh ancestor resolves to null");
    check(Parts::ResolvePartSource(w, head.Id(), hips.Id()).IsNull(),
          "source: an explicit source without a SkinnedMesh is rejected (not trusted blindly)");

    // ---- String64 は「終端より後ろ」もハッシュ対象 (Inspector の ZeroStringTail の理由) ----
    // WorldHasher は登録フィールドを FieldTypeSize 分まるごと読むが、JSON は終端までしか
    // 書かない。残骸があると「保存内容は同じなのに WorldHash だけ違う」= M8 の
    // NameComponent と同じ静かな壊れ方になる
    {
        auto* hp = w.GetComponent<PartComponent>(head.Id());
        const size_t n = std::strlen(hp->joint);
        const uint64_t clean = HashWorld(w, nullptr);
        hp->joint[n + 1] = 'X'; // 終端の 1 つ後ろに残骸を置く (文字列としては同じ)
        check(std::strlen(hp->joint) == n && HashWorld(w, nullptr) != clean,
              "string64: bytes past the terminator are hashed (editors must zero the tail)");
        std::memset(hp->joint + n + 1, 0, sizeof(hp->joint) - n - 1);
        check(HashWorld(w, nullptr) == clean, "string64: zeroing the tail restores the hash");
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

    // ---- C ABI v9 (M48h): スクリプトから見た部位クエリ ----
    // エンジン内の Parts:: 関数が通ることは上で確認済みなので、ここで見るのは
    // 「GameLogic.dll / C# が実際に呼ぶ経路」— テーブルのスロットが埋まっているか、
    // 戻り値の規約 (切り捨て前の総数) が守られているか、Shared 側で再掲した
    // FNV 定数がエンジンの HashStr と一致しているか
    {
        ScriptApiContext apiCtx;
        apiCtx.scene = &scene;
        MyeEngineApi api = {};
        BuildEngineApi(api, &apiCtx);

        const auto toShared = [](EntityID e) { return MyeEntityId{ e.index, e.generation }; };
        const auto same = [](MyeEntityId a, EntityID b) {
            return a.index == b.index && a.generation == b.generation;
        };
        const MyeEntityId root = toShared(enemy.Id());

        check(api.version == MYE_API_VERSION && MYE_API_VERSION == 14u,
              "abi: the table reports v14");
        check(api.FindPart != nullptr && api.FindPartsByTag != nullptr,
              "abi: the v9 part slots are filled in");
        check(api.RaycastParts != nullptr, "abi: the v10 RaycastParts slot is filled in");
        // v13 (M52i): ネット状態 + レーン指定アクション。**充填漏れ**は規則 11 が静的に
        // 見るが、ここでは「テーブルを組み立てた実物に入っているか」を実行時に見る
        check(api.NetIsConnected != nullptr && api.NetLocalPlayer != nullptr
                  && api.NetPlayerCount != nullptr && api.NetPingMs != nullptr
                  && api.NetRollbackCount != nullptr,
              "abi: the v13 net slots are filled in");
        check(api.GetActionForPlayer != nullptr && api.GetAxisForPlayer != nullptr,
              "abi: the v13 per-lane action slots are filled in");
        // セッションを張っていない実行での既定値 (スクリプトが分岐に使う値そのもの)
        check(api.NetIsConnected(api.engine) == 0 && api.NetLocalPlayer(api.engine) == 0
                  && api.NetPlayerCount(api.engine) == 1
                  && api.NetRollbackCount(api.engine) == 0ull,
              "abi: without a session the net slots report 'local, one lane'");
        check(api.GetComponentField != nullptr && api.SetComponentField != nullptr,
              "abi: the v11 generic field slots are filled in");
        check(api.GetMouseWheel != nullptr && api.SetUIRect != nullptr
                  && api.SetUILayout != nullptr && api.SetUITexture != nullptr
                  && api.UIHitTest != nullptr && api.GetActionState != nullptr
                  && api.GetAxisValue != nullptr && api.SetTimeControl != nullptr
                  && api.GetTimeControl != nullptr && api.PersistSet != nullptr
                  && api.PersistGet != nullptr && api.SaveGame != nullptr
                  && api.LoadGame != nullptr && api.SetPadVibration != nullptr,
              "abi: the 14 v12 slots are filled in");
        // v14 (M59k): 物理側の挙動は PhysicsSelfTest が見る。ここで見るのは
        // 「組み立てた実物のテーブルに 8 本とも入っているか」
        check(api.RemoveComponentByName != nullptr && api.HasComponentByName != nullptr
                  && api.AddForceAtPosition != nullptr && api.GetContactInfo != nullptr
                  && api.SampleWind != nullptr && api.SampleTerrainHeight != nullptr
                  && api.WakeRigidbody != nullptr && api.IsSleeping != nullptr,
              "abi: the 8 v14 slots are filled in");
        // 付け外しは名前解決 → World への委譲だけ。イテレーション外なので即時適用される
        check(api.HasComponentByName(&apiCtx, toShared(legL.Id()), "Part") == 1
                  && api.HasComponentByName(&apiCtx, root, "Part") == 0
                  && api.HasComponentByName(&apiCtx, root, "Rigidbody") == 0,
              "abi: HasComponentByName reports what the entity actually carries");
        check(api.HasComponentByName(&apiCtx, root, "NoSuchComponent") == 0
                  && api.HasComponentByName(&apiCtx, root, nullptr) == 0
                  && api.HasComponentByName(&apiCtx, MyeEntityId{}, "Part") == 0,
              "abi: an unknown name, a null name and a dead entity all report 'no'");
        check(api.AddComponentByName(&apiCtx, root, "Rigidbody") == 1
                  && api.HasComponentByName(&apiCtx, root, "Rigidbody") == 1
                  && api.RemoveComponentByName(&apiCtx, root, "Rigidbody") == 1
                  && api.HasComponentByName(&apiCtx, root, "Rigidbody") == 0,
              "abi: AddComponentByName / RemoveComponentByName round-trip outside iteration");
        check(api.RemoveComponentByName(&apiCtx, root, "Rigidbody") == 0,
              "abi: removing something the entity does not carry reports 0");
        // ★スクリプト層は**アーキタイプのイテレーション中**に走るので、Add も Remove も
        //   tick 末送りになる (ADR-005)。「付けた直後の Has が 0」という一番踏みやすい罠を
        //   ここで固定しておく (M59k の C# プローブで実測して分かった挙動)
        {
            const ComponentTypeId req[] = { LocalTransform::sTypeId };
            bool added = false, visibleNow = true, done = false;
            w.ForEachArchetype(req, [&](Archetype&) {
                if (done) { return; } // アーキタイプは複数あるので 1 回だけ
                done = true;
                added = api.AddComponentByName(&apiCtx, root, "Rigidbody") == 1;
                visibleNow = api.HasComponentByName(&apiCtx, root, "Rigidbody") == 1;
            });
            check(added && !visibleNow,
                  "abi: an add issued during iteration is queued - Has still shows tick-start state");
            w.ApplyStructuralChanges();
            check(api.HasComponentByName(&apiCtx, root, "Rigidbody") == 1,
                  "abi: ... and becomes visible once the structural changes are applied");
            bool removed = false, goneNow = false;
            done = false;
            w.ForEachArchetype(req, [&](Archetype&) {
                if (done) { return; }
                done = true;
                removed = api.RemoveComponentByName(&apiCtx, root, "Rigidbody") == 1;
                goneNow = api.HasComponentByName(&apiCtx, root, "Rigidbody") == 0;
            });
            check(removed && !goneNow, "abi: a remove issued during iteration is queued the same way");
            w.ApplyStructuralChanges();
            check(api.HasComponentByName(&apiCtx, root, "Rigidbody") == 0,
                  "abi: ... and lands at the end of the tick");
        }
        // 基本 4 コンポーネントは構造的に外せない — ABI からも壊せないことを固定する
        check(api.RemoveComponentByName(&apiCtx, root, "LocalTransform") == 0
                  && api.RemoveComponentByName(&apiCtx, root, "Hierarchy") == 0
                  && api.HasComponentByName(&apiCtx, root, "LocalTransform") == 1,
              "abi: base components refuse to be removed and say so in the return value");

        // ★Shared/ScriptAPI.h は Engine/Core/Hash.h を include できず FNV 定数を再掲して
        //   いる。ここがズレると「同じタグ名なのに引けない」という静かな壊れ方をする
        check(MyePartTag("Foot") == Parts::TagOf("Foot")
                  && MyePartTag("HandR") == Parts::TagOf("HandR") && MyePartTag("") == 0ull
                  && MyePartTag(nullptr) == 0ull,
              "abi: MyePartTag (Shared) equals Parts::TagOf (engine)");

        check(same(api.FindPart(&apiCtx, root, "Hips/LegL"), legL.Id()),
              "abi: FindPart descends a name path through the C ABI");
        check(same(api.FindPart(&apiCtx, root, ""), enemy.Id())
                  && same(api.FindPart(&apiCtx, root, nullptr), enemy.Id()),
              "abi: an empty or null path resolves to the root itself (no crash)");
        check(MyeEntityIdIsNull(api.FindPart(&apiCtx, root, "Hips/Nope")),
              "abi: a missing part comes back as a null id");

        MyeEntityId buf[4] = {};
        check(api.FindPartsByTag(&apiCtx, root, MyePartTag("Foot"), buf, 4) == 2
                  && same(buf[0], legL.Id()) && same(buf[1], legR.Id()),
              "abi: FindPartsByTag writes the DFS-ordered hits");
        check(MyeEntityIdIsNull(buf[2]) && MyeEntityIdIsNull(buf[3]),
              "abi: nothing is written past the hit count");
        MyeEntityId one = {};
        check(api.FindPartsByTag(&apiCtx, root, MyePartTag("Foot"), &one, 1) == 2
                  && same(one, legL.Id()),
              "abi: the return value is the hit count BEFORE truncation (Overlap* rule)");
        check(api.FindPartsByTag(&apiCtx, root, MyePartTag("Foot"), nullptr, 0) == 2,
              "abi: a null buffer just counts");
        check(api.FindPartsByTag(&apiCtx, root, MyePartTag("Nope"), buf, 4) == 0
                  && api.FindPartsByTag(&apiCtx, MyeEntityId{}, MyePartTag("Foot"), buf, 4) == 0,
              "abi: an unused tag and a dead root yield nothing");

        // ---- 糖衣: 取り付けは既存 SetParent のまま (ABI は増やしていない) ----
        MyeUpdateContext uctx = {};
        uctx.api = &api;
        uctx.self = root;
        GameObject charm = scene.CreateGameObjectTracked("Charm");
        charm.SetParent(enemy);
        w.ApplyStructuralChanges();
        check(MyeAttachToPart(uctx, toShared(charm.Id()), root, "Hips/LegL"),
              "abi: MyeAttachToPart reports success for an existing part");
        w.ApplyStructuralChanges(); // SetParent はコマンドキュー経由なので確定させる
        check(w.GetParent(charm.Id()) == legL.Id(),
              "abi: MyeAttachToPart reparents the entity onto the part");
        check(!MyeAttachToPart(uctx, toShared(charm.Id()), root, "Hips/Nope"),
              "abi: attaching to a missing part fails instead of silently parenting elsewhere");
        w.ApplyStructuralChanges();
        check(w.GetParent(charm.Id()) == legL.Id(), "abi: a failed attach leaves the hierarchy alone");
        check(same(MyeFindPartByTag(uctx, root, "Head"), head.Id())
                  && MyeEntityIdIsNull(MyeFindPartByTag(uctx, root, "Nope")),
              "abi: MyeFindPartByTag returns the first hit (null id when there is none)");
        check(same(MyeFindPart(uctx, "Hips/LegR"), legR.Id()),
              "abi: the self-relative MyeFindPart overload searches ctx.self");

        // ---- RaycastParts (v10、M49): C ABI 経路 + 糖衣のタグ名解決 ----
        // legL に範囲を付けて WorldMatrix を確定させ、テーブル経由で当てる
        legL.AddComponent<PartBoundsComponent>(); // 既定 ±0.5 の箱、原点
        w.ApplyStructuralChanges();
        TransformSystem abiTs;
        abiTs.Update(w);
        MyeRaycastHit rh = {};
        check(api.RaycastParts(&apiCtx, MyeEntityId{}, 0, { 0, 0, -5 }, { 0, 0, 1 }, 100.0f, &rh)
                      == 1
                  && same(rh.entity, legL.Id()) && std::fabs(rh.distance - 4.5f) < 1e-5f,
              "abi: RaycastParts hits the bounded part through the C ABI");
        check(api.RaycastParts(&apiCtx, MyeEntityId{}, MyePartTag("Head"), { 0, 0, -5 },
                               { 0, 0, 1 }, 100.0f, &rh)
                  == 0,
              "abi: a tag filter with no bounded match yields no hit");
        check(MyeRaycastParts(uctx, MyeEntityId{}, "Foot", { 0, 0, -5 }, { 0, 0, 1 }, 100.0f, rh)
                  && same(rh.entity, legL.Id()),
              "abi: the MyeRaycastParts sugar resolves the tag name");

        w.DestroyEntity(charm.Id());
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

    // ---- ボーン追従 (M48g): 実アセット CesiumMan.glb をヘッドレスにロードして回す ----
    {
        RenderResources resources;
        ShaderManager shaders;
        Scene fs;
        const std::wstring modelPath =
            std::filesystem::absolute(L"assets\\models\\CesiumMan.glb").wstring();
        const std::wstring fbxPath =
            std::filesystem::absolute(L"assets\\models\\skinned_beam.fbx").wstring();

        // (0) ヘッドレス登録: エンティティを 1 個も作らずにスケルトンだけ登録できること。
        // ★**Load とは別のライブラリ**に登録して照合する — 同じライブラリだと Load 側の
        //   登録が必ず当たってしまい、「キーがずれている」バグを検出できない。
        //   本番で起きるのはまさにこの形: シーン JSON が持つのは Load が作った AssetID で、
        //   起動時に走っているのはヘッドレス登録だけ
        RenderResources headless;
        const size_t registered = ModelLoader::RegisterSkinnedModels(headless, modelPath);
        const size_t registeredFbx = FbxLoader::RegisterSkinnedModels(headless, fbxPath);
        check(registered == 1 && registeredFbx == 1,
              "follow: RegisterSkinnedModels registers glTF and FBX skeletons headlessly");

        // (0b) M50a: 一括登録 (起動走査の実体) はメッシュ / マテリアルまで揃えること。
        // スケルトンだけだと保存済みシーン経由のロードでモデルが描画されない
        // (M48i 申し送りの穴)。照合はスキン同様、Load とは別ライブラリに対して行う
        RenderResources headlessFull;
        check(ModelLoader::RegisterAssets(headlessFull, shaders, modelPath, false)
                  && FbxLoader::RegisterAssets(headlessFull, shaders, fbxPath, false),
              "assets: RegisterAssets registers glTF and FBX headlessly");

        GameObject root = ModelLoader::Load(fs, resources, shaders, modelPath);
        World& fw = fs.GetWorld();
        fw.ApplyStructuralChanges();
        EntityID skinned = kNullEntity;
        {
            const ComponentTypeId req[] = { SkinnedMeshComponent::sTypeId };
            fw.ForEachArchetype(req, [&](Archetype& arch) {
                for (uint32_t row = 0; row < arch.Count(); ++row) {
                    if (skinned.IsNull()) {
                        skinned = arch.EntityAt(row);
                    }
                }
            });
        }
        check(root && !skinned.IsNull(), "follow: CesiumMan.glb loaded with a skinned mesh");

        // ★ヘッドレス登録が Load と**同じキー**を作っていること。ずれていたら
        //   保存済みシーンからのロードでポーズが取れなくなる (この穴が M48g の修理対象)
        auto* sm = skinned.IsNull() ? nullptr : fw.GetComponent<SkinnedMeshComponent>(skinned);
        check(sm && headless.skinnedModels.Get(sm->model) != nullptr,
              "follow: the AssetID a saved scene holds resolves against a headless-only library");
        // M50a: 同じエンティティの MeshRenderer が持つ mesh / material も一括登録側で
        // 解決できること (= 保存済みシーンからモデルが「描画」される条件そのもの)
        {
            auto* mrr = skinned.IsNull() ? nullptr : fw.GetComponent<MeshRendererComponent>(skinned);
            check(mrr && headlessFull.meshes.Get(mrr->mesh) != nullptr
                      && headlessFull.materials.Get(mrr->material) != nullptr
                      && sm && headlessFull.skinnedModels.Get(sm->model) != nullptr,
                  "assets: mesh/material/skin AssetIDs a saved scene holds resolve headlessly");
        }
        {
            // FBX 側も同じ照合をする (キーが element_id 由来でノード走査と組み方が違うため)
            Scene fbxScene;
            RenderResources fbxRes;
            FbxLoader::Load(fbxScene, fbxRes, shaders, fbxPath);
            fbxScene.GetWorld().ApplyStructuralChanges();
            AssetID fbxModel{};
            const ComponentTypeId req[] = { SkinnedMeshComponent::sTypeId };
            fbxScene.GetWorld().ForEachArchetype(req, [&](Archetype& arch) {
                const int si = arch.FindTypeIndex(SkinnedMeshComponent::sTypeId);
                for (uint32_t row = 0; row < arch.Count(); ++row) {
                    if (fbxModel.IsNull()) {
                        fbxModel = static_cast<const SkinnedMeshComponent*>(arch.GetPtr(si, row))->model;
                    }
                }
            });
            check(!fbxModel.IsNull() && headless.skinnedModels.Get(fbxModel) != nullptr,
                  "follow: the FBX headless key matches FbxLoader::Load as well");
            // M50a: FBX 側も mesh / material キーの一致を照合する (組み方がノード走査で
            // glTF と違うため、キーずれの検知は両ローダで独立に必要)
            AssetID fbxMesh{};
            AssetID fbxMat{};
            const ComponentTypeId mreq[] = { MeshRendererComponent::sTypeId };
            fbxScene.GetWorld().ForEachArchetype(mreq, [&](Archetype& arch) {
                const int mi = arch.FindTypeIndex(MeshRendererComponent::sTypeId);
                for (uint32_t row = 0; row < arch.Count(); ++row) {
                    if (fbxMesh.IsNull()) {
                        const auto* mr =
                            static_cast<const MeshRendererComponent*>(arch.GetPtr(mi, row));
                        fbxMesh = mr->mesh;
                        fbxMat = mr->material;
                    }
                }
            });
            check(!fbxMesh.IsNull() && headlessFull.meshes.Get(fbxMesh) != nullptr
                      && headlessFull.materials.Get(fbxMat) != nullptr,
                  "assets: FBX mesh/material AssetIDs resolve against the bulk registration");
        }

        if (sm) {
            GameObject socket = fs.CreateGameObjectTracked("Socket");
            socket.SetParent(GameObject(&fw, skinned));
            auto* pc = socket.AddComponent<PartComponent>();
            pc->tag = Parts::TagOf("HandR");
            std::snprintf(pc->joint, sizeof(pc->joint), "%s", "Skeleton_arm_joint_R__3_");
            fw.ApplyStructuralChanges();

            // 供給元の解決 (M48i): Inspector のジョイント一覧と PartFollowSystem が
            // **同じ関数**を見ていること。ここがズレると「エディタで選べたのに動かない」
            check(Parts::ResolvePartSource(fw, socket.Id(), kNullEntity) == skinned,
                  "source: falls back to the nearest skinned mesh ancestor");
            check(Parts::ResolvePartSource(fw, socket.Id(), skinned) == skinned,
                  "source: an explicit valid source is honored");
            check(fw.GetComponent<SkinnedMeshComponent>(root.Id()) == nullptr
                      && Parts::ResolvePartSource(fw, socket.Id(), root.Id()) == skinned,
                  "source: an explicit source without a SkinnedMesh falls back to the ancestor");

            PartFollowSystem follow;
            const auto poseAt = [&](int ticks) {
                fw.GetComponent<SkinnedMeshComponent>(skinned)->timeTicks = ticks;
                follow.Update(fw, resources);
                return *fw.GetComponent<LocalTransform>(socket.Id());
            };
            const LocalTransform kIdentityLt{}; // memcmp 用の実体 (一時オブジェクトのアドレスは取れない)
            const LocalTransform t0 = poseAt(0);
            const LocalTransform t30 = poseAt(30);
            const LocalTransform t0b = poseAt(0);

            check(std::memcmp(&t0, &kIdentityLt, sizeof(LocalTransform)) != 0,
                  "follow: the part is moved onto the joint (no longer the identity transform)");
            check(std::memcmp(&t0, &t30, sizeof(LocalTransform)) != 0,
                  "follow: advancing timeTicks moves the part");
            check(std::memcmp(&t0, &t0b, sizeof(LocalTransform)) == 0,
                  "follow: the same tick reproduces the transform bit-for-bit");

            // 部位のワールドが M48a の式 (jointGlobal * sourceWorld) と一致すること。
            // **直子規約が成り立っている**ことの直接検査でもある
            {
                TransformSystem ts;
                ts.Update(fw);
                const XMMATRIX expect =
                    XMMatrixMultiply(ComputeJointGlobal(*resources.skinnedModels.Get(sm->model),
                                                        sm->clip, 0.0f,
                                                        resources.skinnedModels.Get(sm->model)
                                                            ->FindJointByName("Skeleton_arm_joint_R__3_")),
                                     XMLoadFloat4x4(&fw.GetComponent<WorldMatrixComponent>(skinned)->value));
                const XMFLOAT4X4& got = fw.GetComponent<WorldMatrixComponent>(socket.Id())->value;
                XMFLOAT4X4 exp4;
                XMStoreFloat4x4(&exp4, expect);
                float maxDiff = 0.0f;
                for (int i = 0; i < 16; ++i) {
                    maxDiff = std::max(maxDiff, std::fabs((&got._11)[i] - (&exp4._11)[i]));
                }
                check(maxDiff < 1e-4f,
                      "follow: the part world equals jointGlobal * sourceWorld (M48a formula)");
            }

            // 直子でない部位は追従しない (v1 規約のガード)
            GameObject deep = fs.CreateGameObjectTracked("DeepSocket");
            deep.SetParent(socket);
            auto* dp = deep.AddComponent<PartComponent>();
            dp->tag = Parts::TagOf("HandR");
            std::snprintf(dp->joint, sizeof(dp->joint), "%s", "Skeleton_arm_joint_R__3_");
            fw.ApplyStructuralChanges();
            follow.Update(fw, resources);
            check(std::memcmp(fw.GetComponent<LocalTransform>(deep.Id()), &kIdentityLt,
                              sizeof(LocalTransform)) == 0,
                  "follow: a part that is not a direct child of its source is skipped (v1 rule)");

            // joint 空 = 静的ソケット (触らない)
            GameObject staticSock = fs.CreateGameObjectTracked("StaticSocket");
            staticSock.SetParent(GameObject(&fw, skinned));
            staticSock.SetLocalPosition(1.0f, 2.0f, 3.0f);
            staticSock.AddComponent<PartComponent>()->tag = Parts::TagOf("Fixed");
            fw.ApplyStructuralChanges();
            follow.Update(fw, resources);
            check(fw.GetComponent<LocalTransform>(staticSock.Id())->position.y == 2.0f,
                  "follow: an empty joint name means a static socket (left alone)");
        }
    }

    // ---- 部位の範囲 (M49): ポーズ規約 ----
    {
        PartBoundsComponent b;
        b.shape = 0; // 箱
        b.center = { 1.0f, 2.0f, 3.0f };
        b.halfExtents = { 0.5f, 0.6f, 0.7f };
        XMFLOAT4X4 wm;
        XMStoreFloat4x4(&wm, XMMatrixScaling(2.0f, 3.0f, 4.0f));
        ShapePose p = Parts::MakePartBoundsPose(b, wm);
        // ★shape 番号は PartBounds と ShapePose で逆 — ここが入れ替わると
        //   「箱を置いたのに球で当たる」が静かに起きるので機械検査で固定する
        check(p.shape == 1, "bounds: PartBounds box(0) maps to ShapePose box(1)");
        check(p.identityRot == 1, "bounds: a scale-only matrix keeps the identity fast-path");
        check(std::fabs(p.hx - 1.0f) < 1e-5f && std::fabs(p.hy - 1.8f) < 1e-5f
                  && std::fabs(p.hz - 2.8f) < 1e-5f,
              "bounds: box extents scale per-axis");
        check(std::fabs(p.px - 2.0f) < 1e-5f && std::fabs(p.py - 6.0f) < 1e-5f
                  && std::fabs(p.pz - 12.0f) < 1e-5f,
              "bounds: the center offset is scaled into the world position");

        b.shape = 1; // 球
        p = Parts::MakePartBoundsPose(b, wm);
        check(p.shape == 0, "bounds: PartBounds sphere(1) maps to ShapePose sphere(0)");
        check(std::fabs(p.radius - 0.5f * 4.0f) < 1e-5f,
              "bounds: the sphere radius is halfExtents.x times the max scale axis");

        // 回転を含む center 変換は DirectXMath の答えと一致すること (フル変換の検査)
        XMStoreFloat4x4(&wm, XMMatrixRotationY(XM_PIDIV2) * XMMatrixTranslation(5, 0, 0));
        b.shape = 0;
        p = Parts::MakePartBoundsPose(b, wm);
        XMFLOAT3 expect;
        XMStoreFloat3(&expect, XMVector3Transform(XMLoadFloat3(&b.center), XMLoadFloat4x4(&wm)));
        check(p.identityRot == 0 && std::fabs(p.px - expect.x) < 1e-5f
                  && std::fabs(p.py - expect.y) < 1e-5f && std::fabs(p.pz - expect.z) < 1e-5f,
              "bounds: a rotated matrix transforms the center like XMVector3Transform");
    }

    // ---- 部位の範囲 (M49): RaycastParts の決定論規約とフィルタ ----
    {
        Scene rs;
        World& rw = rs.GetWorld();
        GameObject p1 = rs.CreateGameObjectTracked("P1");
        GameObject p2 = rs.CreateGameObjectTracked("P2");
        // 手前から: F (Part 無し) / E (無効) / Z (ゼロ寸法) はどれも当たってはいけない
        GameObject f = rs.CreateGameObjectTracked("NoPart");
        GameObject e = rs.CreateGameObjectTracked("Inactive");
        GameObject z = rs.CreateGameObjectTracked("ZeroSize");
        GameObject a = rs.CreateGameObjectTracked("BoxA");
        GameObject sb = rs.CreateGameObjectTracked("SphereB");
        GameObject d1 = rs.CreateGameObjectTracked("Twin1");
        GameObject d2 = rs.CreateGameObjectTracked("Twin2");
        rw.ApplyStructuralChanges();
        a.SetParent(p1);
        sb.SetParent(p2);

        const uint64_t kWeak = Parts::TagOf("Weak");
        const uint64_t kCore = Parts::TagOf("Core");
        const uint64_t kTwin = Parts::TagOf("Twin");
        auto setup = [&](GameObject& g, float x, int32_t shape, float he, uint64_t tag) {
            g.SetLocalPosition(x, 0.0f, 0.0f);
            g.AddComponent<PartComponent>()->tag = tag;
            auto* pb = g.AddComponent<PartBoundsComponent>();
            pb->shape = shape;
            pb->halfExtents = { he, he, he };
        };
        f.SetLocalPosition(0.8f, 0.0f, 0.0f);
        f.AddComponent<PartBoundsComponent>()->halfExtents = { 0.4f, 0.4f, 0.4f };
        setup(e, 1.0f, 0, 0.25f, kWeak);
        e.AddComponent<ActiveComponent>()->enabled = 0;
        setup(z, 1.2f, 0, 0.0f, kWeak);
        setup(a, 2.0f, 0, 0.5f, kWeak);
        setup(sb, 5.0f, 1, 0.5f, kCore);
        setup(d1, 8.0f, 0, 0.5f, kTwin);
        setup(d2, 8.0f, 0, 0.5f, kTwin);
        rw.ApplyStructuralChanges();
        TransformSystem ts;
        ts.Update(rw);

        const XMFLOAT3 o = { 0, 0, 0 };
        const XMFLOAT3 dx = { 1, 0, 0 };
        Parts::PartRayHit hit;
        check(Parts::RaycastParts(rw, kNullEntity, 0, o, dx, 100.0f, hit)
                  && hit.entity == a.Id() && std::fabs(hit.distance - 1.5f) < 1e-5f
                  && std::fabs(hit.point.x - 1.5f) < 1e-5f && hit.normal.x == -1.0f,
              "raycast: nearest part wins; part-less / inactive / zero-size candidates are skipped");
        check(Parts::RaycastParts(rw, kNullEntity, kCore, o, dx, 100.0f, hit)
                  && hit.entity == sb.Id() && std::fabs(hit.distance - 4.5f) < 1e-5f
                  && std::fabs(hit.normal.x + 1.0f) < 1e-5f,
              "raycast: the tag filter reaches the sphere behind the box");
        check(Parts::RaycastParts(rw, p2.Id(), 0, o, dx, 100.0f, hit) && hit.entity == sb.Id(),
              "raycast: a root limits hits to its subtree");
        check(!Parts::RaycastParts(rw, kNullEntity, 0, o, dx, 1.0f, hit),
              "raycast: maxDist cuts everything off");
        // 同 t (完全に重なった双子) は**低 index が勝つ** (昇順走査 + 厳密 < の規約)。
        // ここが <= に変わる/走査が降順になると落ちる = 決定論規約の変異検出
        check(Parts::RaycastParts(rw, kNullEntity, kTwin, o, dx, 100.0f, hit)
                  && hit.entity == d1.Id(),
              "raycast: an exact tie is won by the lower entity index");
        // 箱の内部始点は t=0 で当たる (RayAabb は tmin=0 開始 — M49 で実測確定した挙動)
        const XMFLOAT3 inside = { 8.0f, 0.0f, 0.0f };
        check(Parts::RaycastParts(rw, kNullEntity, kTwin, inside, dx, 100.0f, hit)
                  && hit.distance == 0.0f && hit.entity == d1.Id(),
              "raycast: a ray starting inside a box hits at t=0 (and ties go to the lower index)");
        GameObject dead = rs.CreateGameObjectTracked("Dead");
        rw.ApplyStructuralChanges();
        const EntityID deadId = dead.Id();
        rw.DestroyEntity(deadId);
        rw.ApplyStructuralChanges();
        check(!Parts::RaycastParts(rw, deadId, 0, o, dx, 100.0f, hit),
              "raycast: a dead root returns no hit (no crash)");
    }

    // ---- 部位の範囲 (M49): シリアライズ往復 + ハッシュ被覆 ----
    {
        Scene hs;
        GameObject g = hs.CreateGameObjectTracked("Bounded");
        hs.GetWorld().ApplyStructuralChanges();
        g.AddComponent<PartComponent>()->tag = Parts::TagOf("Weak");
        auto* pb = g.AddComponent<PartBoundsComponent>();
        pb->shape = 1;
        pb->center = { 0.1f, 0.2f, 0.3f };
        pb->halfExtents = { 0.4f, 0.5f, 0.6f };
        hs.GetWorld().ApplyStructuralChanges();

        const nlohmann::json saved = SceneSerializer::SaveToJson(hs);
        const uint64_t h0 = HashWorld(hs.GetWorld(), nullptr);
        Scene hs2;
        SceneSerializer::LoadFromJson(hs2, saved);
        check(HashWorld(hs2.GetWorld(), nullptr) == h0,
              "bounds: the serialize round trip is hash-identical");
        auto* pb2 = hs2.GetWorld().GetComponent<PartBoundsComponent>(hs2.Find("Bounded").Id());
        check(pb2 && pb2->shape == 1 && std::fabs(pb2->center.y - 0.2f) < 1e-6f
                  && std::fabs(pb2->halfExtents.z - 0.6f) < 1e-6f,
              "bounds: PartBounds fields survive a save/load round trip");
        // hash 対象であること (kComponentNoHash の付け忘れ/付けすぎ検出)
        pb2->halfExtents.y = 9.0f;
        check(HashWorld(hs2.GetWorld(), nullptr) != h0,
              "bounds: halfExtents is covered by the world hash");
    }

    if (failCount == 0) {
        MYE_LOG_INFO("==== Part self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== Part self test: %d FAILURE(S) ====", failCount);
    return false;
}

} // namespace mye
