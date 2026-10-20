//====================================================================================
//                          TilePaletteWindow.cpp
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                          タイルパレット窓の実装
//====================================================================================
#include "Editor/Windows/Tilemap/TilePaletteWindow.h"

#include <algorithm>
#include <cfloat>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "imgui.h"
#include "imgui_internal.h" // SetKeyOwner (Ctrl+S / Ctrl+Z / Ctrl+Y / Delete を握る)

#include "Editor/Scene/Selection.h"
#include "Editor/SourceControl/ScmHint.h"
#include "Editor/Windows/Animation/SpriteAnimEditor.h" // DrawSpriteThumb
#include "Editor/Windows/Tilemap/TileAssetEditUi.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Core/Localization/Localization.h"
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Engine/Tilemap/TilemapAssets.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/ImGui/ImGuiTheme.h" // themeColor

namespace mye {
namespace {

constexpr float kTileThumbSize = 40.0f;
constexpr float kTileThumbSpacing = 4.0f;
constexpr ImU32 kSelectedTileColor = IM_COL32(255, 220, 64, 255);

struct ToolInfo {
    tileedit::Tool tool;
    StrId label;
    ImGuiKey key;
};
constexpr ToolInfo kTools[tileedit::kToolCount] = {
    { tileedit::Tool::Select, StrId::TilePal_ToolSelect, ImGuiKey_S },
    { tileedit::Tool::Brush, StrId::TilePal_ToolBrush, ImGuiKey_B },
    { tileedit::Tool::Erase, StrId::TilePal_ToolErase, ImGuiKey_D },
    { tileedit::Tool::Fill, StrId::TilePal_ToolFill, ImGuiKey_G },
    { tileedit::Tool::Rect, StrId::TilePal_ToolRect, ImGuiKey_U },
    { tileedit::Tool::Pick, StrId::TilePal_ToolPick, ImGuiKey_I },
};

// 現在のツールに応じた「選択中」のボタン色を積む。Pop は 3 色分
void PushToggleColors(bool on)
{
    if (on) {
        ImGui::PushStyleColor(ImGuiCol_Button, themeColor::AccentSoft);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, themeColor::Accent);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, themeColor::Accent);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Button));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
}

// 窓の幅で折り返す色付きの文
void WrappedText(const ImVec4& color, const char* text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

// 直前のボタンの右にラベルのボタンをもう 1 つ置いても窓の幅に収まるか (収まらなければ折り返す)
bool FitsOnLine(const char* label)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const float width = ImGui::CalcTextSize(label, nullptr, true).x + style.FramePadding.x * 2.0f;
    const float right = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - style.WindowPadding.x;
    return ImGui::GetItemRectMax().x + style.ItemSpacing.x + width <= right;
}

} // namespace

// ---------------------------------------------------------------------------
// 対象の選択・読み込み・保存
// ---------------------------------------------------------------------------

bool TilePaletteWindow::SceneEditActive() const
{
    return open && model_.IsLoaded() && !inPlay_ && targetFid_ != 0 && sceneEdit_;
}

void TilePaletteWindow::CollectTargets(EngineContext& ctx)
{
    targets_.clear();
    if (ctx.scene == nullptr) {
        return;
    }
    World& world = ctx.scene->GetWorld();
    std::vector<EntityID> entities;
    const ComponentTypeId req[] = { TilemapComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            entities.push_back(arch.EntityAt(row));
        }
    });
    for (const EntityID e : entities) {
        const auto* tm = world.GetComponent<TilemapComponent>(e);
        if (tm == nullptr) {
            continue;
        }
        const TilemapAsset* asset = tilemap::Resolve(tm->tilemap);
        TargetEntry t;
        t.fileId = ctx.scene->EnsureFileId(e);
        t.assetHash = asset != nullptr ? asset->hash : 0;
        t.label = std::string(world.GetName(e)) + "  (" + (asset != nullptr ? asset->name : std::string("?")) + ")";
        targets_.push_back(std::move(t));
    }
    std::sort(targets_.begin(), targets_.end(), [](const TargetEntry& a, const TargetEntry& b) {
        return a.label != b.label ? a.label < b.label : a.fileId < b.fileId;
    });
}

