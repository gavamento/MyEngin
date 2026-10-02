//====================================================================================
//                          UIWidgetFactory.cpp
//  MyEngine/ 秋田蓮音                                                      09/13/2026
//                                          子構成込みのウィジェット（Toggle / Slider / Scroll / Dropdown）の組み立ての実装
//====================================================================================
#include "Engine/Engine/UI/UIWidgetFactory.h"

#include <cstdio>

#include "Engine/Core/Ecs/Components.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Engine/UI/UILayoutGroup.h" // M75g: Dropdown の一覧の Layout Group / Fitter の定数
#include "Engine/Engine/UI/UIWidgets.h"

namespace mye {
namespace uiwidgets {
namespace {

// ★どの関数も **RectTransform を最初に**足し、足したらすぐ書く。1 つのエンティティへ続けて AddComponent
//   するとアーキタイプが移って前のポインタが無効になる (DemoContent / CreateMenu と同じ規則)
void AddRect(GameObject& go, DirectX::XMFLOAT2 anchorMin, DirectX::XMFLOAT2 anchorMax,
             DirectX::XMFLOAT2 pivot, DirectX::XMFLOAT2 pos, DirectX::XMFLOAT2 size)
{
    auto* rt = go.AddComponent<RectTransformComponent>();
    rt->anchorMin = anchorMin;
    rt->anchorMax = anchorMax;
    rt->pivot = pivot;
    rt->anchoredPosition = pos;
    rt->sizeDelta = size;
}

// 親の anchorMin..anchorMax の矩形から左 / 上 / 右 / 下の余白だけ内側 (Unity の offsetMin / offsetMax)。
// pivot 0.5 の RectFromTransform で x = 親左 + 親幅 * anchorMin.x + left、w = 親幅 * Δanchor - (left + right)
// になる値 = anchoredPosition ((left - right) / 2, (top - bottom) / 2)、sizeDelta (-(left + right), -(top + bottom))
void AddInsetRect(GameObject& go, DirectX::XMFLOAT2 anchorMin, DirectX::XMFLOAT2 anchorMax,
                  float left, float top, float right, float bottom)
{
    AddRect(go, anchorMin, anchorMax, { 0.5f, 0.5f },
            { (left - right) * 0.5f, (top - bottom) * 0.5f }, { -(left + right), -(top + bottom) });
}

void AddPanel(GameObject& go, DirectX::XMFLOAT4 color)
{
    auto* el = go.AddComponent<UIElementComponent>();
    el->kind = 0;
    el->color = color;
}

} // namespace

GameObject CreateToggle(Scene& scene, const char* name, const char* label, float labelScale)
{
    GameObject root = scene.CreateGameObjectTracked(name);
    AddRect(root, { 0.5f, 0.5f }, { 0.5f, 0.5f }, { 0.5f, 0.5f }, { 0.0f, 0.0f }, { 160.0f, 40.0f });

    GameObject background = scene.CreateGameObjectTracked("Background");
    AddRect(background, { 0.0f, 0.5f }, { 0.0f, 0.5f }, { 0.0f, 0.5f }, { 4.0f, 0.0f },
            { 32.0f, 32.0f });
    AddPanel(background, { 1.0f, 1.0f, 1.0f, 1.0f }); // 状態色を掛けるので白 (Unity の既定と同じ)
    background.SetParent(root);

    GameObject checkmark = scene.CreateGameObjectTracked("Checkmark");
    AddRect(checkmark, { 0.5f, 0.5f }, { 0.5f, 0.5f }, { 0.5f, 0.5f }, { 0.0f, 0.0f },
            { 20.0f, 20.0f });
    AddPanel(checkmark, { 0.20f, 0.22f, 0.28f, 1.0f });
    checkmark.SetParent(background);

    GameObject text = scene.CreateGameObjectTracked("Label");
    AddInsetRect(text, { 0.0f, 0.0f }, { 1.0f, 1.0f }, 44.0f, 0.0f, 4.0f, 0.0f);
    {
        auto* el = text.AddComponent<UIElementComponent>();
        el->kind = 1;
        el->align = 3; // 左中央
        el->fontScale = labelScale;
        std::snprintf(el->text, sizeof(el->text), "%s", label);
    }
    text.SetParent(root);

    root.AddComponent<UISelectableComponent>()->targetGraphic = background.Id();
    root.AddComponent<UIToggleComponent>()->graphic = checkmark.Id();
    return root;
}

GameObject CreateSlider(Scene& scene, const char* name, int direction)
{
    const int dir = (direction >= kSliderLeftToRight && direction <= kSliderTopToBottom)
        ? direction : kSliderLeftToRight;
    const bool vertical = dir == kSliderBottomToTop || dir == kSliderTopToBottom;

    GameObject root = scene.CreateGameObjectTracked(name);
    AddRect(root, { 0.5f, 0.5f }, { 0.5f, 0.5f }, { 0.5f, 0.5f }, { 0.0f, 0.0f },
            vertical ? DirectX::XMFLOAT2{ 40.0f, 160.0f } : DirectX::XMFLOAT2{ 160.0f, 40.0f });

    // 軸と直交する向きに 30%..70% の帯 (Unity の 0.25..0.75 を 40 の高さに合わせて少し細く)
    const DirectX::XMFLOAT2 bandMin = vertical ? DirectX::XMFLOAT2{ 0.3f, 0.0f } : DirectX::XMFLOAT2{ 0.0f, 0.3f };
    const DirectX::XMFLOAT2 bandMax = vertical ? DirectX::XMFLOAT2{ 0.7f, 1.0f } : DirectX::XMFLOAT2{ 1.0f, 0.7f };

    GameObject background = scene.CreateGameObjectTracked("Background");
    AddInsetRect(background, bandMin, bandMax, 0.0f, 0.0f, 0.0f, 0.0f);
    AddPanel(background, { 0.25f, 0.26f, 0.32f, 1.0f });
    background.SetParent(root);

    // 塗りの余白は値の始点側 5・終点側 15 (Unity の既定。塗りの sizeDelta 10 と合わせて、値 0 で始点に
    // 半分だけ顔を出し、値 1 でつまみの下に隠れる)。向きを反転したら余白も反転する (Unity の SetDirection)
    float left = 0.0f, top = 0.0f, right = 0.0f, bottom = 0.0f;
    switch (dir) {
    case kSliderRightToLeft: left = 15.0f; right = 5.0f; break;
    case kSliderBottomToTop: bottom = 5.0f; top = 15.0f; break;
    case kSliderTopToBottom: top = 5.0f; bottom = 15.0f; break;
    default: left = 5.0f; right = 15.0f; break;
    }
    GameObject fillArea = scene.CreateGameObjectTracked("Fill Area");
    AddInsetRect(fillArea, bandMin, bandMax, left, top, right, bottom);
    fillArea.SetParent(root);

    GameObject fill = scene.CreateGameObjectTracked("Fill");
    AddRect(fill, { 0.0f, 0.0f }, { 0.0f, 1.0f }, { 0.5f, 0.5f }, { 0.0f, 0.0f },
            vertical ? DirectX::XMFLOAT2{ 0.0f, 10.0f } : DirectX::XMFLOAT2{ 10.0f, 0.0f });
    AddPanel(fill, { 0.35f, 0.55f, 0.90f, 1.0f });
    fill.SetParent(fillArea);

    GameObject handleArea = scene.CreateGameObjectTracked("Handle Slide Area");
    if (vertical) {
        AddInsetRect(handleArea, { 0.0f, 0.0f }, { 1.0f, 1.0f }, 0.0f, 10.0f, 0.0f, 10.0f);
    } else {
        AddInsetRect(handleArea, { 0.0f, 0.0f }, { 1.0f, 1.0f }, 10.0f, 0.0f, 10.0f, 0.0f);
    }
    handleArea.SetParent(root);

    GameObject handle = scene.CreateGameObjectTracked("Handle");
    AddRect(handle, { 0.0f, 0.0f }, { 0.0f, 1.0f }, { 0.5f, 0.5f }, { 0.0f, 0.0f },
            vertical ? DirectX::XMFLOAT2{ 0.0f, 20.0f } : DirectX::XMFLOAT2{ 20.0f, 0.0f });
    AddPanel(handle, { 1.0f, 1.0f, 1.0f, 1.0f });
    handle.SetParent(handleArea);

    root.AddComponent<UISelectableComponent>()->targetGraphic = handle.Id();
    {
        auto* slider = root.AddComponent<UISliderComponent>();
        slider->fillRect = fill.Id();
        slider->handleRect = handle.Id();
        slider->direction = dir;
    }
    return root;
}

GameObject CreateScrollbar(Scene& scene, const char* name, int direction)
{
    const int dir = (direction >= kSliderLeftToRight && direction <= kSliderTopToBottom)
        ? direction : kSliderLeftToRight;
    const bool vertical = dir == kSliderBottomToTop || dir == kSliderTopToBottom;

    GameObject root = scene.CreateGameObjectTracked(name);
    AddRect(root, { 0.5f, 0.5f }, { 0.5f, 0.5f }, { 0.5f, 0.5f }, { 0.0f, 0.0f },
            vertical ? DirectX::XMFLOAT2{ 20.0f, 160.0f } : DirectX::XMFLOAT2{ 160.0f, 20.0f });
    AddPanel(root, { 0.18f, 0.19f, 0.24f, 1.0f });

    // Unity の既定と同じく、溝の内側 10 にアンカー部分を取り、つまみの sizeDelta 20 で溝の端まで届かせる
    GameObject area = scene.CreateGameObjectTracked("Sliding Area");
    AddInsetRect(area, { 0.0f, 0.0f }, { 1.0f, 1.0f }, 10.0f, 10.0f, 10.0f, 10.0f);
    area.SetParent(root);

    GameObject handle = scene.CreateGameObjectTracked("Handle");
    AddRect(handle, { 0.0f, 0.0f }, { 1.0f, 1.0f }, { 0.5f, 0.5f }, { 0.0f, 0.0f }, { 20.0f, 20.0f });
    AddPanel(handle, { 0.72f, 0.74f, 0.80f, 1.0f });
    handle.SetParent(area);

    root.AddComponent<UISelectableComponent>()->targetGraphic = handle.Id();
    {
        auto* bar = root.AddComponent<UIScrollbarComponent>();
        bar->handleRect = handle.Id();
        bar->direction = dir;
    }
    return root;
}

GameObject CreateScrollView(Scene& scene, const char* name)
{
    constexpr float kBarWidth = 20.0f;
    GameObject root = scene.CreateGameObjectTracked(name);
    AddRect(root, { 0.5f, 0.5f }, { 0.5f, 0.5f }, { 0.5f, 0.5f }, { 0.0f, 0.0f }, { 400.0f, 300.0f });
    AddPanel(root, { 0.10f, 0.11f, 0.14f, 1.0f });

    // 左上基準で右と下にスクロールバーの幅を空ける (Unity の Viewport は pivot (0,1) = 左上)
    GameObject viewport = scene.CreateGameObjectTracked("Viewport");
    AddRect(viewport, { 0.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 0.0f }, { 0.0f, 0.0f },
            { -kBarWidth, -kBarWidth });
    {
        auto* el = viewport.AddComponent<UIElementComponent>();
        el->kind = 0;
        el->color = { 0.14f, 0.15f, 0.19f, 1.0f };
        el->clipChildren = true;
    }
    viewport.SetParent(root);

    // 上辺に沿って横いっぱい、pivot は左上 (Unity の Content と同じ)。高さは見える範囲より大きくしておく
    GameObject content = scene.CreateGameObjectTracked("Content");
    AddRect(content, { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 0.0f, 0.0f }, { 0.0f, 0.0f }, { 0.0f, 600.0f });
    content.SetParent(viewport);

    GameObject hbar = CreateScrollbar(scene, "Scrollbar Horizontal", kSliderLeftToRight);
    {
        auto* rt = hbar.GetComponent<RectTransformComponent>();
        rt->anchorMin = { 0.0f, 1.0f };
        rt->anchorMax = { 1.0f, 1.0f };
        rt->pivot = { 0.0f, 1.0f };
        rt->anchoredPosition = { 0.0f, 0.0f };
        rt->sizeDelta = { -kBarWidth, kBarWidth };
    }
    hbar.SetParent(root);

    GameObject vbar = CreateScrollbar(scene, "Scrollbar Vertical", kSliderBottomToTop);
    {
        auto* rt = vbar.GetComponent<RectTransformComponent>();
        rt->anchorMin = { 1.0f, 0.0f };
        rt->anchorMax = { 1.0f, 1.0f };
        rt->pivot = { 1.0f, 0.0f };
        rt->anchoredPosition = { 0.0f, 0.0f };
        rt->sizeDelta = { kBarWidth, -kBarWidth };
    }
    vbar.SetParent(root);

    {
        auto* sr = root.AddComponent<UIScrollRectComponent>();
        sr->content = content.Id();
        sr->viewport = viewport.Id();
        sr->horizontalScrollbar = hbar.Id();
        sr->verticalScrollbar = vbar.Id();
    }
    return root;
}

GameObject CreateDropdown(Scene& scene, const char* name, float labelScale)
{
    constexpr float kItemHeight = 28.0f;
    constexpr float kBarWidth = 20.0f;
    const DirectX::XMFLOAT4 textColor = { 0.15f, 0.16f, 0.20f, 1.0f };

    GameObject root = scene.CreateGameObjectTracked(name);
    AddRect(root, { 0.5f, 0.5f }, { 0.5f, 0.5f }, { 0.5f, 0.5f }, { 0.0f, 0.0f }, { 160.0f, 40.0f });
    AddPanel(root, { 1.0f, 1.0f, 1.0f, 1.0f }); // 状態色を掛けるので白 (Unity の既定と同じ)

    GameObject label = scene.CreateGameObjectTracked("Label");
    AddInsetRect(label, { 0.0f, 0.0f }, { 1.0f, 1.0f }, 10.0f, 6.0f, 30.0f, 7.0f);
    {
        auto* el = label.AddComponent<UIElementComponent>();
        el->kind = 1;
        el->align = 3; // 左中央
        el->color = textColor;
        el->fontScale = labelScale;
    }
    label.SetParent(root);

    GameObject arrow = scene.CreateGameObjectTracked("Arrow");
    AddRect(arrow, { 1.0f, 0.5f }, { 1.0f, 0.5f }, { 0.5f, 0.5f }, { -15.0f, 0.0f }, { 14.0f, 14.0f });
    AddPanel(arrow, { 0.30f, 0.32f, 0.40f, 1.0f });
    arrow.SetParent(root);

    // 一覧は根の下辺から 2 下に吊る (Unity の Template: 横いっぱい・pivot は上辺)。高さは選択肢の数に合わせて
    // 縮む (uiwidgets::WidgetDrivenTransform) ので、ここは最大の高さ
    GameObject tmpl = scene.CreateGameObjectTracked("Template");
    AddRect(tmpl, { 0.0f, 1.0f }, { 1.0f, 1.0f }, { 0.5f, 0.0f }, { 0.0f, 2.0f }, { 0.0f, 200.0f });
    AddPanel(tmpl, { 0.95f, 0.95f, 0.97f, 1.0f });
    tmpl.SetParent(root);

    GameObject viewport = scene.CreateGameObjectTracked("Viewport");
    AddRect(viewport, { 0.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 0.0f }, { 0.0f, 0.0f }, { -kBarWidth, 0.0f });
    {
        auto* el = viewport.AddComponent<UIElementComponent>();
        el->kind = 0;
        el->color = { 0.95f, 0.95f, 0.97f, 1.0f };
        el->clipChildren = true;
    }
    viewport.SetParent(tmpl);

    GameObject content = scene.CreateGameObjectTracked("Content");
    AddRect(content, { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 0.5f, 0.0f }, { 0.0f, 0.0f }, { 0.0f, kItemHeight });
    {
        auto* g = content.AddComponent<UILayoutGroupComponent>();
        g->kind = uilayout::kLayoutVertical;
        g->padding = { 0.0f, 4.0f, 0.0f, 4.0f };
        g->controlChildWidth = true;
        g->controlChildHeight = false;
        g->forceExpandWidth = true;
        g->forceExpandHeight = false;
    }
    content.AddComponent<UIContentSizeFitterComponent>()->verticalFit = uilayout::kFitPreferred;
    content.SetParent(viewport);

    for (int i = 0; i < kDropdownMaxOptions; ++i) {
        char itemName[16];
        std::snprintf(itemName, sizeof(itemName), "Item %d", i);
        GameObject item = scene.CreateGameObjectTracked(itemName);
        AddRect(item, { 0.0f, 0.0f }, { 0.0f, 0.0f }, { 0.5f, 0.5f }, { 0.0f, 0.0f }, { 0.0f, kItemHeight });
        AddPanel(item, { 0.35f, 0.55f, 0.90f, 1.0f });
        item.SetParent(content);

        GameObject check = scene.CreateGameObjectTracked("Item Checkmark");
        AddRect(check, { 0.0f, 0.5f }, { 0.0f, 0.5f }, { 0.5f, 0.5f }, { 14.0f, 0.0f }, { 10.0f, 10.0f });
        AddPanel(check, textColor);
        check.SetParent(item);

        GameObject itemLabel = scene.CreateGameObjectTracked("Item Label");
        AddInsetRect(itemLabel, { 0.0f, 0.0f }, { 1.0f, 1.0f }, 28.0f, 0.0f, 8.0f, 0.0f);
        {
            auto* el = itemLabel.AddComponent<UIElementComponent>();
            el->kind = 1;
            el->align = 3;
            el->color = textColor;
            el->fontScale = labelScale;
        }
        itemLabel.SetParent(item);

        {
            // 背景は普段は透明で、カーソル / フォーカス / 押下の間だけ青く出る (Unity の項目の Toggle と同じ役)
            auto* sel = item.AddComponent<UISelectableComponent>();
            sel->normalColor = { 1.0f, 1.0f, 1.0f, 0.0f };
            sel->highlightedColor = { 1.0f, 1.0f, 1.0f, 0.55f };
            sel->selectedColor = { 1.0f, 1.0f, 1.0f, 0.85f };
            sel->pressedColor = { 0.8f, 0.8f, 0.8f, 1.0f };
            sel->disabledColor = { 1.0f, 1.0f, 1.0f, 0.0f };
        }
        {
            auto* di = item.AddComponent<UIDropdownItemComponent>();
            di->index = i;
            di->label = itemLabel.Id();
            di->checkmark = check.Id();
        }
    }

    GameObject bar = CreateScrollbar(scene, "Scrollbar", kSliderBottomToTop);
    {
        auto* rt = bar.GetComponent<RectTransformComponent>();
        rt->anchorMin = { 1.0f, 0.0f };
        rt->anchorMax = { 1.0f, 1.0f };
        rt->pivot = { 1.0f, 0.0f };
        rt->anchoredPosition = { 0.0f, 0.0f };
        rt->sizeDelta = { kBarWidth, 0.0f };
    }
    bar.SetParent(tmpl);

    {
        // Unity の Template の ScrollRect: 縦だけ・Clamped
        auto* sr = tmpl.AddComponent<UIScrollRectComponent>();
        sr->content = content.Id();
        sr->viewport = viewport.Id();
        sr->horizontal = false;
        sr->vertical = true;
        sr->movementType = kScrollClamped;
        sr->verticalScrollbar = bar.Id();
    }

    root.AddComponent<UISelectableComponent>();
    {
        auto* dd = root.AddComponent<UIDropdownComponent>();
        dd->templateRect = tmpl.Id();
        dd->captionText = label.Id();
    }
    return root;
}

} // namespace uiwidgets
} // namespace mye
