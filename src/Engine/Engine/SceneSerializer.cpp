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