void TilePaletteWindow::OpenForEntity(EngineContext& ctx, uint64_t fileId)
{
    open = true;
    RequestTarget(ctx, fileId);
}

void TilePaletteWindow::RequestTarget(EngineContext& ctx, uint64_t fileId)
{
    if (fileId == targetFid_ || fileId == 0 || ctx.scene == nullptr) {
        return;
    }
    const GameObject go = ctx.scene->FindByFileId(fileId);
    const auto* tm = go ? go.GetWorld()->GetComponent<TilemapComponent>(go.Id()) : nullptr;
    const TilemapAsset* asset = tm != nullptr ? tilemap::Resolve(tm->tilemap) : nullptr;
    if (asset == nullptr) {
        return;
    }
    // 同じアセットを指す別のエンティティへは、編集中の内容を持ったまま切り替えられる
    if (model_.IsLoaded() && model_.Asset().hash == asset->hash) {
        targetFid_ = fileId;
        return;
    }
    if (model_.IsLoaded() && HasUnsavedChanges()) {
        pendingFid_ = fileId;
        modalRequest_ = true;
        return;
    }
    LoadTarget(ctx, fileId);
}

bool TilePaletteWindow::LoadTarget(EngineContext& ctx, uint64_t fileId)
{
    const GameObject go = ctx.scene != nullptr ? ctx.scene->FindByFileId(fileId) : GameObject{};
    const auto* tm = go ? go.GetWorld()->GetComponent<TilemapComponent>(go.Id()) : nullptr;
    const TilemapAsset* asset = tm != nullptr ? tilemap::Resolve(tm->tilemap) : nullptr;
    if (asset == nullptr) {
        return false;
    }
    model_.Load(*asset);
    paletteModel_.Clear();
    if (const TilePalette* palette = tilepalette::Resolve(asset->palette)) {
        paletteModel_.Load(*palette);
    }
    targetFid_ = fileId;
    layer_ = std::clamp(layer_, 0, (std::max)(static_cast<int>(model_.Asset().layers.size()) - 1, 0));
    stroking_ = false;
    status_ = StatusKind::None;
    TilemapLibrary* lib = tilemap::Library();
    syncedRevision_ = model_.Revision();
    syncedEpoch_ = lib != nullptr ? lib->Epoch() : 0;
    TilePaletteLibrary* paletteLib = tilepalette::Library();
    syncedPaletteRevision_ = paletteModel_.Revision();
    syncedPaletteEpoch_ = paletteLib != nullptr ? paletteLib->Epoch() : 0;
    seenMapRevision_ = model_.Revision();
    seenPaletteRevision_ = paletteModel_.Revision();
    return true;
}

// 未保存の編集を捨てる。TilemapLibrary / TilePaletteLibrary に反映済みの内容はファイルから読み直して戻す
void TilePaletteWindow::DiscardEdits()
{
    const std::wstring path = model_.IsLoaded() ? model_.Asset().path : std::wstring();
    const std::wstring palettePath = paletteModel_.IsLoaded() ? paletteModel_.Palette().path : std::wstring();
    model_.Clear();
    paletteModel_.Clear();
    targetFid_ = 0;
    stroking_ = false;
    if (!path.empty()) {
        if (TilemapLibrary* lib = tilemap::Library()) {
            lib->LoadFromFile(path);
        }
    }
    if (!palettePath.empty()) {
        if (TilePaletteLibrary* lib = tilepalette::Library()) {
            lib->LoadFromFile(palettePath);
        }
    }
}

