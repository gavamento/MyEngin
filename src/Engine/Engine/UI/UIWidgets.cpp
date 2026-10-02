//====================================================================================
//                          UIWidgets.cpp
//  MyEngine/ 秋田蓮音                                                      09/13/2026
//                                          UI ウィジェットの規則と値の更新の実装（Unity uGUI の意味論の移植）
//====================================================================================
#include "Engine/Engine/UI/UIWidgets.h"

#include <algorithm>
#include <cmath>

#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
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
// 余裕を見て 4 段。これより深い所に置いた fillRect は駆動されない (Resolve が全要素で呼ぶので浅く抑える)。
// M75g: Scrollbar > Sliding Area > Handle / ScrollRect > Viewport > Content / Dropdown > Template も同じ段数で探す
constexpr int kMaxDriveDepth = 4;
// sim の 1 tick (秒)。EngineLoop / HeadlessSim の kFixedDt と同じ 60 Hz。ScrollRect の Elastic / 慣性は
// Unity の unscaledDeltaTime の代わりにこれで進む = 描画の速さに依らない
constexpr float kTickSeconds = 1.0f / 60.0f;
// ホイール 1 目盛りの生値 (Win32 の WHEEL_DELTA)。InputSnapshot.wheelDelta はこの単位の累積
constexpr float kWheelNotch = 120.0f;

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

// e と祖先 (自分を含む) のうち、最初に T を持つもの。無ければ kNullEntity
template <typename T>
EntityID FindSelfOrAncestorWith(World& world, EntityID e)
{
    EntityID n = e;
    for (int guard = 0; guard < kMaxBubbleDepth && RefAlive(world, n); ++guard) {
        if (world.GetComponent<T>(n) != nullptr) {
            return n;
        }
        n = world.GetParent(n);
    }
    return kNullEntity;
}

// e が root 自身か root の子孫か
bool IsSelfOrDescendant(World& world, EntityID e, EntityID root)
{
    if (root.IsNull()) {
        return false;
    }
    EntityID n = e;
    for (int guard = 0; guard < kMaxBubbleDepth && RefAlive(world, n); ++guard) {
        if (n == root) {
            return true;
        }
        n = world.GetParent(n);
    }
    return false;
}

// ポインタを要素のフレームへ写す道具。Evaluate と同じ換算 (ゲーム面 px → 既定キャンバス → 要素の属する
// キャンバス) で、回転 / スケールは逆変換で外す (= HitTest と同じ)。
// ★1 つのウィジェットの更新の中だけで使う — 値を書き換えた後に同じ要素を解き直すと、自動レイアウトのメモ
//   (scratch) が古い配置を返しうる
class PointerFrame {
public:
    PointerFrame(World& world, const InputSnapshot& in)
        : world_(world), canvas_(uilayout::CanvasOfInput(in))
    {
        wc_ = uilayout::BuildSimWorldContext(world, canvas_.w, canvas_.h, wcData_) ? &wcData_ : nullptr;
        pointX_ = uilayout::SurfaceToCanvas(in.mouseSurfX, canvas_);
        pointY_ = uilayout::SurfaceToCanvas(in.mouseSurfY, canvas_);
    }

    // e の矩形 (e の属するキャンバスの単位、未回転)
    bool Rect(EntityID e, uilayout::UIRect& rect)
    {
        const uilayout::UIResolved r = uilayout::Resolve(world_, e, canvas_.w, canvas_.h, wc_, &scratch_);
        if (!r.visible) {
            return false;
        }
        rect = r.rect;
        return true;
    }

    // e の未回転フレームでのポインタ位置と e の矩形
    bool Local(EntityID e, float& lx, float& ly, uilayout::UIRect& rect)
    {
        const uilayout::UIResolved r = uilayout::Resolve(world_, e, canvas_.w, canvas_.h, wc_, &scratch_);
        if (!r.visible) {
            return false;
        }
        const float toCanvas = uilayout::CanvasOf(world_, e, canvas_.w, canvas_.h).scale;
        lx = pointX_ / toCanvas;
        ly = pointY_ / toCanvas;
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
    }

    // ポインタ位置 (e の属するキャンバスの単位。回転は外さない — ドラッグ量だけを使う所向け)
    void PointIn(EntityID e, float& x, float& y)
    {
        const float toCanvas = uilayout::CanvasOf(world_, e, canvas_.w, canvas_.h).scale;
        x = pointX_ / toCanvas;
        y = pointY_ / toCanvas;
    }

private:
    World& world_;
    uilayout::CanvasInfo canvas_;
    uilayout::UIWorldContext wcData_;
    const uilayout::UIWorldContext* wc_ = nullptr;
    uilayout::LayoutScratch scratch_;
    float pointX_ = 0.0f;
    float pointY_ = 0.0f;
};

