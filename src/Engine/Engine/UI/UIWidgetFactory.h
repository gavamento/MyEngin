//====================================================================================
//                          UIWidgetFactory.h
//  MyEngine/ 秋田蓮音                                                      09/13/2026
//                                          子構成込みのウィジェット（Toggle / Slider）を組み立てる
//====================================================================================
#pragma once
// Create > UI > Toggle / Slider が作る子構成の**正本** (M75f)。エディタの生成メニュー / --ui-demo /
// UISelfTest のプレハブ往復がこの 1 本を通る — 構成を 2 か所に書くと、EntityRef (graphic / fillRect /
// handleRect / targetGraphic) の張り方が片方だけずれる。
// 形は Unity の既定のウィジェットに合わせ、寸法だけこのエンジンの既定 (Create > UI の 160x40) に寄せた。
// 根は中央アンカー・中央 pivot (AddUnityStyleRect と同じ)。どれも CreateGameObjectTracked で作る
// (Undo の同一性キーとプレハブ化に fileId が要る)
#include "Engine/Engine/GameObject.h"

namespace mye {

class Scene;

namespace uiwidgets {

// Toggle (160x40)
//   └ Background (UIElement。Selectable.targetGraphic)
//       └ Checkmark (UIElement。Toggle.graphic)
//   └ Label (UIElement kind 1、左寄せ)
// isOn の既定は 1 (Unity と同じ)。labelScale は Label の fontScale
GameObject CreateToggle(Scene& scene, const char* name, const char* label, float labelScale = 1.0f);

// Slider (横 160x40 / 縦 40x160)
//   └ Background (UIElement。軸と直交する向きに 40% の帯)
//   └ Fill Area (RectTransform だけ。値の始点側 5・終点側 15 の余白)
//       └ Fill (UIElement。Slider.fillRect)
//   └ Handle Slide Area (RectTransform だけ。軸の両端に 10 の余白)
//       └ Handle (UIElement。Slider.handleRect / Selectable.targetGraphic)
// direction は uiwidgets::kSlider*。value の既定は 0 (Unity と同じ)
GameObject CreateSlider(Scene& scene, const char* name, int direction);

} // namespace uiwidgets
} // namespace mye
