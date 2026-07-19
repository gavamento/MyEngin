#pragma once
#include "imgui.h"

namespace mye {

// エディタのキーボードショートカット集約 (M8)。
// 現状は既定割り当て固定。M10 でリバインド (editor_settings 経由) に拡張する。
// enum の順序と下の配列の順序は一致させること。
enum class Shortcut {
    Save,
    Undo,
    Redo,
    Duplicate,
    Delete,
    Focus,
    Rename,
    Copy,
    Cut,
    Paste,
    Count,
};

class ShortcutHub {
public:
    ImGuiKeyChord Chord(Shortcut s) const { return kChords[static_cast<int>(s)]; }
    const char* Label(Shortcut s) const { return kLabels[static_cast<int>(s)]; }

    // このフレームでチョードが押されたか (ImGui のグローバル判定。テキスト入力中は無効)
    bool Pressed(Shortcut s) const { return ImGui::IsKeyChordPressed(Chord(s)); }

private:
    static constexpr ImGuiKeyChord kChords[static_cast<int>(Shortcut::Count)] = {
        ImGuiMod_Ctrl | ImGuiKey_S,           // Save
        ImGuiMod_Ctrl | ImGuiKey_Z,           // Undo
        ImGuiMod_Ctrl | ImGuiKey_Y,           // Redo
        ImGuiMod_Ctrl | ImGuiKey_D,           // Duplicate
        ImGuiKey_Delete,                      // Delete
        ImGuiKey_F,                           // Focus
        ImGuiKey_F2,                          // Rename
        ImGuiMod_Ctrl | ImGuiKey_C,           // Copy
        ImGuiMod_Ctrl | ImGuiKey_X,           // Cut
        ImGuiMod_Ctrl | ImGuiKey_V,           // Paste
    };
    static constexpr const char* kLabels[static_cast<int>(Shortcut::Count)] = {
        "Ctrl+S", "Ctrl+Z", "Ctrl+Y", "Ctrl+D", "Del",
        "F",      "F2",     "Ctrl+C", "Ctrl+X", "Ctrl+V",
    };
};

} // namespace mye
