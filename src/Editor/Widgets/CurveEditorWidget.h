//====================================================================================
//                          CurveEditorWidget.h
//  MyEngin/ 秋田蓮音                                                       11/04/2026
//                                          MyeCurve (Shared/FeelMath.h) の編集部品 (M94d / M94e、ADR-031)
//====================================================================================
#pragma once

#include "Shared/FeelMath.h"

namespace mye {

// MyeCurve を 1 個編集する。方式 (preset / custom) の切り替え、preset の種類と数値、
// 時間 [0,1] → 値のグラフ (custom では制御点をドラッグ・追加・削除できる) を出す。
// id は ImGui の ID の接頭辞 (同じウィンドウに複数並べるため)。
// 戻り値: このフレームで値が変わったか。編集の始まり / 終わり (Undo の区切り) は
// CurveEditActivated / CurveEditDeactivated が返す
bool DrawCurveEditor(const char* id, MyeCurve& curve);

// 直前の DrawCurveEditor で、ドラッグ等の連続した編集が始まった / 終わったか (Undo を 1 件に束ねるため)
bool CurveEditActivated();
bool CurveEditDeactivated();

} // namespace mye
