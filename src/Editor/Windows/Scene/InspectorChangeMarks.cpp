//====================================================================================
//                          InspectorChangeMarks.cpp
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          Inspector に出す AI の変更の印 (Field 差分・コンポーネントの印、実装)
//====================================================================================
#include "Editor/Windows/Scene/InspectorChangeMarks.h"

#include <algorithm>
#include <string>

#include "Editor/ChangeTracking/ChangeFormat.h"
#include "Editor/ChangeTracking/SceneChangeOverlay.h"

namespace mye {
namespace {

constexpr float kMarkRadius = 4.0f;

ImU32 ToU32(const OverlayColor& c)
{
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, c.a));
}

// 点を center に描き、その上にマウスがあればツールチップを出す
// hollow = プレハブから伝わった変更 (直接の編集の塗りの丸と区別して輪だけにする)
void DrawDotWithTooltip(const ImVec2& center, ImU32 color, const std::string& tooltip, bool hollow)
{
    if (hollow) {
        ImGui::GetWindowDrawList()->AddCircle(center, kMarkRadius - 0.5f, color, 0, 1.5f);
    } else {
        ImGui::GetWindowDrawList()->AddCircleFilled(center, kMarkRadius, color);
    }
    const ImVec2 hitMin(center.x - kMarkRadius * 1.5f, center.y - kMarkRadius * 1.5f);
    const ImVec2 hitMax(center.x + kMarkRadius * 1.5f, center.y + kMarkRadius * 1.5f);
    if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(hitMin, hitMax) && !tooltip.empty()) {
        ImGui::SetTooltip("%s", tooltip.c_str());
    }
}

} // namespace

void InspectorChangeMarks::SetEntity(uint64_t fileId)
{
    entity_ = nullptr;
    if (overlay_ == nullptr || !overlay_->Show() || fileId == 0) {
        return;
    }
    entity_ = overlay_->Find(fileId);
}

void InspectorChangeMarks::DrawComponentMark(const char* componentName, bool fieldsListedInTooltip, float offsetFromRight)
{
    const OverlayComponent* component = FindOverlayComponent(entity_, componentName);
    if (component == nullptr) {
        return;
    }
    const OverlayColorId id = component->status == OverlayStatus::Added ? OverlayColorId::Added : OverlayColorId::Modified;
    const ImVec2 mn = ImGui::GetItemRectMin();
    const ImVec2 mx = ImGui::GetItemRectMax();
    const ImVec2 center(mx.x - offsetFromRight, (mn.y + mx.y) * 0.5f);
    const bool propagated = component->status == OverlayStatus::Modified && !component->fields.empty()
                            && std::all_of(component->fields.begin(), component->fields.end(), [](const OverlayField& f) { return f.propagated; });
    DrawDotWithTooltip(center, ToU32(overlay_->Palette().Get(id)), ComponentMarkTooltip(*component, fieldsListedInTooltip), propagated);
    ++componentMarks_;
}

void InspectorChangeMarks::DrawFieldMark(const char* componentName, const char* fieldName)
{
    const OverlayField* field = FindOverlayField(entity_, componentName, fieldName);
    if (field == nullptr) {
        return;
    }
    ImGui::SameLine();
    const float size = kMarkRadius * 2.0f;
    ImGui::Dummy(ImVec2(size, size));
    const ImVec2 mn = ImGui::GetItemRectMin();
    const ImVec2 center(mn.x + kMarkRadius, mn.y + ImGui::GetFrameHeight() * 0.5f);
    DrawDotWithTooltip(center, ToU32(overlay_->Palette().Get(OverlayColorId::Modified)), FieldMarkTooltip(*field), field->propagated);
    ++fieldMarks_;
}

} // namespace mye
