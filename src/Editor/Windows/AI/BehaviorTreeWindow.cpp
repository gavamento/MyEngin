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
#include <filesystem>
#include <unordered_map>
#include <vector>

#include "Editor/Asset/AssetOps.h"         // MakeUniqueAssetPath (新規 BB のファイル名)
#include "Editor/Scene/Selection.h"
#include "Editor/SourceControl/ScmHint.h" // 保存直後に status を取り直させる
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Core/Localization/Localization.h"
#include "Engine/Engine/AI/BehaviorTreeSystem.h"
#include "Engine/Engine/AI/BtTaskRegistry.h"
#include "Engine/Engine/AI/BehaviorTreeLibrary.h"
#include "Engine/Engine/AI/BlackboardLibrary.h"
#include "Engine/Engine/Loop/EngineLoop.h"   // EngineContext (ライブ表示の読み先)
#include "Engine/Engine/Navigation/NavFilterLibrary.h"
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/ImGui/ImGuiTheme.h" // themeColor
#include "imgui_internal.h"                    // SetKeyOwner (Delete / Ctrl+Z / Ctrl+Y を握る)

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
constexpr float kIssuePanelHeight = 130.0f;  // キャンバスの下の検査一覧の高さ (px)
constexpr int kIssueRefreshFrames = 30;      // 木が変わらなくても検査をやり直す間隔 (取り込む木の登録が外で変わる場合の追従)
constexpr float kMinTextPixels = 7.0f;       // これより小さい文字は描かない
constexpr const char* kPaletteDragType = "MYE_BT_NODE_KIND";
constexpr uint64_t kAbortFadeTicks = 30;     // Abort の矢印が消えるまでの tick 数
constexpr ImU32 kLiveColor = IM_COL32(70, 230, 100, 255);  // 実行中のノードの太枠 (緑)
constexpr ImU32 kLiveFill = IM_COL32(70, 230, 100, 40);
constexpr int kAbortRgb[3] = { 255, 150, 40 };              // Abort の矢印 (橙。透明度だけ薄れる)

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
    if (model_.IsLoaded() && (model_.Dirty() || model_.BoardDirty())) {
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
    gestureOwner_ = GestureOwner::None;
    textSlot_ = -1;
    issuesValid_ = false;
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

void BehaviorTreeWindow::DoSaveBoard()
{
    const BlackboardAsset* board = model_.Board();
    if (board == nullptr) {
        return;
    }
    const std::wstring path = board->path;
    const std::string name = board->name + ".bb.json";
    switch (model_.SaveBoard()) {
    case BtSaveResult::Ok:
        status_ = StatusKind::BoardSaved;
        statusText_ = name;
        scmhint::Changed(path);
        break;
    case BtSaveResult::WriteFailed:
        status_ = StatusKind::BoardSaveFailed;
        break;
    case BtSaveResult::NotLoaded:
    case BtSaveResult::Blocked:
        break;
    }
}

// ドラッグ中などでまとめている操作があるときは受け付けない (途中の状態へ戻さない)
void BehaviorTreeWindow::DoUndo()
{
    if (drag_ == DragMode::None && model_.Undo()) {
        status_ = StatusKind::None;
    }
}

void BehaviorTreeWindow::DoRedo()
{
    if (drag_ == DragMode::None && model_.Redo()) {
        status_ = StatusKind::None;
    }
}

// パラメータ欄のドラッグ・文字入力は、触っている間 1 つの操作 (Undo の 1 段) にまとめる。
// 触っている欄が無くなった (ImGui のアクティブな項目が無い) フレームで閉じる
void BehaviorTreeWindow::CommitEdit(bool changed, const std::function<void()>& apply)
{
    if (!changed) {
        return;
    }
    if (gestureOwner_ == GestureOwner::None && model_.BeginGesture()) {
        gestureOwner_ = GestureOwner::Widget;
    }
    apply();
}

void BehaviorTreeWindow::FinishWidgetGesture()
{
    if (gestureOwner_ == GestureOwner::Widget && !ImGui::IsAnyItemActive()) {
        model_.EndGesture();
        gestureOwner_ = GestureOwner::None;
    }
}

// ---------------------------------------------------------------------------
// 窓
// ---------------------------------------------------------------------------

void BehaviorTreeWindow::OnImGui(EngineContext& ctx, const Selection& selection)
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
    tasks_ = ctx.behaviorTree != nullptr ? &ctx.behaviorTree->Tasks() : nullptr;
    BehaviorTreeLibrary* trees = behaviortree::Library();
    model_.BindLibraries(trees, blackboard::Library());
    if (trees == nullptr) {
        ImGui::TextDisabled("%s", Tr(StrId::Bt_NoLibrary));
        ImGui::End();
        return;
    }
    FinishWidgetGesture();
    ReloadFromRegistry();
    model_.SyncBoardWithRegistry();
    if (selected_ >= 0 && model_.FindNode(selected_) == nullptr) {
        selected_ = -1;
    }
    if (dragNode_ >= 0 && model_.FindNode(dragNode_) == nullptr) {
        drag_ = DragMode::None;
        dragNode_ = -1;
    }
    // ノードのドラッグが何かの理由で途切れたら、まとめていた操作を閉じる
    if (gestureOwner_ == GestureOwner::Canvas && drag_ != DragMode::Node) {
        model_.EndGesture();
        gestureOwner_ = GestureOwner::None;
    }
    RefreshIssues();
    RefreshLive(ctx, selection);

    DrawToolbar();
    ImGui::Separator();
    DrawLeftPanel();
    ImGui::SameLine();
    ImGui::BeginGroup();
    DrawLiveBar();
    DrawCanvas();
    DrawIssuePanel();
    ImGui::EndGroup();
    DrawUnsavedModal();
    FinishWidgetGesture();
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
    ImGui::BeginDisabled(!model_.CanUndo());
    if (ImGui::Button(Tr(StrId::Bt_Undo))) {
        DoUndo();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", Tr(StrId::Bt_UndoTip));
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!model_.CanRedo());
    if (ImGui::Button(Tr(StrId::Bt_Redo))) {
        DoRedo();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", Tr(StrId::Bt_RedoTip));
    }
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
        if (model_.BoardDirty()) {
            ImGui::SameLine();
            ImGui::TextColored(themeColor::Warning, "%s", Tr(StrId::Bt_BoardUnsaved));
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
    case StatusKind::BoardSaved:
        std::snprintf(text, sizeof(text), Tr(StrId::Bt_BoardSaved), statusText_.c_str());
        ImGui::TextColored(themeColor::Success, "%s", text);
        break;
    case StatusKind::BoardSaveFailed:
        ImGui::TextColored(themeColor::Error, "%s", Tr(StrId::Bt_BoardSaveFailed));
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
        if (model_.Dirty()) {
            DoSave();
        }
        if (model_.BoardDirty()) {
            DoSaveBoard();
        }
        if (!model_.Dirty() && !model_.BoardDirty()) {
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
        DrawBoardPanel();
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

// CppTask のタスク名。登録表にあるタスクから選ぶ (登録に無い名前も今の値として見える)
void BehaviorTreeWindow::DrawTaskPicker(int32_t id, const BtNodeDef& node)
{
    const std::string current = node.params[btcpptaskparam::kTask].s;
    char preview[96] = {};
    if (current.empty()) {
        std::snprintf(preview, sizeof(preview), "%s", Tr(StrId::Bt_BlackboardNone));
    } else if (tasks_->Find(current) == nullptr) {
        std::snprintf(preview, sizeof(preview), "%s (%s)", current.c_str(), Tr(StrId::Bt_TaskMissing));
    } else {
        std::snprintf(preview, sizeof(preview), "%s", current.c_str());
    }
    if (ImGui::BeginCombo("task", preview)) {
        for (const BtTaskType* task : tasks_->Enumerate()) {
            if (ImGui::Selectable(task->name.c_str(), task->name == current)) {
                BtParamValue value;
                value.s = task->name;
                model_.SetParam(id, btcpptaskparam::kTask, value);
            }
        }
        ImGui::EndCombo();
    }
}

// CppTask のフィールド欄。登録表の記述子から作り、値は .bt.json の "fields" に名前で入る。
// 状態を既定値 + 今の fields から組み立てて ImGui で直接編集し、変えた欄だけ書き戻す
void BehaviorTreeWindow::DrawTaskFields(int32_t id)
{
    const BtNodeDef* node = model_.FindNode(id);
    if (node == nullptr || tasks_ == nullptr) {
        return;
    }
    const std::string name = node->params[btcpptaskparam::kTask].s;
    const BtTaskType* task = name.empty() ? nullptr : tasks_->Find(name);
    ImGui::Separator();
    ImGui::TextUnformatted(Tr(StrId::Bt_TaskFields));
    if (task == nullptr) {
        ImGui::TextDisabled("%s (%s)", name.empty() ? Tr(StrId::Bt_KeyNone) : name.c_str(), Tr(StrId::Bt_TaskMissing));
        return;
    }
    alignas(16) uint8_t state[kBtCppTaskMaxStateBytes] = {};
    BtTaskInitState(*task, node->taskFields, state);
    for (size_t f = 0; f < task->fields.size(); ++f) {
        const BtTaskField& field = task->fields[f];
        const char* label = field.displayName.empty() ? field.name.c_str() : field.displayName.c_str();
        uint8_t* at = state + field.offset;
        const bool ranged = field.rangeMin != field.rangeMax;
        bool changed = false;
        ImGui::PushID(static_cast<int>(f));
        switch (field.type) {
        case MYE_FIELD_FLOAT:
            changed = ranged ? ImGui::SliderFloat(label, reinterpret_cast<float*>(at), field.rangeMin, field.rangeMax)
                             : ImGui::DragFloat(label, reinterpret_cast<float*>(at), 0.05f);
            break;
        case MYE_FIELD_INT32:
            changed = ranged ? ImGui::SliderInt(label, reinterpret_cast<int*>(at), static_cast<int>(field.rangeMin), static_cast<int>(field.rangeMax))
                             : ImGui::DragInt(label, reinterpret_cast<int*>(at));
            break;
        case MYE_FIELD_UINT32: {
            changed = ImGui::InputScalar(label, ImGuiDataType_U32, at);
            break;
        }
        case MYE_FIELD_BOOL: changed = ImGui::Checkbox(label, reinterpret_cast<bool*>(at)); break;
        case MYE_FIELD_FLOAT2: changed = ImGui::DragFloat2(label, reinterpret_cast<float*>(at), 0.05f); break;
        case MYE_FIELD_FLOAT3: changed = ImGui::DragFloat3(label, reinterpret_cast<float*>(at), 0.05f); break;
        case MYE_FIELD_FLOAT4:
        case MYE_FIELD_QUAT: changed = ImGui::DragFloat4(label, reinterpret_cast<float*>(at), 0.05f); break;
        case MYE_FIELD_COLOR: changed = ImGui::ColorEdit4(label, reinterpret_cast<float*>(at)); break;
        case MYE_FIELD_STRING64: changed = ImGui::InputText(label, reinterpret_cast<char*>(at), 64); break;
        case MYE_FIELD_STRING256: changed = ImGui::InputText(label, reinterpret_cast<char*>(at), 256); break;
        case MYE_FIELD_UINT64:
        case MYE_FIELD_ASSETREF: {
            char buf[24] = {};
            uint64_t value = 0;
            std::memcpy(&value, at, sizeof(value));
            if (value != 0) {
                std::snprintf(buf, sizeof(buf), "%llx", static_cast<unsigned long long>(value));
            }
            if (ImGui::InputText(label, buf, sizeof(buf), ImGuiInputTextFlags_CharsHexadecimal)) {
                value = std::strtoull(buf, nullptr, 16);
                std::memcpy(at, &value, sizeof(value));
                changed = true;
            }
            break;
        }
        default: ImGui::TextDisabled("%s (%s)", label, Tr(StrId::Bt_TaskFieldFixed)); break; // EntityRef / Float4x4: ファイルへ書けない
        }
        if (changed) {
            const nlohmann::json value = BtTaskReadField(field, state);
            CommitEdit(true, [&] { model_.SetTaskField(id, field.name, value); });
        }
        ImGui::PopID();
    }
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
                const int firstParam = i;
                CommitEdit(ImGui::DragFloat3("vector", v, 0.05f, desc.minValue, desc.maxValue), [&] {
                    for (int axis = 0; axis < 3; ++axis) {
                        BtParamValue value;
                        value.f = v[axis];
                        model_.SetParam(id, firstParam + axis, value);
                    }
                });
                i += 2;
                ImGui::PopID();
                continue;
            }
            if (node->kind == BtNodeKind::CppTask && i == btcpptaskparam::kTask && tasks_ != nullptr && !tasks_->Enumerate().empty()) {
                DrawTaskPicker(id, *node);
                ImGui::PopID();
                continue;
            }
            BtParamValue value = node->params[static_cast<size_t>(i)];
            CommitEdit(DrawParam(desc, value), [&] { model_.SetParam(id, i, value); });
            ImGui::PopID();
        }
    }
    if (node->kind == BtNodeKind::CppTask) {
        DrawTaskFields(id);
        node = model_.FindNode(id); // 編集で木のコピーが組み替わることがある
        if (node == nullptr) {
            ImGui::PopID();
            return;
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
                CommitEdit(DrawParam(decoInfo.params[p], value), [&] { model_.SetDecoratorParam(id, d, p, value); });
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

// 編集が確定したときだけ committed へ入れて true を返す文字欄。入力の途中の文字列 (空・重複した名前など) を
// 操作として積まないよう、確定まで textBuf_ に持つ
bool BehaviorTreeWindow::DrawCommitText(const char* label, const std::string& current, int slot, std::string& committed)
{
    if (textSlot_ != slot) {
        std::snprintf(textBuf_, sizeof(textBuf_), "%s", current.c_str());
    }
    ImGui::InputText(label, textBuf_, sizeof(textBuf_));
    if (ImGui::IsItemActivated()) {
        textSlot_ = slot;
    }
    bool done = false;
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        committed = textBuf_;
        done = true;
    }
    if (ImGui::IsItemDeactivated()) {
        textSlot_ = -1;
    }
    return done;
}

void BehaviorTreeWindow::CreateBoard()
{
    if (!model_.IsLoaded()) {
        return;
    }
    const std::wstring dir = std::filesystem::path(model_.Asset().path).parent_path().wstring();
    const std::wstring path = MakeUniqueAssetPath(dir, Utf8ToWide(model_.Asset().name) + L".bb.json");
    const uint64_t guid = model_.CreateBoardFile(path);
    if (guid == 0) {
        status_ = StatusKind::BoardSaveFailed;
        return;
    }
    scmhint::Changed(path);
    model_.SetBlackboard(guid); // 使う BB の切り替えは Undo の 1 段 (作ったファイルは Undo では消えない)
}

void BehaviorTreeWindow::DrawBoardPanel()
{
    ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
    if (!ImGui::CollapsingHeader(Tr(StrId::Bt_BoardPanel))) {
        return;
    }
    ImGui::PushID("btboard");
    if (ImGui::Button(Tr(StrId::Bt_NewBoard))) {
        CreateBoard();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", Tr(StrId::Bt_NewBoardTip));
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!model_.BoardDirty());
    if (ImGui::Button(Tr(StrId::Bt_SaveBoard))) {
        DoSaveBoard();
    }
    ImGui::EndDisabled();
    const BlackboardAsset* board = model_.Board();
    if (board == nullptr) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", Tr(StrId::Bt_BoardNoBoard));
        ImGui::PopTextWrapPos();
        ImGui::PopID();
        return;
    }
    ImGui::TextDisabled("%s.bb.json", board->name.c_str());
    int removeIndex = -1;
    for (int i = 0; i < static_cast<int>(board->keys.size()); ++i) {
        if (DrawBoardKey(i)) {
            removeIndex = i;
        }
    }
    if (removeIndex >= 0) {
        model_.RemoveBoardKey(removeIndex);
    }
    ImGui::BeginDisabled(board->keys.size() >= static_cast<size_t>(kBbMaxKeys));
    if (ImGui::Button(Tr(StrId::Bt_AddKey))) {
        model_.AddBoardKey(BbType::Bool);
    }
    ImGui::EndDisabled();
    ImGui::PopID();
}

// キー 1 つ (名前・型・初期値・eventName)。消すよう求められたら true
bool BehaviorTreeWindow::DrawBoardKey(int index)
{
    const BlackboardAsset* board = model_.Board();
    const BbKeyDef key = board->keys[static_cast<size_t>(index)]; // 操作で作業用コピーが変わるので値で持つ
    bool remove = false;
    ImGui::PushID(index);
    char header[256];
    const bool hasLive = static_cast<size_t>(index) < live_.boardValues.size() && !live_.boardValues[static_cast<size_t>(index)].empty();
    std::snprintf(header, sizeof(header), "%s  (%s)%s%s", key.name.c_str(), BbTypeName(key.type), hasLive ? "  =  " : "",
                  hasLive ? live_.boardValues[static_cast<size_t>(index)].c_str() : "");
    const ImGuiTreeNodeFlags flags = board->keys.size() <= 4 ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None;
    if (ImGui::TreeNodeEx("##bbkey", flags, "%s", header)) {
        std::string committed;
        if (DrawCommitText(Tr(StrId::Bt_KeyName), key.name, index * 2, committed)) {
            model_.RenameBoardKey(index, committed);
        }
        if (ImGui::BeginCombo(Tr(StrId::Bt_KeyType), BbTypeName(key.type))) {
            for (const BbType type : { BbType::Bool, BbType::Int, BbType::Float, BbType::Vector, BbType::Entity }) {
                if (ImGui::Selectable(BbTypeName(type), type == key.type)) {
                    model_.SetBoardKeyType(index, type);
                }
            }
            ImGui::EndCombo();
        }
        if (key.type == BbType::Entity) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", Tr(StrId::Bt_KeyEntityNoInitial));
            ImGui::PopTextWrapPos();
        } else {
            BbValue value = key.initial;
            bool hasInitial = value.isSet != 0;
            if (ImGui::Checkbox("##hasinitial", &hasInitial)) {
                value.isSet = hasInitial ? 1 : 0;
                CommitEdit(true, [&] { model_.SetBoardKeyInitial(index, value); });
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(!hasInitial);
            bool changed = false;
            switch (key.type) {
            case BbType::Bool: {
                bool b = value.i != 0;
                changed = ImGui::Checkbox(Tr(StrId::Bt_KeyInitial), &b);
                value.i = b ? 1 : 0;
                break;
            }
            case BbType::Int: changed = ImGui::DragInt(Tr(StrId::Bt_KeyInitial), &value.i, 1.0f); break;
            case BbType::Float: changed = ImGui::DragFloat(Tr(StrId::Bt_KeyInitial), &value.f, 0.05f); break;
            case BbType::Vector: changed = ImGui::DragFloat3(Tr(StrId::Bt_KeyInitial), value.v, 0.05f); break;
            case BbType::Entity: break;
            }
            ImGui::EndDisabled();
            if (changed) {
                value.isSet = 1;
            }
            CommitEdit(changed, [&] { model_.SetBoardKeyInitial(index, value); });
        }
        if (DrawCommitText(Tr(StrId::Bt_KeyEventName), key.eventName, index * 2 + 1, committed)) {
            model_.SetBoardKeyEventName(index, committed);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", Tr(StrId::Bt_KeyEventNameTip));
        }
        if (ImGui::SmallButton(Tr(StrId::Bt_Remove))) {
            remove = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", Tr(StrId::Bt_KeyRemoveTip));
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
    return remove;
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
    return view_.ToGraph(ImVec2(view_.min.x + view_.size.x * 0.5f, view_.min.y + view_.size.y * 0.5f));
}

void BehaviorTreeWindow::FitView()
{
    needFit_ = false;
    const std::vector<BtNodeDef>& nodes = model_.Asset().nodes;
    if (nodes.empty()) {
        view_.pan = ImVec2(kFitMargin, kFitMargin);
        view_.zoom = 1.0f;
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
    view_.zoom = (std::clamp)((std::min)((view_.size.x - kFitMargin * 2.0f) / width, (view_.size.y - kFitMargin * 2.0f) / height), kMinZoom, 1.0f);
    view_.pan.x = (view_.size.x - width * view_.zoom) * 0.5f - minX * view_.zoom;
    view_.pan.y = kFitMargin - minY * view_.zoom;
}

// 選んで、キャンバスの中央へ持ってくる (検査一覧のクリック)
void BehaviorTreeWindow::FocusNode(int32_t id)
{
    const BtNodeDef* node = model_.FindNode(id);
    if (node == nullptr) {
        return;
    }
    selected_ = id;
    view_.pan.x = view_.size.x * 0.5f - (node->pos[0] + kBtNodeWidth * 0.5f) * view_.zoom;
    view_.pan.y = view_.size.y * 0.5f - (node->pos[1] + BehaviorTreeEditModel::NodeHeight(*node) * 0.5f) * view_.zoom;
}

BehaviorTreeWindow::Hit BehaviorTreeWindow::HitTest(const ImVec2& p) const
{
    Hit hit;
    const std::vector<BtNodeDef>& nodes = model_.Asset().nodes;
    const float pinHit = (std::max)(kPinHitRadius, kPinRadius * view_.zoom + 3.0f);
    for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
        if (BehaviorTreeEditModel::MaxChildren(it->kind) == 0) {
            continue;
        }
        ImVec2 mn;
        ImVec2 mx;
        view_.RectOf(*it, mn, mx);
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
        view_.RectOf(*it, mn, mx);
        if (p.x >= mn.x && p.x <= mx.x && p.y >= mn.y && p.y <= mx.y) {
            hit.node = it->id;
            return hit;
        }
    }
    return hit;
}

BehaviorTreeWindow::CanvasStyle BehaviorTreeWindow::MakeStyle() const
{
    CanvasStyle style;
    style.fontSize = ImGui::GetFontSize() * view_.zoom;
    style.drawText = style.fontSize >= kMinTextPixels;
    style.rounding = 4.0f * view_.zoom;
    style.pinRadius = (std::max)(kPinRadius * view_.zoom, 3.0f);
    style.text = ImGui::GetColorU32(ImGuiCol_Text);
    style.dim = ImGui::GetColorU32(ImGuiCol_Text, 0.65f);
    style.line = ImGui::GetColorU32(ImGuiCol_Text, 0.55f);
    style.accent = ImGui::GetColorU32(themeColor::Accent);
    style.root = ImGui::GetColorU32(themeColor::Success);
    style.orphan = ImGui::GetColorU32(themeColor::Warning);
    style.error = ImGui::GetColorU32(themeColor::Error);
    style.warning = ImGui::GetColorU32(themeColor::Warning);
    return style;
}

// 検査をやり直す (木か BB が変わったとき、または一定フレームごと)
void BehaviorTreeWindow::RefreshIssues()
{
    const bool due = !issuesValid_ || issuesRevision_ != model_.Revision() || ImGui::GetFrameCount() % kIssueRefreshFrames == 0;
    if (!due) {
        return;
    }
    issues_ = model_.Inspect();
    issueSeverityOfNode_.clear();
    for (const BtIssue& issue : issues_) {
        BtIssueSeverity& severity = issueSeverityOfNode_.try_emplace(issue.nodeId, BtIssueSeverity::Warning).first->second;
        if (issue.severity == BtIssueSeverity::Error) {
            severity = BtIssueSeverity::Error;
        }
    }
    issuesRevision_ = model_.Revision();
    issuesValid_ = true;
}

// ---------------------------------------------------------------------------
// キャンバス: 入力 (HandleCanvasInput / HandleKeys) -> 描画 (Draw*) -> メニュー (DrawCanvasMenus)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// ライブ表示
// ---------------------------------------------------------------------------

namespace {

// BB の値を 1 行の文字列にする (未設定は「(未設定)」)
std::string FormatBbValue(World& world, const BbValue& value, BbType type)
{
    if (value.isSet == 0) {
        return Tr(StrId::Bt_LiveUnset);
    }
    char text[96];
    switch (type) {
    case BbType::Bool: return value.i != 0 ? "true" : "false";
    case BbType::Int: std::snprintf(text, sizeof(text), "%d", value.i); return text;
    case BbType::Float: std::snprintf(text, sizeof(text), "%.2f", value.f); return text;
    case BbType::Vector: std::snprintf(text, sizeof(text), "(%.2f, %.2f, %.2f)", value.v[0], value.v[1], value.v[2]); return text;
    case BbType::Entity: {
        if (!world.IsAlive(value.entity)) {
            return Tr(StrId::Bt_LiveUnset);
        }
        const char* name = world.GetName(value.entity);
        if (name != nullptr && name[0] != '\0') {
            return name;
        }
        std::snprintf(text, sizeof(text), "#%u", value.entity.index);
        return text;
    }
    }
    return std::string();
}

} // namespace

// 選んだエンティティの実行状態を BehaviorTreeSystem から取り直す。巻き戻した tick でも BT 節から復元された表がそのまま引ける
void BehaviorTreeWindow::RefreshLive(EngineContext& ctx, const Selection& selection)
{
    live_ = LiveView{};
    if (ctx.scene == nullptr) {
        return;
    }
    const GameObject selected = ctx.scene->FindByFileId(selection.primary);
    World& world = ctx.scene->GetWorld();
    const BehaviorTreeComponent* comp = selected ? world.GetComponent<BehaviorTreeComponent>(selected.Id()) : nullptr;
    if (comp == nullptr) {
        return;
    }
    const uint64_t displayed = model_.IsLoaded() ? model_.Asset().hash : 0; // 何も開いていなければ「違う木」で、開くボタンを出す
    live_.hasEntity = true;
    live_.entityTree = comp->tree.value;
    live_.entityName = world.GetName(selected.Id());
    live_.otherTree = comp->tree.value != displayed;

    const BtInstance* inst = ctx.behaviorTree != nullptr ? ctx.behaviorTree->FindInstance(selected.Id()) : nullptr;
    if (inst == nullptr || !inst->tree) {
        return;
    }
    live_.running = true;
    const BehaviorTreeAsset& tree = *inst->tree;
    for (const BtNodeDef& node : tree.nodes) {
        if (tree.OriginTreeOf(node) == displayed) {
            live_.otherTree = false; // 取り込んでいる部分木のアセットを開いている
            break;
        }
    }

    std::vector<int32_t> active;
    BehaviorTreeSystem::ActiveNodeIndices(*inst, active);
    std::vector<uint8_t> isActive(tree.nodes.size(), 0);
    for (const int32_t index : active) {
        isActive[static_cast<size_t>(index)] = 1;
    }
    for (const int32_t index : active) {
        const BtNodeDef& node = tree.nodes[static_cast<size_t>(index)];
        if (tree.OriginTreeOf(node) != displayed) {
            continue;
        }
        live_.runningIds.insert(tree.OriginIdOf(node));
        if (node.kind == BtNodeKind::SubTree && !node.children.empty() && isActive[static_cast<size_t>(node.children[0])] != 0) {
            live_.insideIds.insert(tree.OriginIdOf(node));
        }
    }

    const BtAbortRecord& abort = inst->lastAbort;
    if (abort.sourceId >= 0) {
        const uint64_t age = ctx.tickIndex >= abort.tick ? ctx.tickIndex - abort.tick : 0;
        const int sourceIndex = tree.FindNode(abort.sourceId);
        const int targetIndex = tree.FindNode(abort.targetId);
        if (age < kAbortFadeTicks && sourceIndex >= 0 && targetIndex >= 0) {
            live_.abortFrom = tree.DisplayedIdOf(sourceIndex, displayed);
            live_.abortTo = tree.DisplayedIdOf(targetIndex, displayed);
            live_.abortAlpha = 1.0f - static_cast<float>(age) / static_cast<float>(kAbortFadeTicks);
        }
    }

    // BB の現在値 (パネルが見ている BB と実行中の BB が同じで、キーの名前と型が合うものだけ)
    const BlackboardAsset* board = model_.Board();
    if (board != nullptr && inst->blackboardAsset && inst->blackboardAsset->hash == board->hash) {
        live_.boardValues.resize(board->keys.size());
        for (size_t i = 0; i < board->keys.size(); ++i) {
            if (i < inst->blackboard.size() && i < inst->blackboardAsset->keys.size()
                && inst->blackboardAsset->keys[i].name == board->keys[i].name && inst->blackboardAsset->keys[i].type == board->keys[i].type) {
                live_.boardValues[i] = FormatBbValue(world, inst->blackboard[i], board->keys[i].type);
            }
        }
    }
}

void BehaviorTreeWindow::DrawLiveBar()
{
    if (!live_.hasEntity) {
        return;
    }
    const std::string text = std::string(Tr(StrId::Bt_LiveLabel)) + ": " + live_.entityName + " - "
                             + (live_.running ? Tr(StrId::Bt_LiveRunning) : Tr(StrId::Bt_LiveStopped));
    ImGui::TextColored(live_.running ? themeColor::Success : themeColor::Warning, "%s", text.c_str());
    if (live_.otherTree) {
        ImGui::SameLine();
        ImGui::TextColored(themeColor::Warning, "%s", Tr(StrId::Bt_LiveOtherTree));
        ImGui::SameLine();
        const BehaviorTreeLibrary* trees = behaviortree::Library();
        ImGui::BeginDisabled(live_.entityTree == 0 || trees == nullptr || !trees->Contains(live_.entityTree));
        if (ImGui::SmallButton(Tr(StrId::Bt_LiveOpenTree))) {
            RequestOpen(live_.entityTree);
        }
        ImGui::EndDisabled();
    }
}

// 直前の Abort: Decorator の付いたノードから、止められたタスクへ橙の矢印。同じノードなら枠。30 tick で薄れる
void BehaviorTreeWindow::DrawLiveAbort(ImDrawList* dl, const CanvasStyle& style)
{
    if (live_.abortAlpha <= 0.0f || live_.abortFrom < 0 || live_.abortTo < 0) {
        return;
    }
    const BtNodeDef* from = model_.FindNode(live_.abortFrom);
    const BtNodeDef* to = model_.FindNode(live_.abortTo);
    if (from == nullptr || to == nullptr) {
        return;
    }
    const ImU32 color = IM_COL32(kAbortRgb[0], kAbortRgb[1], kAbortRgb[2], static_cast<int>(255.0f * live_.abortAlpha));
    const float thickness = (std::max)(2.5f * view_.zoom, 1.5f);
    ImVec2 fromMin;
    ImVec2 fromMax;
    ImVec2 toMin;
    ImVec2 toMax;
    view_.RectOf(*from, fromMin, fromMax);
    view_.RectOf(*to, toMin, toMax);
    if (from == to) {
        const float gap = 6.0f;
        dl->AddRect(ImVec2(fromMin.x - gap, fromMin.y - gap), ImVec2(fromMax.x + gap, fromMax.y + gap), color, style.rounding + gap, thickness);
        if (style.drawText) {
            dl->AddText(nullptr, style.fontSize * 0.85f, ImVec2(fromMax.x + gap + 4.0f, fromMin.y), color, Tr(StrId::Bt_LiveAbort));
        }
        return;
    }
    const ImVec2 start(fromMax.x, (fromMin.y + fromMax.y) * 0.5f);
    const ImVec2 end(toMax.x, (toMin.y + toMax.y) * 0.5f);
    const float reach = 70.0f * view_.zoom + std::fabs(end.y - start.y) * 0.2f;
    dl->AddBezierCubic(start, ImVec2(start.x + reach, start.y), ImVec2(end.x + reach, end.y), end, color, thickness);
    const float head = (std::max)(9.0f * view_.zoom, 6.0f);
    dl->AddTriangleFilled(end, ImVec2(end.x + head, end.y - head * 0.6f), ImVec2(end.x + head, end.y + head * 0.6f), color);
    if (style.drawText) {
        dl->AddText(nullptr, style.fontSize * 0.85f, ImVec2(start.x + 6.0f, start.y - style.fontSize), color, Tr(StrId::Bt_LiveAbort));
    }
}

void BehaviorTreeWindow::DrawCanvas()
{
    ImGuiIO& io = ImGui::GetIO();
    ImGui::BeginChild("##btcanvas", ImVec2(0.0f, -kIssuePanelHeight), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove);
    view_.min = ImGui::GetCursorScreenPos();
    view_.size = ImGui::GetContentRegionAvail();
    view_.size.x = (std::max)(view_.size.x, 50.0f);
    view_.size.y = (std::max)(view_.size.y, 50.0f);
    const ImVec2 canvasMax(view_.min.x + view_.size.x, view_.min.y + view_.size.y);
    if (needFit_ && model_.IsLoaded()) {
        FitView();
    }
    RefreshIssues();

    ImGui::InvisibleButton("##btcanvasbtn", view_.size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();

    // パレットからのドロップ
    if (model_.IsLoaded() && ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kPaletteDragType)) {
            const int32_t kindIndex = *static_cast<const int32_t*>(payload->Data);
            if (kindIndex >= 0 && kindIndex < static_cast<int32_t>(BtNodeKind::Count)) {
                const ImVec2 g = view_.ToGraph(io.MousePos);
                AddNodeAt(static_cast<BtNodeKind>(kindIndex), g.x - kBtNodeWidth * 0.5f, g.y);
            }
        }
        ImGui::EndDragDropTarget();
    }

    const MenuRequest menu = HandleCanvasInput(hovered);
    HandleKeys();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(view_.min, canvasMax, true);
    dl->AddRectFilled(view_.min, canvasMax, ImGui::GetColorU32(ImGuiCol_ChildBg));
    DrawGrid(dl);
    const CanvasStyle style = MakeStyle();
    const Hit hover = (model_.IsLoaded() && hovered && drag_ == DragMode::None) ? HitTest(io.MousePos) : Hit{};
    DrawEdges(dl, style);
    DrawConnectPreview(dl, style);
    DrawNodes(dl, style, hover);
    DrawLiveAbort(dl, style);
    dl->PopClipRect();

    DrawCanvasMenus(menu);
    ImGui::EndChild();
}

BehaviorTreeWindow::MenuRequest BehaviorTreeWindow::HandleCanvasInput(bool hovered)
{
    MenuRequest request;
    if (!model_.IsLoaded()) {
        return request;
    }
    ImGuiIO& io = ImGui::GetIO();

    // ズーム (カーソルの下のグラフ座標を動かさない)
    if (hovered && io.MouseWheel != 0.0f && drag_ != DragMode::Connect) {
        const ImVec2 before = view_.ToGraph(io.MousePos);
        view_.zoom = (std::clamp)(view_.zoom * std::pow(kZoomStep, io.MouseWheel), kMinZoom, kMaxZoom);
        view_.pan.x = io.MousePos.x - view_.min.x - before.x * view_.zoom;
        view_.pan.y = io.MousePos.y - view_.min.y - before.y * view_.zoom;
    }

    if (hovered) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) || (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsKeyDown(ImGuiKey_Space))) {
            drag_ = DragMode::Pan;
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const Hit hit = HitTest(io.MousePos);
            selected_ = hit.node;
            dragNode_ = hit.node;
            if (hit.node >= 0) {
                drag_ = hit.outputPin ? DragMode::Connect : DragMode::Node;
                if (drag_ == DragMode::Node && gestureOwner_ == GestureOwner::None && model_.BeginGesture()) {
                    gestureOwner_ = GestureOwner::Canvas; // 押してから離すまでの移動を Undo の 1 段にする
                }
            }
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            const Hit hit = HitTest(io.MousePos);
            contextGraphPos_ = view_.ToGraph(io.MousePos);
            if (hit.node >= 0) {
                selected_ = hit.node;
                contextNode_ = hit.node;
                request.node = true;
            } else {
                request.canvas = true;
            }
        }
    }

    if (drag_ == DragMode::Pan) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Middle) || ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            view_.pan.x += io.MouseDelta.x;
            view_.pan.y += io.MouseDelta.y;
        } else {
            drag_ = DragMode::None;
        }
    } else if (drag_ == DragMode::Node) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            drag_ = DragMode::None;
            if (gestureOwner_ == GestureOwner::Canvas) {
                model_.EndGesture();
                gestureOwner_ = GestureOwner::None;
            }
        } else if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, kNodeDragThreshold)) {
            model_.MoveSubtree(dragNode_, io.MouseDelta.x / view_.zoom, io.MouseDelta.y / view_.zoom);
        }
    } else if (drag_ == DragMode::Connect && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const Hit hit = HitTest(io.MousePos);
        if (hit.node >= 0 && hit.node != dragNode_) {
            if (model_.Connect(dragNode_, hit.node)) {
                selected_ = hit.node;
            }
        }
        drag_ = DragMode::None;
    }
    return request;
}

