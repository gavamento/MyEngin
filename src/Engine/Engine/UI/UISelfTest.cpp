#include "Engine/Engine/UI/UISelfTest.h"

#include <cmath>
#include <vector>

#include "Engine/Core/Components.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/UI/UIGeometry.h"
#include "Engine/Engine/UI/UILayout.h"
#include "Engine/Engine/UI/UINav.h"
#include "Engine/Engine/UI/UITextLayout.h"

namespace mye {

bool RunUISelfTest()
{
    MYE_LOG_INFO("==== UI self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    // ResolveRect (M51e): 9-grid が正しい画面基準点にマップされるか (x/y は基準点からのオフセット)
    RegisterBuiltinComponents();
    constexpr int W = 1000;
    constexpr int H = 800;
    constexpr float ox = 10.0f;
    constexpr float oy = 5.0f;
    struct Case {
        int anchor;
        float baseX;
        float baseY;
        const char* name;
    };
    const Case cases[9] = {
        { 0, 0.0f, 0.0f, "top-left" },      { 1, 500.0f, 0.0f, "top-center" },
        { 2, 1000.0f, 0.0f, "top-right" },  { 3, 0.0f, 400.0f, "mid-left" },
        { 4, 500.0f, 400.0f, "center" },    { 5, 1000.0f, 400.0f, "mid-right" },
        { 6, 0.0f, 800.0f, "bottom-left" }, { 7, 500.0f, 800.0f, "bottom-center" },
        { 8, 1000.0f, 800.0f, "bottom-right" },
    };
    {
        World w;
        const EntityID e = w.CreateEntity("ui");
        auto* el = w.AddComponent<UIElementComponent>(e);
        el->x = ox;
        el->y = oy;
        el->w = 100.0f;
        el->h = 40.0f;
        for (const Case& c : cases) {
            w.GetComponent<UIElementComponent>(e)->anchor = c.anchor;
            const auto r = uilayout::ResolveRect(w, e, W, H);
            const bool ok = std::fabs(r.x - (c.baseX + ox)) < 1e-4f
                && std::fabs(r.y - (c.baseY + oy)) < 1e-4f && r.w == 100.0f && r.h == 40.0f;
            check(ok, c.name);
        }
    }

    // ---- ResolveRect (M51e): space=1 の入れ子 — 親矩形基準の 9 アンカー ----
    {
        World w;
        const EntityID parent = w.CreateEntity("panel");
        auto* pel = w.AddComponent<UIElementComponent>(parent);
        pel->anchor = 4; // 画面中央
        pel->x = 0.0f;
        pel->y = 0.0f;
        pel->w = 200.0f;
        pel->h = 100.0f;
        const EntityID child = w.CreateEntity("child");
        auto* cel = w.AddComponent<UIElementComponent>(child);
        cel->space = 1;
        cel->x = ox;
        cel->y = oy;
        cel->w = 50.0f;
        cel->h = 20.0f;
        w.SetParent(child, parent);
        w.ApplyStructuralChanges();
        // 親の解決済み矩形: anchor は「左上をアンカー点に置く」(センタリングしない —
        // 旧 ResolveAnchor と同じ) ので anchor=4 → 左上 (500,400)、(500,400)-(700,500)
        const float px = 500.0f, py = 400.0f, pw = 200.0f, ph = 100.0f;
        for (const Case& c : cases) {
            w.GetComponent<UIElementComponent>(child)->anchor = c.anchor;
            const auto r = uilayout::ResolveRect(w, child, W, H);
            const float bx = px + (c.baseX / W) * pw; // 9-grid 基準点を親矩形に写像
            const float by = py + (c.baseY / H) * ph;
            const bool ok = std::fabs(r.x - (bx + ox)) < 1e-4f && std::fabs(r.y - (by + oy)) < 1e-4f;
            check(ok, "nested space=1 anchor");
        }
        // 3 段入れ子: 孫 (space=1, anchor=0) は子の左上基準
        const EntityID gc = w.CreateEntity("grandchild");
        auto* gel = w.AddComponent<UIElementComponent>(gc);
        gel->space = 1;
        gel->x = 3.0f;
        gel->y = 4.0f;
        gel->w = 10.0f;
        gel->h = 10.0f;
        w.SetParent(gc, child);
        w.ApplyStructuralChanges();
        w.GetComponent<UIElementComponent>(child)->anchor = 0; // 子 = 親左上 + (ox,oy)
        const auto rg = uilayout::ResolveRect(w, gc, W, H);
        check(std::fabs(rg.x - (px + ox + 3.0f)) < 1e-4f && std::fabs(rg.y - (py + oy + 4.0f)) < 1e-4f,
              "3-level nesting resolves through chain");
        // space=1 でも UIElement 祖先が無ければ screen 基準へフォールバック
        const EntityID orphanParent = w.CreateEntity("plain"); // UIElement 無し
        const EntityID orphan = w.CreateEntity("orphan");
        auto* oel = w.AddComponent<UIElementComponent>(orphan);
        oel->space = 1;
        oel->x = 7.0f;
        oel->y = 8.0f;
        w.SetParent(orphan, orphanParent);
        w.ApplyStructuralChanges();
        const auto ro = uilayout::ResolveRect(w, orphan, W, H);
        check(std::fabs(ro.x - 7.0f) < 1e-4f && std::fabs(ro.y - 8.0f) < 1e-4f,
              "space=1 without UI ancestor falls back to screen");
        // 非 UI ノードを挟んでも最寄りの UIElement 祖先に到達する
        const EntityID mid = w.CreateEntity("group"); // UIElement 無し
        const EntityID leaf = w.CreateEntity("leaf");
        auto* lel = w.AddComponent<UIElementComponent>(leaf);
        lel->space = 1;
        lel->x = 1.0f;
        lel->y = 2.0f;
        w.SetParent(mid, parent);
        w.SetParent(leaf, mid);
        w.ApplyStructuralChanges();
        const auto rl = uilayout::ResolveRect(w, leaf, W, H);
        check(std::fabs(rl.x - (px + 1.0f)) < 1e-4f && std::fabs(rl.y - (py + 2.0f)) < 1e-4f,
              "non-UI middle node is skipped");
        // 循環親 (壊れデータ) でもハングしない — 深度上限打ち切り
        const EntityID a = w.CreateEntity("cycA");
        const EntityID b = w.CreateEntity("cycB");
        w.AddComponent<UIElementComponent>(a)->space = 1;
        w.AddComponent<UIElementComponent>(b)->space = 1;
        w.AddComponent<HierarchyComponent>(a)->parent = b;
        w.AddComponent<HierarchyComponent>(b)->parent = a;
        const auto rc = uilayout::ResolveRect(w, a, W, H);
        check(rc.w >= 0.0f, "parent cycle terminates (depth cap)");
    }

    // ---- ResolveClipRect / ResolveVisibleRect (M51e): 祖先 clipChildren の交差 ----
    {
        World w;
        const EntityID outer = w.CreateEntity("outer");
        auto* oel = w.AddComponent<UIElementComponent>(outer);
        oel->x = 100.0f;
        oel->y = 100.0f;
        oel->w = 300.0f;
        oel->h = 200.0f;
        oel->clipChildren = 1;
        const EntityID inner = w.CreateEntity("inner");
        auto* iel = w.AddComponent<UIElementComponent>(inner);
        iel->space = 1;
        iel->x = 50.0f;
        iel->y = 50.0f;
        iel->w = 200.0f;
        iel->h = 100.0f;
        iel->clipChildren = 1;
        const EntityID item = w.CreateEntity("item");
        auto* tel = w.AddComponent<UIElementComponent>(item);
        tel->space = 1;
        tel->x = 100.0f;
        tel->y = 80.0f;
        tel->w = 500.0f;
        tel->h = 40.0f;
        w.SetParent(inner, outer);
        w.SetParent(item, inner);
        w.ApplyStructuralChanges();
        // outer=(100,100,300,200) ∩ inner=(150,150,200,100) = inner 自身
        const auto clip = uilayout::ResolveClipRect(w, item, W, H);
        check(std::fabs(clip.x - 150.0f) < 1e-4f && std::fabs(clip.y - 150.0f) < 1e-4f
                  && std::fabs(clip.w - 200.0f) < 1e-4f && std::fabs(clip.h - 100.0f) < 1e-4f,
              "clip = intersection of ancestor clipChildren rects");
        // item の解決矩形 (250,230,500,40) は clip 右端 350 で切られる
        const auto vis = uilayout::ResolveVisibleRect(w, item, W, H);
        check(std::fabs(vis.x - 250.0f) < 1e-4f && std::fabs(vis.w - 100.0f) < 1e-4f,
              "visible rect = rect clipped by ancestors");
        // クリップ外へ出し切ると可視矩形は退化する
        w.GetComponent<UIElementComponent>(item)->y = 500.0f;
        const auto gone = uilayout::ResolveVisibleRect(w, item, W, H);
        check(gone.w <= 0.0f || gone.h <= 0.0f, "fully scrolled-out item has empty visible rect");
        // 自分の clipChildren は自分を切らない
        const auto self = uilayout::ResolveClipRect(w, outer, W, H);
        check(std::fabs(self.w - W) < 1e-4f, "own clipChildren does not clip self");
        // クリップ祖先が無ければ screen 全域
        const auto noclip = uilayout::ResolveClipRect(w, outer, W, H);
        check(std::fabs(noclip.x) < 1e-4f && std::fabs(noclip.h - H) < 1e-4f,
              "no clipping ancestor -> full screen");
    }

    // ---- LayoutText (M51e): 折返し行数・整列オフセット (合成グリフマップで D3D 非依存) ----
    {
        using textlayout::AlignX;
        using textlayout::AlignY;
        using textlayout::LayoutText;
        using textlayout::Line;
        auto approx = [](float a, float b) { return std::fabs(a - b) < 1e-4f; };
        FontGlyphMap glyphs;
        auto put = [&](uint32_t cp, float adv) {
            FontGlyphInfo g;
            g.advance = adv;
            g.valid = true;
            glyphs[cp] = g;
        };
        for (uint32_t c = 'A'; c <= 'Z'; ++c) {
            put(c, 10.0f);
        }
        put(' ', 5.0f);
        put('?', 7.0f);
        put(0x3042, 20.0f); // あ (全角は倍幅 — 日本語折返しの検証)
        std::vector<Line> lines;
        // 折返しなし: 1 行 + 幅 = advance 合計
        LayoutText(glyphs, "ABC", 1.0f, false, 0.0f, lines);
        check(lines.size() == 1 && approx(lines[0].width, 30.0f), "layout: single line width");
        // '\n' 分割: 空行 ("a\n\nb" の中間) も行として出る
        LayoutText(glyphs, "AB\n\nC", 1.0f, false, 0.0f, lines);
        check(lines.size() == 3 && approx(lines[1].width, 0.0f) && approx(lines[2].width, 10.0f),
              "layout: newline split keeps empty middle line");
        // 末尾 '\n' は空行を出さない (旧 PushText の描画と同じ見え方)
        LayoutText(glyphs, "AB\n", 1.0f, false, 0.0f, lines);
        check(lines.size() == 1, "layout: trailing newline emits no empty line");
        // 文字単位折返し: 幅 25 に 10px 字 → 2 字 (20) + 次で折る
        LayoutText(glyphs, "ABCDE", 1.0f, true, 25.0f, lines);
        check(lines.size() == 3 && approx(lines[0].width, 20.0f) && approx(lines[2].width, 10.0f),
              "layout: char wrap at width");
        // 全角 20px は幅 25 で 1 字/行
        LayoutText(glyphs, "\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82", 1.0f, true, 25.0f, lines);
        check(lines.size() == 3 && approx(lines[0].width, 20.0f), "layout: wide glyph wraps per char");
        // 行頭 1 文字は必ず載る (幅より広い字でも無限ループしない)
        LayoutText(glyphs, "\xE3\x81\x82\xE3\x81\x82", 1.0f, true, 10.0f, lines);
        check(lines.size() == 2 && approx(lines[0].width, 20.0f),
              "layout: first char always placed (progress guarantee)");
        // 未焼成グリフは '?' の幅で代用
        LayoutText(glyphs, "z", 1.0f, false, 0.0f, lines);
        check(lines.size() == 1 && approx(lines[0].width, 7.0f), "layout: missing glyph uses '?'");
        // k 係数は幅に掛かる
        LayoutText(glyphs, "AB", 2.0f, false, 0.0f, lines);
        check(lines.size() == 1 && approx(lines[0].width, 40.0f), "layout: scale multiplies width");
        // 整列オフセット (9-grid): 列 = x、行 = y
        check(approx(AlignX(0, 30, 100), 0.0f) && approx(AlignX(4, 30, 100), 35.0f)
                  && approx(AlignX(8, 30, 100), 70.0f),
              "align: x offsets (left/center/right)");
        check(approx(AlignY(1, 20, 100), 0.0f) && approx(AlignY(4, 20, 100), 40.0f)
                  && approx(AlignY(7, 20, 100), 80.0f),
              "align: y offsets (top/middle/bottom)");
    }

    // ---- BuildFillQuad (M35): 境界値 0 / 0.5 / 1、水平と垂直 ----
    {
        using uigeom::BuildFillQuad;
        auto approx = [](float a, float b) { return std::fabs(a - b) < 1e-4f; };
        const auto full = BuildFillQuad(10, 20, 100, 40, 1, 1.0f);
        check(approx(full.w, 100) && approx(full.u1, 1.0f), "fill: horizontal 1.0 = full");
        const auto half = BuildFillQuad(10, 20, 100, 40, 1, 0.5f);
        check(approx(half.x, 10) && approx(half.w, 50) && approx(half.u1, 0.5f),
              "fill: horizontal 0.5 clips right half");
        const auto zero = BuildFillQuad(10, 20, 100, 40, 1, 0.0f);
        check(approx(zero.w, 0), "fill: horizontal 0 = empty");
        const auto vhalf = BuildFillQuad(10, 20, 100, 40, 2, 0.5f);
        check(approx(vhalf.y, 40) && approx(vhalf.h, 20) && approx(vhalf.v0, 0.5f) && approx(vhalf.v1, 1.0f),
              "fill: vertical 0.5 fills bottom-up");
        const auto over = BuildFillQuad(0, 0, 100, 40, 1, 1.5f);
        check(approx(over.w, 100), "fill: amount clamped to 1");
    }

    // ---- Build9Slice (M35): 9 矩形 / UV / 退化 / 過大 border ----
    {
        using uigeom::Build9Slice;
        auto approx = [](float a, float b) { return std::fabs(a - b) < 1e-4f; };
        uigeom::UIQuad q[9];
        // 64x64 テクスチャ・border 8px・矩形 200x100 → フル 9 枚
        const int n = Build9Slice(0, 0, 200, 100, 8, 8, 8, 8, 64, 64, q);
        check(n == 9, "9slice: full border -> 9 quads");
        // 左上隅は原寸 8x8、UV は 0..8/64
        check(approx(q[0].w, 8) && approx(q[0].h, 8) && approx(q[0].u1, 8.0f / 64.0f),
              "9slice: corner keeps native size + uv");
        // 中央は伸縮 (200-16 x 100-16)
        check(approx(q[4].w, 184) && approx(q[4].h, 84), "9slice: center stretches");
        // 幅の合計 = 全体幅
        check(approx(q[0].w + q[1].w + q[2].w, 200), "9slice: column widths sum to rect");
        // border 0 → 中央 1 枚だけ
        const int n1 = Build9Slice(0, 0, 200, 100, 0, 0, 0, 0, 64, 64, q);
        check(n1 == 1 && approx(q[0].w, 200), "9slice: zero border -> single quad");
        // 過大 border (l+r > w) は比率縮小で総和が矩形に収まる
        const int n2 = Build9Slice(0, 0, 10, 100, 8, 0, 8, 0, 64, 64, q);
        float wsum = 0;
        for (int i = 0; i < n2; ++i) {
            wsum += q[i].w;
        }
        check(n2 >= 2 && wsum <= 10.0f + 1e-3f, "9slice: oversized border shrinks to fit");
    }

    // ---- UINav::FindNext (M35): 方向選択 / 半平面除外 / タイブレーク ----
    {
        using namespace uinav;
        // 十字配置: 中央(0) 上(1) 下(2) 左(3) 右(4)
        const NavRect r[5] = {
            { 100, 100, 20, 20, 0 },
            { 100, 40, 20, 20, 1 },
            { 100, 160, 20, 20, 2 },
            { 40, 100, 20, 20, 3 },
            { 160, 100, 20, 20, 4 },
        };
        check(FindNext(r, 5, r[0], kNavUp) == 1, "nav: up picks upper");
        check(FindNext(r, 5, r[0], kNavDown) == 2, "nav: down picks lower");
        check(FindNext(r, 5, r[0], kNavLeft) == 3, "nav: left picks left");
        check(FindNext(r, 5, r[0], kNavRight) == 4, "nav: right picks right");
        // 上端からさらに上 → 候補なし = 現在維持
        check(FindNext(r, 5, r[1], kNavUp) == 1, "nav: no candidate keeps current");
        // 同点タイブレーク: 等距離の 2 候補は index 昇順
        const NavRect tie[3] = {
            { 100, 100, 20, 20, 5 },
            { 60, 40, 20, 20, 2 },  // 左上 (等距離)
            { 140, 40, 20, 20, 1 }, // 右上 (等距離)
        };
        check(FindNext(tie, 3, tie[0], kNavUp) == 1, "nav: tie-break by index");
        // 直交ずれの重み: 真上の遠い候補 vs 斜めの近い候補
        const NavRect wt[3] = {
            { 100, 100, 20, 20, 0 },
            { 100, 20, 20, 20, 1 },  // 真上 80px (score 80)
            { 130, 70, 20, 20, 2 },  // 斜め (axial 30 + ortho 30*2 = 90)
        };
        check(FindNext(wt, 3, wt[0], kNavUp) == 1, "nav: orthogonal drift is penalized");
    }

    if (failCount == 0) {
        MYE_LOG_INFO("==== UI self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== UI self test: %d FAILURE(S) ====", failCount);
    return false;
}

} // namespace mye