// 編集中の内容を TilemapLibrary へ反映する (ファイルは書かない)。描画は Library の版で焼き直す。
// Library が外から更新されたとき (保存後の ReloadHub など): 未編集なら内容を取り込み、編集中なら編集側を勝たせる
void TilePaletteWindow::SyncLive()
{
    TilemapLibrary* lib = tilemap::Library();
    if (!model_.IsLoaded() || lib == nullptr) {
        return;
    }
    const bool modelChanged = model_.Revision() != syncedRevision_;
    const bool libChanged = lib->Epoch() != syncedEpoch_;
    if (!modelChanged && !libChanged) {
        return;
    }
    if (!modelChanged) {
        const TilemapAsset* current = lib->Get(model_.Asset().hash);
        if (current == nullptr || model_.SameLayers(*current)) {
            syncedEpoch_ = lib->Epoch(); // 別のアセットの登録、または自分の保存の読み直し
            return;
        }
        if (!model_.Dirty()) {
            model_.Load(*current); // 外部の変更 (git の切り替えなど) を取り込む。Undo は捨てる
            syncedRevision_ = model_.Revision();
            syncedEpoch_ = lib->Epoch();
            return;
        }
    }
    lib->Register(model_.Asset().path, model_.Asset());
    syncedRevision_ = model_.Revision();
    syncedEpoch_ = lib->Epoch();
}

// 編集中のパレットを TilePaletteLibrary へ反映する (ファイルは書かない)。SyncLive と同じ規則
void TilePaletteWindow::SyncPaletteLive()
{
    TilePaletteLibrary* lib = tilepalette::Library();
    if (!paletteModel_.IsLoaded() || lib == nullptr) {
        return;
    }
    const bool modelChanged = paletteModel_.Revision() != syncedPaletteRevision_;
    const bool libChanged = lib->Epoch() != syncedPaletteEpoch_;
    if (!modelChanged && !libChanged) {
        return;
    }
    if (!modelChanged) {
        const TilePalette* current = lib->Get(paletteModel_.Palette().hash);
        if (current == nullptr || paletteModel_.SameTiles(*current)) {
            syncedPaletteEpoch_ = lib->Epoch();
            return;
        }
        if (!paletteModel_.Dirty()) {
            paletteModel_.Load(*current);
            syncedPaletteRevision_ = paletteModel_.Revision();
            seenPaletteRevision_ = paletteModel_.Revision();
            syncedPaletteEpoch_ = lib->Epoch();
            return;
        }
    }
    lib->Register(paletteModel_.Palette().path, paletteModel_.Palette());
    syncedPaletteRevision_ = paletteModel_.Revision();
    syncedPaletteEpoch_ = lib->Epoch();
}

bool TilePaletteWindow::SaveTilemap()
{
    SyncLive();
    const TilemapAsset& asset = model_.Asset();
    std::ofstream out(std::filesystem::path(asset.path), std::ios::binary);
    if (out) {
        out << TilemapLibrary::ToJson(asset).dump(2);
        out.close();
    }
    if (!out) {
        MYE_LOG_ERROR(Tr(StrId::Log_WriteTilemapFail), WideToUtf8(asset.path).c_str());
        status_ = StatusKind::SaveFailed;
        return false;
    }
    scmhint::Changed(asset.path);
    model_.MarkSaved();
    status_ = StatusKind::Saved;
    return true;
}

bool TilePaletteWindow::SavePalette()
{
    SyncPaletteLive();
    const TilePalette& palette = paletteModel_.Palette();
    std::ofstream out(std::filesystem::path(palette.path), std::ios::binary);
    if (out) {
        out << TilePaletteLibrary::ToJson(palette).dump(2);
        out.close();
    }
    if (!out) {
        MYE_LOG_ERROR(Tr(StrId::Log_WriteTilePaletteFail), WideToUtf8(palette.path).c_str());
        status_ = StatusKind::PaletteSaveFailed;
        return false;
    }
    scmhint::Changed(palette.path);
    paletteModel_.MarkSaved();
    status_ = StatusKind::PaletteSaved;
    return true;
}

// タイルマップは常に書き、パレットは変更があるときだけ書く
void TilePaletteWindow::DoSave(EngineContext& ctx)
{
    (void)ctx;
    if (!model_.IsLoaded()) {
        return;
    }
    const bool mapOk = SaveTilemap();
    if (mapOk && paletteModel_.Dirty()) {
        SavePalette();
    }
}

// 直前に変わった側 (Undo / Redo も変更に数える) を覚え、Ctrl+Z がそちらから戻すようにする
void TilePaletteWindow::TrackLastEdited()
{
    if (paletteModel_.Revision() != seenPaletteRevision_) {
        seenPaletteRevision_ = paletteModel_.Revision();
        lastEditPalette_ = true;
    }
    if (model_.Revision() != seenMapRevision_) {
        seenMapRevision_ = model_.Revision();
        lastEditPalette_ = false;
    }
}