// Delete / Ctrl+Z / Ctrl+Y: Editor 全体のショートカットも同じキーでシーンを操作する (エンティティの削除・シーンの Undo)。
// この窓が focus を持つ間はキーを握る (握らないとシーン側まで動く)。窓の外にフォーカスがあるときは握らない
void BehaviorTreeWindow::HandleKeys()
{
    ImGuiIO& io = ImGui::GetIO();
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) || io.WantTextInput) {
        return;
    }
    const ImGuiID owner = ImGui::GetID("##btkeys");
    ImGui::SetKeyOwner(ImGuiKey_Delete, owner, ImGuiInputFlags_LockUntilRelease);
    ImGui::SetKeyOwner(ImGuiKey_Z, owner, ImGuiInputFlags_LockUntilRelease);
    ImGui::SetKeyOwner(ImGuiKey_Y, owner, ImGuiInputFlags_LockUntilRelease);
    if (!model_.IsLoaded()) {
        return;
    }
    if (selected_ >= 0 && ImGui::IsKeyPressed(ImGuiKey_Delete, ImGuiInputFlags_None, owner)) {
        DeleteSelected(io.KeyShift ? BtRemoveMode::WithDescendants : BtRemoveMode::KeepChildren);
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, ImGuiInputFlags_Repeat, owner)) {
        if (io.KeyShift) {
            DoRedo();
        } else {
            DoUndo();
        }
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, ImGuiInputFlags_Repeat, owner)) {
        DoRedo();
    }
}

