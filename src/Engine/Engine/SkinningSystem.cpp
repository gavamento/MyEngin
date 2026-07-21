#include "Engine/Engine/SkinningSystem.h"

#include "Engine/Core/Components.h"
#include "Engine/Core/World.h"
#include "Engine/Renderer/GpuResources.h"

namespace mye {

void SkinningSystem::Update(World& world, const RenderResources& resources)
{
    const ComponentTypeId req[] = { SkinnedMeshComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int si = arch.FindTypeIndex(SkinnedMeshComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            auto* sm = static_cast<SkinnedMeshComponent*>(arch.GetPtr(si, row));
            if (!sm->playing) {
                continue;
            }
            const SkinnedModel* model = resources.skinnedModels.Get(sm->model);
            if (!model || sm->clip < 0 || sm->clip >= static_cast<int>(model->clips.size())) {
                continue;
            }
            // クリップ長 (秒) → tick。60Hz 前提でループ (末尾で 0 に戻す)
            const float durSec = model->clips[static_cast<size_t>(sm->clip)].duration;
            const int durTicks = (durSec > 0.0f) ? static_cast<int>(durSec * 60.0f + 0.5f) : 0;
            sm->timeTicks += 1;
            if (durTicks > 0 && sm->timeTicks >= durTicks) {
                sm->timeTicks = 0;
            }
        }
    });
}

} // namespace mye
