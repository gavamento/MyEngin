//====================================================================================
//                          UIWidgets.h
//  MyEngine/ 秋田蓮音                                                      09/13/2026
//                                          UI ウィジェット（Selectable / Toggle / Slider / Scroll / Dropdown）の規則と値の更新
//====================================================================================
#pragma once
// ゲーム内 UI のウィジェット (M75f、M75g)。Unity uGUI の Selectable / Toggle / ToggleGroup / Slider /
// Scrollbar / ScrollRect / Dropdown の意味論を、「規則は純関数 / 値はハッシュ対象のコンポーネント」の形で持つ。
//
// 分担:
//   - uiinteract::Evaluate (UIInteraction.cpp) … hovered / pressed / clicked / focused。泡立ち
//     (BubbleTarget)・interactable・押したウィジェットへのフォーカス・Navigation のモードはここ。
//     M75g: 開いた Dropdown の Blocker (ApplyModalBlocker)・ドラッグの ScrollRect への引き渡し
//     (DragScrollTarget)・一覧の項目へのホバーでのフォーカス・Cancel もここ
//   - uiwidgets::Update (このファイル) … Toggle.isOn / Slider.value / Scrollbar.value / ScrollRect.position /
//     Dropdown.value・expanded を書き、changed を立てる。
//     **simulateScripts の tick だけ** TickRunner が呼ぶ (エディタの編集中にゲーム面をクリックしても
//     シーンのデータが Undo の外で書き換わらない。スクリプトと同じ門を通るので巻き戻しとも噛み合う)
//   - uilayout::Resolve … Slider の fill / handle・Scrollbar のつまみのアンカー、ScrollRect の中身の位置、
//     Dropdown の一覧の高さを WidgetDrivenTransform から導く (Layout Group と同じく子の RectTransform には書かない)
//   - UIRenderer … CollectVisualOverrides で Selectable の色 / 画像、Toggle のチェックマーク、Dropdown の表題と
//     項目の文字・チェックマークの表示。IsUiHidden / UiOrderBump で閉じた一覧を飛ばし、開いた一覧を手前に描く
//
// ★sim レーン (Evaluate / Update / Resolve) から呼ばれる関数は scalar の四則演算と floor / fmod
//   (と uilayout::DetPow) だけで組む。unordered も使わない (規則 7)。描画用の CollectVisualOverrides だけは
//   決定論の対象外
#include <cstdint>
#include <string>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/Ecs/EntityID.h"