void BehaviorTreeWindow::DrawGrid(ImDrawList* dl)
{
    // 背景の格子 (拡大縮小・移動が見て分かる目印)
    const ImVec2 canvasMax(view_.min.x + view_.size.x, view_.min.y + view_.size.y);
    const ImU32 gridColor = ImGui::GetColorU32(ImGuiCol_Border, 0.35f);
    const float step = kGridSpacing * view_.zoom;
    const float startX = view_.min.x + std::fmod(view_.pan.x, step);
    const float startY = view_.min.y + std::fmod(view_.pan.y, step);
    for (float x = startX; x < canvasMax.x; x += step) {
        dl->AddLine(ImVec2(x, view_.min.y), ImVec2(x, canvasMax.y), gridColor);
    }
    for (float y = startY; y < canvasMax.y; y += step) {
        dl->AddLine(ImVec2(view_.min.x, y), ImVec2(canvasMax.x, y), gridColor);
    }
}

// 接続線 (親の下の点 -> 子の上の中央)。子の順序の番号を子の上に付ける
void BehaviorTreeWindow::DrawEdges(ImDrawList* dl, const CanvasStyle& style)
{
    for (const BtNodeDef& parent : model_.Asset().nodes) {
        ImVec2 parentMin;
        ImVec2 parentMax;
        view_.RectOf(parent, parentMin, parentMax);
        const ImVec2 from((parentMin.x + parentMax.x) * 0.5f, parentMax.y);
        for (size_t order = 0; order < parent.childIds.size(); ++order) {
            const BtNodeDef* child = model_.FindNode(parent.childIds[order]);
            if (child == nullptr) {
                continue;
            }
            ImVec2 childMin;
            ImVec2 childMax;
            view_.RectOf(*child, childMin, childMax);
            const ImVec2 to((childMin.x + childMax.x) * 0.5f, childMin.y);
            const float reach = (std::max)(24.0f * view_.zoom, std::fabs(to.y - from.y) * 0.5f);
            dl->AddBezierCubic(from, ImVec2(from.x, from.y + reach), ImVec2(to.x, to.y - reach), to, style.line, (std::max)(1.5f * view_.zoom, 1.0f));
            if (style.drawText) {
                char number[16];
                std::snprintf(number, sizeof(number), "%d", static_cast<int>(order) + 1);
                const float badge = style.fontSize * 0.75f;
                const ImVec2 center(to.x - badge * 1.4f, to.y - badge * 0.9f);
                dl->AddCircleFilled(center, badge, ImGui::GetColorU32(ImGuiCol_PopupBg));
                dl->AddCircle(center, badge, style.line);
                const ImVec2 textSize = ImGui::CalcTextSize(number);
                dl->AddText(nullptr, style.fontSize * 0.85f, ImVec2(center.x - textSize.x * 0.5f * view_.zoom * 0.85f, center.y - style.fontSize * 0.42f), style.text, number);
            }
        }
    }
}