bool TilePaletteWindow::CanUndoAny() const
{
    return model_.CanUndo() || paletteModel_.CanUndo();
}

bool TilePaletteWindow::CanRedoAny() const
{
    return model_.CanRedo() || paletteModel_.CanRedo();
}

void TilePaletteWindow::UndoLast()
{
    if (lastEditPalette_ ? paletteModel_.CanUndo() : !model_.CanUndo()) {
        paletteModel_.Undo();
    } else {
        model_.Undo();
    }
}

void TilePaletteWindow::RedoLast()
{
    if (lastEditPalette_ ? paletteModel_.CanRedo() : !model_.CanRedo()) {
        paletteModel_.Redo();
    } else {
        model_.Redo();
    }
}

uint32_t TilePaletteWindow::BrushValue() const
{
    return tilecell::Make(brushTile_, flipX_, flipY_, rotation_);
}

void TilePaletteWindow::SetTool(tileedit::Tool tool)
{
    if (stroking_) {
        return; // ドラッグの途中でツールを替えると Undo の 1 段が壊れる
    }
    tool_ = tool;
}

// ---------------------------------------------------------------------------
// Scene View の入力
// ---------------------------------------------------------------------------

void TilePaletteWindow::Report(tileedit::EditResult result)
{
    if (result == tileedit::EditResult::TooLarge) {
        status_ = StatusKind::TooLarge;
    } else if (result == tileedit::EditResult::Changed) {
        status_ = StatusKind::None;
    }
}

void TilePaletteWindow::ApplyAt(int32_t x, int32_t y)
{
    if (tool_ == tileedit::Tool::Brush) {
        if (brushTile_ == 0) {
            status_ = StatusKind::NoTile;
            return;
        }
        Report(model_.PaintCell(layer_, x, y, BrushValue()));
    } else if (tool_ == tileedit::Tool::Erase) {
        Report(model_.EraseCell(layer_, x, y));
    }
}

void TilePaletteWindow::BeginStroke(const TileSceneInput& in)
{
    anchorX_ = in.cellX;
    anchorY_ = in.cellY;
    lastX_ = in.cellX;
    lastY_ = in.cellY;
    switch (tool_) {
    case tileedit::Tool::Brush:
    case tileedit::Tool::Erase:
        model_.BeginGesture();
        stroking_ = true;
        ApplyAt(in.cellX, in.cellY);
        break;
    case tileedit::Tool::Fill:
        if (brushTile_ == 0) {
            status_ = StatusKind::NoTile;
        } else {
            Report(model_.FloodFill(layer_, in.cellX, in.cellY, BrushValue()));
        }
        break;
    case tileedit::Tool::Rect:
    case tileedit::Tool::Select:
        stroking_ = true;
        break;
    case tileedit::Tool::Pick: {
        const uint32_t value = model_.PickCell(layer_, in.cellX, in.cellY);
        if (tilecell::TileId(value) != 0) {
            brushTile_ = tilecell::TileId(value);
            flipX_ = (value & tilecell::kFlipX) != 0;
            flipY_ = (value & tilecell::kFlipY) != 0;
            rotation_ = tilecell::Rotation(value);
            tool_ = tileedit::Tool::Brush; // Unity と同じく、拾ったらブラシに戻る
        }
        break;
    }
    }
}

// 前回のセルから今のセルまでを埋めて塗る (速く動かしても線が途切れない)。Bresenham
void TilePaletteWindow::ContinueStroke(const TileSceneInput& in)
{
    if (tool_ != tileedit::Tool::Brush && tool_ != tileedit::Tool::Erase) {
        return;
    }
    int32_t x = lastX_;
    int32_t y = lastY_;
    const int32_t dx = std::abs(in.cellX - x);
    const int32_t dy = -std::abs(in.cellY - y);
    const int32_t sx = x < in.cellX ? 1 : -1;
    const int32_t sy = y < in.cellY ? 1 : -1;
    int32_t err = dx + dy;
    while (x != in.cellX || y != in.cellY) {
        const int32_t e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y += sy;
        }
        ApplyAt(x, y);
    }
    lastX_ = in.cellX;
    lastY_ = in.cellY;
}

