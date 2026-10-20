//====================================================================================
//                          Shape2DEditor.cpp
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                          2D 形状 (.shape2d.json) の保存と Inspector 編集 UI
//====================================================================================
#include "Editor/Windows/Scene/Shape2DEditor.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "imgui.h"

#include "Editor/SourceControl/ScmHint.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Localization/Localization.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {

namespace {

constexpr float kPointDragSpeed = 0.01f;

} // namespace

bool SaveShape2D(const std::wstring& path, Shape2D& shape)
{
    ShapeLibrary2D::Sanitize(shape); // 手入力の範囲外をファイルへ焼かない
    std::ofstream out(std::filesystem::path(path), std::ios::binary);
    if (!out) {
        MYE_LOG_ERROR("could not write shape2d: %s", WideToUtf8(path).c_str());
        return false;
    }
    out << ShapeLibrary2D::ToJson(shape).dump(2);
    out.close();
    if (ShapeLibrary2D* lib = shape2d::Library()) {
        lib->LoadFromFile(path);
    }
    scmhint::Changed(path); // バッジと Changes の即時反映
    MYE_LOG_INFO("shape2d saved: %s", WideToUtf8(path).c_str());
    return true;
}

void LoadShape2DEdit(const std::wstring& path, Shape2DEditState& state)
{
    state = Shape2DEditState{};
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
    state.valid = ShapeLibrary2D::FromJson(root, state.shape); // 読み値は Sanitize 済み
}

void DrawShape2DInspector(EngineContext& ctx, const std::wstring& path, Shape2DEditState& state)
{
    (void)ctx;
    if (!state.valid) {
        ImGui::TextDisabled("%s", Tr(StrId::Shape2D_Failed));
        return;
    }
    Shape2D& shape = state.shape;
    ImGui::TextWrapped("%s", Tr(StrId::Shape2D_Usage));
    ImGui::Spacing();

    bool edited = false;
    edited |= ImGui::Checkbox(Tr(StrId::Shape2D_Closed), &shape.closed);
    edited |= ImGui::DragFloat(Tr(StrId::Shape2D_EdgeRadius), &shape.edgeRadius, 0.001f, 0.0001f, 100.0f, "%.4f");
    edited |= ImGui::DragFloat(Tr(StrId::Shape2D_Depth), &shape.depth, 0.01f, 0.001f, 1000.0f, "%.3f");

    char header[64];
    std::snprintf(header, sizeof(header), Tr(StrId::Shape2D_Points), static_cast<int>(shape.points.size()));
    ImGui::SeparatorText(header);
    int removeAt = -1;
    for (int i = 0; i < static_cast<int>(shape.points.size()); ++i) {
        DirectX::XMFLOAT2& p = shape.points[static_cast<size_t>(i)];
        ImGui::PushID(i);
        ImGui::Text("%d", i);
        ImGui::SameLine();
        float xy[2] = { p.x, p.y };
        ImGui::SetNextItemWidth((std::max)(ImGui::GetContentRegionAvail().x - 40.0f, 80.0f));
        if (ImGui::DragFloat2("##pt", xy, kPointDragSpeed)) {
            p.x = xy[0];
            p.y = xy[1];
            edited = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
            removeAt = i;
        }
        ImGui::PopID();
    }
    if (removeAt >= 0) {
        shape.points.erase(shape.points.begin() + removeAt);
        edited = true;
    }
    if (shape.points.size() < kMaxShape2DPoints && ImGui::Button(Tr(StrId::Shape2D_AddPoint))) {
        // 最後の点から少し右へ。何も無ければ原点
        DirectX::XMFLOAT2 next = shape.points.empty() ? DirectX::XMFLOAT2{ 0.0f, 0.0f }
                                                      : DirectX::XMFLOAT2{ shape.points.back().x + 0.5f, shape.points.back().y };
        shape.points.push_back(next);
        edited = true;
    }
    if (edited) {
        shape.RebuildDerived();
    }

    ImGui::Separator();
    if (ImGui::Button(Tr(StrId::Common_Save), ImVec2(90, 0))) {
        SaveShape2D(path, shape);
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr(StrId::Insp_Revert), ImVec2(90, 0))) {
        LoadShape2DEdit(path, state);
    }
}

} // namespace mye
