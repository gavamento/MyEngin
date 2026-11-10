//====================================================================================
//                          ComponentDependencies.cpp
//  MyEngin/ 秋田蓮音                                                     10/03/2026
//                                          Add Component で必須の相棒コンポーネントも足す規則の実装
//====================================================================================
#include "Editor/Scene/ComponentDependencies.h"

#include <cstring>

#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"

namespace mye {

void AddComponentWithRequirements(World& world, EntityID e, ComponentTypeId type)
{
    world.AddComponentRaw(e, type);
    // NavMeshAgent は CharacterController に速度を渡して歩く (M82 spec 2. #8)。無いと動かないので同時に足す
    // CharacterInput (M97a) も CharacterController の moveInput / jumpSpeed へ書くだけなので同じ扱い
    const char* name = ComponentRegistry::Get().Desc(type).name;
    if ((std::strcmp(name, "NavMeshAgent") == 0 || std::strcmp(name, "CharacterInput") == 0)
        && !world.HasComponent(e, CharacterControllerComponent::sTypeId)) {
        world.AddComponentRaw(e, CharacterControllerComponent::sTypeId);
    }
}

} // namespace mye
