//====================================================================================
//                          BehaviorTreeWindow.cpp
//  MyEngin/ 秋田蓮音                                                       10/06/2026
//                                          ビヘイビアツリーのグラフエディタ窓の実装
//====================================================================================
#include "Editor/Windows/AI/BehaviorTreeWindow.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "Editor/SourceControl/ScmHint.h" // 保存直後に status を取り直させる
#include "Engine/Core/Localization/Localization.h"
#include "Engine/Engine/AI/BehaviorTreeLibrary.h"
#include "Engine/Engine/AI/BlackboardLibrary.h"
#include "Engine/Engine/Navigation/NavFilterLibrary.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/ImGui/ImGuiTheme.h" // themeColor
#include "imgui_internal.h"                    // SetKeyOwner (Delete キーを握る)

namespace mye {

namespace {

constexpr float kMinZoom = 0.25f;
constexpr float kMaxZoom = 2.0f;
constexpr float kZoomStep = 1.1f;            // ホイール 1 目盛りの倍率
constexpr float kLeftPanelWidth = 320.0f;
constexpr float kPaletteHeightRatio = 0.38f; // 左の欄のうちパレットが占める高さ
constexpr float kPinRadius = 5.0f;           // 接続の点の半径 (zoom 1 のとき px)
constexpr float kPinHitRadius = 9.0f;        // 点を掴める半径 (px)
constexpr float kNodeDragThreshold = 3.0f;   // これ以上動かすまでクリックとして扱う (px)
constexpr float kGridSpacing = 64.0f;        // 背景の格子 (グラフ座標)
constexpr float kFitMargin = 40.0f;          // 全体表示の余白 (px)
constexpr float kMinTextPixels = 7.0f;       // これより小さい文字は描かない
constexpr const char* kPaletteDragType = "MYE_BT_NODE_KIND";

ImU32 FromHsv(float hue, float saturation, float value, float alpha = 1.0f)
{
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    ImGui::ColorConvertHSVtoRGB(hue, saturation, value, r, g, b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, alpha));
}

// 分類ごとの箱の色 (彩度・明度は暗い面に収まる帯。色相だけで分ける)
ImU32 CategoryColor(BtNodeCategory category)
{
    switch (category) {
    case BtNodeCategory::Composite: return FromHsv(0.58f, 0.45f, 0.60f);
    case BtNodeCategory::Task: return FromHsv(0.36f, 0.42f, 0.55f);
    case BtNodeCategory::Ai: return FromHsv(0.76f, 0.40f, 0.62f);
    case BtNodeCategory::Gameplay: return FromHsv(0.07f, 0.45f, 0.65f);
    case BtNodeCategory::Tree: return FromHsv(0.48f, 0.42f, 0.58f);
    }
    return FromHsv(0.0f, 0.0f, 0.5f);
}

ImU32 DecoratorColor()
{
    return FromHsv(0.13f, 0.40f, 0.50f);
}

StrId CategoryLabel(BtNodeCategory category)
{
    switch (category) {
    case BtNodeCategory::Composite: return StrId::Bt_CatComposite;
    case BtNodeCategory::Task: return StrId::Bt_CatTask;
    case BtNodeCategory::Ai: return StrId::Bt_CatAi;
    case BtNodeCategory::Gameplay: return StrId::Bt_CatGameplay;
    case BtNodeCategory::Tree: return StrId::Bt_CatTree;
    }
    return StrId::Bt_CatTask;
}

constexpr BtNodeCategory kCategoryOrder[] = { BtNodeCategory::Composite, BtNodeCategory::Task, BtNodeCategory::Ai,
                                              BtNodeCategory::Gameplay, BtNodeCategory::Tree };

std::string ParamText(const BtParamDesc& desc, const BtParamValue& value)
{
    char buf[64];
    switch (desc.type) {
    case BtParamType::Int: std::snprintf(buf, sizeof(buf), "%d", value.i); return buf;
    case BtParamType::Float: std::snprintf(buf, sizeof(buf), "%.3g", value.f); return buf;
    case BtParamType::Bool: return value.i != 0 ? "true" : "false";
    case BtParamType::Enum: return desc.enumNames[(std::clamp)(value.i, 0, desc.enumCount - 1)];
    case BtParamType::Guid:
    case BtParamType::Mask:
        if (value.u == 0) {
            return "-";
        }
        std::snprintf(buf, sizeof(buf), "%llx", static_cast<unsigned long long>(value.u));
        return buf;
    case BtParamType::String: return value.s.empty() ? std::string("-") : value.s;
    }
    return std::string();
}

// 箱の 2 行目: 指定したキー、無ければ先頭のパラメータ
std::string NodeSummary(const BtNodeDef& node)
{
    const BtNodeTypeInfo& info = BtNodeTypeOf(node.kind);
    std::string text;
    for (int i = 0; i < info.keyCount && static_cast<size_t>(i) < node.keys.size(); ++i) {
        if (!node.keys[static_cast<size_t>(i)].empty()) {
            text += (text.empty() ? "" : ", ") + std::string(info.keyNames[i]) + "=" + node.keys[static_cast<size_t>(i)];
        }
    }
    if (text.empty() && info.paramCount > 0 && !node.params.empty()) {
        text = std::string(info.params[0].name) + "=" + ParamText(info.params[0], node.params[0]);
    }
    return text;
}

// Decorator の帯の文字
std::string DecoratorSummary(const BtDecoratorDef& deco)
{
    const BtDecoratorTypeInfo& info = BtDecoratorTypeOf(deco.kind);
    std::string text = info.name;
    if (info.hasKey) {
        text += " " + deco.key;
    }
    if (info.paramCount > 0 && !deco.params.empty()) {
        text += " " + ParamText(info.params[0], deco.params[0]);
    }
    return text;
}

// アセット参照の欄が何を指すか (パラメータ名で決める。種類表は参照先の種類を持たない)
enum class GuidTarget : uint8_t { BehaviorTree, NavFilter, Other };

GuidTarget GuidTargetOf(const BtParamDesc& desc)
{
    if (std::strcmp(desc.name, "tree") == 0) {
        return GuidTarget::BehaviorTree;
    }
    if (std::strcmp(desc.name, "navFilter") == 0) {
        return GuidTarget::NavFilter;
    }
    return GuidTarget::Other;
}

} // namespace

// ---------------------------------------------------------------------------
// 開く・保存
// ---------------------------------------------------------------------------

