//====================================================================================
//                          UIScrollSelfTest.cpp
//  MyEngine/ 秋田蓮音                                                      10/03/2026
//                                          Scrollbar / ScrollRect / Dropdown（M75g）のヘッドレス回帰テスト
//====================================================================================
#include "Engine/Engine/UI/UISelfTest.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <vector>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/ComponentRegistry.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene/Prefab.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Engine/UI/UIInteraction.h"
#include "Engine/Engine/UI/UILayout.h"
#include "Engine/Engine/UI/UILayoutGroup.h"
#include "Engine/Engine/UI/UINav.h"
#include "Engine/Engine/UI/UIWidgetFactory.h"
#include "Engine/Engine/UI/UIWidgets.h"
#include "Engine/Platform/Input.h"
#include "Engine/Platform/InputActions.h"
#include <nlohmann/json.hpp>

namespace mye {
namespace {

constexpr uint8_t kVkEnter = 0x0D;
constexpr uint8_t kVkEscape = 0x1B;
constexpr uint8_t kVkRight = 0x27;
constexpr uint8_t kVkDown = 0x28;
constexpr int32_t kNotch = 120; // WHEEL_DELTA

// UISelfTest の M75f 節と同じ 1 tick (アクション評価 → Evaluate → Update)。既定キャンバス 1920x1080 を
// ゲーム面 1920x1080 で解くので、ポインタの座標はそのままキャンバス座標
struct Driver {
    InputActions* actions = nullptr;
    InputSnapshot in = {};
    InputSnapshot prev = {};
    UIInteractionState st;

    void Tick(World& w, float x, float y, bool down, uint8_t vk = 0, int32_t wheel = 0)
    {
        std::memset(in.keys, 0, sizeof(in.keys));
        in.surfW = 1920;
        in.surfH = 1080;
        in.mouseSurfX = x;
        in.mouseSurfY = y;
        in.mouseButtons = down ? 1u : 0u;
        in.wheelDelta = wheel;
        if (vk != 0) {
            in.keys[vk >> 3] |= static_cast<uint8_t>(1u << (vk & 7));
        }
        actions->Evaluate(in, prev);
        uiinteract::TickEvents ev;
        uiinteract::Evaluate(w, in, prev, actions, st, &ev);
        uiwidgets::Update(w, in, ev, st);
        prev = in;
    }
    void Click(World& w, float x, float y)
    {
        Tick(w, x, y, false);
        Tick(w, x, y, true);
        Tick(w, x, y, false);
    }
    void Idle(World& w, int ticks, float x = 4.0f, float y = 4.0f)
    {
        for (int i = 0; i < ticks; ++i) {
            Tick(w, x, y, false);
        }
    }
};

// 子構成込みのウィジェットを左上基準 (anchor / pivot = 0) の位置へ置き直す (DemoContent の placeTopLeft と同じ)
void PlaceTopLeft(World& w, EntityID e, float x, float y, float rw, float rh)
{
    auto* rt = w.GetComponent<RectTransformComponent>(e);
    rt->anchorMin = { 0.0f, 0.0f };
    rt->anchorMax = { 0.0f, 0.0f };
    rt->pivot = { 0.0f, 0.0f };
    rt->anchoredPosition = { x, y };
    rt->sizeDelta = { rw, rh };
}

bool Approx(float a, float b, float eps = 1e-3f)
{
    return std::fabs(a - b) < eps;
}

bool RectApprox(const uilayout::UIRect& r, float x, float y, float rw, float rh)
{
    return Approx(r.x, x) && Approx(r.y, y) && Approx(r.w, rw) && Approx(r.h, rh);
}

// 決定論の検査用: 決めた入力の列で Scroll View を動かし、最後の ScrollRect を返す
UIScrollRectComponent RunScrollScript(InputActions& actions)
{
    Scene sc;
    const EntityID root = uiwidgets::CreateScrollView(sc, "Det").Id();
    World& w = sc.GetWorld();
    w.ApplyStructuralChanges();
    Driver d;
    d.actions = &actions;
    d.Tick(w, 960.0f, 500.0f, false, 0, -kNotch);
    d.Tick(w, 960.0f, 500.0f, false, 0, -2 * kNotch);
    d.Tick(w, 960.0f, 600.0f, true);
    for (int i = 1; i <= 8; ++i) {
        d.Tick(w, 960.0f - 3.0f * static_cast<float>(i), 600.0f - 25.0f * static_cast<float>(i), true);
    }
    d.Idle(w, 90);
    return *w.GetComponent<UIScrollRectComponent>(root);
}

} // namespace

bool RunUIScrollSelfTest()
{
    MYE_LOG_INFO("==== UI scroll / dropdown self test (M75g) ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };
    RegisterBuiltinComponents();

    InputActions actions;
    const bool actionsLoaded = actions.LoadFromJsonText(R"({"actions":[
        {"name":"UINavUp","keys":["Up"],"pad":[],"mouse":[]},
        {"name":"UINavDown","keys":["Down"],"pad":[],"mouse":[]},
        {"name":"UINavLeft","keys":["Left"],"pad":[],"mouse":[]},
        {"name":"UINavRight","keys":["Right"],"pad":[],"mouse":[]},
        {"name":"UINavSubmit","keys":["Enter"],"pad":[],"mouse":[]},
        {"name":"UINavCancel","keys":["Escape"],"pad":[],"mouse":[]}],"axes":[]})");
    check(actionsLoaded, "scroll: the navigation action map for the tests loads");

