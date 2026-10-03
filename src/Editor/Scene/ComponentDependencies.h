//====================================================================================
//                          ComponentDependencies.h
//  MyEngin/ 秋田蓮音                                                     10/03/2026
//                                          Add Component で必須の相棒コンポーネントも足す規則
//====================================================================================
#pragma once

#include "Engine/Core/Ecs/ComponentRegistry.h"
#include "Engine/Core/Ecs/EntityID.h"

namespace mye {

class World;

// type を e に足し、type が必須とする相棒 (NavMeshAgent -> CharacterController) が無ければそれも足す。
// どちらも AddComponentRaw なので構造変更の確定 (ApplyStructuralChanges) は呼び出し側。
// 呼び出し側が 1 つの Undo レコードで囲めば、相棒も同じ 1 Undo で戻る
void AddComponentWithRequirements(World& world, EntityID e, ComponentTypeId type);

} // namespace mye