bool Inside(float x, float y, const uilayout::UIRect& r)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
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
    const bool wasOn = toggle->isOn;
    bool on = !wasOn;
    const EntityID group = toggle->group;
    const UIToggleGroupComponent* g = RefAlive(world, group)
        ? world.GetComponent<UIToggleGroupComponent>(group) : nullptr;
    if (g != nullptr && IsEntityActive(world, group)) {
        const bool allowSwitchOff = g->allowSwitchOff;
        const ComponentTypeId req[] = { UIToggleComponent::sTypeId };
        if (!on && !allowSwitchOff) {
            // 自分を off にした後に群のどれも on でなければ、on のまま (Unity: m_IsOn || !AnyTogglesOn())
            bool anyOtherOn = false;
            world.ForEachArchetype(req, [&](Archetype& arch) {
                const int ti = arch.FindTypeIndex(UIToggleComponent::sTypeId);
                for (uint32_t row = 0; row < arch.Count(); ++row) {
                    const EntityID u = arch.EntityAt(row);
                    const auto* other = static_cast<const UIToggleComponent*>(arch.GetPtr(ti, row));
                    if (u != t && other->group == group && other->isOn
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
                        other->isOn = false;
                    }
                }
            });
        }
    }
    if (on != wasOn) {
        toggle->isOn = on;
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
    PointerFrame pf(world, in);

    const bool vertical = SliderIsVertical(*slider);
    if (pressBegan) {
        slider->dragOffset = { 0.0f, 0.0f };
        float hx = 0.0f, hy = 0.0f;
        uilayout::UIRect hr;
        if (!handleArea.IsNull() && pf.Local(handle, hx, hy, hr) && Inside(hx, hy, hr)) {
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
    if (!pf.Local(clickArea, lx, ly, area)) {
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
        (slider->wholeNumbers) ? 1.0f : (slider->maxValue - slider->minValue) * 0.1f;
    const float delta = (towardRightOrDown != SliderIsReversed(*slider)) ? step : -step;
    SetSliderValue(*slider, s, slider->value + delta, state);
}

// ---- Scrollbar (M75g) ----
// Slider と同じ並び (0 左→右 / 1 右→左 / 2 下→上 / 3 上→下)
bool ScrollbarIsVertical(const UIScrollbarComponent& bar)
{
    return bar.direction == kSliderBottomToTop || bar.direction == kSliderTopToBottom;
}

// 値が画面の右 / 下へ向かって減るか (右→左 / 下→上)。Slider と同じ (y 下向きなので Unity の reverseValue とは縦で逆)
bool ScrollbarIsReversed(const UIScrollbarComponent& bar)
{
    return bar.direction == kSliderRightToLeft || bar.direction == kSliderBottomToTop;
}

// Unity の Scrollbar.Set。value は生のまま持ち、丸めた値が変わったときだけ changed
void SetScrollbarValue(UIScrollbarComponent& bar, EntityID e, float value, UIInteractionState& state)
{
    const float before = ScrollbarSteppedValue(bar);
    bar.value = value;
    if (ScrollbarSteppedValue(bar) != before) {
        state.changed = e;
    }
}

// Unity の Scrollbar.OnPointerDown (ClickRepeat) / OnBeginDrag / OnDrag → UpdateDrag。
// つまみの上で押したら掴んだ点とつまみの中心のずれを覚えて値は動かさない。溝を押したらつまみの中心が
// ポインタへ来る (Unity は押している間 ClickRepeat が毎フレーム UpdateDrag するので結果は同じ)
void PointerScrollbar(World& world, const InputSnapshot& in, EntityID s, bool pressBegan,
                      UIInteractionState& state)
{
    auto* bar = world.GetComponent<UIScrollbarComponent>(s);
    const EntityID handle = RefAlive(world, bar->handleRect) ? bar->handleRect : kNullEntity;
    const EntityID area = handle.IsNull() ? kNullEntity : world.GetParent(handle);
    if (area.IsNull()) {
        return;
    }
    PointerFrame pf(world, in);
    if (pressBegan) {
        bar->dragOffset = { 0.0f, 0.0f };
        float hx = 0.0f, hy = 0.0f;
        uilayout::UIRect hr;
        if (pf.Local(handle, hx, hy, hr) && Inside(hx, hy, hr)) {
            bar->dragOffset = { hx - (hr.x + hr.w * 0.5f), hy - (hr.y + hr.h * 0.5f) };
            return;
        }
    }
    float lx = 0.0f, ly = 0.0f;
    uilayout::UIRect ar;
    if (!pf.Local(area, lx, ly, ar)) {
        return;
    }
    const bool vertical = ScrollbarIsVertical(*bar);
    const float parentSize = vertical ? ar.h : ar.w;
    const float size = Clamp01(bar->size);
    const float remaining = parentSize * (1.0f - size);
    if (!(remaining > 0.0f)) {
        return;
    }
    // つまみの中心 (溝の左 / 上端から) → つまみのアンカー部分の左 / 上端 → 残りの長さに対する比
    const float center = vertical ? (ly - bar->dragOffset.y - ar.y) : (lx - bar->dragOffset.x - ar.x);
    const float t = (center - parentSize * size * 0.5f) / remaining;
    SetScrollbarValue(*bar, s, Clamp01(ScrollbarIsReversed(*bar) ? 1.0f - t : t), state);
}

// Unity の Scrollbar.OnMove。stepSize は段数があれば 1 段、無ければ 0.1
void StepScrollbar(World& world, EntityID s, int dir, UIInteractionState& state)
{
    auto* bar = world.GetComponent<UIScrollbarComponent>(s);
    const bool towardRightOrDown = (dir == uinav::kNavRight || dir == uinav::kNavDown);
    const float step = (bar->numberOfSteps > 1) ? 1.0f / static_cast<float>(bar->numberOfSteps - 1) : 0.1f;
    const float delta = (towardRightOrDown != ScrollbarIsReversed(*bar)) ? step : -step;
    SetScrollbarValue(*bar, s, Clamp01(bar->value + delta), state);
}

// Unity の Scrollbar.UpdateVisuals (y 下向きへ写したもの)。つまみのアンカー = 溝の中の [movement, movement + size]
void ScrollbarHandleAnchors(const UIScrollbarComponent& bar, float amin[2], float amax[2])
{
    amin[0] = 0.0f;
    amin[1] = 0.0f;
    amax[0] = 1.0f;
    amax[1] = 1.0f;
    const int axis = ScrollbarIsVertical(bar) ? 1 : 0;
    const float size = Clamp01(bar.size);
    const float movement = Clamp01(ScrollbarSteppedValue(bar)) * (1.0f - size);
    if (ScrollbarIsReversed(bar)) {
        amin[axis] = 1.0f - movement - size;
        amax[axis] = 1.0f - movement;
    } else {
        amin[axis] = movement;
        amax[axis] = movement + size;
    }
}

// ---- ScrollRect (M75g) ----
// Unity の Bounds (中心と大きさ) を 2 軸だけ。座標は要素の属するキャンバスの単位・y 下向き。
// ★ScrollRect の式は軸ごとに独立で、y の向きを反転しても AdjustBounds / CalculateOffset / RubberDelta /
//   SmoothDamp の形は変わらない (両辺が一緒に反転する)。向きが効くのはホイールの符号と縦の normalizedPosition
//   (Unity は 0 = 下) とスクロールバーの向きだけ
struct Bounds2 {
    float center[2] = { 0.0f, 0.0f };
    float size[2] = { 0.0f, 0.0f };
    float Min(int a) const { return center[a] - size[a] * 0.5f; }
    float Max(int a) const { return center[a] + size[a] * 0.5f; }
};

Bounds2 BoundsOf(const uilayout::UIRect& r)
{
    // Unity の InternalGetBounds: Bounds(vMin, 0).Encapsulate(vMax) = 中心 (min + max) / 2、大きさ max - min
    Bounds2 b;
    b.center[0] = (r.x + (r.x + r.w)) * 0.5f;
    b.center[1] = (r.y + (r.y + r.h)) * 0.5f;
    b.size[0] = (r.x + r.w) - r.x;
    b.size[1] = (r.y + r.h) - r.y;
    return b;
}

// Unity の ScrollRect.AdjustBounds: 中身が見える範囲より小さい軸は、中身の pivot の側へ見える範囲の大きさまで広げる
// (スクロールは中身が大きいときだけ起きる)
void AdjustBounds(const Bounds2& view, const float pivot[2], Bounds2& content)
{
    for (int a = 0; a < 2; ++a) {
        const float excess = view.size[a] - content.size[a];
        if (excess > 0.0f) {
            content.center[a] -= excess * (pivot[a] - 0.5f);
            content.size[a] = view.size[a];
        }
    }
}

// Unity の ScrollRect.InternalCalculateOffset: 中身を delta だけ動かしたとき、見える範囲に収めるのに要るずれ
void CalculateOffset(const Bounds2& view, const Bounds2& content, const bool enabled[2], int movementType,
                     const float delta[2], float offset[2])
{
    offset[0] = 0.0f;
    offset[1] = 0.0f;
    if (movementType == kScrollUnrestricted) {
        return;
    }
    for (int a = 0; a < 2; ++a) {
        if (!enabled[a]) {
            continue;
        }
        const float minC = content.Min(a) + delta[a];
        const float maxC = content.Max(a) + delta[a];
        const float maxOffset = view.Max(a) - maxC;
        const float minOffset = view.Min(a) - minC;
        if (minOffset < -0.001f) {
            offset[a] = minOffset;
        } else if (maxOffset > 0.001f) {
            offset[a] = maxOffset;
        }
    }
}

// Unity の ScrollRect.RubberDelta (Elastic で端を越えて引っ張ったときの手応え)
float RubberDelta(float overStretching, float viewSize)
{
    const float sign = (overStretching < 0.0f) ? -1.0f : 1.0f; // Mathf.Sign(0) = 1
    return (1.0f - (1.0f / ((std::fabs(overStretching) * 0.55f / viewSize) + 1.0f))) * viewSize * sign;
}

// Unity の Mathf.SmoothDamp (maxSpeed = 無限大)。指数の近似も Unity と同じ多項式
float SmoothDamp(float current, float target, float& currentVelocity, float smoothTime, float deltaTime)
{
    const float st = (smoothTime > 0.0001f) ? smoothTime : 0.0001f;
    const float omega = 2.0f / st;
    const float x = omega * deltaTime;
    const float expo = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
    const float change = current - target;
    const float originalTo = target;
    const float tgt = current - change;
    const float temp = (currentVelocity + omega * change) * deltaTime;
    currentVelocity = (currentVelocity - omega * temp) * expo;
    float output = tgt + (change + temp) * expo;
    if ((originalTo - current > 0.0f) == (output > originalTo)) {
        output = originalTo;
        currentVelocity = (output - originalTo) / deltaTime;
    }
    return output;
}

// 1 つの ScrollRect の 1 tick ぶんの状態。中身の矩形は tick の頭で 1 回だけ解き、以後は position の差で
// 平行移動させる (Unity の UpdateBounds が SetContentAnchoredPosition の後に解き直すのと同じ値になる。
// 解き直さないのは、自動レイアウトのメモが古い配置を返しうるため)
class ScrollFrame {
public:
    ScrollFrame(const uilayout::UIRect& viewRect, const uilayout::UIRect& contentRect, const float pivot[2],
                const UIScrollRectComponent& sr)
        : view_(BoundsOf(viewRect)), raw_(BoundsOf(contentRect)), movementType_(sr.movementType)
    {
        pivot_[0] = pivot[0];
        pivot_[1] = pivot[1];
        enabled_[0] = sr.horizontal;
        enabled_[1] = sr.vertical;
        start_[0] = sr.position.x;
        start_[1] = sr.position.y;
        pos[0] = start_[0];
        pos[1] = start_[1];
    }

    // Unity の UpdateBounds 後の m_ContentBounds (今の pos での中身の範囲を、見える範囲の大きさまで広げたもの)
    Bounds2 Content() const
    {
        Bounds2 b = raw_;
        b.center[0] += pos[0] - start_[0];
        b.center[1] += pos[1] - start_[1];
        AdjustBounds(view_, pivot_, b);
        return b;
    }
    const Bounds2& View() const { return view_; }

    void Offset(const float delta[2], float offset[2]) const
    {
        CalculateOffset(view_, Content(), enabled_, movementType_, delta, offset);
    }

    // Unity の SetContentAnchoredPosition: 動かせない軸は今の値のまま
    void Set(const float p[2])
    {
        if (enabled_[0]) {
            pos[0] = p[0];
        }
        if (enabled_[1]) {
            pos[1] = p[1];
        }
    }

    // Unity の normalizedPosition の getter。横は 0 = 左端、縦は **0 = 下端** (Unity と同じ意味。y 下向きなので
    // 式は下端どうしの差で書く)
    float Normalized(int a) const
    {
        const Bounds2 c = Content();
        const bool fits = c.size[a] <= view_.size[a] || std::fabs(c.size[a] - view_.size[a]) < 1e-5f;
        if (a == 0) {
            if (fits) {
                return (view_.Min(0) > c.Min(0)) ? 1.0f : 0.0f;
            }
            return (view_.Min(0) - c.Min(0)) / (c.size[0] - view_.size[0]);
        }
        if (fits) {
            return (c.Max(1) > view_.Max(1)) ? 1.0f : 0.0f;
        }
        return (c.Max(1) - view_.Max(1)) / (c.size[1] - view_.size[1]);
    }

    // Unity の SetNormalizedPosition (速度のその軸を 0 にする)
    void SetNormalized(int a, float value, float velocity[2])
    {
        const Bounds2 c = Content();
        const float hidden = c.size[a] - view_.size[a];
        float target = pos[a];
        if (a == 0) {
            target = pos[0] + ((view_.Min(0) - value * hidden) - c.Min(0));
        } else {
            target = pos[1] + ((view_.Max(1) + value * hidden) - c.Max(1));
        }
        if (std::fabs(pos[a] - target) > 0.01f) {
            pos[a] = target;
            velocity[a] = 0.0f;
        }
    }

    bool Enabled(int a) const { return enabled_[a]; }
    int MovementType() const { return movementType_; }

    float pos[2] = { 0.0f, 0.0f };

private:
    Bounds2 view_;
    Bounds2 raw_; // tick の頭の pos での中身の矩形 (広げる前)
    float pivot_[2] = { 0.0f, 0.0f };
    bool enabled_[2] = { true, true };
    int movementType_ = kScrollElastic;
    float start_[2] = { 0.0f, 0.0f };
};

// ScrollRect 1 つを 1 tick 進める (Unity の OnScroll / OnInitializePotentialDrag / OnBeginDrag / OnDrag /
// OnEndDrag / LateUpdate / UpdateScrollbars を、この tick の出来事だけで順に当てる)
void UpdateScrollRect(World& world, const InputSnapshot& in, const uiinteract::TickEvents& events,
                      UIInteractionState& state, EntityID e, EntityID wheelTarget)
{
    auto* sr = world.GetComponent<UIScrollRectComponent>(e);
    if (!RefAlive(world, sr->content)) {
        return; // Unity の IsActive() は content が無ければ偽
    }
    const EntityID viewE = RefAlive(world, sr->viewport) ? sr->viewport : e;
    PointerFrame pf(world, in);
    uilayout::UIRect viewRect;
    uilayout::UIRect contentRect;
    if (!pf.Rect(viewE, viewRect) || !pf.Rect(sr->content, contentRect)) {
        return;
    }
    float pivot[2] = { 0.0f, 0.0f };
    if (const auto* crt = world.GetComponent<RectTransformComponent>(sr->content)) {
        pivot[0] = crt->pivot.x;
        pivot[1] = crt->pivot.y;
    }
    ScrollFrame f(viewRect, contentRect, pivot, *sr);
    float velocity[2] = { sr->velocity.x, sr->velocity.y };
    const float startPos[2] = { f.pos[0], f.pos[1] };
    bool scrolling = false; // Unity の m_Scrolling (この tick にホイールを回した = Elastic の戻りを 3 倍遅く)

    // ---- スクロールバーを操作した (Unity の onValueChanged → SetNormalizedPosition) ----
    const EntityID bars[2] = { sr->horizontalScrollbar, sr->verticalScrollbar };
    for (int a = 0; a < 2; ++a) {
        if (RefAlive(world, bars[a]) && state.changed == bars[a]) {
            const auto* bar = world.GetComponent<UIScrollbarComponent>(bars[a]);
            if (bar != nullptr) {
                const float v = ScrollbarSteppedValue(*bar);
                if (f.Normalized(a) != v) {
                    f.SetNormalized(a, v, velocity);
                }
            }
        }
    }

    // ---- 押した (Unity の OnInitializePotentialDrag: 滑っている中身を止める) ----
    if (!events.pressBegan.IsNull() && DragScrollTarget(world, events.pressBegan) == e) {
        velocity[0] = 0.0f;
        velocity[1] = 0.0f;
    }

    // ---- ホイール (Unity の OnScroll) ----
    if (wheelTarget == e && in.wheelDelta != 0) {
        // Unity の scrollDelta.y は上へ回すと正で、OnScroll が y を反転する (UI は上が正)。y 下向きの
        // ここでは反転が打ち消し合い、上へ回すと中身が下へ動く (= 上の方が見える)
        const float notches = static_cast<float>(in.wheelDelta) / kWheelNotch;
        float delta[2] = { 0.0f, notches };
        if (f.Enabled(1) && !f.Enabled(0)) {
            delta[0] = 0.0f;
        }
        if (f.Enabled(0) && !f.Enabled(1)) {
            delta[0] = -notches; // Unity: 横だけなら縦の量 (反転後の値) を横へ回す
            delta[1] = 0.0f;
        }
        scrolling = true;
        float p[2] = { f.pos[0] + delta[0] * sr->scrollSensitivity, f.pos[1] + delta[1] * sr->scrollSensitivity };
        if (f.MovementType() == kScrollClamped) {
            const float d[2] = { p[0] - f.pos[0], p[1] - f.pos[1] };
            float off[2];
            f.Offset(d, off);
            p[0] += off[0];
            p[1] += off[1];
        }
        f.Set(p);
    }

    // ---- ドラッグ (Unity の OnBeginDrag / OnDrag / OnEndDrag) ----
    float pointX = 0.0f, pointY = 0.0f;
    pf.PointIn(viewE, pointX, pointY);
    if (events.scrollDragBegan == e) {
        sr->dragging = true;
        sr->pointerStart = { pointX, pointY };
        sr->contentStart = { f.pos[0], f.pos[1] };
    }
    if (sr->dragging) {
        if (state.pressed == e && in.MouseDown(0)) {
            float p[2] = { sr->contentStart.x + (pointX - sr->pointerStart.x),
                           sr->contentStart.y + (pointY - sr->pointerStart.y) };
            const float d[2] = { p[0] - f.pos[0], p[1] - f.pos[1] };
            float off[2];
            f.Offset(d, off);
            p[0] += off[0];
            p[1] += off[1];
            if (f.MovementType() == kScrollElastic) {
                const Bounds2& view = f.View();
                for (int a = 0; a < 2; ++a) {
                    if (off[a] != 0.0f) {
                        p[a] = p[a] - RubberDelta(off[a], view.size[a]);
                    }
                }
            }
            f.Set(p);
        } else {
            sr->dragging = false;
        }
    }

    // ---- LateUpdate: Elastic で戻す / 慣性で滑らせる ----
    const float zero[2] = { 0.0f, 0.0f };
    float offset[2];
    f.Offset(zero, offset);
    const float dt = kTickSeconds;
    if (!sr->dragging && (offset[0] != 0.0f || offset[1] != 0.0f || velocity[0] != 0.0f || velocity[1] != 0.0f)) {
        float p[2] = { f.pos[0], f.pos[1] };
        // 慣性の 1 tick の減速 (decelerationRate^dt)。CRT の pow は使わない (UILayout.h の DetPow)
        const float rate = sr->decelerationRate;
        const float decay = (rate <= 0.0f) ? 0.0f
            : (rate >= 1.0f) ? 1.0f
                             : static_cast<float>(uilayout::DetPow(static_cast<double>(rate), static_cast<double>(dt)));
        for (int a = 0; a < 2; ++a) {
            if (f.MovementType() == kScrollElastic && offset[a] != 0.0f) {
                float speed = velocity[a];
                float smoothTime = sr->elasticity;
                if (scrolling) {
                    smoothTime *= 3.0f;
                }
                p[a] = SmoothDamp(f.pos[a], f.pos[a] + offset[a], speed, smoothTime, dt);
                if (std::fabs(speed) < 1.0f) {
                    speed = 0.0f;
                }
                velocity[a] = speed;
            } else if (sr->inertia) {
                velocity[a] *= decay;
                if (std::fabs(velocity[a]) < 1.0f) {
                    velocity[a] = 0.0f;
                }
                p[a] += velocity[a] * dt;
            } else {
                velocity[a] = 0.0f;
            }
        }
        if (f.MovementType() == kScrollClamped) {
            const float d[2] = { p[0] - f.pos[0], p[1] - f.pos[1] };
            float off[2];
            f.Offset(d, off);
            p[0] += off[0];
            p[1] += off[1];
        }
        f.Set(p);
    }
    if (sr->dragging && sr->inertia) {
        // Unity: newVelocity = (今 - 前の LateUpdate の位置) / dt、velocity = Lerp(velocity, newVelocity, dt * 10)
        const float t = Clamp01(dt * 10.0f);
        for (int a = 0; a < 2; ++a) {
            const float newVelocity = (f.pos[a] - startPos[a]) / dt;
            velocity[a] = velocity[a] + (newVelocity - velocity[a]) * t;
        }
    }

    // ---- 書き戻し + スクロールバー (Unity の UpdateScrollbars。size は LateUpdate の頭の offset で縮める) ----
    const Bounds2 content = f.Content();
    const Bounds2& view = f.View();
    for (int a = 0; a < 2; ++a) {
        auto* bar = RefAlive(world, bars[a]) ? world.GetComponent<UIScrollbarComponent>(bars[a]) : nullptr;
        if (bar == nullptr) {
            continue;
        }
        bar->size = (content.size[a] > 0.0f)
            ? Clamp01((view.size[a] - std::fabs(offset[a])) / content.size[a]) : 1.0f;
        bar->value = f.Normalized(a);
    }
    const bool moved = f.pos[0] != startPos[0] || f.pos[1] != startPos[1];
    sr->position = { f.pos[0], f.pos[1] };
    sr->velocity = { velocity[0], velocity[1] };
    if (moved && state.changed.IsNull()) {
        // 操作されたウィジェットを優先する (スクロールバーで動かした tick はスクロールバーが立っている)。
        // 慣性 / Elastic で動いている tick も立つ (Unity の onValueChanged と同じ)
        state.changed = e;
    }
}

// ---- Dropdown (M75g) ----
const UIDropdownComponent* DropdownOf(World& world, EntityID e)
{
    return RefAlive(world, e) ? world.GetComponent<UIDropdownComponent>(e) : nullptr;
}

// 項目の持ち主 = 最寄りの祖先の Dropdown (自分は含まない)
EntityID OwnerDropdown(World& world, EntityID item)
{
    return RefAlive(world, item) ? FindSelfOrAncestorWith<UIDropdownComponent>(world, world.GetParent(item))
                                 : kNullEntity;
}

// 開いた Dropdown の一覧の中で、選択中の項目 (同じ index が複数あれば entity.index 最小)
EntityID SelectedItemOf(World& world, EntityID dd, int32_t value)
{
    EntityID best = kNullEntity;
    const ComponentTypeId req[] = { UIDropdownItemComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int ii = arch.FindTypeIndex(UIDropdownItemComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID it = arch.EntityAt(row);
            const auto* item = static_cast<const UIDropdownItemComponent*>(arch.GetPtr(ii, row));
            if (item->index != value || !IsEntityActive(world, it) || OwnerDropdown(world, it) != dd) {
                continue;
            }
            if (best.IsNull() || it.index < best.index) {
                best = it;
            }
        }
    });
    return best;
}

// Unity の Dropdown.Hide → Select()
void HideDropdown(World& world, EntityID dd, UIInteractionState& state)
{
    world.GetComponent<UIDropdownComponent>(dd)->expanded = false;
    if (IsFocusCandidate(world, dd)) {
        state.focused = dd;
    }
}

// Unity の Dropdown.Show。一覧は作り直さない代わりに、一覧の ScrollRect を先頭へ戻す (Unity は毎回新しく
// 作るので常に先頭から見える)。他に開いている Dropdown は閉じる (Blocker が先に押される Unity と同じ結果)
void ShowDropdown(World& world, EntityID dd, UIInteractionState& state)
{
    const ComponentTypeId req[] = { UIDropdownComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int di = arch.FindTypeIndex(UIDropdownComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            static_cast<UIDropdownComponent*>(arch.GetPtr(di, row))->expanded = false;
        }
    });
    auto* d = world.GetComponent<UIDropdownComponent>(dd);
    d->expanded = true;
    const int32_t value = d->value;
    if (RefAlive(world, d->templateRect)) {
        if (auto* list = world.GetComponent<UIScrollRectComponent>(d->templateRect)) {
            list->position = { 0.0f, 0.0f };
            list->velocity = { 0.0f, 0.0f };
            list->dragging = false;
        }
    }
    // Unity: 選択中の項目の Toggle を Select() する
    const EntityID selected = SelectedItemOf(world, dd, value);
    if (!selected.IsNull() && IsFocusCandidate(world, selected)) {
        state.focused = selected;
    }
}

// Cancel / 開閉 / 項目の選択。どれかを処理したら true
void UpdateDropdowns(World& world, const uiinteract::TickEvents& events, UIInteractionState& state)
{
    if (events.cancel) {
        const EntityID open = ExpandedDropdown(world);
        if (!open.IsNull()) {
            HideDropdown(world, open, state); // Unity の OnCancel (Dropdown 自身と項目の両方が持つ)
            return;
        }
    }
    const EntityID c = state.clicked;
    if (!RefAlive(world, c) || !IsInteractable(world, c)) {
        return;
    }
    if (const auto* d = world.GetComponent<UIDropdownComponent>(c)) {
        // Unity の OnPointerClick / OnSubmit → Show。開いている間の押下は Blocker 経由でここへ来る = Hide
        if (d->expanded) {
            HideDropdown(world, c, state);
        } else {
            ShowDropdown(world, c, state);
        }
        return;
    }
    if (const auto* item = world.GetComponent<UIDropdownItemComponent>(c)) {
        const EntityID dd = OwnerDropdown(world, c);
        auto* d = RefAlive(world, dd) ? world.GetComponent<UIDropdownComponent>(dd) : nullptr;
        if (d == nullptr || !d->expanded) {
            return;
        }
        const int32_t index = item->index;
        // Unity の OnSelectItem → value の setter (0..options.Count-1 に収める) → Hide
        if (index >= 0 && index < d->optionCount && index != d->value) {
            d->value = index;
            state.changed = dd;
        }
        HideDropdown(world, dd, state);
    }
}

} // namespace

