#pragma once
// UI 矩形解決の共有実装 (M51e、ワールド追従 UI 追加)。D3D 非依存 — UISelfTest が
// ヘッドレスで検証する。UIRenderer (描画) / UIFocusNav (EngineApiTable) / UIHitTest (M51h) の
// 3 者が同じ関数で矩形を解くことで「描画とヒットテストのズレ」を構造的に断つ。
// UIElement は描画専用 (kComponentNoHash) だが、UIFocusNav は sim レーンから呼ばれる —
// ここは World の状態と引数のみに依存する純関数群 (ウィンドウ実寸などは読まない)。
//
// ワールド追従 (オブジェクト追従 UI) は**エンティティ構成による完全自動判定**:
//   「UI 専用オブジェクト」(基本 4 種 + UIElement + エディタ帳簿 + スクリプト状態のみ) に
//   付いた UIElement は従来どおり画面 UI。**それ以外のコンポーネント (メッシュ/コライダー等)
//   を持つオブジェクトに付いた UIElement は、そのオブジェクトのワールド位置の射影点が基準**
//   になる (= オブジェクトに UI が出る)。判定の正本は UILayout.cpp の IsUiOnlyEntity。
//   複合ウィジェット (HP バー等) は追従オブジェクトに背景 UIElement を直付けし、
//   子の UI 専用エンティティを space=1 でぶら下げる (親矩形基準なので一緒に追従する)。
#include <DirectXMath.h>

#include "Engine/Core/EntityID.h"

namespace mye {

class World;
struct UIElementComponent;
struct PrevWorldStore; // RenderSystem.h (描画補間 M36b)。sim レーンは使わない

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

// ワールド追従 UI の射影入力。null = world 追従要素は非表示扱い (screen UI は無関係)。
// render 側: RenderSystem が解決した補間済みカメラの view*projNoJitter + PrevWorldStore。
// sim 側 (UIHitTest / FocusNav): BuildSimWorldContext (scalar 決定論構築、補間なし)。
struct UIWorldContext {
    DirectX::XMFLOAT4X4 viewProj = {};         // ジッタ無しの view*proj
    const PrevWorldStore* prevWorld = nullptr; // 位置補間元 (render のみ)。sim は nullptr
    float alpha = 1.0f;                        // 補間係数 (interpAlpha)
};

// 解決結果。scale は距離スケール (UIElement.distanceScale) の伝播係数 — space=1 の子は
// 親の scale を継承しオフセットとサイズに掛かる。screen UI は常に 1.0f で、x*1.0f = x は
// ビット恒等なので既存要素の矩形は従来と完全一致する。
struct UIResolved {
    UIRect rect;
    float scale = 1.0f;
    bool visible = true; // false = カメラ背面 (クランプ OFF) / コンテキスト無しの world 要素
};

// e の UIElement を screen px 矩形に解決する (正本)。space=1 は最寄りの UIElement 祖先の
// 解決済み矩形基準。それ以外はワールド追従判定 (冒頭コメント: UI 専用でないオブジェクト上の
// UIElement は自エンティティの射影点基準) → 該当しなければ screen 基準 (従来)。
// 壊れ親/循環は深度上限で打ち切り安全。UIElement 非所持は visible=false。
UIResolved Resolve(World& world, EntityID e, int screenW, int screenH,
                   const UIWorldContext* wc);

// sim レーン用の決定論カメラ構築 — RenderSystem と同じ選択規則 (走査順の先頭、isPrimary 優先)
// で scalar 演算のみ (SIMD 禁止 = Debug/Release ビット一致)。WorldMatrix は tick 内で
// TransformSystem が更新済み (RaycastWorld と同じ前例)。カメラ不在は false。
// aspect は screenW/screenH — sim は基準解像度 (1920x1080) 固定なので、実ウィンドウが
// 16:9 でないときは描画との射影が横方向にずれる (screen UI の比例ズレと同じ既知の割り切り)。
bool BuildSimWorldContext(World& world, int screenW, int screenH, UIWorldContext& out);

// 互換ラッパ: Resolve().rect (visible=false は {0,0,0,0} = 従来の「隠れている」表現に合流)
UIRect ResolveRect(World& world, EntityID e, int screenW, int screenH,
                   const UIWorldContext* wc = nullptr);

// e の祖先の clipChildren 矩形をすべて交差した「見えてよい範囲」。クリップ祖先が
// 無ければ screen 全域。e 自身の clipChildren は含まない (自分は切らない)。
UIRect ResolveClipRect(World& world, EntityID e, int screenW, int screenH,
                       const UIWorldContext* wc = nullptr);

// 便利形: 要素の可視矩形 = ResolveRect ∩ ResolveClipRect (完全に隠れていれば w/h<=0)
UIRect ResolveVisibleRect(World& world, EntityID e, int screenW, int screenH,
                          const UIWorldContext* wc = nullptr);

} // namespace uilayout
} // namespace mye