void BehaviorTreeWindow::DrawConnectPreview(ImDrawList* dl, const CanvasStyle& style)
{
    if (drag_ != DragMode::Connect) {
        return;
    }
    const BtNodeDef* source = model_.FindNode(dragNode_);
    if (source == nullptr) {
        return;
    }
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    ImVec2 mn;
    ImVec2 mx;
    view_.RectOf(*source, mn, mx);
    const ImVec2 from((mn.x + mx.x) * 0.5f, mx.y);
    const Hit target = HitTest(mouse);
    const bool valid = target.node >= 0 && target.node != dragNode_;
    dl->AddLine(from, mouse, valid ? style.accent : style.line, 2.0f);
}

void BehaviorTreeWindow::DrawNodes(ImDrawList* dl, const CanvasStyle& style, const Hit& hover)
{
    const std::vector<BtNodeDef>& nodes = model_.Asset().nodes;
    // 親の有無 (根でも子でもないノードを注意色で描く)。childIds が正本
    std::unordered_map<int32_t, int32_t> parentOf;
    for (const BtNodeDef& node : nodes) {
        for (const int32_t child : node.childIds) {
            parentOf[child] = node.id;
        }
    }
    const auto addText = [&](const ImVec2& pos, ImU32 color, const char* text, const ImVec2& clipMin, const ImVec2& clipMax, float size) {
        const ImVec4 clip(clipMin.x, clipMin.y, clipMax.x, clipMax.y);
        dl->AddText(nullptr, size, pos, color, text, nullptr, 0.0f, &clip);
    };

    for (const BtNodeDef& node : nodes) {
        const BtNodeTypeInfo& info = BtNodeTypeOf(node.kind);
        ImVec2 mn;
        ImVec2 mx;
        view_.RectOf(node, mn, mx);
        const float bandH = kBtDecoratorBandHeight * view_.zoom;
        const float bodyTop = mn.y + bandH * static_cast<float>(node.decorators.size());
        const bool isSelected = node.id == selected_;
        const bool isRoot = node.id == model_.RootId();
        const bool isOrphan = !isRoot && parentOf.find(node.id) == parentOf.end();

        dl->AddRectFilled(mn, mx, ImGui::GetColorU32(ImGuiCol_FrameBg), style.rounding);
        // Decorator の帯 (上から評価する順に積む)
        for (size_t d = 0; d < node.decorators.size(); ++d) {
            const ImVec2 bandMin(mn.x, mn.y + bandH * static_cast<float>(d));
            const ImVec2 bandMax(mx.x, bandMin.y + bandH);
            dl->AddRectFilled(bandMin, bandMax, DecoratorColor(), d == 0 ? style.rounding : 0.0f, d == 0 ? ImDrawFlags_RoundCornersTop : ImDrawFlags_RoundCornersNone);
            dl->AddLine(ImVec2(bandMin.x, bandMax.y), ImVec2(bandMax.x, bandMax.y), ImGui::GetColorU32(ImGuiCol_Border));
            if (style.drawText) {
                const std::string text = DecoratorSummary(node.decorators[d]);
                addText(ImVec2(bandMin.x + 6.0f * view_.zoom, bandMin.y + (bandH - style.fontSize * 0.85f) * 0.5f), style.text, text.c_str(),
                        bandMin, bandMax, style.fontSize * 0.85f);
            }
        }
        // 本体
        dl->AddRectFilled(ImVec2(mn.x, bodyTop), mx, CategoryColor(info.category), style.rounding,
                          node.decorators.empty() ? ImDrawFlags_RoundCornersAll : ImDrawFlags_RoundCornersBottom);
        if (style.drawText) {
            const ImVec2 bodyMin(mn.x, bodyTop);
            addText(ImVec2(mn.x + 8.0f * view_.zoom, bodyTop + 6.0f * view_.zoom), style.text, info.name, bodyMin, mx, style.fontSize);
            char idText[16];
            std::snprintf(idText, sizeof(idText), "#%d", node.id);
            const std::string summary = NodeSummary(node);
            addText(ImVec2(mn.x + 8.0f * view_.zoom, bodyTop + 6.0f * view_.zoom + style.fontSize * 1.25f), style.dim,
                    summary.empty() ? idText : summary.c_str(), bodyMin, mx, style.fontSize * 0.85f);
        }
        // 枠: 選択 = アクセント、根 = 成功色、親なしの根でないもの = 注意色
        const ImU32 border = isSelected ? style.accent : isRoot ? style.root : isOrphan ? style.orphan : ImGui::GetColorU32(ImGuiCol_Border);
        dl->AddRect(mn, mx, border, style.rounding, isSelected ? 2.5f : (isRoot || isOrphan) ? 1.5f : 1.0f);
        // 検査に引っかかったノードは外側に赤 (エラー) / 橙 (警告) の枠を重ねる
        const auto issue = issueSeverityOfNode_.find(node.id);
        if (issue != issueSeverityOfNode_.end()) {
            const float gap = 3.0f;
            dl->AddRect(ImVec2(mn.x - gap, mn.y - gap), ImVec2(mx.x + gap, mx.y + gap),
                        issue->second == BtIssueSeverity::Error ? style.error : style.warning, style.rounding + gap, 2.0f);
        }
        // ライブ: 実行中のノードは緑の太枠と薄い塗り。取り込んだ部分木の中で実行中の SubTree ノードは箱の下に語を添える (SubTree は子を持たず、下は空いている)
        if (live_.runningIds.count(node.id) != 0) {
            const float gap = 2.0f;
            dl->AddRectFilled(mn, mx, kLiveFill, style.rounding);
            dl->AddRect(ImVec2(mn.x - gap, mn.y - gap), ImVec2(mx.x + gap, mx.y + gap), kLiveColor, style.rounding + gap, (std::max)(3.5f * view_.zoom, 2.0f));
        }
        if (style.drawText && live_.insideIds.count(node.id) != 0) {
            const float size = style.fontSize * 0.8f;
            const ImVec2 textPos(mn.x + 2.0f, mx.y + 4.0f * view_.zoom);
            const ImVec2 textSize = ImGui::CalcTextSize(Tr(StrId::Bt_LiveInside));
            const float scale = size / ImGui::GetFontSize();
            dl->AddRectFilled(ImVec2(textPos.x - 2.0f, textPos.y), ImVec2(textPos.x + textSize.x * scale + 4.0f, textPos.y + size + 2.0f),
                              ImGui::GetColorU32(ImGuiCol_PopupBg), 3.0f);
            dl->AddText(nullptr, size, ImVec2(textPos.x, textPos.y + 1.0f), kLiveColor, Tr(StrId::Bt_LiveInside));
        }
        // 点 (上 = 入力、下 = 出力)
        if (!isRoot) {
            dl->AddCircleFilled(ImVec2((mn.x + mx.x) * 0.5f, mn.y), style.pinRadius, ImGui::GetColorU32(ImGuiCol_PopupBg));
            dl->AddCircle(ImVec2((mn.x + mx.x) * 0.5f, mn.y), style.pinRadius, style.line);
        }
        if (BehaviorTreeEditModel::MaxChildren(node.kind) > 0) {
            const ImVec2 pin((mn.x + mx.x) * 0.5f, mx.y);
            const bool pinHot = hover.outputPin && hover.node == node.id;
            dl->AddCircleFilled(pin, style.pinRadius, pinHot ? style.accent : style.line);
        }
    }
}

