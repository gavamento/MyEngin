#pragma once
// UI 矩形解決の共有実装 (M51e、ワールド追従 UI 追加)。D3D 非依存 — UISelfTest が
// ヘッドレスで検証する。UIRenderer (描画) / UIFocusNav (EngineApiTable) / UIHitTest (M51h) の
// 3 者が同じ関数で矩形を解くことで「描画とヒットテストのズレ」を構造的に断つ。
// UIElement は描画専用 (kComponentNoHash) だが、UIFocusNav は sim レーンから呼ばれる —
// ここは World の状態と引数のみに依存する純関数群 (ウィンドウ実寸などは読まない)。
//
// ワールド追従 (オブジェクト追従 UI) は**エンティティ構成による完全自動判定**:
//   「UI 専用オブジェクト」(基本 4 種 + UIElement + エディタ帳簿 + スクリプト状態のみ) に
//   付いた UIElement は画面 UI。**それ以外のコンポーネント (メッシュ/コライダー等)
//   を持つオブジェクトに付いた UIElement は、そのオブジェクトのワールド位置の射影点が基準**
//   になる (= オブジェクトに UI が出る)。判定の正本は UILayout.cpp の IsUiOnlyEntity。
//   複合ウィジェット (HP バー等) は追従オブジェクトに背景 UIElement を直付けし、
//   子の UI 専用エンティティを space=1 でぶら下げる (親矩形基準なので一緒に追従する)。
#include <DirectXMath.h>

#include "Engine/Core/EntityID.h"

