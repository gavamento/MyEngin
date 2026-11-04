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
    ImGui::Dummy(ImVec2(width, kGraphHeight)); // グラフの場所を確保 (この後に制御点の当たり判定を重ねる)
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
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID selKey = ImGui::GetID("##selectedKey");
    const ImGuiID loKey = ImGui::GetID("##dragLo");
    const ImGuiID hiKey = ImGui::GetID("##dragHi");

    int mode = c.mode == MYE_CURVE_CUSTOM ? 1 : 0;
    const char* modes[2] = { Tr(StrId::Feel_CurvePreset), Tr(StrId::Feel_CurveCustom) };
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::Combo(Tr(StrId::Feel_CurveMode), &mode, modes, 2)) {
        if (mode == 1 && c.mode != MYE_CURVE_CUSTOM) {
            // preset → custom: 今の形を 3 点で写した制御点から始める (白紙から描かせない)。
            // preset の欄 (ease / from / to) は残るので、preset へ戻せば元の形に戻る
            MyeCurve preset = c;
            c.mode = MYE_CURVE_CUSTOM;
            c.keyCount = 3;
            for (int i = 0; i < 3; ++i) {
                const float t = static_cast<float>(i) * 0.5f;
                c.keys[i] = { t, MyeEvalCurve(preset, t), 0.0f, 0.0f };
            }
            MyeAutoTangents(c);
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

    // グラフの値の範囲。制御点のドラッグ中は始めた時点の範囲で固定する
    // (範囲が点に追従すると、点の画面位置がマウスから逃げて値が暴走する)
    float lo = 0.0f;
    float hi = 1.0f;
    const bool draggingKey = storage->GetBool(ImGui::GetID("##dragging"), false);
    if (draggingKey) {
        lo = storage->GetFloat(loKey, 0.0f);
        hi = storage->GetFloat(hiKey, 1.0f);
    } else {
        CurveRange(c, lo, hi);
    }
    ImVec2 gMin;
    ImVec2 gMax;
    DrawGraph(c, lo, hi, gMin, gMax);
    const ImVec2 graphCursor = ImGui::GetCursorScreenPos();

    if (c.mode == MYE_CURVE_CUSTOM) {
        const float w = gMax.x - gMin.x;
        const float h = gMax.y - gMin.y;
        const auto toScreen = [&](float t, float v) {
            return ImVec2(gMin.x + t * w, gMax.y - (v - lo) / (hi - lo) * h);
        };
        const auto fromScreen = [&](ImVec2 p, float& t, float& v) {
            t = std::clamp((p.x - gMin.x) / w, 0.0f, 1.0f);
            v = lo + (gMax.y - p.y) / h * (hi - lo);
        };
        int selected = storage->GetInt(selKey, -1);
        if (selected >= c.keyCount) {
            selected = -1;
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        constexpr float kHandle = 6.0f;
        bool anyKeyHovered = false;
        bool anyKeyActive = false;
        int removeIndex = -1;
        for (int i = 0; i < c.keyCount; ++i) {
            MyeCurveKey& k = c.keys[i];
            const ImVec2 sp = toScreen(k.t, k.value);
            ImGui::SetCursorScreenPos(ImVec2(sp.x - kHandle, sp.y - kHandle));
            ImGui::PushID(i);
            ImGui::InvisibleButton("key", ImVec2(kHandle * 2.0f, kHandle * 2.0f));
            ImGui::PopID();
            const bool hovered = ImGui::IsItemHovered();
            const bool active = ImGui::IsItemActive();
            anyKeyHovered = anyKeyHovered || hovered;
            anyKeyActive = anyKeyActive || active;
            if (ImGui::IsItemActivated()) {
                selected = i;
                sActivated = true;
                storage->SetFloat(loKey, lo);
                storage->SetFloat(hiKey, hi);
            }
            if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
                float t = 0.0f;
                float v = 0.0f;
                fromScreen(ImGui::GetIO().MousePos, t, v);
                // 隣の点を追い越さない (並びが入れ替わると、ドラッグ中に掴んでいる点がすり替わる)
                const float tMin = i > 0 ? c.keys[i - 1].t : 0.0f;
                const float tMax = i + 1 < c.keyCount ? c.keys[i + 1].t : 1.0f;
                k.t = std::clamp(t, tMin, tMax);
                k.value = v;
                changed = true;
            }
            if (ImGui::IsItemDeactivated()) {
                sDeactivated = true;
            }
            if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && c.keyCount > 1) {
                removeIndex = i;
            }
            const ImU32 col = ImGui::GetColorU32((i == selected || hovered) ? ImGuiCol_PlotLinesHovered : ImGuiCol_Text);
            dl->AddCircleFilled(sp, (i == selected) ? kHandle : kHandle - 1.5f, col);
        }
        storage->SetBool(ImGui::GetID("##dragging"), anyKeyActive);
        if (removeIndex >= 0) {
            for (int i = removeIndex; i + 1 < c.keyCount; ++i) {
                c.keys[i] = c.keys[i + 1];
            }
            --c.keyCount;
            c.keys[c.keyCount] = MyeCurveKey{};
            selected = -1;
            changed = true;
            sActivated = true;
            sDeactivated = true;
        }
        // 何も無いところをダブルクリック = 点を足す (t の昇順の位置へ差し込む)
        if (!anyKeyHovered && c.keyCount < MYE_CURVE_MAX_KEYS && ImGui::IsWindowHovered()
            && ImGui::IsMouseHoveringRect(gMin, gMax) && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            float t = 0.0f;
            float v = 0.0f;
            fromScreen(ImGui::GetIO().MousePos, t, v);
            int at = c.keyCount;
            for (int i = 0; i < c.keyCount; ++i) {
                if (c.keys[i].t > t) {
                    at = i;
                    break;
                }
            }
            for (int i = c.keyCount; i > at; --i) {
                c.keys[i] = c.keys[i - 1];
            }
            c.keys[at] = { t, v, 0.0f, 0.0f };
            ++c.keyCount;
            selected = at;
            changed = true;
            sActivated = true;
            sDeactivated = true;
        }
        storage->SetInt(selKey, selected);
        ImGui::SetCursorScreenPos(graphCursor);
        ImGui::Dummy(ImVec2(0.0f, 0.0f));

        // 選んだ点の数値 (t / 値 / 入出の接線)
        if (selected >= 0 && selected < c.keyCount) {
            MyeCurveKey& k = c.keys[selected];
            float tv[2] = { k.t, k.value };
            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::DragFloat2(Tr(StrId::Feel_CurveKey), tv, 0.005f, -1000.0f, 1000.0f, "%.3f")) {
                const float tMin = selected > 0 ? c.keys[selected - 1].t : 0.0f;
                const float tMax = selected + 1 < c.keyCount ? c.keys[selected + 1].t : 1.0f;
                k.t = std::clamp(tv[0], tMin, tMax);
                k.value = tv[1];
                changed = true;
            }
            TrackItemEdges();
            float tan[2] = { k.inTangent, k.outTangent };
            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::DragFloat2(Tr(StrId::Feel_CurveTangents), tan, 0.02f, -10000.0f, 10000.0f, "%.2f")) {
                k.inTangent = tan[0];
                k.outTangent = tan[1];
                changed = true;
            }
            TrackItemEdges();
        }
        if (ImGui::SmallButton(Tr(StrId::Feel_CurveAutoTangents))) {
            MyeAutoTangents(c);
            changed = true;
            sActivated = true;
            sDeactivated = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", Tr(StrId::Feel_CurveHelp));
    } else {
        ImGui::SetCursorScreenPos(graphCursor);
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
    }

    ImGui::PopID();
    return changed;
}

} // namespace mye
