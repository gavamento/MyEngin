#include "Engine/Engine/SceneSerializer.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <unordered_map>
#include <vector>

#include "Engine/Core/Components.h"
#include "Engine/Core/JsonUtil.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/EntityNaming.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Engine/Script/ManagedHost.h"
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

// 1 エンティティ → JSON オブジェクト (fileId/name/parent/childIndex/components)。
// EntityRef フィールドと親は fileId で出力する (参照透過な復元のため)
json WriteEntity(World& world, EntityID e, uint32_t childIndex)
{
    const ComponentRegistry& reg = ComponentRegistry::Get();
    json item;
    item["fileId"] = FidOf(world, e);
    item["name"] = world.GetName(e);
    const EntityID parent = world.GetParent(e);
    if (!parent.IsNull() && world.IsAlive(parent)) {
        item["parent"] = FidOf(world, parent);
    }
    item["childIndex"] = childIndex;

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
    item["components"] = std::move(comps);
    return item;
}

// JSON の components を エンティティ e へ流し込む。EntityRef は toEntity で fileId→EntityID に解決。
// removeMissing=true のとき、JSON に無いシリアライズ対象コンポーネントを除去する。
// removeHiddenMissing=true なら kComponentHidden なものも除去対象に含める (M48c) —
// シリアライズされる隠しコンポーネントは PrefabInstance / PrefabLink だけなので、
// これは実質「プレハブタグを JSON に一致させるか」のスイッチ。既定 false は従来どおりの
// ビット不変ロード (シーンロード/ApplyDiff/プレハブ展開はタグを消してはならない)
void ReadEntityComponents(World& world, EntityID e, const json& item,
                          const std::function<EntityID(uint64_t)>& toEntity, bool removeMissing,
                          bool removeHiddenMissing = false)
{
    const ComponentRegistry& reg = ComponentRegistry::Get();
    const json comps = item.contains("components") ? item["components"] : json::object();
    for (const auto& [compName, fields] : comps.items()) {
        const ComponentTypeId t = reg.FindByName(compName);
        if (t == kInvalidComponentType) {
            MYE_LOG_WARN("scene load: unknown component '%s' (skipped)", compName.c_str());
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
                if (!comps.contains(desc.name)) {
                    world.RemoveComponentRaw(e, t);
                }
            }
        }
    }
}

// DFS 順 (ルート firstRoot → 子は firstChild/nextSibling) で全エンティティと兄弟 index を収集
void CollectHierarchyOrdered(World& world, std::vector<EntityID>& out, std::vector<uint32_t>& outIdx)
{
    std::function<void(EntityID, uint32_t)> visit = [&](EntityID e, uint32_t idx) {
        out.push_back(e);
        outIdx.push_back(idx);
        auto* h = world.GetComponent<HierarchyComponent>(e);
        if (!h) {
            return;
        }
        EntityID c = h->firstChild;
        uint32_t ci = 0;
        while (!c.IsNull()) {
            auto* ch = world.GetComponent<HierarchyComponent>(c);
            const EntityID next = ch ? ch->nextSibling : kNullEntity;
            visit(c, ci++);
            c = next;
        }
    };
    EntityID r = world.FirstRoot();
    uint32_t ri = 0;
    while (!r.IsNull()) {
        auto* rh = world.GetComponent<HierarchyComponent>(r);
        const EntityID next = rh ? rh->nextSibling : kNullEntity;
        visit(r, ri++);
        r = next;
    }
}

void CollectSubtreeOrdered(World& world, EntityID root, uint32_t rootIdx, std::vector<EntityID>& out,
                           std::vector<uint32_t>& outIdx)
{
    std::function<void(EntityID, uint32_t)> visit = [&](EntityID e, uint32_t idx) {
        out.push_back(e);
        outIdx.push_back(idx);
        auto* h = world.GetComponent<HierarchyComponent>(e);
        if (!h) {
            return;
        }
        EntityID c = h->firstChild;
        uint32_t ci = 0;
        while (!c.IsNull()) {
            auto* ch = world.GetComponent<HierarchyComponent>(c);
            const EntityID next = ch ? ch->nextSibling : kNullEntity;
            visit(c, ci++);
            c = next;
        }
    };
    visit(root, rootIdx);
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

    json items = json::array();
    for (size_t i = 0; i < entities.size(); ++i) {
        items.push_back(WriteEntity(world, entities[i], childIndices[i]));
    }

    json root;
    root["engine"] = "MyEngine";
    root["version"] = 2; // v2: EntityRef を fileId で保存 + childIndex (兄弟順)
    root["sceneName"] = scene.Name();
    root["nextFileId"] = scene.PeekNextFileId();
    root["entities"] = std::move(items);
    return root;
}

bool LoadFromJson(Scene& scene, const json& root)
{
    if (!root.is_object() || !root.contains("entities")) {
        MYE_LOG_ERROR("scene load: invalid json");
        return false;
    }
    World& world = scene.GetWorld();

    scene.Clear();
    scene.SetName(root.value("sceneName", std::string("Untitled")));
    scene.SetNextFileId(root.value("nextFileId", 1ull));

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
    for (const json& item : items) {
        const EntityID e = toEntity(item.value("fileId", 0ull));
        if (e.IsNull()) {
            continue;
        }
        ReadEntityComponents(world, e, item, toEntity, /*removeMissing*/ false);
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
    return true;
}

bool ApplyDiff(Scene& scene, const json& root)
{
    if (!root.is_object() || !root.contains("entities")) {
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

    const json& items = root["entities"];
    std::unordered_map<uint64_t, const json*> incoming;
    for (const json& item : items) {
        const uint64_t fid = item.value("fileId", 0ull);
        if (fid != 0) {
            incoming[fid] = &item;
        }
    }

    int created = 0, updated = 0, destroyed = 0;

    // 1) 新規生成
    for (const auto& [fid, item] : incoming) {
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
    for (const auto& [fid, itemPtr] : incoming) {
        const json& item = *itemPtr;
        const EntityID e = existing[fid];
        if (!world.IsAlive(e)) {
            continue;
        }
        SetEntityName(world, e, item.value("name", std::string()));
        ReadEntityComponents(world, e, item, toEntity, /*removeMissing*/ true);
        ++updated;
    }

    // 3) ファイルから消えたものは破棄
    for (const auto& [fid, e] : existing) {
        if (!incoming.contains(fid)) {
            world.DestroyEntity(e);
            ++destroyed;
        }
    }

    // 4) 親子関係
    for (const auto& [fid, itemPtr] : incoming) {
        const EntityID child = existing[fid];
        const uint64_t parentFid = itemPtr->value("parent", 0ull);
        const EntityID parent = toEntity(parentFid);
        if (world.GetParent(child) != parent) {
            world.SetParent(child, parent);
        }
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
    return WriteEntity(world, e, SiblingIndexOf(world, e));
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
        arr.push_back(WriteEntity(world, ents[i], idxs[i]));
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
        ReadEntityComponents(world, e, item, toEntity, /*removeMissing*/ true, removeHiddenMissing);
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
    std::ofstream f(std::filesystem::path(path), std::ios::binary);
    if (!f) {
        MYE_LOG_ERROR("scene save: cannot open %s", WideToUtf8(path).c_str());
        return false;
    }
    const std::string text = root.dump(2);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
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
    const bool ok = LoadFromJson(scene, root);
    if (ok) {
        MYE_LOG_INFO("scene loaded: %s (%u entities)", WideToUtf8(path).c_str(),
                     scene.GetWorld().AliveCount());
    }
    return ok;
}

} // namespace mye::SceneSerializer
