#include "Engine/Engine/UI/UIInteraction.h"

#include <vector>

#include "Engine/Core/Components.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/UI/UILayout.h"
#include "Engine/Engine/UI/UILayoutGroup.h" // M75e: LayoutScratch
#include "Engine/Engine/UI/UINav.h"
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
                             // (= 従来の ResolveVisibleRect と同じ式。恒等要素の判定は M75a 前と
                             // 1 ビットも変わらない)
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
    ForEachUiElement(world, canvasW, canvasH,
                     [&](EntityID e, const UIElementComponent& el,
                         const uilayout::UIWorldContext* wc, uilayout::LayoutScratch& scratch) {
                         if (el.focusable == 0) {
                             return;
                         }
                         // 祖先クリップで完全に隠れた要素は候補から外す
                         // (スクロール範囲外の項目へ飛ばない)
                         const auto vis = uilayout::ResolveVisibleRect(world, e, canvasW, canvasH,
                                                                       wc, &scratch);
                         if (vis.w <= 0.0f || vis.h <= 0.0f) {
                             return;
                         }
                         const auto rect =
                             uilayout::ResolveRect(world, e, canvasW, canvasH, wc, &scratch);
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
                     });
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
              const InputActions* actions, UIInteractionState& state)
{
    (void)prevIn; // M75h (InputField のキーエッジ) が読む。配線だけ先に通してある
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
                if (el->focusable == 0 || el->focused == 0) {
                    continue;
                }
                const EntityID e = arch.EntityAt(row);
                if (!IsEntityActive(world, e)) {
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
    const EntityID under = HitTest(world, canvasW, canvasH, mouseX, mouseY);
    state.hovered = under;
    const bool down = in.MouseDown(0);
    if (down) {
        if (state.pressed == kNullEntity) {
            state.pressed = under; // 押した瞬間に掴む (以後、離すまで移らない)
            // M75b: ドラッグの原点。何も掴めなかった tick も書く = 空き地で押したまま要素の上へ
            // 入ってきたときは「掴んだ tick の位置」が原点になる
            state.pressSurfX = in.mouseSurfX;
            state.pressSurfY = in.mouseSurfY;
            state.dragging = 0;
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
            state.focused = FindNextFocus(world, canvasW, canvasH, state.focused, dir);
        }
        // Submit = フォーカス中の要素を「クリックした」ことにする。マウスと同じ出口へ
        // 合流させるのが要点 — ゲーム側の分岐を 1 本に保てる (パッドとマウスで別経路を
        // 書かせない)。**マウスの click より後**なので同 tick で両方来たら Submit が勝つ
        if (state.focused != kNullEntity && navPressed(kActionSubmit)) {
            state.clicked = state.focused;
        }
    }

    // ---- 生存確認 + UIElement.focused への書き戻し ----
    // 参照先が消えている / focusable でなくなっていたら手放す (世代付き EntityID なので
    // 破棄後の再利用は別 ID になる = ここは「本当に同じ要素か」の検査になる)
    const auto stillUsable = [&world](EntityID e) {
        if (e == kNullEntity) {
            return false;
        }
        return world.IsAlive(e) && world.GetComponent<UIElementComponent>(e) != nullptr;
    };
    if (!stillUsable(state.hovered)) { state.hovered = kNullEntity; }
    if (!stillUsable(state.pressed)) { state.pressed = kNullEntity; }
    if (!stillUsable(state.clicked)) { state.clicked = kNullEntity; }
    if (!stillUsable(state.focused)) { state.focused = kNullEntity; }
    if (!stillUsable(state.changed)) { state.changed = kNullEntity; }
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
    state.prevSurfX = in.mouseSurfX;
    state.prevSurfY = in.mouseSurfY;
}

} // namespace uiinteract
} // namespace mye