void TilePaletteWindow::EndStroke(const TileSceneInput& in)
{
    const int32_t endX = in.hasCell ? in.cellX : cursorX_;
    const int32_t endY = in.hasCell ? in.cellY : cursorY_;
    switch (tool_) {
    case tileedit::Tool::Brush:
    case tileedit::Tool::Erase:
        model_.EndGesture();
        break;
    case tileedit::Tool::Rect:
        if (brushTile_ == 0) {
            status_ = StatusKind::NoTile;
        } else {
            Report(model_.FillRect(layer_, anchorX_, anchorY_, endX, endY, BrushValue()));
        }
        break;
    case tileedit::Tool::Select:
        model_.SelectRect(anchorX_, anchorY_, endX, endY);
        break;
    default:
        break;
    }
    stroking_ = false;
}

void TilePaletteWindow::OnSceneInput(const TileSceneInput& in)
{
    if (!SceneEditActive()) {
        return;
    }
    hasCursor_ = in.hasCell && (in.hovered || stroking_);
    if (in.hasCell) {
        cursorX_ = in.cellX;
        cursorY_ = in.cellY;
    }
    if (in.hovered && !stroking_) {
        HandleToolKeys();
        if (tool_ == tileedit::Tool::Select && model_.HasSelection()) {
            deleteClaimFrame_ = ImGui::GetFrameCount();
        }
    }
    if (!stroking_) {
        if (in.pressed && in.hasCell) {
            BeginStroke(in);
        }
        return;
    }
    if (in.hasCell && in.down) {
        ContinueStroke(in);
    }
    if (in.released || !in.down) {
        EndStroke(in);
    }
}

TileSceneOverlay TilePaletteWindow::SceneOverlay() const
{
    TileSceneOverlay o;
    if (!SceneEditActive()) {
        return o;
    }
    o.hasCursor = hasCursor_;
    o.cursorX = cursorX_;
    o.cursorY = cursorY_;
    if (stroking_ && (tool_ == tileedit::Tool::Rect || tool_ == tileedit::Tool::Select)) {
        o.hasDragRect = true;
        o.dragRect = tileedit::MakeCellRect(anchorX_, anchorY_, cursorX_, cursorY_);
    }
    o.hasSelection = model_.HasSelection();
    o.selection = model_.Selection();
    return o;
}

// ---------------------------------------------------------------------------
// 窓
// ---------------------------------------------------------------------------

