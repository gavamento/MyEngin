//====================================================================================
//                          UIWidgets.h
//  MyEngine/ 秋田蓮音                                                      09/13/2026
//                                          UI ウィジェット（Selectable / Toggle / Slider）の規則と値の更新
//====================================================================================
#pragma once
// ゲーム内 UI のウィジェット (M75f)。Unity uGUI の Selectable / Toggle / ToggleGroup / Slider の意味論を、
// 「規則は純関数 / 値はハッシュ対象のコンポーネント」の形で持つ。
//
// 分担:
//   - uiinteract::Evaluate (UIInteraction.cpp) … hovered / pressed / clicked / focused。泡立ち
//     (BubbleTarget)・interactable・押したウィジェットへのフォーカス・Navigation のモードはここ
//   - uiwidgets::Update (このファイル) … Toggle.isOn / Slider.value を書き、changed を立てる。
//     **simulateScripts の tick だけ** TickRunner が呼ぶ (エディタの編集中にゲーム面をクリックしても
//     シーンのデータが Undo の外で書き換わらない。スクリプトと同じ門を通るので巻き戻しとも噛み合う)
//   - uilayout::Resolve … Slider の fill / handle のアンカーを SliderDrivenTransform から導く
//     (Layout Group と同じく子の RectTransform には書かない)
//   - UIRenderer … CollectVisualOverrides で Selectable の色 / 画像と Toggle のチェックマークの表示
//
// ★sim レーン (Evaluate / Update / Resolve) から呼ばれる関数は scalar の四則演算と floor / fmod だけで組む。
//   unordered も使わない (規則 7)。描画用の CollectVisualOverrides だけは決定論の対象外
#include <cstdint>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/EntityID.h"

