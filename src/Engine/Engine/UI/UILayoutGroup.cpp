//====================================================================================
//                          UILayoutGroup.cpp
//  MyEngine/ 秋田蓮音                                                      09/13/2026
//                                          UI の自動レイアウトの実装（Unity uGUI の配置式の移植）
//====================================================================================
#include "Engine/Engine/UI/UILayoutGroup.h"

#include <climits>
#include <cmath>

#include "Engine/Core/Components.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/UI/UIWidgets.h" // M75f: LayoutDrivenBits の Slider 駆動
#include "Engine/Engine/UI/UITextMetrics.h"

namespace mye {
namespace uilayout {
namespace {

// 入れ子の Group を辿る深さの上限 (壊れたデータでも止まる)。正常な UI 階層はこれより浅い
constexpr int kMaxLayoutDepth = 64;
// 1 つの Group の子を辿る上限 (兄弟リンクが壊れて環になっていても止まる)
constexpr uint32_t kMaxLayoutChildren = 1u << 20;

// ---- Unity の Mathf と同じ式 (比較の向きと加算順まで揃える) ----
float MaxF(float a, float b)
{
    return (a > b) ? a : b;
}

float Clamp01(float v)
{
    return (v < 0.0f) ? 0.0f : (v > 1.0f) ? 1.0f : v;
}

float ClampF(float v, float lo, float hi)
{
    if (v < lo) {
        v = lo;
    } else if (v > hi) {
        v = hi;
    }
    return v;
}

int ClampI(int v, int lo, int hi)
{
    if (v < lo) {
        v = lo;
    } else if (v > hi) {
        v = hi;
    }
    return v;
}

float LerpF(float a, float b, float t)
{
    return a + (b - a) * Clamp01(t);
}

// float → int の飽和変換。巨大値 / NaN を int へ直接 cast すると未定義動作なので端へ倒す
// (C# の (int) は未定義にならないが、Unity の実用域 = 子の数程度の値では同じ結果)
int ToIntSat(float v)
{
    if (!(v < 2147483647.0f)) {
        return INT_MAX; // NaN もここ
    }
    if (v < -2147483648.0f) {
        return INT_MIN;
    }
    return static_cast<int>(v);
}

int CeilToInt(float v)
{
    return ToIntSat(std::ceil(v));
}

int FloorToInt(float v)
{
    return ToIntSat(std::floor(v));
}

// メモの行。**戻り値は添字** — 参照を返すと、再帰の途中の push_back で無効になる
int32_t SlotOf(LayoutScratch& s, EntityID e)
{
    if (e.index >= s.slotOfIndex.size()) {
        // index は World のスロット番号 (詰まった小さな整数) なので表はエンティティ数程度に収まる
        s.slotOfIndex.resize(static_cast<size_t>(e.index) + 1, -1);
    }
    int32_t slot = s.slotOfIndex[e.index];
    if (slot >= 0 && s.entries[static_cast<size_t>(slot)].e == e) {
        return slot;
    }
    if (slot < 0) {
        slot = static_cast<int32_t>(s.entries.size());
        s.entries.emplace_back();
        s.slotOfIndex[e.index] = slot;
    }
    // 同じ index の別世代 (破棄 → 再利用) は覚えていた値を捨てる
    s.entries[static_cast<size_t>(slot)] = LayoutScratch::Entry{};
    s.entries[static_cast<size_t>(slot)].e = e;
    return slot;
}

LayoutScratch::Entry& EntryAt(LayoutScratch& s, int32_t slot)
{
    return s.entries[static_cast<size_t>(slot)];
}

bool IsUiNodeForLayout(World& world, EntityID e)
{
    return world.GetComponent<RectTransformComponent>(e) != nullptr
        || world.GetComponent<UIElementComponent>(e) != nullptr
        || world.GetComponent<UICanvasComponent>(e) != nullptr;
}

// 作者の置いた sizeDelta (RectTransform 無しの UIElement は既定値 = Resolve と同じ扱い)
float RawSizeDelta(World& world, EntityID e, int axis)
{
    static const RectTransformComponent kDefaultRt = {};
    const auto* rt = world.GetComponent<RectTransformComponent>(e);
    const RectTransformComponent& r = rt ? *rt : kDefaultRt;
    return (axis == 0) ? r.sizeDelta.x : r.sizeDelta.y;
}

// ---- LayoutGroup.cs の共通部 ----
// padding は (左, 上, 右, 下)。Unity の padding.horizontal = left + right
float PaddingSum(const UILayoutGroupComponent& g, int axis)
{
    return (axis == 0) ? (g.padding.x + g.padding.z) : (g.padding.y + g.padding.w);
}

float PaddingStart(const UILayoutGroupComponent& g, int axis)
{
    return (axis == 0) ? g.padding.x : g.padding.y;
}

float AlignmentOnAxis(const UILayoutGroupComponent& g, int axis)
{
    const int a = ClampI(g.childAlignment, 0, 8);
    return (axis == 0) ? static_cast<float>(a % 3) * 0.5f : static_cast<float>(a / 3) * 0.5f;
}

// GetStartOffset。available は Group 自身の大きさ (rectTransform.rect.size[axis])
float StartOffset(const UILayoutGroupComponent& g, int axis, float available,
                  float requiredSpaceWithoutPadding)
{
    const float requiredSpace = requiredSpaceWithoutPadding + PaddingSum(g, axis);
    const float surplusSpace = available - requiredSpace;
    return PaddingStart(g, axis) + surplusSpace * AlignmentOnAxis(g, axis);
}

// rectChildren (兄弟順)
void CollectLayoutChildren(World& world, EntityID group, std::vector<EntityID>& out)
{
    out.clear();
    const auto* h = world.GetComponent<HierarchyComponent>(group);
    EntityID c = h ? h->firstChild : kNullEntity;
    for (uint32_t guard = 0; guard < kMaxLayoutChildren && !c.IsNull(); ++guard) {
        if (IsLayoutChild(world, group, c)) {
            out.push_back(c);
        }
        const auto* ch = world.GetComponent<HierarchyComponent>(c);
        c = ch ? ch->nextSibling : kNullEntity;
    }
}

LayoutSizes InputAxis(World& world, EntityID e, int axis, float width, LayoutScratch& s, int depth);

// 子の大きさ = ContentSizeFitter があればその値、無ければ sizeDelta。Group に制御されない軸で使う
float OwnSize(World& world, EntityID e, int axis, float width, LayoutScratch& s, int depth)
{
    const auto* fit = world.GetComponent<UIContentSizeFitterComponent>(e);
    const int mode = fit ? ((axis == 0) ? fit->horizontalFit : fit->verticalFit) : kFitUnconstrained;
    if (mode == kFitMinSize || mode == kFitPreferred) {
        const LayoutSizes sz = InputAxis(world, e, axis, width, s, depth);
        return (mode == kFitMinSize) ? sz.min : sz.preferred;
    }
    return RawSizeDelta(world, e, axis);
}

// HorizontalOrVerticalLayoutGroup.GetChildSizes
LayoutSizes ChildSizes(World& world, EntityID child, int axis, bool controlSize, bool forceExpand,
                       float width, LayoutScratch& s, int depth)
{
    LayoutSizes out;
    if (!controlSize) {
        out.min = OwnSize(world, child, axis, width, s, depth);
        out.preferred = out.min;
        out.flexible = 0.0f;
    } else {
        out = InputAxis(world, child, axis, width, s, depth);
    }
    if (forceExpand) {
        out.flexible = MaxF(out.flexible, 1.0f);
    }
    return out;
}

void ChildSizesAlong(World& world, const UILayoutGroupComponent& g,
                     const std::vector<EntityID>& children, int axis,
                     const std::vector<float>* widths, LayoutScratch& s, int depth,
                     std::vector<LayoutSizes>& out)
{
    const bool control = (axis == 0) ? g.controlChildWidth != 0 : g.controlChildHeight != 0;
    const bool expand = (axis == 0) ? g.forceExpandWidth != 0 : g.forceExpandHeight != 0;
    out.resize(children.size());
    for (size_t i = 0; i < children.size(); ++i) {
        out[i] = ChildSizes(world, children[i], axis, control, expand,
                            widths ? (*widths)[i] : 0.0f, s, depth);
    }
}

// HorizontalOrVerticalLayoutGroup.CalcAlongAxis (max サイズ無し = totalMax が +inf の場合と同値)
LayoutSizes CalcAlongAxis(const UILayoutGroupComponent& g, int axis,
                          const std::vector<LayoutSizes>& sizes)
{
    const bool isVertical = g.kind == kLayoutVertical;
    const float combinedPadding = PaddingSum(g, axis);
    const float spacing = isVertical ? g.spacing.y : g.spacing.x; // 並べる軸の間隔
    const bool alongOtherAxis = isVertical ^ (axis == 1);
    float totalMin = combinedPadding;
    float totalPreferred = combinedPadding;
    float totalFlexible = 0.0f;
    for (const LayoutSizes& c : sizes) {
        if (alongOtherAxis) {
            totalMin = MaxF(c.min + combinedPadding, totalMin);
            totalPreferred = MaxF(c.preferred + combinedPadding, totalPreferred);
            totalFlexible = MaxF(c.flexible, totalFlexible);
        } else {
            totalMin += c.min + spacing;
            totalPreferred += c.preferred + spacing;
            totalFlexible += c.flexible;
        }
    }
    if (!alongOtherAxis && !sizes.empty()) {
        totalMin -= spacing;
        totalPreferred -= spacing;
    }
    // Mathf.Clamp(totalPreferred, totalMin, +inf)
    if (totalPreferred < totalMin) {
        totalPreferred = totalMin;
    }
    return { totalMin, totalPreferred, totalFlexible };
}

// HorizontalOrVerticalLayoutGroup.SetChildrenAlongAxis。pos / len は子の並び (兄弟順) の添字で返す。
// 制御しない軸の子の大きさは sizes[i].min (= GetChildSizes が入れた sizeDelta)
void SetChildrenAlongAxis(const UILayoutGroupComponent& g, int axis, float size,
                          const LayoutSizes& totals, const std::vector<LayoutSizes>& sizes,
                          std::vector<float>& pos, std::vector<float>& len)
{
    const bool isVertical = g.kind == kLayoutVertical;
    const bool controlSize = (axis == 0) ? g.controlChildWidth != 0 : g.controlChildHeight != 0;
    const float spacing = isVertical ? g.spacing.y : g.spacing.x;
    const float alignmentOnAxis = AlignmentOnAxis(g, axis);
    const bool alongOtherAxis = isVertical ^ (axis == 1);
    const bool reverse = g.reverseArrangement != 0;
    const size_t count = sizes.size();
    pos.assign(count, 0.0f);
    len.assign(count, 0.0f);
    if (alongOtherAxis) {
        const float innerSize = size - PaddingSum(g, axis);
        for (size_t i = 0; i < count; ++i) { // 子ごとに独立なので逆順でも結果は同じ
            const LayoutSizes& c = sizes[i];
            const float requiredSpace =
                ClampF(innerSize, c.min, (c.flexible > 0.0f) ? size : c.preferred);
            const float startOffset = StartOffset(g, axis, size, requiredSpace);
            if (controlSize) {
                pos[i] = startOffset;
                len[i] = requiredSpace;
            } else {
                const float offsetInCell = (requiredSpace - c.min) * alignmentOnAxis;
                pos[i] = startOffset + offsetInCell;
                len[i] = c.min;
            }
        }
        return;
    }
    float p = PaddingStart(g, axis);
    float itemFlexibleMultiplier = 0.0f;
    const float surplusSpace = size - totals.preferred;
    if (surplusSpace > 0.0f) {
        if (totals.flexible == 0.0f) {
            p = StartOffset(g, axis, size, totals.preferred - PaddingSum(g, axis));
        } else if (totals.flexible > 0.0f) {
            itemFlexibleMultiplier = surplusSpace / totals.flexible;
        }
    }
    float minMaxLerp = 0.0f;
    if (totals.min != totals.preferred) {
        minMaxLerp = Clamp01((size - totals.min) / (totals.preferred - totals.min));
    }
    for (size_t k = 0; k < count; ++k) {
        const size_t i = reverse ? count - 1 - k : k;
        const LayoutSizes& c = sizes[i];
        float childSize = LerpF(c.min, c.preferred, minMaxLerp);
        childSize += c.flexible * itemFlexibleMultiplier;
        if (controlSize) {
            pos[i] = p;
            len[i] = childSize;
        } else {
            const float offsetInCell = (childSize - c.min) * alignmentOnAxis;
            pos[i] = p + offsetInCell;
            len[i] = c.min;
        }
        p += childSize + spacing;
    }
}

// ---- GridLayoutGroup.cs ----
// CalculateLayoutInputHorizontal / Vertical。flexible は -1 (= 未指定。Unity と同じ)
LayoutSizes GridTotals(const UILayoutGroupComponent& g, int axis, int count, float width)
{
    const int constraintCount = (g.constraintCount < 1) ? 1 : g.constraintCount;
    LayoutSizes t;
    t.flexible = -1.0f;
    if (axis == 0) {
        const float cellWidthWithSpacing = g.cellSize.x + g.spacing.x;
        if (g.constraint == kGridFixedColumnCount) {
            t.min = PaddingSum(g, 0) + cellWidthWithSpacing * static_cast<float>(constraintCount)
                - g.spacing.x;
            t.preferred = t.min;
        } else if (g.constraint == kGridFixedRowCount) {
            const int columns = CeilToInt(
                static_cast<float>(count) / static_cast<float>(constraintCount) - 0.001f);
            t.min = PaddingSum(g, 0) + cellWidthWithSpacing * static_cast<float>(columns)
                - g.spacing.x;
            t.preferred = t.min;
        } else {
            t.min = PaddingSum(g, 0) + cellWidthWithSpacing - g.spacing.x;
            const int preferredColumnCount = CeilToInt(std::sqrt(static_cast<float>(count)));
            t.preferred = PaddingSum(g, 0)
                + cellWidthWithSpacing * static_cast<float>(preferredColumnCount) - g.spacing.x;
        }
        return t;
    }
    const float cellHeightWithSpacing = g.cellSize.y + g.spacing.y;
    if (g.constraint == kGridFixedColumnCount) {
        const int rows =
            CeilToInt(static_cast<float>(count) / static_cast<float>(constraintCount) - 0.001f);
        t.min = PaddingSum(g, 1) + cellHeightWithSpacing * static_cast<float>(rows) - g.spacing.y;
        t.preferred = t.min;
    } else if (g.constraint == kGridFixedRowCount) {
        t.min = PaddingSum(g, 1) + cellHeightWithSpacing * static_cast<float>(constraintCount)
            - g.spacing.y;
        t.preferred = t.min;
    } else {
        t.min = PaddingSum(g, 1) + cellHeightWithSpacing - g.spacing.y;
        const float usableWidth = width - PaddingSum(g, 0) + g.spacing.x + 0.001f;
        const float cellWidthWithSpacing = g.cellSize.x + g.spacing.x;
        // ★Unity はここで 0 除算を守っていない (SetCellsAlongAxis の側は守る)。同じ守り方に揃える
        int cellCountX = INT_MAX;
        if (cellWidthWithSpacing > 0.0f) {
            const int fit = FloorToInt(usableWidth / cellWidthWithSpacing);
            cellCountX = (fit > 1) ? fit : 1; // Mathf.Max(1, ...)
        }
        const int rowCount =
            CeilToInt(static_cast<float>(count) / static_cast<float>(cellCountX));
        t.preferred =
            PaddingSum(g, 1) + cellHeightWithSpacing * static_cast<float>(rowCount) - g.spacing.y;
    }
    return t;
}

// SetCellsAlongAxis (axis 1 の後半 = 位置を決める部分)。サイズは常に cellSize
void ArrangeGrid(const UILayoutGroupComponent& g, int count, float width, float height,
                 std::vector<float>& px, std::vector<float>& py)
{
    px.assign(static_cast<size_t>(count), 0.0f);
    py.assign(static_cast<size_t>(count), 0.0f);
    if (count <= 0) {
        return;
    }
    const int constraintCount = (g.constraintCount < 1) ? 1 : g.constraintCount;
    int cellCountX = 1;
    int cellCountY = 1;
    if (g.constraint == kGridFixedColumnCount) {
        cellCountX = constraintCount;
        if (count > cellCountX) {
            cellCountY = count / cellCountX + (count % cellCountX > 0 ? 1 : 0);
        }
    } else if (g.constraint == kGridFixedRowCount) {
        cellCountY = constraintCount;
        if (count > cellCountY) {
            cellCountX = count / cellCountY + (count % cellCountY > 0 ? 1 : 0);
        }
    } else {
        if (g.cellSize.x + g.spacing.x <= 0.0f) {
            cellCountX = INT_MAX;
        } else {
            const int n = FloorToInt((width - PaddingSum(g, 0) + g.spacing.x + 0.001f)
                                     / (g.cellSize.x + g.spacing.x));
            cellCountX = (n > 1) ? n : 1;
        }
        if (g.cellSize.y + g.spacing.y <= 0.0f) {
            cellCountY = INT_MAX;
        } else {
            const int n = FloorToInt((height - PaddingSum(g, 1) + g.spacing.y + 0.001f)
                                     / (g.cellSize.y + g.spacing.y));
            cellCountY = (n > 1) ? n : 1;
        }
    }

    const int corner = ClampI(g.startCorner, 0, 3);
    const int cornerX = corner % 2;
    const int cornerY = corner / 2;
    const bool horizontalFirst = g.startAxis != 1;

    int cellsPerMainAxis = 0;
    int actualCellCountX = 0;
    int actualCellCountY = 0;
    if (horizontalFirst) {
        cellsPerMainAxis = cellCountX;
        actualCellCountX = ClampI(cellCountX, 1, count);
        if (g.constraint == kGridFixedRowCount) {
            actualCellCountY = (cellCountY < count) ? cellCountY : count;
        } else {
            actualCellCountY = ClampI(
                cellCountY, 1,
                CeilToInt(static_cast<float>(count) / static_cast<float>(cellsPerMainAxis)));
        }
    } else {
        cellsPerMainAxis = cellCountY;
        actualCellCountY = ClampI(cellCountY, 1, count);
        if (g.constraint == kGridFixedColumnCount) {
            actualCellCountX = (cellCountX < count) ? cellCountX : count;
        } else {
            actualCellCountX = ClampI(
                cellCountX, 1,
                CeilToInt(static_cast<float>(count) / static_cast<float>(cellsPerMainAxis)));
        }
    }

    const float requiredX = static_cast<float>(actualCellCountX) * g.cellSize.x
        + static_cast<float>(actualCellCountX - 1) * g.spacing.x;
    const float requiredY = static_cast<float>(actualCellCountY) * g.cellSize.y
        + static_cast<float>(actualCellCountY - 1) * g.spacing.y;
    const float startX = StartOffset(g, 0, width, requiredX);
    const float startY = StartOffset(g, 1, height, requiredY);

    // Unity の case 1345471 の修正 (行数 / 列数の固定を最後の数個で必ず満たす) をそのまま移す
    int childrenToMove = 0;
    const int mainLines =
        CeilToInt(static_cast<float>(count) / static_cast<float>(cellsPerMainAxis));
    if (count > constraintCount && mainLines < constraintCount) {
        childrenToMove = constraintCount - mainLines;
        childrenToMove += FloorToInt(static_cast<float>(childrenToMove)
                                     / (static_cast<float>(cellsPerMainAxis) - 1.0f));
        if (count % cellsPerMainAxis == 1) {
            childrenToMove += 1;
        }
    }

    for (int i = 0; i < count; ++i) {
        int positionX = 0;
        int positionY = 0;
        if (horizontalFirst) {
            if (g.constraint == kGridFixedRowCount && count - i <= childrenToMove) {
                positionX = 0;
                positionY = constraintCount - (count - i);
            } else {
                positionX = i % cellsPerMainAxis;
                positionY = i / cellsPerMainAxis;
            }
        } else {
            if (g.constraint == kGridFixedColumnCount && count - i <= childrenToMove) {
                positionX = constraintCount - (count - i);
                positionY = 0;
            } else {
                positionX = i / cellsPerMainAxis;
                positionY = i % cellsPerMainAxis;
            }
        }
        if (cornerX == 1) {
            positionX = actualCellCountX - 1 - positionX;
        }
        if (cornerY == 1) {
            positionY = actualCellCountY - 1 - positionY;
        }
        px[static_cast<size_t>(i)] =
            startX + (g.cellSize.x + g.spacing.x) * static_cast<float>(positionX);
        py[static_cast<size_t>(i)] =
            startY + (g.cellSize.y + g.spacing.y) * static_cast<float>(positionY);
    }
}

// ---- Group 自身の集計 (LayoutGroup の m_Total*Size) ----
LayoutSizes GroupTotals(World& world, EntityID group, const UILayoutGroupComponent& g, int axis,
                        float width, LayoutScratch& s, int depth)
{
    {
        const LayoutScratch::Entry& en = EntryAt(s, SlotOf(s, group));
        if (axis == 0 && en.hasTotalsW) {
            return en.totalsW;
        }
        if (axis == 1 && en.hasTotalsH && en.totalsHFor == width) {
            return en.totalsH;
        }
    }
    std::vector<EntityID> children;
    CollectLayoutChildren(world, group, children);
    LayoutSizes t;
    if (g.kind == kLayoutGrid) {
        t = GridTotals(g, axis, static_cast<int>(children.size()), width);
    } else if (axis == 0) {
        std::vector<LayoutSizes> sizesW;
        ChildSizesAlong(world, g, children, 0, nullptr, s, depth, sizesW);
        t = CalcAlongAxis(g, 0, sizesW);
    } else {
        // 高さの集計は子の高さの関数で、子の高さは子の幅の関数 (Unity は SetLayoutHorizontal の後に
        // CalculateLayoutInputVertical を回す)。自分の幅 width で子の幅を先に配る
        std::vector<LayoutSizes> sizesW;
        ChildSizesAlong(world, g, children, 0, nullptr, s, depth, sizesW);
        const LayoutSizes totalsW = GroupTotals(world, group, g, 0, 0.0f, s, depth);
        std::vector<float> px, pw;
        SetChildrenAlongAxis(g, 0, width, totalsW, sizesW, px, pw);
        std::vector<LayoutSizes> sizesH;
        ChildSizesAlong(world, g, children, 1, &pw, s, depth, sizesH);
        t = CalcAlongAxis(g, 1, sizesH);
    }
    LayoutScratch::Entry& en = EntryAt(s, SlotOf(s, group));
    if (axis == 0) {
        en.totalsW = t;
        en.hasTotalsW = 1;
    } else {
        en.totalsH = t;
        en.totalsHFor = width;
        en.hasTotalsH = 1;
    }
    return t;
}

// LayoutUtility.GetMinSize / GetPreferredSize / GetFlexibleSize
LayoutSizes InputAxis(World& world, EntityID e, int axis, float width, LayoutScratch& s, int depth)
{
    if (depth > kMaxLayoutDepth || e.IsNull()) {
        return {};
    }
    {
        const LayoutScratch::Entry& en = EntryAt(s, SlotOf(s, e));
        if (axis == 0 && en.hasWidth) {
            return en.width;
        }
        if (axis == 1 && en.hasHeight && en.heightFor == width) {
            return en.height;
        }
    }

    // ILayoutElement の提供者。組込み (Group / テキスト) は優先度 0、LayoutElement は自分の優先度
    struct Provider {
        int32_t priority;
        float min;
        float preferred;
        float flexible;
    };
    Provider providers[3] = {};
    int n = 0;
    if (const auto* g = world.GetComponent<UILayoutGroupComponent>(e)) {
        const LayoutSizes t = GroupTotals(world, e, *g, axis, width, s, depth + 1);
        providers[n++] = { 0, t.min, t.preferred, t.flexible };
    }
    if (const auto* el = world.GetComponent<UIElementComponent>(e); el && el->kind == 1) {
        // Unity の Text: min 0 / flexible 未指定。幅は折り返さない長さ、高さは今の幅で折り返した行数
        const uitext::TextSize ts = (axis == 0)
            ? uitext::Measure(el->text, el->fontScale, false, 0.0f, uitext::ActiveFontMetrics())
            : uitext::Measure(el->text, el->fontScale, el->wrap != 0, width,
                              uitext::ActiveFontMetrics());
        providers[n++] = { 0, 0.0f, (axis == 0) ? ts.w : ts.h, -1.0f };
    }
    if (const auto* le = world.GetComponent<UILayoutElementComponent>(e)) {
        providers[n++] = { le->layoutPriority, (axis == 0) ? le->minWidth : le->minHeight,
                           (axis == 0) ? le->preferredWidth : le->preferredHeight,
                           (axis == 0) ? le->flexibleWidth : le->flexibleHeight };
    }
    // GetLayoutProperty: 負 (未指定) の提供者は飛ばし、残りのうち最高優先度の中で最大の値
    const auto pick = [&](float Provider::*member, float defaultValue, bool* found) {
        float current = defaultValue;
        int32_t maxPriority = INT_MIN;
        bool any = false;
        for (int i = 0; i < n; ++i) {
            const Provider& p = providers[i];
            if (p.priority < maxPriority) {
                continue;
            }
            const float prop = p.*member;
            if (prop < 0.0f) {
                continue;
            }
            if (p.priority > maxPriority) {
                current = prop;
                maxPriority = p.priority;
                any = true;
            } else if (prop > current) {
                current = prop;
            }
        }
        if (found) {
            *found = any;
        }
        return current;
    };
    LayoutSizes out;
    out.min = pick(&Provider::min, 0.0f, nullptr);
    bool hasPreferred = false;
    float preferred = pick(&Provider::preferred, 0.0f, &hasPreferred);
    if (!hasPreferred) {
        // ★Unity との違い: 誰も preferred を言わない要素 (素のパネル等) は sizeDelta を希望値にする。
        //   Fitter は通さない (Fitter → この関数 → Fitter の環になる)
        preferred = MaxF(RawSizeDelta(world, e, axis), 0.0f);
    }
    out.preferred = MaxF(out.min, preferred); // Unity: Max(GetMinWidth, preferredWidth)
    out.flexible = pick(&Provider::flexible, 0.0f, nullptr);

    LayoutScratch::Entry& en = EntryAt(s, SlotOf(s, e));
    if (axis == 0) {
        en.width = out;
        en.hasWidth = 1;
    } else {
        en.height = out;
        en.heightFor = width;
        en.hasHeight = 1;
    }
    return out;
}

// group の子を W x H (作者単位) の中に並べ、placed に積む。戻り値は group のメモの行
int32_t Arrange(World& world, EntityID group, const UILayoutGroupComponent& g, float width,
                float height, LayoutScratch& s)
{
    {
        const int32_t slot = SlotOf(s, group);
        const LayoutScratch::Entry& en = EntryAt(s, slot);
        if (en.hasArrange && en.arrangeW == width && en.arrangeH == height) {
            return slot;
        }
    }
    std::vector<EntityID> children;
    CollectLayoutChildren(world, group, children);
    const size_t count = children.size();
    std::vector<float> px, pw, py, ph;
    if (g.kind == kLayoutGrid) {
        // Grid は子のサイズを常に cellSize にする (Unity は SetLayoutHorizontal でサイズだけ先に書く)
        ArrangeGrid(g, static_cast<int>(count), width, height, px, py);
        pw.assign(count, g.cellSize.x);
        ph.assign(count, g.cellSize.y);
    } else {
        std::vector<LayoutSizes> sizesW;
        ChildSizesAlong(world, g, children, 0, nullptr, s, 1, sizesW);
        const LayoutSizes totalsW = GroupTotals(world, group, g, 0, 0.0f, s, 1);
        SetChildrenAlongAxis(g, 0, width, totalsW, sizesW, px, pw);
        std::vector<LayoutSizes> sizesH;
        ChildSizesAlong(world, g, children, 1, &pw, s, 1, sizesH);
        const LayoutSizes totalsH = CalcAlongAxis(g, 1, sizesH);
        SetChildrenAlongAxis(g, 1, height, totalsH, sizesH, py, ph);
    }

    const uint32_t first = static_cast<uint32_t>(s.placed.size());
    for (size_t i = 0; i < count; ++i) {
        LayoutScratch::Placed p;
        p.e = children[i];
        p.x = px[i];
        p.y = py[i];
        p.w = pw[i];
        p.h = ph[i];
        s.placed.push_back(p);
        LayoutScratch::Entry& ce = EntryAt(s, SlotOf(s, children[i]));
        ce.placedBy = group;
        ce.placedAt = first + static_cast<uint32_t>(i);
    }
    const int32_t slot = SlotOf(s, group);
    LayoutScratch::Entry& en = EntryAt(s, slot);
    en.hasArrange = 1;
    en.arrangeW = width;
    en.arrangeH = height;
    en.placedFirst = first;
    en.placedCount = static_cast<uint32_t>(count);
    return slot;
}

} // namespace

bool IsLayoutChild(World& world, EntityID group, EntityID child)
{
    if (group.IsNull() || child.IsNull() || !world.IsAlive(child)
        || world.GetParent(child) != group) {
        return false;
    }
    if (world.GetComponent<UILayoutGroupComponent>(group) == nullptr
        || !IsUiNodeForLayout(world, group)) {
        return false;
    }
    const auto* rt = world.GetComponent<RectTransformComponent>(child);
    if (rt == nullptr && world.GetComponent<UIElementComponent>(child) == nullptr) {
        return false;
    }
    if (world.GetComponent<UICanvasComponent>(child) != nullptr) {
        return false; // Canvas の矩形は常にキャンバス全面 (UILayout.cpp)
    }
    if (rt != nullptr && rt->basis != 0) {
        return false;
    }
    if (const auto* le = world.GetComponent<UILayoutElementComponent>(child);
        le != nullptr && le->ignoreLayout) {
        return false;
    }
    return IsEntityActive(world, child);
}

LayoutSizes LayoutInputWidth(World& world, EntityID e, LayoutScratch& scratch)
{
    return InputAxis(world, e, 0, 0.0f, scratch, 0);
}

LayoutSizes LayoutInputHeight(World& world, EntityID e, float width, LayoutScratch& scratch)
{
    return InputAxis(world, e, 1, width, scratch, 0);
}

bool ResolveLayoutChild(World& world, EntityID group, EntityID child, const UIRect& groupRect,
                        float scale, LayoutScratch& scratch, UIRect& out)
{
    if (group.IsNull() || child.IsNull()) {
        return false;
    }
    const auto* g = world.GetComponent<UILayoutGroupComponent>(group);
    if (g == nullptr || !IsLayoutChild(world, group, child)) {
        return false;
    }
    // 配置は作者単位で解いてから距離スケールを掛ける。screen UI は scale 1 = /1 と *1 はビット恒等
    const float sc = (scale > 0.0f) ? scale : 1.0f;
    const int32_t gslot = Arrange(world, group, *g, groupRect.w / sc, groupRect.h / sc, scratch);
    const uint32_t first = EntryAt(scratch, gslot).placedFirst;
    const uint32_t count = EntryAt(scratch, gslot).placedCount;
    const LayoutScratch::Entry& ce = EntryAt(scratch, SlotOf(scratch, child));
    const LayoutScratch::Placed* p = nullptr;
    if (ce.placedBy == group && ce.placedAt >= first && ce.placedAt < first + count
        && scratch.placed[ce.placedAt].e == child) {
        p = &scratch.placed[ce.placedAt];
    } else {
        for (uint32_t i = first; i < first + count; ++i) {
            if (scratch.placed[i].e == child) {
                p = &scratch.placed[i];
                break;
            }
        }
    }
    if (p == nullptr) {
        return false;
    }
    out.x = groupRect.x + p->x * sc;
    out.y = groupRect.y + p->y * sc;
    out.w = p->w * sc;
    out.h = p->h * sc;
    return true;
}

bool ApplyContentSizeFitter(World& world, EntityID e, const RectTransformComponent& rt,
                            const UIRect& base, float scale, LayoutScratch& scratch,
                            RectTransformComponent& outRt)
{
    const auto* fit = world.GetComponent<UIContentSizeFitterComponent>(e);
    if (fit == nullptr) {
        return false;
    }
    const bool fitW = fit->horizontalFit == kFitMinSize || fit->horizontalFit == kFitPreferred;
    const bool fitH = fit->verticalFit == kFitMinSize || fit->verticalFit == kFitPreferred;
    if (!fitW && !fitH) {
        return false;
    }
    const float sc = (scale > 0.0f) ? scale : 1.0f;
    const float parentW = base.w / sc;
    const float parentH = base.h / sc;
    outRt = rt;
    // SetSizeWithCurrentAnchors: sizeDelta = size - 親の大きさ * (anchorMax - anchorMin)
    float width = parentW * (rt.anchorMax.x - rt.anchorMin.x) + rt.sizeDelta.x;
    if (fitW) {
        const LayoutSizes sz = InputAxis(world, e, 0, 0.0f, scratch, 0);
        width = (fit->horizontalFit == kFitMinSize) ? sz.min : sz.preferred;
        outRt.sizeDelta.x = width - parentW * (rt.anchorMax.x - rt.anchorMin.x);
    }
    if (fitH) {
        // 高さは幅が決まってから (Unity も SetLayoutHorizontal → SetLayoutVertical の順)
        const LayoutSizes sz = InputAxis(world, e, 1, width, scratch, 0);
        const float height = (fit->verticalFit == kFitMinSize) ? sz.min : sz.preferred;
        outRt.sizeDelta.y = height - parentH * (rt.anchorMax.y - rt.anchorMin.y);
    }
    return true;
}

uint32_t LayoutDrivenBits(World& world, EntityID e)
{
    uint32_t bits = 0;
    if (e.IsNull() || !world.IsAlive(e) || world.GetComponent<UICanvasComponent>(e) != nullptr) {
        return bits;
    }
    const EntityID parent = world.GetParent(e);
    if (!parent.IsNull() && IsLayoutChild(world, parent, e)) {
        const auto* g = world.GetComponent<UILayoutGroupComponent>(parent);
        bits |= kDrivenByGroup;
        if (g->kind == kLayoutGrid || g->controlChildWidth) {
            bits |= kDrivenWidth;
        }
        if (g->kind == kLayoutGrid || g->controlChildHeight) {
            bits |= kDrivenHeight;
        }
    }
    // Group が制御していない軸は、自分の Fitter があればそれが決める (Resolve と同じ規則)
    if (const auto* fit = world.GetComponent<UIContentSizeFitterComponent>(e)) {
        const bool fitW = fit->horizontalFit == kFitMinSize || fit->horizontalFit == kFitPreferred;
        const bool fitH = fit->verticalFit == kFitMinSize || fit->verticalFit == kFitPreferred;
        if (fitW && (bits & kDrivenWidth) == 0) {
            bits |= kDrivenWidth | kDrivenByFitter;
        }
        if (fitH && (bits & kDrivenHeight) == 0) {
            bits |= kDrivenHeight | kDrivenByFitter;
        }
    }
    if (uiwidgets::IsSliderDriven(world, e)) {
        bits |= kDrivenBySlider; // M75f: 位置とサイズは編集できるが、アンカーは効かない
    }
    return bits;
}

} // namespace uilayout
} // namespace mye
