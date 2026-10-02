//====================================================================================
//                          UIWidgetFactory.h
//  MyEngine/ 秋田蓮音                                                      09/13/2026
//                                          子構成込みのウィジェット（Toggle / Slider / Scroll / Dropdown）を組み立てる
//====================================================================================
#pragma once
// Create > UI > Toggle / Slider (M75f) / Scrollbar / Scroll View / Dropdown (M75g) が作る子構成の**正本**。
// エディタの生成メニュー / --ui-demo / UISelfTest のプレハブ往復がこの 1 本を通る — 構成を 2 か所に書くと、
// EntityRef (graphic / fillRect / handleRect / targetGraphic / content / templateRect ...) の張り方が片方だけずれる。
// 形は Unity の既定のウィジェットに合わせ、寸法だけこのエンジンの既定 (Create > UI の 160x40) に寄せた。
// 根は中央アンカー・中央 pivot (AddUnityStyleRect と同じ)。どれも CreateGameObjectTracked で作る
// (Undo の同一性キーとプレハブ化に fileId が要る)
#include "Engine/Engine/Scene/GameObject.h"

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

// ---- M75g ----
// Scrollbar (横 160x20 / 縦 20x160)
//   (根が UIElement の溝)
//   └ Sliding Area (RectTransform だけ。縦横とも 10 内側)
//       └ Handle (UIElement。Scrollbar.handleRect / Selectable.targetGraphic。sizeDelta 20 で溝の端まで届く)
// direction は uiwidgets::kSlider* (Scrollbar も同じ並び)。size の既定は 0.2 (Unity と同じ)
GameObject CreateScrollbar(Scene& scene, const char* name, int direction);

// Scroll View (400x300)
//   (根が UIElement の背景 + UIScrollRect)
//   └ Viewport (UIElement。clipChildren。右と下に 20 のスクロールバーの幅を空ける)
//       └ Content (RectTransform だけ。上辺に沿って横いっぱい、高さ 600 = 見える範囲の 2 倍強)
//   └ Scrollbar Horizontal (下辺、左→右) / Scrollbar Vertical (右辺、下→上)
// Unity の既定と違い、スクロールバーは常に表示 (AutoHide / AutoHideAndExpandViewport は無い)
GameObject CreateScrollView(Scene& scene, const char* name);

// Dropdown (160x40)
//   (根が UIElement の背景 + UISelectable + UIDropdown)
//   └ Label (UIElement kind 1。Dropdown.captionText)
//   └ Arrow (UIElement)
//   └ Template (UIElement + UIScrollRect (縦だけ・Clamped)。Dropdown.templateRect。根の下辺から下へ伸びる)
//       └ Viewport (UIElement。clipChildren)
//           └ Content (縦の Layout Group + ContentSizeFitter)
//               └ Item 0..7 (UIElement + UISelectable + UIDropdownItem)
//                   └ Item Checkmark / Item Label
//       └ Scrollbar (縦、下→上)
// 項目は kDropdownMaxOptions 個を常設で作り、選択肢の数を越えた分は描かれない (UIDropdownComponent)。
// 選択肢の既定は Unity と同じ "Option A" / "Option B" / "Option C"。labelScale は表題と項目の文字の fontScale
GameObject CreateDropdown(Scene& scene, const char* name, float labelScale = 1.0f);

} // namespace uiwidgets
} // namespace mye
