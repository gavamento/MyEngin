#include "Engine/Engine/UI/UISelfTest.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include "Engine/Core/ComponentRegistry.h" // ワールド追従 UI の検証 (スクリプト状態の脇役扱い)
#include "Engine/Core/Components.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/TransformSystem.h" // ワールド追従 UI の検証 (WorldMatrix 生成)
#include "Engine/Engine/UI/UIGeometry.h"
#include "Engine/Engine/UI/UIInteraction.h"
#include "Engine/Engine/UI/UILayout.h"
#include "Engine/Engine/UI/UILayoutGroup.h"     // M75e
#include "Engine/Engine/UI/UIProjectSettings.h" // M75c
#include "Engine/Engine/UI/UINav.h"
#include "Engine/Platform/Input.h"
#include "Engine/Platform/InputActions.h"
#include "Engine/Engine/UI/UITextLayout.h"
#include "Engine/Engine/UI/UIFontMetricsCook.h" // M75d
#include "Engine/Engine/UI/UITextMetrics.h"     // M75d
#include "Engine/Engine/Scene.h"           // M75a: 旧形式 (v3) シーンのロード時変換
#include "Engine/Engine/SceneSerializer.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <iterator>

#include "stb/stb_truetype.h" // M75d: cook した表と実グリフ幅の比較

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
        // ゲーム面 + ゲーム面 px のマウスを持つ入力を組む (レーン 0 の規約、M75b)。
        // 面 1920x1080 = キャンバスと 1:1 なので、下の座標はそのままキャンバス座標でもある
        InputSnapshot in = {};
        in.surfW = 1920;
        in.surfH = 1080;
        InputSnapshot prevIn = {};
        const auto mouse = [&in](float x, float y, bool down) {
            in.mouseSurfX = x;
            in.mouseSurfY = y;
            in.mouseButtons = down ? 1u : 0u;
        };

        // (a) ヒットテスト: 矩形の内と外
        check(uiinteract::HitTest(w, 1920, 1080, 150.0f, 150.0f) == a,
              "interaction: hit test finds the element under the point");
        check(uiinteract::HitTest(w, 1920, 1080, 50.0f, 150.0f) == kNullEntity,
              "interaction: hit test misses outside the rect");

        // (b) hover → press → release で click が 1 tick だけ立つ
        mouse(150.0f, 150.0f, false);
        uiinteract::Evaluate(w, in, prevIn, &actions, st);
        check(st.hovered == a && st.pressed == kNullEntity && st.clicked == kNullEntity,
              "interaction: hovering alone does not press or click");
        mouse(150.0f, 150.0f, true);
        uiinteract::Evaluate(w, in, prevIn, &actions, st);
        check(st.pressed == a && st.clicked == kNullEntity,
              "interaction: the press is captured but does not click yet");
        mouse(150.0f, 150.0f, false);
        uiinteract::Evaluate(w, in, prevIn, &actions, st);
        check(st.clicked == a && st.pressed == kNullEntity,
              "interaction: releasing over the pressed element clicks it");
        uiinteract::Evaluate(w, in, prevIn, &actions, st);
        check(st.clicked == kNullEntity, "interaction: clicked lasts exactly one tick");

        // (c) 押したまま別の要素へ移っても掴んだ相手は変わらない / そこで離しても click しない
        mouse(150.0f, 150.0f, true);
        uiinteract::Evaluate(w, in, prevIn, &actions, st);
        mouse(150.0f, 350.0f, true); // btnB の上へドラッグ
        uiinteract::Evaluate(w, in, prevIn, &actions, st);
        check(st.pressed == a && st.hovered == b,
              "interaction: the captured element does not change while the button is held");
        mouse(150.0f, 350.0f, false);
        uiinteract::Evaluate(w, in, prevIn, &actions, st);
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
        uiinteract::Evaluate(w, in, prevIn, &actions, st);
        check(w.GetComponent<UIElementComponent>(b)->focused == 1
                  && w.GetComponent<UIElementComponent>(a)->focused == 0,
              "interaction: UIElement.focused mirrors the engine focus");

        // (e) 参照先が消えたら手放す (破棄済みの EntityID を握り続けない)
        w.DestroyEntity(b);
        w.ApplyStructuralChanges();
        uiinteract::Evaluate(w, in, prevIn, &actions, st);
        check(st.focused == kNullEntity, "interaction: a destroyed element drops the focus");

        // (f) BitsFor はビットの意味を固定する (ScriptAPI.h の MyeUIButton* と同値)
        st.Clear();
        st.hovered = a;
        st.clicked = a;
        check(uiinteract::BitsFor(st, a) == (uiinteract::kHovered | uiinteract::kClicked),
              "interaction: BitsFor reports exactly the states that hold");

        // (g) M75b: ドラッグ状態。原点は掴んだ tick の位置、閾値 (10 面 px) を超えたら
        //     離すまで dragging を保持する (元の位置へ戻っても落ちない)
        st.Clear();
        mouse(150.0f, 150.0f, true);
        uiinteract::Evaluate(w, in, prevIn, &actions, st);
        check(st.pressed == a && st.pressSurfX == 150.0f && st.pressSurfY == 150.0f
                  && st.dragging == 0 && st.prevSurfX == 150.0f,
              "drag: the press records its origin in surface px");
        mouse(157.0f, 150.0f, true); // 7 px
        uiinteract::Evaluate(w, in, prevIn, &actions, st);
        check(st.dragging == 0, "drag: moving within the threshold is not a drag yet");
        mouse(157.0f, 158.0f, true); // 2 乗距離 49 + 64 = 113 > 100
        uiinteract::Evaluate(w, in, prevIn, &actions, st);
        check(st.dragging == 1 && st.prevSurfX == 157.0f && st.prevSurfY == 158.0f,
              "drag: crossing the threshold starts the drag, prevSurf follows the pointer");
        mouse(150.0f, 150.0f, true);
        uiinteract::Evaluate(w, in, prevIn, &actions, st);
        check(st.dragging == 1 && st.pressSurfX == 150.0f,
              "drag: returning to the origin keeps dragging (latched until release)");
        mouse(150.0f, 150.0f, false);
        uiinteract::Evaluate(w, in, prevIn, &actions, st);
        check(st.dragging == 0 && st.pressed == kNullEntity && st.clicked == a,
              "drag: releasing ends the drag (and still clicks the element it started on)");
        st.changed = a;
        uiinteract::Evaluate(w, in, prevIn, &actions, st);
        check(st.changed == kNullEntity, "interaction: changed lasts exactly one tick");
        st.dragging = 1;
        st.prevSurfX = 5.0f;
        st.Clear();
        check(st.dragging == 0 && st.prevSurfX == 0.0f && st.changed == kNullEntity,
              "interaction: Clear drops the M75b drag state with the rest");

        // (h) M75b: 面が 2 倍のときはキャンバス座標へ半分に換算してから当てる
        //     (記録はゲーム面 px、換算は Evaluate の中 = 描画と同じ CanvasOfInput)
        {
            InputSnapshot big = {};
            big.surfW = 3840;
            big.surfH = 2160;
            big.mouseSurfX = 300.0f; // キャンバス 150
            big.mouseSurfY = 300.0f;
            UIInteractionState st2;
            uiinteract::Evaluate(w, big, prevIn, &actions, st2);
            check(st2.hovered == a, "surface: a 2x surface hit-tests at half the surface px");
        }

        // (i) M75b: 記録を「キャンバス座標」から「ゲーム面 px + 面の寸法」へ変えても、既定キャンバスの
        //     座標は M70b の記録値と**同じビット**になる。M70b は EngineLoop が CanvasSize(実寸) を解き、
        //     CaptureSnapshot が float(mouseX) / scale を記録していた。M75b はその除算を sim 側の
        //     CanvasOfInput + SurfaceToCanvas で行う — 式が 1 回の除算のまま保たれていることの固定
        {
            struct Surf {
                int32_t w;
                int32_t h;
            };
            const Surf surfs[] = { { 960, 540 },  { 1600, 900 },  { 1920, 1080 },
                                   { 1366, 768 }, { 1920, 1200 }, { 3840, 2160 } };
            const int32_t pts[][2] = { { 0, 0 },     { 1, 1 },       { 479, 269 },  { 959, 539 },
                                       { 123, 457 }, { 1365, 767 },  { -5, 20 },    { 3839, 2159 } };
            bool sameCanvas = true;
            bool sameBits = true;
            for (const Surf& s : surfs) {
                const uilayout::CanvasInfo old = uilayout::CanvasSize(s.w, s.h);
                InputSnapshot si = {};
                si.surfW = s.w;
                si.surfH = s.h;
                const uilayout::CanvasInfo now = uilayout::CanvasOfInput(si);
                sameCanvas = sameCanvas && now.w == old.w && now.h == old.h
                    && std::memcmp(&now.scale, &old.scale, sizeof(float)) == 0;
                for (const auto& p : pts) {
                    const float oldX = static_cast<float>(p[0]) / old.scale; // M70b CaptureSnapshot
                    const float oldY = static_cast<float>(p[1]) / old.scale;
                    si.mouseSurfX = static_cast<float>(p[0]); // M75b CaptureSnapshot
                    si.mouseSurfY = static_cast<float>(p[1]);
                    const float newX = uilayout::SurfaceToCanvas(si.mouseSurfX, now);
                    const float newY = uilayout::SurfaceToCanvas(si.mouseSurfY, now);
                    sameBits = sameBits && std::memcmp(&oldX, &newX, sizeof(float)) == 0
                        && std::memcmp(&oldY, &newY, sizeof(float)) == 0;
                }
            }
            check(sameCanvas, "surface: CanvasOfInput equals CanvasSize of the recorded surface");
            check(sameBits,
                  "surface: canvas mouse is bit-identical to the M70b recording (960x540 / 1600x900 / ...)");
            const InputSnapshot unset = {};
            const uilayout::CanvasInfo c0 = uilayout::CanvasOfInput(unset);
            check(c0.w == uilayout::kCanvasRefW && c0.h == uilayout::kCanvasRefH && c0.scale == 1.0f,
                  "surface: an unset surface (0) falls back to the reference canvas with scale 1");
        }
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

    // ---- Canvas + Canvas Scaler (M75c) ----
    // 主張: (1) 既定の解き方は M70b の Expand とビット同一、(2) Shrink / Match が Unity の式どおりで
    // Match の自前 ln/exp は std::pow と一致、(3) 既定と同じ解き方の Canvas の下の UI は Canvas の
    // 無い UI と矩形もヒットも同ビット、(4) 明示 Canvas は自分の単位で解け、既定キャンバス座標の点で
    // 押せる、(5) sortOrder が order より先、(6) クリップは Canvas を越えない、(7) FocusNav は既定
    // キャンバス座標で比べる、(8) 基準解像度の実効値と project_settings.json の読み書き
    {
        // (1) M70b の式 (min + lroundf) を書き直した旧値と比べる
        {
            bool same = true;
            const int res[][2] = { { 960, 540 }, { 1280, 720 }, { 1366, 768 }, { 960, 600 },
                                   { 2560, 1080 }, { 1080, 1920 }, { 0, 0 }, { 1, 1 } };
            for (const auto& r : res) {
                const uilayout::CanvasInfo a = uilayout::CanvasSize(r[0], r[1]);
                const uilayout::CanvasInfo b = uilayout::CanvasSize(r[0], r[1], uilayout::CanvasDesc{});
                uilayout::CanvasInfo old;
                if (r[0] > 0 && r[1] > 0) {
                    const float sx = static_cast<float>(r[0]) / 1920.0f;
                    const float sy = static_cast<float>(r[1]) / 1080.0f;
                    old.scale = (sx < sy) ? sx : sy;
                    old.w = static_cast<int>(std::lroundf(static_cast<float>(r[0]) / old.scale));
                    old.h = static_cast<int>(std::lroundf(static_cast<float>(r[1]) / old.scale));
                }
                same = same && std::memcmp(&a, &b, sizeof(a)) == 0 && a.scale == old.scale
                    && a.w == old.w && a.h == old.h;
            }
            check(same, "scaler: Expand with the default desc is bit-identical to the M70b formula");
        }

        // (2) 基準 1024x768 (4:3) を 960x540 (16:9) で解く
        {
            uilayout::CanvasDesc d;
            d.referenceW = 1024;
            d.referenceH = 768;
            d.scaleMode = uilayout::kScaleExpand;
            const uilayout::CanvasInfo ex = uilayout::CanvasSize(960, 540, d);
            d.scaleMode = uilayout::kScaleShrink;
            const uilayout::CanvasInfo sh = uilayout::CanvasSize(960, 540, d);
            check(ex.scale == 540.0f / 768.0f && ex.w == 1365 && ex.h == 768,
                  "scaler: Expand = min (960x540 on 1024x768 -> 1365x768)");
            check(sh.scale == 960.0f / 1024.0f && sh.w == 1024 && sh.h == 576,
                  "scaler: Shrink = max (960x540 on 1024x768 -> 1024x576)");
            d.scaleMode = uilayout::kScaleMatch;
            d.match = 0.0f;
            const uilayout::CanvasInfo m0 = uilayout::CanvasSize(960, 540, d);
            d.match = 1.0f;
            const uilayout::CanvasInfo m1 = uilayout::CanvasSize(960, 540, d);
            check(m0.scale == sh.scale && m1.scale == ex.scale,
                  "scaler: Match 0 / 1 are exactly the width / height ratio (no pow)");
            bool close = true;
            const int sws[] = { 640, 800, 960, 1280, 1920, 2560, 3840 };
            const int shs[] = { 480, 540, 720, 1080, 1200, 2160 };
            const float ms[] = { 0.1f, 0.25f, 0.5f, 0.75f, 0.9f };
            for (const int sw : sws) {
                for (const int shh : shs) {
                    for (const float m : ms) {
                        d.match = m;
                        const uilayout::CanvasInfo ci = uilayout::CanvasSize(sw, shh, d);
                        const float sxf = static_cast<float>(sw) / 1024.0f;
                        const float syf = static_cast<float>(shh) / 768.0f;
                        const double expect = std::pow(static_cast<double>(sxf), 1.0 - m)
                            * std::pow(static_cast<double>(syf), static_cast<double>(m));
                        const bool ok = std::fabs(static_cast<double>(ci.scale) - expect) <= expect * 1e-6;
                        if (!ok) {
                            MYE_LOG_ERROR("    match %.2f %dx%d -> %.9f (std::pow %.9f)",
                                          static_cast<double>(m), sw, shh,
                                          static_cast<double>(ci.scale), expect);
                        }
                        close = close && ok;
                    }
                }
            }
            check(close, "scaler: Match agrees with std::pow within 1e-6 (deterministic ln/exp)");
            // 基準と同じアスペクトなら 3 モードが同ビット (べき乗を通さない近道)
            d.referenceW = 1920;
            d.referenceH = 1080;
            d.match = 0.37f;
            const uilayout::CanvasInfo mm = uilayout::CanvasSize(1280, 720, d);
            d.scaleMode = uilayout::kScaleExpand;
            const uilayout::CanvasInfo ee = uilayout::CanvasSize(1280, 720, d);
            check(std::memcmp(&mm, &ee, sizeof(mm)) == 0,
                  "scaler: at the reference aspect Match is bit-identical to Expand");
        }

        // (3) 既定と同じ解き方の Canvas (基準 0 = project 既定 + Expand) の下 ≡ Canvas 無し。
        //     Canvas エンティティは**最後に**作る = 要素の entity.index が 2 つの World で揃う
        const auto buildUi = [](World& w, bool withCanvas) {
            const EntityID root = w.CreateEntity("root");
            *w.AddComponent<RectTransformComponent>(root) =
                uilayout::FromLegacyRect(4, -300.0f, -200.0f, 600.0f, 400.0f, 0, false);
            {
                auto* el = w.AddComponent<UIElementComponent>(root);
                el->clipChildren = 1;
                el->order = 1;
            }
            const EntityID stretch = w.CreateEntity("stretch");
            {
                auto* rt = w.AddComponent<RectTransformComponent>(stretch);
                rt->anchorMin = { 0.1f, 0.2f };
                rt->anchorMax = { 0.9f, 0.8f };
                rt->pivot = { 0.5f, 0.5f };
                rt->sizeDelta = { -10.0f, -10.0f };
            }
            w.AddComponent<UIElementComponent>(stretch)->order = 2;
            const EntityID overlay = w.CreateEntity("overlay");
            {
                auto* rt = w.AddComponent<RectTransformComponent>(overlay);
                rt->basis = 1;
                rt->anchorMin = { 1.0f, 1.0f };
                rt->anchorMax = { 1.0f, 1.0f };
                rt->pivot = { 1.0f, 1.0f };
                rt->sizeDelta = { 200.0f, 100.0f };
            }
            w.AddComponent<UIElementComponent>(overlay)->order = 3;
            const EntityID rot = w.CreateEntity("rot");
            {
                auto* rt = w.AddComponent<RectTransformComponent>(rot);
                rt->anchorMin = { 0.5f, 0.5f };
                rt->anchorMax = { 0.5f, 0.5f };
                rt->pivot = { 0.5f, 0.5f };
                rt->sizeDelta = { 300.0f, 80.0f };
                rt->rotation = 30.0f;
            }
            w.AddComponent<UIElementComponent>(rot)->order = 4;
            w.SetParent(stretch, root);
            w.SetParent(overlay, root);
            w.SetParent(rot, root);
            if (withCanvas) {
                const EntityID cv = w.CreateEntity("canvas");
                {
                    auto* rt = w.AddComponent<RectTransformComponent>(cv);
                    rt->anchorMax = { 1.0f, 1.0f };
                    rt->sizeDelta = { 0.0f, 0.0f };
                }
                w.AddComponent<UICanvasComponent>(cv);
                w.SetParent(root, cv);
            }
            w.ApplyStructuralChanges();
            return std::vector<EntityID>{ root, stretch, overlay, rot };
        };
        {
            World w1;
            World w2;
            const std::vector<EntityID> a = buildUi(w1, false);
            const std::vector<EntityID> b = buildUi(w2, true);
            bool same = true;
            const int dims[][2] = { { 1920, 1080 }, { 1920, 1200 }, { 1000, 800 } };
            for (const auto& dm : dims) {
                for (size_t i = 0; i < a.size(); ++i) {
                    const auto ra = uilayout::ResolveRect(w1, a[i], dm[0], dm[1]);
                    const auto rb = uilayout::ResolveRect(w2, b[i], dm[0], dm[1]);
                    const auto ca = uilayout::ResolveClipRect(w1, a[i], dm[0], dm[1]);
                    const auto cb = uilayout::ResolveClipRect(w2, b[i], dm[0], dm[1]);
                    same = same && std::memcmp(&ra, &rb, sizeof(ra)) == 0
                        && std::memcmp(&ca, &cb, sizeof(ca)) == 0
                        && uilayout::CanvasOf(w2, b[i], dm[0], dm[1]).scale == 1.0f;
                }
                for (float y = 0.5f; y < static_cast<float>(dm[1]); y += 37.0f) {
                    for (float x = 0.25f; x < static_cast<float>(dm[0]); x += 37.0f) {
                        const EntityID ha = uiinteract::HitTest(w1, dm[0], dm[1], x, y);
                        const EntityID hb = uiinteract::HitTest(w2, dm[0], dm[1], x, y);
                        same = same && (ha == kNullEntity) == (hb == kNullEntity)
                            && ha.index == hb.index;
                    }
                }
            }
            check(same, "canvas: UI under a default-desc Canvas resolves and hits bit-identically to no Canvas");
        }

        // (4)〜(7) 基準 1024x768 Shrink の Canvas を 1920x1080 の既定キャンバス上で解く
        //     → s' = 1920/1024 = 1.875、キャンバス 1024x576 (どちらも 2 進で割り切れる)
        {
            World w;
            const EntityID cv = w.CreateEntity("canvas");
            w.AddComponent<RectTransformComponent>(cv);
            {
                auto* c = w.AddComponent<UICanvasComponent>(cv);
                c->referenceW = 1024;
                c->referenceH = 768;
                c->scaleMode = uilayout::kScaleShrink;
            }
            const EntityID box = w.CreateEntity("box"); // 既定 RectTransform = 左上・pivot 0
            {
                auto* rt = w.AddComponent<RectTransformComponent>(box);
                rt->anchoredPosition = { 10.0f, 20.0f };
                rt->sizeDelta = { 100.0f, 50.0f };
            }
            w.AddComponent<UIElementComponent>(box)->focusable = 1;
            const EntityID panelE = w.CreateEntity("panel");
            {
                auto* rt = w.AddComponent<RectTransformComponent>(panelE);
                rt->anchorMin = { 0.5f, 0.5f };
                rt->anchorMax = { 0.5f, 0.5f };
                rt->pivot = { 0.5f, 0.5f };
                rt->sizeDelta = { 400.0f, 300.0f };
            }
            w.AddComponent<UIElementComponent>(panelE)->clipChildren = 1;
            const EntityID corner = w.CreateEntity("corner");
            {
                auto* rt = w.AddComponent<RectTransformComponent>(corner);
                rt->basis = 1;
                rt->anchorMin = { 1.0f, 1.0f };
                rt->anchorMax = { 1.0f, 1.0f };
                rt->pivot = { 1.0f, 1.0f };
                rt->sizeDelta = { 50.0f, 50.0f };
            }
            w.AddComponent<UIElementComponent>(corner);
            // 既定キャンバスの要素 (Canvas 無し)。box と重なる位置に order 100 で置く
            const EntityID front = w.CreateEntity("front");
            {
                auto* rt = w.AddComponent<RectTransformComponent>(front);
                rt->sizeDelta = { 400.0f, 200.0f };
            }
            w.AddComponent<UIElementComponent>(front)->order = 100;
            // FocusNav の共通座標の検査用に既定キャンバスへ 2 つ。mid の中心 y=65 は box の中心の
            // **Canvas 単位 (45) より下、既定キャンバス単位 (84.375) より上** に置いてある
            const EntityID mid = w.CreateEntity("mid");
            {
                auto* rt = w.AddComponent<RectTransformComponent>(mid);
                rt->anchoredPosition = { 100.0f, 55.0f };
                rt->sizeDelta = { 20.0f, 20.0f };
            }
            w.AddComponent<UIElementComponent>(mid)->focusable = 1;
            const EntityID below = w.CreateEntity("below");
            {
                auto* rt = w.AddComponent<RectTransformComponent>(below);
                rt->anchoredPosition = { 0.0f, 900.0f };
                rt->sizeDelta = { 100.0f, 50.0f };
            }
            w.AddComponent<UIElementComponent>(below)->focusable = 1;
            w.SetParent(box, cv);
            w.SetParent(panelE, cv);
            w.SetParent(corner, panelE);
            w.ApplyStructuralChanges();

            const uilayout::CanvasInfo ci = uilayout::CanvasOf(w, box, 1920, 1080);
            check(ci.w == 1024 && ci.h == 576 && ci.scale == 1.875f
                      && uilayout::FindCanvas(w, corner) == cv
                      && uilayout::FindCanvas(w, front) == kNullEntity,
                  "canvas: Shrink 1024x768 on the 1920x1080 default canvas is 1024x576 at 1.875");
            const auto rc = uilayout::ResolveRect(w, cv, 1920, 1080);
            check(rc.x == 0.0f && rc.y == 0.0f && rc.w == 1024.0f && rc.h == 576.0f,
                  "canvas: the Canvas element itself always covers its whole canvas");
            const auto rb = uilayout::ResolveRect(w, box, 1920, 1080);
            check(rb.x == 10.0f && rb.y == 20.0f && rb.w == 100.0f && rb.h == 50.0f,
                  "canvas: children resolve in the canvas's own units");
            const auto rk = uilayout::ResolveRect(w, corner, 1920, 1080);
            check(rk.x == 974.0f && rk.y == 526.0f && rk.w == 50.0f && rk.h == 50.0f,
                  "canvas: basis=1 is the owning canvas, not the default canvas");
            // (6) corner は panel (312..712, 138..438) の clipChildren の下で完全に外 → 見えない。
            //     panel 自身の祖先 (Canvas) より上のクリップは効かない = 全面
            const auto vk = uilayout::ResolveVisibleRect(w, corner, 1920, 1080);
            const auto cp = uilayout::ResolveClipRect(w, panelE, 1920, 1080);
            check(vk.w <= 0.0f && cp.x == 0.0f && cp.y == 0.0f && cp.w == 1024.0f && cp.h == 576.0f,
                  "canvas: clipping works inside a Canvas and starts from the canvas bounds");

            // (4)(5) 既定キャンバス座標 (20,40) → Canvas 単位 (10.67, 21.3) = box の中。
            //        front (0..400, 0..200) とも重なるので、決め手はキーの順
            check(uiinteract::HitTest(w, 1920, 1080, 20.0f, 40.0f) == front,
                  "canvas: equal sortOrder (0) falls back to element order (front order 100 wins)");
            w.GetComponent<UICanvasComponent>(cv)->sortOrder = 1;
            check(uiinteract::HitTest(w, 1920, 1080, 20.0f, 40.0f) == box,
                  "canvas: a higher Canvas sortOrder beats a higher element order");
            check(uiinteract::HitTest(w, 1920, 1080, 15.0f, 30.0f) == front,
                  "canvas: the point is rescaled per canvas (15,30 -> 8,16 misses the box)");
            w.GetComponent<UICanvasComponent>(cv)->sortOrder = -1;
            check(uiinteract::HitTest(w, 1920, 1080, 20.0f, 40.0f) == front,
                  "canvas: a negative sortOrder puts the whole Canvas behind the default canvas");
            // (7) box の中心は既定座標で y=84.375。下へ行くと mid (y=65) は上なので飛ばして below へ、
            //     上へ行くと mid。Canvas 単位のまま比べると box の中心 y=45 で mid を「下」と誤判定する
            check(uiinteract::FindNextFocus(w, 1920, 1080, box, uinav::kNavDown) == below
                      && uiinteract::FindNextFocus(w, 1920, 1080, box, uinav::kNavUp) == mid,
                  "canvas: focus navigation compares rects in default-canvas units");
        }

        // (8) 実効値と JSON
        {
            uilayout::SetDefaultCanvasReference(1280, 720);
            const uilayout::CanvasInfo big = uilayout::CanvasSize(1920, 1080);
            const uilayout::CanvasInfo zero = uilayout::CanvasSize(0, 0);
            InputSnapshot headless = {};
            const uilayout::CanvasInfo fromInput = uilayout::CanvasOfInput(headless);
            World w;
            const EntityID cv = w.CreateEntity("canvas");
            w.AddComponent<UICanvasComponent>(cv); // 基準 0 = project 既定に従う
            const uilayout::CanvasInfo follow = uilayout::CanvasOfEntity(w, cv, 1280, 720);
            uilayout::SetDefaultCanvasReference(0, -5); // <= 0 は 1920x1080 へ倒す
            const bool reset = uilayout::DefaultCanvasDesc().referenceW == uilayout::kCanvasRefW
                && uilayout::DefaultCanvasDesc().referenceH == uilayout::kCanvasRefH;
            check(big.scale == 1.5f && big.w == 1280 && big.h == 720 && zero.w == 1280
                      && zero.h == 720 && fromInput.w == 1280 && follow.scale == 1.0f
                      && follow.w == 1280 && reset,
                  "settings: the project reference drives the default canvas and reference-0 Canvases");

            uilayout::ProjectUiSettings p;
            const bool okParse = uilayout::ParseProjectUiSettings(
                R"({"particleBackend":"cpu","ui":{"referenceW":1280,"referenceH":720}})", p);
            check(okParse && p.referenceW == 1280 && p.referenceH == 720,
                  "settings: ui.referenceW/H parse");
            uilayout::ProjectUiSettings q;
            const bool noUi = !uilayout::ParseProjectUiSettings(R"({"particleBackend":"gpu"})", q);
            uilayout::ProjectUiSettings r;
            const bool bad = !uilayout::ParseProjectUiSettings(R"({"ui":{"referenceW":0,"referenceH":720}})", r);
            uilayout::ProjectUiSettings half;
            const bool halfBad = !uilayout::ParseProjectUiSettings(R"({"ui":{"referenceW":1280}})", half);
            check(noUi && q.referenceW == 1920 && bad && r.referenceW == 1920 && r.referenceH == 1080
                      && halfBad && half.referenceW == 1920,
                  "settings: missing / out-of-range / half-specified ui falls back to 1920x1080 as a pair");

            // マージ保存: 他のキーを消さず、読み直すと同じ値
            std::error_code ec;
            const std::filesystem::path dir =
                std::filesystem::temp_directory_path(ec) / L"mye_ui_settings_selftest";
            std::filesystem::remove_all(dir, ec);
            std::filesystem::create_directories(dir, ec);
            {
                std::ofstream f(dir / L"project_settings.json", std::ios::binary);
                f << R"({"particleBackend":"gpu","physicsLayers":["a"]})";
            }
            uilayout::ProjectUiSettings save;
            save.referenceW = 1600;
            save.referenceH = 1200;
            const bool saved = uilayout::SaveProjectUiSettings(dir.wstring(), save);
            const uilayout::ProjectUiSettings loaded = uilayout::LoadProjectUiSettings(dir.wstring());
            std::string text;
            {
                std::ifstream f(dir / L"project_settings.json", std::ios::binary);
                std::stringstream ss;
                ss << f.rdbuf();
                text = ss.str();
            }
            check(saved && loaded == save && text.find("\"particleBackend\": \"gpu\"") != std::string::npos
                      && text.find("physicsLayers") != std::string::npos,
                  "settings: save merges the ui section and keeps the other keys");
            uilayout::ProjectUiSettings tooBig;
            tooBig.referenceW = 100000;
            check(!uilayout::SaveProjectUiSettings(dir.wstring(), tooBig),
                  "settings: an out-of-range reference is refused instead of written");
            std::filesystem::remove_all(dir, ec);
            const uilayout::ProjectUiSettings none =
                uilayout::LoadProjectUiSettings(dir.wstring());
            check(none.referenceW == 1920 && none.referenceH == 1080,
                  "settings: no project_settings.json = 1920x1080");
        }
    }

    // ---- M75d: フォント計測表 ----
    {
        // (1) 固定メトリクス (表なし) は内蔵 8x8 の組版 (textlayout::LayoutText) と**ビット一致**する。
        //     golden を撮る構成 (--font-embedded) で Layout の箱と描いた文字が 1 画素もずれない根拠。
        //     8x8 の GlyphScale(fs) = kUILineH × fs / baseLineH(= kUILineH) = fs
        {
            FontGlyphMap embedded;
            for (uint32_t cp = 0x20; cp < 0x80; ++cp) {
                FontGlyphInfo g;
                g.advance = 8.0f;
                g.valid = true;
                embedded[cp] = g;
            }
            const uitext::FontMetrics none;
            const char* texts[] = { "Button", "Hello, World!", "ab\ncd\n", "\n", "",
                                    "line one\n\nline three", "wrap me please into several lines",
                                    "\t", "a\tb", "x\n\ny\n\n", "\xE6\x97\xA5\xE6\x9C\xAC UI" };
            const float scales[] = { 1.0f, 1.5f, 2.0f, 0.75f };
            const float widths[] = { 0.0f, 40.0f, 72.0f, 100.5f };
            bool same = true;
            std::vector<textlayout::Line> lines;
            for (const char* t : texts) {
                for (float fs : scales) {
                    for (float mw : widths) {
                        for (int wrap = 0; wrap < 2; ++wrap) {
                            textlayout::LayoutText(embedded, t, fs, wrap != 0, mw, lines);
                            float lw = 0.0f;
                            for (const textlayout::Line& ln : lines) {
                                lw = std::max(lw, ln.width);
                            }
                            const uitext::TextSize ts = uitext::Measure(t, fs, wrap != 0, mw, none);
                            const float expectH =
                                static_cast<float>(lines.size()) * (uitext::kLineH * fs);
                            if (ts.lines != static_cast<int32_t>(lines.size()) || ts.w != lw
                                || ts.h != expectH) {
                                MYE_LOG_ERROR("    measure mismatch: \"%s\" fs=%g maxW=%g wrap=%d "
                                              "lines %d/%zu w %g/%g", t, fs, mw, wrap, ts.lines,
                                              lines.size(), ts.w, lw);
                                same = false;
                            }
                        }
                    }
                }
            }
            check(same && none.Empty() && none.Hash() == 0 && none.AdvanceOf('A') == 800,
                  "fontmetrics: fixed metrics measure exactly like the embedded 8x8 layout");
            check(uitext::Measure(nullptr, 1.0f, false, 0.0f, none).lines == 0
                      && uitext::Measure("", 1.0f, false, 0.0f, none).h == 0.0f,
                  "fontmetrics: null / empty text measures as zero lines");
        }

        // (2) 構築 → 直列化 → 読み戻し
        std::vector<uitext::GlyphAdvance> gl;
        for (uint32_t cp = 0x20; cp <= 0x7E; ++cp) {
            gl.push_back({ cp, static_cast<uint16_t>(300 + (cp * 37) % 400) });
        }
        for (uint32_t cp = 0x3040; cp <= 0x30FF; ++cp) {
            gl.push_back({ cp, static_cast<uint16_t>((cp == 0x3099 || cp == 0x309A) ? 0 : 1000) });
        }
        gl.push_back({ 0x4E00, 1000 });
        gl.push_back({ 0xFFFD, 1000 });
        uitext::FontMetrics built;
        std::string err;
        const bool okBuild = uitext::FontMetrics::Build("Test.ttf", 1234, 32, 11520, gl, built, &err);
        const std::string text = uitext::SerializeFontMetricsJson(built);
        {
            uitext::FontMetrics parsed;
            const bool okParse = uitext::ParseFontMetricsJson(text, parsed, &err);
            const std::vector<uitext::GlyphAdvance> back = parsed.Glyphs();
            bool sameGlyphs = back.size() == gl.size();
            for (size_t i = 0; sameGlyphs && i < gl.size(); ++i) {
                sameGlyphs = back[i].codepoint == gl[i].codepoint && back[i].advance == gl[i].advance;
            }
            check(okBuild && okParse && sameGlyphs && built.Hash() != 0
                      && parsed.Hash() == built.Hash() && parsed.FontName() == "Test.ttf"
                      && parsed.FontBytes() == 1234 && parsed.LineH256() == 11520
                      && parsed.GlyphCount() == gl.size()
                      && uitext::SerializeFontMetricsJson(parsed) == text,
                  "fontmetrics: build -> serialize -> parse round-trips byte-identically");
            // 同値の並びは長さ 1 の配列に畳まれ、短い並びは明示配列のまま
            check(text.find("[12352, 12440, [1000]]") != std::string::npos
                      && text.find("[12441, 12442, [0, 0]]") != std::string::npos
                      && text.find("[12443, 12543, [1000]]") != std::string::npos
                      && text.find("[19968, 19968, [1000]]") != std::string::npos,
                  "fontmetrics: uniform runs collapse to a single advance");

            // 改行コード (core.autocrlf) と区間の切り方はハッシュに出ない
            std::string crlf;
            for (char c : text) {
                if (c == '\n') {
                    crlf += '\r';
                }
                crlf += c;
            }
            uitext::FontMetrics fromCrlf;
            const std::string head =
                R"({"format":1,"font":"a.ttf","fontBytes":0,"basePx":32,"lineH256":0,"advPerLine":1000,"ranges":)";
            uitext::FontMetrics split;
            uitext::FontMetrics joined;
            uitext::FontMetrics uniform;
            const bool okSplit = uitext::ParseFontMetricsJson(
                head + "[[65,65,[500]],[66,66,[500]],[67,67,[600]]]}", split);
            const bool okJoined = uitext::ParseFontMetricsJson(head + "[[65,67,[500,500,600]]]}", joined);
            const bool okUniform =
                uitext::ParseFontMetricsJson(head + "[[65,66,[500]],[67,67,[600]]]}", uniform);
            check(uitext::ParseFontMetricsJson(crlf, fromCrlf) && fromCrlf.Hash() == built.Hash()
                      && okSplit && okJoined && okUniform && split.Hash() == joined.Hash()
                      && joined.Hash() == uniform.Hash(),
                  "fontmetrics: hash ignores line endings and how ranges are chunked");

            // (3) 規則違反は読まない (空 = 固定メトリクスへ倒れる)
            const std::string bad[] = {
                R"({"format":2,"font":"a.ttf","fontBytes":0,"basePx":32,"lineH256":0,"advPerLine":1000,"ranges":[[65,65,[500]]]})",
                R"({"format":1,"font":"a.ttf","fontBytes":0,"basePx":32,"lineH256":0,"advPerLine":256,"ranges":[[65,65,[500]]]})",
                R"({"format":1,"fontBytes":0,"basePx":32,"lineH256":0,"advPerLine":1000,"ranges":[[65,65,[500]]]})",
                head + "[[65,70,[500]],[70,71,[500]]]}",   // 重なり
                head + "[[70,71,[500]],[65,66,[500]]]}",   // 降順
                head + "[[65,67,[500,500]]]}",             // 配列長が 1 でも区間長でもない
                head + "[[65,65,[65535]]]}",               // 番兵値
                head + "[[65,65,[-1]]]}",
                head + "[[65536,65536,[1]]]}",             // BMP 外
                head + "[[66,65,[1]]]}",                   // start > end
                head + "[[65,65]]}",
                head + "[]}",                              // 0 文字 = 表なしと区別できない
                "{",
            };
            bool allRejected = true;
            for (const std::string& b : bad) {
                uitext::FontMetrics m;
                if (uitext::ParseFontMetricsJson(b, m) || !m.Empty() || m.Hash() != 0) {
                    MYE_LOG_ERROR("    accepted a bad table: %s", b.c_str());
                    allRejected = false;
                }
            }
            check(allRejected, "fontmetrics: malformed tables are rejected");
        }

        // (4) 送り幅の引き方と計測
        {
            const uint16_t advA = static_cast<uint16_t>(300 + ('A' * 37) % 400);
            const uint16_t advB = static_cast<uint16_t>(300 + ('B' * 37) % 400);
            const uint16_t advQ = static_cast<uint16_t>(300 + ('?' * 37) % 400);
            uitext::FontMetrics noQuestion;
            const bool okNoQ = uitext::FontMetrics::Build("x.ttf", 0, 32, 0, { { 'A', 500 } }, noQuestion);
            check(built.AdvanceOf('A') == advA && built.Has('A') && !built.Has(0x4E01)
                      && built.AdvanceOf(0x4E01) == advQ && built.AdvanceOf(0x1F600) == advQ
                      && okNoQ && noQuestion.AdvanceOf('B') == uitext::kFixedAdvance,
                  "fontmetrics: missing glyphs measure as '?' (like the renderer), then fixed");
            const uitext::TextSize ab = uitext::Measure("AB", 2.0f, false, 0.0f, built);
            const float expectAb =
                static_cast<float>(advA + advB) * (uitext::kLineH * 2.0f) / static_cast<float>(uitext::kAdvPerLine);
            const uitext::TextSize wrapped = uitext::Measure("AAAA", 1.0f, true, 12.0f, noQuestion);
            const uitext::TextSize narrow = uitext::Measure("AAAA", 1.0f, true, 1.0f, noQuestion);
            check(ab.w == expectAb && ab.lines == 1 && ab.h == 20.0f && wrapped.lines == 2
                      && wrapped.w == 10.0f && narrow.lines == 4 && narrow.w == 5.0f,
                  "fontmetrics: measure sums table advances and wraps (first glyph always fits)");
            uitext::FontMetrics badBuild;
            const bool rejectOrder = !uitext::FontMetrics::Build(
                "x", 0, 0, 0, { { 'B', 1 }, { 'A', 1 } }, badBuild);
            const bool rejectRange = !uitext::FontMetrics::Build("x", 0, 0, 0, { { 0x10000, 1 } }, badBuild);
            check(rejectOrder && rejectRange && badBuild.Empty(),
                  "fontmetrics: build rejects unsorted / non-BMP glyphs");
        }

        // (5) プロジェクトからのロード: 表は描画フォント (名前順の先頭) の stem に付いて行く
        {
            std::error_code ec;
            const std::filesystem::path dir =
                std::filesystem::temp_directory_path(ec) / L"mye_fontmetrics_selftest";
            std::filesystem::remove_all(dir, ec);
            std::filesystem::create_directories(dir, ec);
            const std::wstring root = dir.wstring();
            const bool noFonts = uitext::LoadProjectFontMetrics(root).Empty();
            const uitext::FontMetricsCookResult noFontCook = uitext::CookProjectFontMetrics(root);
            std::filesystem::create_directories(dir / L"fonts", ec);
            {
                std::ofstream f(dir / L"fonts" / L"B.ttf", std::ios::binary);
                f << "0123456789abcdef"; // 16 バイトの偽物 (ロードは名前とサイズしか見ない)
            }
            const bool noTable = uitext::LoadProjectFontMetrics(root).Empty(); // WARN 1 行が出る
            uitext::FontMetrics bm;
            uitext::FontMetrics::Build("B.ttf", 16, 32, 0, gl, bm);
            {
                std::ofstream f(std::filesystem::path(
                                    uitext::FontMetricsPathFor((dir / L"fonts" / L"B.ttf").wstring())),
                                std::ios::binary);
                const std::string t = uitext::SerializeFontMetricsJson(bm);
                f.write(t.data(), static_cast<std::streamsize>(t.size()));
            }
            const uitext::FontMetrics loadedB = uitext::LoadProjectFontMetrics(root);
            {
                std::ofstream f(dir / L"fonts" / L"A.ttc", std::ios::binary); // 名前順で B より前
                f << "x";
            }
            const bool followsAtlas = uitext::LoadProjectFontMetrics(root).Empty();
            std::filesystem::remove_all(dir, ec);
            check(noFonts && noFontCook.noFont && !noFontCook.ok && noTable
                      && loadedB.Hash() == bm.Hash() && !loadedB.Empty() && followsAtlas
                      && uitext::FontMetricsPathFor(L"C:\\x\\B.ttf") == L"C:\\x\\B.fontmetrics.json",
                  "fontmetrics: the table is <atlas font stem>.fontmetrics.json, else fixed metrics");

            uitext::SetActiveFontMetrics(bm);
            const bool setOk = uitext::ActiveFontMetrics().Hash() == bm.Hash();
            uitext::SetActiveFontMetrics({}); // 後続のテストへ持ち越さない
            check(setOk && uitext::ActiveFontMetrics().Empty(),
                  "fontmetrics: active table is set once and can be reset");
        }

        // (6) 実フォントの cook (%WINDIR%\Fonts\arial.ttf がある機械だけ)。
        //     表で測った幅 >= FontAtlas と同じ式で組んだ実グリフ幅 (切り上げの効果) を固定する
        {
            std::wstring arial;
            wchar_t* windirEnv = nullptr;
            size_t envLen = 0;
            if (_wdupenv_s(&windirEnv, &envLen, L"WINDIR") == 0 && windirEnv != nullptr) {
                arial = std::wstring(windirEnv) + L"\\Fonts\\arial.ttf";
                free(windirEnv);
            }
            std::vector<uint8_t> ttf;
            if (!arial.empty()) {
                std::ifstream f(std::filesystem::path(arial), std::ios::binary);
                if (f) {
                    ttf.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
                }
            }
            if (ttf.empty()) {
                MYE_LOG_INFO("  SKIP: fontmetrics: arial.ttf not found (real-font cook check)");
            } else {
                uitext::FontMetrics cooked;
                const bool okCook = uitext::CookFontMetricsFromTtf(ttf, "arial.ttf", cooked, &err);
                uitext::FontMetrics reparsed;
                const bool okRe = uitext::ParseFontMetricsJson(uitext::SerializeFontMetricsJson(cooked), reparsed);

                stbtt_fontinfo info{};
                bool boundOk = false;
                if (okCook && stbtt_InitFont(&info, ttf.data(), stbtt_GetFontOffsetForIndex(ttf.data(), 0))) {
                    const float scale = stbtt_ScaleForPixelHeight(&info, 32.0f);
                    int asc = 0, desc = 0, gap = 0;
                    stbtt_GetFontVMetrics(&info, &asc, &desc, &gap);
                    const float baseLineHPx = static_cast<float>(asc - desc + gap) * scale;
                    FontGlyphMap real;
                    for (uint32_t cp = 0x20; cp < 0x7F; ++cp) {
                        const int gi = stbtt_FindGlyphIndex(&info, static_cast<int>(cp));
                        if (gi == 0) {
                            continue;
                        }
                        int adv = 0, lsb = 0;
                        stbtt_GetGlyphHMetrics(&info, gi, &adv, &lsb);
                        FontGlyphInfo g;
                        g.advance = static_cast<float>(adv) * scale;
                        g.valid = true;
                        real[cp] = g;
                    }
                    const char* sample = "The quick brown fox jumps over the lazy dog 0123456789";
                    const float n = static_cast<float>(std::strlen(sample));
                    boundOk = true;
                    for (float fs : { 1.0f, 2.5f }) {
                        const float k = uitext::kLineH * fs / baseLineHPx; // FontAtlas::GlyphScale と同じ式
                        std::vector<textlayout::Line> lines;
                        textlayout::LayoutText(real, sample, k, false, 0.0f, lines);
                        const float realW = lines.empty() ? 0.0f : lines[0].width;
                        const uitext::TextSize ts = uitext::Measure(sample, fs, false, 0.0f, cooked);
                        const float slack = n * uitext::kLineH * fs / static_cast<float>(uitext::kAdvPerLine);
                        if (!(ts.w + 1e-3f >= realW && ts.w <= realW + slack + 1e-3f)) {
                            MYE_LOG_ERROR("    fs=%g table %g vs real %g (slack %g)", fs, ts.w, realW, slack);
                            boundOk = false;
                        }
                    }
                }

                // プロジェクトの cook: 書く → 2 回目は書かない → ロードで同じ表
                std::error_code ec;
                const std::filesystem::path dir =
                    std::filesystem::temp_directory_path(ec) / L"mye_fontmetrics_cook_selftest";
                std::filesystem::remove_all(dir, ec);
                std::filesystem::create_directories(dir / L"fonts", ec);
                {
                    std::ofstream f(dir / L"fonts" / L"arial.ttf", std::ios::binary);
                    f.write(reinterpret_cast<const char*>(ttf.data()), static_cast<std::streamsize>(ttf.size()));
                }
                const uitext::FontMetricsCookResult r1 = uitext::CookProjectFontMetrics(dir.wstring());
                const uitext::FontMetricsCookResult r2 = uitext::CookProjectFontMetrics(dir.wstring());
                const uitext::FontMetrics loaded = uitext::LoadProjectFontMetrics(dir.wstring());
                std::filesystem::remove_all(dir, ec);

                check(okCook && okRe && reparsed.Hash() == cooked.Hash() && cooked.Has('A')
                          && !cooked.Has(0x3042) && cooked.GlyphCount() > 90,
                      "fontmetrics: cook arial.ttf and round-trip it");
                check(boundOk, "fontmetrics: cooked advances never measure narrower than the glyphs");
                check(r1.ok && !r1.unchanged && r2.ok && r2.unchanged && r1.hash == cooked.Hash()
                          && loaded.Hash() == cooked.Hash() && loaded.FontName() == "arial.ttf",
                      "fontmetrics: project cook writes once, then reports up to date");
            }
        }
    }

    // ---- M75e: 自動レイアウト (Layout Group / LayoutElement / ContentSizeFitter) ----
    // 期待値は Unity の配置式 (UILayoutGroup.cpp が移した HorizontalOrVerticalLayoutGroup /
    // GridLayoutGroup / LayoutUtility) を手で追った値。テキストは固定メトリクス (全文字 0.8 行 =
    // fontScale 1 で 8 送り・行高 10) で測るので、計測表を空にしてから回す
    {
        const uitext::FontMetrics savedMetrics = uitext::ActiveFontMetrics();
        uitext::SetActiveFontMetrics({});

        const auto rectNear = [](const uilayout::UIRect& r, float x, float y, float rw, float rh) {
            return std::fabs(r.x - x) < 1e-3f && std::fabs(r.y - y) < 1e-3f
                && std::fabs(r.w - rw) < 1e-3f && std::fabs(r.h - rh) < 1e-3f;
        };
        const auto rr = [&](World& w, EntityID e) { return uilayout::ResolveRect(w, e, W, H); };
        // 左上アンカー・pivot 0 の単色パネル (sizeDelta = 大きさ)
        const auto box = [](World& w, const char* name, EntityID parent, float x, float y,
                            float rw, float rh) {
            const EntityID e = w.CreateEntity(name);
            auto* rt = w.AddComponent<RectTransformComponent>(e);
            rt->anchoredPosition = { x, y };
            rt->sizeDelta = { rw, rh };
            w.AddComponent<UIElementComponent>(e);
            if (!parent.IsNull()) {
                w.SetParent(e, parent);
            }
            return e;
        };
        const auto text = [](World& w, const char* name, EntityID parent, const char* s,
                             float fontScale, int wrap) {
            const EntityID e = w.CreateEntity(name);
            w.AddComponent<RectTransformComponent>(e);
            auto* el = w.AddComponent<UIElementComponent>(e);
            el->kind = 1;
            el->fontScale = fontScale;
            el->wrap = wrap;
            std::snprintf(el->text, sizeof(el->text), "%s", s);
            if (!parent.IsNull()) {
                w.SetParent(e, parent);
            }
            return e;
        };

        // (1) 水平・サイズを制御しない。group (100,100) 200x60、padding (10,5,10,5)、spacing 4
        {
            World w;
            const EntityID g = box(w, "group", kNullEntity, 100.0f, 100.0f, 200.0f, 60.0f);
            {
                auto* lg = w.AddComponent<UILayoutGroupComponent>(g);
                lg->padding = { 10.0f, 5.0f, 10.0f, 5.0f };
                lg->spacing = { 4.0f, 99.0f }; // 水平は y を読まない
                lg->forceExpandWidth = 0;
                lg->forceExpandHeight = 0;
            }
            const EntityID a = box(w, "a", g, 900.0f, 900.0f, 50.0f, 20.0f); // 位置は効かない
            const EntityID b = box(w, "b", g, 0.0f, 0.0f, 30.0f, 40.0f);
            w.ApplyStructuralChanges();
            check(rectNear(rr(w, a), 110.0f, 105.0f, 50.0f, 20.0f)
                      && rectNear(rr(w, b), 164.0f, 105.0f, 30.0f, 40.0f),
                  "layout: a horizontal group places children left to right inside padding and spacing");
            // 中央揃え: 並び (84) を 200 の中央へ、各子を 60 の中央へ
            w.GetComponent<UILayoutGroupComponent>(g)->childAlignment = 4;
            check(rectNear(rr(w, a), 158.0f, 120.0f, 50.0f, 20.0f)
                      && rectNear(rr(w, b), 212.0f, 110.0f, 30.0f, 40.0f),
                  "layout: middle-center alignment centres the run and each child across the group");
            w.GetComponent<UILayoutGroupComponent>(g)->reverseArrangement = 1;
            check(rectNear(rr(w, b), 158.0f, 110.0f, 30.0f, 40.0f)
                      && rectNear(rr(w, a), 192.0f, 120.0f, 50.0f, 20.0f),
                  "layout: reverseArrangement lays out the last sibling first");
            // 広げる + 制御しない: 余り 96 を 2 つの枠へ 48 ずつ配るが、子は sizeDelta のまま枠の中央に立つ
            w.GetComponent<UILayoutGroupComponent>(g)->reverseArrangement = 0;
            w.GetComponent<UILayoutGroupComponent>(g)->forceExpandWidth = 1;
            check(rectNear(rr(w, a), 134.0f, 120.0f, 50.0f, 20.0f)
                      && rectNear(rr(w, b), 236.0f, 110.0f, 30.0f, 40.0f),
                  "layout: forceExpand without size control widens the cell, not the child");
            check(uilayout::LayoutDrivenBits(w, a) == uilayout::kDrivenByGroup
                      && uilayout::LayoutDrivenBits(w, g) == 0,
                  "layout: an uncontrolled child reports only its position as driven");
        }

        // (2)(3) 幅を制御する。preferred は sizeDelta (何も指定が無い要素) = 50 / 30
        {
            World w;
            const EntityID g = box(w, "group", kNullEntity, 100.0f, 100.0f, 200.0f, 60.0f);
            {
                auto* lg = w.AddComponent<UILayoutGroupComponent>(g);
                lg->controlChildWidth = 1;
                lg->forceExpandHeight = 0; // forceExpandWidth は既定の 1
            }
            const EntityID a = box(w, "a", g, 0.0f, 0.0f, 50.0f, 20.0f);
            const EntityID b = box(w, "b", g, 0.0f, 0.0f, 30.0f, 40.0f);
            w.ApplyStructuralChanges();
            check(rectNear(rr(w, a), 100.0f, 100.0f, 110.0f, 20.0f)
                      && rectNear(rr(w, b), 210.0f, 100.0f, 90.0f, 40.0f),
                  "layout: controlled width + forceExpand hands the surplus out evenly on top of preferred");
            check(uilayout::LayoutDrivenBits(w, a) == (uilayout::kDrivenByGroup | uilayout::kDrivenWidth),
                  "layout: a width-controlled child reports position and width as driven");
            w.AddComponent<UILayoutElementComponent>(b)->flexibleWidth = 3.0f;
            w.ApplyStructuralChanges();
            check(rectNear(rr(w, a), 100.0f, 100.0f, 80.0f, 20.0f)
                      && rectNear(rr(w, b), 180.0f, 100.0f, 120.0f, 40.0f),
                  "layout: LayoutElement.flexibleWidth weights the surplus (1 : 3)");

            // (3) 足りないときは min と preferred の間を同じ比で縮める (minMaxLerp = (40-30)/(80-30))
            w.GetComponent<UILayoutElementComponent>(b)->flexibleWidth = -1.0f;
            w.GetComponent<UILayoutElementComponent>(b)->minWidth = 20.0f;
            w.GetComponent<UILayoutGroupComponent>(g)->forceExpandWidth = 0;
            w.GetComponent<RectTransformComponent>(g)->sizeDelta.x = 40.0f;
            w.AddComponent<UILayoutElementComponent>(a)->minWidth = 10.0f;
            w.ApplyStructuralChanges();
            check(rectNear(rr(w, a), 100.0f, 100.0f, 18.0f, 20.0f)
                      && rectNear(rr(w, b), 118.0f, 100.0f, 22.0f, 40.0f),
                  "layout: below the preferred total children shrink from preferred toward min");
        }

        // (4) 垂直: Active でない子と ignoreLayout の子は並べない (自分の RectTransform で解く)。兄弟順
        {
            World w;
            const EntityID g = box(w, "column", kNullEntity, 0.0f, 0.0f, 100.0f, 200.0f);
            {
                auto* lg = w.AddComponent<UILayoutGroupComponent>(g);
                lg->kind = uilayout::kLayoutVertical;
                lg->spacing = { 99.0f, 5.0f }; // 垂直は x を読まない
                lg->controlChildWidth = 1;
                lg->forceExpandHeight = 0;
            }
            const EntityID a = box(w, "a", g, 0.0f, 0.0f, 10.0f, 30.0f);
            const EntityID b = box(w, "b", g, 0.0f, 0.0f, 10.0f, 20.0f);
            const EntityID c = box(w, "c", g, 7.0f, 8.0f, 10.0f, 40.0f);
            const EntityID d = box(w, "d", g, 0.0f, 0.0f, 10.0f, 10.0f);
            w.AddComponent<ActiveComponent>(b)->enabled = 0;
            w.AddComponent<UILayoutElementComponent>(c)->ignoreLayout = 1;
            w.ApplyStructuralChanges();
            check(rectNear(rr(w, a), 0.0f, 0.0f, 100.0f, 30.0f) && rectNear(rr(w, d), 0.0f, 35.0f, 100.0f, 10.0f)
                      && rectNear(rr(w, c), 7.0f, 8.0f, 10.0f, 40.0f)
                      && rectNear(rr(w, b), 0.0f, 0.0f, 10.0f, 20.0f)
                      && uilayout::LayoutDrivenBits(w, c) == 0,
                  "layout: a vertical group skips inactive and ignoreLayout children");
            w.SetSiblingIndex(d, 0);
            w.ApplyStructuralChanges();
            check(rectNear(rr(w, d), 0.0f, 0.0f, 100.0f, 10.0f) && rectNear(rr(w, a), 0.0f, 15.0f, 100.0f, 30.0f),
                  "layout: sibling order follows the Hierarchy (SetSiblingIndex moves the child)");
        }

        // (5) Grid: cell 20x10、spacing (2,3)、7 個
        {
            World w;
            const EntityID g = box(w, "grid", kNullEntity, 0.0f, 0.0f, 100.0f, 100.0f);
            {
                auto* lg = w.AddComponent<UILayoutGroupComponent>(g);
                lg->kind = uilayout::kLayoutGrid;
                lg->cellSize = { 20.0f, 10.0f };
                lg->spacing = { 2.0f, 3.0f };
                lg->constraint = uilayout::kGridFixedColumnCount;
                lg->constraintCount = 3;
            }
            EntityID cells[7];
            for (EntityID& cell : cells) {
                cell = box(w, "cell", g, 0.0f, 0.0f, 1.0f, 1.0f); // 大きさは cellSize に置き換わる
            }
            w.ApplyStructuralChanges();
            check(rectNear(rr(w, cells[4]), 22.0f, 13.0f, 20.0f, 10.0f)
                      && rectNear(rr(w, cells[6]), 0.0f, 26.0f, 20.0f, 10.0f),
                  "layout: a fixed-column grid fills rows from the upper left");
            w.GetComponent<UILayoutGroupComponent>(g)->startCorner = 3;
            check(rectNear(rr(w, cells[0]), 44.0f, 26.0f, 20.0f, 10.0f)
                      && rectNear(rr(w, cells[6]), 44.0f, 0.0f, 20.0f, 10.0f),
                  "layout: a lower-right start corner mirrors both axes");
            w.GetComponent<UILayoutGroupComponent>(g)->startCorner = 0;
            {
                auto* f = w.AddComponent<UIContentSizeFitterComponent>(g);
                f->horizontalFit = uilayout::kFitPreferred;
                f->verticalFit = uilayout::kFitPreferred;
            }
            w.ApplyStructuralChanges();
            check(rectNear(rr(w, g), 0.0f, 0.0f, 64.0f, 36.0f) && rectNear(rr(w, cells[5]), 44.0f, 13.0f, 20.0f, 10.0f),
                  "layout: ContentSizeFitter shrinks a fixed-column grid to 3 columns x 3 rows");
            // 幅に合わせる: 50 には floor((50 + 2 + 0.001) / 22) = 2 列 → 4 行
            w.GetComponent<UIContentSizeFitterComponent>(g)->horizontalFit = uilayout::kFitUnconstrained;
            w.GetComponent<UIContentSizeFitterComponent>(g)->verticalFit = uilayout::kFitUnconstrained;
            w.GetComponent<UILayoutGroupComponent>(g)->constraint = uilayout::kGridFlexible;
            w.GetComponent<RectTransformComponent>(g)->sizeDelta.x = 50.0f;
            uilayout::LayoutScratch s;
            check(rectNear(rr(w, cells[4]), 0.0f, 26.0f, 20.0f, 10.0f)
                      && uilayout::LayoutInputHeight(w, g, 50.0f, s).preferred == 49.0f
                      && uilayout::LayoutInputWidth(w, g, s).preferred == 64.0f,
                  "layout: a flexible grid derives its column count from its width");
        }
        {
            World w;
            const EntityID g = box(w, "grid", kNullEntity, 0.0f, 0.0f, 100.0f, 100.0f);
            {
                auto* lg = w.AddComponent<UILayoutGroupComponent>(g);
                lg->kind = uilayout::kLayoutGrid;
                lg->cellSize = { 20.0f, 10.0f };
                lg->spacing = { 2.0f, 3.0f };
                lg->constraint = uilayout::kGridFixedRowCount;
                lg->constraintCount = 3;
            }
            EntityID cells[4];
            for (EntityID& cell : cells) {
                cell = box(w, "cell", g, 0.0f, 0.0f, 1.0f, 1.0f);
            }
            w.ApplyStructuralChanges();
            check(rectNear(rr(w, cells[0]), 0.0f, 0.0f, 20.0f, 10.0f)
                      && rectNear(rr(w, cells[1]), 22.0f, 0.0f, 20.0f, 10.0f)
                      && rectNear(rr(w, cells[2]), 0.0f, 13.0f, 20.0f, 10.0f)
                      && rectNear(rr(w, cells[3]), 0.0f, 26.0f, 20.0f, 10.0f),
                  "layout: a fixed-row grid keeps every row used (Unity case 1345471)");
        }

        // (6) テキスト + ContentSizeFitter / LayoutElement の優先度
        {
            World w;
            const EntityID t = text(w, "hello", kNullEntity, "HELLO", 2.0f, 0);
            {
                auto* f = w.AddComponent<UIContentSizeFitterComponent>(t);
                f->horizontalFit = uilayout::kFitPreferred;
                f->verticalFit = uilayout::kFitPreferred;
            }
            w.ApplyStructuralChanges();
            {
                auto* rt = w.GetComponent<RectTransformComponent>(t);
                rt->anchorMin = { 0.5f, 0.5f };
                rt->anchorMax = { 0.5f, 0.5f };
                rt->pivot = { 0.5f, 0.5f };
                rt->sizeDelta = { 10.0f, 10.0f };
            }
            check(rectNear(rr(w, t), 460.0f, 390.0f, 80.0f, 20.0f)
                      && uilayout::LayoutDrivenBits(w, t)
                          == (uilayout::kDrivenWidth | uilayout::kDrivenHeight | uilayout::kDrivenByFitter),
                  "layout: ContentSizeFitter sizes text from the font metrics around its pivot");
            w.GetComponent<UIContentSizeFitterComponent>(t)->horizontalFit = uilayout::kFitMinSize;
            check(rectNear(rr(w, t), 500.0f, 390.0f, 0.0f, 20.0f),
                  "layout: MinSize fit uses the text's min width (0, as Unity's Text)");
            w.GetComponent<UIContentSizeFitterComponent>(t)->horizontalFit = uilayout::kFitPreferred;
            w.AddComponent<UILayoutElementComponent>(t)->preferredWidth = 50.0f;
            w.ApplyStructuralChanges();
            uilayout::LayoutScratch s1;
            const float byPriority1 = uilayout::LayoutInputWidth(w, t, s1).preferred;
            w.GetComponent<UILayoutElementComponent>(t)->layoutPriority = 0;
            uilayout::LayoutScratch s2;
            const float samePriorityText = uilayout::LayoutInputWidth(w, t, s2).preferred;
            w.GetComponent<UILayoutElementComponent>(t)->preferredWidth = 90.0f;
            uilayout::LayoutScratch s3;
            const float samePriorityElement = uilayout::LayoutInputWidth(w, t, s3).preferred;
            check(byPriority1 == 50.0f && samePriorityText == 80.0f && samePriorityElement == 90.0f,
                  "layout: a higher layoutPriority wins; equal priorities take the larger value");
            // 折り返し: 幅 40 に 8 送りの 10 文字 = 5 文字ずつ 2 行
            const EntityID wrapped = text(w, "wrap", kNullEntity, "ABCDEFGHIJ", 1.0f, 1);
            w.AddComponent<UIContentSizeFitterComponent>(wrapped)->verticalFit = uilayout::kFitPreferred;
            w.ApplyStructuralChanges();
            w.GetComponent<RectTransformComponent>(wrapped)->sizeDelta = { 40.0f, 5.0f };
            check(rectNear(rr(w, wrapped), 0.0f, 0.0f, 40.0f, 20.0f),
                  "layout: a vertical fit measures wrapped text at the element's own width");
        }

        // (7)〜(10) 垂直 Group + Fitter + 折り返すテキスト = 幅 → 高さの 2 パス。入れ子 / メモ / ヒット
        {
            World w;
            const EntityID col = box(w, "column", kNullEntity, 0.0f, 0.0f, 100.0f, 10.0f);
            {
                auto* lg = w.AddComponent<UILayoutGroupComponent>(col);
                lg->kind = uilayout::kLayoutVertical;
                lg->padding = { 5.0f, 5.0f, 5.0f, 5.0f };
                lg->spacing = { 0.0f, 2.0f };
                lg->controlChildWidth = 1;
                lg->controlChildHeight = 1;
                lg->forceExpandHeight = 0;
            }
            w.AddComponent<UIContentSizeFitterComponent>(col)->verticalFit = uilayout::kFitPreferred;
            const EntityID t1 = text(w, "long", col, "ABCDEFGHIJKL", 1.0f, 1);
            const EntityID t2 = text(w, "short", col, "ABC", 1.0f, 1);
            w.ApplyStructuralChanges();
            // 内幅 90 = 11 文字/行 → 12 文字は 2 行 (20)。高さ = 5 + 20 + 2 + 10 + 5
            check(rectNear(rr(w, col), 0.0f, 0.0f, 100.0f, 42.0f) && rectNear(rr(w, t1), 5.0f, 5.0f, 90.0f, 20.0f)
                      && rectNear(rr(w, t2), 5.0f, 27.0f, 90.0f, 10.0f),
                  "layout: a fitted vertical group measures wrapped text at the width it hands out");
            // 内幅 34 = 4 文字/行 → 3 行 (30)。Group は 52 に伸びる
            w.GetComponent<RectTransformComponent>(col)->sizeDelta.x = 44.0f;
            check(rectNear(rr(w, col), 0.0f, 0.0f, 44.0f, 52.0f) && rectNear(rr(w, t1), 5.0f, 5.0f, 34.0f, 30.0f)
                      && rectNear(rr(w, t2), 5.0f, 37.0f, 34.0f, 10.0f),
                  "layout: narrowing the group re-wraps its text and makes it taller");

            // (8) 入れ子: 水平 Group (高さを制御) の子になった垂直 Group は、自分の幅で測った集計を希望にする
            w.GetComponent<RectTransformComponent>(col)->sizeDelta.x = 100.0f;
            w.GetComponent<UIContentSizeFitterComponent>(col)->verticalFit = uilayout::kFitUnconstrained;
            const EntityID row = box(w, "row", kNullEntity, 0.0f, 200.0f, 300.0f, 100.0f);
            {
                auto* lg = w.AddComponent<UILayoutGroupComponent>(row);
                lg->controlChildHeight = 1;
                lg->forceExpandWidth = 0;
                lg->forceExpandHeight = 0;
            }
            w.SetParent(col, row);
            w.ApplyStructuralChanges();
            check(rectNear(rr(w, col), 0.0f, 200.0f, 100.0f, 42.0f) && rectNear(rr(w, t2), 5.0f, 227.0f, 90.0f, 10.0f)
                      && uilayout::LayoutDrivenBits(w, col) == (uilayout::kDrivenByGroup | uilayout::kDrivenHeight),
                  "layout: a nested group reports its height for the width its parent leaves it");

            // (9) メモは結果を変えない: 共有メモで子から先に 2 周解いても、単発の呼び出しとビット一致
            {
                const EntityID order[] = { t2, t1, col, row };
                uilayout::LayoutScratch shared;
                bool same = true;
                for (int pass = 0; pass < 2; ++pass) {
                    for (const EntityID e : order) {
                        const uilayout::UIRect rs = uilayout::ResolveRect(w, e, W, H, nullptr, &shared);
                        const uilayout::UIRect r1 = uilayout::ResolveRect(w, e, W, H);
                        const uilayout::UIRect cs = uilayout::ResolveClipRect(w, e, W, H, nullptr, &shared);
                        const uilayout::UIRect c1 = uilayout::ResolveClipRect(w, e, W, H);
                        same = same && std::memcmp(&rs, &r1, sizeof(rs)) == 0
                            && std::memcmp(&cs, &c1, sizeof(cs)) == 0;
                    }
                }
                check(same, "layout: the scratch memo never changes a resolved rect");
            }

            // (10) ヒットテストは並べた後の矩形で当たる。t2 の RectTransform (既定 160x40) のままなら
            //      (150, 210) にも当たるが、並べた矩形 (5..95, 227..237) の外なので row に落ちる
            w.GetComponent<UIElementComponent>(t2)->order = 1;
            check(uiinteract::HitTest(w, W, H, 50.0f, 232.0f) == t2
                      && uiinteract::HitTest(w, W, H, 150.0f, 210.0f) == row,
                  "layout: hit testing uses the arranged rect, not the authored one");
        }

        // (11) シーンの保存 / 読み込みで 3 つとも残る。UI 専用オブジェクトのまま (UiAux)
        {
            Scene scene;
            GameObject go = scene.CreateGameObject("LayoutRoundTrip");
            go.AddComponent<RectTransformComponent>();
            {
                auto* g = go.AddComponent<UILayoutGroupComponent>();
                g->kind = uilayout::kLayoutGrid;
                g->padding = { 1.0f, 2.0f, 3.0f, 4.0f };
                g->spacing = { 5.0f, 6.0f };
                g->childAlignment = 7;
                g->controlChildWidth = 1;
                g->forceExpandHeight = 0;
                g->reverseArrangement = 1;
                g->cellSize = { 7.0f, 8.0f };
                g->startCorner = 3;
                g->startAxis = 1;
                g->constraint = uilayout::kGridFixedRowCount;
                g->constraintCount = 5;
            }
            {
                auto* le = go.AddComponent<UILayoutElementComponent>();
                le->ignoreLayout = 1;
                le->minWidth = 1.5f;
                le->preferredHeight = 2.5f;
                le->flexibleWidth = 3.5f;
                le->layoutPriority = 4;
            }
            {
                auto* f = go.AddComponent<UIContentSizeFitterComponent>();
                f->horizontalFit = uilayout::kFitMinSize;
                f->verticalFit = uilayout::kFitPreferred;
            }
            scene.GetWorld().ApplyStructuralChanges();
            const nlohmann::json saved = SceneSerializer::SaveToJson(scene);
            Scene again;
            const bool loaded = SceneSerializer::LoadFromJson(again, saved);
            World& w1 = scene.GetWorld();
            World& w2 = again.GetWorld();
            const EntityID e1 = scene.Find("LayoutRoundTrip").Id();
            GameObject found = again.Find("LayoutRoundTrip");
            bool same = loaded && found;
            if (same) {
                const EntityID e2 = found.Id();
                const auto* g1 = w1.GetComponent<UILayoutGroupComponent>(e1);
                const auto* g2 = w2.GetComponent<UILayoutGroupComponent>(e2);
                const auto* l1 = w1.GetComponent<UILayoutElementComponent>(e1);
                const auto* l2 = w2.GetComponent<UILayoutElementComponent>(e2);
                const auto* f1 = w1.GetComponent<UIContentSizeFitterComponent>(e1);
                const auto* f2 = w2.GetComponent<UIContentSizeFitterComponent>(e2);
                same = g1 && g2 && l1 && l2 && f1 && f2
                    && std::memcmp(g1, g2, sizeof(UILayoutGroupComponent)) == 0
                    && std::memcmp(l1, l2, sizeof(UILayoutElementComponent)) == 0
                    && std::memcmp(f1, f2, sizeof(UIContentSizeFitterComponent)) == 0
                    && uilayout::IsUiOnlyEntity(w2, e2);
            }
            check(same, "layout: LayoutGroup / LayoutElement / ContentSizeFitter survive save/load and stay UI-only");
        }

        uitext::SetActiveFontMetrics(savedMetrics);
    }

    if (failCount == 0) {
        MYE_LOG_INFO("==== UI self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== UI self test: %d FAILURE(S) ====", failCount);
    return false;
}

} // namespace mye
