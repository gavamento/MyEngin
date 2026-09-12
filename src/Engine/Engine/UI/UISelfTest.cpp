#include "Engine/Engine/UI/UISelfTest.h"

#include <cmath>
#include <cstring>
#include <vector>

#include "Engine/Core/ComponentRegistry.h" // ワールド追従 UI の検証 (スクリプト状態の脇役扱い)
#include "Engine/Core/Components.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/TransformSystem.h" // ワールド追従 UI の検証 (WorldMatrix 生成)
#include "Engine/Engine/UI/UIGeometry.h"
#include "Engine/Engine/UI/UIInteraction.h"
#include "Engine/Engine/UI/UILayout.h"
#include "Engine/Engine/UI/UINav.h"
#include "Engine/Platform/Input.h"
#include "Engine/Platform/InputActions.h"
#include "Engine/Engine/UI/UITextLayout.h"
#include "Engine/Engine/Scene.h"           // M75a: 旧形式 (v3) シーンのロード時変換
#include "Engine/Engine/SceneSerializer.h"
#include <nlohmann/json.hpp>

namespace mye {

namespace {

// M75a: 旧 UIElement の配置引数 (anchor / x / y / w / h / space) で RectTransform + UIElement を
// 足す検査用ヘルパ。**RectTransform を先に**足す (後から足すと UIElement のポインタが
// アーキタイプ移動で無効になる)。戻り値は UIElement
UIElementComponent* AddLegacyUi(World& w, EntityID e, int anchor, float x, float y, float rw,
                                float rh, int space = 0, bool hasUiAncestor = false)
{
    *w.AddComponent<RectTransformComponent>(e) =
        uilayout::FromLegacyRect(anchor, x, y, rw, rh, space, hasUiAncestor);
    return w.AddComponent<UIElementComponent>(e);
}

} // namespace

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
        AddLegacyUi(w, e, 0, ox, oy, 100.0f, 40.0f);
        for (const Case& c : cases) {
            *w.GetComponent<RectTransformComponent>(e) =
                uilayout::FromLegacyRect(c.anchor, ox, oy, 100.0f, 40.0f, 0, false);
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
        AddLegacyUi(w, parent, 4 /*画面中央*/, 0.0f, 0.0f, 200.0f, 100.0f);
        const EntityID child = w.CreateEntity("child");
        AddLegacyUi(w, child, 0, ox, oy, 50.0f, 20.0f, /*space*/ 1, true);
        w.SetParent(child, parent);
        w.ApplyStructuralChanges();
        // 親の解決済み矩形: anchor は「左上をアンカー点に置く」(センタリングしない —
        // 旧 ResolveAnchor と同じ) ので anchor=4 → 左上 (500,400)、(500,400)-(700,500)
        const float px = 500.0f, py = 400.0f, pw = 200.0f, ph = 100.0f;
        for (const Case& c : cases) {
            *w.GetComponent<RectTransformComponent>(child) =
                uilayout::FromLegacyRect(c.anchor, ox, oy, 50.0f, 20.0f, 1, true);
            const auto r = uilayout::ResolveRect(w, child, W, H);
            const float bx = px + (c.baseX / W) * pw; // 9-grid 基準点を親矩形に写像
            const float by = py + (c.baseY / H) * ph;
            const bool ok = std::fabs(r.x - (bx + ox)) < 1e-4f && std::fabs(r.y - (by + oy)) < 1e-4f;
            check(ok, "nested space=1 anchor");
        }
        // 3 段入れ子: 孫 (space=1, anchor=0) は子の左上基準
        const EntityID gc = w.CreateEntity("grandchild");
        AddLegacyUi(w, gc, 0, 3.0f, 4.0f, 10.0f, 10.0f, 1, true);
        w.SetParent(gc, child);
        w.ApplyStructuralChanges();
        *w.GetComponent<RectTransformComponent>(child) =
            uilayout::FromLegacyRect(0, ox, oy, 50.0f, 20.0f, 1, true); // 子 = 親左上 + (ox,oy)
        const auto rg = uilayout::ResolveRect(w, gc, W, H);
        check(std::fabs(rg.x - (px + ox + 3.0f)) < 1e-4f && std::fabs(rg.y - (py + oy + 4.0f)) < 1e-4f,
              "3-level nesting resolves through chain");
        // space=1 でも UIElement 祖先が無ければ screen 基準へフォールバック
        const EntityID orphanParent = w.CreateEntity("plain"); // UIElement 無し
        const EntityID orphan = w.CreateEntity("orphan");
        AddLegacyUi(w, orphan, 0, 7.0f, 8.0f, 160.0f, 40.0f, 1, false);
        w.SetParent(orphan, orphanParent);
        w.ApplyStructuralChanges();
        const auto ro = uilayout::ResolveRect(w, orphan, W, H);
        check(std::fabs(ro.x - 7.0f) < 1e-4f && std::fabs(ro.y - 8.0f) < 1e-4f,
              "space=1 without UI ancestor falls back to screen");
        // 非 UI ノードを挟んでも最寄りの UIElement 祖先に到達する
        const EntityID mid = w.CreateEntity("group"); // UIElement 無し
        const EntityID leaf = w.CreateEntity("leaf");
        AddLegacyUi(w, leaf, 0, 1.0f, 2.0f, 160.0f, 40.0f, 1, true);
        w.SetParent(mid, parent);
        w.SetParent(leaf, mid);
        w.ApplyStructuralChanges();
        const auto rl = uilayout::ResolveRect(w, leaf, W, H);
        check(std::fabs(rl.x - (px + 1.0f)) < 1e-4f && std::fabs(rl.y - (py + 2.0f)) < 1e-4f,
              "non-UI middle node is skipped");
        // 循環親 (壊れデータ) でもハングしない — 深度上限打ち切り
        const EntityID a = w.CreateEntity("cycA");
        const EntityID b = w.CreateEntity("cycB");
        AddLegacyUi(w, a, 0, 0.0f, 0.0f, 10.0f, 10.0f, 1, true);
        AddLegacyUi(w, b, 0, 0.0f, 0.0f, 10.0f, 10.0f, 1, true);
        w.AddComponent<HierarchyComponent>(a)->parent = b;
        w.AddComponent<HierarchyComponent>(b)->parent = a;
        const auto rc = uilayout::ResolveRect(w, a, W, H);
        check(rc.w >= 0.0f, "parent cycle terminates (depth cap)");
    }

