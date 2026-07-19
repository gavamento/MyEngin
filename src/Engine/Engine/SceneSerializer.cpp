#include "Engine/Engine/SceneSerializer.h"

#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <vector>

#include "Engine/Core/Components.h"
#include "Engine/Core/JsonUtil.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Platform/PathUtil.h"

namespace mye::SceneSerializer {

using nlohmann::json;

json SaveToJson(Scene& scene)
{
    World& world = scene.GetWorld();
    const ComponentRegistry& reg = ComponentRegistry::Get();

    // 保留中の構造変更 (SetParent / Destroy 等) を反映してから保存する。
    // これが無いと OnStart 直後や UI 操作直後のスナップショットが不完全になる
    world.ApplyStructuralChanges();

    // 先にエンティティ一覧を確定 (fileId 採番はアーキタイプ移動を伴うため)
    std::vector<EntityID> entities;
    {
        const ComponentTypeId req[] = { NameComponent::sTypeId };
        world.ForEachArchetype(req, [&](Archetype& arch) {
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                entities.push_back(arch.EntityAt(row));
            }
        });
    }
    // fileId 未割り当てのエンティティに採番
    for (EntityID e : entities) {
        if (!world.HasComponent(e, FileIdComponent::sTypeId)) {
            world.AddComponent<FileIdComponent>(e)->value = scene.NextFileId();
        } else if (world.GetComponent<FileIdComponent>(e)->value == 0) {
            world.GetComponent<FileIdComponent>(e)->value = scene.NextFileId();
        }
    }

    json items = json::array();
    for (EntityID e : entities) {
        json item;
        item["fileId"] = world.GetComponent<FileIdComponent>(e)->value;
        item["name"] = world.GetName(e);

        const EntityID parent = world.GetParent(e);
        if (!parent.IsNull() && world.IsAlive(parent)) {
            auto* pfid = world.GetComponent<FileIdComponent>(parent);
            item["parent"] = pfid ? pfid->value : 0;
        }

        json comps = json::object();
        // アーキタイプの型リスト順 (TypeId 昇順) — 出力順は決定論的
        const Archetype* arch = world.GetArchetype(e);
        if (!arch) {
            continue;
        }
        for (ComponentTypeId t : arch->Types()) {
            const ComponentDesc& desc = reg.Desc(t);
            if (desc.flags & kComponentNoSerialize) {
                continue;
            }
            if (t == NameComponent::sTypeId) {
                continue; // "name" として出力済み
            }
            json fields = json::object();
            const void* comp = world.GetComponentRaw(e, t);
            for (const FieldDesc& f : desc.fields) {
                if (f.flags & kFieldNoSerialize) {
                    continue;
                }
                fields[f.name] = FieldToJson(comp, f);
            }
            comps[desc.name] = std::move(fields);
        }
        item["components"] = std::move(comps);
        items.push_back(std::move(item));
    }

    json root;
    root["engine"] = "MyEngine";
    root["version"] = 1;
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
    const ComponentRegistry& reg = ComponentRegistry::Get();

    scene.Clear();
    scene.SetName(root.value("sceneName", std::string("Untitled")));
    scene.SetNextFileId(root.value("nextFileId", 1ull));

    // 1) 全エンティティを生成して fileId → EntityID 対応表を作る
    std::unordered_map<uint64_t, EntityID> byFileId;
    const json& items = root["entities"];
    for (const json& item : items) {
        const uint64_t fileId = item.value("fileId", 0ull);
        GameObject obj = scene.CreateGameObject(item.value("name", std::string("entity")));
        obj.AddComponent<FileIdComponent>()->value = fileId;
        if (fileId != 0) {
            byFileId[fileId] = obj.Id();
        }
    }

    // 2) コンポーネントとフィールドを流し込む
    for (const json& item : items) {
        const uint64_t fileId = item.value("fileId", 0ull);
        auto it = byFileId.find(fileId);
        if (it == byFileId.end()) {
            continue;
        }
        const EntityID e = it->second;
        if (item.contains("components")) {
            for (const auto& [compName, fields] : item["components"].items()) {
                const ComponentTypeId t = reg.FindByName(compName);
                if (t == kInvalidComponentType) {
                    MYE_LOG_WARN("scene load: unknown component '%s' (skipped)", compName.c_str());
                    continue;
                }
                void* comp = world.AddComponentRaw(e, t); // 既存ならそのポインタ
                if (!comp) {
                    continue;
                }
                const ComponentDesc& desc = reg.Desc(t);
                for (const FieldDesc& f : desc.fields) {
                    if (fields.contains(f.name)) {
                        FieldFromJson(comp, f, fields[f.name]);
                    }
                }
            }
        }
    }

    // 3) 親子関係 (fileId → EntityID)
    for (const json& item : items) {
        if (!item.contains("parent")) {
            continue;
        }
        const uint64_t childId = item.value("fileId", 0ull);
        const uint64_t parentId = item.value("parent", 0ull);
        auto ci = byFileId.find(childId);
        auto pi = byFileId.find(parentId);
        if (ci != byFileId.end() && pi != byFileId.end()) {
            world.SetParent(ci->second, pi->second);
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
    const ComponentRegistry& reg = ComponentRegistry::Get();

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

    // 2) 更新 (名前 / コンポーネント追加・更新・除去)
    for (const auto& [fid, itemPtr] : incoming) {
        const json& item = *itemPtr;
        const EntityID e = existing[fid];
        if (!world.IsAlive(e)) {
            continue;
        }
        if (auto* nc = world.GetComponent<NameComponent>(e)) {
            const std::string name = item.value("name", std::string());
            strncpy_s(nc->value, name.c_str(), _TRUNCATE);
        }
        const json comps = item.contains("components") ? item["components"] : json::object();
        for (const auto& [compName, fields] : comps.items()) {
            const ComponentTypeId t = reg.FindByName(compName);
            if (t == kInvalidComponentType) {
                continue;
            }
            void* comp = world.AddComponentRaw(e, t);
            if (!comp) {
                continue;
            }
            for (const FieldDesc& f : reg.Desc(t).fields) {
                if (fields.contains(f.name)) {
                    FieldFromJson(comp, f, fields[f.name]);
                }
            }
        }
        // JSON に無いシリアライズ対象コンポーネントは除去
        if (const Archetype* arch = world.GetArchetype(e)) {
            std::vector<ComponentTypeId> types(arch->Types().begin(), arch->Types().end());
            for (ComponentTypeId t : types) {
                const ComponentDesc& desc = reg.Desc(t);
                if ((desc.flags & (kComponentNoSerialize | kComponentHidden)) != 0
                    || t == NameComponent::sTypeId || t == LocalTransform::sTypeId) {
                    continue;
                }
                if (!comps.contains(desc.name)) {
                    world.RemoveComponentRaw(e, t);
                }
            }
        }
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
        EntityID parent = kNullEntity;
        if (parentFid != 0) {
            auto pi = existing.find(parentFid);
            if (pi != existing.end()) {
                parent = pi->second;
            }
        }
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
    MYE_LOG_INFO("[reload] scene diff applied: %d updated, %d created, %d destroyed",
                 updated, created, destroyed);
    return true;
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
    MYE_LOG_INFO("scene saved: %s (%zu entities)", WideToUtf8(path).c_str(),
                 root["entities"].size());
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