    // (1) 登録: 値を持つ Scrollbar / ScrollRect / Dropdown はハッシュ対象、項目は NoHash。全部 UiAux
    {
        const ComponentRegistry& reg = ComponentRegistry::Get();
        const auto flagsOf = [&reg](const char* name) -> uint32_t {
            const ComponentTypeId t = reg.FindByName(name);
            return (t == kInvalidComponentType) ? 0xFFFFFFFFu : reg.Desc(t).flags;
        };
        const auto hashedAux = [&](const char* name) {
            const uint32_t f = flagsOf(name);
            return f != 0xFFFFFFFFu && (f & kComponentNoHash) == 0 && (f & kComponentUiAux) != 0;
        };
        const uint32_t itemFlags = flagsOf("UIDropdownItem");
        check(hashedAux("UIScrollbar") && hashedAux("UIScrollRect") && hashedAux("UIDropdown")
                  && itemFlags != 0xFFFFFFFFu && (itemFlags & kComponentNoHash) != 0
                  && (itemFlags & kComponentUiAux) != 0,
              "scroll: Scrollbar / ScrollRect / Dropdown are hashed, DropdownItem is not, all are UiAux");
    }

    // (2) Scrollbar: つまみのアンカーは value / size から。溝を押すとつまみの中心が飛び、つまみを掴めば
    //     ずれを保って追従。段数で丸め、向きの軸のキーで 1 段動く
    {
        Scene sc;
        const EntityID root = uiwidgets::CreateScrollbar(sc, "Bar", uiwidgets::kSliderLeftToRight).Id();
        World& w = sc.GetWorld();
        w.ApplyStructuralChanges();
        const EntityID handle = w.GetComponent<UIScrollbarComponent>(root)->handleRect;
        const auto bar = [&]() { return w.GetComponent<UIScrollbarComponent>(root); };
        // 溝 (880,530)-(1040,550)、Sliding Area は x 890..1030。value 0 / size 0.2 のつまみ = 幅 140*0.2 + 20
        check(RectApprox(uilayout::ResolveRect(w, handle, 1920, 1080), 880.0f, 530.0f, 48.0f, 20.0f)
                  && (uilayout::LayoutDrivenBits(w, handle) & uilayout::kDrivenByScrollbar) != 0,
              "scroll: the scrollbar handle spans size of the track at value 0 (driven, not written)");
        Driver d;
        d.actions = &actions;
        d.Tick(w, 1000.0f, 540.0f, false);
        d.Tick(w, 1000.0f, 540.0f, true);
        // 中心 110 → (110 - 14) / 112
        check(Approx(bar()->value, 96.0f / 112.0f) && d.st.changed == root,
              "scroll: pressing the track jumps the handle centre to the pointer");
        d.Tick(w, 1000.0f, 540.0f, false);
        const uilayout::UIRect hr = uilayout::ResolveRect(w, handle, 1920, 1080);
        // つまみの中心から右へ 5 ずらして掴み、左へ 40 動かす = 中心も 40 左へ (値は 40 / 112 減る)
        const float grabX = hr.x + hr.w * 0.5f + 5.0f;
        const float before = bar()->value;
        d.Tick(w, grabX, 540.0f, true);
        check(bar()->value == before && Approx(bar()->dragOffset.x, 5.0f),
              "scroll: grabbing the handle keeps the value and remembers the offset");
        d.Tick(w, grabX - 40.0f, 540.0f, true);
        check(Approx(bar()->value, before - 40.0f / 112.0f), "scroll: dragging the handle follows the pointer");
        d.Tick(w, grabX - 40.0f, 540.0f, false);
        bar()->numberOfSteps = 5;
        bar()->value = 0.3f;
        check(Approx(uiwidgets::ScrollbarSteppedValue(*bar()), 0.25f),
              "scroll: numberOfSteps snaps the read value (5 steps = quarters)");
        bar()->numberOfSteps = 0;
        bar()->value = 0.5f;
        d.Tick(w, 4.0f, 4.0f, false, kVkRight); // フォーカスは押したときに取っている
        check(d.st.focused == root && Approx(bar()->value, 0.6f),
              "scroll: a horizontal scrollbar takes Right as a step of 0.1");
    }

