//====================================================================================
//                          CurveEditorWidget.cpp
//  MyEngin/ 秋田蓮音                                                       11/04/2026
//                                          MyeCurve (Shared/FeelMath.h) の編集部品 (M94d / M94e、ADR-031) の実装
//====================================================================================
#include "Editor/Widgets/CurveEditorWidget.h"

#include <algorithm>
#include <cmath>

#include "imgui.h"

#include "Engine/Core/Localization/Localization.h"

namespace mye {

namespace {

constexpr int kPlotSamples = 64;
constexpr float kGraphHeight = 90.0f;

bool sActivated = false;
bool sDeactivated = false;

// 1 個の widget の始まり / 終わりを、この部品全体の「編集の区切り」に畳む
void TrackItemEdges()
{
    sActivated = sActivated || ImGui::IsItemActivated();
    sDeactivated = sDeactivated || ImGui::IsItemDeactivatedAfterEdit();
}

// グラフの値の範囲 (カーブの値 + 少しの余白)。0 と 1 は常に入れる (基準線が見える)
void CurveRange(const MyeCurve& c, float& lo, float& hi)
{
    lo = 0.0f;
    hi = 1.0f;
    for (int i = 0; i <= kPlotSamples; ++i) {
        const float v = MyeEvalCurve(c, static_cast<float>(i) / static_cast<float>(kPlotSamples));
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    const float pad = (hi - lo) * 0.08f + 1e-3f;
    lo -= pad;
    hi += pad;
}

// 時間 [0,1] → 値のグラフ。戻り値はグラフの矩形 (custom の制御点の編集が同じ座標系を使う)
void DrawGraph(const MyeCurve& c, float lo, float hi, ImVec2& outMin, ImVec2& outMax)
{
    const float width = std::max(ImGui::GetContentRegionAvail().x, 120.0f);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1 = ImVec2(p0.x + width, p0.y + kGraphHeight);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ImGuiCol_FrameBg));
    const auto toScreen = [&](float t, float v) {
        const float y = (v - lo) / (hi - lo);
        return ImVec2(p0.x + t * width, p1.y - y * kGraphHeight);
    };
    // 基準線 (0 と 1)
    const ImU32 guide = ImGui::GetColorU32(ImGuiCol_Border);
    dl->AddLine(toScreen(0.0f, 0.0f), toScreen(1.0f, 0.0f), guide);
    dl->AddLine(toScreen(0.0f, 1.0f), toScreen(1.0f, 1.0f), guide);
    ImVec2 pts[kPlotSamples + 1];
    for (int i = 0; i <= kPlotSamples; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kPlotSamples);
        pts[i] = toScreen(t, MyeEvalCurve(c, t));
    }
    dl->AddPolyline(pts, kPlotSamples + 1, ImGui::GetColorU32(ImGuiCol_PlotLines), ImDrawFlags_None, 2.0f);
    outMin = p0;
    outMax = p1;
}

} // namespace

bool CurveEditActivated()
{
    return sActivated;
}

bool CurveEditDeactivated()
{
    return sDeactivated;
}

bool DrawCurveEditor(const char* id, MyeCurve& c)
{
    sActivated = false;
    sDeactivated = false;
    bool changed = false;
    ImGui::PushID(id);

    int mode = c.mode == MYE_CURVE_CUSTOM ? 1 : 0;
    const char* modes[2] = { Tr(StrId::Feel_CurvePreset), Tr(StrId::Feel_CurveCustom) };
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::Combo(Tr(StrId::Feel_CurveMode), &mode, modes, 2)) {
        if (mode == 1 && c.mode != MYE_CURVE_CUSTOM) {
            // preset → custom: 今の形を 3 点で近似した制御点から始める (白紙から描かせない)
            c.mode = MYE_CURVE_CUSTOM;
            c.keyCount = 3;
            for (int i = 0; i < 3; ++i) {
                const float t = static_cast<float>(i) * 0.5f;
                MyeCurve preset = c;
                preset.mode = MYE_CURVE_PRESET;
                c.keys[i] = { t, MyeEvalCurve(preset, t), 0.0f, 0.0f };
            }
        } else if (mode == 0) {
            c.mode = MYE_CURVE_PRESET;
        }
        sActivated = true;
        sDeactivated = true;
        changed = true;
    }

    if (c.mode != MYE_CURVE_CUSTOM) {
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::BeginCombo(Tr(StrId::Feel_CurveEase), MyeEaseName(c.ease))) {
            for (int32_t e = 0; e < MYE_EASE_COUNT; ++e) {
                const bool selected = e == c.ease;
                if (ImGui::Selectable(MyeEaseName(e), selected)) {
                    c.ease = e;
                    changed = true;
                    sActivated = true;
                    sDeactivated = true;
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SetNextItemWidth(160.0f);
        changed |= ImGui::DragFloat(Tr(StrId::Feel_CurveFrom), &c.from, 0.01f, -1000.0f, 1000.0f, "%.3f");
        TrackItemEdges();
        ImGui::SetNextItemWidth(160.0f);
        changed |= ImGui::DragFloat(Tr(StrId::Feel_CurveTo), &c.to, 0.01f, -1000.0f, 1000.0f, "%.3f");
        TrackItemEdges();
        if (c.ease == MYE_EASE_IN_BACK || c.ease == MYE_EASE_OUT_BACK || c.ease == MYE_EASE_INOUT_BACK) {
            float overshoot = c.param > 0.0f ? c.param : MYE_EASE_BACK_DEFAULT;
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::DragFloat(Tr(StrId::Feel_CurveParam), &overshoot, 0.01f, 0.0f, 10.0f, "%.3f")) {
                c.param = overshoot;
                changed = true;
            }
            TrackItemEdges();
        }
    }

    float lo = 0.0f;
    float hi = 1.0f;
    CurveRange(c, lo, hi);
    ImVec2 gMin;
    ImVec2 gMax;
    DrawGraph(c, lo, hi, gMin, gMax);
    ImGui::Dummy(ImVec2(gMax.x - gMin.x, gMax.y - gMin.y));

    ImGui::PopID();
    return changed;
}

} // namespace mye