namespace mye {

class World;
struct InputSnapshot;
struct UIElementComponent;
struct RectTransformComponent;
struct PrevWorldStore; // RenderSystem.h (描画補間 M36b)。sim レーンは使わない

namespace uilayout {

class LayoutScratch; // UILayoutGroup.h (M75e: 自動レイアウトの呼び出し単位のメモ)

// 解決済みキャンバス矩形 (左上原点)。**単位は px ではなくキャンバス単位** (M70b) —
// 実 px へ落とすのは描画側の仕事で、CanvasSize().scale を掛ける
struct UIRect {
    float x = 0, y = 0, w = 0, h = 0;
};

// ---- キャンバス (M70b) ----
// 基準解像度の**既定値**。UI の数値 (x/y/w/h/fontScale/sliceBorder) はすべて基準解像度で
// オーサリングされているものとして扱う。M75c で実効値は project_settings.json の
// ui.referenceW/H になった (DefaultCanvasDesc)。この定数は「設定が無いとき」の値
inline constexpr int kCanvasRefW = 1920;
inline constexpr int kCanvasRefH = 1080;

// Canvas Scaler の Screen Match Mode (M75c)。UICanvasComponent.scaleMode と同じ値
inline constexpr int kScaleExpand = 0; // s = min(sx, sy)。キャンバスは基準より広がる側へ伸びる
inline constexpr int kScaleShrink = 1; // s = max(sx, sy)。キャンバスは基準より狭まる側へ縮む
inline constexpr int kScaleMatch = 2;  // s = sx^(1-m) * sy^m (Unity の対数空間 lerp と同値)

// キャンバスの解き方 (M75c)。**ConstantPixelSize は無い** — キャンバス寸法が画素数の関数になり、
// sim のヒットテストがウィンドウの画素数に依存してしまう。3 モードとも「一様スケール +
// アスペクトで伸びる矩形」の枠内で s の決め方が違うだけ
struct CanvasDesc {
    int referenceW = kCanvasRefW;
    int referenceH = kCanvasRefH;
    int scaleMode = kScaleExpand;
    float match = 0.0f; // kScaleMatch のとき 0 = 幅に合わせる / 1 = 高さに合わせる
};

// キャンバス寸法と、キャンバス → 実 px の一様倍率。
struct CanvasInfo {
    float scale = 1.0f;   // キャンバス単位 → 実 px。**縦横で同じ値** (歪ませない)
    int w = kCanvasRefW;  // キャンバス幅 (キャンバス単位)
    int h = kCanvasRefH;  // キャンバス高さ
};

// Unity (Canvas Scaler の Scale With Screen Size / Screen Match Mode = Expand) と
// UE5 (UMG の DPI スケーリング) に合わせたモデル。**基準解像度からは一様スケールだけを
// 取り出し、キャンバス矩形そのものは画面のアスペクトへ伸ばす** — レターボックス (黒帯) は
// 作らないので、端アンカーの UI は常に本当の画面端まで届く。
//
//   s       = min(w / 1920, h / 1080)
//   canvasW = w / s        canvasH = h / s
//
// ★この式の効きどころ: s が min なので **キャンバス寸法はアスペクト比だけの関数**になり
//   画素数に依らない。16:9 なら 960x540 でも 1600x900 でも 4K でもキャンバスは厳密に
//   1920x1080 = 既存の golden も CI も 1 ビットも動かない。可変になるのは非 16:9 のときだけ。
//
// ★キャンバス寸法を **int に丸める**のは Resolve* の引数型を変えないため (UISelfTest の
//   40 検査が全書き換えになるのを避けた)。丸めの誤差は最大 0.5 px で、しかも描画と
//   ヒットテストは同じ整数を通るので**両者がズレることは無い** — 損をするのは
//   「右端/下端の 0.5 px にだけ UI が届かない (or 半 px はみ出す)」ことだけ。
//   非 16:9 かつ端数が出る解像度 (1366x768 → canvas 1920.94x1080) でしか効かない。
//
// ★モードは Expand (min) / Shrink (max) / Match Width Or Height (M75c)。正本は desc を取る版で、
//   引数無し版は既定キャンバス (DefaultCanvasDesc = project_settings の基準解像度 + Expand)。
//   Expand の式には golden 4 枚の不変が掛かっているので、1 ビットも変えないこと。
// ★Match の pow / log は CRT を呼ばず、UILayout.cpp の double の級数で解く — UCRT の
//   数学関数は CPU (FMA3 の有無) で経路が変わり、2 台のヒットテストが割れうるため。
//   m <= 0 / m >= 1 / sx == sy (基準と同じアスペクト) はべき乗を通さず sx / sy をそのまま使う
CanvasInfo CanvasSize(int screenW, int screenH, const CanvasDesc& desc);
CanvasInfo CanvasSize(int screenW, int screenH);

// 既定キャンバスの基準解像度 (M75c)。**起動時に 1 回だけ** EngineLoop が project_settings.json
// から書く静的な値で、sim 中は変えない (.rep に載らない = 途中で変えると同じ記録の再生が割れる。
// actions.json と同じ扱い)。<= 0 は kCanvasRefW/H へ倒す
void SetDefaultCanvasReference(int referenceW, int referenceH);
const CanvasDesc& DefaultCanvasDesc();

// ---- 複数キャンバス (M75c) ----
// e が属するキャンバス = 自分を含む最寄りの UICanvas 祖先。無ければ kNullEntity (= 既定キャンバス)。
// 入れ子は非対応: 最寄りが勝ち、その上の階層とは座標系もクリップも切れる
EntityID FindCanvas(World& world, EntityID e);

// キャンバスエンティティ (FindCanvas の戻り値) の寸法と、そのキャンバス単位 → **既定キャンバス単位**
// の倍率 (scale)。defaultW/H は既定キャンバスの寸法 (Resolve* に渡している値)。
// ★明示キャンバスは**既定キャンバスを仮想の画面として**解く。3 モードとも s は画面寸法の 1 次
//   同次式なので、実画面から直接解いた s_c と「既定キャンバスで解いた s' × 既定の s」は数学的に
//   一致する。こうすると sim 側 (HitTest / FocusNav / ABI) は既定キャンバス座標だけを共通語に
//   でき、Resolve* の引数も変わらない (実 px は描画側が最後に既定の scale を掛けるだけ)。
// canvas == kNullEntity、または既定と同じ解き方 (基準解像度が既定 + Expand) の Canvas は
// {1.0f, defaultW, defaultH} を返す = Canvas の無い UI と同ビット
CanvasInfo CanvasOfEntity(World& world, EntityID canvas, int defaultW, int defaultH);
inline CanvasInfo CanvasOf(World& world, EntityID e, int defaultW, int defaultH)
{
    return CanvasOfEntity(world, FindCanvas(world, e), defaultW, defaultH);
}
// 描画/ヒットの第 1 キー。既定キャンバスは 0
int32_t CanvasSortOrder(World& world, EntityID canvas);

// ---- ゲーム面 → キャンバス (M75b) ----
// レーン 0 の入力に記録されたゲーム面 (surfW/H) から既定キャンバスを解く。**sim レーンの UI
// (HitTest / FocusNav / ABI の GetUIRect・UIHitTest・MouseCanvasPos) はすべてここを通る** —
// 面の寸法 → キャンバスの式を読み手ごとに書くと、「0 なら基準解像度」の倒し方が
// 1 箇所だけ食い違う。surfW/H <= 0 (ヘッドレス / 未確定) は CanvasSize の退化扱い
// (= 基準解像度 + scale 1) に倒れる
CanvasInfo CanvasOfInput(const InputSnapshot& in);

// ゲーム面 px → キャンバス座標。`float(px) / scale` の 1 回の除算 (UISelfTest が memcmp で固定)
inline float SurfaceToCanvas(float surfPx, const CanvasInfo& canvas)
{
    return surfPx / canvas.scale;
}

// ---- RectTransform (M75a) ----
// 9-grid anchor (0..8、M51e の UIElement.anchor) を anchorMin/anchorMax の 0..1 へ写す。
// 旧式 AnchorOrigin は base.x + {0, base.w*0.5f, base.w} だったので、ここで 0/0.5/1 を
// 掛けても **同じ float** になる (0*w = 0、0.5f*w、1*w = w)。これが旧シーンの矩形が
// 1 ビットも動かない根拠
inline void AnchorPreset(int anchor, float& outAx, float& outAy)
{
    const int col = anchor % 3; // 0=左 1=中 2=右
    const int row = anchor / 3; // 0=上 1=中 2=下
    outAx = (col == 0) ? 0.0f : (col == 1) ? 0.5f : 1.0f;
    outAy = (row == 0) ? 0.0f : (row == 1) ? 0.5f : 1.0f;
}

// 旧 UIElement (anchor / x / y / w / h / space) → RectTransform。**変換の正本はこの 1 本**
// (SceneSerializer のロード時変換 / ABI の SetUIRect・SetUILayout / DemoContent / 自己検査が
// 全部ここを通る)。pivot は (0,0) = 「矩形の左上をアンカー点 + オフセットに置く」旧意味論。
// hasUiAncestor: space==0 (画面基準) の要素に UI 祖先がいるなら basis=1 (キャンバス) で
// 旧挙動を保ち、いなければ basis=0 (親 = 無いのでキャンバス。Unity 風の既定に寄せる)
RectTransformComponent FromLegacyRect(int anchor, float x, float y, float w, float h, int space,
                                      bool hasUiAncestor);

// RectTransform を base 矩形 (親の未回転矩形 or キャンバス) の上で解く純関数。scale は
// 距離スケールの伝播係数 (オフセットとサイズに掛かる。screen UI は 1.0f)。
//   w = base.w * (anchorMax.x - anchorMin.x) + sizeDelta.x * scale
//   x = (base.x + base.w * anchorMin.x) + anchoredPosition.x * scale - pivot.x * (sizeDelta.x * scale)
// ★**加算順を変えないこと** — 一致アンカー・pivot 0 では旧式 (AnchorOrigin + オフセット) と
//   恒等演算 (+0 / *1 / -0) しか違わず、golden 4 枚の不変はこの順序に掛かっている
UIRect RectFromTransform(const RectTransformComponent& rt, const UIRect& base, float scale);

// 2x3 アフィン (M75a: rotation / scale)。p' = (a*x + c*y + tx, b*x + d*y + ty)。
// 描画側は 4 頂点に、ヒットテストは点を逆変換してローカル軸平行矩形で判定する。
// rotation == 0 && scale == (1,1) の要素 (と祖先) は **hasXform=false で一切通らない**
// (恒等ゲート) — 既存経路の数値には 1 ビットも触れない
struct UIXform {
    float a = 1.0f, b = 0.0f, c = 0.0f, d = 1.0f, tx = 0.0f, ty = 0.0f;
};
inline void XformPoint(const UIXform& m, float x, float y, float& ox, float& oy)
{
    ox = m.a * x + m.c * y + m.tx;
    oy = m.b * x + m.d * y + m.ty;
}
// 逆行列。退化 (scale 0) は false で out は恒等
bool InvertXform(const UIXform& m, UIXform& out);
// 矩形の 4 隅を変換した AABB (回転要素のシザー / アウトライン / ナビ用の近似)
UIRect XformAabb(const UIXform& m, const UIRect& r);

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
// ビット恒等なので screen UI の矩形は scale を掛けない式と完全一致する。
struct UIResolved {
    UIRect rect;         // 未回転の矩形 (親の未回転フレーム上)。回転/スケールは xform が持つ
    float scale = 1.0f;
    bool visible = true; // false = カメラ背面 (クランプ OFF) / コンテキスト無しの world 要素
    bool hasXform = false; // M75a: 自分か祖先に回転/スケールがある (恒等ゲート)
    UIXform xform;         // rect のフレーム → キャンバス座標 (hasXform のときだけ意味を持つ)
};

// e の RectTransform (無ければ UIElement 用の既定値) を screen px 矩形に解決する (正本)。
// basis=0 は最寄りの UI 祖先 (RectTransform / UIElement / UICanvas 持ち) の解決済み矩形基準。祖先が
// 無い / basis=1 はワールド追従判定 (冒頭コメント: UI 専用でないオブジェクト上の
// UIElement は自エンティティの射影点基準) → 該当しなければ screen 基準。
// ★M75c: 戻る矩形は **e が属するキャンバスの単位**。screenW/H は既定キャンバスの寸法のまま。
//   UICanvas を持つ要素自身は常に (0,0,cw,ch)、basis=1 は属するキャンバスの全面が基準。
//   既定キャンバス単位へは CanvasOf(...).scale を掛ける (Canvas の無い要素は 1.0f = 恒等)。
// 壊れ親/循環は深度上限で打ち切り安全。UIElement も RectTransform も無ければ visible=false。
// ★M75e: 親が UILayoutGroup なら RectTransform の代わりに Group の配置結果で、自分に
//   UIContentSizeFitter があれば中身に合わせた大きさで解く (UILayoutGroup.h)。どちらも無い要素は
//   RectFromTransform だけを通る。scratch は結果を変えないメモ — 同じ World を何度も
//   解く呼び出し単位 (描画 1 フレーム / HitTest 1 回) で 1 つ作って渡す。nullptr なら内部で作る
UIResolved Resolve(World& world, EntityID e, int screenW, int screenH,
                   const UIWorldContext* wc, LayoutScratch* scratch = nullptr);

// sim レーン用の決定論カメラ構築 — RenderSystem と同じ選択規則 (走査順の先頭、isPrimary 優先)
// で scalar 演算のみ (SIMD 禁止 = Debug/Release ビット一致)。WorldMatrix は tick 内で
// TransformSystem が更新済み (RaycastWorld と同じ前例)。カメラ不在は false。
// aspect は screenW/screenH。**ここへ渡すのはキャンバス寸法** — キャンバスは
// 実画面と同じアスペクトなので sim (ヒットテスト) と描画で射影が一致する
// (1920x1080 固定を渡すと、非 16:9 で sim と描画の射影が横方向にずれる)。
bool BuildSimWorldContext(World& world, int screenW, int screenH, UIWorldContext& out);

// 互換ラッパ: Resolve().rect (visible=false は {0,0,0,0} = 「隠れている」表現に合流)。
// 回転/スケールのある要素は変換後の **AABB** (ナビ / クリップ / GameView / ABI GetUIRect が読む)
UIRect ResolveRect(World& world, EntityID e, int screenW, int screenH,
                   const UIWorldContext* wc = nullptr, LayoutScratch* scratch = nullptr);

// e に UI ノード (RectTransform / UIElement 持ち) の祖先がいるか。SceneSerializer が旧形式の
// 変換で basis を確定するときと、Inspector の表示に使う
bool HasUiAncestor(World& world, EntityID e);

// 「UI 専用オブジェクト」か (ワールド追従の自動判定。UILayout.cpp 冒頭)。
// 許容 = 基本 4 種 + 帳簿 (FileId/Active/Prefab*) + kComponentScriptState + **kComponentUiAux**
bool IsUiOnlyEntity(World& world, EntityID e);

// e の祖先の clipChildren 矩形をすべて交差した「見えてよい範囲」。クリップ祖先が
// 無ければ screen 全域。e 自身の clipChildren は含まない (自分は切らない)。
// M75c: 単位は e のキャンバス。祖先を辿るのは**属するキャンバスまで** (その上は別の座標系)
UIRect ResolveClipRect(World& world, EntityID e, int screenW, int screenH,
                       const UIWorldContext* wc = nullptr, LayoutScratch* scratch = nullptr);

// 便利形: 要素の可視矩形 = ResolveRect ∩ ResolveClipRect (完全に隠れていれば w/h<=0)
UIRect ResolveVisibleRect(World& world, EntityID e, int screenW, int screenH,
                          const UIWorldContext* wc = nullptr, LayoutScratch* scratch = nullptr);

} // namespace uilayout
} // namespace mye