    // (3) ScrollRect: Create > UI > Scroll View (中央)。根 (760,390)-(1160,690)、見える範囲 (760,390) 380x280、
    //     中身 380x600 (上端揃え)。縦のバー (1140,390) 20x280、横のバー (760,670) 380x20
    {
        Scene sc;
        const EntityID root = uiwidgets::CreateScrollView(sc, "Scroll").Id();
        World& w = sc.GetWorld();
        w.ApplyStructuralChanges();
        const auto sr = [&]() { return w.GetComponent<UIScrollRectComponent>(root); };
        const EntityID content = sr()->content;
        const EntityID vbar = sr()->verticalScrollbar;
        const EntityID hbar = sr()->horizontalScrollbar;
        Driver d;
        d.actions = &actions;
        d.Tick(w, 960.0f, 500.0f, false);
        check(Approx(w.GetComponent<UIScrollbarComponent>(vbar)->value, 1.0f)
                  && Approx(w.GetComponent<UIScrollbarComponent>(vbar)->size, 280.0f / 600.0f)
                  && Approx(w.GetComponent<UIScrollbarComponent>(hbar)->size, 1.0f)
                  && sr()->position.y == 0.0f && d.st.changed == kNullEntity,
              "scroll: at rest the vertical bar shows the top (value 1) and the view / content ratio");
        d.Tick(w, 960.0f, 500.0f, false, 0, -kNotch);
        check(sr()->position.y == -40.0f && d.st.changed == root
                  && RectApprox(uilayout::ResolveRect(w, content, 1920, 1080), 760.0f, 350.0f, 380.0f, 600.0f)
                  && Approx(w.GetComponent<UIScrollbarComponent>(vbar)->value, 0.875f),
              "scroll: one wheel notch toward you moves the content up by scrollSensitivity (driven position)");
        check((uilayout::LayoutDrivenBits(w, content) & uilayout::kDrivenByScrollRect) != 0
                  && w.GetComponent<RectTransformComponent>(content)->anchoredPosition.y == 0.0f,
              "scroll: the content's RectTransform is not written (the scroll position is added when resolving)");
        // Elastic: 上端を越えて回すとバネで戻る (ホイール直後は時定数 3 倍)
        d.Tick(w, 960.0f, 500.0f, false, 0, 2 * kNotch);
        const float overshoot = sr()->position.y;
        check(overshoot > 0.0f && overshoot < 40.0f,
              "scroll: elastic lets the wheel overshoot the top and starts springing back in the same tick");
        d.Idle(w, 120, 960.0f, 500.0f);
        check(std::fabs(sr()->position.y) < 0.01f && sr()->velocity.y == 0.0f,
              "scroll: elastic springs back to the edge and stops");
        // Clamped: 端で止まる
        sr()->movementType = uiwidgets::kScrollClamped;
        sr()->position = { 0.0f, 0.0f };
        d.Tick(w, 960.0f, 500.0f, false, 0, 2 * kNotch);
        check(sr()->position.y == 0.0f, "scroll: clamped never overshoots the top");
        sr()->movementType = uiwidgets::kScrollElastic;

        // ドラッグ: 閾値を越えた tick から ScrollRect が引き取り、離してもクリックにならず、慣性で滑ってから
        // 下端を越えた分を Elastic で戻す
        d.Tick(w, 960.0f, 600.0f, false);
        d.Tick(w, 960.0f, 600.0f, true);
        d.Tick(w, 960.0f, 590.0f, true); // ちょうど 10 = 閾値以下
        check(d.st.dragging == 0 && sr()->dragging == 0, "scroll: no drag before the threshold");
        d.Tick(w, 960.0f, 580.0f, true);
        check(d.st.dragging == 1 && d.st.pressed == root && sr()->dragging != 0 && sr()->position.y == 0.0f,
              "scroll: past the threshold the scroll rect takes the press and the drag starts there");
        d.Tick(w, 960.0f, 500.0f, true);
        d.Tick(w, 960.0f, 400.0f, true);
        check(sr()->position.y == -180.0f, "scroll: dragging moves the content by the pointer delta");
        d.Tick(w, 960.0f, 400.0f, false);
        check(d.st.clicked == kNullEntity && sr()->dragging == 0 && sr()->position.y < -180.0f
                  && sr()->velocity.y < 0.0f,
              "scroll: releasing a drag is not a click and the content keeps sliding (inertia)");
        d.Idle(w, 300, 960.0f, 400.0f);
        check(Approx(sr()->position.y, -320.0f, 0.01f) && sr()->velocity.y == 0.0f,
              "scroll: the inertia overshoots the bottom and elastic brings it back to the edge");

        // スクロールバー → ScrollRect: 中身は下端 (つまみも下端) なので、縦のバーの上端 (溝の外) を押すと値 1 =
        // 中身の上端が見える。押したまま下端まで動かすと値 0 = 中身の下端
        d.Tick(w, 1150.0f, 395.0f, true);
        check(d.st.changed == vbar && Approx(sr()->position.y, 0.0f, 0.01f)
                  && Approx(w.GetComponent<UIScrollbarComponent>(vbar)->value, 1.0f),
              "scroll: operating the scrollbar sets the normalized position");
        d.Tick(w, 1150.0f, 665.0f, true);
        check(Approx(sr()->position.y, -320.0f, 0.01f),
              "scroll: dragging the scrollbar to the bottom scrolls the content to the bottom");
        d.Tick(w, 1150.0f, 665.0f, false);
    }