void TilePaletteWindow::OnImGui(EngineContext& ctx, const Selection& selection)
{
    // 窓を閉じた・Play に入ったなどで入力が途切れたら、開いたままのドラッグを閉じる
    if (stroking_ && !SceneEditActive()) {
        if (tool_ == tileedit::Tool::Brush || tool_ == tileedit::Tool::Erase) {
            model_.EndGesture();
        }
        stroking_ = false;
    }
    SyncLive();
    SyncPaletteLive();
    TrackLastEdited();
    if (!open) {
        // 窓を閉じた時点で開いたままのドラッグ (Inspector の値など) を閉じる
        if (paletteModel_.InGesture()) {
            paletteModel_.EndGesture();
        }
        if (model_.InGesture() && !stroking_) {
            model_.EndGesture();
        }
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(420.0f, 520.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(Tr(StrId::Win_TilePalette), &open)) {
        ImGui::End();
        return;
    }
    CollectTargets(ctx);
    // 対象が無いときは、選択中の Tilemap エンティティを自動で対象にする
    if (targetFid_ == 0 && selection.primary != 0) {
        RequestTarget(ctx, selection.primary);
    }
    DrawUnsavedModal(ctx);
    HandleKeys(ctx);
    DrawTargetRow(ctx);
    if (inPlay_) {
        WrappedText(themeColor::Warning, Tr(StrId::TilePal_Play));
    }
    ImGui::BeginDisabled(inPlay_ || !model_.IsLoaded());
    DrawToolRow();
    DrawBrushRow();
    ImGui::EndDisabled();
    DrawStatus();
    ImGui::Separator();
    DrawBody(ctx);
    ImGui::End();
}

void TilePaletteWindow::DrawUnsavedModal(EngineContext& ctx)
{
    if (modalRequest_) {
        ImGui::OpenPopup(Tr(StrId::TilePal_ModalTitle));
        modalRequest_ = false;
    }
    if (!ImGui::BeginPopupModal(Tr(StrId::TilePal_ModalTitle), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    ImGui::TextUnformatted(Tr(StrId::TilePal_ModalBody));
    ImGui::TextDisabled("%s", model_.IsLoaded() ? WideToUtf8(model_.Asset().path).c_str() : "");
    if (ImGui::Button(Tr(StrId::TilePal_ModalSave))) {
        DoSave(ctx);
        if (!HasUnsavedChanges()) {
            DiscardEdits();
            LoadTarget(ctx, pendingFid_);
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr(StrId::TilePal_ModalDiscard))) {
        DiscardEdits();
        LoadTarget(ctx, pendingFid_);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr(StrId::Common_Cancel))) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// S / B / D / G / U / I (Unity と同じ)。ドラッグ中・修飾キー・テキスト入力中は効かせない
void TilePaletteWindow::HandleToolKeys()
{
    const ImGuiIO& io = ImGui::GetIO();
    if (stroking_ || io.WantTextInput || io.KeyCtrl || io.KeyAlt || io.KeyShift || io.MouseDown[ImGuiMouseButton_Right]) {
        return;
    }
    for (const ToolInfo& t : kTools) {
        if (ImGui::IsKeyPressed(t.key, false)) {
            SetTool(t.tool);
        }
    }
    if (tool_ == tileedit::Tool::Select && model_.HasSelection()) {
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
            Report(model_.EraseSelection(layer_));
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            model_.ClearSelection(); // 選択を外せば Delete は再びエンティティの削除に戻る
        }
    }
}

bool TilePaletteWindow::ClaimsDeleteKey() const
{
    return SceneEditActive() && tool_ == tileedit::Tool::Select && model_.HasSelection()
           && ImGui::GetFrameCount() - deleteClaimFrame_ <= 1;
}

// Ctrl+S / Ctrl+Z / Ctrl+Y / Delete: エディタ全体のショートカットも同じキーを使うので、
// この窓にフォーカスがあるあいだだけ握る
void TilePaletteWindow::HandleKeys(EngineContext& ctx)
{
    ImGuiIO& io = ImGui::GetIO();
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) || io.WantTextInput) {
        return;
    }
    const ImGuiID owner = ImGui::GetID("##tp_keys");
    ImGui::SetKeyOwner(ImGuiKey_Delete, owner, ImGuiInputFlags_LockUntilRelease);
    ImGui::SetKeyOwner(ImGuiKey_Z, owner, ImGuiInputFlags_LockUntilRelease);
    ImGui::SetKeyOwner(ImGuiKey_Y, owner, ImGuiInputFlags_LockUntilRelease);
    ImGui::SetKeyOwner(ImGuiKey_S, owner, ImGuiInputFlags_LockUntilRelease);
    if (stroking_ || !model_.IsLoaded()) {
        return;
    }
    if (tool_ == tileedit::Tool::Select && model_.HasSelection()) {
        deleteClaimFrame_ = ImGui::GetFrameCount();
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, ImGuiInputFlags_None, owner)) {
        DoSave(ctx);
        return;
    }
    if (inPlay_) {
        return;
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, ImGuiInputFlags_Repeat, owner)) {
        if (io.KeyShift) {
            RedoLast();
        } else {
            UndoLast();
        }
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, ImGuiInputFlags_Repeat, owner)) {
        RedoLast();
    }
    if (tool_ == tileedit::Tool::Select && ImGui::IsKeyPressed(ImGuiKey_Delete, ImGuiInputFlags_None, owner)) {
        Report(model_.EraseSelection(layer_));
    }
    if (!io.KeyCtrl && !io.KeyAlt && !io.KeyShift) {
        for (const ToolInfo& t : kTools) {
            if (ImGui::IsKeyPressed(t.key, false, owner)) {
                SetTool(t.tool);
            }
        }
    }
}

