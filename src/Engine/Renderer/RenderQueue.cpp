#include "Engine/Renderer/RenderTypes.h"

#include <algorithm>
#include <cmath>

namespace mye {
namespace {

bool ViewZLess(float a, float b)
{
    const bool aNan = std::isnan(a);
    const bool bNan = std::isnan(b);
    if (aNan != bNan) {
        return !aNan; // 壊れた深度は有限値の後ろへ隔離する
    }
    return !aNan && a < b;
}

bool ViewZGreater(float a, float b)
{
    const bool aNan = std::isnan(a);
    const bool bNan = std::isnan(b);
    if (aNan != bNan) {
        return !aNan; // 降順でも NaN は有限値の後ろ
    }
    return !aNan && a > b;
}

bool EntityLess(EntityID a, EntityID b)
{
    if (a.index != b.index) {
        return a.index < b.index;
    }
    return a.generation < b.generation;
}

} // namespace

void RenderQueue::Sort()
{
    std::sort(opaque.begin(), opaque.end(), [](const RenderItem& a, const RenderItem& b) {
        if (a.material.value != b.material.value) {
            return a.material.value < b.material.value;
        }
        if (a.mesh.value != b.mesh.value) {
            return a.mesh.value < b.mesh.value;
        }
        if (a.viewZ != b.viewZ && (ViewZLess(a.viewZ, b.viewZ) || ViewZLess(b.viewZ, a.viewZ))) {
            return ViewZLess(a.viewZ, b.viewZ); // 近い順 (early-Z)、NaN は末尾
        }
        return EntityLess(a.entity, b.entity);
    });

    std::sort(transparent.begin(), transparent.end(), [](const RenderItem& a, const RenderItem& b) {
        if (a.viewZ != b.viewZ
            && (ViewZGreater(a.viewZ, b.viewZ) || ViewZGreater(b.viewZ, a.viewZ))) {
            return ViewZGreater(a.viewZ, b.viewZ); // 遠い順、NaN は末尾
        }
        if (a.material.value != b.material.value) {
            return a.material.value < b.material.value;
        }
        if (a.mesh.value != b.mesh.value) {
            return a.mesh.value < b.mesh.value;
        }
        return EntityLess(a.entity, b.entity);
    });
}

} // namespace mye
