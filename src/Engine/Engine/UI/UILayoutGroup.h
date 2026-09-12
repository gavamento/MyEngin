//====================================================================================
//                          UILayoutGroup.h
//  MyEngine/ 秋田蓮音                                                      09/13/2026
//                                          UI の自動レイアウト（Layout Group / LayoutElement / ContentSizeFitter）
//====================================================================================
#pragma once
// 自動レイアウト (M75e)。Unity uGUI の HorizontalOrVerticalLayoutGroup / GridLayoutGroup /
// LayoutUtility / ContentSizeFitter を**純関数**として移植したもの (com.unity.ugui の
// Runtime/UGUI/UI/Core/Layout/*.cs と式・加算順を揃えてある)。
//
// ★Unity は配置結果を子の RectTransform へ**書き込む** (DrivenRectTransformTracker) が、ここでは
//   書かない。uilayout::Resolve が子の矩形を解くときに「親が Layout Group なら RectTransform の
//   代わりにこの配置結果を使う」だけ。書き込み方式を採らなかったのは、駆動値を保存しない印 /
//   Inspector の read-only / Undo との干渉 / 非 Play 中に誰が回すか、の 4 つを抱えるため (計画 E)。
//   描画 (フレーム) とヒットテスト (tick) が同じ純関数を通るので、配置が食い違うことも無い。
// ★LayoutScratch は**結果を変えないメモ**。呼び出し単位 (描画 1 フレーム / HitTest 1 回) で作って
//   捨てる。エンティティ単位で覚えているだけで World の変更を検知しないので、World を書き換えた後に
//   使い回さないこと。nullptr を渡す単発の呼び出しは Resolve が内部で作る。
// ★sim レーン (HitTest / FocusNav / ABI) からも呼ばれるので、scalar の四則演算と floor / ceil / sqrt
//   (どれも IEEE-754 で正確) だけで組む。unordered も使わない (メモは entity.index の添字表)。
//
// Unity との違い (意図したもの):
//   - 何も指定の無い要素の preferred は **sizeDelta** (Unity は 0)。min と flexible は Unity と同じ 0。
//     「Group に入れたら素のパネルが幅 0 に潰れる」を避け、作者が置いた大きさを希望値として扱う
//   - max サイズ / FitMode.Clamped / childScaleWidth・Height は無い (機能集合は Unity 2019 系。
//     max が無いときの式は現行の Unity と同値)
//   - 制御されない軸の子の大きさは、子に ContentSizeFitter があればその値 (Unity は sizeDelta を読み、
//     Fitter が後から書き換えて次のリビルドで同じ値に収束する。純関数では収束後の値を直接出す)
//   - テキスト (UIElement kind 1) の preferred は uitext::Measure (コミットされた計測表)。
//     ボタン (kind 2) のラベルは測らない (Unity の Button も背景の大きさで、ラベルは子の Text)
//   - 兄弟順は Hierarchy の firstChild → nextSibling (= Hierarchy 窓の順 = 保存順)。SimSnapshot には
//     載るがワールドハッシュには載らない (UIElement の幾何と同じ「authored な NoHash 入力」のクラス)
#include <cstdint>
#include <vector>

#include "Engine/Core/EntityID.h"
#include "Engine/Engine/UI/UILayout.h"

