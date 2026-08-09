#include "Engine/Engine/Scene.h"

#include <cstring>

namespace mye {

GameObject Scene::FindByFileId(uint64_t fileId)
{
    // M51a: ヒット時検証つきキャッシュ。fileId のシーン内一意 (NextFileId 単調採番) が
    // 前提 — 重複していると線形走査と異なる方を返し得るが、それは元データの破損。
    // 検証がヒット毎に走るため、書込点 (シリアライザ / Prefab / Undo 復元) のフックは不要
    const bool useCache = (fileId != 0) && World::SimCacheEnabled();
    if (useCache) {
        if (auto it = fileIdCache_.find(fileId); it != fileIdCache_.end()) {
            const EntityID e = it->second;
            if (world_.IsAlive(e)) {
                if (const auto* f = world_.GetComponent<FileIdComponent>(e);
                    f && f->value == fileId) {
                    return GameObject(&world_, e);
                }
            }
            fileIdCache_.erase(it); // stale — 線形走査へフォールバックして補修
        }
    }
    const ComponentTypeId fidType = FileIdComponent::sTypeId;
    GameObject result;
    world_.ForEachArchetype({ &fidType, 1 }, [&](Archetype& arch) {
        if (result) {
            return;
        }
        const int fi = arch.FindTypeIndex(fidType);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            if (static_cast<const FileIdComponent*>(arch.GetPtr(fi, row))->value == fileId) {
                result = GameObject(&world_, arch.EntityAt(row));
                return;
            }
        }
    });
    if (useCache && result) {
        fileIdCache_[fileId] = result.Id();
    }
    return result;
}

uint64_t Scene::EnsureFileId(EntityID e)
{
    if (!world_.IsAlive(e)) {
        return 0;
    }
    auto* f = world_.GetComponent<FileIdComponent>(e);
    if (!f) {
        f = world_.AddComponent<FileIdComponent>(e); // イテレーション外 (エディタ) では即時
        f->value = NextFileId();
    } else if (f->value == 0) {
        f->value = NextFileId();
    }
    if (f->value != 0 && World::SimCacheEnabled()) {
        fileIdCache_[f->value] = e; // 採番点でも充填 (直後の FindByFileId の初回走査を省く)
    }
    return f->value;
}

GameObject Scene::Find(std::string_view name)
{
    const ComponentTypeId nameType = NameComponent::sTypeId;
    GameObject result;
    world_.ForEachArchetype({ &nameType, 1 }, [&](Archetype& arch) {
        if (result) {
            return;
        }
        const int ni = arch.FindTypeIndex(nameType);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const auto* nc = static_cast<const NameComponent*>(arch.GetPtr(ni, row));
            if (name == nc->value) {
                result = GameObject(&world_, arch.EntityAt(row));
                return;
            }
        }
    });
    return result;
}

} // namespace mye