namespace mye {

class World;
struct InputSnapshot;
struct RectTransformComponent;
struct UISelectableComponent;
struct UISliderComponent;
struct UIInteractionState;

namespace uiinteract {
struct TickEvents;
}

namespace uiwidgets {

// UISelectableComponent.transition (Unity の Selectable.Transition。Animation は無い)
inline constexpr int kTransitionNone = 0;
inline constexpr int kTransitionColorTint = 1;
inline constexpr int kTransitionSpriteSwap = 2;

// UISelectableComponent.navigationMode (Unity の Navigation.Mode と同じ値。1 / 2 はビットとして読む)
inline constexpr int kNavNone = 0;
inline constexpr int kNavHorizontal = 1;
inline constexpr int kNavVertical = 2;
inline constexpr int kNavAutomatic = 3;
inline constexpr int kNavExplicit = 4;

// UISliderComponent.direction (Unity の Slider.Direction と同じ並び)
inline constexpr int kSliderLeftToRight = 0;
inline constexpr int kSliderRightToLeft = 1;
inline constexpr int kSliderBottomToTop = 2;
inline constexpr int kSliderTopToBottom = 3;

// Unity の Selectable.SelectionState と同じ優先順位 (Disabled > Pressed > Selected > Highlighted > Normal)
enum SelectionState : int {
    kStateNormal = 0,
    kStateHighlighted = 1,
    kStatePressed = 2,
    kStateSelected = 3,
    kStateDisabled = 4,
};

// ---- Selectable ----
// ウィジェットの根か (UISelectable / UIToggle / UISlider のどれかを持つ)。押下の泡立ちが止まる所
bool IsWidgetRoot(World& world, EntityID e);
// e の UISelectable。持っていなければ既定値 (操作可能・色の遷移・自動ナビ) を返す — Toggle / Slider は
// Selectable を付け忘れても押せる。**返る参照は構造変更で無効になる** (その場で読むこと)
const UISelectableComponent& SelectableOf(World& world, EntityID e);
bool IsInteractable(World& world, EntityID e);
// フォーカス候補か。ウィジェットの根は「操作可能 && navigationMode != None」、それ以外は従来どおり
// UIElement.focusable (M35)。**FindNextFocus / 起動時の authored focus / ABI UISetFocused の共通規則**
bool IsFocusCandidate(World& world, EntityID e);
// ヒットした要素 (leaf) から祖先を辿り、最初のウィジェットの根を返す (Unity の ExecuteEvents が
// ハンドラを持つ祖先へ泡立つのと同じ)。無ければ leaf のまま。
// ★旧来のボタン (Selectable の無い UIElement kind 2) と Canvas で止まる — 止めないと、M75f 以前の
//   シーンでボタンの子の文字を押したときの clicked が変わりうる (祖先にウィジェットがあれば)
EntityID BubbleTarget(World& world, EntityID leaf);
// 描画の状態。ui は評価済みの対話状態 (hovered / pressed / focused は泡立ち後の根を指している)
int SelectionStateOf(World& world, const UIInteractionState& ui, EntityID e);

// ---- Slider ----
float SliderNormalizedValue(const UISliderComponent& slider); // Unity の Mathf.InverseLerp (min == max は 0)
float SliderClampValue(const UISliderComponent& slider, float value); // clamp → wholeNumbers なら偶数丸め
bool SliderIsVertical(const UISliderComponent& slider);
// 値が画面の右 / 下へ向かって減るか (右→左 / 下→上)。y 下向きなので Unity の reverseValue とは縦で逆
bool SliderIsReversed(const UISliderComponent& slider);
// dir (uinav::NavDir) が向きの軸に沿っているか = Slider がナビの代わりに値の変更として受けうる
bool SliderMoveOnAxis(const UISliderComponent& slider, int dir);
// e が最寄りの祖先 Slider の fillRect / handleRect なら、value から導いたアンカーを rt に当てたコピーを
// out に書いて true (Unity の Slider.UpdateVisuals と同じ式)。それ以外は false で out に触らない。
// ★uilayout::Resolve が全要素で呼ぶ — Slider の無いシーンでは祖先 4 段の GetComponent だけで抜ける
bool SliderDrivenTransform(World& world, EntityID e, const RectTransformComponent& rt,
                           RectTransformComponent& out);
bool IsSliderDriven(World& world, EntityID e);

// ---- 値の更新 (1 tick) ----
// Evaluate の後・スクリプト層の前に、**simulateScripts の tick だけ** 1 回呼ぶ (TickRunner)。
//   - state.clicked が Toggle なら反転 (ToggleGroup の規則つき)
//   - state.pressed が Slider ならポインタ位置から値を解く (押した tick はハンドルを掴んだ位置を覚える)
//   - events.navStepTarget の Slider を 1 段動かす
// 値が変わったウィジェットを state.changed に立てる (1 tick に 1 つ = 操作されたもの。ToggleGroup が
// 巻き添えで off にした Toggle は立てない)
void Update(World& world, const InputSnapshot& in, const uiinteract::TickEvents& events,
            UIInteractionState& state);

// ---- 描画 (UIRenderer) ----
inline constexpr uint32_t kVisTint = 1u << 0;              // color に tint を掛ける
inline constexpr uint32_t kVisSprite = 1u << 1;            // texture を sprite に差し替える (kind 0)
inline constexpr uint32_t kVisHidden = 1u << 2;            // 描かない (off の Toggle の graphic)
inline constexpr uint32_t kVisNoLegacyHighlight = 1u << 3; // ボタンのハードコードのハイライトを当てない
struct VisualOverride {
    EntityID e;
    uint32_t flags = 0;
    DirectX::XMFLOAT4 tint = { 1.0f, 1.0f, 1.0f, 1.0f };
    AssetID sprite = {};
};
// Active な Selectable / Toggle から描画の上書きを集める。ウィジェットの無いシーンでは空
void CollectVisualOverrides(World& world, const UIInteractionState* ui,
                            std::vector<VisualOverride>& out);
// e に掛かる上書きを合成する (flags は OR、tint は積、sprite は後勝ち)
VisualOverride MergedOverrideFor(const std::vector<VisualOverride>& overrides, EntityID e);

} // namespace uiwidgets
} // namespace mye
