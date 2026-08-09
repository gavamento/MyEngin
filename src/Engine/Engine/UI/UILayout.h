#pragma once
// UI 矩形解決の共有実装 (M51e)。D3D 非依存 — UISelfTest がヘッドレスで検証する。
// UIRenderer (描画) / UIFocusNav (EngineApiTable) / UIHitTest (M51h) の 3 者が同じ関数で
// 矩形を解くことで「描画とヒットテストのズレ」を構造的に断つ。
// UIElement は描画専用 (kComponentNoHash) だが、UIFocusNav は sim レーンから呼ばれる —
// ここは World の状態と引数のみに依存する純関数群 (ウィンドウ実寸などは読まない)。
#include "Engine/Core/EntityID.h"

namespace mye {

class World;
struct UIElementComponent;

namespace uilayout {

// 解決済みスクリーン px 矩形 (左上原点)
struct UIRect {
    float x = 0, y = 0, w = 0, h = 0;
};

// 9-grid anchor (0..8) の基準点: base 矩形の 左/中/右 × 上/中/下
inline void AnchorOrigin(int anchor, const UIRect& base, float& outX, float& outY)
{
    const int col = anchor % 3;  // 0=左 1=中 2=右
    const int row = anchor / 3;  // 0=上 1=中 2=下
    outX = base.x + ((col == 0) ? 0.0f : (col == 1) ? base.w * 0.5f : base.w);
    outY = base.y + ((row == 0) ? 0.0f : (row == 1) ? base.h * 0.5f : base.h);
}

// a ∩ b (交差なしは w/h<=0 の退化矩形)
inline UIRect Intersect(const UIRect& a, const UIRect& b)
{
    UIRect r;
    r.x = (a.x > b.x) ? a.x : b.x;
    r.y = (a.y > b.y) ? a.y : b.y;
    const float ax1 = a.x + a.w, bx1 = b.x + b.w;
    const float ay1 = a.y + a.h, by1 = b.y + b.h;
    r.w = ((ax1 < bx1) ? ax1 : bx1) - r.x;
    r.h = ((ay1 < by1) ? ay1 : by1) - r.y;
    return r;
}

// e の UIElement を screen px 矩形に解決する。space=1 は最寄りの UIElement 祖先の解決済み
// 矩形基準 (無ければ screen へフォールバック)。壊れ親/循環は深度上限で打ち切り安全。
// e が UIElement を持たなければ {0,0,0,0} を返す。
UIRect ResolveRect(World& world, EntityID e, int screenW, int screenH);

// e の祖先の clipChildren 矩形をすべて交差した「見えてよい範囲」。クリップ祖先が
// 無ければ screen 全域。e 自身の clipChildren は含まない (自分は切らない)。
UIRect ResolveClipRect(World& world, EntityID e, int screenW, int screenH);

// 便利形: 要素の可視矩形 = ResolveRect ∩ ResolveClipRect (完全に隠れていれば w/h<=0)
UIRect ResolveVisibleRect(World& world, EntityID e, int screenW, int screenH);

} // namespace uilayout
} // namespace mye
