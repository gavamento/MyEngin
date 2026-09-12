//====================================================================================
//                          UIWidgets.cpp
//  MyEngine/ 秋田蓮音                                                      09/13/2026
//                                          UI ウィジェットの規則と値の更新の実装（Unity uGUI の意味論の移植）
//====================================================================================
#include "Engine/Engine/UI/UIWidgets.h"

#include <cmath>

#include "Engine/Core/Components.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/UI/UIInteraction.h"
#include "Engine/Engine/UI/UILayout.h"
#include "Engine/Engine/UI/UILayoutGroup.h" // LayoutScratch
#include "Engine/Engine/UI/UINav.h"
#include "Engine/Platform/Input.h"

namespace mye {
namespace uiwidgets {
namespace {

// 泡立ちの上限 (壊れた親循環でも止まる)。UILayout の kMaxDepth と同じ値
constexpr int kMaxBubbleDepth = 64;
// fill / handle から駆動元の Slider を探す段数。Unity の既定の構成は Slider > Fill Area > Fill の 2 段で、
// 余裕を見て 4 段。これより深い所に置いた fillRect は駆動されない (Resolve が全要素で呼ぶので浅く抑える)
constexpr int kMaxDriveDepth = 4;

bool RefAlive(World& world, EntityID e)
{
    return !e.IsNull() && world.IsAlive(e);
}

float Clamp01(float t)
{
    return (t < 0.0f) ? 0.0f : ((t > 1.0f) ? 1.0f : t);
}

// 偶数丸め (Unity の Mathf.Round = System.Math.Round の既定)。floor と減算と fmod だけ = 丸めモードに依らない
float RoundHalfEven(float v)
{
    const float f = std::floor(v);
    const float diff = v - f;
    if (diff > 0.5f) {
        return f + 1.0f;
    }
    if (diff < 0.5f) {
        return f;
    }
    return (std::fmod(f, 2.0f) == 0.0f) ? f : f + 1.0f;
}

void SetSliderValue(UISliderComponent& slider, EntityID e, float value, UIInteractionState& state)
{
    const float v = SliderClampValue(slider, value);
    if (v != slider.value) {
        slider.value = v;
        state.changed = e;
    }
}

// Unity の Toggle.InternalToggle → Set(!isOn)。群の規則は ToggleGroup.NotifyToggleOn / AnyTogglesOn と同じ
void ClickToggle(World& world, EntityID t, UIInteractionState& state)
{
    auto* toggle = world.GetComponent<UIToggleComponent>(t);
    const bool wasOn = toggle->isOn != 0;
    bool on = !wasOn;
    const EntityID group = toggle->group;
    const UIToggleGroupComponent* g = RefAlive(world, group)
        ? world.GetComponent<UIToggleGroupComponent>(group) : nullptr;
    if (g != nullptr && IsEntityActive(world, group)) {
        const bool allowSwitchOff = g->allowSwitchOff != 0;
        const ComponentTypeId req[] = { UIToggleComponent::sTypeId };
        if (!on && !allowSwitchOff) {
            // 自分を off にした後に群のどれも on でなければ、on のまま (Unity: m_IsOn || !AnyTogglesOn())
            bool anyOtherOn = false;
            world.ForEachArchetype(req, [&](Archetype& arch) {
                const int ti = arch.FindTypeIndex(UIToggleComponent::sTypeId);
                for (uint32_t row = 0; row < arch.Count(); ++row) {
                    const EntityID u = arch.EntityAt(row);
                    const auto* other = static_cast<const UIToggleComponent*>(arch.GetPtr(ti, row));
                    if (u != t && other->group == group && other->isOn != 0
                        && IsEntityActive(world, u)) {
                        anyOtherOn = true;
                    }
                }
            });
            if (!anyOtherOn) {
                on = true;
            }
        }
        if (on) {
            // 群の他の Toggle を off にする。書く先は isOn だけ = 走査順に依らない
            world.ForEachArchetype(req, [&](Archetype& arch) {
                const int ti = arch.FindTypeIndex(UIToggleComponent::sTypeId);
                for (uint32_t row = 0; row < arch.Count(); ++row) {
                    const EntityID u = arch.EntityAt(row);
                    auto* other = static_cast<UIToggleComponent*>(arch.GetPtr(ti, row));
                    if (u != t && other->group == group && IsEntityActive(world, u)) {
                        other->isOn = 0;
                    }
                }
            });
        }
    }
    if (on != wasOn) {
        toggle->isOn = on ? 1 : 0;
        state.changed = t;
    }
}

// Unity の Slider.OnPointerDown / OnDrag → UpdateDrag。pressBegan は掴んだ tick
void PointerSlider(World& world, const InputSnapshot& in, EntityID s, bool pressBegan,
                   UIInteractionState& state)
{
    auto* slider = world.GetComponent<UISliderComponent>(s);
    const EntityID handle = RefAlive(world, slider->handleRect) ? slider->handleRect : kNullEntity;
    const EntityID fill = RefAlive(world, slider->fillRect) ? slider->fillRect : kNullEntity;
    const EntityID handleArea = handle.IsNull() ? kNullEntity : world.GetParent(handle);
    const EntityID fillArea = fill.IsNull() ? kNullEntity : world.GetParent(fill);
    // 値を解く矩形 = ハンドルの親 (無ければ塗りの親)。Unity の clickRect と同じ選び方
    const EntityID clickArea = !handleArea.IsNull() ? handleArea : fillArea;
    if (clickArea.IsNull()) {
        return;
    }

    // ポインタは Evaluate と同じ換算 (ゲーム面 px → 既定キャンバス → 要素の属するキャンバス)
    const uilayout::CanvasInfo canvas = uilayout::CanvasOfInput(in);
    uilayout::UIWorldContext wcData;
    const uilayout::UIWorldContext* wc =
        uilayout::BuildSimWorldContext(world, canvas.w, canvas.h, wcData) ? &wcData : nullptr;
    uilayout::LayoutScratch scratch;
    const float pointX = uilayout::SurfaceToCanvas(in.mouseSurfX, canvas);
    const float pointY = uilayout::SurfaceToCanvas(in.mouseSurfY, canvas);
    // e の未回転フレームでのポインタ位置と e の矩形 (回転 / スケールは逆変換で外す = HitTest と同じ)
    const auto localPoint = [&](EntityID e, float& lx, float& ly, uilayout::UIRect& rect) {
        const uilayout::UIResolved r = uilayout::Resolve(world, e, canvas.w, canvas.h, wc, &scratch);
        if (!r.visible) {
            return false;
        }
        const float toCanvas = uilayout::CanvasOf(world, e, canvas.w, canvas.h).scale;
        lx = pointX / toCanvas;
        ly = pointY / toCanvas;
        if (r.hasXform) {
            uilayout::UIXform inv;
            if (!uilayout::InvertXform(r.xform, inv)) {
                return false;
            }
            const float x = lx;
            const float y = ly;
            uilayout::XformPoint(inv, x, y, lx, ly);
        }
        rect = r.rect;
        return true;
    };

    const bool vertical = SliderIsVertical(*slider);
    if (pressBegan) {
        slider->dragOffset = { 0.0f, 0.0f };
        float hx = 0.0f, hy = 0.0f;
        uilayout::UIRect hr;
        if (!handleArea.IsNull() && localPoint(handle, hx, hy, hr) && hx >= hr.x
            && hx < hr.x + hr.w && hy >= hr.y && hy < hr.y + hr.h) {
            // ハンドルの上で押した: pivot からのずれを覚えて**値は動かさない** (Unity と同じ)。
            // 以後のドラッグはこのずれを引いて解くので、掴んだ瞬間にハンドルが飛ばない
            const auto* hrt = world.GetComponent<RectTransformComponent>(handle);
            const float pivotX = hrt ? hrt->pivot.x : 0.0f;
            const float pivotY = hrt ? hrt->pivot.y : 0.0f;
            slider->dragOffset = { hx - (hr.x + pivotX * hr.w), hy - (hr.y + pivotY * hr.h) };
            return;
        }
    }
    float lx = 0.0f, ly = 0.0f;
    uilayout::UIRect area;
    if (!localPoint(clickArea, lx, ly, area)) {
        return;
    }
    const float size = vertical ? area.h : area.w;
    if (!(size > 0.0f)) {
        return;
    }
    const float t = Clamp01(vertical ? (ly - area.y - slider->dragOffset.y) / size
                                     : (lx - area.x - slider->dragOffset.x) / size);
    const float n = SliderIsReversed(*slider) ? 1.0f - t : t;
    // Unity の normalizedValue の setter = Mathf.Lerp(min, max, n)
    SetSliderValue(*slider, s, slider->minValue + (slider->maxValue - slider->minValue) * n, state);
}

// Unity の Slider.OnMove。step は wholeNumbers なら 1、それ以外は範囲の 1/10
void StepSlider(World& world, EntityID s, int dir, UIInteractionState& state)
{
    auto* slider = world.GetComponent<UISliderComponent>(s);
    const bool towardRightOrDown = (dir == uinav::kNavRight || dir == uinav::kNavDown);
    const float step =
        (slider->wholeNumbers != 0) ? 1.0f : (slider->maxValue - slider->minValue) * 0.1f;
    const float delta = (towardRightOrDown != SliderIsReversed(*slider)) ? step : -step;
    SetSliderValue(*slider, s, slider->value + delta, state);
}

} // namespace

bool IsWidgetRoot(World& world, EntityID e)
{
    if (!RefAlive(world, e)) {
        return false;
    }
    return world.GetComponent<UISelectableComponent>(e) != nullptr
        || world.GetComponent<UIToggleComponent>(e) != nullptr
        || world.GetComponent<UISliderComponent>(e) != nullptr;
}

const UISelectableComponent& SelectableOf(World& world, EntityID e)
{
    static const UISelectableComponent kDefault{};
    const UISelectableComponent* sel =
        RefAlive(world, e) ? world.GetComponent<UISelectableComponent>(e) : nullptr;
    return (sel != nullptr) ? *sel : kDefault;
}

bool IsInteractable(World& world, EntityID e)
{
    return SelectableOf(world, e).interactable != 0;
}

bool IsFocusCandidate(World& world, EntityID e)
{
    if (!RefAlive(world, e)) {
        return false;
    }
    if (IsWidgetRoot(world, e)) {
        const UISelectableComponent& sel = SelectableOf(world, e);
        return sel.interactable != 0 && sel.navigationMode != kNavNone;
    }
    const auto* el = world.GetComponent<UIElementComponent>(e);
    return el != nullptr && el->focusable != 0;
}

EntityID BubbleTarget(World& world, EntityID leaf)
{
    if (!RefAlive(world, leaf)) {
        return leaf;
    }
    EntityID n = leaf;
    for (int guard = 0; guard < kMaxBubbleDepth && !n.IsNull(); ++guard) {
        if (IsWidgetRoot(world, n)) {
            return n;
        }
        const auto* el = world.GetComponent<UIElementComponent>(n);
        if (el != nullptr && el->kind == 2) {
            return leaf; // 旧来のボタンは自分の部分木を外のウィジェットから遮る
        }
        if (world.GetComponent<UICanvasComponent>(n) != nullptr) {
            return leaf; // Canvas の外は別の座標系 (ClipRect と同じ切り方)
        }
        n = world.GetParent(n);
    }
    return leaf;
}

int SelectionStateOf(World& world, const UIInteractionState& ui, EntityID e)
{
    if (SelectableOf(world, e).interactable == 0) {
        return kStateDisabled;
    }
    if (ui.pressed == e) {
        return kStatePressed; // 押したまま外へ出ても Pressed (Unity の isPointerDown)
    }
    if (ui.focused == e) {
        return kStateSelected;
    }
    if (ui.hovered == e) {
        return kStateHighlighted;
    }
    return kStateNormal;
}

float SliderNormalizedValue(const UISliderComponent& slider)
{
    if (slider.minValue == slider.maxValue) {
        return 0.0f;
    }
    return Clamp01((slider.value - slider.minValue) / (slider.maxValue - slider.minValue));
}

float SliderClampValue(const UISliderComponent& slider, float value)
{
    const float lo = slider.minValue;
    const float hi = (slider.maxValue < slider.minValue) ? slider.minValue : slider.maxValue;
    float v = value;
    if (!(v == v)) {
        v = lo; // NaN は最小値へ (比較が全部偽になって素通りするのを塞ぐ)
    }
    v = (v < lo) ? lo : ((v > hi) ? hi : v);
    if (slider.wholeNumbers != 0) {
        v = RoundHalfEven(v);
    }
    return v;
}

bool SliderIsVertical(const UISliderComponent& slider)
{
    return slider.direction == kSliderBottomToTop || slider.direction == kSliderTopToBottom;
}

bool SliderIsReversed(const UISliderComponent& slider)
{
    return slider.direction == kSliderRightToLeft || slider.direction == kSliderBottomToTop;
}

bool SliderMoveOnAxis(const UISliderComponent& slider, int dir)
{
    return SliderIsVertical(slider) ? (dir == uinav::kNavUp || dir == uinav::kNavDown)
                                    : (dir == uinav::kNavLeft || dir == uinav::kNavRight);
}

bool SliderDrivenTransform(World& world, EntityID e, const RectTransformComponent& rt,
                           RectTransformComponent& out)
{
    if (!RefAlive(world, e)) {
        return false;
    }
    EntityID p = world.GetParent(e);
    for (int d = 0; d < kMaxDriveDepth && !p.IsNull(); ++d) {
        if (const auto* slider = world.GetComponent<UISliderComponent>(p)) {
            const bool isFill = slider->fillRect == e;
            const bool isHandle = !isFill && slider->handleRect == e;
            if (!isFill && !isHandle) {
                return false; // 駆動するのは最寄りの Slider だけ
            }
            const float n = SliderNormalizedValue(*slider);
            const int axis = SliderIsVertical(*slider) ? 1 : 0;
            const bool reverse = SliderIsReversed(*slider);
            // Unity の UpdateVisuals: もう一方の軸は 0..1 に張る。塗りは値の始点から、つまみは値の位置に
            float amin[2] = { 0.0f, 0.0f };
            float amax[2] = { 1.0f, 1.0f };
            if (isFill) {
                if (reverse) {
                    amin[axis] = 1.0f - n;
                } else {
                    amax[axis] = n;
                }
            } else {
                const float a = reverse ? 1.0f - n : n;
                amin[axis] = a;
                amax[axis] = a;
            }
            out = rt;
            out.anchorMin = { amin[0], amin[1] };
            out.anchorMax = { amax[0], amax[1] };
            return true;
        }
        p = world.GetParent(p);
    }
    return false;
}

bool IsSliderDriven(World& world, EntityID e)
{
    const auto* rt = RefAlive(world, e) ? world.GetComponent<RectTransformComponent>(e) : nullptr;
    if (rt == nullptr) {
        return false;
    }
    RectTransformComponent unused;
    return SliderDrivenTransform(world, e, *rt, unused);
}

void Update(World& world, const InputSnapshot& in, const uiinteract::TickEvents& events,
            UIInteractionState& state)
{
    // ---- Toggle: clicked はマウスの「掴んだ要素の上で離した」と Submit の合流先 (Evaluate) ----
    if (RefAlive(world, state.clicked) && IsInteractable(world, state.clicked)
        && world.GetComponent<UIToggleComponent>(state.clicked) != nullptr) {
        ClickToggle(world, state.clicked, state);
    }
    // ---- Slider: 掴んでいる間は毎 tick ポインタ位置から値を解く (Unity はドラッグ閾値を使わない) ----
    if (RefAlive(world, state.pressed) && in.MouseDown(0) && IsInteractable(world, state.pressed)
        && world.GetComponent<UISliderComponent>(state.pressed) != nullptr) {
        PointerSlider(world, in, state.pressed, events.pressBegan == state.pressed, state);
    }
    // ---- Slider: 向きの軸の UINav* を値の変更として受けた (Evaluate がフォーカスを動かさなかった) ----
    if (events.navStepDir >= 0 && RefAlive(world, events.navStepTarget)
        && IsInteractable(world, events.navStepTarget)
        && world.GetComponent<UISliderComponent>(events.navStepTarget) != nullptr) {
        StepSlider(world, events.navStepTarget, events.navStepDir, state);
    }
}

void CollectVisualOverrides(World& world, const UIInteractionState* ui,
                            std::vector<VisualOverride>& out)
{
    out.clear();
    static const UIInteractionState kIdle{};
    const UIInteractionState& state = (ui != nullptr) ? *ui : kIdle;
    {
        const ComponentTypeId req[] = { UISelectableComponent::sTypeId };
        world.ForEachArchetype(req, [&](Archetype& arch) {
            const int si = arch.FindTypeIndex(UISelectableComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                const EntityID e = arch.EntityAt(row);
                if (!IsEntityActive(world, e)) {
                    continue;
                }
                const auto& sel = *static_cast<const UISelectableComponent*>(arch.GetPtr(si, row));
                const bool ownGraphic = world.GetComponent<UIElementComponent>(e) != nullptr;
                if (ownGraphic) {
                    VisualOverride v;
                    v.e = e;
                    v.flags = kVisNoLegacyHighlight; // Selectable を持つボタンは遷移の設定だけで見せる
                    out.push_back(v);
                }
                const EntityID target = (RefAlive(world, sel.targetGraphic)
                                         && world.GetComponent<UIElementComponent>(sel.targetGraphic))
                    ? sel.targetGraphic
                    : (ownGraphic ? e : kNullEntity);
                if (target.IsNull()) {
                    continue;
                }
                const int st = SelectionStateOf(world, state, e);
                VisualOverride v;
                v.e = target;
                if (sel.transition == kTransitionColorTint) {
                    const DirectX::XMFLOAT4& c = (st == kStateHighlighted) ? sel.highlightedColor
                        : (st == kStatePressed)                            ? sel.pressedColor
                        : (st == kStateSelected)                           ? sel.selectedColor
                        : (st == kStateDisabled)                           ? sel.disabledColor
                                                                           : sel.normalColor;
                    const float m = sel.colorMultiplier;
                    v.flags = kVisTint;
                    v.tint = { c.x * m, c.y * m, c.z * m, c.w * m };
                } else if (sel.transition == kTransitionSpriteSwap) {
                    const AssetID sprite = (st == kStateHighlighted) ? sel.highlightedSprite
                        : (st == kStatePressed)                      ? sel.pressedSprite
                        : (st == kStateSelected)                     ? sel.selectedSprite
                        : (st == kStateDisabled)                     ? sel.disabledSprite
                                                                     : AssetID{};
                    if (sprite.IsNull()) {
                        continue; // その状態の画像が無い = 元のテクスチャのまま
                    }
                    v.flags = kVisSprite;
                    v.sprite = sprite;
                } else {
                    continue;
                }
                out.push_back(v);
            }
        });
    }
    {
        const ComponentTypeId req[] = { UIToggleComponent::sTypeId };
        world.ForEachArchetype(req, [&](Archetype& arch) {
            const int ti = arch.FindTypeIndex(UIToggleComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                const auto* toggle = static_cast<const UIToggleComponent*>(arch.GetPtr(ti, row));
                if (toggle->isOn != 0 || !RefAlive(world, toggle->graphic)
                    || !IsEntityActive(world, arch.EntityAt(row))) {
                    continue;
                }
                VisualOverride v;
                v.e = toggle->graphic;
                v.flags = kVisHidden;
                out.push_back(v);
            }
        });
    }
}

VisualOverride MergedOverrideFor(const std::vector<VisualOverride>& overrides, EntityID e)
{
    VisualOverride merged;
    merged.e = e;
    for (const VisualOverride& v : overrides) {
        if (v.e != e) {
            continue;
        }
        merged.flags |= v.flags;
        if ((v.flags & kVisTint) != 0) {
            merged.tint = { merged.tint.x * v.tint.x, merged.tint.y * v.tint.y,
                            merged.tint.z * v.tint.z, merged.tint.w * v.tint.w };
        }
        if ((v.flags & kVisSprite) != 0) {
            merged.sprite = v.sprite;
        }
    }
    return merged;
}

} // namespace uiwidgets
} // namespace mye
