//====================================================================================
//                          UIWidgetFactory.cpp
//  MyEngine/ 秋田蓮音                                                      09/13/2026
//                                          子構成込みのウィジェット（Toggle / Slider）の組み立ての実装
//====================================================================================
#include "Engine/Engine/UI/UIWidgetFactory.h"

#include <cstdio>

#include "Engine/Core/Components.h"
#include "Engine/Engine/Scene.h"
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

} // namespace uiwidgets
} // namespace mye
