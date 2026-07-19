#include "Engine/Engine/Scene.h"

#include <cstring>

namespace mye {

GameObject Scene::FindByFileId(uint64_t fileId)
{
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
