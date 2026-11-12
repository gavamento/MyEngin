#pragma once
#include <algorithm>
#include <cfloat>
#include <cstdio>

#include "imgui.h"

// プロファイラの各タブが共有する表の部品 (ProfilerWindow*.cpp だけが include する)
namespace mye::profui {

inline constexpr float kZeroMs = 0.0005f; // これ未満は「0 の行」として扱う (表示は小数 3 桁)

inline constexpr ImGuiTableFlags kTableFlags = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg
                                               | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;

inline void MsCell(float ms)
{
    ImGui::Text("%.3f", static_cast<double>(ms));
}

// 「フレームに占める割合」の棒。frameMs が 0 のとき (履歴が空) は空の棒
inline void ShareBar(float ms, float frameMs)
{
    const float fraction = (frameMs > kZeroMs) ? ms / frameMs : 0.0f;
    char overlay[16];
    std::snprintf(overlay, sizeof(overlay), "%.1f%%", static_cast<double>(fraction * 100.0f));
    ImGui::ProgressBar((std::min)(fraction, 1.0f), ImVec2(-FLT_MIN, ImGui::GetTextLineHeight()), overlay);
}

// 「項目 | 値」の 2 列表。BeginValueTable が true のときだけ行を足して EndValueTable を呼ぶ
inline bool BeginValueTable(const char* id, const char* itemHeader, const char* valueHeader)
{
    if (!ImGui::BeginTable(id, 2, kTableFlags)) {
        return false;
    }
    ImGui::TableSetupColumn(itemHeader, ImGuiTableColumnFlags_WidthStretch, 3.0f);
    ImGui::TableSetupColumn(valueHeader, ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableHeadersRow();
    return true;
}

inline void EndValueTable()
{
    ImGui::EndTable();
}

// 行の左列を書いて右列へ移る。値は呼び出し側が続けて ImGui::Text などで書く
inline void ValueRowLabel(const char* label)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(label);
    ImGui::TableSetColumnIndex(1);
}

inline void ValueRowInt(const char* label, long long value)
{
    ValueRowLabel(label);
    ImGui::Text("%lld", value);
}

inline void ValueRowFloat(const char* label, double value, int decimals = 3)
{
    ValueRowLabel(label);
    ImGui::Text("%.*f", decimals, value);
}

inline void ValueRowText(const char* label, const char* value)
{
    ValueRowLabel(label);
    ImGui::TextUnformatted(value);
}

} // namespace mye::profui