void TilePaletteWindow::DrawTargetRow(EngineContext& ctx)
{
    // 対象のコンボ
    const TargetEntry* current = nullptr;
    for (const TargetEntry& t : targets_) {
        if (t.fileId == targetFid_) {
            current = &t;
        }
    }
    const char* preview = current != nullptr ? current->label.c_str() : Tr(StrId::TilePal_NoTarget);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##tp_target", preview)) {
        for (const TargetEntry& t : targets_) {
            ImGui::PushID(static_cast<int>(t.fileId & 0x7fffffff));
            if (ImGui::Selectable(t.label.c_str(), t.fileId == targetFid_)) {
                RequestTarget(ctx, t.fileId);
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if (!model_.IsLoaded()) {
        return;
    }

    // レイヤーのコンボ
    const std::vector<TilemapLayer>& layers = model_.Asset().layers;
    if (layers.empty()) {
        return;
    }
    layer_ = std::clamp(layer_, 0, static_cast<int>(layers.size()) - 1);
    std::string layerLabel = layers[static_cast<size_t>(layer_)].name;
    if (layerLabel.empty()) {
        layerLabel = std::to_string(layer_);
    }
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::BeginCombo(Tr(StrId::TilePal_Layer), layerLabel.c_str())) {
        for (int i = 0; i < static_cast<int>(layers.size()); ++i) {
            std::string label = layers[static_cast<size_t>(i)].name.empty() ? std::to_string(i)
                                                                            : layers[static_cast<size_t>(i)].name;
            ImGui::PushID(i);
            if (ImGui::Selectable(label.c_str(), i == layer_) && !stroking_) {
                layer_ = i;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }

    // Undo / Redo / Save
    ImGui::BeginDisabled(inPlay_ || !CanUndoAny());
    if (ImGui::Button(Tr(StrId::Spr_Undo))) {
        UndoLast();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(inPlay_ || !CanRedoAny());
    if (ImGui::Button(Tr(StrId::Spr_Redo))) {
        RedoLast();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(stroking_);
    if (ImGui::Button(Tr(StrId::Common_Save))) {
        DoSave(ctx);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", HasUnsavedChanges() ? "*" : "");
}

void TilePaletteWindow::DrawToolRow()
{
    for (int i = 0; i < tileedit::kToolCount; ++i) {
        const ToolInfo& t = kTools[i];
        if (i > 0 && FitsOnLine(Tr(t.label))) {
            ImGui::SameLine();
        }
        PushToggleColors(tool_ == t.tool);
        if (ImGui::Button(Tr(t.label))) {
            SetTool(t.tool);
        }
        ImGui::PopStyleColor(3);
    }
}

void TilePaletteWindow::DrawBrushRow()
{
    PushToggleColors(flipX_);
    if (ImGui::Button(Tr(StrId::TilePal_FlipX))) {
        flipX_ = !flipX_;
    }
    ImGui::PopStyleColor(3);
    ImGui::SameLine();
    PushToggleColors(flipY_);
    if (ImGui::Button(Tr(StrId::TilePal_FlipY))) {
        flipY_ = !flipY_;
    }
    ImGui::PopStyleColor(3);
    ImGui::SameLine();
    if (ImGui::Button(Tr(StrId::TilePal_Rotate))) {
        rotation_ = (rotation_ + 1) & 3u;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%u", rotation_ * 90u);
    ImGui::Checkbox(Tr(StrId::TilePal_SceneEdit), &sceneEdit_);
}

void TilePaletteWindow::DrawStatus()
{
    switch (status_) {
    case StatusKind::None:
        if (model_.IsLoaded() && !inPlay_) {
            if (!sceneMode2d_) {
                WrappedText(themeColor::Warning, Tr(StrId::TilePal_Need2D));
            } else {
                WrappedText(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), Tr(StrId::TilePal_Help));
            }
        }
        break;
    case StatusKind::Saved:
        ImGui::TextColored(themeColor::Success, "%s", Tr(StrId::TilePal_StatusSaved));
        break;
    case StatusKind::SaveFailed:
        ImGui::TextColored(themeColor::Error, "%s", Tr(StrId::TilePal_StatusSaveFailed));
        break;
    case StatusKind::PaletteSaved:
        ImGui::TextColored(themeColor::Success, "%s", Tr(StrId::TilePal_StatusPaletteSaved));
        break;
    case StatusKind::PaletteSaveFailed:
        ImGui::TextColored(themeColor::Error, "%s", Tr(StrId::TilePal_StatusPaletteSaveFailed));
        break;
    case StatusKind::TooLarge:
        WrappedText(themeColor::Warning, Tr(StrId::TilePal_StatusTooLarge));
        break;
    case StatusKind::NoTile:
        ImGui::TextColored(themeColor::Warning, "%s", Tr(StrId::TilePal_StatusNoTile));
        break;
    }
}

// 下半分 (スクロールする): レイヤー / パレットのタイル一覧と追加・削除 / 選択中のタイルの設定
void TilePaletteWindow::DrawBody(EngineContext& ctx)
{
    if (!model_.IsLoaded()) {
        return;
    }
    ImGui::BeginChild("##tp_body", ImVec2(0.0f, 0.0f), false);
    ImGui::BeginDisabled(inPlay_);
    if (ImGui::CollapsingHeader(Tr(StrId::TilePal_Layers), ImGuiTreeNodeFlags_DefaultOpen)) {
        tileui::DrawLayerPanel(ctx, model_, layer_);
    }
    if (ImGui::CollapsingHeader(Tr(StrId::TilePal_Tiles), ImGuiTreeNodeFlags_DefaultOpen)) {
        DrawPaletteList(ctx);
        if (const uint32_t picked = tileui::DrawPaletteToolbar(paletteModel_, brushTile_)) {
            brushTile_ = picked;
        }
        if (paletteModel_.IsLoaded() && paletteModel_.Palette().Find(brushTile_) == nullptr) {
            brushTile_ = 0; // 消したタイルをブラシに残さない
        }
    }
    if (paletteModel_.IsLoaded() && ImGui::CollapsingHeader(Tr(StrId::TilePal_TileInspector), ImGuiTreeNodeFlags_DefaultOpen)) {
        tileui::DrawTileInspector(ctx, paletteModel_, brushTile_);
    }
    ImGui::EndDisabled();
    ImGui::EndChild();
}

void TilePaletteWindow::DrawPaletteList(EngineContext& ctx)
{
    if (!paletteModel_.IsLoaded()) {
        ImGui::TextDisabled("%s", Tr(StrId::TilePal_NoPalette));
        return;
    }
    const TilePalette* palette = &paletteModel_.Palette();
    if (palette->tiles.empty()) {
        ImGui::TextDisabled("%s", Tr(StrId::TilePal_PaletteEmpty));
        return;
    }
    const float width = ImGui::GetContentRegionAvail().x;
    const float cell = kTileThumbSize + kTileThumbSpacing;
    const int perRow = (std::max)(1, static_cast<int>(width / cell));
    int column = 0;
    for (const TileDef& tile : palette->tiles) {
        ImGui::PushID(static_cast<int>(tile.id));
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        DrawSpriteThumb(ctx, tile.sprite, kTileThumbSize);
        ImGui::SetCursorScreenPos(p0);
        if (ImGui::InvisibleButton("##tile", ImVec2(kTileThumbSize, kTileThumbSize)) && !inPlay_) {
            brushTile_ = tile.id;
            if (tool_ != tileedit::Tool::Brush && tool_ != tileedit::Tool::Rect && tool_ != tileedit::Tool::Fill) {
                SetTool(tileedit::Tool::Brush);
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s  #%u", tile.name.c_str(), tile.id);
        }
        if (tile.id == brushTile_) {
            ImGui::GetWindowDrawList()->AddRect(p0, ImVec2(p0.x + kTileThumbSize, p0.y + kTileThumbSize),
                                                kSelectedTileColor, 0.0f, 2.0f);
        }
        ImGui::PopID();
        if (++column < perRow) {
            ImGui::SameLine(0.0f, kTileThumbSpacing);
        } else {
            column = 0;
        }
    }
    if (column != 0) {
        ImGui::NewLine(); // 行の途中で終わったときも次の部品は新しい行から
    }
}

} // namespace mye