void BehaviorTreeWindow::DrawCanvasMenus(const MenuRequest& request)
{
    if (request.node) {
        ImGui::OpenPopup("##btnodemenu");
    }
    if (request.canvas) {
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
}

// ---------------------------------------------------------------------------
// 検査一覧 (キャンバスの下)
// ---------------------------------------------------------------------------

void BehaviorTreeWindow::DrawIssuePanel()
{
    ImGui::BeginChild("##btissues", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
    if (!model_.IsLoaded()) {
        ImGui::EndChild();
        return;
    }
    int errors = 0;
    int warnings = 0;
    for (const BtIssue& issue : issues_) {
        (issue.severity == BtIssueSeverity::Error ? errors : warnings) += 1;
    }
    char title[96];
    std::snprintf(title, sizeof(title), Tr(StrId::Bt_IssuesTitle), errors, warnings);
    ImGui::TextColored(errors > 0 ? themeColor::Error : warnings > 0 ? themeColor::Warning : themeColor::Success, "%s", title);
    if (issues_.empty()) {
        ImGui::TextDisabled("%s", Tr(StrId::Bt_IssuesNone));
    }
    for (size_t i = 0; i < issues_.size(); ++i) {
        const BtIssue& issue = issues_[i];
        const BtNodeDef* node = model_.FindNode(issue.nodeId);
        if (node == nullptr) {
            continue;
        }
        const char* kindName = BtNodeTypeOf(node->kind).name;
        char slot[64] = {};
        if (issue.decoratorIndex >= 0) {
            std::snprintf(slot, sizeof(slot), Tr(StrId::Bt_IssueDecoratorSlot), issue.decoratorIndex + 1);
        } else if (issue.keyIndex >= 0) {
            std::snprintf(slot, sizeof(slot), "%s", BtNodeTypeOf(node->kind).keyNames[issue.keyIndex]);
        }
        char text[256] = {};
        switch (issue.kind) {
        case BtIssueKind::ChildCount: std::snprintf(text, sizeof(text), Tr(StrId::Bt_IssueChildCount), kindName, node->id); break;
        case BtIssueKind::ParallelMainNotTask: std::snprintf(text, sizeof(text), Tr(StrId::Bt_IssueParallelMain), kindName, node->id, issue.otherId); break;
        case BtIssueKind::LowerPriorityParent: std::snprintf(text, sizeof(text), Tr(StrId::Bt_IssueLowerPriority), kindName, node->id); break;
        case BtIssueKind::SubTreeBoardMismatch: std::snprintf(text, sizeof(text), Tr(StrId::Bt_IssueSubTreeBoard), kindName, node->id); break;
        case BtIssueKind::SubTreeUnresolved: std::snprintf(text, sizeof(text), Tr(StrId::Bt_IssueSubTreeUnresolved), kindName, node->id); break;
        case BtIssueKind::KeyUnset: std::snprintf(text, sizeof(text), Tr(StrId::Bt_IssueKeyUnset), kindName, node->id, slot); break;
        case BtIssueKind::KeyMissing: std::snprintf(text, sizeof(text), Tr(StrId::Bt_IssueKeyMissing), kindName, node->id, slot); break;
        case BtIssueKind::KeyTypeMismatch: std::snprintf(text, sizeof(text), Tr(StrId::Bt_IssueKeyType), kindName, node->id, slot); break;
        case BtIssueKind::CSharpTask: std::snprintf(text, sizeof(text), Tr(StrId::Bt_IssueCSharp), kindName, node->id); break;
        }
        ImGui::PushID(static_cast<int>(i));
        ImGui::PushStyleColor(ImGuiCol_Text, issue.severity == BtIssueSeverity::Error ? themeColor::Error : themeColor::Warning);
        const bool clicked = ImGui::Selectable(text, issue.nodeId == selected_);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", Tr(StrId::Bt_IssueJumpTip));
        }
        ImGui::PopID();
        if (clicked) {
            FocusNode(issue.nodeId);
        }
    }
    ImGui::EndChild();
}

} // namespace mye
