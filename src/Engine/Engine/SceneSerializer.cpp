#include "Engine/Engine/SceneSerializer.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Engine/Core/Components.h"
#include "Engine/Core/HierarchyWalk.h"
#include "Engine/Engine/UI/UILayout.h" // M75a: 旧 UIElement の配置 → RectTransform
#include "Engine/Core/JsonUtil.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/EntityNaming.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Engine/Script/ManagedHost.h"
#include "Engine/Engine/Tags.h" // 汎用タグ: 直下キー "tagMask" の読み書き (Tags::OwnMask/SetOwnMask)
#include "Engine/Platform/PathUtil.h"

namespace mye::SceneSerializer {

using nlohmann::json;

// C# コンポーネントのフィールド JSON 化フック (EngineLoop が設定)。
ManagedHost* g_managedHost = nullptr;

void SetManagedHost(ManagedHost* mh) { g_managedHost = mh; }

namespace {

// EntityID → fileId (alive かつ FileIdComponent を持てばその値、なければ 0=null 参照)
uint64_t FidOf(World& world, EntityID e)
{
    if (!world.IsAlive(e)) {
        return 0;
    }
    auto* f = world.GetComponent<FileIdComponent>(e);
    return f ? f->value : 0;
}

void EnsureFileId(Scene& scene, World& world, EntityID e)
{
    if (!world.IsAlive(e)) {
        return;
    }
    if (auto* f = world.GetComponent<FileIdComponent>(e)) {
        if (f->value == 0) {
            f->value = scene.NextFileId();
        }
    } else {
        world.AddComponent<FileIdComponent>(e)->value = scene.NextFileId();
    }
}

// 兄弟内での位置 (0 始まり)。ルートは firstRoot_ を先頭とするリストで数える
uint32_t SiblingIndexOf(World& world, EntityID e)
{
    auto* h = world.GetComponent<HierarchyComponent>(e);
    if (!h) {
        return 0;
    }
    EntityID cur;
    if (h->parent.IsNull()) {
        cur = world.FirstRoot();
    } else {
        auto* ph = world.GetComponent<HierarchyComponent>(h->parent);
        cur = ph ? ph->firstChild : kNullEntity;
    }
    uint32_t i = 0;
    while (!cur.IsNull()) {
        if (cur == e) {
            return i;
        }
        auto* ch = world.GetComponent<HierarchyComponent>(cur);
        cur = ch ? ch->nextSibling : kNullEntity;
        ++i;
    }
    return i;
}

// 1 エンティティ → JSON オブジェクト (fileId/name/parent/childIndex/components[/overrides])。
// EntityRef フィールドと親は fileId で出力する (参照透過な復元のため)。
//
// **ここが overrides キーの唯一の出口** (M48e)。SaveToJson / EntityToJson / SubtreeToJson は
// すべてこの関数を通るので、シーン保存・Undo スナップショット・プレハブ抽出・複製が
// まとめて override リストを運ぶ
json WriteEntity(Scene& scene, EntityID e, uint32_t childIndex)
{
    World& world = scene.GetWorld();
    const ComponentRegistry& reg = ComponentRegistry::Get();
    json item;
    const uint64_t fileId = FidOf(world, e);
    item["fileId"] = fileId;
    item["name"] = world.GetName(e);
    const EntityID parent = world.GetParent(e);
    if (!parent.IsNull() && world.IsAlive(parent)) {
        item["parent"] = FidOf(world, parent);
    }
    item["childIndex"] = childIndex;
    // 汎用タグ (TypeId=62) は NoSerialize = 下の comps ループには出ない。**非ゼロのときだけ**
    // この直下キーに出すことで「mask=0 のコンポーネント」と「コンポーネント無し」がファイル上で
    // 同値になる (WorldHasher の内容ゲートと対)。名前と違い既定値では書かない
    if (const uint64_t tagMask = Tags::OwnMask(world, e); tagMask != 0) {
        item["tagMask"] = tagMask;
    }

    json comps = json::object();
    const Archetype* arch = world.GetArchetype(e);
    if (arch) {
        for (ComponentTypeId t : arch->Types()) { // TypeId 昇順 = 決定論
            const ComponentDesc& desc = reg.Desc(t);
            if (desc.flags & kComponentNoSerialize) {
                continue;
            }
            if (t == NameComponent::sTypeId) {
                continue; // "name" として出力済み
            }
            // C# スクリプトコンポーネント: フィールドは managed が保持 → hook で JSON 化
            if (g_managedHost && g_managedHost->IsManagedComponent(t)) {
                const std::string js = g_managedHost->SerializeComponent(t, e, world.GetComponentRaw(e, t));
                json parsed = js.empty() ? json::object() : json::parse(js, nullptr, false);
                comps[desc.name] = parsed.is_discarded() ? json::object() : std::move(parsed);
                continue;
            }
            json fields = json::object();
            const void* comp = world.GetComponentRaw(e, t);
            for (const FieldDesc& f : desc.fields) {
                if (f.flags & kFieldNoSerialize) {
                    continue;
                }
                if (f.type == FieldType::EntityRef) {
                    const EntityID ref = *reinterpret_cast<const EntityID*>(
                        static_cast<const uint8_t*>(comp) + f.offset);
                    fields[f.name] = FidOf(world, ref); // fileId (0=null)
                } else {
                    fields[f.name] = FieldToJson(comp, f);
                }
            }
            comps[desc.name] = std::move(fields);
        }
    }
    // ★M70a: 型を引けなかったコンポーネントを生 JSON のまま書き戻す。ここが無いと
    //   「読めなかった状態で Ctrl+S」がディスクの側からデータを消す (Scene.h の解説)。
    //   同名が登録済みになっていたら**アーキタイプ側が勝つ** — スキーマや DLL を直して
    //   開き直したあとに二重書きしないため。json のオブジェクトはキー昇順で出力されるので、
    //   ここでの挿入順は出力バイト列に影響しない
    if (const Scene::UnknownCompSet* unknown = scene.GetUnknownComponents(fileId)) {
        for (const auto& [compName, raw] : *unknown) {
            if (comps.contains(compName)) {
                continue;
            }
            json parsed = json::parse(raw, nullptr, false);
            comps[compName] = parsed.is_discarded() ? json::object() : std::move(parsed);
        }
    }
    item["components"] = std::move(comps);
    // 記録があるときだけキーを出す (空配列も出す = 「新形式・上書き無し」の明示)。
    // 記録が無いエンティティ = プレハブ非メンバ or レガシー → キーごと省略し、旧形式の
    // ファイルと 1 バイトも変わらないようにする
    if (const Scene::OverrideSet* ov = scene.GetOverrides(fileId)) {
        json arr = json::array();
        for (const std::string& key : *ov) { // std::set = ソート済み
            arr.push_back(key);
        }
        item["overrides"] = std::move(arr);
    }
    return item;
}

// エンティティ JSON の "overrides" キーを Scene の記録へ復元する (M48e)。
// **ReadEntityComponents を呼ぶ経路すべての直後で呼ぶこと** — LoadFromJson / ApplyDiff /
// ApplyPartial の 3 本。JSON が唯一の正解なので、キーが無ければ記録も消す
// (Undo 復元やプレハブ展開で「前の状態の override」が残らないように)
void ReadEntityOverrides(Scene& scene, uint64_t fileId, const json& item)
{
    if (fileId == 0) {
        return;
    }
    if (!item.contains("overrides") || !item["overrides"].is_array()) {
        scene.ClearOverrides(fileId);
        return;
    }
    Scene::OverrideSet keys;
    for (const json& v : item["overrides"]) {
        if (v.is_string()) {
            keys.insert(v.get<std::string>());
        }
    }
    scene.SetOverrides(fileId, std::move(keys));
}

// JSON の components を エンティティ e へ流し込む。EntityRef は toEntity で fileId→EntityID に解決。
// removeMissing=true のとき、JSON に無いシリアライズ対象コンポーネントを除去する。
// removeHiddenMissing=true なら kComponentHidden なものも除去対象に含める (M48c) —
// シリアライズされる隠しコンポーネントは PrefabInstance / PrefabLink だけなので、
// これは実質「プレハブタグを JSON に一致させるか」のスイッチ。既定 false は従来どおりの
// ビット不変ロード (シーンロード/ApplyDiff/プレハブ展開はタグを消してはならない)。
//
// ★M70a: 型を引けなかった分は捨てず Scene へ預ける (WriteEntity が書き戻す)。戻り値は
// 預けた件数 — 呼び出し元がまとめて 1 行警告を出せるようにするため。fileId は預かりの鍵で、
// 呼び出し元 3 本 (LoadFromJson / ApplyDiff / ApplyPartial) はいずれも 0 のものを弾いている
//
// ★M75a: 旧形式 (v3 以前) の UIElement.anchor/x/y/w/h/space は登録フィールドから消えたので
// FieldFromJson では拾えない。ここで生 JSON から読んで RectTransform を追加する
// (変換式は uilayout::FromLegacyRect の 1 本)。outMigratedRects に追加した実体を返し、
// LoadFromJson が親リンク確定後に basis を確定する (それまでは「UI 祖先あり」仮置き =
// キャンバス基準 = 旧挙動と同値なので、確定しない経路 (ApplyPartial 等) でも壊れない)
int ReadEntityComponents(Scene& scene, uint64_t fileId, EntityID e, const json& item,
                         const std::function<EntityID(uint64_t)>& toEntity, bool removeMissing,
                         bool removeHiddenMissing = false,
                         std::vector<EntityID>* outMigratedRects = nullptr)
{
    World& world = scene.GetWorld();
    const ComponentRegistry& reg = ComponentRegistry::Get();
    const json comps = item.contains("components") ? item["components"] : json::object();
    Scene::UnknownCompSet unknown;
    bool migratedRect = false;
    if (comps.contains("UIElement") && !comps.contains("RectTransform")) {
        const json& ui = comps["UIElement"];
        if (ui.is_object() && ui.contains("anchor")) {
            if (auto* rt = world.AddComponent<RectTransformComponent>(e)) {
                *rt = uilayout::FromLegacyRect(ui.value("anchor", 0), ui.value("x", 0.0f),
                                               ui.value("y", 0.0f), ui.value("w", 160.0f),
                                               ui.value("h", 40.0f), ui.value("space", 0),
                                               /*hasUiAncestor*/ true);
                migratedRect = true;
                if (outMigratedRects) {
                    outMigratedRects->push_back(e);
                }
            }
        }
    }
    for (const auto& [compName, fields] : comps.items()) {
        const ComponentTypeId t = reg.FindByName(compName);
        if (t == kInvalidComponentType) {
            // 生 JSON のまま預ける。ここを `continue` だけにしていたのが M70a の地雷で、
            // 「読めなかった」事実が呼び出し元にも保存側にも 1 バイトも伝わっていなかった
            unknown.emplace(compName, fields.dump());
            MYE_LOG_WARN("scene load: unknown component '%s' (kept verbatim)", compName.c_str());
            continue;
        }
        void* comp = world.AddComponentRaw(e, t); // 既存ならそのポインタ
        if (!comp) {
            continue;
        }
        // C# スクリプトコンポーネント: managed インスタンスにフィールドを復元
        if (g_managedHost && g_managedHost->IsManagedComponent(t)) {
            g_managedHost->DeserializeComponent(t, e, comp, fields.dump());
            continue;
        }
        const ComponentDesc& desc = reg.Desc(t);
        for (const FieldDesc& f : desc.fields) {
            if (f.flags & kFieldNoSerialize) {
                continue;
            }
            if (!fields.contains(f.name)) {
                continue;
            }
            if (f.type == FieldType::EntityRef) {
                EntityID target = kNullEntity;
                const json& v = fields[f.name];
                if (v.is_number_unsigned() || v.is_number_integer()) {
                    target = toEntity(v.get<uint64_t>());
                }
                // else: version 1 の [index, generation] 配列 → 読み捨て (null)
                *reinterpret_cast<EntityID*>(static_cast<uint8_t*>(comp) + f.offset) = target;
            } else {
                FieldFromJson(comp, f, fields[f.name]);
            }
        }
    }
    // JSON が唯一の正解 (ReadEntityOverrides と同じ意味論) — 前回の預かりは必ず置き換える。
    // マージにすると、ファイルから消したはずの未知コンポーネントが次の保存で蘇る
    const int unknownCount = static_cast<int>(unknown.size());
    scene.SetUnknownComponents(fileId, std::move(unknown));
    if (removeMissing) {
        if (const Archetype* arch = world.GetArchetype(e)) {
            std::vector<ComponentTypeId> types(arch->Types().begin(), arch->Types().end());
            for (ComponentTypeId t : types) {
                const ComponentDesc& desc = reg.Desc(t);
                if ((desc.flags & kComponentNoSerialize) != 0 || t == NameComponent::sTypeId
                    || t == LocalTransform::sTypeId) {
                    continue;
                }
                if ((desc.flags & kComponentHidden) != 0 && !removeHiddenMissing) {
                    continue;
                }
                if (migratedRect && t == RectTransformComponent::sTypeId) {
                    continue; // 旧形式から今作ったばかり (JSON には無い) — 消さない
                }
                if (!comps.contains(desc.name)) {
                    world.RemoveComponentRaw(e, t);
                }
            }
        }
    }
    return unknownCount;
}

// DFS 順 (ルート firstRoot → 子は firstChild/nextSibling) で全エンティティと兄弟 index を収集
void CollectSubtreeOrdered(World& world, EntityID root, uint32_t rootIdx, std::vector<EntityID>& out,
                           std::vector<uint32_t>& outIdx)
{
    ForEachInSubtree(
        world, root,
        [&](EntityID e, uint32_t idx) {
            out.push_back(e);
            outIdx.push_back(idx);
            return WalkStep::Continue;
        },
        rootIdx);
}

void CollectHierarchyOrdered(World& world, std::vector<EntityID>& out, std::vector<uint32_t>& outIdx)
{
    // ルートも子と同じく兄弟リスト (FirstRoot → nextSibling) で並んでいる
    EntityID r = world.FirstRoot();
    uint32_t ri = 0;
    while (!r.IsNull()) {
        auto* rh = world.GetComponent<HierarchyComponent>(r);
        const EntityID next = rh ? rh->nextSibling : kNullEntity;
        CollectSubtreeOrdered(world, r, ri++, out, outIdx);
        r = next;
    }
}

bool IsFileIdValue(const json& v)
{
    return v.is_number_unsigned() || (v.is_number_integer() && v.get<int64_t>() >= 0);
}

// エンティティ直下キー "tagMask" → TagComponent。**ReadEntityComponents を呼ぶ経路すべてで
// 呼ぶこと** — LoadFromJson / ApplyDiff / ApplyPartial の 3 本 (name と同じ扱い)。
// JSON が唯一の正解なので、キーが無ければタグも消す — Undo の復元 (ApplyPartial) がこれで成立する。
// ★ApplyPartial は ValidateDocument を通らないので、型の確認はここでも行う
// ★旧形式 (fa37257 の components.Tag.mask) も拾う。書き出しは新形式だけなので、開いて保存すれば
//   自然に移行する
void ApplyTagMask(World& world, EntityID e, const json& item)
{
    uint64_t mask = 0;
    if (item.contains("tagMask") && IsFileIdValue(item["tagMask"])) {
        mask = item["tagMask"].get<uint64_t>();
    } else if (item.contains("components") && item["components"].is_object()) {
        const json& comps = item["components"];
        if (comps.contains("Tag") && comps["Tag"].is_object()) {
            const json& tag = comps["Tag"];
            if (tag.contains("mask") && IsFileIdValue(tag["mask"])) {
                mask = tag["mask"].get<uint64_t>();
            }
        }
    }
    Tags::SetOwnMask(world, e, mask);
}

// シーン文書の事前検査。LoadFromJson / ApplyDiff は**シーンに触る前**にここを通す。
// ★Clear の後で json の型不一致 (value / get の例外) が出ると、不正なファイルを開こうとしただけで
//   編集中のシーンが消える。見るのは読み出し側が value / get で型を仮定しているキーだけ —
//   読み出しにキーを足したらここにも足すこと。フィールド値は FieldFromJson が自前で例外を捕まえる
bool ValidateDocument(const json& root, std::string& why)
{
    const auto fail = [&why](std::string message) {
        why = std::move(message);
        return false;
    };
    if (!root.is_object()) {
        return fail("the root is not an object");
    }
    if (!root.contains("entities") || !root["entities"].is_array()) {
        return fail("'entities' is not an array");
    }
    if (root.contains("sceneName") && !root["sceneName"].is_string()) {
        return fail("'sceneName' is not a string");
    }
    if (root.contains("nextFileId") && !IsFileIdValue(root["nextFileId"])) {
        return fail("'nextFileId' is not a non-negative integer");
    }
    if (root.contains("version") && !root["version"].is_number_integer()) {
        return fail("'version' is not an integer");
    }
    std::unordered_set<uint64_t> fileIds;
    size_t index = 0;
    for (const json& item : root["entities"]) {
        const std::string at = "entities[" + std::to_string(index++) + "]";
        if (!item.is_object()) {
            return fail(at + " is not an object");
        }
        if (item.contains("fileId")) {
            if (!IsFileIdValue(item["fileId"])) {
                return fail(at + ".fileId is not a non-negative integer");
            }
            // 重複すると fileId → EntityID の対応表が後勝ちになり、親と参照が別の実体を指す
            const uint64_t fid = item["fileId"].get<uint64_t>();
            if (fid != 0 && !fileIds.insert(fid).second) {
                return fail(at + ".fileId " + std::to_string(fid) + " is used twice");
            }
        }
        if (item.contains("name") && !item["name"].is_string()) {
            return fail(at + ".name is not a string");
        }
        if (item.contains("tagMask") && !IsFileIdValue(item["tagMask"])) {
            return fail(at + ".tagMask is not a non-negative integer");
        }
        if (item.contains("parent") && !IsFileIdValue(item["parent"])) {
            return fail(at + ".parent is not a non-negative integer");
        }
        if (item.contains("childIndex") && !item["childIndex"].is_number_integer()) {
            return fail(at + ".childIndex is not an integer");
        }
        if (!item.contains("components")) {
            continue;
        }
        const json& comps = item["components"];
        if (!comps.is_object()) {
            return fail(at + ".components is not an object");
        }
        for (const auto& [compName, fields] : comps.items()) {
            if (!fields.is_object()) {
                return fail(at + ".components." + compName + " is not an object");
            }
        }
        // M75a: 旧形式の UIElement 配置は ReadEntityComponents が value で直接読む
        if (comps.contains("UIElement") && comps["UIElement"].contains("anchor")) {
            const json& ui = comps["UIElement"];
            for (const char* key : { "anchor", "x", "y", "w", "h", "space" }) {
                if (ui.contains(key) && !ui[key].is_number()) {
                    return fail(at + ".components.UIElement." + key + " is not a number");
                }
            }
        }
    }
    return true;
}

} // namespace

json SaveToJson(Scene& scene)
{
    World& world = scene.GetWorld();

    // 保留中の構造変更 (SetParent / Destroy / SetSiblingIndex 等) を反映してから保存する
    world.ApplyStructuralChanges();

    // DFS (兄弟順) でエンティティを収集 — 兄弟順が保存され、ロードで復元される
    std::vector<EntityID> entities;
    std::vector<uint32_t> childIndices;
    CollectHierarchyOrdered(world, entities, childIndices);

    // fileId 未割り当てに採番 (アーキタイプ移動を伴うため一覧確定後に行う。EntityID は不変)
    for (EntityID e : entities) {
        EnsureFileId(scene, world, e);
    }

    // M70a: 生きているエンティティの分だけ預かりを残す (孤児の掃除)。採番の後に行うこと
    {
        std::vector<uint64_t> liveFids;
        liveFids.reserve(entities.size());
        for (EntityID e : entities) {
            liveFids.push_back(FidOf(world, e));
        }
        scene.RetainUnknownComponents(liveFids);
    }

    json items = json::array();
    for (size_t i = 0; i < entities.size(); ++i) {
        items.push_back(WriteEntity(scene, entities[i], childIndices[i]));
    }

    json root;
    root["engine"] = "MyEngine";
    // 書く値は Scene::kDocVersion (現行 4)。版ごとの意味は Scene.h の kDocVersion が正本
    root["version"] = Scene::kDocVersion;
    root["sceneName"] = scene.Name();
    root["nextFileId"] = scene.PeekNextFileId();
    root["entities"] = std::move(items);
    return root;
}

bool LoadFromJson(Scene& scene, const json& root)
{
    // ★Clear より前に検査する。不正な文書ならシーンに 1 バイトも触らずに false を返す
    std::string why;
    if (!ValidateDocument(root, why)) {
        MYE_LOG_ERROR("scene load: invalid json (%s)", why.c_str());
        return false;
    }
    World& world = scene.GetWorld();

    scene.Clear();
    scene.SetName(root.value("sceneName", std::string("Untitled")));
    scene.SetNextFileId(root.value("nextFileId", 1ull));
    // 文書 version を保持する (M50c)。RefreshNonOverridden が v3 の構造追随
    // (欠落 comp のロード時追加) を掛けてよいかをこれで判定する。キー無し = v1
    scene.SetLoadedVersion(root.value("version", 1));

    const json& items = root["entities"];

    // 1) 全エンティティを生成して fileId → EntityID 対応表を作る (生成順 = ファイル順 = DFS 兄弟順)
    std::unordered_map<uint64_t, EntityID> byFileId;
    for (const json& item : items) {
        const uint64_t fileId = item.value("fileId", 0ull);
        GameObject obj = scene.CreateGameObject(item.value("name", std::string("entity")));
        obj.AddComponent<FileIdComponent>()->value = fileId;
        if (fileId != 0) {
            byFileId[fileId] = obj.Id();
        }
    }
    auto toEntity = [&](uint64_t fid) -> EntityID {
        if (fid == 0) {
            return kNullEntity;
        }
        auto it = byFileId.find(fid);
        return (it != byFileId.end()) ? it->second : kNullEntity;
    };

    // 2) コンポーネントとフィールド (EntityRef は fileId で解決)
    int unknownTotal = 0;
    std::vector<EntityID> migratedRects; // M75a: 旧 UIElement 配置から作った RectTransform
    for (const json& item : items) {
        const uint64_t fileId = item.value("fileId", 0ull);
        const EntityID e = toEntity(fileId);
        if (e.IsNull()) {
            continue;
        }
        ApplyTagMask(world, e, item); // 直下キー "tagMask" (name と同じ扱い)
        unknownTotal += ReadEntityComponents(scene, fileId, e, item, toEntity,
                                             /*removeMissing*/ false, false, &migratedRects);
        ReadEntityOverrides(scene, fileId, item); // M48e
    }
    if (unknownTotal > 0) {
        // 型が引けない = スキーマ未登録 / GameLogic.dll のロード失敗 / C# ホストの初期化失敗。
        // どの経路でも起動は続くので、「保存しても消えない」ことをここで明言しておく (M70a)
        MYE_LOG_WARN("scene load: %d unknown component(s) kept verbatim (saving preserves them)",
                     unknownTotal);
    }

    // 3) 親子関係 (ファイル順に SetParent → 兄弟順は DFS 順で復元される)
    for (const json& item : items) {
        if (!item.contains("parent")) {
            continue;
        }
        const EntityID child = toEntity(item.value("fileId", 0ull));
        const EntityID parent = toEntity(item.value("parent", 0ull));
        if (!child.IsNull() && !parent.IsNull()) {
            world.SetParent(child, parent);
        }
    }
    world.ApplyStructuralChanges();
    // 4) M75a: 旧形式から変換した RectTransform の basis を確定する (親リンク確定後にしか
    //    「UI 祖先がいるか」が分からない)。変換時は basis=1 (キャンバス) の仮置き = 旧挙動と
    //    同値。UI 祖先がいなければ basis=0 (親基準 = 無いのでキャンバス) へ戻す — 結果の
    //    矩形は同じで、Unity 風の既定 (親基準) に揃うだけ
    if (!migratedRects.empty()) {
        for (const EntityID e : migratedRects) {
            auto* rt = world.GetComponent<RectTransformComponent>(e);
            if (rt && rt->basis == 1 && !uilayout::HasUiAncestor(world, e)) {
                rt->basis = 0;
            }
        }
        MYE_LOG_INFO("scene load: %d UI element(s) migrated to RectTransform (doc v%d)",
                     static_cast<int>(migratedRects.size()), scene.LoadedVersion());
    }
    return true;
}

bool ApplyDiff(Scene& scene, const json& root)
{
    // 外部エディタで壊れた文書は触る前に弾く (途中で例外になると差分が半分だけ当たる)
    std::string why;
    if (!ValidateDocument(root, why)) {
        MYE_LOG_ERROR("[reload] scene diff rejected: %s", why.c_str());
        return false;
    }
    World& world = scene.GetWorld();

    // 既存: fileId → EntityID
    std::unordered_map<uint64_t, EntityID> existing;
    {
        const ComponentTypeId req[] = { FileIdComponent::sTypeId };
        world.ForEachArchetype(req, [&](Archetype& arch) {
            const int fi = arch.FindTypeIndex(FileIdComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                const uint64_t fid = static_cast<const FileIdComponent*>(arch.GetPtr(fi, row))->value;
                if (fid != 0) {
                    existing[fid] = arch.EntityAt(row);
                }
            }
        });
    }

    // ★以降は**ファイルの配列順** (= DFS 兄弟順) で回す。unordered_map の反復順で生成すると、
    //   新規エンティティの生成順 (ルートの兄弟順 / EntityID の払い出し) が通常ロードと食い違う。
    //   fileId の重複は ValidateDocument が弾いている
    std::vector<const json*> ordered;
    std::unordered_set<uint64_t> incoming;
    for (const json& item : root["entities"]) {
        const uint64_t fid = item.value("fileId", 0ull);
        if (fid != 0) {
            ordered.push_back(&item);
            incoming.insert(fid);
        }
    }

    int created = 0, updated = 0, destroyed = 0;

    // 1) 新規生成
    for (const json* item : ordered) {
        const uint64_t fid = item->value("fileId", 0ull);
        if (!existing.contains(fid)) {
            GameObject obj = scene.CreateGameObject(item->value("name", std::string("entity")));
            obj.AddComponent<FileIdComponent>()->value = fid;
            existing[fid] = obj.Id();
            ++created;
        }
    }

    auto toEntity = [&](uint64_t fid) -> EntityID {
        if (fid == 0) {
            return kNullEntity;
        }
        auto it = existing.find(fid);
        return (it != existing.end()) ? it->second : kNullEntity;
    };

    // 2) 更新 (名前 / コンポーネント追加・更新・除去。EntityRef は fileId で解決)
    for (const json* itemPtr : ordered) {
        const json& item = *itemPtr;
        const uint64_t fid = item.value("fileId", 0ull);
        const EntityID e = existing[fid];
        if (!world.IsAlive(e)) {
            continue;
        }
        SetEntityName(world, e, item.value("name", std::string()));
        ApplyTagMask(world, e, item); // 直下キー "tagMask" (name と同じ扱い)
        ReadEntityComponents(scene, fid, e, item, toEntity, /*removeMissing*/ true);
        ReadEntityOverrides(scene, fid, item); // M48e
        ++updated;
    }

    // 3) ファイルから消えたものは破棄。★fileId 昇順 — 破棄の順番が空きスロットの再利用順になる
    std::vector<uint64_t> gone;
    for (const auto& [fid, e] : existing) {
        if (!incoming.contains(fid)) {
            gone.push_back(fid);
        }
    }
    std::sort(gone.begin(), gone.end());
    for (uint64_t fid : gone) {
        world.DestroyEntity(existing[fid]);
        ++destroyed;
    }

    // 4) 親子関係 + 兄弟位置。★親が変わらない子にも childIndex を当てる — 同じ親の中で並べ替えただけの
    //    変更はここでしか反映されない。配列順 = 兄弟の中では childIndex 昇順なので、
    //    前から順に差し込めば並びが揃う (ApplyPartial と同じ)
    for (const json* itemPtr : ordered) {
        const EntityID child = existing[itemPtr->value("fileId", 0ull)];
        const EntityID parent = toEntity(itemPtr->value("parent", 0ull));
        if (world.GetParent(child) != parent) {
            world.SetParent(child, parent);
        }
        world.SetSiblingIndex(child, itemPtr->value("childIndex", 0xFFFFFFFFu));
    }

    world.ApplyStructuralChanges();
    if (root.contains("nextFileId")) {
        const uint64_t next = root.value("nextFileId", 1ull);
        if (next > scene.PeekNextFileId()) {
            scene.SetNextFileId(next);
        }
    }
    MYE_LOG_INFO("[reload] scene diff applied: %d updated, %d created, %d destroyed", updated,
                 created, destroyed);
    return true;
}

json EntityToJson(Scene& scene, EntityID e)
{
    World& world = scene.GetWorld();
    world.ApplyStructuralChanges();
    if (!world.IsAlive(e)) {
        return json::object();
    }
    EnsureFileId(scene, world, e);
    return WriteEntity(scene, e, SiblingIndexOf(world, e));
}

json SubtreeToJson(Scene& scene, EntityID root)
{
    World& world = scene.GetWorld();
    world.ApplyStructuralChanges();
    json arr = json::array();
    if (!world.IsAlive(root)) {
        return arr;
    }
    std::vector<EntityID> ents;
    std::vector<uint32_t> idxs;
    CollectSubtreeOrdered(world, root, SiblingIndexOf(world, root), ents, idxs);
    // 参照 (親 / EntityRef) 解決のため、書き出し前に全 fileId を確定する
    for (EntityID e : ents) {
        EnsureFileId(scene, world, e);
    }
    for (size_t i = 0; i < ents.size(); ++i) {
        arr.push_back(WriteEntity(scene, ents[i], idxs[i]));
    }
    return arr;
}

bool ApplyPartial(Scene& scene, const json& entities, bool removeHiddenMissing)
{
    if (!entities.is_array()) {
        return false;
    }
    World& world = scene.GetWorld();

    // fileId で照合し find-or-create
    std::unordered_map<uint64_t, EntityID> incoming;
    for (const json& item : entities) {
        const uint64_t fid = item.value("fileId", 0ull);
        if (fid == 0) {
            continue;
        }
        GameObject g = scene.FindByFileId(fid);
        EntityID e;
        if (g) {
            e = g.Id();
        } else {
            GameObject obj = scene.CreateGameObject(item.value("name", std::string("entity")));
            obj.AddComponent<FileIdComponent>()->value = fid;
            e = obj.Id();
        }
        incoming[fid] = e;
    }
    auto toEntity = [&](uint64_t fid) -> EntityID {
        if (fid == 0) {
            return kNullEntity;
        }
        auto it = incoming.find(fid);
        if (it != incoming.end()) {
            return it->second;
        }
        GameObject g = scene.FindByFileId(fid);
        return g ? g.Id() : kNullEntity;
    };

    // 名前 + コンポーネント (シリアライズ対象を JSON に一致させる)
    for (const json& item : entities) {
        const uint64_t fid = item.value("fileId", 0ull);
        if (fid == 0) {
            continue;
        }
        const EntityID e = incoming[fid];
        if (!world.IsAlive(e)) {
            continue;
        }
        SetEntityName(world, e, item.value("name", std::string()));
        ApplyTagMask(world, e, item); // 直下キー "tagMask" (name と同じ扱い)
        ReadEntityComponents(scene, fid, e, item, toEntity, /*removeMissing*/ true,
                             removeHiddenMissing);
        ReadEntityOverrides(scene, fid, item); // M48e
    }

    // 親 + 兄弟位置
    for (const json& item : entities) {
        const uint64_t fid = item.value("fileId", 0ull);
        if (fid == 0) {
            continue;
        }
        const EntityID child = incoming[fid];
        if (!world.IsAlive(child)) {
            continue;
        }
        const EntityID parent = item.contains("parent") ? toEntity(item.value("parent", 0ull))
                                                         : kNullEntity;
        if (world.GetParent(child) != parent) {
            world.SetParent(child, parent);
        }
        world.SetSiblingIndex(child, item.value("childIndex", 0xFFFFFFFFu));
    }

    world.ApplyStructuralChanges();
    return true;
}

void RemapEntityRefsInComponents(json& components,
                                 const std::unordered_map<uint64_t, uint64_t>& remap,
                                 bool zeroExternal)
{
    const ComponentRegistry& reg = ComponentRegistry::Get();
    for (auto& [compName, fields] : components.items()) {
        const ComponentTypeId t = reg.FindByName(compName);
        if (t == kInvalidComponentType) {
            // 未登録型 (M70a のパススルー分) はフィールド表が無いので、どのキーが
            // EntityRef なのか判定できない = 付け替えられない。複製した先の参照は
            // 元の fileId を指したままになる。型が引ける状態で複製し直せば直る
            continue;
        }
        for (const FieldDesc& f : reg.Desc(t).fields) {
            if (f.type != FieldType::EntityRef || !fields.contains(f.name)) {
                continue;
            }
            const json& v = fields[f.name];
            if (v.is_number_unsigned() || v.is_number_integer()) {
                const uint64_t id = v.get<uint64_t>();
                auto it = remap.find(id);
                if (it != remap.end()) {
                    fields[f.name] = it->second;
                } else if (zeroExternal) {
                    fields[f.name] = 0;
                }
            }
        }
    }
}

std::vector<uint64_t> CloneSubtree(Scene& scene, const json& subtree)
{
    std::vector<uint64_t> newRoots;
    if (!subtree.is_array() || subtree.empty()) {
        return newRoots;
    }
    // old fileId → new fileId (集合内の全エンティティに新採番)
    std::unordered_map<uint64_t, uint64_t> remap;
    for (const json& item : subtree) {
        const uint64_t old = item.value("fileId", 0ull);
        if (old != 0) {
            remap[old] = scene.NextFileId();
        }
    }

    json out = json::array();
    for (const json& item : subtree) {
        json ni = item;
        const uint64_t old = item.value("fileId", 0ull);
        if (auto it = remap.find(old); it != remap.end()) {
            ni["fileId"] = it->second;
        }
        bool topLevel = true;
        if (item.contains("parent")) {
            const uint64_t p = item.value("parent", 0ull);
            if (auto it = remap.find(p); it != remap.end()) {
                ni["parent"] = it->second; // 集合内 → 新 fileId に付け替え
                topLevel = false;
            }
            // 集合外の親はそのまま維持 (複製は元と同じ親の兄弟になる)
        }
        // EntityRef フィールドを付け替え (集合内なら新 fileId、集合外なら**維持**)
        if (ni.contains("components")) {
            RemapEntityRefsInComponents(ni["components"], remap, /*zeroExternal=*/false);
        }
        if (topLevel && old != 0) {
            newRoots.push_back(remap[old]);
        }
        out.push_back(std::move(ni));
    }

    ApplyPartial(scene, out);
    return newRoots;
}

bool SaveToFile(Scene& scene, const std::wstring& path)
{
    const json root = SaveToJson(scene);
    const std::string text = root.dump(2);
    // ★直接開くと既存の中身がその場で切り詰められ、書き込みが途中で失敗すると前の保存も失う。
    //   false を返せば、エディタは保存済みの基準を更新せず dirty のまま残す
    if (!WriteFileReplacing(path, text)) {
        MYE_LOG_ERROR("scene save: cannot write %s", WideToUtf8(path).c_str());
        return false;
    }
    scene.SetSourcePath(path); // M51g: SaveGame の「現シーンパス」記録用
    MYE_LOG_INFO("scene saved: %s (%zu entities)", WideToUtf8(path).c_str(), root["entities"].size());
    return true;
}

bool LoadFromFile(Scene& scene, const std::wstring& path)
{
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    if (!f) {
        MYE_LOG_ERROR("scene open failed: %s", WideToUtf8(path).c_str());
        return false;
    }
    json root;
    try {
        f >> root;
    } catch (const json::exception& ex) {
        MYE_LOG_ERROR("scene parse failed: %s (%s)", WideToUtf8(path).c_str(), ex.what());
        return false;
    }
    bool ok = false;
    try {
        ok = LoadFromJson(scene, root);
    } catch (const json::exception& ex) {
        // 事前検査をすり抜けた型不一致。落とさずに「開けなかった」として返す
        MYE_LOG_ERROR("scene apply failed: %s (%s)", WideToUtf8(path).c_str(), ex.what());
        return false;
    }
    if (ok) {
        scene.SetSourcePath(path); // M51g: SaveGame の「現シーンパス」記録用
        MYE_LOG_INFO("scene loaded: %s (%u entities)", WideToUtf8(path).c_str(),
                     scene.GetWorld().AliveCount());
    }
    return ok;
}

} // namespace mye::SceneSerializer
