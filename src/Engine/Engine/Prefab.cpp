#include "Engine/Engine/Prefab.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>

#include "Engine/Core/ComponentRegistry.h"
#include "Engine/Core/Components.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/JsonUtil.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Engine/SceneSerializer.h"
#include "Engine/Platform/PathUtil.h"

namespace fs = std::filesystem;

namespace mye {

using nlohmann::json;

namespace {

// ".prefab.json" / ".json" を落としたファイル名 (表示名)
std::string NameFromPath(const std::wstring& path)
{
    std::string name = WideToUtf8(fs::path(path).stem().wstring()); // "X.prefab.json" -> "X.prefab"
    const std::string suf = ".prefab";
    if (name.size() > suf.size() && name.compare(name.size() - suf.size(), suf.size(), suf) == 0) {
        name.resize(name.size() - suf.size()); // -> "X"
    }
    return name;
}

// entities 配列 (SubtreeToJson 形式) から fileId==localId の item を返す (無ければ nullptr)
const json* FindLocal(const json& entities, uint64_t localId)
{
    if (!entities.is_array()) {
        return nullptr;
    }
    for (const json& item : entities) {
        if (item.value("fileId", 0ull) == localId) {
            return &item;
        }
    }
    return nullptr;
}

// components 内の EntityRef フィールド (シリアライズでは fileId の生値) を remap で置換する。
// remap に無い参照は zeroExternal なら 0 (null) に、そうでなければ据え置き
void RemapEntityRefs(json& components, const std::unordered_map<uint64_t, uint64_t>& remap,
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

bool WritePrefabFile(const std::wstring& path, const std::string& name, const json& entities)
{
    json root;
    root["engine"] = "MyEngine";
    root["prefab"] = 1;
    root["name"] = name;
    root["entities"] = entities;
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::ofstream f(fs::path(path), std::ios::binary);
    if (!f) {
        return false;
    }
    const std::string text = root.dump(2);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    return true;
}

// root サブツリー内で PrefabLink を持つエンティティを DFS 順に収集 (インスタンスメンバ)
void CollectInstanceMembers(World& w, EntityID root, std::vector<EntityID>& out)
{
    std::function<void(EntityID)> visit = [&](EntityID e) {
        if (w.GetComponent<PrefabLinkComponent>(e)) {
            out.push_back(e);
        }
        auto* h = w.GetComponent<HierarchyComponent>(e);
        EntityID c = h ? h->firstChild : kNullEntity;
        while (!c.IsNull()) {
            auto* ch = w.GetComponent<HierarchyComponent>(c);
            const EntityID next = ch ? ch->nextSibling : kNullEntity;
            visit(c);
            c = next;
        }
    };
    visit(root);
}

// e のプレハブベース item (無ければ nullptr)。ライブラリと PrefabLink.localId で解決
const json* ResolveBase(World& w, const PrefabLibrary& lib, EntityID e)
{
    auto* link = w.GetComponent<PrefabLinkComponent>(e);
    if (!link) {
        return nullptr;
    }
    const EntityID root = Prefab::FindInstanceRoot(w, e);
    if (root.IsNull()) {
        return nullptr;
    }
    auto* inst = w.GetComponent<PrefabInstanceComponent>(root);
    if (!inst) {
        return nullptr;
    }
    const PrefabAsset* a = lib.Get(inst->prefabHash);
    if (!a) {
        return nullptr;
    }
    return FindLocal(a->entities, link->localId);
}

// NameComponent へ決定論的に名前を書く (WorldHash が 64 バイト生読みするためゼロ埋め必須)
void SetName(World& w, EntityID e, const std::string& name)
{
    if (auto* nc = w.GetComponent<NameComponent>(e)) {
        std::memset(nc->value, 0, sizeof(nc->value));
        const size_t n = (name.size() < sizeof(nc->value) - 1) ? name.size() : sizeof(nc->value) - 1;
        std::memcpy(nc->value, name.data(), n);
    }
}

std::string GetNameStr(World& w, EntityID e)
{
    auto* nc = w.GetComponent<NameComponent>(e);
    return nc ? std::string(nc->value, strnlen(nc->value, sizeof(nc->value))) : std::string();
}

} // namespace

// ==== PrefabLibrary ====

uint64_t PrefabLibrary::HashForPath(const std::wstring& path)
{
    return HashStr(WideToUtf8(NormalizePathKey(path)));
}

uint64_t PrefabLibrary::Register(const std::wstring& path, std::string name, json entities)
{
    const uint64_t hash = HashForPath(path);
    PrefabAsset& a = assets_[hash];
    a.hash = hash;
    a.name = std::move(name);
    a.path = path;
    a.entities = std::move(entities);
    return hash;
}

uint64_t PrefabLibrary::LoadFromFile(const std::wstring& path)
{
    std::ifstream f(fs::path(path), std::ios::binary);
    if (!f) {
        return 0;
    }
    json root;
    try {
        f >> root;
    } catch (const json::exception& ex) {
        MYE_LOG_WARN("prefab parse failed: %s (%s)", WideToUtf8(path).c_str(), ex.what());
        return 0;
    }
    if (!root.contains("entities") || !root["entities"].is_array()) {
        MYE_LOG_WARN("prefab load: no entities array in %s", WideToUtf8(path).c_str());
        return 0;
    }
    std::string name = root.value("name", NameFromPath(path));
    return Register(path, std::move(name), root["entities"]);
}

const PrefabAsset* PrefabLibrary::Get(uint64_t hash) const
{
    auto it = assets_.find(hash);
    return (it != assets_.end()) ? &it->second : nullptr;
}

std::vector<PrefabEntry> PrefabLibrary::Enumerate() const
{
    std::vector<PrefabEntry> out;
    out.reserve(assets_.size());
    for (const auto& [h, a] : assets_) {
        out.push_back({ a.hash, a.name, a.path });
    }
    std::sort(out.begin(), out.end(),
              [](const PrefabEntry& x, const PrefabEntry& y) { return x.name < y.name; });
    return out;
}

// ==== Prefab operations ====

namespace Prefab {

EntityID FindInstanceRoot(World& world, EntityID e)
{
    EntityID cur = e;
    while (!cur.IsNull() && world.IsAlive(cur)) {
        if (world.GetComponent<PrefabInstanceComponent>(cur)) {
            return cur;
        }
        cur = world.GetParent(cur);
    }
    return kNullEntity;
}

json ExtractLocal(Scene& scene, EntityID root)
{
    const json sub = SceneSerializer::SubtreeToJson(scene, root); // scene fileId、DFS (root 先頭)
    json out = json::array();
    if (!sub.is_array() || sub.empty()) {
        return out;
    }
    // scene fileId -> ローカル id (配列順 = DFS。root=1)
    std::unordered_map<uint64_t, uint64_t> remap;
    uint64_t next = 1;
    for (const json& item : sub) {
        const uint64_t f = item.value("fileId", 0ull);
        if (f != 0) {
            remap[f] = next++;
        }
    }
    for (const json& item : sub) {
        json ni = item;
        const uint64_t f = item.value("fileId", 0ull);
        if (auto it = remap.find(f); it != remap.end()) {
            ni["fileId"] = it->second;
        }
        if (ni.contains("parent")) {
            const uint64_t p = ni.value("parent", 0ull);
            if (auto it = remap.find(p); it != remap.end()) {
                ni["parent"] = it->second;
            } else {
                ni.erase("parent"); // 集合外の親 (= root の元親) → プレハブルート化
            }
        }
        if (ni.contains("components")) {
            RemapEntityRefs(ni["components"], remap, /*zeroExternal*/ true);
            ni["components"].erase("PrefabInstance"); // タグは新規インスタンス化時に付与
            ni["components"].erase("PrefabLink");
        }
        out.push_back(std::move(ni));
    }
    return out;
}

// PrefabLink.localId を保った抽出 (Apply 用 — ベースの localId 対応を崩さない)
static json ExtractLocalByLinks(Scene& scene, EntityID root)
{
    World& w = scene.GetWorld();
    const json sub = SceneSerializer::SubtreeToJson(scene, root);
    json out = json::array();
    if (!sub.is_array()) {
        return out;
    }
    std::unordered_map<uint64_t, uint64_t> remap; // scene fileId -> localId
    for (const json& item : sub) {
        const uint64_t f = item.value("fileId", 0ull);
        GameObject g = scene.FindByFileId(f);
        if (!g) {
            continue;
        }
        if (auto* link = w.GetComponent<PrefabLinkComponent>(g.Id())) {
            remap[f] = link->localId;
        }
    }
    for (const json& item : sub) {
        const uint64_t f = item.value("fileId", 0ull);
        auto it = remap.find(f);
        if (it == remap.end()) {
            continue; // ユーザーが追加した非プレハブの子は新ベースに含めない
        }
        json ni = item;
        ni["fileId"] = it->second;
        if (ni.contains("parent")) {
            const uint64_t p = ni.value("parent", 0ull);
            if (auto pit = remap.find(p); pit != remap.end()) {
                ni["parent"] = pit->second;
            } else {
                ni.erase("parent");
            }
        }
        if (ni.contains("components")) {
            RemapEntityRefs(ni["components"], remap, /*zeroExternal*/ true);
            ni["components"].erase("PrefabInstance");
            ni["components"].erase("PrefabLink");
        }
        out.push_back(std::move(ni));
    }
    return out;
}

uint64_t InstantiateEntities(Scene& scene, const json& localEntities, uint64_t prefabHash,
                             uint64_t parentFileId)
{
    if (!localEntities.is_array() || localEntities.empty()) {
        return 0;
    }
    // localId -> 新 scene fileId
    std::unordered_map<uint64_t, uint64_t> remap;
    for (const json& item : localEntities) {
        const uint64_t local = item.value("fileId", 0ull);
        if (local != 0) {
            remap[local] = scene.NextFileId();
        }
    }
    uint64_t rootLocal = 0;
    json out = json::array();
    for (const json& item : localEntities) {
        json ni = item;
        const uint64_t local = item.value("fileId", 0ull);
        if (auto it = remap.find(local); it != remap.end()) {
            ni["fileId"] = it->second;
        }
        bool hasParent = false;
        if (ni.contains("parent")) {
            const uint64_t p = ni.value("parent", 0ull);
            if (auto it = remap.find(p); it != remap.end()) {
                ni["parent"] = it->second;
                hasParent = true;
            } else {
                ni.erase("parent");
            }
        }
        if (!hasParent) {
            rootLocal = local; // プレハブルート (集合内に親を持たない)
            if (parentFileId != 0) {
                ni["parent"] = parentFileId;
            }
        }
        if (ni.contains("components")) {
            RemapEntityRefs(ni["components"], remap, /*zeroExternal*/ true);
        }
        out.push_back(std::move(ni));
    }

    SceneSerializer::ApplyPartial(scene, out);
    scene.GetWorld().ApplyStructuralChanges();

    // タグ付け (localId 昇順で決定論的に処理。順序はハッシュに影響しないが規約に合わせる)
    std::vector<std::pair<uint64_t, uint64_t>> pairs; // (localId, newFid)
    pairs.reserve(remap.size());
    for (const auto& [local, fid] : remap) {
        pairs.emplace_back(local, fid);
    }
    std::sort(pairs.begin(), pairs.end());
    const uint64_t newRootFid = remap.count(rootLocal) ? remap[rootLocal] : 0;
    for (const auto& [local, fid] : pairs) {
        GameObject g = scene.FindByFileId(fid);
        if (!g) {
            continue;
        }
        g.AddComponent<PrefabLinkComponent>()->localId = local;
        if (local == rootLocal) {
            g.AddComponent<PrefabInstanceComponent>()->prefabHash = prefabHash;
        }
    }
    scene.GetWorld().ApplyStructuralChanges();
    return newRootFid;
}

uint64_t Instantiate(Scene& scene, const PrefabLibrary& lib, uint64_t prefabHash,
                     uint64_t parentFileId)
{
    const PrefabAsset* a = lib.Get(prefabHash);
    if (!a) {
        MYE_LOG_WARN("prefab instantiate: unknown hash %llu",
                     static_cast<unsigned long long>(prefabHash));
        return 0;
    }
    return InstantiateEntities(scene, a->entities, prefabHash, parentFileId);
}

uint64_t CreateAsset(Scene& scene, PrefabLibrary& lib, const std::wstring& path, EntityID root)
{
    const json entities = ExtractLocal(scene, root);
    if (entities.empty()) {
        return 0;
    }
    const std::string name = NameFromPath(path);
    if (!WritePrefabFile(path, name, entities)) {
        MYE_LOG_ERROR("prefab create: cannot write %s", WideToUtf8(path).c_str());
        return 0;
    }
    const uint64_t hash = lib.Register(path, name, entities);

    // 既存の root サブツリーをこのプレハブのインスタンスとしてタグ付けする
    // (localId は ExtractLocal と同じ DFS 順で 1..N = ベースと一致)
    World& w = scene.GetWorld();
    const json sub = SceneSerializer::SubtreeToJson(scene, root);
    uint64_t local = 1;
    for (const json& item : sub) {
        const uint64_t f = item.value("fileId", 0ull);
        GameObject g = scene.FindByFileId(f);
        if (g) {
            g.AddComponent<PrefabLinkComponent>()->localId = local;
            if (local == 1) {
                g.AddComponent<PrefabInstanceComponent>()->prefabHash = hash;
            }
        }
        ++local;
    }
    w.ApplyStructuralChanges();
    MYE_LOG_INFO("prefab created: %s (%zu entities)", WideToUtf8(path).c_str(), entities.size());
    return hash;
}

bool IsFieldOverridden(Scene& scene, const PrefabLibrary& lib, EntityID e, const char* compName,
                       const FieldDesc& field)
{
    if (field.type == FieldType::EntityRef) {
        return false; // remap 判定が不確実なため対象外
    }
    World& w = scene.GetWorld();
    const json* base = ResolveBase(w, lib, e);
    if (!base) {
        return false;
    }
    const json comps = base->contains("components") ? (*base)["components"] : json::object();
    if (!comps.contains(compName)) {
        return true; // インスタンスで追加されたコンポーネント
    }
    const json& bc = comps[compName];
    if (!bc.contains(field.name)) {
        return true;
    }
    const void* comp = w.GetComponentRaw(e, ComponentRegistry::Get().FindByName(compName));
    if (!comp) {
        return false;
    }
    return FieldToJson(comp, field) != bc[field.name];
}

bool IsNameOverridden(Scene& scene, const PrefabLibrary& lib, EntityID e)
{
    World& w = scene.GetWorld();
    const json* base = ResolveBase(w, lib, e);
    if (!base) {
        return false;
    }
    return GetNameStr(w, e) != base->value("name", std::string());
}

void RevertField(Scene& scene, const PrefabLibrary& lib, EntityID e, const char* compName,
                 const FieldDesc& field)
{
    if (field.type == FieldType::EntityRef) {
        return;
    }
    World& w = scene.GetWorld();
    const json* base = ResolveBase(w, lib, e);
    if (!base || !base->contains("components")) {
        return;
    }
    const json& comps = (*base)["components"];
    if (!comps.contains(compName) || !comps[compName].contains(field.name)) {
        return;
    }
    void* comp = w.GetComponentRaw(e, ComponentRegistry::Get().FindByName(compName));
    if (comp) {
        FieldFromJson(comp, field, comps[compName][field.name]);
    }
}

void RevertInstance(Scene& scene, const PrefabLibrary& lib, uint64_t rootFileId)
{
    World& w = scene.GetWorld();
    GameObject rootGo = scene.FindByFileId(rootFileId);
    if (!rootGo) {
        return;
    }
    auto* inst = w.GetComponent<PrefabInstanceComponent>(rootGo.Id());
    if (!inst) {
        return;
    }
    const PrefabAsset* a = lib.Get(inst->prefabHash);
    if (!a) {
        return;
    }
    std::vector<EntityID> members;
    CollectInstanceMembers(w, rootGo.Id(), members);
    const ComponentRegistry& reg = ComponentRegistry::Get();
    for (EntityID m : members) {
        auto* link = w.GetComponent<PrefabLinkComponent>(m);
        if (!link) {
            continue;
        }
        const json* base = FindLocal(a->entities, link->localId);
        if (!base) {
            continue;
        }
        SetName(w, m, base->value("name", std::string()));
        if (!base->contains("components")) {
            continue;
        }
        for (const auto& [compName, fields] : (*base)["components"].items()) {
            const ComponentTypeId t = reg.FindByName(compName);
            if (t == kInvalidComponentType) {
                continue;
            }
            void* comp = w.GetComponentRaw(m, t);
            if (!comp) {
                continue; // インスタンスに無いコンポーネントは足さない (フィールドのみ復元)
            }
            for (const FieldDesc& f : reg.Desc(t).fields) {
                if (f.type == FieldType::EntityRef || (f.flags & kFieldNoSerialize)) {
                    continue;
                }
                if (fields.contains(f.name)) {
                    FieldFromJson(comp, f, fields[f.name]);
                }
            }
        }
    }
    w.ApplyStructuralChanges();
}

void PropagateBaseChange(Scene& scene, const json& oldBase, const json& newBase, uint64_t prefabHash)
{
    World& w = scene.GetWorld();
    const ComponentRegistry& reg = ComponentRegistry::Get();

    // 対象プレハブの全インスタンスルートを収集
    std::vector<EntityID> roots;
    {
        const ComponentTypeId req[] = { PrefabInstanceComponent::sTypeId };
        w.ForEachArchetype(req, [&](Archetype& arch) {
            const int fi = arch.FindTypeIndex(PrefabInstanceComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                if (static_cast<const PrefabInstanceComponent*>(arch.GetPtr(fi, row))->prefabHash
                    == prefabHash) {
                    roots.push_back(arch.EntityAt(row));
                }
            }
        });
    }

    for (EntityID root : roots) {
        std::vector<EntityID> members;
        CollectInstanceMembers(w, root, members);
        for (EntityID m : members) {
            auto* link = w.GetComponent<PrefabLinkComponent>(m);
            if (!link) {
                continue;
            }
            const json* ob = FindLocal(oldBase, link->localId);
            const json* nb = FindLocal(newBase, link->localId);
            if (!nb) {
                continue; // 新ベースに存在しない → 触らない
            }
            // 名前 (非オーバーライドのみ伝播)
            {
                const std::string oldName = ob ? ob->value("name", std::string()) : std::string();
                const std::string newName = nb->value("name", std::string());
                if (oldName != newName && GetNameStr(w, m) == oldName) {
                    SetName(w, m, newName);
                }
            }
            if (!nb->contains("components")) {
                continue;
            }
            const json obComps = (ob && ob->contains("components")) ? (*ob)["components"]
                                                                    : json::object();
            for (const auto& [compName, nfields] : (*nb)["components"].items()) {
                const ComponentTypeId t = reg.FindByName(compName);
                if (t == kInvalidComponentType) {
                    continue;
                }
                void* comp = w.GetComponentRaw(m, t);
                bool added = false;
                if (!comp) {
                    comp = w.AddComponentRaw(m, t); // 新規にベースへ加わったコンポーネント
                    added = true;
                }
                if (!comp) {
                    continue;
                }
                const json obFields = obComps.contains(compName) ? obComps[compName] : json::object();
                for (const FieldDesc& f : reg.Desc(t).fields) {
                    if (f.type == FieldType::EntityRef || (f.flags & kFieldNoSerialize)) {
                        continue;
                    }
                    if (!nfields.contains(f.name)) {
                        continue;
                    }
                    const json& newVal = nfields[f.name];
                    const bool hadOld = obFields.contains(f.name);
                    if (added) {
                        FieldFromJson(comp, f, newVal); // 新規コンポーネント → ベース値で埋める
                        continue;
                    }
                    if (hadOld && obFields[f.name] == newVal) {
                        continue; // ベース側で変化なし → 触らない
                    }
                    // 非オーバーライド (現在値が旧ベースと一致、または旧ベースに無い) のみ伝播
                    const json cur = FieldToJson(comp, f);
                    if (!hadOld || cur == obFields[f.name]) {
                        FieldFromJson(comp, f, newVal);
                    }
                }
            }
        }
    }
    w.ApplyStructuralChanges();
}

bool ApplyInstance(Scene& scene, PrefabLibrary& lib, uint64_t rootFileId)
{
    World& w = scene.GetWorld();
    GameObject rootGo = scene.FindByFileId(rootFileId);
    if (!rootGo) {
        return false;
    }
    auto* inst = w.GetComponent<PrefabInstanceComponent>(rootGo.Id());
    if (!inst) {
        return false;
    }
    const PrefabAsset* a = lib.Get(inst->prefabHash);
    if (!a) {
        return false;
    }
    const uint64_t hash = inst->prefabHash;
    const json oldBase = a->entities; // 伝播用にコピー
    const std::wstring path = a->path;
    const std::string name = a->name;

    const json newBase = ExtractLocalByLinks(scene, rootGo.Id());
    if (newBase.empty()) {
        return false;
    }
    if (!WritePrefabFile(path, name, newBase)) {
        MYE_LOG_WARN("prefab apply: file write failed for %s", WideToUtf8(path).c_str());
    }
    lib.Register(path, name, newBase); // library 更新 (hash 不変)
    PropagateBaseChange(scene, oldBase, newBase, hash); // 他インスタンスへ伝播
    MYE_LOG_INFO("prefab apply: '%s' base updated + propagated", name.c_str());
    return true;
}

} // namespace Prefab
} // namespace mye