void BehaviorTreeWindow::OpenAsset(const std::wstring& path)
{
    open = true;
    BehaviorTreeLibrary* trees = behaviortree::Library();
    if (trees == nullptr) {
        return;
    }
    // 登録済みの木は読み直さない (Register し直すと走っている木が Abort -> やり直しになる)
    uint64_t guid = BehaviorTreeLibrary::HashForPath(path);
    if (!trees->Contains(guid)) {
        guid = trees->LoadFromFile(path);
        if (guid == 0) {
            status_ = StatusKind::LoadFailed;
            statusText_ = WideToUtf8(path);
            return;
        }
    }
    RequestOpen(guid);
}

void BehaviorTreeWindow::RequestOpen(uint64_t guid)
{
    if (model_.IsLoaded() && model_.Asset().hash == guid) {
        open = true; // 開いている木をもう一度開いても、編集中の内容を読み直さない
        return;
    }
    if (model_.IsLoaded() && model_.Dirty()) {
        pendingGuid_ = guid;
        openModal_ = true;
        return;
    }
    OpenGuid(guid);
}

void BehaviorTreeWindow::OpenGuid(uint64_t guid)
{
    BehaviorTreeLibrary* trees = behaviortree::Library();
    std::shared_ptr<const BehaviorTreeAsset> asset = trees != nullptr ? trees->GetShared(guid) : nullptr;
    model_.BindLibraries(trees, blackboard::Library());
    if (!asset || !model_.Load(std::move(asset))) {
        status_ = StatusKind::LoadFailed;
        statusText_ = std::to_string(guid);
        return;
    }
    selected_ = -1;
    drag_ = DragMode::None;
    needFit_ = true;
    status_ = StatusKind::None;
    open = true;
}

// 別の経路 (ファイルの外部編集 -> ReloadHub、別の保存) で登録が差し替わったとき、未保存の編集が無ければ読み直す
void BehaviorTreeWindow::ReloadFromRegistry()
{
    if (!model_.IsLoaded() || model_.IsRegistered() || model_.Dirty()) {
        return;
    }
    BehaviorTreeLibrary* trees = behaviortree::Library();
    std::shared_ptr<const BehaviorTreeAsset> asset = trees != nullptr ? trees->GetShared(model_.Asset().hash) : nullptr;
    if (!asset) {
        model_.Clear();
        selected_ = -1;
        return;
    }
    model_.Load(std::move(asset));
}

void BehaviorTreeWindow::DoSave()
{
    BtSaveCheck check;
    switch (model_.Save(&check)) {
    case BtSaveResult::Ok:
        status_ = StatusKind::Saved;
        statusText_ = model_.Asset().name + ".bt.json";
        scmhint::Changed(model_.Asset().path); // バッジと Changes の即時反映
        break;
    case BtSaveResult::Blocked:
        status_ = check.problem == BtSaveProblem::ChildCount ? StatusKind::BlockedChildren : StatusKind::BlockedStructure;
        statusNode_ = check.nodeId;
        break;
    case BtSaveResult::WriteFailed:
        status_ = StatusKind::SaveFailed;
        break;
    case BtSaveResult::NotLoaded:
        break;
    }
}

// ---------------------------------------------------------------------------
// 窓
// ---------------------------------------------------------------------------

void BehaviorTreeWindow::OnImGui()
{
    if (!open) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(1040.0f, 640.0f), ImGuiCond_FirstUseEver);
    // 窓名はアイコン無しの素の文字列 (LayoutManager / DockBuilder / Window メニューがこの文字列で参照する)
    if (!ImGui::Begin(Tr(StrId::Win_BehaviorTree), &open)) {
        ImGui::End();
        return;
    }
    BehaviorTreeLibrary* trees = behaviortree::Library();
    model_.BindLibraries(trees, blackboard::Library());
    if (trees == nullptr) {
        ImGui::TextDisabled("%s", Tr(StrId::Bt_NoLibrary));
        ImGui::End();
        return;
    }
    ReloadFromRegistry();
    if (selected_ >= 0 && model_.FindNode(selected_) == nullptr) {
        selected_ = -1;
    }
    if (dragNode_ >= 0 && model_.FindNode(dragNode_) == nullptr) {
        drag_ = DragMode::None;
        dragNode_ = -1;
    }

    DrawToolbar();
    ImGui::Separator();
    DrawLeftPanel();
    ImGui::SameLine();
    DrawCanvas();
    DrawUnsavedModal();
    ImGui::End();
}