    // (4) 中身の中のボタン: 押して離せばクリック、押して閾値を越えて動かせば ScrollRect がドラッグを取り
    //     クリックにならない (Unity の eligibleForClick)
    {
        Scene sc;
        const EntityID root = uiwidgets::CreateScrollView(sc, "Scroll").Id();
        World& w = sc.GetWorld();
        w.ApplyStructuralChanges();
        GameObject content(&w, w.GetComponent<UIScrollRectComponent>(root)->content);
        GameObject toggle = uiwidgets::CreateToggle(sc, "InnerToggle", "L");
        PlaceTopLeft(w, toggle.Id(), 10.0f, 10.0f, 160.0f, 40.0f);
        toggle.SetParent(content);
        w.ApplyStructuralChanges();
        const auto isOn = [&]() { return w.GetComponent<UIToggleComponent>(toggle.Id())->isOn; };
        Driver d;
        d.actions = &actions;
        d.Click(w, 790.0f, 420.0f);
        check(!isOn(), "scroll: a toggle inside the content still clicks");
        d.Tick(w, 790.0f, 420.0f, true);
        d.Tick(w, 790.0f, 450.0f, true);
        d.Tick(w, 790.0f, 470.0f, true);
        d.Tick(w, 790.0f, 470.0f, false);
        check(!isOn() && w.GetComponent<UIScrollRectComponent>(root)->position.y > 0.0f,
              "scroll: dragging from a toggle scrolls instead of clicking it");
    }

    // (5) 決定論: 同じ入力の列は同じ ScrollRect の状態になる (慣性 / Elastic の float 列を含む)
    {
        const UIScrollRectComponent a = RunScrollScript(actions);
        const UIScrollRectComponent b = RunScrollScript(actions);
        check(std::memcmp(&a, &b, sizeof(a)) == 0 && a.position.y != 0.0f,
              "scroll: the same inputs give the same scroll state bit for bit");
    }