namespace mye {

class World;
struct InputSnapshot;
struct RectTransformComponent;
struct UISelectableComponent;
struct UISliderComponent;
struct UIScrollbarComponent;
struct UIDropdownComponent;
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
// フォーカス候補か。ウィジェットの根は「操作可能 && navigationMode != None」、それ以外は
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
// dir の向きの UINav* を値の変更として受けるか (Slider / Scrollbar がフォーカス中で向きの軸に沿っている)。
// Evaluate が navStepTarget を立てる条件の 1 本
bool TakesNavStep(World& world, EntityID e, int dir);

// ---- ウィジェットが導く RectTransform (uilayout::Resolve の入口、M75g) ----
// e が次のどれかなら、導いた値を rt に当てたコピーを out に書いて true。それ以外は false で out に触らない:
//   - 最寄りの祖先 Slider の fillRect / handleRect (SliderDrivenTransform)
//   - 最寄りの祖先 Scrollbar の handleRect (アンカー = value と size。Unity の Scrollbar.UpdateVisuals)
//   - 祖先 ScrollRect の content (anchoredPosition に position を足す)
//   - Dropdown の templateRect (高さを選択肢の数に合わせて縮める。Unity の Show と同じ)
// ★ウィジェットの無いシーンでは祖先 4 段の GetComponent だけで抜ける
bool WidgetDrivenTransform(World& world, EntityID e, const RectTransformComponent& rt,
                           RectTransformComponent& out);
// Inspector の注記用 (uilayout::kDrivenBySlider / kDrivenByScrollbar / kDrivenByScrollRect / kDrivenByDropdown)
uint32_t WidgetDrivenBits(World& world, EntityID e);

// ---- Scrollbar (M75g) ----
// numberOfSteps (2 以上) で丸めた値 (Unity の Scrollbar.value の getter。偶数丸め)
float ScrollbarSteppedValue(const UIScrollbarComponent& bar);

// ---- ScrollRect (M75g) ----
// UIScrollRectComponent.movementType (Unity の ScrollRect.MovementType と同じ値)
inline constexpr int kScrollUnrestricted = 0;
inline constexpr int kScrollElastic = 1;
inline constexpr int kScrollClamped = 2;
// 押した要素からドラッグを引き取る ScrollRect (Unity の pointerDrag の探し方: 押した所から祖先へ辿って最初の
// ScrollRect)。押したのが Slider / Scrollbar なら自分でドラッグするので無し。開いた Dropdown の一覧の外も無し
EntityID DragScrollTarget(World& world, EntityID pressed);
// ホイールを受ける ScrollRect (ホバー中の要素から祖先へ辿って最初の ScrollRect。開いた Dropdown の一覧の外は無し)
EntityID WheelScrollTarget(World& world, EntityID hovered);

// ---- Dropdown (M75g) ----
// 選択肢 i の文字列 (範囲外は "")
const char* DropdownOption(const UIDropdownComponent& dd, int i);
// e が祖先 Dropdown の templateRect か (一覧の根)
bool IsDropdownTemplate(World& world, EntityID e);
// e が UIDropdownItem を持ち、index が持ち主の optionCount 以上 (= 使われていない項目) か
bool IsDropdownItemUnused(World& world, EntityID e);
// 描画 / ヒット / ナビから見た e の扱い。閉じた一覧の中と使われていない項目の中は hidden、
// 開いた一覧の中は描画 / ヒットの order に kDropdownListOrderBump を足す (両側で同じ規則)
inline constexpr int32_t kDropdownListOrderBump = 10000;
struct UiScope {
    bool hidden = false;
    int32_t orderBump = 0;
};
UiScope ScopeOf(World& world, EntityID e);
inline bool IsUiHidden(World& world, EntityID e) { return ScopeOf(world, e).hidden; }
// 開いている Dropdown (Active なもののうち entity.index 最小)。無ければ kNullEntity
EntityID ExpandedDropdown(World& world);
// Unity の Blocker: Dropdown が開いている間、その一覧の外のヒット (何も無い所を含む) は Dropdown の根に
// なる (= 押すと閉じる)。開いていなければ under のまま
EntityID ApplyModalBlocker(World& world, EntityID under);
// フォーカスを置けるか (Dropdown が開いている間は、その一覧の中だけ)
bool IsInModalScope(World& world, EntityID e);
// e が開いている Dropdown の一覧の項目の根か (ホバーでフォーカスを移す対象)
bool IsDropdownItemRoot(World& world, EntityID e);

// ---- 値の更新 (1 tick) ----
// Evaluate の後・スクリプト層の前に、**simulateScripts の tick だけ** 1 回呼ぶ (TickRunner)。
//   - state.clicked が Toggle なら反転 (ToggleGroup の規則つき)
//   - M75g: Cancel / state.clicked で Dropdown を開閉し、項目のクリックで value を変えて閉じる
//   - state.pressed が Slider / Scrollbar ならポインタ位置から値を解く (押した tick はつまみを掴んだ位置を覚える)
//   - events.navStepTarget の Slider / Scrollbar を 1 段動かす
//   - M75g: ScrollRect を entity.index 順に 1 tick 進める (スクロールバーの操作 → ホイール → ドラッグ →
//     Elastic / 慣性 → スクロールバーへ書き戻し。Unity の OnScroll / OnDrag / LateUpdate を固定 tick で)
// 値が変わったウィジェットを state.changed に立てる (1 tick に 1 つ = 操作されたもの。ToggleGroup が
// 巻き添えで off にした Toggle は立てない。ScrollRect は他に何も立っていない tick だけ立てる)
void Update(World& world, const InputSnapshot& in, const uiinteract::TickEvents& events,
            UIInteractionState& state);

// ---- 描画 (UIRenderer) ----
inline constexpr uint32_t kVisTint = 1u << 0;              // color に tint を掛ける
inline constexpr uint32_t kVisSprite = 1u << 1;            // texture を sprite に差し替える (kind 0)
inline constexpr uint32_t kVisHidden = 1u << 2;            // 描かない (off の Toggle の graphic)
inline constexpr uint32_t kVisNoLegacyHighlight = 1u << 3; // ボタンのハードコードのハイライトを当てない
inline constexpr uint32_t kVisText = 1u << 4; // M75g: 文字を text に差し替える (Dropdown の表題と項目)
struct VisualOverride {
    EntityID e;
    uint32_t flags = 0;
    DirectX::XMFLOAT4 tint = { 1.0f, 1.0f, 1.0f, 1.0f };
    AssetID sprite = {};
    std::string text;
};
// Active な Selectable / Toggle / Dropdown から描画の上書きを集める。ウィジェットの無いシーンでは空
void CollectVisualOverrides(World& world, const UIInteractionState* ui,
                            std::vector<VisualOverride>& out);
// e に掛かる上書きを合成する (flags は OR、tint は積、sprite と text は後勝ち)
VisualOverride MergedOverrideFor(const std::vector<VisualOverride>& overrides, EntityID e);

} // namespace uiwidgets
} // namespace mye
