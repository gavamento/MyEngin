#include "Engine/Engine/UI/UIInteraction.h"

#include <vector>

#include "Engine/Core/Components.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/UI/UILayout.h"
#include "Engine/Engine/UI/UILayoutGroup.h" // M75e: LayoutScratch
#include "Engine/Engine/UI/UINav.h"
#include "Engine/Engine/UI/UIWidgets.h" // M75f: 泡立ち / interactable / Navigation / Slider の軸
#include "Engine/Platform/Input.h"
#include "Engine/Platform/InputActions.h"

namespace mye {
namespace uiinteract {
namespace {

// 全 active UIElement を走査する共通形。ワールド追従 UI の射影コンテキストは
// **sim レーン用の決定論構築** (BuildSimWorldContext) を 1 回だけ組んで使い回す。
// M75e: 自動レイアウトのメモも走査 1 回ぶん共有する (結果は変えない。World はこの間書き換えない)
template <typename F>
void ForEachUiElement(World& world, int canvasW, int canvasH, F&& fn)
{
    uilayout::UIWorldContext wcData;
    const uilayout::UIWorldContext* wc =
        uilayout::BuildSimWorldContext(world, canvasW, canvasH, wcData) ? &wcData : nullptr;
    uilayout::LayoutScratch scratch;
    const ComponentTypeId req[] = { UIElementComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int ci = arch.FindTypeIndex(UIElementComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            if (!IsEntityActive(world, e)) {
                continue;
            }
            fn(e, *static_cast<const UIElementComponent*>(arch.GetPtr(ci, row)), wc, scratch);
        }
    });
}

} // namespace

EntityID HitTest(World& world, int canvasW, int canvasH, float pointX, float pointY)
{
    EntityID best = kNullEntity;
    int32_t bestSort = 0;
    int32_t bestOrder = 0;
    bool have = false;
    ForEachUiElement(world, canvasW, canvasH,
                     [&](EntityID e, const UIElementComponent& el,
                         const uilayout::UIWorldContext* wc, uilayout::LayoutScratch& scratch) {
                         const uilayout::UIResolved res =
                             uilayout::Resolve(world, e, canvasW, canvasH, wc, &scratch);
                         if (!res.visible) {
                             return;
                         }
                         // M75c: 点は既定キャンバス座標で来る。要素の属するキャンバスの単位へ
                         // 直してから判定する (Canvas の無い要素は 1.0f で割る = ビット恒等)
                         const EntityID canvasE = uilayout::FindCanvas(world, e);
                         const uilayout::CanvasInfo cv =
                             uilayout::CanvasOfEntity(world, canvasE, canvasW, canvasH);
                         const float x = pointX / cv.scale;
                         const float y = pointY / cv.scale;
                         if (!res.hasXform) {
                             // 可視矩形 (祖先クリップ適用済み) で判定 — 見えない部分には当たらない
                             // (ResolveVisibleRect と同じ式)
                             const auto vis = uilayout::Intersect(
                                 res.rect,
                                 uilayout::ResolveClipRect(world, e, canvasW, canvasH, wc,
                                                           &scratch));
                             if (vis.w <= 0.0f || vis.h <= 0.0f || x < vis.x
                                 || x >= vis.x + vis.w || y < vis.y || y >= vis.y + vis.h) {
                                 return;
                             }
                         } else {
                             // 回転/スケール (M75a): 点を要素のフレームへ逆変換して軸平行矩形で
                             // 判定する (Unity と同じ)。クリップは祖先の AABB で近似 — 描画側の
                             // シザーも同じ AABB なので「見えているのに押せない」は起きない
                             const auto clip = uilayout::ResolveClipRect(world, e, canvasW,
                                                                         canvasH, wc, &scratch);
                             if (clip.w <= 0.0f || clip.h <= 0.0f || x < clip.x
                                 || x >= clip.x + clip.w || y < clip.y || y >= clip.y + clip.h) {
                                 return;
                             }
                             uilayout::UIXform inv;
                             if (!uilayout::InvertXform(res.xform, inv)) {
                                 return; // scale 0 = 面積ゼロ = 当たらない
                             }
                             float lx = 0.0f, ly = 0.0f;
                             uilayout::XformPoint(inv, x, y, lx, ly);
                             const uilayout::UIRect& r = res.rect;
                             if (r.w <= 0.0f || r.h <= 0.0f || lx < r.x || lx >= r.x + r.w
                                 || ly < r.y || ly >= r.y + r.h) {
                                 return;
                             }
                         }
                         // 最前面 = (キャンバスの sortOrder, order, entity.index) の最大 (M75c)。
                         // UIRenderer の描画順と同じキー
                         const int32_t sortOrder = uilayout::CanvasSortOrder(world, canvasE);
                         if (!have || sortOrder > bestSort
                             || (sortOrder == bestSort
                                 && (el.order > bestOrder
                                     || (el.order == bestOrder && e.index > best.index)))) {
                             best = e;
                             bestSort = sortOrder;
                             bestOrder = el.order;
                             have = true;
                         }
                     });
    return best;
}

EntityID FindNextFocus(World& world, int canvasW, int canvasH, EntityID current, int dir)
{
    std::vector<uinav::NavRect> rects;
    std::vector<EntityID> ids;
    uinav::NavRect cur = {};
    bool haveCur = false;
    uilayout::UIWorldContext wcData;
    const uilayout::UIWorldContext* wc =
        uilayout::BuildSimWorldContext(world, canvasW, canvasH, wcData) ? &wcData : nullptr;
    uilayout::LayoutScratch scratch;
    // 候補の判定は uiwidgets::IsFocusCandidate の 1 本 (旧来の要素は UIElement.focusable、
    // ウィジェットの根は「操作可能 && navigationMode != None」、M75f)
    const auto consider = [&](EntityID e) {
        if (!IsEntityActive(world, e) || !uiwidgets::IsFocusCandidate(world, e)) {
            return;
        }
        // 祖先クリップで完全に隠れた要素は候補から外す
        // (スクロール範囲外の項目へ飛ばない)
        const auto vis = uilayout::ResolveVisibleRect(world, e, canvasW, canvasH, wc, &scratch);
        if (vis.w <= 0.0f || vis.h <= 0.0f) {
            return;
        }
        const auto rect = uilayout::ResolveRect(world, e, canvasW, canvasH, wc, &scratch);
        // M75c: 候補の比較は既定キャンバス座標で行う (キャンバスごとに単位が
        // 違うので、そのままでは別キャンバスの要素と距離を比べられない)。
        // Canvas の無い要素は 1.0f を掛ける = ビット恒等
        const float toDefault = uilayout::CanvasOf(world, e, canvasW, canvasH).scale;
        uinav::NavRect r;
        r.x = rect.x * toDefault;
        r.y = rect.y * toDefault;
        r.w = rect.w * toDefault;
        r.h = rect.h * toDefault;
        r.index = e.index;
        if (e == current) {
            cur = r;
            haveCur = true;
        }
        rects.push_back(r);
        ids.push_back(e);
    };
    {
        const ComponentTypeId req[] = { UIElementComponent::sTypeId };
        world.ForEachArchetype(req, [&](Archetype& arch) {
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                consider(arch.EntityAt(row));
            }
        });
    }
    // M75f: UIElement を持たないウィジェットの根 (Toggle / Slider。見た目は子が持つ) も候補になる。
    // 1 つのエンティティを 2 回数えないよう「持っている型のうち並びの先頭」の走査でだけ拾う
    // (並べ方は結果に効かない — 吸着は index 最小、FindNext の同点も index で決まる)
    {
        const ComponentTypeId widgetTypes[] = { UISelectableComponent::sTypeId,
                                                UIToggleComponent::sTypeId,
                                                UISliderComponent::sTypeId };
        for (int ti = 0; ti < 3; ++ti) {
            const ComponentTypeId req[] = { widgetTypes[ti] };
            world.ForEachArchetype(req, [&](Archetype& arch) {
                if (arch.FindTypeIndex(UIElementComponent::sTypeId) >= 0) {
                    return; // 上の走査で見た
                }
                for (int prev = 0; prev < ti; ++prev) {
                    if (arch.FindTypeIndex(widgetTypes[prev]) >= 0) {
                        return; // 前の型の走査で見た
                    }
                }
                for (uint32_t row = 0; row < arch.Count(); ++row) {
                    consider(arch.EntityAt(row));
                }
            });
        }
    }
    if (rects.empty()) {
        return kNullEntity; // 候補が 1 つも無い = フォーカスは持てない
    }
    if (!haveCur) {
        // ★フォーカス不在 (または現フォーカスが消えた) からの入りぐち。
        //   「最初の候補」= 走査順の先頭ではなく **entity.index 最小** にする —
        //   アーキタイプの走査順は構造変更で入れ替わりうるので、そこに依存すると
        //   「同じシーンなのに実行ごとに最初のフォーカスが違う」ことが起こりうる
        EntityID first = ids[0];
        for (const EntityID e : ids) {
            if (e.index < first.index) {
                first = e;
            }
        }
        return first;
    }
    // ---- M75f: 今のフォーカスの Navigation (Unity の Navigation.Mode) ----
    // 旧来の要素 (Selectable 無し) は常に自動 = M70c と同じ動き
    int mode = uiwidgets::kNavAutomatic;
    if (uiwidgets::IsWidgetRoot(world, current)) {
        const UISelectableComponent& sel = uiwidgets::SelectableOf(world, current);
        mode = sel.navigationMode;
        if (mode == uiwidgets::kNavExplicit) {
            const EntityID target = (dir == uinav::kNavUp)     ? sel.selectOnUp
                : (dir == uinav::kNavDown)                     ? sel.selectOnDown
                : (dir == uinav::kNavLeft)                     ? sel.selectOnLeft
                                                               : sel.selectOnRight;
            // 明示の先も候補 (Active・操作可能・見えている) に限る。外れていれば動かない
            for (const EntityID e : ids) {
                if (e == target) {
                    return target;
                }
            }
            return current;
        }
    }
    const bool vertical = (dir == uinav::kNavUp || dir == uinav::kNavDown);
    if ((mode & (vertical ? uiwidgets::kNavVertical : uiwidgets::kNavHorizontal)) == 0) {
        return current; // 左右だけ / 上下だけのモードで、許されていない向き
    }
    const uint32_t next = uinav::FindNext(rects.data(), static_cast<int>(rects.size()), cur, dir);
    for (size_t i = 0; i < ids.size(); ++i) {
        if (ids[i].index == next) {
            return ids[i];
        }
    }
    return current;
}