    // (6) Dropdown: Create > UI > Dropdown (中央)。根 (880,520)-(1040,560)、一覧は下辺の 2 下 (y 562)。
    //     選択肢 3 つ = 一覧の高さ 4 + 3*28 + 4 = 92 に縮む。項目 i は y 566 + 28i
    {
        Scene sc;
        const EntityID dd = uiwidgets::CreateDropdown(sc, "Dropdown").Id();
        GameObject toggle = uiwidgets::CreateToggle(sc, "OtherToggle", "L");
        World& w = sc.GetWorld();
        w.ApplyStructuralChanges();
        w.GetComponent<RectTransformComponent>(toggle.Id())->anchoredPosition = { 0.0f, 260.0f };
        const auto ddc = [&]() { return w.GetComponent<UIDropdownComponent>(dd); };
        const EntityID tmpl = ddc()->templateRect;
        const EntityID caption = ddc()->captionText;
        std::vector<EntityID> items(kDropdownMaxOptions, kNullEntity);
        {
            const ComponentTypeId req[] = { UIDropdownItemComponent::sTypeId };
            w.ForEachArchetype(req, [&](Archetype& arch) {
                const int ii = arch.FindTypeIndex(UIDropdownItemComponent::sTypeId);
                for (uint32_t row = 0; row < arch.Count(); ++row) {
                    const auto* it = static_cast<const UIDropdownItemComponent*>(arch.GetPtr(ii, row));
                    items[static_cast<size_t>(it->index)] = arch.EntityAt(row);
                }
            });
        }
        check(uiwidgets::IsUiHidden(w, tmpl) && uiwidgets::IsUiHidden(w, items[0])
                  && uiinteract::HitTest(w, 1920, 1080, 950.0f, 608.0f) == kNullEntity
                  && !uiwidgets::IsUiHidden(w, caption),
              "dropdown: a closed list is neither drawn nor hit");
        std::vector<uiwidgets::VisualOverride> ov;
        uiwidgets::CollectVisualOverrides(w, nullptr, ov);
        check(uiwidgets::MergedOverrideFor(ov, caption).text == "Option A"
                  && (uiwidgets::MergedOverrideFor(ov, caption).flags & uiwidgets::kVisText) != 0,
              "dropdown: the caption shows the selected option");

        Driver d;
        d.actions = &actions;
        d.Click(w, 960.0f, 540.0f);
        check(ddc()->expanded && d.st.focused == items[0] && !uiwidgets::IsUiHidden(w, items[2])
                  && uiwidgets::IsUiHidden(w, items[3])
                  && uiwidgets::ScopeOf(w, items[0]).orderBump == uiwidgets::kDropdownListOrderBump,
              "dropdown: clicking opens the list, focuses the selected item and hides the unused items");
        check(RectApprox(uilayout::ResolveRect(w, tmpl, 1920, 1080), 880.0f, 562.0f, 160.0f, 92.0f)
                  && RectApprox(uilayout::ResolveRect(w, items[1], 1920, 1080), 880.0f, 594.0f, 140.0f, 28.0f)
                  && (uilayout::LayoutDrivenBits(w, tmpl) & uilayout::kDrivenByDropdown) != 0,
              "dropdown: the list shrinks to its options and the items stack under the button");
        // 一覧の背景 (上の余白 y 562..566) は一覧の中 = Blocker にも Dropdown の根にもならず、押しても閉じない
        d.Click(w, 950.0f, 564.0f);
        check(ddc()->expanded && d.st.hovered != dd && uiwidgets::BubbleTarget(w, tmpl) == tmpl,
              "dropdown: pressing the list background does not bubble to the dropdown or close it");
        d.Tick(w, 950.0f, 608.0f, false);
        check(d.st.hovered == items[1] && d.st.focused == items[1],
              "dropdown: moving onto an item hovers it and moves the focus there");
        d.Tick(w, 950.0f, 608.0f, true);
        d.Tick(w, 950.0f, 608.0f, false);
        check(ddc()->value == 1 && !ddc()->expanded && d.st.changed == dd && d.st.focused == dd,
              "dropdown: clicking an item selects it, closes the list and focuses the dropdown");
        uiwidgets::CollectVisualOverrides(w, &d.st, ov);
        {
            const auto* it1 = w.GetComponent<UIDropdownItemComponent>(items[1]);
            const auto* it0 = w.GetComponent<UIDropdownItemComponent>(items[0]);
            check(uiwidgets::MergedOverrideFor(ov, caption).text == "Option B"
                      && uiwidgets::MergedOverrideFor(ov, it1->label).text == "Option B"
                      && (uiwidgets::MergedOverrideFor(ov, it1->checkmark).flags & uiwidgets::kVisHidden) == 0
                      && (uiwidgets::MergedOverrideFor(ov, it0->checkmark).flags & uiwidgets::kVisHidden) != 0,
                  "dropdown: item labels show their option and only the selected item shows the check mark");
        }
        // キー: 開く → 下 → Enter
        d.Click(w, 960.0f, 540.0f);
        check(d.st.focused == items[1], "dropdown: reopening focuses the selected item");
        d.Tick(w, 960.0f, 540.0f, false, kVkDown);
        d.Tick(w, 960.0f, 540.0f, false);
        check(d.st.focused == items[2], "dropdown: Down moves the focus to the next item");
        d.Tick(w, 960.0f, 540.0f, false, kVkEnter);
        check(ddc()->value == 2 && !ddc()->expanded, "dropdown: Submit on an item selects it");
        d.Tick(w, 960.0f, 540.0f, false);
        // Cancel
        d.Click(w, 960.0f, 540.0f);
        d.Tick(w, 960.0f, 540.0f, false, kVkEscape);
        check(!ddc()->expanded && ddc()->value == 2 && d.st.focused == dd,
              "dropdown: Cancel closes the list without changing the value");
        d.Tick(w, 960.0f, 540.0f, false);
        // Blocker: 開いている間は一覧の外 (他のウィジェットの上を含む) を押すと閉じるだけ
        d.Click(w, 960.0f, 540.0f);
        const bool toggleBefore = w.GetComponent<UIToggleComponent>(toggle.Id())->isOn;
        d.Tick(w, 900.0f, 800.0f, false);
        check(d.st.hovered == dd, "dropdown: while open, anything outside the list hovers the dropdown (blocker)");
        d.Tick(w, 900.0f, 800.0f, true);
        d.Tick(w, 900.0f, 800.0f, false);
        check(!ddc()->expanded && w.GetComponent<UIToggleComponent>(toggle.Id())->isOn == toggleBefore,
              "dropdown: clicking outside closes the list and the widget underneath is not clicked");
        // 開いている間のナビは一覧の中だけ
        // (選択中は項目 2 = 一覧の最後。その下の Toggle へは降りない)
        d.Click(w, 960.0f, 540.0f);
        check(d.st.focused == items[2]
                  && uiinteract::FindNextFocus(w, 1920, 1080, items[2], uinav::kNavDown) == items[2]
                  && uiinteract::FindNextFocus(w, 1920, 1080, items[2], uinav::kNavUp) == items[1],
              "dropdown: while open the focus cannot leave the list");
        d.Tick(w, 960.0f, 540.0f, false, kVkEscape);
        d.Tick(w, 960.0f, 540.0f, false);

        // ハッシュ: 値は載り、項目 (見た目の入力) は載らない
        const uint64_t h0 = HashWorld(w);
        w.GetComponent<UIDropdownItemComponent>(items[7])->checkmark = kNullEntity;
        const uint64_t h1 = HashWorld(w);
        ddc()->value = 0;
        const uint64_t h2 = HashWorld(w);
        check(h0 == h1 && h1 != h2, "dropdown: the value is in the world hash, the items are not");
    }

