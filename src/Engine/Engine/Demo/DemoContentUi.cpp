// ゲーム内 UI のショーケースと、その決定論の入力台本 (--ui-demo / --ui-demo-input)。
// DemoContent.cpp から領域ごとの翻訳単位へ分けたもの (M93g)。関数の契約は DemoContent.h、
// ショーケースを跨いで使うヘルパーは DemoContentInternal.h。
#include "Engine/Engine/Demo/DemoContent.h"
#include "Engine/Engine/Demo/DemoContentInternal.h"

#include <cstdio>

#include "Engine/Core/Ecs/Components.h"
#include "Engine/Engine/Loop/EngineLoop.h"
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/UI/UILayout.h" // M75a: 旧 anchor/x/y/w/h → RectTransform
#include "Engine/Engine/UI/UILayoutGroup.h" // M75e: --ui-demo の自動レイアウト
#include "Engine/Engine/UI/UIWidgetFactory.h" // M75f: --ui-demo のウィジェット (Create > UI と同じ構成)
#include "Engine/Engine/UI/UIWidgets.h"       // M75f: kSlider*
#include "Engine/Platform/Input.h"            // M75f: UiDemoScriptInput
#include "Engine/Engine/Scene/Scene.h"

namespace mye {

// ---- M75c: ゲーム内 UI のショーケース (--ui-demo) ----
// golden 25 枚目 (ui_widgets) の被写体。Canvas Scaler の 3 モードと Canvas の sortOrder (M75c)、
// 自動レイアウト (M75e)、ウィジェット (M75f) を載せる。積み増しはこのシーンの**末尾に**。
// ★4 隅の箱は**どれも自分のキャンバス単位で 300x150**。基準 1024x768 (4:3) を 16:9 の画面で
//   解くので Expand / Shrink / Match で実寸が変わる = 3 モードの差がそのまま絵に出る。
//   基準を 16:9 にすると 3 モードが一致して何も写らない (UILayout.cpp の sx == sy の近道)
void BuildUiShowcaseScene(EngineContext& ctx)
{
    Scene& s = *ctx.scene;
    s.SetName("ui_showcase");

    GameObject cam = s.CreateGameObject("Main Camera");
    cam.SetLocalPosition(0.0f, 2.0f, -8.0f);
    cam.AddComponent<CameraComponent>()->isPrimary = 1;

    // 全面ストレッチの Canvas。RectTransform は CreateMenu と同じ値 (Canvas の矩形はこれに依らない)
    auto canvas = [&](const char* name, int refW, int refH, int mode, float match, int sortOrder) {
        GameObject go = s.CreateGameObject(name);
        auto* rt = go.AddComponent<RectTransformComponent>();
        rt->anchorMin = { 0.0f, 0.0f };
        rt->anchorMax = { 1.0f, 1.0f };
        rt->pivot = { 0.5f, 0.5f };
        rt->sizeDelta = { 0.0f, 0.0f };
        auto* cv = go.AddComponent<UICanvasComponent>();
        cv->referenceW = refW;
        cv->referenceH = refH;
        cv->scaleMode = mode;
        cv->match = match;
        cv->sortOrder = sortOrder;
        return go;
    };
    // 単色パネル。anchor と pivot は同じ点 (角に寄せる置き方)。**RectTransform を先に**足す —
    // 後から足すと UIElement のポインタがアーキタイプ移動で無効になる
    auto panel = [&](const char* name, GameObject* parent, float ax, float ay, float x, float y,
                     float w, float h, DirectX::XMFLOAT4 color, int order) {
        GameObject go = s.CreateGameObject(name);
        auto* rt = go.AddComponent<RectTransformComponent>();
        rt->anchorMin = { ax, ay };
        rt->anchorMax = { ax, ay };
        rt->pivot = { ax, ay };
        rt->anchoredPosition = { x, y };
        rt->sizeDelta = { w, h };
        auto* el = go.AddComponent<UIElementComponent>();
        el->kind = 0;
        el->color = color;
        el->order = order;
        if (parent != nullptr) {
            go.SetParent(*parent);
        }
        return go;
    };
    // 親の全面に中央揃えのテキスト
    auto label = [&](const char* name, GameObject& parent, const char* text, float fontScale,
                     int order) {
        GameObject go = s.CreateGameObject(name);
        auto* rt = go.AddComponent<RectTransformComponent>();
        rt->anchorMin = { 0.0f, 0.0f };
        rt->anchorMax = { 1.0f, 1.0f };
        rt->pivot = { 0.5f, 0.5f };
        rt->sizeDelta = { 0.0f, 0.0f };
        auto* el = go.AddComponent<UIElementComponent>();
        el->kind = 1;
        el->align = 4;
        el->fontScale = fontScale;
        el->order = order;
        std::snprintf(el->text, sizeof(el->text), "%s", text);
        go.SetParent(parent);
        return go;
    };

    // ---- 既定キャンバス (Canvas 無し = project_settings の基準解像度 + Expand) ----
    {
        GameObject bg = s.CreateGameObject("Background");
        auto* rt = bg.AddComponent<RectTransformComponent>();
        rt->anchorMin = { 0.0f, 0.0f };
        rt->anchorMax = { 1.0f, 1.0f };
        rt->sizeDelta = { 0.0f, 0.0f };
        auto* el = bg.AddComponent<UIElementComponent>();
        el->color = { 0.08f, 0.09f, 0.12f, 1.0f };
        el->order = -1000;
    }
    // 中央やや上。上端の 2 箱 (Shrink は 16:9 で最も大きく出る) と下の sortOrder の 2 枚に
    // 重ならない高さに置く (anchor == pivot == 0.5 = 中央基準)
    GameObject titleBox = panel("TitleBox", nullptr, 0.5f, 0.5f, 0.0f, -100.0f, 1200.0f, 220.0f,
                                { 0.14f, 0.15f, 0.20f, 1.0f }, -500);
    label("Title", titleBox, "M75c  CANVAS SCALER", 5.0f, -499);
    GameObject defBox = panel("DefaultBox", nullptr, 1.0f, 1.0f, -32.0f, -32.0f, 300.0f, 150.0f,
                              { 0.45f, 0.25f, 0.60f, 1.0f }, 0);
    label("DefaultLabel", defBox, "DEFAULT 1920x1080", 2.0f, 1);

    // ---- Scaler 3 モード (基準 1024x768)。箱は同じ 300x150 ----
    GameObject cExpand = canvas("CanvasExpand", 1024, 768, 0, 0.0f, 1);
    GameObject expandBox = panel("ExpandBox", &cExpand, 0.0f, 0.0f, 16.0f, 16.0f, 300.0f, 150.0f,
                                 { 0.20f, 0.35f, 0.70f, 1.0f }, 0);
    label("ExpandLabel", expandBox, "EXPAND 1024x768", 2.0f, 1);

    GameObject cShrink = canvas("CanvasShrink", 1024, 768, 1, 0.0f, 1);
    GameObject shrinkBox = panel("ShrinkBox", &cShrink, 1.0f, 0.0f, -16.0f, 16.0f, 300.0f, 150.0f,
                                 { 0.20f, 0.55f, 0.30f, 1.0f }, 0);
    label("ShrinkLabel", shrinkBox, "SHRINK 1024x768", 2.0f, 1);

    GameObject cMatch = canvas("CanvasMatch", 1024, 768, 2, 0.5f, 1);
    GameObject matchBox = panel("MatchBox", &cMatch, 0.0f, 1.0f, 16.0f, -16.0f, 300.0f, 150.0f,
                                { 0.70f, 0.45f, 0.15f, 1.0f }, 0);
    label("MatchLabel", matchBox, "MATCH 0.5 1024x768", 2.0f, 1);

    // ---- sortOrder: Canvas の sortOrder は要素の order より先に効く ----
    // 既定キャンバスの order 100 の赤より、sortOrder 5 の Canvas にある order -100 の青が手前に出る
    GameObject lowSort = panel("DefaultOrder100", nullptr, 0.5f, 1.0f, -120.0f, -220.0f, 480.0f,
                               200.0f, { 0.75f, 0.20f, 0.20f, 1.0f }, 100);
    label("DefaultOrder100Label", lowSort, "DEFAULT ORDER 100", 2.5f, 101);
    GameObject cOverlay = canvas("CanvasOverlay", 0, 0, 0, 0.0f, 5);
    GameObject highSort = panel("OverlayOrderMinus100", &cOverlay, 0.5f, 1.0f, 120.0f, -120.0f,
                                480.0f, 200.0f, { 0.20f, 0.60f, 0.70f, 1.0f }, -100);
    label("OverlayLabel", highSort, "SORT 5 ORDER -100", 2.5f, -99);

    // ---- M75e: 自動レイアウト (Layout Group / LayoutElement / ContentSizeFitter) ----
    // ★M75e 以降の積み増しは**この関数の末尾へ**。前へ挿すと既存要素の entity.index (描画順の同値キー) が動く。
    // 置き場所は既存の箱の隙間 (上辺中央 / 左の中段 / 右の中段) で、どれも既定キャンバスの要素。
    // **RectTransform を先に**足す規則は他と同じ (panel が守る)。1 つのエンティティへ続けて AddComponent
    // するとアーキタイプが移って前のポインタが無効になるので、書いてから次を足す

    // (1) 水平 Group: 幅を制御 + 高さは広げる。MIN 120 (LayoutElement の min/preferred) / FLEX 1 / FLEX 2
    //     (余りを 1:2 で配る) / 入れ子の垂直 Group (Group 自身の集計 40 より LayoutElement の 120 が勝つ)。
    //     右上の赤い角は ignoreLayout = 自分の RectTransform で行の角に乗る
    {
        GameObject row = panel("LayoutRow", nullptr, 0.5f, 0.0f, 0.0f, 24.0f, 720.0f, 120.0f,
                               { 0.12f, 0.13f, 0.17f, 1.0f }, 10);
        {
            auto* g = row.AddComponent<UILayoutGroupComponent>();
            g->kind = uilayout::kLayoutHorizontal;
            g->padding = { 12.0f, 12.0f, 12.0f, 12.0f };
            g->spacing = { 8.0f, 0.0f };
            g->controlChildWidth = 1;
            g->controlChildHeight = 1;
            g->forceExpandWidth = 0;
            g->forceExpandHeight = 1;
        }
        GameObject minCell = panel("RowMin120", &row, 0.0f, 0.0f, 0.0f, 0.0f, 40.0f, 40.0f,
                                   { 0.30f, 0.32f, 0.40f, 1.0f }, 11);
        {
            auto* le = minCell.AddComponent<UILayoutElementComponent>();
            le->minWidth = 120.0f;
            le->preferredWidth = 120.0f;
        }
        label("RowMin120Label", minCell, "MIN 120", 2.0f, 12);
        GameObject flex1 = panel("RowFlex1", &row, 0.0f, 0.0f, 0.0f, 0.0f, 40.0f, 40.0f,
                                 { 0.20f, 0.45f, 0.55f, 1.0f }, 11);
        flex1.AddComponent<UILayoutElementComponent>()->flexibleWidth = 1.0f;
        label("RowFlex1Label", flex1, "FLEX 1", 2.0f, 12);
        GameObject flex2 = panel("RowFlex2", &row, 0.0f, 0.0f, 0.0f, 0.0f, 40.0f, 40.0f,
                                 { 0.25f, 0.55f, 0.35f, 1.0f }, 11);
        flex2.AddComponent<UILayoutElementComponent>()->flexibleWidth = 2.0f;
        label("RowFlex2Label", flex2, "FLEX 2", 2.0f, 12);
        GameObject nested = panel("RowNested", &row, 0.0f, 0.0f, 0.0f, 0.0f, 40.0f, 40.0f,
                                  { 0.18f, 0.18f, 0.24f, 1.0f }, 11);
        {
            auto* g = nested.AddComponent<UILayoutGroupComponent>();
            g->kind = uilayout::kLayoutVertical;
            g->spacing = { 0.0f, 6.0f };
            g->controlChildWidth = 1;
            g->controlChildHeight = 1;
        }
        {
            // ★flexible も 0 で押さえる。子の棒は Force Expand (flexible 1) なので、Group 自身の集計は
            //   flexible 1 を名乗る (Unity と同じ = 入れ子の Group は伸びる側に回る)。優先度 1 の 0 で上書き
            auto* le = nested.AddComponent<UILayoutElementComponent>();
            le->preferredWidth = 120.0f;
            le->flexibleWidth = 0.0f;
        }
        const DirectX::XMFLOAT4 barColors[3] = { { 0.85f, 0.55f, 0.20f, 1.0f },
                                                 { 0.75f, 0.75f, 0.25f, 1.0f },
                                                 { 0.55f, 0.80f, 0.35f, 1.0f } };
        const char* const barNames[3] = { "RowNestedBar0", "RowNestedBar1", "RowNestedBar2" };
        for (int i = 0; i < 3; ++i) {
            panel(barNames[i], &nested, 0.0f, 0.0f, 0.0f, 0.0f, 40.0f, 40.0f, barColors[i], 12);
        }
        GameObject badge = panel("RowIgnoreLayout", &row, 1.0f, 0.0f, 0.0f, 0.0f, 28.0f, 28.0f,
                                 { 0.85f, 0.20f, 0.25f, 1.0f }, 13);
        badge.GetComponent<RectTransformComponent>()->pivot = { 0.5f, 0.5f }; // 角の上に中心を置く
        badge.AddComponent<UILayoutElementComponent>()->ignoreLayout = 1;
    }

    // (2) 垂直 Group + ContentSizeFitter (縦 = 推奨)。幅は Group が配り、折り返すテキストの高さは
    //     **配られた幅で**測る (Unity の 2 パス)。パネルの高さは中身の合計に縮む (作者の値は 10)
    {
        GameObject col = panel("LayoutColumn", nullptr, 0.0f, 0.0f, 24.0f, 250.0f, 320.0f, 10.0f,
                               { 0.12f, 0.13f, 0.17f, 1.0f }, 10);
        {
            auto* g = col.AddComponent<UILayoutGroupComponent>();
            g->kind = uilayout::kLayoutVertical;
            g->padding = { 12.0f, 12.0f, 12.0f, 12.0f };
            g->spacing = { 0.0f, 8.0f };
            g->controlChildWidth = 1;
            g->controlChildHeight = 1;
            g->forceExpandWidth = 1;
            g->forceExpandHeight = 0;
        }
        col.AddComponent<UIContentSizeFitterComponent>()->verticalFit = uilayout::kFitPreferred;
        GameObject bar = panel("ColumnBar", &col, 0.0f, 0.0f, 0.0f, 0.0f, 40.0f, 40.0f,
                               { 0.45f, 0.25f, 0.60f, 1.0f }, 11);
        bar.AddComponent<UILayoutElementComponent>()->preferredHeight = 24.0f;
        const auto columnText = [&](const char* name, const char* text) {
            GameObject go = s.CreateGameObject(name);
            go.AddComponent<RectTransformComponent>();
            auto* el = go.AddComponent<UIElementComponent>();
            el->kind = 1;
            el->wrap = 1;
            el->fontScale = 2.0f;
            el->order = 11;
            std::snprintf(el->text, sizeof(el->text), "%s", text);
            go.SetParent(col);
        };
        columnText("ColumnTitle", "VERTICAL GROUP");
        columnText("ColumnBody", "CONTENT SIZE FITTER GROWS THIS PANEL TO FIT ITS WRAPPED TEXT");
        columnText("ColumnTail", "WIDTH FIRST, THEN HEIGHT");
    }

    // (3) Grid: 列数 4 固定・右上から埋める・Fitter で縦横とも中身に合わせる。10 個なので最後の行は右寄せの 2 個
    {
        GameObject grid = panel("LayoutGrid", nullptr, 1.0f, 0.0f, -24.0f, 340.0f, 10.0f, 10.0f,
                                { 0.12f, 0.13f, 0.17f, 1.0f }, 10);
        {
            auto* g = grid.AddComponent<UILayoutGroupComponent>();
            g->kind = uilayout::kLayoutGrid;
            g->padding = { 12.0f, 12.0f, 12.0f, 12.0f };
            g->spacing = { 8.0f, 8.0f };
            g->cellSize = { 56.0f, 56.0f };
            g->startCorner = 1; // 右上
            g->constraint = uilayout::kGridFixedColumnCount;
            g->constraintCount = 4;
        }
        {
            auto* f = grid.AddComponent<UIContentSizeFitterComponent>();
            f->horizontalFit = uilayout::kFitPreferred;
            f->verticalFit = uilayout::kFitPreferred;
        }
        const char* const cellNames[10] = { "GridCell0", "GridCell1", "GridCell2", "GridCell3",
                                            "GridCell4", "GridCell5", "GridCell6", "GridCell7",
                                            "GridCell8", "GridCell9" };
        const char* const digits[10] = { "0", "1", "2", "3", "4", "5", "6", "7", "8", "9" };
        for (int i = 0; i < 10; ++i) {
            const float t = static_cast<float>(i) / 9.0f;
            GameObject cell = panel(cellNames[i], &grid, 0.0f, 0.0f, 0.0f, 0.0f, 10.0f, 10.0f,
                                    { 0.20f + 0.55f * t, 0.35f, 0.75f - 0.45f * t, 1.0f }, 11);
            label("GridCellLabel", cell, digits[i], 3.0f, 12);
        }
    }

    // ---- M75f: ウィジェット (Selectable / Toggle / ToggleGroup / Slider) ----
    // ★座標は replay 8 ペア目の入力台本 (UiDemoScriptInput、このファイルの末尾) が押す点と対。動かすなら台本も直す。
    // 左の中段 (垂直 Group の下) に Toggle、右の中段 (Grid の下) に Slider。どちらの箱も既定キャンバスの左上基準で、
    // 中身は箱の左上からの位置。子の構成は Create > UI と同じ uiwidgets::CreateToggle / CreateSlider。
    // ★箱の order は -400 (Title の -500 より手前、ウィジェットの order 0 より奥)。ウィジェットの UIElement は
    //   order 0 なので、箱を 10 にすると箱の後ろに隠れる
    const auto placeTopLeft = [](GameObject& go, float x, float y, float w, float h) {
        auto* rt = go.GetComponent<RectTransformComponent>();
        rt->anchorMin = { 0.0f, 0.0f };
        rt->anchorMax = { 0.0f, 0.0f };
        rt->pivot = { 0.0f, 0.0f };
        rt->anchoredPosition = { x, y };
        rt->sizeDelta = { w, h };
    };
    // 箱の上辺の見出し (左寄せ)
    const auto caption = [&](const char* name, GameObject& parent, const char* text) {
        GameObject go = s.CreateGameObject(name);
        auto* rt = go.AddComponent<RectTransformComponent>();
        rt->anchoredPosition = { 12.0f, 8.0f };
        rt->sizeDelta = { 300.0f, 28.0f };
        auto* el = go.AddComponent<UIElementComponent>();
        el->kind = 1;
        el->align = 3;
        el->fontScale = 2.0f;
        std::snprintf(el->text, sizeof(el->text), "%s", text);
        go.SetParent(parent);
    };

    // (1) Toggle: 群 (allowSwitchOff なし) の A / B、単独、操作不可。箱自身が UIToggleGroup を持つ。
    //     一番下は Selectable を付けた旧来のボタン (色の遷移がハードコードのハイライトに代わる)
    {
        GameObject box = panel("WidgetToggles", nullptr, 0.0f, 0.0f, 24.0f, 470.0f, 330.0f, 310.0f,
                               { 0.12f, 0.13f, 0.17f, 1.0f }, -400);
        box.AddComponent<UIToggleGroupComponent>();
        caption("WidgetTogglesTitle", box, "TOGGLE");
        const char* const names[4] = { "ToggleGroupA", "ToggleGroupB", "ToggleSolo", "ToggleDisabled" };
        const char* const labels[4] = { "GROUP A", "GROUP B", "SOLO", "DISABLED" };
        for (int i = 0; i < 4; ++i) {
            GameObject t = uiwidgets::CreateToggle(s, names[i], labels[i], 2.0f);
            placeTopLeft(t, 12.0f, 40.0f + 48.0f * static_cast<float>(i), 300.0f, 40.0f);
            t.SetParent(box);
            {
                auto* toggle = t.GetComponent<UIToggleComponent>();
                toggle->isOn = (i == 0 || i == 3) ? 1 : 0;
                if (i < 2) {
                    toggle->group = box.Id();
                }
            }
            if (i == 3) {
                t.GetComponent<UISelectableComponent>()->interactable = 0;
            }
        }
        GameObject button = panel("SelectableButton", &box, 0.0f, 0.0f, 12.0f, 244.0f, 300.0f, 52.0f,
                                  { 0.30f, 0.36f, 0.50f, 1.0f }, 0);
        {
            auto* el = button.GetComponent<UIElementComponent>();
            el->kind = 2;
            el->fontScale = 2.0f;
            std::snprintf(el->text, sizeof(el->text), "%s", "SELECTABLE");
        }
        {
            auto* sel = button.AddComponent<UISelectableComponent>();
            sel->normalColor = { 0.8f, 0.8f, 0.8f, 1.0f };
            sel->highlightedColor = { 1.0f, 1.0f, 1.0f, 1.0f };
            sel->pressedColor = { 0.5f, 0.5f, 0.5f, 1.0f };
            sel->selectedColor = { 0.9f, 1.0f, 0.9f, 1.0f };
            sel->colorMultiplier = 1.25f;
        }
    }

    // (2) Slider: 左→右 0..1 / 右→左の整数 0..10 / 操作不可 / 下→上 0..100 (縦)
    {
        GameObject box = panel("WidgetSliders", nullptr, 0.0f, 0.0f, 1340.0f, 560.0f, 560.0f, 320.0f,
                               { 0.12f, 0.13f, 0.17f, 1.0f }, -400);
        caption("WidgetSlidersTitle", box, "SLIDER");
        struct SliderSpec {
            const char* name;
            int direction;
            float x, y, w, h;
            float minValue, maxValue, value;
            int wholeNumbers;
            int interactable;
        };
        const SliderSpec specs[4] = {
            { "SliderLeftToRight", uiwidgets::kSliderLeftToRight, 16.0f, 44.0f, 400.0f, 40.0f, 0.0f, 1.0f, 0.25f, 0, 1 },
            { "SliderWholeRightToLeft", uiwidgets::kSliderRightToLeft, 16.0f, 100.0f, 400.0f, 40.0f, 0.0f, 10.0f, 7.0f, 1, 1 },
            { "SliderDisabled", uiwidgets::kSliderLeftToRight, 16.0f, 156.0f, 400.0f, 40.0f, 0.0f, 1.0f, 0.6f, 0, 0 },
            { "SliderBottomToTop", uiwidgets::kSliderBottomToTop, 460.0f, 44.0f, 40.0f, 240.0f, 0.0f, 100.0f, 30.0f, 0, 1 },
        };
        for (const SliderSpec& sp : specs) {
            GameObject sl = uiwidgets::CreateSlider(s, sp.name, sp.direction);
            placeTopLeft(sl, sp.x, sp.y, sp.w, sp.h);
            sl.SetParent(box);
            {
                auto* slider = sl.GetComponent<UISliderComponent>();
                slider->minValue = sp.minValue;
                slider->maxValue = sp.maxValue;
                slider->value = sp.value;
                slider->wholeNumbers = sp.wholeNumbers;
            }
            sl.GetComponent<UISelectableComponent>()->interactable = sp.interactable;
        }
    }

    // ---- M75g: スクロールとドロップダウン ----
    // 置き場所は上辺中央の行 (LayoutRow) と Title の間の帯 (y 156..326)。子の構成は Create > UI と同じ
    // uiwidgets::CreateScrollView / CreateDropdown。★座標は入力台本 (UiDemoScriptInput) と対
    //
    // (1) Scroll View 360x170: 中身は 10 行の縦の Group + Fitter (高さ 412) で、幅は見える範囲より 200 広い =
    //     縦横とも動く。Elastic + 慣性 (既定)
    {
        GameObject sv = uiwidgets::CreateScrollView(s, "WidgetScrollView");
        placeTopLeft(sv, 470.0f, 156.0f, 360.0f, 170.0f);
        GameObject content(&s.GetWorld(), sv.GetComponent<UIScrollRectComponent>()->content);
        {
            content.GetComponent<RectTransformComponent>()->sizeDelta = { 200.0f, 0.0f };
            {
                auto* g = content.AddComponent<UILayoutGroupComponent>();
                g->kind = uilayout::kLayoutVertical;
                g->padding = { 8.0f, 8.0f, 8.0f, 8.0f };
                g->spacing = { 0.0f, 4.0f };
                g->controlChildWidth = 1;
                g->controlChildHeight = 1;
                g->forceExpandWidth = 1;
                g->forceExpandHeight = 0;
            }
            content.AddComponent<UIContentSizeFitterComponent>()->verticalFit = uilayout::kFitPreferred;
            const char* const rowNames[10] = { "ScrollRow0", "ScrollRow1", "ScrollRow2", "ScrollRow3",
                                               "ScrollRow4", "ScrollRow5", "ScrollRow6", "ScrollRow7",
                                               "ScrollRow8", "ScrollRow9" };
            const char* const rowTexts[10] = { "ROW 0", "ROW 1", "ROW 2", "ROW 3", "ROW 4",
                                               "ROW 5", "ROW 6", "ROW 7", "ROW 8", "ROW 9" };
            for (int i = 0; i < 10; ++i) {
                const float t = static_cast<float>(i) / 9.0f;
                GameObject row = panel(rowNames[i], &content, 0.0f, 0.0f, 0.0f, 0.0f, 10.0f, 10.0f,
                                       { 0.25f + 0.40f * t, 0.30f, 0.55f - 0.25f * t, 1.0f }, 0);
                row.AddComponent<UILayoutElementComponent>()->preferredHeight = 36.0f;
                label("ScrollRowLabel", row, rowTexts[i], 2.0f, 0);
            }
        }
    }

    // (2) Dropdown 300x40: 選択肢 4 つ・値 1。**開いた状態で始める** = golden に一覧 (手前に出る・高さが選択肢の
    //     数に縮む・選択中の印) が写る。入力台本の最初の押下は Blocker に当たって閉じる
    {
        GameObject dd = uiwidgets::CreateDropdown(s, "WidgetDropdown", 2.0f);
        placeTopLeft(dd, 860.0f, 160.0f, 300.0f, 40.0f);
        {
            auto* d = dd.GetComponent<UIDropdownComponent>();
            d->optionCount = 4;
            d->value = 1;
            d->expanded = true;
            std::snprintf(d->option0, sizeof(d->option0), "%s", "EASY");
            std::snprintf(d->option1, sizeof(d->option1), "%s", "NORMAL");
            std::snprintf(d->option2, sizeof(d->option2), "%s", "HARD");
            std::snprintf(d->option3, sizeof(d->option3), "%s", "EXPERT");
        }
    }
}

void UiDemoScriptInput(uint64_t tick, InputSnapshot& lane0)
{
    // 1 手 = [from, to) の区間。ポインタは (x0,y0) → (x1,y1) を線形に動き、buttons / vk はその間ずっと
    // 押している (vk は UINav* / Submit のキー。2 tick 押して離す = pressed のエッジが 1 回)。
    // M75g: wheel はその区間の毎 tick に渡すホイールの生値 (WHEEL_DELTA = 120 が 1 目盛り、負 = 手前へ回す)
    struct Step {
        uint16_t from, to;
        float x0, y0, x1, y1;
        uint8_t buttons;
        uint8_t vk;
        int16_t wheel;
    };
    constexpr uint8_t kVkReturn = 0x0D; // VK_RETURN = UINavSubmit
    constexpr uint8_t kVkEscape = 0x1B; // VK_ESCAPE = UINavCancel (M75g)
    constexpr uint8_t kVkLeft = 0x25;   // VK_LEFT = UINavLeft
    constexpr uint8_t kVkRight = 0x27;  // VK_RIGHT = UINavRight
    constexpr uint8_t kVkDown = 0x28;   // VK_DOWN = UINavDown
    constexpr int16_t kWheelDown = -120; // 手前へ 1 目盛り = 中身が上へ動く (下の方が見える)
    // 座標 = 既定キャンバス 1920x1080 の点 (= ゲーム面 px)。BuildUiShowcaseScene の配置から手で解いた値:
    //   Toggle i の Background の中心 (56, 530 + 48i) / SOLO のラベル (206, 626) / SELECTABLE (186, 740)
    //   左→右の溝 x 1366..1746 (y 624) / 右→左 (y 680) / 操作不可 (y 736) / 下→上の溝 y 614..834 (x 1820)
    // M75g: Scroll View (470,156)-(830,326) の見える範囲 (470,156)-(810,306) / 縦のバー x 810..830 / 横のバー
    //   y 306..326。Dropdown (860,160)-(1160,200)、開いた一覧の項目 i は y 206 + 28i から 28 (選択肢 4 つ)
    static constexpr Step kSteps[] = {
        // Toggle A (群で唯一の on): 押しても on のまま = changed は立たない。
        // ★M75g: 1 周目は Dropdown が開いた状態で始まるので、この押下は Blocker に当たって一覧を閉じるだけになる
        { 0, 10, 56, 530, 56, 530, 0, 0 },     { 10, 14, 56, 530, 56, 530, 1, 0 },
        { 14, 20, 56, 530, 56, 530, 0, 0 },
        // Toggle B: B が on になり A が off (群の規則)
        { 20, 30, 56, 578, 56, 578, 0, 0 },    { 30, 34, 56, 578, 56, 578, 1, 0 },
        { 34, 40, 56, 578, 56, 578, 0, 0 },
        // SOLO はラベルの上で押す (文字から Toggle の根へ泡立つ)
        { 40, 50, 206, 626, 206, 626, 0, 0 },  { 50, 54, 206, 626, 206, 626, 1, 0 },
        { 54, 60, 206, 626, 206, 626, 0, 0 },
        // 操作不可の Toggle: 押下を吸うだけで何も起きない
        { 60, 70, 56, 674, 56, 674, 0, 0 },    { 70, 74, 56, 674, 56, 674, 1, 0 },
        { 74, 80, 56, 674, 56, 674, 0, 0 },
        // Selectable 付きのボタン: clicked + フォーカス
        { 80, 90, 186, 740, 186, 740, 0, 0 },  { 90, 94, 186, 740, 186, 740, 1, 0 },
        { 94, 100, 186, 740, 186, 740, 0, 0 },
        // 左→右: 溝を押して値が飛ぶ → 押したまま右へ
        { 100, 110, 1600, 624, 1600, 624, 0, 0 }, { 110, 130, 1600, 624, 1700, 624, 1, 0 },
        { 130, 140, 1700, 624, 1700, 624, 0, 0 },
        // 左→右: つまみを中心から 3 ずらして掴み、左へ (掴んだ位置を保つ)。つまみの中心は 1695 —
        // 直前のドラッグで最後に押していた tick (p=129) の位置。★右端 (1705) はヒットの外 (半開区間) なので
        // 1705 で押すと掴めずに溝を押したことになる (1 回踏んだ)
        { 140, 150, 1698, 624, 1698, 624, 0, 0 }, { 150, 170, 1698, 624, 1450, 624, 1, 0 },
        { 170, 180, 1450, 624, 1450, 624, 0, 0 },
        // キー: 左→右がフォーカス中。右 / 右 / 左で値、下で右→左へ移り、右で値 (右→左なので減る)
        { 180, 182, 1450, 624, 1450, 624, 0, kVkRight }, { 182, 186, 1450, 624, 1450, 624, 0, 0 },
        { 186, 188, 1450, 624, 1450, 624, 0, kVkRight }, { 188, 192, 1450, 624, 1450, 624, 0, 0 },
        { 192, 194, 1450, 624, 1450, 624, 0, kVkLeft },  { 194, 200, 1450, 624, 1450, 624, 0, 0 },
        { 200, 202, 1450, 624, 1450, 624, 0, kVkDown },  { 202, 206, 1450, 624, 1450, 624, 0, 0 },
        { 206, 208, 1450, 624, 1450, 624, 0, kVkRight }, { 208, 212, 1450, 624, 1450, 624, 0, 0 },
        // 下→上: 押して下へドラッグ (値は下ほど小さい)
        { 212, 220, 1820, 700, 1820, 700, 0, 0 }, { 220, 240, 1820, 700, 1820, 800, 1, 0 },
        { 240, 250, 1820, 800, 1820, 800, 0, 0 },
        // 右→左の整数: 溝の中央を押して 5
        { 250, 260, 1556, 680, 1556, 680, 0, 0 }, { 260, 264, 1556, 680, 1556, 680, 1, 0 },
        { 264, 270, 1556, 680, 1556, 680, 0, 0 },
        // SOLO を押して off → Enter (Submit) で on に戻す
        { 270, 280, 56, 626, 56, 626, 0, 0 },  { 280, 284, 56, 626, 56, 626, 1, 0 },
        { 284, 290, 56, 626, 56, 626, 0, 0 },  { 290, 292, 56, 626, 56, 626, 0, kVkReturn },
        { 292, 300, 56, 626, 56, 626, 0, 0 },
        // 操作不可の Slider: 押してドラッグしても値は動かない
        { 300, 306, 1600, 736, 1600, 736, 0, 0 }, { 306, 316, 1600, 736, 1650, 736, 1, 0 },
        { 316, 320, 1650, 736, 1650, 736, 0, 0 },
        // ---- M75g ----
        // Scroll View: ホイールを手前へ 3 目盛り (1 目盛りずつ間を空ける)
        { 320, 330, 640, 230, 640, 230, 0, 0 },
        { 330, 331, 640, 230, 640, 230, 0, 0, kWheelDown }, { 331, 334, 640, 230, 640, 230, 0, 0 },
        { 334, 335, 640, 230, 640, 230, 0, 0, kWheelDown }, { 335, 338, 640, 230, 640, 230, 0, 0 },
        { 338, 339, 640, 230, 640, 230, 0, 0, kWheelDown }, { 339, 346, 640, 230, 640, 230, 0, 0 },
        // 行をつかんで上へ投げる: 閾値を越えた tick に ScrollRect がドラッグを引き取り、離した後は慣性で滑って
        // 下端を越えた分を Elastic で戻す
        { 346, 356, 640, 250, 640, 150, 1, 0 }, { 356, 380, 640, 150, 640, 150, 0, 0 },
        // 縦のバー: 溝の上端より上 (バーの背景) を押して値 1 (上端) へ → 押したまま下へ (つまみが付いてくる)
        { 380, 384, 820, 165, 820, 165, 1, 0 }, { 384, 396, 820, 165, 820, 290, 1, 0 },
        { 396, 402, 820, 290, 820, 290, 0, 0 },
        // 横のバー: 右寄りを押す (つまみの中心が押した点へ飛ぶ)
        { 402, 406, 780, 316, 780, 316, 1, 0 }, { 406, 412, 780, 316, 780, 316, 0, 0 },
        // Dropdown: 開く → 項目 2 (HARD) を押す = 値 2 で閉じる。★離す tick は根の上に置く (離すと同時に
        // 項目へ動かすと、まだ一覧が開いていないので根の外で離したことになり開かない。1 回踏んだ)
        { 412, 416, 1010, 180, 1010, 180, 1, 0 }, { 416, 418, 1010, 180, 1010, 180, 0, 0 },
        { 418, 422, 1010, 276, 1010, 276, 0, 0 },
        { 422, 426, 1010, 276, 1010, 276, 1, 0 }, { 426, 432, 1010, 276, 1010, 276, 0, 0 },
        // 開く (選択中の項目 2 にフォーカス。カーソルは根の上 = Blocker なので項目を奪わない) → 下 (項目 3) →
        // Enter = 値 3 (EXPERT) で閉じる
        { 432, 436, 1010, 180, 1010, 180, 1, 0 }, { 436, 440, 1010, 180, 1010, 180, 0, 0 },
        { 440, 442, 1010, 180, 1010, 180, 0, kVkDown }, { 442, 446, 1010, 180, 1010, 180, 0, 0 },
        { 446, 448, 1010, 180, 1010, 180, 0, kVkReturn }, { 448, 452, 1010, 180, 1010, 180, 0, 0 },
        // 開く → Escape (Cancel) で閉じる = 値は変わらない
        { 452, 456, 1010, 180, 1010, 180, 1, 0 }, { 456, 460, 1010, 180, 1010, 180, 0, 0 },
        { 460, 462, 1010, 180, 1010, 180, 0, kVkEscape }, { 462, 466, 1010, 180, 1010, 180, 0, 0 },
        // 開く → 一覧の外 (Title の上) を押す = Blocker で閉じる
        { 466, 470, 1010, 180, 1010, 180, 1, 0 }, { 470, 476, 1010, 180, 700, 450, 0, 0 },
        { 476, 480, 700, 450, 700, 450, 1, 0 },   { 480, 490, 700, 450, 700, 450, 0, 0 },
    };
    constexpr uint64_t kStart = 30;   // シーンが落ち着くまで待つ
    constexpr uint64_t kPeriod = 490; // 600 tick で 1.2 周

    float x = 8.0f; // 待機位置: 背景だけがある左下の隅
    float y = 1072.0f;
    uint8_t buttons = 0;
    uint8_t vk = 0;
    int16_t wheel = 0;
    if (tick >= kStart) {
        const uint64_t p = (tick - kStart) % kPeriod;
        for (const Step& st : kSteps) {
            if (p >= st.from && p < st.to) {
                const float t = static_cast<float>(p - st.from) / static_cast<float>(st.to - st.from);
                x = st.x0 + (st.x1 - st.x0) * t;
                y = st.y0 + (st.y1 - st.y0) * t;
                buttons = st.buttons;
                vk = st.vk;
                wheel = st.wheel;
                break;
            }
        }
    }
    lane0.surfW = 1920; // 合成入力と同じく基準解像度に固定 (窓の大きさを混ぜない)
    lane0.surfH = 1080;
    lane0.mouseSurfX = x;
    lane0.mouseSurfY = y;
    lane0.mouseX = static_cast<int32_t>(x);
    lane0.mouseY = static_cast<int32_t>(y);
    lane0.mouseButtons = buttons;
    // M75g: ホイールも置き換える (キーと同じ理由 — 同じフレームの 2 本目の tick に残らないように)
    lane0.wheelDelta = wheel;
    // ★キーは OR ではなく**丸ごと置き換える**。EngineLoop はライブ入力をフレーム頭に 1 回だけ写し、
    //   同じフレームで回る 2 本目以降の tick は前の tick が書いた ctx.inputs[0] をそのまま受け取る。
    //   OR だと前の tick で押したキーが消えず (--replay-fast は 1 フレームで何 tick も回る)、
    //   同じキーの 2 回目が「押しっぱなし」になって pressed のエッジが立たない (1 回踏んだ)
    for (uint8_t& k : lane0.keys) {
        k = 0;
    }
    if (vk != 0) {
        lane0.keys[vk >> 3] |= static_cast<uint8_t>(1u << (vk & 7));
    }
}

} // namespace mye