uint32_t BitsFor(const UIInteractionState& state, EntityID e)
{
    if (e == kNullEntity) {
        return 0u;
    }
    uint32_t bits = 0u;
    if (state.hovered == e) { bits |= kHovered; }
    if (state.pressed == e) { bits |= kPressed; }
    if (state.clicked == e) { bits |= kClicked; }
    if (state.focused == e) { bits |= kFocused; }
    return bits;
}

void Evaluate(World& world, const InputSnapshot& in, const InputSnapshot& prevIn,
              const InputActions* actions, UIInteractionState& state, TickEvents* events)
{
    (void)prevIn; // M75h (InputField のキーエッジ) が読む。配線だけ先に通してある
    TickEvents localEvents;
    TickEvents& ev = (events != nullptr) ? *events : localEvents;
    ev = TickEvents{};
    // clicked / changed は 1 tick だけ立つ値。ここで必ず落とす (立てるのは下の「離した」判定と
    // M75f 以降のウィジェット更新だけ)
    state.clicked = kNullEntity;
    state.changed = kNullEntity;

    // キャンバスは入力に記録されたゲーム面から解く (M75b)。0 = 未確定 (ヘッドレス / 旧い記録) は
    // 基準解像度へ倒れる — ABI の UiCanvasOf / MouseCanvasPos と同じ関数を通す
    const uilayout::CanvasInfo canvas = uilayout::CanvasOfInput(in);
    const int canvasW = canvas.w;
    const int canvasH = canvas.h;

    // ---- シーンが書いた focused を一度だけ拾う ----
    // 起動直後 / シーン遷移直後の 1 回だけ。候補が複数あれば entity.index 最小
    // (走査順に依存しない = FindNextFocus の入りぐちと同じ規則)
    if (state.adoptedAuthored == 0) {
        state.adoptedAuthored = 1;
        EntityID authored = kNullEntity;
        const ComponentTypeId req[] = { UIElementComponent::sTypeId };
        world.ForEachArchetype(req, [&](Archetype& arch) {
            const int ci = arch.FindTypeIndex(UIElementComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                const auto* el = static_cast<const UIElementComponent*>(arch.GetPtr(ci, row));
                if (el->focused == 0) {
                    continue;
                }
                const EntityID e = arch.EntityAt(row);
                // M75f: 候補の規則は FindNextFocus と同じ 1 本 (旧来の要素は focusable のまま)
                if (!uiwidgets::IsFocusCandidate(world, e) || !IsEntityActive(world, e)) {
                    continue;
                }
                if (authored == kNullEntity || e.index < authored.index) {
                    authored = e;
                }
            }
        });
        if (authored != kNullEntity) {
            state.focused = authored;
        }
    }

    // ---- マウス: hovered / pressed / clicked ----
    const float mouseX = uilayout::SurfaceToCanvas(in.mouseSurfX, canvas);
    const float mouseY = uilayout::SurfaceToCanvas(in.mouseSurfY, canvas);
    // M75f: ヒットした要素 (画像や文字) から最寄りのウィジェットの根へ泡立てる。ウィジェットの無い
    // シーンでは HitTest の結果そのまま
    const EntityID under =
        uiwidgets::BubbleTarget(world, HitTest(world, canvasW, canvasH, mouseX, mouseY));
    state.hovered = under;
    const bool down = in.MouseDown(0);
    if (down) {
        if (state.pressed == kNullEntity) {
            // 押した瞬間に掴む (以後、離すまで移らない)。
            // M75f: 操作できない Selectable は**押下を吸うが掴まない** (下の要素にも渡らない)
            const EntityID grab =
                (uiwidgets::IsWidgetRoot(world, under) && !uiwidgets::IsInteractable(world, under))
                ? kNullEntity : under;
            state.pressed = grab;
            // M75b: ドラッグの原点。何も掴めなかった tick も書く = 空き地で押したまま要素の上へ
            // 入ってきたときは「掴んだ tick の位置」が原点になる
            state.pressSurfX = in.mouseSurfX;
            state.pressSurfY = in.mouseSurfY;
            state.dragging = 0;
            if (grab != kNullEntity) {
                ev.pressBegan = grab;
                // M75f: 押したウィジェットがフォーカスを取る (Unity の Selectable.OnPointerDown)。
                // 旧来の要素と Navigation なしのウィジェットは取らない = M75e 以前の動きのまま。
                // 空き地を押してもフォーカスは外さない (Unity は外す — 既存のシーンの挙動を変えないため)
                if (uiwidgets::IsWidgetRoot(world, grab) && uiwidgets::IsFocusCandidate(world, grab)) {
                    state.focused = grab;
                }
            }
        } else if (state.dragging == 0) {
            const float dx = in.mouseSurfX - state.pressSurfX;
            const float dy = in.mouseSurfY - state.pressSurfY;
            if (dx * dx + dy * dy > kDragThresholdSurfPx * kDragThresholdSurfPx) {
                state.dragging = 1; // 以後、離すまで保持する
            }
        }
    } else if (state.pressed != kNullEntity) {
        // 離した。**掴んだ要素の上で離したときだけ** click (Unity 意味論) —
        // 押してから外へドラッグして離す操作は取り消しになる
        if (state.pressed == under) {
            state.clicked = under;
        }
        state.pressed = kNullEntity;
        state.dragging = 0;
    }

    // ---- フォーカス: UINav* アクションで移動 ----
    // ★押した tick (pressed エッジ) だけ動かす。held で動かすと 60 回/秒で飛ぶ
    if (actions != nullptr) {
        const auto navPressed = [actions](const char* name) {
            return (actions->ActionState(HashStr(name)) & kActionPressed) != 0;
        };
        int dir = -1;
        if (navPressed(kActionNavUp)) {
            dir = uinav::kNavUp;
        } else if (navPressed(kActionNavDown)) {
            dir = uinav::kNavDown;
        } else if (navPressed(kActionNavLeft)) {
            dir = uinav::kNavLeft;
        } else if (navPressed(kActionNavRight)) {
            dir = uinav::kNavRight;
        }
        if (dir >= 0) {
            // M75f: フォーカス中の Slider は向きの軸の入力を値の変更として受ける (Unity の Slider.OnMove)。
            // 自動ナビなら常に受け、それ以外のモードはその向きに行き先が無いときだけ受ける
            const EntityID current = state.focused;
            const auto* slider = (current != kNullEntity && world.IsAlive(current))
                ? world.GetComponent<UISliderComponent>(current) : nullptr;
            if (slider != nullptr && uiwidgets::IsInteractable(world, current)
                && uiwidgets::SliderMoveOnAxis(*slider, dir)) {
                const EntityID next =
                    (uiwidgets::SelectableOf(world, current).navigationMode == uiwidgets::kNavAutomatic)
                    ? current : FindNextFocus(world, canvasW, canvasH, current, dir);
                if (next == current) {
                    ev.navStepTarget = current;
                    ev.navStepDir = dir;
                } else {
                    state.focused = next;
                }
            } else {
                state.focused = FindNextFocus(world, canvasW, canvasH, state.focused, dir);
            }
        }
        // Submit = フォーカス中の要素を「クリックした」ことにする。マウスと同じ出口へ
        // 合流させるのが要点 — ゲーム側の分岐を 1 本に保てる (パッドとマウスで別経路を
        // 書かせない)。**マウスの click より後**なので同 tick で両方来たら Submit が勝つ。
        // M75f: 操作できないウィジェットはクリックにしない
        if (state.focused != kNullEntity && navPressed(kActionSubmit)
            && !(uiwidgets::IsWidgetRoot(world, state.focused)
                 && !uiwidgets::IsInteractable(world, state.focused))) {
            state.clicked = state.focused;
        }
    }

    // ---- 生存確認 + UIElement.focused への書き戻し ----
    // 参照先が消えている / focusable でなくなっていたら手放す (世代付き EntityID なので
    // 破棄後の再利用は別 ID になる = ここは「本当に同じ要素か」の検査になる)。
    // M75f: UIElement を持たないウィジェットの根 (Toggle / Slider) も対話の相手になれる
    const auto stillUsable = [&world](EntityID e) {
        if (e == kNullEntity) {
            return false;
        }
        return world.IsAlive(e)
            && (world.GetComponent<UIElementComponent>(e) != nullptr
                || uiwidgets::IsWidgetRoot(world, e));
    };
    if (!stillUsable(state.hovered)) { state.hovered = kNullEntity; }
    if (!stillUsable(state.pressed)) { state.pressed = kNullEntity; }
    if (!stillUsable(state.clicked)) { state.clicked = kNullEntity; }
    if (!stillUsable(state.focused)) { state.focused = kNullEntity; }
    if (!stillUsable(state.changed)) { state.changed = kNullEntity; }
    // M75f: 操作できなくなったウィジェットはフォーカスを手放す (Unity の Selectable が interactable を
    // 落としたときに選択を外すのと同じ)。旧来の要素は focusable を落としても手放さない (M70c のまま)
    if (state.focused != kNullEntity && uiwidgets::IsWidgetRoot(world, state.focused)
        && !uiwidgets::IsInteractable(world, state.focused)) {
        state.focused = kNullEntity;
    }
    if (state.pressed == kNullEntity) {
        state.dragging = 0; // 掴んだ要素が消えたらドラッグも終わる
    }

    // UIElement.focused は**表示専用のミラー** (NoHash)。正本は state.focused 側で、
    // ここは毎 tick 上書きする = スクリプトが直接書いても次の tick で戻る
    // (書きたいときは ABI の UISetFocused を通す)
    const ComponentTypeId req[] = { UIElementComponent::sTypeId };
    const EntityID focused = state.focused;
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int ci = arch.FindTypeIndex(UIElementComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            auto* el = static_cast<UIElementComponent*>(arch.GetPtr(ci, row));
            el->focused = (arch.EntityAt(row) == focused) ? 1 : 0;
        }
    });

    // M75b: ドラッグ量の基準を今 tick へ進める。**必ず最後** — M75f 以降のウィジェット更新は
    // この手前で「今 - prevSurf」を読む
    // (M75f の Toggle / Slider は差分を読まないので uiwidgets::Update は TickRunner がこの後に呼ぶ。
    //  差分が要る M75g の ScrollRect は、ここへ来る前に events へ積むこと)
    state.prevSurfX = in.mouseSurfX;
    state.prevSurfY = in.mouseSurfY;
}

} // namespace uiinteract
} // namespace mye