    // (7) クリップ: クリップする親の下のドロップダウンでも、開いた一覧は親に切られない
    {
        Scene sc;
        GameObject clip = sc.CreateGameObjectTracked("Clip");
        World& w = sc.GetWorld();
        {
            auto* rt = clip.AddComponent<RectTransformComponent>();
            rt->anchorMin = { 0.5f, 0.5f };
            rt->anchorMax = { 0.5f, 0.5f };
            rt->pivot = { 0.5f, 0.5f };
            rt->sizeDelta = { 200.0f, 60.0f };
        }
        clip.AddComponent<UIElementComponent>()->clipChildren = true;
        GameObject ddGo = uiwidgets::CreateDropdown(sc, "Clipped");
        ddGo.SetParent(clip);
        w.ApplyStructuralChanges();
        w.GetComponent<UIDropdownComponent>(ddGo.Id())->expanded = true;
        EntityID item0 = kNullEntity;
        const ComponentTypeId req[] = { UIDropdownItemComponent::sTypeId };
        w.ForEachArchetype(req, [&](Archetype& arch) {
            const int ii = arch.FindTypeIndex(UIDropdownItemComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                if (static_cast<const UIDropdownItemComponent*>(arch.GetPtr(ii, row))->index == 0) {
                    item0 = arch.EntityAt(row);
                }
            }
        });
        // 一覧の Viewport (880,562) 140x92 だけが効き、親 (860,510)-(1060,570) には切られない
        check(RectApprox(uilayout::ResolveClipRect(w, item0, 1920, 1080), 880.0f, 562.0f, 140.0f, 92.0f),
              "dropdown: an open list escapes the clipping of its ancestors (only its own viewport clips)");
    }

