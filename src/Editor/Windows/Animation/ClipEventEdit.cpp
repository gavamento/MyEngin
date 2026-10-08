//====================================================================================
//                          ClipEventEdit.cpp
//  MyEngin/ 秋田蓮音                                                       10/09/2026
//                                  アニメイベントの編集 (純関数、M89o)
//====================================================================================
#include "Editor/Windows/Animation/ClipEventEdit.h"

#include <algorithm>

#include "Engine/Core/Util/Hash.h"

namespace mye {

ControllerClipEvents* FindClipEventsMutable(ControllerAsset& ctrl, uint64_t clipHash)
{
    for (ControllerClipEvents& ce : ctrl.clipEvents) {
        if (ce.clipHash == clipHash) {
            return &ce;
        }
    }
    return nullptr;
}

int32_t AddClipEvent(ControllerAsset& ctrl, const std::string& clip, int32_t tick)
{
    const uint64_t clipHash = HashStr(clip);
    ControllerClipEvents* ce = FindClipEventsMutable(ctrl, clipHash);
    if (ce == nullptr) {
        ControllerClipEvents added;
        added.clip = clip;
        added.clipHash = clipHash;
        ctrl.clipEvents.push_back(std::move(added));
        ce = &ctrl.clipEvents.back();
    }
    ControllerClipEvent ev;
    SetClipEventName(ev, "Event");
    ev.tick = std::max(tick, 0);
    ce->events.push_back(std::move(ev));
    return static_cast<int32_t>(ce->events.size()) - 1;
}

void RemoveClipEvent(ControllerAsset& ctrl, uint64_t clipHash, int32_t index)
{
    for (size_t i = 0; i < ctrl.clipEvents.size(); ++i) {
        ControllerClipEvents& ce = ctrl.clipEvents[i];
        if (ce.clipHash != clipHash) {
            continue;
        }
        if (index < 0 || index >= static_cast<int32_t>(ce.events.size())) {
            return;
        }
        ce.events.erase(ce.events.begin() + index);
        if (ce.events.empty()) {
            ctrl.clipEvents.erase(ctrl.clipEvents.begin() + static_cast<std::ptrdiff_t>(i));
        }
        return;
    }
}

void SetClipEventName(ControllerClipEvent& ev, const std::string& name)
{
    ev.name = name;
    ev.nameHash = HashStr(name);
}

void SetClipEventAsset(ControllerClipEvent& ev, const std::string& asset)
{
    ev.asset = asset;
    ev.assetHash = HashStr(asset);
}

void SetClipEventKind(ControllerClipEvent& ev, ClipEventKind kind)
{
    if (ev.kind == kind) {
        return;
    }
    ev.kind = kind;
    SetClipEventAsset(ev, std::string());
}

int32_t TimelineTickToClipTick(int32_t timelineTick, int32_t timelineTicks, int32_t clipTicks)
{
    if (timelineTicks <= 0 || clipTicks <= 0) {
        return 0;
    }
    const int64_t scaled = (static_cast<int64_t>(timelineTick) * clipTicks * 2 + timelineTicks) / (2 * static_cast<int64_t>(timelineTicks));
    return static_cast<int32_t>(std::clamp<int64_t>(scaled, 0, clipTicks));
}

} // namespace mye