bool IsWidgetRoot(World& world, EntityID e)
{
    if (!RefAlive(world, e)) {
        return false;
    }
    return world.GetComponent<UISelectableComponent>(e) != nullptr
        || world.GetComponent<UIToggleComponent>(e) != nullptr
        || world.GetComponent<UISliderComponent>(e) != nullptr
        || world.GetComponent<UIScrollbarComponent>(e) != nullptr
        || world.GetComponent<UIDropdownComponent>(e) != nullptr
        || world.GetComponent<UIDropdownItemComponent>(e) != nullptr;
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
    return el != nullptr && el->focusable;
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
        if (IsDropdownTemplate(world, n)) {
            // M75g: Dropdown の一覧は Dropdown の子として置くが、意味は別の窓 (Unity は一覧を最上位の Canvas へ
            // 作る)。一覧の背景を押しても Dropdown の根へ泡立てない (泡立てると押しただけで閉じる)
            return leaf;
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
    if (slider.wholeNumbers) {
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

bool TakesNavStep(World& world, EntityID e, int dir)
{
    if (!RefAlive(world, e)) {
        return false;
    }
    if (const auto* slider = world.GetComponent<UISliderComponent>(e)) {
        return SliderMoveOnAxis(*slider, dir);
    }
    if (const auto* bar = world.GetComponent<UIScrollbarComponent>(e)) {
        return ScrollbarIsVertical(*bar) ? (dir == uinav::kNavUp || dir == uinav::kNavDown)
                                         : (dir == uinav::kNavLeft || dir == uinav::kNavRight);
    }
    return false;
}

namespace {

// WidgetDrivenTransform と WidgetDrivenBits の共通部。bits は uilayout::kDrivenBy* の 1 つ
bool DriveImpl(World& world, EntityID e, const RectTransformComponent& rt, RectTransformComponent& out,
               uint32_t& bits)
{
    bits = 0;
    if (!RefAlive(world, e)) {
        return false;
    }
    // Slider (M75f) は最寄りの Slider だけが駆動する (祖先 4 段)
    if (SliderDrivenTransform(world, e, rt, out)) {
        bits = uilayout::kDrivenBySlider;
        return true;
    }
    EntityID p = world.GetParent(e);
    for (int d = 0; d < kMaxDriveDepth && !p.IsNull(); ++d) {
        if (const auto* bar = world.GetComponent<UIScrollbarComponent>(p); bar != nullptr && bar->handleRect == e) {
            float amin[2];
            float amax[2];
            ScrollbarHandleAnchors(*bar, amin, amax);
            out = rt;
            out.anchorMin = { amin[0], amin[1] };
            out.anchorMax = { amax[0], amax[1] };
            bits = uilayout::kDrivenByScrollbar;
            return true;
        }
        if (const auto* sr = world.GetComponent<UIScrollRectComponent>(p); sr != nullptr && sr->content == e) {
            // Unity は content.anchoredPosition を書き換える。ここでは保存値に足した位置で解く
            out = rt;
            out.anchoredPosition = { rt.anchoredPosition.x + sr->position.x,
                                     rt.anchoredPosition.y + sr->position.y };
            bits = uilayout::kDrivenByScrollRect;
            return true;
        }
        if (const auto* dd = world.GetComponent<UIDropdownComponent>(p); dd != nullptr && dd->templateRect == e) {
            // Unity の Show: 一覧の高さが中身 (項目の数) より大きければ、余りの分だけ縮める。
            // 前提は Create > UI > Dropdown の構成 = 一覧の縦のアンカーが 1 点 (高さ = sizeDelta.y)、一覧の
            // ScrollRect の viewport は縦に伸縮 (高さ = 一覧の高さ + viewport の sizeDelta.y)、content は
            // 縦の Layout Group で項目を並べる。外れた構成では縮めない
            const auto* list = world.GetComponent<UIScrollRectComponent>(e);
            if (list == nullptr || rt.anchorMin.y != rt.anchorMax.y || !RefAlive(world, list->content)
                || !RefAlive(world, list->viewport)) {
                return false;
            }
            const auto* vrt = world.GetComponent<RectTransformComponent>(list->viewport);
            const auto* group = world.GetComponent<UILayoutGroupComponent>(list->content);
            if (vrt == nullptr || vrt->anchorMin.y != 0.0f || vrt->anchorMax.y != 1.0f || group == nullptr
                || group->kind != uilayout::kLayoutVertical) {
                return false;
            }
            // 中身の高さ = 余白 + 並ぶ項目の高さの和 + 間隔 (Layout Group が子のサイズを制御しない構成の値)
            float contentH = group->padding.y + group->padding.w;
            int count = 0;
            const ComponentTypeId req[] = { UIDropdownItemComponent::sTypeId };
            world.ForEachArchetype(req, [&](Archetype& arch) {
                for (uint32_t row = 0; row < arch.Count(); ++row) {
                    const EntityID it = arch.EntityAt(row);
                    if (world.GetParent(it) != list->content
                        || !uilayout::IsLayoutChild(world, list->content, it)) {
                        continue;
                    }
                    const auto* irt = world.GetComponent<RectTransformComponent>(it);
                    contentH += (irt != nullptr) ? irt->sizeDelta.y : 0.0f;
                    ++count;
                }
            });
            if (count > 1) {
                contentH += group->spacing.y * static_cast<float>(count - 1);
            }
            const float fitted = contentH - vrt->sizeDelta.y;
            if (!(fitted < rt.sizeDelta.y)) {
                return false;
            }
            out = rt;
            out.sizeDelta = { rt.sizeDelta.x, fitted };
            bits = uilayout::kDrivenByDropdown;
            return true;
        }
        if (world.GetComponent<UISliderComponent>(p) != nullptr) {
            return false; // Slider の部分木の中は Slider だけが駆動する (上の SliderDrivenTransform)
        }
        p = world.GetParent(p);
    }
    return false;
}

} // namespace

bool WidgetDrivenTransform(World& world, EntityID e, const RectTransformComponent& rt,
                           RectTransformComponent& out)
{
    uint32_t bits = 0;
    return DriveImpl(world, e, rt, out, bits);
}

uint32_t WidgetDrivenBits(World& world, EntityID e)
{
    const auto* rt = RefAlive(world, e) ? world.GetComponent<RectTransformComponent>(e) : nullptr;
    if (rt == nullptr) {
        return 0u;
    }
    RectTransformComponent unused;
    uint32_t bits = 0;
    DriveImpl(world, e, *rt, unused, bits);
    return bits;
}

float ScrollbarSteppedValue(const UIScrollbarComponent& bar)
{
    float v = bar.value;
    if (!(v == v)) {
        v = 0.0f;
    }
    if (bar.numberOfSteps > 1) {
        const float n = static_cast<float>(bar.numberOfSteps - 1);
        v = RoundHalfEven(v * n) / n;
    }
    return v;
}

EntityID DragScrollTarget(World& world, EntityID pressed)
{
    if (!RefAlive(world, pressed)) {
        return kNullEntity;
    }
    if (world.GetComponent<UISliderComponent>(pressed) != nullptr
        || world.GetComponent<UIScrollbarComponent>(pressed) != nullptr) {
        return kNullEntity; // 自分でドラッグを受けるウィジェット (Unity の IDragHandler を持つ)
    }
    if (!IsInModalScope(world, pressed)) {
        return kNullEntity; // 開いた Dropdown の Blocker はドラッグを受けない
    }
    const EntityID sr = FindSelfOrAncestorWith<UIScrollRectComponent>(world, pressed);
    if (sr.IsNull() || !IsEntityActive(world, sr) || IsUiHidden(world, sr)) {
        return kNullEntity;
    }
    return sr;
}

EntityID WheelScrollTarget(World& world, EntityID hovered)
{
    if (!RefAlive(world, hovered) || !IsInModalScope(world, hovered)) {
        return kNullEntity;
    }
    const EntityID sr = FindSelfOrAncestorWith<UIScrollRectComponent>(world, hovered);
    if (sr.IsNull() || !IsEntityActive(world, sr) || IsUiHidden(world, sr)) {
        return kNullEntity;
    }
    return sr;
}

const char* DropdownOption(const UIDropdownComponent& dd, int i)
{
    if (i < 0 || i >= kDropdownMaxOptions || i >= dd.optionCount) {
        return "";
    }
    const char* const options[kDropdownMaxOptions] = { dd.option0, dd.option1, dd.option2, dd.option3,
                                                        dd.option4, dd.option5, dd.option6, dd.option7 };
    return options[i];
}

bool IsDropdownTemplate(World& world, EntityID e)
{
    if (!RefAlive(world, e)) {
        return false;
    }
    const EntityID dd = FindSelfOrAncestorWith<UIDropdownComponent>(world, world.GetParent(e));
    const UIDropdownComponent* d = DropdownOf(world, dd);
    return d != nullptr && d->templateRect == e;
}

bool IsDropdownItemUnused(World& world, EntityID e)
{
    const auto* item = RefAlive(world, e) ? world.GetComponent<UIDropdownItemComponent>(e) : nullptr;
    if (item == nullptr) {
        return false;
    }
    const UIDropdownComponent* d = DropdownOf(world, OwnerDropdown(world, e));
    return d != nullptr && (item->index < 0 || item->index >= d->optionCount);
}

UiScope ScopeOf(World& world, EntityID e)
{
    UiScope scope;
    if (!RefAlive(world, e)) {
        return scope;
    }
    // 祖先を 1 回だけ辿る。Dropdown に出会ったら、それより下で通った (子孫側の) ノードに一覧があるかを見る
    EntityID chain[kMaxBubbleDepth];
    int depth = 0;
    bool itemSeen = false;
    EntityID n = e;
    while (depth < kMaxBubbleDepth && RefAlive(world, n)) {
        if (const auto* d = world.GetComponent<UIDropdownComponent>(n)) {
            for (int i = 0; i < depth; ++i) {
                if (chain[i] == d->templateRect) {
                    if (!d->expanded) {
                        scope.hidden = true;
                        return scope;
                    }
                    scope.orderBump += kDropdownListOrderBump;
                    break;
                }
            }
        }
        if (!itemSeen && IsDropdownItemUnused(world, n)) {
            scope.hidden = true;
            return scope;
        }
        if (world.GetComponent<UIDropdownItemComponent>(n) != nullptr) {
            itemSeen = true; // 判定は最寄りの項目だけ (項目の中に項目は置かない)
        }
        chain[depth++] = n;
        n = world.GetParent(n);
    }
    return scope;
}

EntityID ExpandedDropdown(World& world)
{
    EntityID best = kNullEntity;
    const ComponentTypeId req[] = { UIDropdownComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int di = arch.FindTypeIndex(UIDropdownComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            const auto* d = static_cast<const UIDropdownComponent*>(arch.GetPtr(di, row));
            if (!d->expanded || !IsEntityActive(world, e)) {
                continue;
            }
            if (best.IsNull() || e.index < best.index) {
                best = e;
            }
        }
    });
    return best;
}

EntityID ApplyModalBlocker(World& world, EntityID under)
{
    const EntityID dd = ExpandedDropdown(world);
    if (dd.IsNull()) {
        return under;
    }
    const UIDropdownComponent* d = DropdownOf(world, dd);
    if (RefAlive(world, under) && IsSelfOrDescendant(world, under, d->templateRect)) {
        return under;
    }
    return dd;
}

bool IsInModalScope(World& world, EntityID e)
{
    const EntityID dd = ExpandedDropdown(world);
    if (dd.IsNull()) {
        return true;
    }
    return IsSelfOrDescendant(world, e, DropdownOf(world, dd)->templateRect);
}

bool IsDropdownItemRoot(World& world, EntityID e)
{
    if (!RefAlive(world, e) || world.GetComponent<UIDropdownItemComponent>(e) == nullptr) {
        return false;
    }
    const UIDropdownComponent* d = DropdownOf(world, OwnerDropdown(world, e));
    return d != nullptr && d->expanded;
}

void Update(World& world, const InputSnapshot& in, const uiinteract::TickEvents& events,
            UIInteractionState& state)
{
    // ---- Toggle: clicked はマウスの「掴んだ要素の上で離した」と Submit の合流先 (Evaluate) ----
    if (RefAlive(world, state.clicked) && IsInteractable(world, state.clicked)
        && world.GetComponent<UIToggleComponent>(state.clicked) != nullptr) {
        ClickToggle(world, state.clicked, state);
    }
    // ---- Dropdown (M75g): Cancel で閉じる / 根のクリックで開閉 / 項目のクリックで選んで閉じる ----
    UpdateDropdowns(world, events, state);
    // ---- Slider / Scrollbar: 掴んでいる間は毎 tick ポインタ位置から値を解く (Unity はドラッグ閾値を使わない) ----
    if (RefAlive(world, state.pressed) && in.MouseDown(0) && IsInteractable(world, state.pressed)) {
        const bool began = events.pressBegan == state.pressed;
        if (world.GetComponent<UISliderComponent>(state.pressed) != nullptr) {
            PointerSlider(world, in, state.pressed, began, state);
        } else if (world.GetComponent<UIScrollbarComponent>(state.pressed) != nullptr) {
            PointerScrollbar(world, in, state.pressed, began, state);
        }
    }
    // ---- Slider / Scrollbar: 向きの軸の UINav* を値の変更として受けた (Evaluate がフォーカスを動かさなかった) ----
    if (events.navStepDir >= 0 && RefAlive(world, events.navStepTarget)
        && IsInteractable(world, events.navStepTarget)) {
        if (world.GetComponent<UISliderComponent>(events.navStepTarget) != nullptr) {
            StepSlider(world, events.navStepTarget, events.navStepDir, state);
        } else if (world.GetComponent<UIScrollbarComponent>(events.navStepTarget) != nullptr) {
            StepScrollbar(world, events.navStepTarget, events.navStepDir, state);
        }
    }
    // ---- ScrollRect (M75g): entity.index の昇順で 1 つずつ進める ----
    // ★入れ子の ScrollRect は外側の位置が内側の矩形に効くので、順序を走査順 (アーキタイプの並び = 構造変更で
    //   入れ替わりうる) に任せず明示的なキーで決める
    std::vector<EntityID> rects;
    {
        const ComponentTypeId req[] = { UIScrollRectComponent::sTypeId };
        world.ForEachArchetype(req, [&](Archetype& arch) {
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                const EntityID e = arch.EntityAt(row);
                if (IsEntityActive(world, e) && !IsUiHidden(world, e)) {
                    rects.push_back(e);
                }
            }
        });
    }
    if (!rects.empty()) {
        std::sort(rects.begin(), rects.end(), [](EntityID a, EntityID b) { return a.index < b.index; });
        const EntityID wheelTarget = (in.wheelDelta != 0) ? WheelScrollTarget(world, state.hovered) : kNullEntity;
        for (const EntityID e : rects) {
            UpdateScrollRect(world, in, events, state, e, wheelTarget);
        }
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
                if (toggle->isOn || !RefAlive(world, toggle->graphic)
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
    // M75g: Dropdown の表題 = 選択中の選択肢の文字 (Unity の RefreshShownValue)
    {
        const ComponentTypeId req[] = { UIDropdownComponent::sTypeId };
        world.ForEachArchetype(req, [&](Archetype& arch) {
            const int di = arch.FindTypeIndex(UIDropdownComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                const auto* d = static_cast<const UIDropdownComponent*>(arch.GetPtr(di, row));
                if (!RefAlive(world, d->captionText) || !IsEntityActive(world, arch.EntityAt(row))) {
                    continue;
                }
                VisualOverride v;
                v.e = d->captionText;
                v.flags = kVisText;
                v.text = DropdownOption(*d, d->value);
                out.push_back(v);
            }
        });
    }
    // M75g: 一覧の項目 = 選択肢 index の文字 + 選択中の印 (Unity の AddItem / Toggle.isOn)
    {
        const ComponentTypeId req[] = { UIDropdownItemComponent::sTypeId };
        world.ForEachArchetype(req, [&](Archetype& arch) {
            const int ii = arch.FindTypeIndex(UIDropdownItemComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                const EntityID e = arch.EntityAt(row);
                const auto* item = static_cast<const UIDropdownItemComponent*>(arch.GetPtr(ii, row));
                const UIDropdownComponent* d = DropdownOf(world, OwnerDropdown(world, e));
                if (d == nullptr || !IsEntityActive(world, e)) {
                    continue;
                }
                if (RefAlive(world, item->label)) {
                    VisualOverride v;
                    v.e = item->label;
                    v.flags = kVisText;
                    v.text = DropdownOption(*d, item->index);
                    out.push_back(v);
                }
                if (RefAlive(world, item->checkmark) && item->index != d->value) {
                    VisualOverride v;
                    v.e = item->checkmark;
                    v.flags = kVisHidden;
                    out.push_back(v);
                }
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
        if ((v.flags & kVisText) != 0) {
            merged.text = v.text;
        }
    }
    return merged;
}

} // namespace uiwidgets
} // namespace mye