    // (8) プレハブ往復: インスタンスごとに EntityRef が自分の子孫へ付け替わる
    {
        PrefabLibrary lib;
        const std::filesystem::path tmpDir = std::filesystem::temp_directory_path();
        for (int kind = 0; kind < 2; ++kind) {
            Scene base;
            const EntityID baseRoot = (kind == 0) ? uiwidgets::CreateScrollView(base, "PfScroll").Id()
                                                  : uiwidgets::CreateDropdown(base, "PfDropdown").Id();
            base.GetWorld().ApplyStructuralChanges();
            const nlohmann::json local = Prefab::ExtractLocal(base, baseRoot);
            const std::wstring path = (tmpDir / ((kind == 0) ? L"mye_selftest_ui_scroll.prefab.json"
                                                             : L"mye_selftest_ui_dropdown.prefab.json")).wstring();
            const uint64_t hash = lib.Register(path, (kind == 0) ? "scroll" : "dropdown", local);
            Scene inst;
            const uint64_t r1 = Prefab::Instantiate(inst, lib, hash, 0);
            const uint64_t r2 = Prefab::Instantiate(inst, lib, hash, 0);
            World& w = inst.GetWorld();
            w.ApplyStructuralChanges();
            const EntityID e1 = inst.FindByFileId(r1).Id();
            const EntityID e2 = inst.FindByFileId(r2).Id();
            const auto under = [&w](EntityID ref, EntityID root, int depth) {
                if (ref.IsNull() || !w.IsAlive(ref)) {
                    return false;
                }
                EntityID p = ref;
                for (int i = 0; i < depth; ++i) {
                    p = w.GetParent(p);
                }
                return p == root;
            };
            bool ok = r1 != 0 && r2 != 0 && !e1.IsNull() && !e2.IsNull() && e1 != e2;
            for (const EntityID e : { e1, e2 }) {
                if (!ok) {
                    break;
                }
                if (kind == 0) {
                    const auto* sr = w.GetComponent<UIScrollRectComponent>(e);
                    ok = sr && under(sr->content, e, 2) && under(sr->viewport, e, 1)
                        && under(sr->horizontalScrollbar, e, 1) && under(sr->verticalScrollbar, e, 1);
                } else {
                    const auto* d = w.GetComponent<UIDropdownComponent>(e);
                    const auto* list = d ? w.GetComponent<UIScrollRectComponent>(d->templateRect) : nullptr;
                    ok = d && list && under(d->templateRect, e, 1) && under(d->captionText, e, 1)
                        && under(list->content, e, 3);
                }
            }
            check(ok, (kind == 0)
                          ? "scroll: each scroll view prefab instance points content / viewport / bars at its own children"
                          : "dropdown: each dropdown prefab instance points template / caption / list at its own children");
        }
    }

    if (failCount == 0) {
        MYE_LOG_INFO("==== UI scroll / dropdown self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== UI scroll / dropdown self test: %d FAILURE(S) ====", failCount);
    return false;
}

} // namespace mye
