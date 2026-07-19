#include "Engine/Renderer/RenderTypes.h"

#include <algorithm>

namespace mye {

void RenderQueue::Sort()
{
    std::sort(opaque.begin(), opaque.end(), [](const RenderItem& a, const RenderItem& b) {
        if (a.material.value != b.material.value) {
            return a.material.value < b.material.value;
        }
        if (a.mesh.value != b.mesh.value) {
            return a.mesh.value < b.mesh.value;
        }
        return a.viewZ < b.viewZ; // 近い順 (early-Z)
    });

    std::sort(transparent.begin(), transparent.end(), [](const RenderItem& a, const RenderItem& b) {
        if (a.viewZ != b.viewZ) {
            return a.viewZ > b.viewZ; // 遠い順
        }
        if (a.material.value != b.material.value) {
            return a.material.value < b.material.value;
        }
        return a.mesh.value < b.mesh.value;
    });
}

} // namespace mye