void BehaviorTreeWindow::DrawToolbar()
{
    BehaviorTreeLibrary* trees = behaviortree::Library();
    if (ImGui::BeginCombo("##btopen", Tr(StrId::Bt_Open), ImGuiComboFlags_WidthFitPreview)) {
        for (const BehaviorTreeEntry& entry : trees->Enumerate()) {
            ImGui::PushID(static_cast<int>(entry.hash & 0x7fffffff));
            if (ImGui::Selectable(entry.name.c_str(), model_.IsLoaded() && model_.Asset().hash == entry.hash)) {
                RequestOpen(entry.hash);
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!model_.IsLoaded() || !model_.Dirty());
    if (ImGui::Button(Tr(StrId::Common_Save))) {
        DoSave();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!model_.IsLoaded());
    if (ImGui::Button(Tr(StrId::Bt_AutoLayout))) {
        model_.AutoLayout();
        needFit_ = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr(StrId::Bt_FitView))) {
        needFit_ = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", Tr(StrId::Bt_CanvasHint));
    }
    if (model_.IsLoaded()) {
        ImGui::SameLine();
        ImGui::TextUnformatted((model_.Asset().name + ".bt.json").c_str());
        if (model_.Dirty()) {
            ImGui::SameLine();
            ImGui::TextColored(themeColor::Warning, "%s", Tr(StrId::Bt_Unsaved));
        }
    }

    char text[256];
    switch (status_) {
    case StatusKind::None:
        break;
    case StatusKind::Saved:
        std::snprintf(text, sizeof(text), Tr(StrId::Bt_Saved), statusText_.c_str());
        ImGui::TextColored(themeColor::Success, "%s", text);
        break;
    case StatusKind::BlockedChildren:
        std::snprintf(text, sizeof(text), Tr(StrId::Bt_SaveBlockedChildren), statusNode_);
        ImGui::TextColored(themeColor::Error, "%s", text);
        break;
    case StatusKind::BlockedStructure:
        ImGui::TextColored(themeColor::Error, "%s", Tr(StrId::Bt_SaveBlockedStructure));
        break;
    case StatusKind::SaveFailed:
        ImGui::TextColored(themeColor::Error, "%s", Tr(StrId::Bt_SaveFailed));
        break;
    case StatusKind::LoadFailed:
        std::snprintf(text, sizeof(text), Tr(StrId::Bt_LoadFailed), statusText_.c_str());
        ImGui::TextColored(themeColor::Error, "%s", text);
        break;
    }
    if (model_.IsLoaded() && model_.Dirty() && !model_.IsRegistered()) {
        ImGui::TextColored(themeColor::Warning, "%s", Tr(StrId::Bt_ExternalChange));
    }
}

void BehaviorTreeWindow::DrawUnsavedModal()
{
    if (openModal_) {
        ImGui::OpenPopup(Tr(StrId::Bt_ModalTitle));
        openModal_ = false;
    }
    if (!ImGui::BeginPopupModal(Tr(StrId::Bt_ModalTitle), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    char text[256];
    std::snprintf(text, sizeof(text), Tr(StrId::Bt_ModalBody), model_.Asset().name.c_str());
    ImGui::TextUnformatted(text);
    if (ImGui::Button(Tr(StrId::Bt_ModalSave))) {
        DoSave();
        if (!model_.Dirty()) {
            OpenGuid(pendingGuid_);
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr(StrId::Bt_ModalDiscard))) {
        OpenGuid(pendingGuid_);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr(StrId::Common_Cancel))) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ---------------------------------------------------------------------------
// 左: パレットとパラメータ欄
// ---------------------------------------------------------------------------

void BehaviorTreeWindow::DrawLeftPanel()
{
    ImGui::BeginChild("##btleft", ImVec2(kLeftPanelWidth, 0.0f), ImGuiChildFlags_None);
    DrawPalette(ImGui::GetContentRegionAvail().y * kPaletteHeightRatio);
    DrawProperties();
    ImGui::EndChild();
}

void BehaviorTreeWindow::DrawPalette(float height)
{
    ImGui::BeginChild("##btpalette", ImVec2(0.0f, height), ImGuiChildFlags_Borders);
    ImGui::TextDisabled("%s", Tr(StrId::Bt_Palette));
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", Tr(StrId::Bt_PaletteHint));
    }
    // 種類表から分類ごとに自動で並べる (ノードの種類を足せば窓の変更なしで出る)
    for (const BtNodeCategory category : kCategoryOrder) {
        if (!ImGui::CollapsingHeader(Tr(CategoryLabel(category)), ImGuiTreeNodeFlags_DefaultOpen)) {
            continue;
        }
        for (int k = 0; k < static_cast<int>(BtNodeKind::Count); ++k) {
            const BtNodeKind kind = static_cast<BtNodeKind>(k);
            const BtNodeTypeInfo& info = BtNodeTypeOf(kind);
            if (info.category != category) {
                continue;
            }
            ImGui::PushID(k);
            ImGui::BeginDisabled(!model_.IsLoaded());
            if (ImGui::Selectable(info.name)) {
                const ImVec2 center = CanvasCenterGraph();
                AddNodeAt(kind, center.x - kBtNodeWidth * 0.5f, center.y);
            }
            ImGui::EndDisabled();
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                const int32_t payload = k;
                ImGui::SetDragDropPayload(kPaletteDragType, &payload, sizeof(payload));
                ImGui::TextUnformatted(info.name);
                ImGui::EndDragDropSource();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                const int max = BehaviorTreeEditModel::MaxChildren(kind);
                char tip[96];
                if (max == 0) {
                    std::snprintf(tip, sizeof(tip), "%s", Tr(StrId::Bt_ChildrenNone));
                } else if (max >= kBtMaxNodes) {
                    std::snprintf(tip, sizeof(tip), "%s", Tr(StrId::Bt_ChildrenAny));
                } else {
                    std::snprintf(tip, sizeof(tip), Tr(StrId::Bt_ChildrenRange), info.minChildren, max);
                }
                ImGui::SetTooltip("%s", tip);
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

void BehaviorTreeWindow::DrawProperties()
{
    ImGui::BeginChild("##btprops", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
    if (!model_.IsLoaded()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", Tr(StrId::Bt_NothingOpen));
        ImGui::PopTextWrapPos();
    } else {
        DrawTreeSettings();
        if (selected_ >= 0) {
            ImGui::Separator();
            DrawNodeProperties(selected_);
        }
    }
    ImGui::EndChild();
}

// 木全体の設定: ノード数・根・使う BB
void BehaviorTreeWindow::DrawTreeSettings()
{
    char text[96];
    if (model_.RootId() >= 0) {
        std::snprintf(text, sizeof(text), Tr(StrId::Bt_TreeInfo), static_cast<int>(model_.Asset().nodes.size()), model_.RootId());
    } else {
        std::snprintf(text, sizeof(text), Tr(StrId::Bt_NoRoot), static_cast<int>(model_.Asset().nodes.size()));
    }
    ImGui::TextUnformatted(text);

    BlackboardLibrary* boards = blackboard::Library();
    const uint64_t current = model_.Asset().blackboard;
    const BlackboardAsset* board = model_.Board();
    char preview[96];
    if (current == 0) {
        std::snprintf(preview, sizeof(preview), "%s", Tr(StrId::Bt_BlackboardNone));
    } else if (board != nullptr) {
        std::snprintf(preview, sizeof(preview), "%s", board->name.c_str());
    } else {
        std::snprintf(preview, sizeof(preview), Tr(StrId::Bt_BlackboardMissing), static_cast<unsigned long long>(current));
    }
    if (ImGui::BeginCombo(Tr(StrId::Bt_Blackboard), preview)) {
        if (ImGui::Selectable(Tr(StrId::Bt_BlackboardNone), current == 0)) {
            model_.SetBlackboard(0);
        }
        if (boards != nullptr) {
            for (const BlackboardEntry& entry : boards->Enumerate()) {
                ImGui::PushID(static_cast<int>(entry.hash & 0x7fffffff));
                if (ImGui::Selectable(entry.name.c_str(), entry.hash == current)) {
                    model_.SetBlackboard(entry.hash);
                }
                ImGui::PopID();
            }
        }
        ImGui::EndCombo();
    }
}

bool BehaviorTreeWindow::DrawKeyCombo(const char* label, const std::string& current, bool allowNone,
                                      const std::function<bool(BbType)>& accepts, std::string& chosen)
{
    const BlackboardAsset* board = model_.Board();
    char preview[160];
    if (current.empty()) {
        std::snprintf(preview, sizeof(preview), "%s", Tr(StrId::Bt_KeyNone));
    } else if (board != nullptr && board->FindKey(current) >= 0) {
        std::snprintf(preview, sizeof(preview), "%s", current.c_str());
    } else {
        std::snprintf(preview, sizeof(preview), Tr(StrId::Bt_KeyMissing), current.c_str());
    }
    bool picked = false;
    if (ImGui::BeginCombo(label, preview)) {
        if (allowNone && ImGui::Selectable(Tr(StrId::Bt_KeyNone), current.empty())) {
            chosen.clear();
            picked = true;
        }
        if (board == nullptr) {
            ImGui::TextDisabled("%s", Tr(StrId::Bt_NoBoardForKeys));
        } else {
            for (size_t i = 0; i < board->keys.size(); ++i) {
                const BbKeyDef& key = board->keys[i];
                if (!accepts(key.type)) {
                    continue;
                }
                const std::string text = key.name + "  (" + BbTypeName(key.type) + ")";
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::Selectable(text.c_str(), key.name == current)) {
                    chosen = key.name;
                    picked = true;
                }
                ImGui::PopID();
            }
        }
        ImGui::EndCombo();
    }
    return picked;
}

// パラメータ 1 つの欄。種類表の記述子から作る。値が変わったら true
bool BehaviorTreeWindow::DrawParam(const BtParamDesc& desc, BtParamValue& value)
{
    bool changed = false;
    switch (desc.type) {
    case BtParamType::Int: {
        int v = value.i;
        const float range = desc.maxValue - desc.minValue;
        if (ImGui::DragInt(desc.name, &v, (std::clamp)(range / 2000.0f, 1.0f, 100.0f), static_cast<int>(desc.minValue),
                           static_cast<int>(desc.maxValue))) {
            value.i = v;
            changed = true;
        }
        break;
    }
    case BtParamType::Float: {
        float v = value.f;
        const float range = desc.maxValue - desc.minValue;
        if (ImGui::DragFloat(desc.name, &v, (std::clamp)(range / 400.0f, 0.02f, 1.0f), desc.minValue, desc.maxValue, "%.3f")) {
            value.f = v;
            changed = true;
        }
        break;
    }
    case BtParamType::Bool: {
        bool v = value.i != 0;
        if (ImGui::Checkbox(desc.name, &v)) {
            value.i = v ? 1 : 0;
            changed = true;
        }
        break;
    }
    case BtParamType::Enum: {
        const int current = (std::clamp)(value.i, 0, desc.enumCount - 1);
        if (ImGui::BeginCombo(desc.name, desc.enumNames[current])) {
            for (int i = 0; i < desc.enumCount; ++i) {
                if (ImGui::Selectable(desc.enumNames[i], i == current)) {
                    value.i = i;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        break;
    }
    case BtParamType::Guid: {
        const GuidTarget target = GuidTargetOf(desc);
        if (target == GuidTarget::Other) {
            // 参照先の種類が分からない欄は 16 進で直接書く (Mask と同じ)
            char buf[24] = {};
            if (value.u != 0) {
                std::snprintf(buf, sizeof(buf), "%llx", static_cast<unsigned long long>(value.u));
            }
            if (ImGui::InputText(desc.name, buf, sizeof(buf), ImGuiInputTextFlags_CharsHexadecimal)) {
                value.u = std::strtoull(buf, nullptr, 16);
                changed = true;
            }
            break;
        }
        std::vector<std::pair<uint64_t, std::string>> entries;
        if (target == GuidTarget::BehaviorTree) {
            if (BehaviorTreeLibrary* trees = behaviortree::Library()) {
                for (const BehaviorTreeEntry& entry : trees->Enumerate()) {
                    if (entry.hash != model_.Asset().hash) { // 自分自身は入れ子にできない
                        entries.emplace_back(entry.hash, entry.name);
                    }
                }
            }
        } else if (NavFilterLibrary* filters = navfilter::Library()) {
            for (const NavFilterEntry& entry : filters->Enumerate()) {
                entries.emplace_back(entry.hash, entry.name);
            }
        }
        char preview[96] = {};
        if (value.u == 0) {
            std::snprintf(preview, sizeof(preview), "%s", Tr(StrId::Bt_BlackboardNone));
        } else {
            const auto it = std::find_if(entries.begin(), entries.end(), [&value](const auto& e) { return e.first == value.u; });
            if (it != entries.end()) {
                std::snprintf(preview, sizeof(preview), "%s", it->second.c_str());
            } else {
                std::snprintf(preview, sizeof(preview), Tr(StrId::Bt_BlackboardMissing), static_cast<unsigned long long>(value.u));
            }
        }
        if (ImGui::BeginCombo(desc.name, preview)) {
            if (ImGui::Selectable(Tr(StrId::Bt_BlackboardNone), value.u == 0)) {
                value.u = 0;
                changed = true;
            }
            for (size_t i = 0; i < entries.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::Selectable(entries[i].second.c_str(), entries[i].first == value.u)) {
                    value.u = entries[i].first;
                    changed = true;
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        break;
    }
    case BtParamType::Mask: {
        char buf[24] = {};
        if (value.u != 0) {
            std::snprintf(buf, sizeof(buf), "%llx", static_cast<unsigned long long>(value.u));
        }
        if (ImGui::InputText(desc.name, buf, sizeof(buf), ImGuiInputTextFlags_CharsHexadecimal)) {
            value.u = std::strtoull(buf, nullptr, 16);
            changed = true;
        }
        break;
    }
    case BtParamType::String: {
        char buf[kBbMaxNameBytes + 1] = {};
        std::snprintf(buf, sizeof(buf), "%s", value.s.c_str());
        if (ImGui::InputText(desc.name, buf, sizeof(buf))) {
            value.s = buf;
            changed = true;
        }
        break;
    }
    }
    return changed;
}

void BehaviorTreeWindow::DrawNodeProperties(int32_t id)
{
    const BtNodeDef* node = model_.FindNode(id);
    if (node == nullptr) {
        return;
    }
    const BtNodeTypeInfo& info = BtNodeTypeOf(node->kind);
    ImGui::PushID(id);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(CategoryColor(info.category)), "%s", info.name);
    ImGui::SameLine();
    ImGui::TextDisabled("#%d", id);

    const int32_t parentId = model_.ParentOf(id);
    char text[96];
    if (parentId >= 0) {
        std::snprintf(text, sizeof(text), Tr(StrId::Bt_ParentInfo), parentId, model_.ChildOrderOf(id) + 1);
        ImGui::TextDisabled("%s", text);
    } else {
        ImGui::TextDisabled("%s", Tr(StrId::Bt_NoParent));
    }

    ImGui::BeginDisabled(parentId < 0);
    if (ImGui::Button(Tr(StrId::Bt_Disconnect))) {
        model_.Disconnect(id);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(parentId >= 0 || model_.RootId() == id);
    if (ImGui::Button(Tr(StrId::Bt_SetRoot))) {
        model_.SetRoot(id);
    }
    ImGui::EndDisabled();
    if (ImGui::Button(Tr(StrId::Bt_DeleteKeep))) {
        DeleteSelected(BtRemoveMode::KeepChildren);
        ImGui::PopID();
        return;
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr(StrId::Bt_DeleteAll))) {
        DeleteSelected(BtRemoveMode::WithDescendants);
        ImGui::PopID();
        return;
    }

    // ---- ブラックボードのキー ----
    if (info.keyCount > 0) {
        ImGui::Separator();
        ImGui::TextUnformatted(Tr(StrId::Bt_Keys));
        for (int i = 0; i < info.keyCount; ++i) {
            std::string chosen;
            ImGui::PushID(i);
            const BtNodeKind kind = node->kind;
            if (DrawKeyCombo(info.keyNames[i], node->keys[static_cast<size_t>(i)], /*allowNone=*/true,
                             [kind, i](BbType type) { return BehaviorTreeEditModel::KeyAccepts(kind, i, type); }, chosen)) {
                model_.SetKey(id, i, chosen);
            }
            ImGui::PopID();
        }
    }

    // ---- パラメータ (種類表から自動生成) ----
    if (info.paramCount > 0) {
        ImGui::Separator();
        ImGui::TextUnformatted(Tr(StrId::Bt_Params));
        for (int i = 0; i < info.paramCount; ++i) {
            const BtParamDesc& desc = info.params[i];
            ImGui::PushID(i);
            // vectorX / vectorY / vectorZ は 3 つ並べて 1 つの Vector の欄にする
            if (std::strcmp(desc.name, "vectorX") == 0 && i + 2 < info.paramCount && std::strcmp(info.params[i + 1].name, "vectorY") == 0
                && std::strcmp(info.params[i + 2].name, "vectorZ") == 0) {
                float v[3] = { node->params[static_cast<size_t>(i)].f, node->params[static_cast<size_t>(i) + 1].f,
                               node->params[static_cast<size_t>(i) + 2].f };
                if (ImGui::DragFloat3("vector", v, 0.05f, desc.minValue, desc.maxValue)) {
                    for (int axis = 0; axis < 3; ++axis) {
                        BtParamValue value;
                        value.f = v[axis];
                        model_.SetParam(id, i + axis, value);
                    }
                }
                i += 2;
                ImGui::PopID();
                continue;
            }
            BtParamValue value = node->params[static_cast<size_t>(i)];
            if (DrawParam(desc, value)) {
                model_.SetParam(id, i, value);
            }
            ImGui::PopID();
        }
    }

    // ---- Decorator (上から順に評価) ----
    ImGui::Separator();
    ImGui::TextUnformatted(Tr(StrId::Bt_Decorators));
    int removeIndex = -1;
    int moveFrom = -1;
    int moveTo = -1;
    const int decoratorCount = static_cast<int>(node->decorators.size());
    for (int d = 0; d < decoratorCount; ++d) {
        const BtDecoratorDef& deco = node->decorators[static_cast<size_t>(d)];
        const BtDecoratorTypeInfo& decoInfo = BtDecoratorTypeOf(deco.kind);
        ImGui::PushID(d);
        if (ImGui::TreeNodeEx("##deco", ImGuiTreeNodeFlags_DefaultOpen, "%d. %s", d + 1, decoInfo.name)) {
            ImGui::BeginDisabled(d == 0);
            if (ImGui::SmallButton(Tr(StrId::Bt_Up))) {
                moveFrom = d;
                moveTo = d - 1;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(d == decoratorCount - 1);
            if (ImGui::SmallButton(Tr(StrId::Bt_Down))) {
                moveFrom = d;
                moveTo = d + 1;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::SmallButton(Tr(StrId::Bt_Remove))) {
                removeIndex = d;
            }
            if (decoInfo.hasKey) {
                std::string chosen;
                if (DrawKeyCombo("key", deco.key, /*allowNone=*/false, [](BbType) { return true; }, chosen)) {
                    model_.SetDecoratorKey(id, d, chosen);
                }
            }
            // BlackboardCondition は比べるキーの型に合う値の欄だけを出す (IsSet / IsNotSet は値なし)
            const BlackboardAsset* board = model_.Board();
            const int keyIndex = board != nullptr ? board->FindKey(deco.key) : -1;
            const BbType keyType = keyIndex >= 0 ? board->keys[static_cast<size_t>(keyIndex)].type : BbType::Int;
            for (int p = 0; p < decoInfo.paramCount; ++p) {
                if (deco.kind == BtDecoratorKind::BlackboardCondition && (p == btbbparam::kIntValue || p == btbbparam::kFloatValue)) {
                    const int32_t query = deco.params[btbbparam::kQuery].i;
                    const bool noValue = query == btquery::kIsSet || query == btquery::kIsNotSet;
                    const bool floatKey = keyIndex >= 0 && keyType == BbType::Float;
                    if (noValue || (p == btbbparam::kFloatValue) != floatKey) {
                        continue;
                    }
                }
                ImGui::PushID(p);
                BtParamValue value = deco.params[static_cast<size_t>(p)];
                if (DrawParam(decoInfo.params[p], value)) {
                    model_.SetDecoratorParam(id, d, p, value);
                }
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (removeIndex >= 0) {
        model_.RemoveDecorator(id, removeIndex);
    } else if (moveFrom >= 0) {
        model_.MoveDecorator(id, moveFrom, moveTo);
    }

    if (decoratorCount >= kBtMaxDecoratorsPerNode) {
        ImGui::TextDisabled("%s", Tr(StrId::Bt_DecoratorFull));
    } else if (ImGui::BeginCombo("##adddeco", Tr(StrId::Bt_AddDecorator))) {
        const BlackboardAsset* board = model_.Board();
        for (int k = 0; k < static_cast<int>(BtDecoratorKind::Count); ++k) {
            const BtDecoratorKind kind = static_cast<BtDecoratorKind>(k);
            const BtDecoratorTypeInfo& decoInfo = BtDecoratorTypeOf(kind);
            const bool needsKey = decoInfo.hasKey && (board == nullptr || board->keys.empty());
            ImGui::BeginDisabled(needsKey);
            if (ImGui::Selectable(decoInfo.name)) {
                model_.AddDecorator(id, kind);
            }
            ImGui::EndDisabled();
            if (needsKey && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("%s", Tr(StrId::Bt_DecoratorNeedsKey));
            }
        }
        ImGui::EndCombo();
    }
    ImGui::PopID();
}

void BehaviorTreeWindow::AddNodeAt(BtNodeKind kind, float gx, float gy)
{
    const int32_t id = model_.AddNode(kind, gx, gy);
    if (id >= 0) {
        selected_ = id;
    }
}

void BehaviorTreeWindow::DeleteSelected(BtRemoveMode mode)
{
    if (selected_ >= 0 && model_.Remove(selected_, mode)) {
        selected_ = -1;
        drag_ = DragMode::None;
    }
}

// ---------------------------------------------------------------------------
// キャンバス
// ---------------------------------------------------------------------------

ImVec2 BehaviorTreeWindow::CanvasCenterGraph() const
{
    return ImVec2((canvasSize_.x * 0.5f - pan_.x) / zoom_, (canvasSize_.y * 0.5f - pan_.y) / zoom_);
}

void BehaviorTreeWindow::FitView(const ImVec2& canvasSize)
{
    needFit_ = false;
    const std::vector<BtNodeDef>& nodes = model_.Asset().nodes;
    if (nodes.empty()) {
        pan_ = ImVec2(kFitMargin, kFitMargin);
        zoom_ = 1.0f;
        return;
    }
    float minX = nodes[0].pos[0];
    float minY = nodes[0].pos[1];
    float maxX = minX;
    float maxY = minY;
    for (const BtNodeDef& node : nodes) {
        minX = (std::min)(minX, node.pos[0]);
        minY = (std::min)(minY, node.pos[1]);
        maxX = (std::max)(maxX, node.pos[0] + kBtNodeWidth);
        maxY = (std::max)(maxY, node.pos[1] + BehaviorTreeEditModel::NodeHeight(node));
    }
    const float width = (std::max)(maxX - minX, 1.0f);
    const float height = (std::max)(maxY - minY, 1.0f);
    zoom_ = (std::clamp)((std::min)((canvasSize.x - kFitMargin * 2.0f) / width, (canvasSize.y - kFitMargin * 2.0f) / height), kMinZoom, 1.0f);
    pan_.x = (canvasSize.x - width * zoom_) * 0.5f - minX * zoom_;
    pan_.y = kFitMargin - minY * zoom_;
}

void BehaviorTreeWindow::DrawCanvas()
{
    ImGuiIO& io = ImGui::GetIO();
    ImGui::BeginChild("##btcanvas", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove);
    const ImVec2 canvasMin = ImGui::GetCursorScreenPos();
    ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    canvasSize.x = (std::max)(canvasSize.x, 50.0f);
    canvasSize.y = (std::max)(canvasSize.y, 50.0f);
    canvasMin_ = canvasMin;
    canvasSize_ = canvasSize;
    const ImVec2 canvasMax(canvasMin.x + canvasSize.x, canvasMin.y + canvasSize.y);
    if (needFit_ && model_.IsLoaded()) {
        FitView(canvasSize);
    }

    ImGui::InvisibleButton("##btcanvasbtn", canvasSize,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const bool loaded = model_.IsLoaded();

    const auto toScreen = [&](float gx, float gy) { return ImVec2(canvasMin.x + pan_.x + gx * zoom_, canvasMin.y + pan_.y + gy * zoom_); };
    const auto toGraph = [&](const ImVec2& p) { return ImVec2((p.x - canvasMin.x - pan_.x) / zoom_, (p.y - canvasMin.y - pan_.y) / zoom_); };

    // パレットからのドロップ
    if (loaded && ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kPaletteDragType)) {
            const int32_t kindIndex = *static_cast<const int32_t*>(payload->Data);
            if (kindIndex >= 0 && kindIndex < static_cast<int32_t>(BtNodeKind::Count)) {
                const ImVec2 g = toGraph(io.MousePos);
                AddNodeAt(static_cast<BtNodeKind>(kindIndex), g.x - kBtNodeWidth * 0.5f, g.y);
            }
        }
        ImGui::EndDragDropTarget();
    }

    // ---- ズーム (カーソルの下のグラフ座標を動かさない) ----
    if (loaded && hovered && io.MouseWheel != 0.0f && drag_ != DragMode::Connect) {
        const ImVec2 before = toGraph(io.MousePos);
        zoom_ = (std::clamp)(zoom_ * std::pow(kZoomStep, io.MouseWheel), kMinZoom, kMaxZoom);
        pan_.x = io.MousePos.x - canvasMin.x - before.x * zoom_;
        pan_.y = io.MousePos.y - canvasMin.y - before.y * zoom_;
    }

    const std::vector<BtNodeDef>& nodes = model_.Asset().nodes;
    const float nodeW = kBtNodeWidth * zoom_;
    const auto rectOf = [&](const BtNodeDef& node, ImVec2& outMin, ImVec2& outMax) {
        outMin = toScreen(node.pos[0], node.pos[1]);
        outMax = ImVec2(outMin.x + nodeW, outMin.y + BehaviorTreeEditModel::NodeHeight(node) * zoom_);
    };

    // 親の id (描画と点の判定用)。childIds が正本
    std::unordered_map<int32_t, int32_t> parentOf;
    for (const BtNodeDef& node : nodes) {
        for (const int32_t child : node.childIds) {
            parentOf[child] = node.id;
        }
    }

    // ---- 当たり判定 ----
    struct Hit {
        int32_t node = -1;
        bool outputPin = false;
    };
    const auto hitTest = [&](const ImVec2& p) {
        Hit hit;
        const float pinHit = (std::max)(kPinHitRadius, kPinRadius * zoom_ + 3.0f);
        for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
            if (BehaviorTreeEditModel::MaxChildren(it->kind) == 0) {
                continue;
            }
            ImVec2 mn;
            ImVec2 mx;
            rectOf(*it, mn, mx);
            const float dx = p.x - (mn.x + mx.x) * 0.5f;
            const float dy = p.y - mx.y;
            if (dx * dx + dy * dy <= pinHit * pinHit) {
                hit.node = it->id;
                hit.outputPin = true;
                return hit;
            }
        }
        for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
            ImVec2 mn;
            ImVec2 mx;
            rectOf(*it, mn, mx);
            if (p.x >= mn.x && p.x <= mx.x && p.y >= mn.y && p.y <= mx.y) {
                hit.node = it->id;
                return hit;
            }
        }
        return hit;
    };

    // ---- マウス操作 ----
    bool openNodeMenu = false;
    bool openCanvasMenu = false;
    if (loaded && hovered) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) || (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsKeyDown(ImGuiKey_Space))) {
            drag_ = DragMode::Pan;
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const Hit hit = hitTest(io.MousePos);
            selected_ = hit.node;
            dragNode_ = hit.node;
            if (hit.node >= 0) {
                drag_ = hit.outputPin ? DragMode::Connect : DragMode::Node;
            }
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            const Hit hit = hitTest(io.MousePos);
            contextGraphPos_ = toGraph(io.MousePos);
            if (hit.node >= 0) {
                selected_ = hit.node;
                contextNode_ = hit.node;
                openNodeMenu = true;
            } else {
                openCanvasMenu = true;
            }
        }
    }
    if (drag_ == DragMode::Pan) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Middle) || ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            pan_.x += io.MouseDelta.x;
            pan_.y += io.MouseDelta.y;
        } else {
            drag_ = DragMode::None;
        }
    } else if (drag_ == DragMode::Node) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            drag_ = DragMode::None;
        } else if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, kNodeDragThreshold)) {
            model_.MoveSubtree(dragNode_, io.MouseDelta.x / zoom_, io.MouseDelta.y / zoom_);
        }
    } else if (drag_ == DragMode::Connect && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const Hit hit = hitTest(io.MousePos);
        if (hit.node >= 0 && hit.node != dragNode_) {
            if (model_.Connect(dragNode_, hit.node)) {
                selected_ = hit.node;
            }
        }
        drag_ = DragMode::None;
    }

    // Delete: 選んでいるノードを消す (Shift で子ごと)。Editor 全体のショートカットも同じキーで選択エンティティを消すので、
    // この窓が focus を持つ間はキーを握る (握らないとシーンのエンティティまで消える)
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput) {
        const ImGuiID deleteOwner = ImGui::GetID("##btdelete");
        ImGui::SetKeyOwner(ImGuiKey_Delete, deleteOwner, ImGuiInputFlags_LockUntilRelease);
        if (loaded && selected_ >= 0 && ImGui::IsKeyPressed(ImGuiKey_Delete, ImGuiInputFlags_None, deleteOwner)) {
            DeleteSelected(io.KeyShift ? BtRemoveMode::WithDescendants : BtRemoveMode::KeepChildren);
        }
    }

    // ---- 描画 ----
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(canvasMin, canvasMax, true);
    dl->AddRectFilled(canvasMin, canvasMax, ImGui::GetColorU32(ImGuiCol_ChildBg));
    {
        // 背景の格子 (拡大縮小・移動が見て分かる目印)
        const ImU32 gridColor = ImGui::GetColorU32(ImGuiCol_Border, 0.35f);
        const float step = kGridSpacing * zoom_;
        const float startX = canvasMin.x + std::fmod(pan_.x, step);
        const float startY = canvasMin.y + std::fmod(pan_.y, step);
        for (float x = startX; x < canvasMax.x; x += step) {
            dl->AddLine(ImVec2(x, canvasMin.y), ImVec2(x, canvasMax.y), gridColor);
        }
        for (float y = startY; y < canvasMax.y; y += step) {
            dl->AddLine(ImVec2(canvasMin.x, y), ImVec2(canvasMax.x, y), gridColor);
        }
    }

    const Hit hoverHit = (loaded && hovered && drag_ == DragMode::None) ? hitTest(io.MousePos) : Hit{};
    const float fontSize = ImGui::GetFontSize() * zoom_;
    const bool drawText = fontSize >= kMinTextPixels;
    const ImU32 textColor = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 dimColor = ImGui::GetColorU32(ImGuiCol_Text, 0.65f);
    const ImU32 lineColor = ImGui::GetColorU32(ImGuiCol_Text, 0.55f);
    const ImU32 accentColor = ImGui::GetColorU32(themeColor::Accent);
    const ImU32 rootColor = ImGui::GetColorU32(themeColor::Success);
    const ImU32 orphanColor = ImGui::GetColorU32(themeColor::Warning);
    const float rounding = 4.0f * zoom_;
    const float pinRadius = (std::max)(kPinRadius * zoom_, 3.0f);
    const auto addText = [&](const ImVec2& pos, ImU32 color, const char* text, const ImVec2& clipMin, const ImVec2& clipMax, float size) {
        const ImVec4 clip(clipMin.x, clipMin.y, clipMax.x, clipMax.y);
        dl->AddText(nullptr, size, pos, color, text, nullptr, 0.0f, &clip);
    };

    // 接続線 (親の下の点 -> 子の上の中央)。子の順序の番号を子の上に付ける
    for (const BtNodeDef& parent : nodes) {
        ImVec2 parentMin;
        ImVec2 parentMax;
        rectOf(parent, parentMin, parentMax);
        const ImVec2 from((parentMin.x + parentMax.x) * 0.5f, parentMax.y);
        for (size_t order = 0; order < parent.childIds.size(); ++order) {
            const BtNodeDef* child = model_.FindNode(parent.childIds[order]);
            if (child == nullptr) {
                continue;
            }
            ImVec2 childMin;
            ImVec2 childMax;
            rectOf(*child, childMin, childMax);
            const ImVec2 to((childMin.x + childMax.x) * 0.5f, childMin.y);
            const float reach = (std::max)(24.0f * zoom_, std::fabs(to.y - from.y) * 0.5f);
            dl->AddBezierCubic(from, ImVec2(from.x, from.y + reach), ImVec2(to.x, to.y - reach), to, lineColor, (std::max)(1.5f * zoom_, 1.0f));
            if (drawText) {
                char number[16];
                std::snprintf(number, sizeof(number), "%d", static_cast<int>(order) + 1);
                const float badge = fontSize * 0.75f;
                const ImVec2 center(to.x - badge * 1.4f, to.y - badge * 0.9f);
                dl->AddCircleFilled(center, badge, ImGui::GetColorU32(ImGuiCol_PopupBg));
                dl->AddCircle(center, badge, lineColor);
                const ImVec2 textSize = ImGui::CalcTextSize(number);
                dl->AddText(nullptr, fontSize * 0.85f, ImVec2(center.x - textSize.x * 0.5f * zoom_ * 0.85f, center.y - fontSize * 0.42f), textColor, number);
            }
        }
    }

    // 接続のプレビュー
    if (drag_ == DragMode::Connect) {
        if (const BtNodeDef* source = model_.FindNode(dragNode_)) {
            ImVec2 mn;
            ImVec2 mx;
            rectOf(*source, mn, mx);
            const ImVec2 from((mn.x + mx.x) * 0.5f, mx.y);
            const Hit target = hitTest(io.MousePos);
            const bool valid = target.node >= 0 && target.node != dragNode_;
            dl->AddLine(from, io.MousePos, valid ? accentColor : lineColor, 2.0f);
        }
    }

    for (const BtNodeDef& node : nodes) {
        const BtNodeTypeInfo& info = BtNodeTypeOf(node.kind);
        ImVec2 mn;
        ImVec2 mx;
        rectOf(node, mn, mx);
        const float bandH = kBtDecoratorBandHeight * zoom_;
        const float bodyTop = mn.y + bandH * static_cast<float>(node.decorators.size());
        const bool isSelected = node.id == selected_;
        const bool isRoot = node.id == model_.RootId();
        const bool isOrphan = !isRoot && parentOf.find(node.id) == parentOf.end();

        dl->AddRectFilled(mn, mx, ImGui::GetColorU32(ImGuiCol_FrameBg), rounding);
        // Decorator の帯 (上から評価する順に積む)
        for (size_t d = 0; d < node.decorators.size(); ++d) {
            const ImVec2 bandMin(mn.x, mn.y + bandH * static_cast<float>(d));
            const ImVec2 bandMax(mx.x, bandMin.y + bandH);
            dl->AddRectFilled(bandMin, bandMax, DecoratorColor(), d == 0 ? rounding : 0.0f, d == 0 ? ImDrawFlags_RoundCornersTop : ImDrawFlags_RoundCornersNone);
            dl->AddLine(ImVec2(bandMin.x, bandMax.y), ImVec2(bandMax.x, bandMax.y), ImGui::GetColorU32(ImGuiCol_Border));
            if (drawText) {
                const std::string text = DecoratorSummary(node.decorators[d]);
                addText(ImVec2(bandMin.x + 6.0f * zoom_, bandMin.y + (bandH - fontSize * 0.85f) * 0.5f), textColor, text.c_str(),
                        bandMin, bandMax, fontSize * 0.85f);
            }
        }
        // 本体
        dl->AddRectFilled(ImVec2(mn.x, bodyTop), mx, CategoryColor(info.category), rounding,
                          node.decorators.empty() ? ImDrawFlags_RoundCornersAll : ImDrawFlags_RoundCornersBottom);
        if (drawText) {
            const ImVec2 bodyMin(mn.x, bodyTop);
            addText(ImVec2(mn.x + 8.0f * zoom_, bodyTop + 6.0f * zoom_), textColor, info.name, bodyMin, mx, fontSize);
            char idText[16];
            std::snprintf(idText, sizeof(idText), "#%d", node.id);
            const std::string summary = NodeSummary(node);
            addText(ImVec2(mn.x + 8.0f * zoom_, bodyTop + 6.0f * zoom_ + fontSize * 1.25f), dimColor,
                    summary.empty() ? idText : summary.c_str(), bodyMin, mx, fontSize * 0.85f);
        }
        // 枠: 選択 = アクセント、根 = 成功色、親なしの根でないもの = 注意色
        const ImU32 border = isSelected ? accentColor : isRoot ? rootColor : isOrphan ? orphanColor : ImGui::GetColorU32(ImGuiCol_Border);
        dl->AddRect(mn, mx, border, rounding, isSelected ? 2.5f : (isRoot || isOrphan) ? 1.5f : 1.0f);
        // 点 (上 = 入力、下 = 出力)
        if (!isRoot) {
            dl->AddCircleFilled(ImVec2((mn.x + mx.x) * 0.5f, mn.y), pinRadius, ImGui::GetColorU32(ImGuiCol_PopupBg));
            dl->AddCircle(ImVec2((mn.x + mx.x) * 0.5f, mn.y), pinRadius, lineColor);
        }
        if (BehaviorTreeEditModel::MaxChildren(node.kind) > 0) {
            const ImVec2 pin((mn.x + mx.x) * 0.5f, mx.y);
            const bool pinHot = hoverHit.outputPin && hoverHit.node == node.id;
            dl->AddCircleFilled(pin, pinRadius, pinHot ? accentColor : lineColor);
        }
    }
    dl->PopClipRect();

    // ---- 右クリックのメニュー ----
    if (openNodeMenu) {
        ImGui::OpenPopup("##btnodemenu");
    }
    if (openCanvasMenu) {
        ImGui::OpenPopup("##btcanvasmenu");
    }
    if (ImGui::BeginPopup("##btnodemenu")) {
        const BtNodeDef* node = model_.FindNode(contextNode_);
        if (node != nullptr) {
            const int32_t parentId = model_.ParentOf(node->id);
            if (ImGui::MenuItem(Tr(StrId::Bt_Disconnect), nullptr, false, parentId >= 0)) {
                model_.Disconnect(node->id);
            }
            if (ImGui::MenuItem(Tr(StrId::Bt_SetRoot), nullptr, false, parentId < 0 && model_.RootId() != node->id)) {
                model_.SetRoot(node->id);
            }
            ImGui::Separator();
            if (ImGui::MenuItem(Tr(StrId::Bt_DeleteKeep))) {
                DeleteSelected(BtRemoveMode::KeepChildren);
            }
            if (ImGui::MenuItem(Tr(StrId::Bt_DeleteAll))) {
                DeleteSelected(BtRemoveMode::WithDescendants);
            }
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##btcanvasmenu")) {
        ImGui::TextDisabled("%s", Tr(StrId::Bt_AddNodeMenu));
        for (const BtNodeCategory category : kCategoryOrder) {
            if (!ImGui::BeginMenu(Tr(CategoryLabel(category)))) {
                continue;
            }
            for (int k = 0; k < static_cast<int>(BtNodeKind::Count); ++k) {
                const BtNodeKind kind = static_cast<BtNodeKind>(k);
                if (BtNodeTypeOf(kind).category == category && ImGui::MenuItem(BtNodeTypeOf(kind).name)) {
                    AddNodeAt(kind, contextGraphPos_.x - kBtNodeWidth * 0.5f, contextGraphPos_.y);
                }
            }
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }
    ImGui::EndChild();
}

} // namespace mye
