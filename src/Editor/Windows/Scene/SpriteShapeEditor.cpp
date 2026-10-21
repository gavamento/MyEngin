//====================================================================================
//                          SpriteShapeEditor.cpp
//  MyEngin/ 秋田蓮音                                                       10/22/2026
//                                  スプライトシェイプのプロファイル (.spriteshape.json) の保存と Inspector 編集 UI
//====================================================================================
#include "Editor/Windows/Scene/SpriteShapeEditor.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "imgui.h"

#include "Editor/SourceControl/ScmHint.h"
#include "Editor/Windows/Animation/SpriteAnimEditor.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Localization/Localization.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {

namespace {

constexpr float kAngleDragSpeed = 0.5f;
constexpr float kHeightDragSpeed = 0.01f;
constexpr float kAngleFieldWidth = 70.0f;
// 角度の入力範囲 (180 度をまたぐ範囲を書けるよう、±360 まで)
constexpr float kAngleLimit = 360.0f;

} // namespace

bool SaveSpriteShape(const std::wstring& path, SpriteShapeProfile& profile)
{
    SpriteShapeLibrary::Sanitize(profile); // 手入力の範囲外をファイルへ焼かない
    std::ofstream out(std::filesystem::path(path), std::ios::binary);
    if (!out) {
        MYE_LOG_ERROR("could not write spriteshape: %s", WideToUtf8(path).c_str());
        return false;
    }
    out << SpriteShapeLibrary::ToJson(profile).dump(2);
    out.close();
    if (SpriteShapeLibrary* lib = spriteshape::Library()) {
        lib->LoadFromFile(path);
    }
    scmhint::Changed(path); // バッジと Changes の即時反映
    MYE_LOG_INFO("spriteshape saved: %s", WideToUtf8(path).c_str());
    return true;
}

void LoadSpriteShapeEdit(const std::wstring& path, SpriteShapeEditState& state)
{
    state = SpriteShapeEditState{};
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    if (!f) {
        return;
    }
    nlohmann::json root;
    try {
        f >> root;
    } catch (const nlohmann::json::exception&) {
        return;
    }
    state.valid = SpriteShapeLibrary::FromJson(root, state.profile); // 読み値は Sanitize 済み
}

void DrawSpriteShapeInspector(EngineContext& ctx, const std::wstring& path, SpriteShapeEditState& state)
{
    (void)ctx;
    if (!state.valid) {
        ImGui::TextDisabled("%s", Tr(StrId::SpriteShape_Failed));
        return;
    }
    SpriteShapeProfile& profile = state.profile;
    ImGui::TextWrapped("%s", Tr(StrId::SpriteShape_Usage));
    ImGui::Spacing();

    ImGui::TextUnformatted(Tr(StrId::SpriteShape_Fill));
    AssetID fill{ profile.fill };
    if (DrawSpritePicker("fill", fill, -1.0f)) {
        profile.fill = fill.value;
    }
    ImGui::DragFloat(Tr(StrId::SpriteShape_Height), &profile.height, kHeightDragSpeed, kMinSpriteShapeHeight,
                     kMaxSpriteShapeHeight, "%.3f");

    char header[64];
    std::snprintf(header, sizeof(header), Tr(StrId::SpriteShape_Edges), static_cast<int>(profile.edges.size()));
    ImGui::SeparatorText(header);
    int removeAt = -1;
    int swapFrom = -1;
    int swapWith = -1;
    for (int i = 0; i < static_cast<int>(profile.edges.size()); ++i) {
        SpriteShapeEdge& e = profile.edges[static_cast<size_t>(i)];
        ImGui::PushID(i);
        ImGui::Text("%d", i);
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::SetNextItemWidth(kAngleFieldWidth);
        ImGui::DragFloat(Tr(StrId::SpriteShape_AngleFrom), &e.angleFrom, kAngleDragSpeed, -kAngleLimit, kAngleLimit,
                         "%.0f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(kAngleFieldWidth);
        ImGui::DragFloat(Tr(StrId::SpriteShape_AngleTo), &e.angleTo, kAngleDragSpeed, -kAngleLimit, kAngleLimit,
                         "%.0f");
        AssetID picked{ e.sprite };
        if (DrawSpritePicker("edge", picked, -1.0f)) {
            e.sprite = picked.value;
        }
        ImGui::EndGroup();
        ImGui::SameLine();
        if (ImGui::SmallButton("^") && i > 0) {
            swapFrom = i;
            swapWith = i - 1;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("v") && i + 1 < static_cast<int>(profile.edges.size())) {
            swapFrom = i;
            swapWith = i + 1;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
            removeAt = i;
        }
        ImGui::PopID();
    }
    if (swapFrom >= 0) {
        std::swap(profile.edges[static_cast<size_t>(swapFrom)], profile.edges[static_cast<size_t>(swapWith)]);
    }
    if (removeAt >= 0) {
        profile.edges.erase(profile.edges.begin() + removeAt);
    }
    if (profile.edges.size() < kMaxSpriteShapeEdges && ImGui::Button(Tr(StrId::SpriteShape_AddEdge))) {
        profile.edges.push_back(SpriteShapeEdge{});
    }
    // 編集内容は保存ボタンで確定する (描画側はライブラリの値を引くので、保存までシーンには出ない)

    ImGui::Separator();
    if (ImGui::Button(Tr(StrId::Common_Save), ImVec2(90, 0))) {
        SaveSpriteShape(path, profile);
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr(StrId::Insp_Revert), ImVec2(90, 0))) {
        LoadSpriteShapeEdit(path, state);
    }
}

} // namespace mye
