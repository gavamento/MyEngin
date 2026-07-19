#include "Engine/Engine/Scene.h"

#include <cstring>

namespace mye {

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