namespace mye {

class World;
struct RectTransformComponent;

namespace uilayout {

// UILayoutGroupComponent.kind
inline constexpr int kLayoutHorizontal = 0;
inline constexpr int kLayoutVertical = 1;
inline constexpr int kLayoutGrid = 2;

// UILayoutGroupComponent.constraint (Unity の GridLayoutGroup.Constraint と同じ並び)
inline constexpr int kGridFlexible = 0;
inline constexpr int kGridFixedColumnCount = 1;
inline constexpr int kGridFixedRowCount = 2;

// UIContentSizeFitterComponent.horizontalFit / verticalFit (Unity の FitMode と同じ並び。Clamped は無い)
inline constexpr int kFitUnconstrained = 0;
inline constexpr int kFitMinSize = 1;
inline constexpr int kFitPreferred = 2;

// 1 軸ぶんのレイアウト入力 (Unity の ILayoutElement の min / preferred / flexible)。
// 単位は**作者の数値のまま** (距離スケールを掛ける前)。preferred は常に min 以上
struct LayoutSizes {
    float min = 0.0f;
    float preferred = 0.0f;
    float flexible = 0.0f;
};

// 呼び出し単位のメモ。中身は UILayoutGroup.cpp の内部表現 — 外から触らない
class LayoutScratch {
public:
    struct Entry {
        EntityID e;
        uint8_t hasWidth = 0;
        uint8_t hasHeight = 0;
        uint8_t hasTotalsW = 0;
        uint8_t hasTotalsH = 0;
        uint8_t hasArrange = 0;
        LayoutSizes width;         // LayoutInputWidth
        float heightFor = 0.0f;    // height を測ったときの幅
        LayoutSizes height;        // LayoutInputHeight
        LayoutSizes totalsW;       // Group 自身の集計 (幅)
        float totalsHFor = 0.0f;
        LayoutSizes totalsH;       // Group 自身の集計 (高さ。幅の関数)
        float arrangeW = 0.0f;     // 子を並べたときの自分の大きさ
        float arrangeH = 0.0f;
        uint32_t placedFirst = 0;  // placed の区間
        uint32_t placedCount = 0;
        EntityID placedBy;         // 子として並べられた親 (見つけ直しの近道)
        uint32_t placedAt = 0;
    };
    struct Placed {
        EntityID e;
        float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f; // 親の左上基準、作者単位
    };
    std::vector<int32_t> slotOfIndex; // entity.index → entries の添字 (-1 = 無し)
    std::vector<Entry> entries;
    std::vector<Placed> placed;
};

// child が group に並べられる子か (Unity の rectChildren の条件 + このエンジンの規則):
// group が UILayoutGroup を持つ UI ノード / child の**直属の**親が group / child が RectTransform か
// UIElement を持つ / Canvas でない / basis=0 (basis=1 はキャンバス基準へ抜ける宣言) /
// LayoutElement.ignoreLayout でない / Active (祖先込み)
bool IsLayoutChild(World& world, EntityID group, EntityID child);

// Unity の LayoutUtility.GetMinSize / GetPreferredSize / GetFlexibleSize を 1 軸まとめて。
// 高さは幅の関数 (折り返すテキスト / 子の幅で行数が変わる Group) なので幅を渡す
LayoutSizes LayoutInputWidth(World& world, EntityID e, LayoutScratch& scratch);
LayoutSizes LayoutInputHeight(World& world, EntityID e, float width, LayoutScratch& scratch);

// group が child を並べた結果の矩形 (未回転、group と同じ単位)。groupRect は group の解決済み矩形、
// scale は距離スケールの伝播係数 (配置は作者単位で解いてから掛ける。screen UI は 1.0f)。
// child が並べられる子でなければ false (out は触らない) = 呼び出し側は RectTransform で解く
bool ResolveLayoutChild(World& world, EntityID group, EntityID child, const UIRect& groupRect,
                        float scale, LayoutScratch& scratch, UIRect& out);

// e の ContentSizeFitter を rt の sizeDelta へ当てたコピーを outRt に書く (Unity の
// SetSizeWithCurrentAnchors と同じ式)。base は親の矩形。Fitter が無い / 両軸 Unconstrained なら false
bool ApplyContentSizeFitter(World& world, EntityID e, const RectTransformComponent& rt,
                            const UIRect& base, float scale, LayoutScratch& scratch,
                            RectTransformComponent& outRt);

// e の RectTransform のどこが自動レイアウトに上書きされているか (Inspector の表示 / M75i の Rect Tool)
inline constexpr uint32_t kDrivenByGroup = 1u << 0;  // 親の Layout Group が位置を決めている
inline constexpr uint32_t kDrivenWidth = 1u << 1;    // 幅が上書きされている
inline constexpr uint32_t kDrivenHeight = 1u << 2;   // 高さが上書きされている
inline constexpr uint32_t kDrivenByFitter = 1u << 3; // 上書きの一部は自分の ContentSizeFitter
uint32_t LayoutDrivenBits(World& world, EntityID e);

} // namespace uilayout
} // namespace mye
