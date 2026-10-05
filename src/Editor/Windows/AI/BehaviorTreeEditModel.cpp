//====================================================================================
//                          BehaviorTreeEditModel.cpp
//  MyEngin/ 秋田蓮音                                                       10/06/2026
//                                          BT 窓が編集する木のモデルの実装
//====================================================================================
#include "Editor/Windows/AI/BehaviorTreeEditModel.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "Engine/Platform/PathUtil.h"

namespace mye {

namespace {

std::string DumpAsset(const BehaviorTreeAsset& asset)
{
    return BehaviorTreeLibrary::ToJson(asset).dump();
}

std::string DumpBoard(const BlackboardAsset& board)
{
    return BlackboardLibrary::ToJson(board).dump();
}

// キー欄 keyIndex が空だと実行時に Failure になる (必須) か。空でも動く欄 (任意) は検査しない
bool KeyRequired(const BtNodeDef& node, int keyIndex)
{
    const auto param = [&node](int index) { return static_cast<size_t>(index) < node.params.size() ? node.params[static_cast<size_t>(index)].i : 0; };
    switch (node.kind) {
    case BtNodeKind::SetBlackboard:
        return keyIndex == btnodekey::kTarget || param(btsetparam::kSource) == btsetparam::kCopy;
    case BtNodeKind::FindRandomPoint:
        return keyIndex == btrandomkey::kResult;
    case BtNodeKind::FindNearestTarget:
        return keyIndex == btnearestkey::kTarget;
    case BtNodeKind::SearchArea:
        return keyIndex == btsearchkey::kOrigin ? param(btsearchparam::kUsePrediction) == 0 : param(btsearchparam::kUsePrediction) != 0;
    case BtNodeKind::SendEvent:
        return keyIndex == btsendkey::kTarget && param(btsendparam::kTarget) == btsendtarget::kEntity;
    case BtNodeKind::MoveTo:
    case BtNodeKind::RotateTo:
    case BtNodeKind::ClearBlackboard:
    case BtNodeKind::FindTarget:
    case BtNodeKind::Patrol:
        return true;
    default:
        return false;
    }
}

BtParamValue DefaultValueOf(const BtParamDesc& desc)
{
    BtParamValue value;
    switch (desc.type) {
    case BtParamType::Float: value.f = desc.defaultValue; break;
    case BtParamType::Guid:
    case BtParamType::Mask:
    case BtParamType::String: break; // 0 / 空
    default: value.i = static_cast<int32_t>(desc.defaultValue); break;
    }
    return value;
}

std::vector<BtParamValue> DefaultParamsOf(const BtParamDesc* descs, int count)
{
    std::vector<BtParamValue> out;
    out.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        out.push_back(DefaultValueOf(descs[i]));
    }
    return out;
}

// desc の範囲へ丸めた値を out へ。型に合わない値は false
bool NormalizeParam(const BtParamDesc& desc, const BtParamValue& in, BtParamValue& out)
{
    out = in;
    switch (desc.type) {
    case BtParamType::Int:
        out.i = (std::clamp)(in.i, static_cast<int32_t>(desc.minValue), static_cast<int32_t>(desc.maxValue));
        return true;
    case BtParamType::Float:
        if (!std::isfinite(in.f)) {
            return false;
        }
        out.f = (std::clamp)(in.f, desc.minValue, desc.maxValue);
        return true;
    case BtParamType::Bool:
        out.i = in.i != 0 ? 1 : 0;
        return true;
    case BtParamType::Enum:
        out.i = (std::clamp)(in.i, 0, desc.enumCount - 1);
        return true;
    case BtParamType::Guid:
    case BtParamType::Mask:
        return true;
    case BtParamType::String:
        return in.s.size() <= kBbMaxNameBytes;
    }
    return false;
}

bool SameParam(const BtParamDesc& desc, const BtParamValue& a, const BtParamValue& b)
{
    switch (desc.type) {
    case BtParamType::Float: return a.f == b.f;
    case BtParamType::Guid:
    case BtParamType::Mask: return a.u == b.u;
    case BtParamType::String: return a.s == b.s;
    default: return a.i == b.i;
    }
}

} // namespace

bool BehaviorTreeEditModel::Load(std::shared_ptr<const BehaviorTreeAsset> registered)
{
    if (!registered) {
        return false;
    }
    asset_ = *registered;
    registered_ = std::move(registered);
    loaded_ = true;
    // 位置を持たない木 (手書きの .bt.json は pos が 0 のまま) は、箱が 1 か所に積み重ならないよう整列する
    const bool noPositions = asset_.nodes.size() > 1
        && std::all_of(asset_.nodes.begin(), asset_.nodes.end(), [this](const BtNodeDef& node) {
               return node.pos[0] == asset_.nodes[0].pos[0] && node.pos[1] == asset_.nodes[0].pos[1];
           });
    if (noPositions) {
        LayoutAll();
    }
    savedBtJson_ = DumpAsset(asset_);
    undo_.clear();
    redo_.clear();
    gesture_ = false;
    gesturePushed_ = false;
    LoadBoardFromRegistry();
    committed_ = Capture();
    ++revision_;
    btDirtyValid_ = false;
    return true;
}

void BehaviorTreeEditModel::Clear()
{
    asset_ = BehaviorTreeAsset{};
    board_ = BlackboardAsset{};
    boardLoaded_ = false;
    boardRegistered_.reset();
    registered_.reset();
    savedBtJson_.clear();
    savedBoardJson_.clear();
    undo_.clear();
    redo_.clear();
    committed_ = EditSnapshot{};
    gesture_ = false;
    gesturePushed_ = false;
    loaded_ = false;
    ++revision_;
    btDirtyValid_ = false;
    boardDirtyValid_ = false;
}