    // ---- ResolveClipRect / ResolveVisibleRect (M51e): 祖先 clipChildren の交差 ----
    {
        World w;
        const EntityID outer = w.CreateEntity("outer");
        AddLegacyUi(w, outer, 0, 100.0f, 100.0f, 300.0f, 200.0f)->clipChildren = 1;
        const EntityID inner = w.CreateEntity("inner");
        AddLegacyUi(w, inner, 0, 50.0f, 50.0f, 200.0f, 100.0f, 1, true)->clipChildren = 1;
        const EntityID item = w.CreateEntity("item");
        AddLegacyUi(w, item, 0, 100.0f, 80.0f, 500.0f, 40.0f, 1, true);
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
        w.GetComponent<RectTransformComponent>(item)->anchoredPosition.y = 500.0f;
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

    // ---- ワールド追従 UI (エンティティ構成による完全自動判定) ----
    // 規則: UI 専用オブジェクト (基本 4 種 + UIElement + 帳簿 + スクリプト状態のみ) は画面 UI、
    // それ以外のコンポーネントを持つオブジェクト上の UIElement はそのオブジェクトに追従。
    // カメラ = 原点・無回転 (+Z を向く LH)。光軸上の点は厳密に画面中心へ射影される。
    // ★構造変更 (AddComponent/SetParent) のたびに ApplyStructuralChanges + ポインタ再取得
    //   (アーキタイプ移動で古いポインタは無効になる)
    {
        World w;
        TransformSystem xform;
        const EntityID cam = w.CreateEntity("cam");
        w.AddComponent<CameraComponent>(cam); // isPrimary=1 / fovY 60 / near 0.1 / far 1000
        // 3D オブジェクト (MeshRenderer 持ち = UI 専用でない) に UIElement を直付け
        const EntityID obj = w.CreateEntity("enemy");
        w.AddComponent<MeshRendererComponent>(obj);
        AddLegacyUi(w, obj, 0, 0.0f, 0.0f, 100.0f, 40.0f);
        w.ApplyStructuralChanges();
        w.GetComponent<LocalTransform>(obj)->position = { 0, 0, 10.0f };
        xform.Update(w);

        uilayout::UIWorldContext wc;
        check(uilayout::BuildSimWorldContext(w, 1920, 1080, wc),
              "world UI: sim camera context builds (scalar)");
        auto res = uilayout::Resolve(w, obj, 1920, 1080, &wc);
        check(res.visible && std::fabs(res.rect.x - 960.0f) < 1e-2f
                  && std::fabs(res.rect.y - 540.0f) < 1e-2f,
              "world UI: UI on a mesh entity projects to the object (screen center)");
        check(res.rect.w == 100.0f && res.rect.h == 40.0f,
              "world UI: size is unscaled by default");
        check(!uilayout::Resolve(w, obj, 1920, 1080, nullptr).visible,
              "world UI: hidden without a camera context");

        // 符号: +X は画面右、+Y は画面上 (= y 減少)
        w.GetComponent<LocalTransform>(obj)->position = { 2.0f, 0, 10.0f };
        xform.Update(w);
        res = uilayout::Resolve(w, obj, 1920, 1080, &wc);
        check(res.visible && res.rect.x > 960.0f, "world UI: +X moves right on screen");
        w.GetComponent<LocalTransform>(obj)->position = { 0, 2.0f, 10.0f };
        xform.Update(w);
        res = uilayout::Resolve(w, obj, 1920, 1080, &wc);
        check(res.visible && res.rect.y < 540.0f, "world UI: +Y moves up on screen");

        // 距離スケール: 等倍距離 10m → z=10 で 1.0 / z=20 で 0.5 (子 space=1 にも伝播)
        w.GetComponent<LocalTransform>(obj)->position = { 0, 0, 10.0f };
        {
            auto* el = w.GetComponent<UIElementComponent>(obj);
            el->distanceScale = true;
            el->distanceRef = 10.0f;
        }
        xform.Update(w);
        res = uilayout::Resolve(w, obj, 1920, 1080, &wc);
        check(res.visible && std::fabs(res.rect.w - 100.0f) < 1e-3f,
              "world UI: scale = 1 at the reference distance");
        w.GetComponent<LocalTransform>(obj)->position = { 0, 0, 20.0f };
        xform.Update(w);
        res = uilayout::Resolve(w, obj, 1920, 1080, &wc);
        check(res.visible && std::fabs(res.rect.w - 50.0f) < 1e-3f
                  && std::fabs(res.rect.h - 20.0f) < 1e-3f,
              "world UI: size halves at twice the reference distance");
        // 複合ウィジェット: UI 専用の子を space=1 でぶら下げると親矩形基準で一緒に追従し、
        // 距離スケールも伝播する
        const EntityID fill = w.CreateEntity("fill");
        AddLegacyUi(w, fill, 0, 0.0f, 0.0f, 60.0f, 10.0f, 1, true);
        w.SetParent(fill, obj);
        w.ApplyStructuralChanges();
        res = uilayout::Resolve(w, fill, 1920, 1080, &wc);
        check(res.visible && std::fabs(res.rect.w - 30.0f) < 1e-3f,
              "world UI: distance scale propagates to space=1 children");

        // 背面: クランプ OFF は非表示 (子ごと)、ON は画面内へ
        w.GetComponent<UIElementComponent>(obj)->distanceScale = false;
        w.GetComponent<LocalTransform>(obj)->position = { 0, 0, -10.0f };
        xform.Update(w);
        check(!uilayout::Resolve(w, obj, 1920, 1080, &wc).visible,
              "world UI: behind the camera is hidden (clamp off)");
        check(!uilayout::Resolve(w, fill, 1920, 1080, &wc).visible,
              "world UI: children vanish with their hidden parent");
        w.GetComponent<UIElementComponent>(obj)->clampToScreen = true;
        res = uilayout::Resolve(w, obj, 1920, 1080, &wc);
        check(res.visible && res.rect.x >= 0.0f && res.rect.y >= 0.0f
                  && res.rect.x + res.rect.w <= 1920.0f && res.rect.y + res.rect.h <= 1080.0f,
              "world UI: behind + clamp stays on screen");

        // クランプ (前方だが画面外): 右端に貼り付く
        w.GetComponent<LocalTransform>(obj)->position = { 1000.0f, 0, 10.0f };
        xform.Update(w);
        res = uilayout::Resolve(w, obj, 1920, 1080, &wc);
        check(res.visible && std::fabs((res.rect.x + res.rect.w) - 1920.0f) < 1e-3f,
              "world UI: clamp pins the rect to the screen edge");

        // 既存不変 1: UI 専用のルートエンティティはカメラコンテキストが有っても従来とビット同一
        const EntityID rootUi = w.CreateEntity("screen");
        AddLegacyUi(w, rootUi, 0, 10.0f, 20.0f, 100.0f, 40.0f);
        w.ApplyStructuralChanges();
        const auto ra = uilayout::ResolveRect(w, rootUi, 1920, 1080, &wc);
        const auto rb = uilayout::ResolveRect(w, rootUi, 1920, 1080);
        check(std::memcmp(&ra, &rb, sizeof(ra)) == 0 && ra.x == 10.0f && ra.y == 20.0f,
              "world UI: screen elements are bit-identical with/without a camera context");

        // 既存不変 2: UI 専用の子を 3D オブジェクトの下に整理してもそれ自体は画面 UI のまま
        // (space=0 なので screen 基準。追従はコンポーネント構成でのみ決まる)
        const EntityID grouped = w.CreateEntity("grouped");
        AddLegacyUi(w, grouped, 0, 30.0f, 40.0f, 50.0f, 20.0f, /*space*/ 0, /*hasUiAncestor*/ true);
        w.SetParent(grouped, obj);
        w.ApplyStructuralChanges();
        // 親 obj は UIElement 持ちだが grouped は旧 space=0 = basis 1 (キャンバス基準。従来意味論)
        const auto rg = uilayout::ResolveRect(w, grouped, 1920, 1080, &wc);
        check(rg.x == 30.0f && rg.y == 40.0f,
              "world UI: a ui-only child under a 3D object stays screen-anchored (basis=1)");

        // 既存不変 3: スクリプト状態コンポーネントは「UI 専用」を壊さない
        // (ボタンにロジックを付けても画面 UI のまま)
        ComponentDesc sd;
        sd.name = "UiSelfTestScriptState";
        sd.nameHash = HashStr("UiSelfTestScriptState");
        sd.size = 4;
        sd.align = 4;
        sd.flags = kComponentScriptState;
        sd.construct = [](void* dst) { *static_cast<int32_t*>(dst) = 0; };
        const ComponentTypeId scriptType = ComponentRegistry::Get().Register(sd);
        const EntityID button = w.CreateEntity("button");
        AddLegacyUi(w, button, 0, 5.0f, 6.0f, 80.0f, 30.0f);
        w.AddComponentRaw(button, scriptType);
        w.ApplyStructuralChanges();
        const auto rs = uilayout::ResolveRect(w, button, 1920, 1080, &wc);
        check(rs.x == 5.0f && rs.y == 6.0f,
              "world UI: script-state components keep an entity ui-only (screen)");
    }

    // ---- UI の対話 (M70c) ----
    // エンジンが hovered / pressed / clicked / focused を持つようになった経路の検査。
    // ここが守るのは 4 つ:
    //   (1) click = 「掴んだ要素の上で離した」(押しっぱなしで外へ出て離すのは取り消し)
    //   (2) 押下中は掴んだ要素から移らない (別の要素の上を通っても pressed は動かない)
    //   (3) フォーカスは UINav* の pressed エッジでだけ動き、UIElement.focused へ書き戻る
    //   (4) 参照先が消えたら手放す (破棄済み EntityID を握り続けない)
    {
        World w;
        // 縦に 2 個。キャンバス 1920x1080 の左上基準 (anchor=0) なので座標がそのまま矩形
        const EntityID a = w.CreateEntity("btnA");
        const EntityID b = w.CreateEntity("btnB");
        for (const EntityID e : { a, b }) {
            auto* el = AddLegacyUi(w, e, 0, 100.0f, (e == a) ? 100.0f /*上*/ : 300.0f /*下*/,
                                   200.0f, 80.0f);
            el->kind = 2;
            el->focusable = 1;
        }
        w.ApplyStructuralChanges();

        InputActions actions;
        UIInteractionState st;
        // キャンバス寸法 + キャンバス座標のマウスを持つ入力を組む (レーン 0 の規約)
        InputSnapshot in = {};
        in.canvasW = 1920.0f;
        in.canvasH = 1080.0f;
        const auto mouse = [&in](float x, float y, bool down) {
            in.mouseCanvasX = x;
            in.mouseCanvasY = y;
            in.mouseButtons = down ? 1u : 0u;
        };

        // (a) ヒットテスト: 矩形の内と外
        check(uiinteract::HitTest(w, 1920, 1080, 150.0f, 150.0f) == a,
              "interaction: hit test finds the element under the point");
        check(uiinteract::HitTest(w, 1920, 1080, 50.0f, 150.0f) == kNullEntity,
              "interaction: hit test misses outside the rect");

        // (b) hover → press → release で click が 1 tick だけ立つ
        mouse(150.0f, 150.0f, false);
        uiinteract::Evaluate(w, in, &actions, st);
        check(st.hovered == a && st.pressed == kNullEntity && st.clicked == kNullEntity,
              "interaction: hovering alone does not press or click");
        mouse(150.0f, 150.0f, true);
        uiinteract::Evaluate(w, in, &actions, st);
        check(st.pressed == a && st.clicked == kNullEntity,
              "interaction: the press is captured but does not click yet");
        mouse(150.0f, 150.0f, false);
        uiinteract::Evaluate(w, in, &actions, st);
        check(st.clicked == a && st.pressed == kNullEntity,
              "interaction: releasing over the pressed element clicks it");
        uiinteract::Evaluate(w, in, &actions, st);
        check(st.clicked == kNullEntity, "interaction: clicked lasts exactly one tick");

        // (c) 押したまま別の要素へ移っても掴んだ相手は変わらない / そこで離しても click しない
        mouse(150.0f, 150.0f, true);
        uiinteract::Evaluate(w, in, &actions, st);
        mouse(150.0f, 350.0f, true); // btnB の上へドラッグ
        uiinteract::Evaluate(w, in, &actions, st);
        check(st.pressed == a && st.hovered == b,
              "interaction: the captured element does not change while the button is held");
        mouse(150.0f, 350.0f, false);
        uiinteract::Evaluate(w, in, &actions, st);
        check(st.clicked == kNullEntity,
              "interaction: releasing over a different element cancels the click");

        // (d) フォーカス: 候補が無い状態から下へ動かすと index 最小の候補へ吸着し、
        //     UIElement.focused へ書き戻る
        check(uiinteract::FindNextFocus(w, 1920, 1080, kNullEntity, uinav::kNavDown) == a,
              "interaction: focus enters at the lowest entity index");
        check(uiinteract::FindNextFocus(w, 1920, 1080, a, uinav::kNavDown) == b,
              "interaction: focus moves down to the next candidate");
        check(uiinteract::FindNextFocus(w, 1920, 1080, b, uinav::kNavDown) == b,
              "interaction: focus stays put when there is nothing further down");
        st.focused = b;
        uiinteract::Evaluate(w, in, &actions, st);
        check(w.GetComponent<UIElementComponent>(b)->focused == 1
                  && w.GetComponent<UIElementComponent>(a)->focused == 0,
              "interaction: UIElement.focused mirrors the engine focus");

        // (e) 参照先が消えたら手放す (破棄済みの EntityID を握り続けない)
        w.DestroyEntity(b);
        w.ApplyStructuralChanges();
        uiinteract::Evaluate(w, in, &actions, st);
        check(st.focused == kNullEntity, "interaction: a destroyed element drops the focus");

        // (f) BitsFor はビットの意味を固定する (ScriptAPI.h の MyeUIButton* と同値)
        st.Clear();
        st.hovered = a;
        st.clicked = a;
        check(uiinteract::BitsFor(st, a) == (uiinteract::kHovered | uiinteract::kClicked),
              "interaction: BitsFor reports exactly the states that hold");
        check(uiinteract::BitsFor(st, kNullEntity) == 0u,
              "interaction: BitsFor of a null entity is 0");
    }

    // ---- キャンバス (M70b) ----
    // 主張は 3 つ: (1) 16:9 はどの画素数でも厳密に 1920x1080 = 既存 golden が動かない、
    // (2) 非 16:9 はアスペクトぶんだけキャンバスが伸びる (黒帯は作らない)、
    // (3) キャンバス寸法は**アスペクト比だけの関数**で画素数に依らない
    {
        struct CanvasCase {
            int w, h;      // 実 px
            int cw, ch;    // 期待キャンバス
            float scale;   // 期待スケール
            const char* what;
        };
        const CanvasCase canvasCases[] = {
            { 960, 540, 1920, 1080, 0.5f, "canvas: 960x540 (16:9) is exactly 1920x1080 at 0.5" },
            { 1600, 900, 1920, 1080, 1600.0f / 1920.0f, "canvas: 1600x900 (16:9) is 1920x1080" },
            { 1920, 1080, 1920, 1080, 1.0f, "canvas: 1920x1080 is identity" },
            { 3840, 2160, 1920, 1080, 2.0f, "canvas: 4K (16:9) is 1920x1080 at 2.0" },
            { 960, 600, 1920, 1200, 0.5f, "canvas: 960x600 (16:10) grows to 1920x1200" },
            { 2560, 1080, 2560, 1080, 1.0f, "canvas: 2560x1080 (21:9) grows sideways" },
        };
        bool ok = true;
        for (const CanvasCase& c : canvasCases) {
            const uilayout::CanvasInfo ci = uilayout::CanvasSize(c.w, c.h);
            const bool hit = ci.w == c.cw && ci.h == c.ch && std::fabs(ci.scale - c.scale) < 1e-6f;
            if (!hit) {
                MYE_LOG_ERROR("    %dx%d -> canvas %dx%d scale %.6f (expected %dx%d %.6f)", c.w,
                              c.h, ci.w, ci.h, static_cast<double>(ci.scale), c.cw, c.ch,
                              static_cast<double>(c.scale));
            }
            check(hit, c.what);
            ok = ok && hit;
        }
        // 黒帯を作らない = キャンバスを実 px へ戻すと画面ぴったりになる (丸め 1 px 以内)
        for (const CanvasCase& c : canvasCases) {
            const uilayout::CanvasInfo ci = uilayout::CanvasSize(c.w, c.h);
            ok = ok && std::fabs(static_cast<float>(ci.w) * ci.scale - static_cast<float>(c.w)) < 1.0f
                && std::fabs(static_cast<float>(ci.h) * ci.scale - static_cast<float>(c.h)) < 1.0f;
        }
        check(ok, "canvas: the canvas always covers the whole screen (no letterbox)");
        // 退化した画面 (最小化 / 0 px) は基準解像度へ倒す — 0 除算で NaN を作らない
        const uilayout::CanvasInfo degenerate = uilayout::CanvasSize(0, 0);
        check(degenerate.w == uilayout::kCanvasRefW && degenerate.h == uilayout::kCanvasRefH
                  && degenerate.scale == 1.0f,
              "canvas: a degenerate screen falls back to the reference resolution");
    }

    // ---- RectTransform (M75a) ----
    // 主張: (1) 旧式 (9-grid + オフセット) と新式 (anchorMin/Max + pivot) は一致アンカー・
    // pivot 0 で**ビット同一** = golden 4 枚が動かない根拠、(2) ストレッチ / pivot / basis が
    // Unity の式どおり、(3) 回転はヒットテストが逆変換して判定し、恒等要素は xform を作らない、
    // (4) UI コンポーネントは UiAux フラグ付きで登録されている (IsUiOnlyEntity の許容リスト)
    {
        // (1) 旧式との memcmp。旧式 = AnchorOrigin (base.x + {0, w*0.5f, w}) + オフセット*scale
        bool same = true;
        int tested = 0;
        const float bases[][4] = { { 0, 0, 1920, 1080 }, { 500, 400, 200, 100 },
                                   { 123.5f, 77.25f, 333.3f, 19.7f }, { 960.0f, 540.0f, 0, 0 } };
        const float offs[][4] = { { 10, 5, 100, 40 }, { -920, -500, 1840, 1000 },
                                  { -600.0f, 60.0f, 1200.0f, 96.0f }, { 0.1f, -0.7f, 33.3f, 7.77f } };
        const float scales[] = { 1.0f, 0.5f, 1.7f, 0.3333f };
        for (int anchor = 0; anchor < 9 && same; ++anchor) {
            for (const auto& bv : bases) {
                for (const auto& ov : offs) {
                    for (const float sc : scales) {
                        const uilayout::UIRect base = { bv[0], bv[1], bv[2], bv[3] };
                        const int col = anchor % 3;
                        const int row = anchor / 3;
                        uilayout::UIRect oldR;
                        oldR.x = base.x + ((col == 0) ? 0.0f : (col == 1) ? base.w * 0.5f : base.w);
                        oldR.y = base.y + ((row == 0) ? 0.0f : (row == 1) ? base.h * 0.5f : base.h);
                        oldR.x += ov[0] * sc;
                        oldR.y += ov[1] * sc;
                        oldR.w = ov[2] * sc;
                        oldR.h = ov[3] * sc;
                        const RectTransformComponent rt =
                            uilayout::FromLegacyRect(anchor, ov[0], ov[1], ov[2], ov[3], 0, false);
                        const uilayout::UIRect newR = uilayout::RectFromTransform(rt, base, sc);
                        if (std::memcmp(&oldR, &newR, sizeof(oldR)) != 0) {
                            same = false;
                            MYE_LOG_ERROR("    anchor %d base(%g,%g,%g,%g) off(%g,%g,%g,%g) s=%g:"
                                          " old(%g,%g,%g,%g) new(%g,%g,%g,%g)",
                                          anchor, bv[0], bv[1], bv[2], bv[3], ov[0], ov[1], ov[2],
                                          ov[3], sc, oldR.x, oldR.y, oldR.w, oldR.h, newR.x,
                                          newR.y, newR.w, newR.h);
                        }
                        ++tested;
                    }
                }
            }
        }
        check(same && tested == 9 * 4 * 4 * 4,
              "rect: legacy 9-grid formula and RectTransform formula are bit-identical");

        // (2) ストレッチ / pivot / basis
        World w;
        const EntityID panel = w.CreateEntity("panel");
        AddLegacyUi(w, panel, 4, 0.0f, 0.0f, 200.0f, 100.0f); // (500,400)-(700,500) on 1000x800
        const EntityID st = w.CreateEntity("stretch");
        {
            auto* rt = w.AddComponent<RectTransformComponent>(st);
            rt->anchorMin = { 0.0f, 0.0f };
            rt->anchorMax = { 1.0f, 1.0f };
            rt->pivot = { 0.5f, 0.5f };
            rt->anchoredPosition = { 0.0f, 0.0f };
            rt->sizeDelta = { -20.0f, -10.0f }; // 親から左右 10 / 上下 5 の余白
            w.AddComponent<UIElementComponent>(st);
        }
        w.SetParent(st, panel);
        w.ApplyStructuralChanges();
        {
            const auto r = uilayout::ResolveRect(w, st, W, H);
            check(std::fabs(r.x - 510.0f) < 1e-4f && std::fabs(r.y - 405.0f) < 1e-4f
                      && std::fabs(r.w - 180.0f) < 1e-4f && std::fabs(r.h - 90.0f) < 1e-4f,
                  "rect: stretch anchors follow the parent with sizeDelta as margins");
            // 親を広げると子も追従する (固定サイズなら動かない)
            w.GetComponent<RectTransformComponent>(panel)->sizeDelta = { 400.0f, 100.0f };
            const auto r2 = uilayout::ResolveRect(w, st, W, H);
            check(std::fabs(r2.w - 380.0f) < 1e-4f, "rect: stretched child grows with its parent");
            w.GetComponent<RectTransformComponent>(panel)->sizeDelta = { 200.0f, 100.0f };
        }
        const EntityID centered = w.CreateEntity("centered");
        {
            auto* rt = w.AddComponent<RectTransformComponent>(centered);
            rt->anchorMin = { 0.5f, 0.5f };
            rt->anchorMax = { 0.5f, 0.5f };
            rt->pivot = { 0.5f, 0.5f };
            rt->sizeDelta = { 100.0f, 40.0f };
            w.AddComponent<UIElementComponent>(centered);
        }
        w.ApplyStructuralChanges();
        {
            const auto r = uilayout::ResolveRect(w, centered, W, H);
            check(std::fabs(r.x - 450.0f) < 1e-4f && std::fabs(r.y - 380.0f) < 1e-4f,
                  "rect: pivot (0.5,0.5) centres the rect on the anchor point");
        }
        // basis=1 は親の下にいてもキャンバス基準
        const EntityID canvasChild = w.CreateEntity("canvasChild");
        {
            auto* rt = w.AddComponent<RectTransformComponent>(canvasChild);
            rt->anchoredPosition = { 10.0f, 20.0f };
            rt->sizeDelta = { 30.0f, 40.0f };
            rt->basis = 1;
            w.AddComponent<UIElementComponent>(canvasChild);
        }
        w.SetParent(canvasChild, panel);
        w.ApplyStructuralChanges();
        {
            const auto r = uilayout::ResolveRect(w, canvasChild, W, H);
            check(r.x == 10.0f && r.y == 20.0f, "rect: basis=1 ignores the UI parent (canvas)");
        }
        // RectTransform だけのノード (空コンテナ) も基準になれる / UIElement だけは既定で解ける
        const EntityID container = w.CreateEntity("container");
        {
            auto* rt = w.AddComponent<RectTransformComponent>(container);
            rt->anchoredPosition = { 100.0f, 100.0f };
            rt->sizeDelta = { 300.0f, 300.0f };
        }
        const EntityID inContainer = w.CreateEntity("inContainer");
        AddLegacyUi(w, inContainer, 0, 1.0f, 2.0f, 10.0f, 10.0f, 1, true);
        w.SetParent(inContainer, container);
        const EntityID bare = w.CreateEntity("bare");
        w.AddComponent<UIElementComponent>(bare); // RectTransform 無し
        w.ApplyStructuralChanges();
        {
            const auto rc = uilayout::ResolveRect(w, container, W, H);
            const auto ri = uilayout::ResolveRect(w, inContainer, W, H);
            const auto rb = uilayout::ResolveRect(w, bare, W, H);
            check(rc.w == 300.0f && ri.x == 101.0f && ri.y == 102.0f,
                  "rect: a RectTransform-only node resolves and anchors its children");
            check(rb.x == 0.0f && rb.y == 0.0f && rb.w == 160.0f && rb.h == 40.0f,
                  "rect: UIElement without RectTransform resolves with the legacy defaults");
        }
        // FromLegacyRect の basis 規則
        check(uilayout::FromLegacyRect(0, 0, 0, 1, 1, 1, true).basis == 0
                  && uilayout::FromLegacyRect(0, 0, 0, 1, 1, 0, true).basis == 1
                  && uilayout::FromLegacyRect(0, 0, 0, 1, 1, 0, false).basis == 0,
              "rect: legacy space maps to basis (parent / canvas / root)");

        // (3) 回転: (100,100)-(300,150) を中心 (200,125) で 90 度回すと縦長になる。
        //     pivot (0.5,0.5) なので anchoredPosition は**矩形の中心**を指す (Unity と同じ)
        const EntityID rot = w.CreateEntity("rot");
        {
            auto* rt = w.AddComponent<RectTransformComponent>(rot);
            rt->anchoredPosition = { 200.0f, 125.0f };
            rt->sizeDelta = { 200.0f, 50.0f };
            rt->pivot = { 0.5f, 0.5f };
            rt->rotation = 90.0f;
            w.AddComponent<UIElementComponent>(rot);
        }
        w.ApplyStructuralChanges();
        {
            const auto res = uilayout::Resolve(w, rot, W, H, nullptr);
            check(res.visible && res.hasXform, "rect: rotated element carries an xform");
            const auto aabb = uilayout::ResolveRect(w, rot, W, H);
            check(std::fabs(aabb.x - 175.0f) < 1e-3f && std::fabs(aabb.y - 25.0f) < 1e-3f
                      && std::fabs(aabb.w - 50.0f) < 1e-3f && std::fabs(aabb.h - 200.0f) < 1e-3f,
                  "rect: ResolveRect of a rotated element is the AABB");
            // 回転後は上下に 100、左右に 25 — 未回転なら当たらない点が当たり、逆も
            check(uiinteract::HitTest(w, W, H, 200.0f, 215.0f) == rot,
                  "rect: hit test follows the rotated rect (inside after rotation)");
            check(uiinteract::HitTest(w, W, H, 290.0f, 125.0f) == kNullEntity,
                  "rect: hit test misses where only the unrotated rect would be");
            // 恒等ゲート: 回転 0 / スケール 1 の要素は xform を持たない (既存経路のまま)
            w.GetComponent<RectTransformComponent>(rot)->rotation = 0.0f;
            w.GetComponent<RectTransformComponent>(rot)->scale = { 1.0f, 1.0f };
            check(!uilayout::Resolve(w, rot, W, H, nullptr).hasXform,
                  "rect: identity rotation/scale produces no xform (bit-exact legacy path)");
            // スケール 2 は pivot 中心に広がる: 中心 (200,125)、幅 400 → x 0..400
            w.GetComponent<RectTransformComponent>(rot)->scale = { 2.0f, 1.0f };
            const auto sc = uilayout::ResolveRect(w, rot, W, H);
            check(std::fabs(sc.x - 0.0f) < 1e-3f && std::fabs(sc.w - 400.0f) < 1e-3f,
                  "rect: scale grows around the pivot");
            // 親の回転は子にも掛かる (子の未回転矩形は親フレーム上で解け、xform は合成される)
            w.GetComponent<RectTransformComponent>(rot)->scale = { 1.0f, 1.0f };
            w.GetComponent<RectTransformComponent>(rot)->rotation = 90.0f;
            const EntityID rotChild = w.CreateEntity("rotChild");
            AddLegacyUi(w, rotChild, 0, 0.0f, 0.0f, 200.0f, 50.0f, 1, true); // 親と同じ矩形
            w.SetParent(rotChild, rot);
            w.ApplyStructuralChanges();
            const auto rcr = uilayout::ResolveRect(w, rotChild, W, H);
            check(std::fabs(rcr.x - 175.0f) < 1e-3f && std::fabs(rcr.h - 200.0f) < 1e-3f,
                  "rect: a child inherits its parent's rotation");
            // 逆行列の往復
            uilayout::UIXform inv;
            check(uilayout::InvertXform(res.xform, inv), "rect: xform inverts");
            float ax = 0, ay = 0, bx = 0, by = 0;
            uilayout::XformPoint(res.xform, 123.0f, 45.0f, ax, ay);
            uilayout::XformPoint(inv, ax, ay, bx, by);
            check(std::fabs(bx - 123.0f) < 1e-3f && std::fabs(by - 45.0f) < 1e-3f,
                  "rect: xform round-trips through its inverse");
        }

        // (4) UiAux: UI 側のコンポーネントは全部フラグ付き (IsUiOnlyEntity の許容リスト)。
        //     名前が "UI" で始まる / RectTransform / *Canvas は UI 側とみなす
        {
            const ComponentRegistry& reg = ComponentRegistry::Get();
            bool allUi = true;
            int uiCount = 0;
            for (uint32_t t = 0; t < reg.Count(); ++t) {
                const ComponentDesc& d = reg.Desc(t);
                const bool uiName = (std::strncmp(d.name, "UI", 2) == 0
                                     && std::strncmp(d.name, "UiSelfTest", 10) != 0)
                    || std::strcmp(d.name, "RectTransform") == 0;
                if (!uiName) {
                    continue;
                }
                ++uiCount;
                if ((d.flags & kComponentUiAux) == 0) {
                    allUi = false;
                    MYE_LOG_ERROR("    '%s' is a UI component but lacks kComponentUiAux", d.name);
                }
            }
            check(allUi && uiCount >= 2, "rect: every UI component is registered with kComponentUiAux");
            check(uilayout::IsUiOnlyEntity(w, container),
                  "rect: a RectTransform-only entity counts as ui-only (screen UI, not world-follow)");
        }
    }

    // ---- 旧形式シーンのロード (M75a): v3 の UIElement.anchor/x/y/w/h/space → RectTransform ----
    {
        const char* v3 = R"({
          "engine": "MyEngine", "version": 3, "sceneName": "legacy", "nextFileId": 4,
          "entities": [
            { "fileId": 1, "name": "Root", "childIndex": 0, "components": {
                "UIElement": { "kind": 0, "anchor": 4, "x": -100.0, "y": -50.0, "w": 200.0, "h": 100.0,
                               "space": 0, "color": [1,1,1,1] } } },
            { "fileId": 2, "name": "Child", "parent": 1, "childIndex": 0, "components": {
                "UIElement": { "kind": 1, "anchor": 8, "x": -10.0, "y": -5.0, "w": 50.0, "h": 20.0,
                               "space": 1, "text": "hi" } } },
            { "fileId": 3, "name": "Overlay", "parent": 1, "childIndex": 1, "components": {
                "UIElement": { "kind": 2, "anchor": 0, "x": 5.0, "y": 6.0, "w": 70.0, "h": 30.0,
                               "space": 0 } } }
          ] })";
        Scene scene;
        const nlohmann::json doc = nlohmann::json::parse(v3);
        check(SceneSerializer::LoadFromJson(scene, doc), "legacy: v3 scene loads");
        World& w = scene.GetWorld();
        GameObject root = scene.Find("Root");
        GameObject child = scene.Find("Child");
        GameObject overlay = scene.Find("Overlay");
        const auto* rr = root ? w.GetComponent<RectTransformComponent>(root.Id()) : nullptr;
        const auto* rc = child ? w.GetComponent<RectTransformComponent>(child.Id()) : nullptr;
        const auto* ro = overlay ? w.GetComponent<RectTransformComponent>(overlay.Id()) : nullptr;
        check(rr && rc && ro, "legacy: every UIElement gained a RectTransform");
        if (rr && rc && ro) {
            check(rr->anchorMin.x == 0.5f && rr->anchorMax.y == 0.5f && rr->pivot.x == 0.0f
                      && rr->anchoredPosition.x == -100.0f && rr->sizeDelta.y == 100.0f
                      && rr->basis == 0,
                  "legacy: root keeps its 9-grid anchor as matching anchors (basis=parent)");
            check(rc->anchorMin.x == 1.0f && rc->anchorMin.y == 1.0f && rc->basis == 0,
                  "legacy: space=1 child resolves against its parent (basis=0)");
            check(ro->basis == 1, "legacy: space=0 under a UI parent keeps the canvas basis");
            // 解決結果は旧式と同じ (1000x800): root = 中央 (500,400) + (-100,-50)
            const auto r = uilayout::ResolveRect(w, root.Id(), 1000, 800);
            const auto c = uilayout::ResolveRect(w, child.Id(), 1000, 800);
            const auto o = uilayout::ResolveRect(w, overlay.Id(), 1000, 800);
            check(r.x == 400.0f && r.y == 350.0f && c.x == 590.0f && c.y == 445.0f && o.x == 5.0f
                      && o.y == 6.0f,
                  "legacy: converted rects resolve exactly where the v3 layout put them");
        }
        // 保存すると v4 になり、UIElement から旧キーが消え RectTransform が書かれる
        const nlohmann::json saved = SceneSerializer::SaveToJson(scene);
        check(saved.value("version", 0) == Scene::kDocVersion && Scene::kDocVersion == 4,
              "legacy: re-saved document declares v4");
        bool cleaned = true;
        for (const auto& item : saved["entities"]) {
            const auto& comps = item["components"];
            if (!comps.contains("UIElement")) {
                continue;
            }
            cleaned = cleaned && !comps["UIElement"].contains("anchor")
                && !comps["UIElement"].contains("x") && comps.contains("RectTransform");
        }
        check(cleaned, "legacy: v4 output has no legacy layout keys and carries RectTransform");
        // v4 を読み直しても再変換は走らない (RectTransform があるので anchor キーは無視される)
        Scene again;
        check(SceneSerializer::LoadFromJson(again, saved), "legacy: v4 reloads");
        GameObject root2 = again.Find("Root");
        const auto* rr2 = root2 ? again.GetWorld().GetComponent<RectTransformComponent>(root2.Id())
                                : nullptr;
        check(rr2 && std::memcmp(rr2, rr, sizeof(RectTransformComponent)) == 0,
              "legacy: RectTransform survives a v4 save/load round trip bit-exactly");
    }

    if (failCount == 0) {
        MYE_LOG_INFO("==== UI self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== UI self test: %d FAILURE(S) ====", failCount);
    return false;
}

} // namespace mye
