//====================================================================================
//                          ControllerStateEdit.cpp
//  MyEngin/ 秋田蓮音                                                       10/08/2026
//                                  コントローラ窓のステート編集 (純関数)
//====================================================================================
#include "Editor/Windows/Animation/ControllerStateEdit.h"

#include "Engine/Core/Util/Hash.h"
#include "Engine/Renderer/Mesh/Skeleton.h"

namespace mye {

StateSkelKind GetStateSkelKind(const ControllerState& state)
{
    switch (state.blendType) {
    case ControllerBlendType::Blend1D:
        return StateSkelKind::Blend1D;
    case ControllerBlendType::Blend2D:
        return StateSkelKind::Blend2D;
    case ControllerBlendType::None:
        break;
    }
    return state.skelClip.empty() ? StateSkelKind::None : StateSkelKind::Clip;
}

void SetStateSkelClip(ControllerState& state, const std::string& clip)
{
    state.skelClip = clip;
    state.skelClipHash = clip.empty() ? 0 : HashStr(clip);
}

void SetBlendChildClip(ControllerBlendChild& child, const std::string& clip)
{
    child.clip = clip;
    child.clipHash = clip.empty() ? 0 : HashStr(clip);
}

void AddBlendChild(ControllerState& state, const std::string& clip)
{
    ControllerBlendChild child;
    SetBlendChildClip(child, clip);
    if (!state.blendChildren.empty()) {
        const ControllerBlendChild& last = state.blendChildren.back();
        child.threshold = last.threshold + 1.0f;
        child.posX = last.posX + 1.0f;
        child.posY = last.posY;
    }
    state.blendChildren.push_back(child);
}

void SetStateSkelKind(ControllerState& state, StateSkelKind kind, const std::string& fallbackClip)
{
    switch (kind) {
    case StateSkelKind::None:
        state.blendType = ControllerBlendType::None;
        SetStateSkelClip(state, std::string());
        return;
    case StateSkelKind::Clip:
        state.blendType = ControllerBlendType::None;
        if (state.skelClip.empty()) {
            std::string clip = fallbackClip;
            for (const ControllerBlendChild& child : state.blendChildren) {
                if (!child.clip.empty()) {
                    clip = child.clip;
                    break;
                }
            }
            SetStateSkelClip(state, clip);
        }
        return;
    case StateSkelKind::Blend1D:
    case StateSkelKind::Blend2D:
        state.blendType = kind == StateSkelKind::Blend1D ? ControllerBlendType::Blend1D : ControllerBlendType::Blend2D;
        if (state.blendChildren.empty()) {
            AddBlendChild(state, state.skelClip.empty() ? fallbackClip : state.skelClip);
        }
        return;
    }
}

std::vector<std::string> NamedSkeletalClips(const SkinnedModel* model)
{
    std::vector<std::string> names;
    if (model == nullptr) {
        return names;
    }
    for (const SkeletalClip& clip : model->clips) {
        if (!clip.name.empty()) {
            names.push_back(clip.name);
        }
    }
    return names;
}

} // namespace mye