bool BehaviorTreeEditModel::Dirty() const
{
    if (!loaded_) {
        return false;
    }
    if (!btDirtyValid_) {
        btDirty_ = DumpAsset(asset_) != savedBtJson_;
        btDirtyValid_ = true;
    }
    return btDirty_;
}

bool BehaviorTreeEditModel::BoardDirty() const
{
    if (!boardLoaded_) {
        return false;
    }
    if (!boardDirtyValid_) {
        boardDirty_ = DumpBoard(board_) != savedBoardJson_;
        boardDirtyValid_ = true;
    }
    return boardDirty_;
}

bool BehaviorTreeEditModel::IsRegistered() const
{
    return loaded_ && trees_ != nullptr && trees_->GetShared(asset_.hash) == registered_;
}

const BtNodeDef* BehaviorTreeEditModel::FindNode(int32_t id) const
{
    const int index = asset_.FindNode(id);
    return index >= 0 ? &asset_.nodes[static_cast<size_t>(index)] : nullptr;
}

int BehaviorTreeEditModel::ParentIndexOf(int index) const
{
    const int32_t id = asset_.nodes[static_cast<size_t>(index)].id;
    for (size_t i = 0; i < asset_.nodes.size(); ++i) {
        const std::vector<int32_t>& kids = asset_.nodes[i].childIds;
        if (std::find(kids.begin(), kids.end(), id) != kids.end()) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int32_t BehaviorTreeEditModel::ParentOf(int32_t id) const
{
    const int index = FindIndex(id);
    if (index < 0) {
        return -1;
    }
    const int parent = ParentIndexOf(index);
    return parent >= 0 ? asset_.nodes[static_cast<size_t>(parent)].id : -1;
}

int BehaviorTreeEditModel::ChildOrderOf(int32_t id) const
{
    const int32_t parentId = ParentOf(id);
    if (parentId < 0) {
        return -1;
    }
    const std::vector<int32_t>& kids = FindNode(parentId)->childIds;
    return static_cast<int>(std::find(kids.begin(), kids.end(), id) - kids.begin());
}

int BehaviorTreeEditModel::MaxChildren(BtNodeKind kind)
{
    if (kind == BtNodeKind::SubTree) {
        return 0; // ファイルの SubTree は子を持たない (取り込んだ部分木は実行用の木にだけ付く)
    }
    const int max = BtNodeTypeOf(kind).maxChildren;
    return max == kBtUnlimitedChildren ? kBtMaxNodes : max;
}

float BehaviorTreeEditModel::NodeHeight(const BtNodeDef& node)
{
    return kBtNodeBodyHeight + kBtDecoratorBandHeight * static_cast<float>(node.decorators.size());
}

bool BehaviorTreeEditModel::KeyAccepts(BtNodeKind kind, int keyIndex, BbType type)
{
    const bool spatial = type == BbType::Vector || type == BbType::Entity;
    switch (kind) {
    case BtNodeKind::MoveTo:
    case BtNodeKind::RotateTo:
        return spatial; // 目標 = Vector か Entity
    case BtNodeKind::FindRandomPoint:
        return type == BbType::Vector; // 中心・結果とも Vector
    case BtNodeKind::FindNearestTarget:
        return keyIndex == btnearestkey::kTarget ? type == BbType::Entity : type == BbType::Vector;
    case BtNodeKind::SearchArea:
        return keyIndex == btsearchkey::kOrigin ? type == BbType::Vector : type == BbType::Entity;
    case BtNodeKind::FindTarget:
    case BtNodeKind::Patrol:
        return type == BbType::Entity;
    case BtNodeKind::SendEvent:
        return keyIndex == btsendkey::kTarget ? type == BbType::Entity : type == BbType::Vector;
    default:
        return true; // SetBlackboard / ClearBlackboard は全部の型
    }
}

// ---- ノード・接続 ----

int32_t BehaviorTreeEditModel::NextId() const
{
    int32_t next = 0;
    for (const BtNodeDef& node : asset_.nodes) {
        next = (std::max)(next, node.id + 1);
    }
    return next;
}

int32_t BehaviorTreeEditModel::AddNode(BtNodeKind kind, float x, float y)
{
    if (!loaded_ || asset_.nodes.size() >= static_cast<size_t>(kBtMaxNodes) || static_cast<size_t>(kind) >= static_cast<size_t>(BtNodeKind::Count)) {
        return -1;
    }
    const BtNodeTypeInfo& info = BtNodeTypeOf(kind);
    BtNodeDef node;
    node.id = NextId();
    node.kind = kind;
    node.params = DefaultParamsOf(info.params, info.paramCount);
    node.keys.assign(static_cast<size_t>(info.keyCount), std::string());
    node.pos[0] = x;
    node.pos[1] = y;
    const int32_t id = node.id;
    asset_.nodes.push_back(std::move(node));
    if (asset_.rootId < 0) {
        asset_.rootId = id;
    }
    Touch();
    return id;
}

void BehaviorTreeEditModel::RemoveFromParent(int32_t childId)
{
    for (BtNodeDef& node : asset_.nodes) {
        node.childIds.erase(std::remove(node.childIds.begin(), node.childIds.end(), childId), node.childIds.end());
    }
}

void BehaviorTreeEditModel::SortChildrenByX(int32_t parentId)
{
    const int index = FindIndex(parentId);
    if (index < 0) {
        return;
    }
    std::vector<int32_t>& kids = asset_.nodes[static_cast<size_t>(index)].childIds;
    std::stable_sort(kids.begin(), kids.end(), [this](int32_t a, int32_t b) {
        return FindNode(a)->pos[0] < FindNode(b)->pos[0];
    });
}

void BehaviorTreeEditModel::CollectSubtree(int32_t id, std::vector<int32_t>& out) const
{
    out.push_back(id);
    // 木として成り立つ前提 (Connect が循環を拒む)。out の大きさでも打ち切る
    for (const int32_t child : FindNode(id)->childIds) {
        if (out.size() <= asset_.nodes.size()) {
            CollectSubtree(child, out);
        }
    }
}

int BehaviorTreeEditModel::DepthOf(int32_t id) const
{
    int depth = 0;
    int32_t at = id;
    while (depth <= kBtMaxNodes) {
        const int32_t parent = ParentOf(at);
        if (parent < 0) {
            break;
        }
        at = parent;
        ++depth;
    }
    return depth;
}

int BehaviorTreeEditModel::HeightOf(int32_t id) const
{
    int height = 0;
    for (const int32_t child : FindNode(id)->childIds) {
        height = (std::max)(height, 1 + HeightOf(child));
    }
    return height;
}

bool BehaviorTreeEditModel::Connect(int32_t parentId, int32_t childId)
{
    const BtNodeDef* parent = FindNode(parentId);
    const BtNodeDef* child = FindNode(childId);
    if (parent == nullptr || child == nullptr || parentId == childId || childId == asset_.rootId) {
        return false;
    }
    // 循環: parent の祖先に child がいる
    for (int32_t at = parentId; at >= 0; at = ParentOf(at)) {
        if (at == childId) {
            return false;
        }
    }
    const bool alreadyChild = std::find(parent->childIds.begin(), parent->childIds.end(), childId) != parent->childIds.end();
    const int others = static_cast<int>(parent->childIds.size()) - (alreadyChild ? 1 : 0);
    if (others >= MaxChildren(parent->kind)) {
        return false;
    }
    // 付け替えたあと一番深いノードの祖先の数が上限 (BtLinkAsset の kBtMaxDepth) を超えない
    const int32_t oldParent = ParentOf(childId);
    const int parentDepth = DepthOf(parentId);
    if (parentDepth + 1 + HeightOf(childId) >= kBtMaxDepth) {
        return false;
    }
    if (oldParent == parentId && alreadyChild) {
        SortChildrenByX(parentId);
        Touch();
        return true;
    }
    RemoveFromParent(childId);
    asset_.nodes[static_cast<size_t>(FindIndex(parentId))].childIds.push_back(childId);
    SortChildrenByX(parentId);
    Touch();
    return true;
}

bool BehaviorTreeEditModel::Disconnect(int32_t childId)
{
    if (ParentOf(childId) < 0) {
        return false;
    }
    RemoveFromParent(childId);
    Touch();
    return true;
}

bool BehaviorTreeEditModel::Remove(int32_t id, BtRemoveMode mode)
{
    if (FindNode(id) == nullptr) {
        return false;
    }
    std::vector<int32_t> doomed;
    if (mode == BtRemoveMode::WithDescendants) {
        CollectSubtree(id, doomed);
    } else {
        doomed.push_back(id);
    }
    for (const int32_t victim : doomed) {
        RemoveFromParent(victim);
    }
    asset_.nodes.erase(std::remove_if(asset_.nodes.begin(), asset_.nodes.end(),
                                      [&doomed](const BtNodeDef& node) {
                                          return std::find(doomed.begin(), doomed.end(), node.id) != doomed.end();
                                      }),
                       asset_.nodes.end());
    if (std::find(doomed.begin(), doomed.end(), asset_.rootId) != doomed.end()) {
        asset_.rootId = -1;
    }
    Touch();
    return true;
}

bool BehaviorTreeEditModel::SetRoot(int32_t id)
{
    if (FindNode(id) == nullptr || ParentOf(id) >= 0) {
        return false;
    }
    if (asset_.rootId == id) {
        return false;
    }
    asset_.rootId = id;
    Touch();
    return true;
}

bool BehaviorTreeEditModel::MoveNode(int32_t id, float x, float y)
{
    const int index = FindIndex(id);
    if (index < 0 || !std::isfinite(x) || !std::isfinite(y)) {
        return false;
    }
    BtNodeDef& node = asset_.nodes[static_cast<size_t>(index)];
    if (node.pos[0] == x && node.pos[1] == y) {
        return false;
    }
    node.pos[0] = x;
    node.pos[1] = y;
    const int32_t parentId = ParentOf(id);
    if (parentId >= 0) {
        SortChildrenByX(parentId);
    }
    Touch();
    return true;
}

bool BehaviorTreeEditModel::MoveSubtree(int32_t id, float dx, float dy)
{
    if (FindNode(id) == nullptr || !std::isfinite(dx) || !std::isfinite(dy) || (dx == 0.0f && dy == 0.0f)) {
        return false;
    }
    std::vector<int32_t> moved;
    CollectSubtree(id, moved);
    for (const int32_t movedId : moved) {
        BtNodeDef& node = asset_.nodes[static_cast<size_t>(FindIndex(movedId))];
        node.pos[0] += dx;
        node.pos[1] += dy;
    }
    const int32_t parentId = ParentOf(id);
    if (parentId >= 0) {
        SortChildrenByX(parentId);
    }
    Touch();
    return true;
}

// 整列: 根 (と孤立した部分木) ごとに、兄弟を左から並べて親を子の真上の中央に置く。段ごとの高さの最大で行を決める
void BehaviorTreeEditModel::LayoutAll()
{
    std::unordered_map<int32_t, size_t> indexOf;
    for (size_t i = 0; i < asset_.nodes.size(); ++i) {
        indexOf[asset_.nodes[i].id] = i;
    }
    std::vector<bool> hasParent(asset_.nodes.size(), false);
    for (const BtNodeDef& node : asset_.nodes) {
        for (const int32_t child : node.childIds) {
            hasParent[indexOf[child]] = true;
        }
    }
    std::vector<int32_t> roots;
    if (asset_.rootId >= 0 && indexOf.count(asset_.rootId) != 0) {
        roots.push_back(asset_.rootId);
    }
    for (size_t i = 0; i < asset_.nodes.size(); ++i) {
        if (!hasParent[i] && asset_.nodes[i].id != asset_.rootId) {
            roots.push_back(asset_.nodes[i].id);
        }
    }

    // 子孫を含めた幅
    std::unordered_map<int32_t, float> width;
    const auto widthOf = [&](auto&& self, int32_t id) -> float {
        const BtNodeDef& node = asset_.nodes[indexOf[id]];
        float sum = 0.0f;
        for (const int32_t child : node.childIds) {
            sum += self(self, child);
        }
        if (!node.childIds.empty()) {
            sum += kBtLayoutGapX * static_cast<float>(node.childIds.size() - 1);
        }
        return width[id] = (std::max)(kBtNodeWidth, sum);
    };

    std::vector<float> rowHeight; // 段ごとの箱の高さの最大
    std::unordered_map<int32_t, int> depthOf;
    const auto place = [&](auto&& self, int32_t id, float left, int depth) -> void {
        BtNodeDef& node = asset_.nodes[indexOf[id]];
        const float span = width[id];
        node.pos[0] = left + (span - kBtNodeWidth) * 0.5f;
        depthOf[id] = depth;
        if (rowHeight.size() <= static_cast<size_t>(depth)) {
            rowHeight.resize(static_cast<size_t>(depth) + 1, 0.0f);
        }
        rowHeight[static_cast<size_t>(depth)] = (std::max)(rowHeight[static_cast<size_t>(depth)], NodeHeight(node));
        float childrenWidth = 0.0f;
        for (const int32_t child : node.childIds) {
            childrenWidth += width[child];
        }
        if (!node.childIds.empty()) {
            childrenWidth += kBtLayoutGapX * static_cast<float>(node.childIds.size() - 1);
        }
        float at = left + (span - childrenWidth) * 0.5f;
        for (const int32_t child : node.childIds) {
            self(self, child, at, depth + 1);
            at += width[child] + kBtLayoutGapX;
        }
    };

    float left = 0.0f;
    for (const int32_t root : roots) {
        widthOf(widthOf, root);
        place(place, root, left, 0);
        left += width[root] + kBtLayoutGapX * 2.0f;
    }
    std::vector<float> rowTop(rowHeight.size(), 0.0f);
    for (size_t d = 1; d < rowTop.size(); ++d) {
        rowTop[d] = rowTop[d - 1] + rowHeight[d - 1] + kBtLayoutGapY;
    }
    for (BtNodeDef& node : asset_.nodes) {
        const auto it = depthOf.find(node.id);
        node.pos[1] = it != depthOf.end() ? rowTop[static_cast<size_t>(it->second)] : 0.0f;
    }
}

void BehaviorTreeEditModel::AutoLayout()
{
    if (!loaded_) {
        return;
    }
    LayoutAll();
    Touch();
}

// ---- パラメータ・キー・Decorator ----

bool BehaviorTreeEditModel::SetParam(int32_t id, int paramIndex, const BtParamValue& value)
{
    const int index = FindIndex(id);
    if (index < 0) {
        return false;
    }
    BtNodeDef& node = asset_.nodes[static_cast<size_t>(index)];
    const BtNodeTypeInfo& info = BtNodeTypeOf(node.kind);
    if (paramIndex < 0 || paramIndex >= info.paramCount || static_cast<size_t>(paramIndex) >= node.params.size()) {
        return false;
    }
    BtParamValue normalized;
    if (!NormalizeParam(info.params[paramIndex], value, normalized) || SameParam(info.params[paramIndex], normalized, node.params[static_cast<size_t>(paramIndex)])) {
        return false;
    }
    node.params[static_cast<size_t>(paramIndex)] = std::move(normalized);
    Touch();
    return true;
}

bool BehaviorTreeEditModel::SetTaskField(int32_t id, const std::string& name, const nlohmann::json& value)
{
    const int index = FindIndex(id);
    if (index < 0 || name.empty() || name.size() > kBbMaxNameBytes || value.is_null()) {
        return false;
    }
    BtNodeDef& node = asset_.nodes[static_cast<size_t>(index)];
    if (node.kind != BtNodeKind::CppTask) {
        return false;
    }
    // 保存できる形 (数・真偽・短い文字列・数の短い配列) だけ受ける。LinkAsset / FromJson と同じ上限
    const bool plain = value.is_number() || value.is_boolean() || (value.is_string() && value.get<std::string>().size() <= kBtMaxTaskFieldTextBytes)
                       || (value.is_array() && value.size() <= 16
                           && std::all_of(value.begin(), value.end(), [](const nlohmann::json& element) { return element.is_number(); }));
    if (!plain) {
        return false;
    }
    if (node.taskFields.is_object() && node.taskFields.contains(name) && node.taskFields[name] == value) {
        return false;
    }
    if (!node.taskFields.is_object()) {
        node.taskFields = nlohmann::json::object();
    }
    if (!node.taskFields.contains(name) && node.taskFields.size() >= kBtMaxTaskFieldEntries) {
        return false;
    }
    node.taskFields[name] = value;
    Touch();
    return true;
}

bool BehaviorTreeEditModel::SetKey(int32_t id, int keyIndex, const std::string& name)
{
    const int index = FindIndex(id);
    if (index < 0 || name.size() > kBbMaxNameBytes) {
        return false;
    }
    BtNodeDef& node = asset_.nodes[static_cast<size_t>(index)];
    if (keyIndex < 0 || static_cast<size_t>(keyIndex) >= node.keys.size() || node.keys[static_cast<size_t>(keyIndex)] == name) {
        return false;
    }
    node.keys[static_cast<size_t>(keyIndex)] = name;
    Touch();
    return true;
}

int BehaviorTreeEditModel::AddDecorator(int32_t id, BtDecoratorKind kind)
{
    const int index = FindIndex(id);
    if (index < 0 || static_cast<size_t>(kind) >= static_cast<size_t>(BtDecoratorKind::Count)) {
        return -1;
    }
    BtNodeDef& node = asset_.nodes[static_cast<size_t>(index)];
    if (node.decorators.size() >= static_cast<size_t>(kBtMaxDecoratorsPerNode)) {
        return -1;
    }
    const BtDecoratorTypeInfo& info = BtDecoratorTypeOf(kind);
    BtDecoratorDef deco;
    deco.kind = kind;
    deco.params = DefaultParamsOf(info.params, info.paramCount);
    if (info.hasKey) {
        // キー名が空だと木として成り立たない (BtLinkAsset が拒む) ので、BB の先頭のキーで始める
        const BlackboardAsset* board = Board();
        if (board == nullptr || board->keys.empty()) {
            return -1;
        }
        deco.key = board->keys.front().name;
    }
    node.decorators.push_back(std::move(deco));
    Touch();
    return static_cast<int>(node.decorators.size()) - 1;
}

bool BehaviorTreeEditModel::RemoveDecorator(int32_t id, int index)
{
    const int nodeIndex = FindIndex(id);
    if (nodeIndex < 0) {
        return false;
    }
    std::vector<BtDecoratorDef>& list = asset_.nodes[static_cast<size_t>(nodeIndex)].decorators;
    if (index < 0 || static_cast<size_t>(index) >= list.size()) {
        return false;
    }
    list.erase(list.begin() + index);
    Touch();
    return true;
}

bool BehaviorTreeEditModel::MoveDecorator(int32_t id, int from, int to)
{
    const int nodeIndex = FindIndex(id);
    if (nodeIndex < 0) {
        return false;
    }
    std::vector<BtDecoratorDef>& list = asset_.nodes[static_cast<size_t>(nodeIndex)].decorators;
    if (from < 0 || static_cast<size_t>(from) >= list.size() || to < 0 || static_cast<size_t>(to) >= list.size() || from == to) {
        return false;
    }
    BtDecoratorDef moved = std::move(list[static_cast<size_t>(from)]);
    list.erase(list.begin() + from);
    list.insert(list.begin() + to, std::move(moved));
    Touch();
    return true;
}

bool BehaviorTreeEditModel::SetDecoratorParam(int32_t id, int decoratorIndex, int paramIndex, const BtParamValue& value)
{
    const int nodeIndex = FindIndex(id);
    if (nodeIndex < 0) {
        return false;
    }
    std::vector<BtDecoratorDef>& list = asset_.nodes[static_cast<size_t>(nodeIndex)].decorators;
    if (decoratorIndex < 0 || static_cast<size_t>(decoratorIndex) >= list.size()) {
        return false;
    }
    BtDecoratorDef& deco = list[static_cast<size_t>(decoratorIndex)];
    const BtDecoratorTypeInfo& info = BtDecoratorTypeOf(deco.kind);
    if (paramIndex < 0 || paramIndex >= info.paramCount || static_cast<size_t>(paramIndex) >= deco.params.size()) {
        return false;
    }
    BtParamValue normalized;
    if (!NormalizeParam(info.params[paramIndex], value, normalized) || SameParam(info.params[paramIndex], normalized, deco.params[static_cast<size_t>(paramIndex)])) {
        return false;
    }
    deco.params[static_cast<size_t>(paramIndex)] = std::move(normalized);
    Touch();
    return true;
}

bool BehaviorTreeEditModel::SetDecoratorKey(int32_t id, int decoratorIndex, const std::string& name)
{
    const int nodeIndex = FindIndex(id);
    if (nodeIndex < 0 || name.empty() || name.size() > kBbMaxNameBytes) {
        return false; // 空のキーは木として成り立たない
    }
    std::vector<BtDecoratorDef>& list = asset_.nodes[static_cast<size_t>(nodeIndex)].decorators;
    if (decoratorIndex < 0 || static_cast<size_t>(decoratorIndex) >= list.size()) {
        return false;
    }
    BtDecoratorDef& deco = list[static_cast<size_t>(decoratorIndex)];
    if (!BtDecoratorTypeOf(deco.kind).hasKey || deco.key == name) {
        return false;
    }
    deco.key = name;
    Touch();
    return true;
}

bool BehaviorTreeEditModel::SetBlackboard(uint64_t guid)
{
    if (!loaded_ || asset_.blackboard == guid) {
        return false;
    }
    asset_.blackboard = guid;
    LoadBoardFromRegistry();
    Touch();
    return true;
}

// ---- Undo / Redo ----

void BehaviorTreeEditModel::Touch()
{
    ++revision_;
    btDirtyValid_ = false;
    boardDirtyValid_ = false;
    redo_.clear();
    if (gesture_) {
        if (!gesturePushed_) {
            PushUndo(committed_);
            gesturePushed_ = true;
        }
        return;
    }
    PushUndo(committed_);
    committed_ = Capture();
}

BehaviorTreeEditModel::EditSnapshot BehaviorTreeEditModel::Capture() const
{
    EditSnapshot snapshot;
    snapshot.asset = asset_;
    snapshot.board = board_;
    snapshot.boardLoaded = boardLoaded_;
    return snapshot;
}

void BehaviorTreeEditModel::Restore(EditSnapshot&& snapshot)
{
    asset_ = std::move(snapshot.asset);
    board_ = std::move(snapshot.board);
    boardLoaded_ = snapshot.boardLoaded;
    BindBoardRegistered(boardLoaded_ && boards_ != nullptr ? boards_->GetShared(board_.hash) : nullptr);
    committed_ = Capture();
    ++revision_;
    btDirtyValid_ = false;
    boardDirtyValid_ = false;
}

void BehaviorTreeEditModel::PushUndo(const EditSnapshot& snapshot)
{
    undo_.push_back(snapshot);
    while (undo_.size() > kBtMaxUndoSteps) {
        undo_.pop_front();
    }
}

bool BehaviorTreeEditModel::Undo()
{
    if (!loaded_ || !CanUndo()) {
        return false;
    }
    redo_.push_back(Capture());
    EditSnapshot snapshot = std::move(undo_.back());
    undo_.pop_back();
    Restore(std::move(snapshot));
    return true;
}

bool BehaviorTreeEditModel::Redo()
{
    if (!loaded_ || !CanRedo()) {
        return false;
    }
    PushUndo(Capture());
    EditSnapshot snapshot = std::move(redo_.back());
    redo_.pop_back();
    Restore(std::move(snapshot));
    return true;
}

bool BehaviorTreeEditModel::BeginGesture()
{
    if (gesture_) {
        return false;
    }
    gesture_ = true;
    gesturePushed_ = false;
    return true;
}

void BehaviorTreeEditModel::EndGesture()
{
    if (!gesture_) {
        return;
    }
    if (gesturePushed_) {
        committed_ = Capture();
    }
    gesture_ = false;
    gesturePushed_ = false;
}

// ---- Blackboard ----

void BehaviorTreeEditModel::BindBoardRegistered(std::shared_ptr<const BlackboardAsset> registered)
{
    boardRegistered_ = std::move(registered);
    savedBoardJson_ = boardRegistered_ ? DumpBoard(*boardRegistered_) : std::string();
    boardDirtyValid_ = false;
}

void BehaviorTreeEditModel::LoadBoardFromRegistry()
{
    std::shared_ptr<const BlackboardAsset> registered
        = boards_ != nullptr && asset_.blackboard != 0 ? boards_->GetShared(asset_.blackboard) : nullptr;
    board_ = registered ? *registered : BlackboardAsset{};
    boardLoaded_ = registered != nullptr;
    BindBoardRegistered(std::move(registered));
}

void BehaviorTreeEditModel::SyncBoardWithRegistry()
{
    if (!loaded_ || boards_ == nullptr || gesture_) {
        return;
    }
    std::shared_ptr<const BlackboardAsset> registered = asset_.blackboard != 0 ? boards_->GetShared(asset_.blackboard) : nullptr;
    if (registered == boardRegistered_ || BoardDirty()) {
        return; // 変わっていない / 未保存の編集を守る
    }
    board_ = registered ? *registered : BlackboardAsset{};
    boardLoaded_ = registered != nullptr;
    BindBoardRegistered(std::move(registered));
    committed_ = Capture();
    ++revision_;
}

void BehaviorTreeEditModel::RenameKeyReferences(const std::string& from, const std::string& to)
{
    for (BtNodeDef& node : asset_.nodes) {
        for (std::string& key : node.keys) {
            if (key == from) {
                key = to;
            }
        }
        for (BtDecoratorDef& deco : node.decorators) {
            if (BtDecoratorTypeOf(deco.kind).hasKey && deco.key == from) {
                deco.key = to;
            }
        }
    }
}

int BehaviorTreeEditModel::AddBoardKey(BbType type)
{
    if (!boardLoaded_ || board_.keys.size() >= static_cast<size_t>(kBbMaxKeys)) {
        return -1;
    }
    std::string name = "NewKey";
    for (int n = 1; board_.FindKey(name) >= 0; ++n) {
        name = "NewKey" + std::to_string(n);
    }
    BbKeyDef key;
    key.name = name;
    key.type = type;
    board_.keys.push_back(std::move(key));
    Touch();
    return static_cast<int>(board_.keys.size()) - 1;
}

bool BehaviorTreeEditModel::RemoveBoardKey(int index)
{
    if (!boardLoaded_ || index < 0 || static_cast<size_t>(index) >= board_.keys.size()) {
        return false;
    }
    const std::string name = board_.keys[static_cast<size_t>(index)].name;
    board_.keys.erase(board_.keys.begin() + index);
    for (BtNodeDef& node : asset_.nodes) {
        for (std::string& key : node.keys) {
            if (key == name) {
                key.clear();
            }
        }
    }
    Touch();
    return true;
}

bool BehaviorTreeEditModel::RenameBoardKey(int index, const std::string& name)
{
    if (!boardLoaded_ || index < 0 || static_cast<size_t>(index) >= board_.keys.size() || name.empty() || name.size() > kBbMaxNameBytes) {
        return false;
    }
    BbKeyDef& key = board_.keys[static_cast<size_t>(index)];
    if (key.name == name || board_.FindKey(name) >= 0) {
        return false;
    }
    const std::string from = key.name;
    key.name = name;
    RenameKeyReferences(from, name);
    Touch();
    return true;
}

bool BehaviorTreeEditModel::SetBoardKeyType(int index, BbType type)
{
    if (!boardLoaded_ || index < 0 || static_cast<size_t>(index) >= board_.keys.size() || board_.keys[static_cast<size_t>(index)].type == type) {
        return false;
    }
    BbKeyDef& key = board_.keys[static_cast<size_t>(index)];
    key.type = type;
    key.initial = BbValue{};
    Touch();
    return true;
}

bool BehaviorTreeEditModel::SetBoardKeyInitial(int index, const BbValue& value)
{
    if (!boardLoaded_ || index < 0 || static_cast<size_t>(index) >= board_.keys.size()) {
        return false;
    }
    BbKeyDef& key = board_.keys[static_cast<size_t>(index)];
    BbValue normalized;
    if (value.isSet != 0) {
        normalized.isSet = 1;
        switch (key.type) {
        case BbType::Bool: normalized.i = value.i != 0 ? 1 : 0; break;
        case BbType::Int: normalized.i = value.i; break;
        case BbType::Float:
            if (!std::isfinite(value.f)) {
                return false;
            }
            normalized.f = value.f;
            break;
        case BbType::Vector:
            for (int axis = 0; axis < 3; ++axis) {
                if (!std::isfinite(value.v[axis])) {
                    return false;
                }
                normalized.v[axis] = value.v[axis];
            }
            break;
        case BbType::Entity:
            return false; // エンティティはアセットから指せない
        }
    }
    const BbValue& old = key.initial;
    const bool same = old.isSet == normalized.isSet && old.i == normalized.i && old.f == normalized.f && old.v[0] == normalized.v[0]
                      && old.v[1] == normalized.v[1] && old.v[2] == normalized.v[2];
    if (same) {
        return false;
    }
    key.initial = normalized;
    Touch();
    return true;
}

bool BehaviorTreeEditModel::SetBoardKeyEventName(int index, const std::string& eventName)
{
    if (!boardLoaded_ || index < 0 || static_cast<size_t>(index) >= board_.keys.size() || eventName.size() > kBbMaxNameBytes
        || board_.keys[static_cast<size_t>(index)].eventName == eventName) {
        return false;
    }
    board_.keys[static_cast<size_t>(index)].eventName = eventName;
    Touch();
    return true;
}

uint64_t BehaviorTreeEditModel::CreateBoardFile(const std::wstring& path)
{
    if (boards_ == nullptr) {
        return 0;
    }
    BlackboardAsset empty;
    if (!WriteFileReplacing(path, BlackboardLibrary::ToJson(empty).dump(2))) {
        return 0;
    }
    return boards_->LoadFromFile(path);
}

BtSaveResult BehaviorTreeEditModel::SaveBoard()
{
    if (!boardLoaded_ || boards_ == nullptr) {
        return BtSaveResult::NotLoaded;
    }
    if (!WriteFileReplacing(board_.path, BlackboardLibrary::ToJson(board_).dump(2))) {
        return BtSaveResult::WriteFailed;
    }
    const uint64_t hash = boards_->Register(board_.path, board_);
    BindBoardRegistered(boards_->GetShared(hash));
    return BtSaveResult::Ok;
}

// ---- 検査 ----

bool BehaviorTreeEditModel::IsTaskKind(BtNodeKind kind)
{
    const BtNodeCategory category = BtNodeTypeOf(kind).category;
    return category == BtNodeCategory::Task || category == BtNodeCategory::Ai || category == BtNodeCategory::Gameplay;
}

std::vector<BtIssue> BehaviorTreeEditModel::Inspect() const
{
    std::vector<BtIssue> issues;
    if (!loaded_) {
        return issues;
    }
    std::unordered_map<int32_t, int32_t> parentOf; // 子 -> 親 (childIds が正本)
    for (const BtNodeDef& node : asset_.nodes) {
        for (const int32_t child : node.childIds) {
            parentOf[child] = node.id;
        }
    }
    const BlackboardAsset* board = Board();
    const auto add = [&issues](BtIssueKind kind, BtIssueSeverity severity, int32_t nodeId, int decoratorIndex = -1, int keyIndex = -1, int32_t otherId = -1) {
        BtIssue issue;
        issue.kind = kind;
        issue.severity = severity;
        issue.nodeId = nodeId;
        issue.decoratorIndex = decoratorIndex;
        issue.keyIndex = keyIndex;
        issue.otherId = otherId;
        issues.push_back(issue);
    };
    // 名前 name が BB にあり、(typed のとき) 型がその欄に合うか。required のとき空は未設定
    const auto checkKey = [&](const BtNodeDef& node, const std::string& name, bool required, int decoratorIndex, int keyIndex, bool typed) {
        if (name.empty()) {
            if (required) {
                add(BtIssueKind::KeyUnset, BtIssueSeverity::Error, node.id, decoratorIndex, keyIndex);
            }
            return;
        }
        const int found = board != nullptr ? board->FindKey(name) : -1;
        if (found < 0) {
            add(BtIssueKind::KeyMissing, BtIssueSeverity::Error, node.id, decoratorIndex, keyIndex);
        } else if (typed && !KeyAccepts(node.kind, keyIndex, board->keys[static_cast<size_t>(found)].type)) {
            add(BtIssueKind::KeyTypeMismatch, BtIssueSeverity::Error, node.id, decoratorIndex, keyIndex);
        }
    };

    for (const BtNodeDef& node : asset_.nodes) {
        const BtNodeTypeInfo& info = BtNodeTypeOf(node.kind);
        const int count = static_cast<int>(node.childIds.size());
        if (count < info.minChildren || count > MaxChildren(node.kind)) {
            add(BtIssueKind::ChildCount, BtIssueSeverity::Error, node.id);
        }
        if (node.kind == BtNodeKind::SimpleParallel && !node.childIds.empty()) {
            const BtNodeDef* mainChild = FindNode(node.childIds.front());
            if (mainChild != nullptr && !IsTaskKind(mainChild->kind)) {
                add(BtIssueKind::ParallelMainNotTask, BtIssueSeverity::Error, node.id, -1, -1, mainChild->id);
            }
        }
        for (size_t d = 0; d < node.decorators.size(); ++d) {
            const BtDecoratorDef& deco = node.decorators[d];
            const BtDecoratorTypeInfo& decoInfo = BtDecoratorTypeOf(deco.kind);
            if (deco.kind == BtDecoratorKind::BlackboardCondition && static_cast<size_t>(btbbparam::kAbort) < deco.params.size()) {
                const int32_t abortMode = deco.params[btbbparam::kAbort].i;
                const auto parent = parentOf.find(node.id);
                if ((abortMode == btabort::kLowerPriority || abortMode == btabort::kBoth) && parent != parentOf.end()
                    && FindNode(parent->second)->kind != BtNodeKind::Selector) {
                    add(BtIssueKind::LowerPriorityParent, BtIssueSeverity::Error, node.id, static_cast<int>(d));
                }
            }
            if (decoInfo.hasKey) {
                checkKey(node, deco.key, true, static_cast<int>(d), -1, /*typed=*/false);
            }
        }
        for (int i = 0; i < info.keyCount && static_cast<size_t>(i) < node.keys.size(); ++i) {
            checkKey(node, node.keys[static_cast<size_t>(i)], KeyRequired(node, i), -1, i, /*typed=*/true);
        }
        if (node.kind == BtNodeKind::SubTree && static_cast<size_t>(btsubtreeparam::kTree) < node.params.size()) {
            // 取り込めない条件は BtExpandSubTrees と同じ (未指定・未登録・根なし・BB 違い)
            const uint64_t guid = node.params[btsubtreeparam::kTree].u;
            const BehaviorTreeAsset* sub = guid != 0 && trees_ != nullptr ? trees_->Get(guid) : nullptr;
            if (sub == nullptr || sub->rootId < 0) {
                add(BtIssueKind::SubTreeUnresolved, BtIssueSeverity::Warning, node.id);
            } else if (sub->blackboard != asset_.blackboard) {
                add(BtIssueKind::SubTreeBoardMismatch, BtIssueSeverity::Error, node.id);
            }
        }
    }
    return issues;
}

// ---- 保存 ----

BtSaveCheck BehaviorTreeEditModel::CheckSavable() const
{
    BtSaveCheck check;
    for (const BtNodeDef& node : asset_.nodes) {
        const BtNodeTypeInfo& info = BtNodeTypeOf(node.kind);
        const int count = static_cast<int>(node.childIds.size());
        if (count < info.minChildren || count > MaxChildren(node.kind)) {
            check.problem = BtSaveProblem::ChildCount;
            check.nodeId = node.id;
            return check;
        }
    }
    BehaviorTreeAsset linked = asset_;
    if (!BtLinkAsset(linked)) {
        check.problem = BtSaveProblem::Structure;
    }
    return check;
}

BtSaveResult BehaviorTreeEditModel::Save(BtSaveCheck* blocked)
{
    if (!loaded_) {
        return BtSaveResult::NotLoaded;
    }
    const BtSaveCheck check = CheckSavable();
    if (blocked != nullptr) {
        *blocked = check;
    }
    if (check.problem != BtSaveProblem::None) {
        return BtSaveResult::Blocked;
    }
    BehaviorTreeAsset linked = asset_;
    if (!BtLinkAsset(linked)) {
        return BtSaveResult::Blocked; // CheckSavable が通っていれば来ない
    }
    const std::string text = BehaviorTreeLibrary::ToJson(linked).dump(2);
    if (!WriteFileReplacing(linked.path, text)) {
        return BtSaveResult::WriteFailed;
    }
    // ReloadHub の再読込を待たず、直接ライブラリへ反映する。走っている木は次の tick に Abort して根からやり直す (spec 4.1.9)
    if (trees_ != nullptr) {
        const uint64_t hash = trees_->Register(linked.path, linked);
        registered_ = trees_->GetShared(hash);
    }
    savedBtJson_ = DumpAsset(asset_);
    btDirtyValid_ = false;
    return BtSaveResult::Ok;
}

} // namespace mye
