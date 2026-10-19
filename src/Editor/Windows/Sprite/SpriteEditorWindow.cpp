//====================================================================================
//                          SpriteEditorWindow.cpp
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                          スプライトエディタ窓の実装
//====================================================================================
#include "Editor/Windows/Sprite/SpriteEditorWindow.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h" // SetKeyOwner (Delete / Ctrl+Z / Ctrl+Y / Ctrl+S を握る)
#include "stb/stb_image.h"

#include "Editor/SourceControl/ScmHint.h"
#include "Engine/Core/Localization/Localization.h"
#include "Engine/Engine/Rendering/Sprite2D/SpriteLibrary.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/Device/GpuResources.h"
#include "Engine/Renderer/ImGui/ImGuiTheme.h" // themeColor

namespace mye {
namespace {

using spriteedit::IRect;

constexpr float kDetailWidth = 290.0f;
constexpr float kHandleSize = 7.0f;     // ハンドル 1 辺 (画面画素)
constexpr float kZoomSteps[] = { 0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f, 64.0f };
constexpr int kZoomStepCount = static_cast<int>(sizeof(kZoomSteps) / sizeof(kZoomSteps[0]));
constexpr int kHandleCount = 8;
// ハンドルの位置 (矩形内の割合)。左上から時計回り
constexpr float kHandleFrac[kHandleCount][2] = {
    { 0.0f, 0.0f }, { 0.5f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 0.5f },
    { 1.0f, 1.0f }, { 0.5f, 1.0f }, { 0.0f, 1.0f }, { 0.0f, 0.5f },
};
constexpr StrId kPivotNames[spriteedit::kPivotPresetCount] = {
    StrId::Spr_PivotCenter,     StrId::Spr_PivotTopLeft, StrId::Spr_PivotTop,
    StrId::Spr_PivotTopRight,   StrId::Spr_PivotLeft,    StrId::Spr_PivotRight,
    StrId::Spr_PivotBottomLeft, StrId::Spr_PivotBottom,  StrId::Spr_PivotBottomRight,
};

struct StbDeleter {
    void operator()(stbi_uc* p) const { stbi_image_free(p); }
};
using PixelPtr = std::unique_ptr<stbi_uc, StbDeleter>;

PixelPtr LoadRgba(const std::wstring& path, int& w, int& h)
{
    int comp = 0;
    return PixelPtr(stbi_load(WideToUtf8(path).c_str(), &w, &h, &comp, 4));
}

// 数値欄を触っている間の変更を Undo 1 段にまとめる。変更を適用する前に GestureBegin、後に GestureEnd を呼ぶ
void GestureBegin(SpriteEditModel& model)
{
    if (ImGui::IsItemActivated()) {
        model.BeginGesture();
    }
}

void GestureEnd(SpriteEditModel& model)
{
    if (ImGui::IsItemDeactivated()) {
        model.EndGesture();
    }
}

ImU32 Col(int r, int g, int b, int a = 255)
{
    return IM_COL32(r, g, b, a);
}

} // namespace

// ---------------------------------------------------------------------------
// 開く・保存
// ---------------------------------------------------------------------------

void SpriteEditorWindow::OpenTexture(const std::wstring& path)
{
    open = true;
    if (model_.IsLoaded() && model_.Path() == path) {
        return; // 開いているものをもう一度開いても、編集中の内容を読み直さない
    }
    if (model_.IsLoaded() && model_.Dirty()) {
        pendingPath_ = path;
        modalRequest_ = true;
        return;
    }
    LoadTexture(path);
}

bool SpriteEditorWindow::LoadTexture(const std::wstring& path)
{
    int w = 0;
    int h = 0;
    int comp = 0;
    if (!stbi_info(WideToUtf8(path).c_str(), &w, &h, &comp) || w <= 0 || h <= 0) {
        status_ = StatusKind::LoadFailed;
        model_.Clear();
        return false;
    }
    model_.Load(path, w, h);
    stem_ = WideToUtf8(std::filesystem::path(path).stem().wstring());
    selected_ = -1;
    drag_ = Drag::None;
    status_ = StatusKind::None;
    fitPending_ = true;
    return true;
}

void SpriteEditorWindow::DoSave(EngineContext& ctx)
{
    if (!model_.IsLoaded()) {
        return;
    }
    if (!model_.Save()) {
        status_ = StatusKind::SaveFailed;
        return;
    }
    ctx.resources->textures.ReplaceFromFile(TextureLibrary::IdForFile(model_.Path()), model_.Path());
    scmhint::Changed(model_.Path() + L".meta");
    status_ = StatusKind::Saved;
    statusCount_ = static_cast<int>(model_.Sprites().size());
}

void SpriteEditorWindow::SlicePivot(float& pivotX, float& pivotY) const
{
    if (slicePivotPreset_ >= 0 && slicePivotPreset_ < spriteedit::kPivotPresetCount) {
        spriteedit::PivotPreset(slicePivotPreset_, pivotX, pivotY);
    } else {
        pivotX = sliceCustomPivot_[0];
        pivotY = sliceCustomPivot_[1];
    }
}

void SpriteEditorWindow::DoSlice()
{
    if (!model_.IsLoaded()) {
        return;
    }
    const int texW = model_.TexW();
    const int texH = model_.TexH();
    // 自動分割と「空セルを捨てる」だけが画素を要る
    PixelPtr pixels;
    if (sliceType_ == 2 || !keepEmpty_) {
        int w = 0;
        int h = 0;
        pixels = LoadRgba(model_.Path(), w, h);
        if (!pixels || w != texW || h != texH) {
            status_ = StatusKind::NoPixels;
            return;
        }
    }
    std::vector<IRect> rects;
    switch (sliceType_) {
    case 0:
        rects = spriteedit::SliceGridByCellSize(texW, texH, cellW_, cellH_, offsetX_, offsetY_, padX_, padY_,
                                                keepEmpty_, pixels.get());
        break;
    case 1:
        rects = spriteedit::SliceGridByCellCount(texW, texH, cols_, rows_, offsetX_, offsetY_, padX_, padY_,
                                                 keepEmpty_, pixels.get());
        break;
    default:
        rects = spriteedit::SliceAutomatic(pixels.get(), texW, texH, minSize_);
        break;
    }
    if (rects.empty()) {
        status_ = StatusKind::Nothing; // Delete Existing で全消しにならないよう、何もせず知らせる
        return;
    }
    float pivotX = 0.5f;
    float pivotY = 0.5f;
    SlicePivot(pivotX, pivotY);
    // id は既存の最大 + 1 から振る: Delete Existing でも消した id を使い回さない (古い参照が別の絵を指さない)
    std::vector<importmeta::SpriteSlice> next = spriteedit::ApplySlices(
        model_.Sprites(), rects, static_cast<spriteedit::SliceMethod>(method_), pivotX, pivotY, stem_, model_.NextId());
    model_.ReplaceAll(std::move(next));
    selected_ = -1;
    status_ = StatusKind::Sliced;
    statusCount_ = static_cast<int>(model_.Sprites().size());
}

// ---------------------------------------------------------------------------
// 窓
// ---------------------------------------------------------------------------

void SpriteEditorWindow::OnImGui(EngineContext& ctx)
{
    if (!open) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(960.0f, 680.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(Tr(StrId::Win_SpriteEditor), &open)) {
        ImGui::End();
        return;
    }
    DrawUnsavedModal(ctx);
    if (!model_.IsLoaded()) {
        ImGui::TextWrapped("%s", Tr(StrId::Spr_NoTexture));
        if (status_ == StatusKind::LoadFailed) {
            ImGui::TextColored(themeColor::Error, "%s", Tr(StrId::Spr_StatusLoadFailed));
        }
        ImGui::End();
        return;
    }
    HandleKeys(ctx);
    DrawToolbar(ctx);
    DrawSlicePanel();

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float canvasW = (std::max)(avail.x - kDetailWidth - ImGui::GetStyle().ItemSpacing.x, 64.0f);
    ImGui::BeginChild("##spr_canvas", ImVec2(canvasW, avail.y), true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    DrawCanvas(ctx);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##spr_detail", ImVec2(0.0f, avail.y), true);
    DrawDetail();
    ImGui::EndChild();
    ImGui::End();
}

void SpriteEditorWindow::DrawUnsavedModal(EngineContext& ctx)
{
    if (modalRequest_) {
        ImGui::OpenPopup(Tr(StrId::Spr_ModalTitle));
        modalRequest_ = false;
    }
    if (!ImGui::BeginPopupModal(Tr(StrId::Spr_ModalTitle), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    char text[256];
    std::snprintf(text, sizeof(text), Tr(StrId::Spr_ModalBody), stem_.c_str());
    ImGui::TextUnformatted(text);
    if (ImGui::Button(Tr(StrId::Spr_ModalSave))) {
        DoSave(ctx);
        if (!model_.Dirty()) {
            LoadTexture(pendingPath_);
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr(StrId::Spr_ModalDiscard))) {
        LoadTexture(pendingPath_);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr(StrId::Common_Cancel))) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// Delete / Ctrl+Z / Ctrl+Y / Ctrl+S: Editor 全体のショートカットも同じキーを使うので、
// この窓にフォーカスがあるあいだだけ握る (握らないとシーン側まで動く)
void SpriteEditorWindow::HandleKeys(EngineContext& ctx)
{
    ImGuiIO& io = ImGui::GetIO();
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) || io.WantTextInput) {
        return;
    }
    const ImGuiID owner = ImGui::GetID("##spr_keys");
    ImGui::SetKeyOwner(ImGuiKey_Delete, owner, ImGuiInputFlags_LockUntilRelease);
    ImGui::SetKeyOwner(ImGuiKey_Z, owner, ImGuiInputFlags_LockUntilRelease);
    ImGui::SetKeyOwner(ImGuiKey_Y, owner, ImGuiInputFlags_LockUntilRelease);
    ImGui::SetKeyOwner(ImGuiKey_S, owner, ImGuiInputFlags_LockUntilRelease);
    if (drag_ != Drag::None) {
        return; // ドラッグ中の Undo は 1 段の途中を壊す
    }
    if (selected_ >= 0 && ImGui::IsKeyPressed(ImGuiKey_Delete, ImGuiInputFlags_None, owner)) {
        model_.RemoveAt(selected_);
        selected_ = -1;
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, ImGuiInputFlags_Repeat, owner)) {
        if (io.KeyShift) {
            model_.Redo();
        } else {
            model_.Undo();
        }
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, ImGuiInputFlags_Repeat, owner)) {
        model_.Redo();
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, ImGuiInputFlags_None, owner)) {
        DoSave(ctx);
    }
}

void SpriteEditorWindow::DrawToolbar(EngineContext& ctx)
{
    ImGui::TextDisabled("%s", WideToUtf8(model_.Path()).c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%d x %d%s", model_.TexW(), model_.TexH(), model_.Dirty() ? "  *" : "");

    ImGui::BeginDisabled(!model_.CanUndo());
    if (ImGui::Button(Tr(StrId::Spr_Undo))) {
        model_.Undo();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!model_.CanRedo());
    if (ImGui::Button(Tr(StrId::Spr_Redo))) {
        model_.Redo();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(Tr(StrId::Common_Save))) {
        DoSave(ctx);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!model_.Dirty());
    if (ImGui::Button(Tr(StrId::Insp_Revert))) {
        model_.RevertToSaved();
        selected_ = -1;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(Tr(StrId::Spr_Fit))) {
        fitPending_ = true;
    }
    ImGui::SameLine();
    char zoomText[48];
    std::snprintf(zoomText, sizeof(zoomText), Tr(StrId::Spr_Zoom), static_cast<double>(zoom_));
    ImGui::TextUnformatted(zoomText);

    char statusText[160];
    switch (status_) {
    case StatusKind::None:
        ImGui::TextDisabled("%s", Tr(StrId::Spr_Help));
        break;
    case StatusKind::Sliced:
        std::snprintf(statusText, sizeof(statusText), Tr(StrId::Spr_StatusSliced), statusCount_);
        ImGui::TextColored(themeColor::Success, "%s", statusText);
        break;
    case StatusKind::Saved:
        std::snprintf(statusText, sizeof(statusText), Tr(StrId::Spr_StatusSaved), statusCount_);
        ImGui::TextColored(themeColor::Success, "%s", statusText);
        break;
    case StatusKind::Nothing:
        ImGui::TextColored(themeColor::Warning, "%s", Tr(StrId::Spr_StatusNothing));
        break;
    case StatusKind::SaveFailed:
        ImGui::TextColored(themeColor::Error, "%s", Tr(StrId::Spr_StatusSaveFailed));
        break;
    case StatusKind::LoadFailed:
        ImGui::TextColored(themeColor::Error, "%s", Tr(StrId::Spr_StatusLoadFailed));
        break;
    case StatusKind::NoPixels:
        ImGui::TextColored(themeColor::Error, "%s", Tr(StrId::Spr_StatusNoPixels));
        break;
    }
}

void SpriteEditorWindow::DrawSlicePanel()
{
    if (!ImGui::CollapsingHeader(Tr(StrId::Spr_Slice), ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    const char* typeLabels[] = { Tr(StrId::Spr_TypeCellSize), Tr(StrId::Spr_TypeCellCount), Tr(StrId::Spr_TypeAuto) };
    ImGui::SetNextItemWidth(200.0f);
    ImGui::Combo(Tr(StrId::Spr_Type), &sliceType_, typeLabels, 3);

    ImGui::SetNextItemWidth(200.0f);
    if (sliceType_ == 0) {
        int cell[2] = { cellW_, cellH_ };
        if (ImGui::InputInt2(Tr(StrId::Spr_CellSize), cell)) {
            cellW_ = (std::max)(cell[0], 1);
            cellH_ = (std::max)(cell[1], 1);
        }
    } else if (sliceType_ == 1) {
        int count[2] = { cols_, rows_ };
        if (ImGui::InputInt2(Tr(StrId::Spr_CellCount), count)) {
            cols_ = (std::max)(count[0], 1);
            rows_ = (std::max)(count[1], 1);
        }
    } else {
        ImGui::InputInt(Tr(StrId::Spr_MinSize), &minSize_);
        minSize_ = (std::max)(minSize_, 1);
    }
    if (sliceType_ != 2) {
        int offset[2] = { offsetX_, offsetY_ };
        ImGui::SetNextItemWidth(200.0f);
        if (ImGui::InputInt2(Tr(StrId::Spr_Offset), offset)) {
            offsetX_ = (std::max)(offset[0], 0);
            offsetY_ = (std::max)(offset[1], 0);
        }
        int pad[2] = { padX_, padY_ };
        ImGui::SetNextItemWidth(200.0f);
        if (ImGui::InputInt2(Tr(StrId::Spr_Padding), pad)) {
            padX_ = (std::max)(pad[0], 0);
            padY_ = (std::max)(pad[1], 0);
        }
        ImGui::Checkbox(Tr(StrId::Spr_KeepEmpty), &keepEmpty_);
    }

    const char* methodLabels[] = { Tr(StrId::Spr_MethodDelete), Tr(StrId::Spr_MethodSafe) };
    ImGui::SetNextItemWidth(200.0f);
    ImGui::Combo(Tr(StrId::Spr_Method), &method_, methodLabels, 2);

    const char* pivotLabels[spriteedit::kPivotPresetCount + 1];
    for (int k = 0; k < spriteedit::kPivotPresetCount; ++k) {
        pivotLabels[k] = Tr(kPivotNames[k]);
    }
    pivotLabels[spriteedit::kPivotCustom] = Tr(StrId::Spr_PivotCustom);
    ImGui::SetNextItemWidth(200.0f);
    ImGui::Combo(Tr(StrId::Spr_Pivot), &slicePivotPreset_, pivotLabels, spriteedit::kPivotPresetCount + 1);
    if (slicePivotPreset_ == spriteedit::kPivotCustom) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140.0f);
        ImGui::DragFloat2("##spr_slice_pivot", sliceCustomPivot_, 0.01f, 0.0f, 1.0f, "%.2f");
    }
    if (ImGui::Button(Tr(StrId::Common_Apply), ImVec2(90, 0))) {
        DoSlice();
    }
}

// ---------------------------------------------------------------------------
// キャンバス
// ---------------------------------------------------------------------------

void SpriteEditorWindow::FitView(const ImVec2& canvasSize)
{
    const float fitX = canvasSize.x / static_cast<float>((std::max)(model_.TexW(), 1));
    const float fitY = canvasSize.y / static_cast<float>((std::max)(model_.TexH(), 1));
    const float fit = (std::min)(fitX, fitY) * 0.95f;
    // 表示倍率は整数倍と 1/2 だけ: 収まる最大の段
    zoom_ = kZoomSteps[0];
    for (int i = 0; i < kZoomStepCount; ++i) {
        if (kZoomSteps[i] <= fit) {
            zoom_ = kZoomSteps[i];
        }
    }
    panX_ = (canvasSize.x - static_cast<float>(model_.TexW()) * zoom_) * 0.5f;
    panY_ = (canvasSize.y - static_cast<float>(model_.TexH()) * zoom_) * 0.5f;
}

void SpriteEditorWindow::DrawCanvas(EngineContext& ctx)
{
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size = ImGui::GetContentRegionAvail();
    if (size.x < 8.0f || size.y < 8.0f) {
        return;
    }
    if (fitPending_) {
        FitView(size);
        fitPending_ = false;
    }
    ImGui::InvisibleButton("##spr_hit", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle
                               | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();

    // ---- ズーム (マウス位置を固定) とパン ----
    if (hovered && io.MouseWheel != 0.0f) {
        int step = 0;
        for (int i = 0; i < kZoomStepCount; ++i) {
            if (kZoomSteps[i] <= zoom_) {
                step = i;
            }
        }
        step = std::clamp(step + (io.MouseWheel > 0.0f ? 1 : -1), 0, kZoomStepCount - 1);
        const float next = kZoomSteps[step];
        const float texX = (io.MousePos.x - origin.x - panX_) / zoom_;
        const float texY = (io.MousePos.y - origin.y - panY_) / zoom_;
        zoom_ = next;
        panX_ = io.MousePos.x - origin.x - texX * zoom_;
        panY_ = io.MousePos.y - origin.y - texY * zoom_;
    }
    if (active && (ImGui::IsMouseDown(ImGuiMouseButton_Middle) || ImGui::IsMouseDown(ImGuiMouseButton_Right))) {
        panX_ += io.MouseDelta.x;
        panY_ += io.MouseDelta.y;
    }

    const ImVec2 imgMin(origin.x + panX_, origin.y + panY_);
    const ImVec2 imgMax(imgMin.x + static_cast<float>(model_.TexW()) * zoom_,
                        imgMin.y + static_cast<float>(model_.TexH()) * zoom_);
    auto toScreen = [&](float x, float y) { return ImVec2(imgMin.x + x * zoom_, imgMin.y + y * zoom_); };
    // マウスの画素座標 (小数)
    const float mouseTexX = (io.MousePos.x - imgMin.x) / zoom_;
    const float mouseTexY = (io.MousePos.y - imgMin.y) / zoom_;
    const std::vector<importmeta::SpriteSlice>& sprites = model_.Sprites();
    const int spriteCount = static_cast<int>(sprites.size());
    if (selected_ >= spriteCount) {
        selected_ = -1; // Undo で個数が減った
    }

    // ---- 左ボタン: 選択・移動・リサイズ・新規 ----
    if (drag_ == Drag::None && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const int px = static_cast<int>(std::floor(mouseTexX));
        const int py = static_cast<int>(std::floor(mouseTexY));
        dragStartX_ = px;
        dragStartY_ = py;
        int hitHandle = -1;
        if (selected_ >= 0) {
            const importmeta::SpriteSlice& s = sprites[selected_];
            for (int k = 0; k < kHandleCount; ++k) {
                const ImVec2 c = toScreen(static_cast<float>(s.x) + kHandleFrac[k][0] * static_cast<float>(s.w),
                                          static_cast<float>(s.y) + kHandleFrac[k][1] * static_cast<float>(s.h));
                if (std::fabs(io.MousePos.x - c.x) <= kHandleSize && std::fabs(io.MousePos.y - c.y) <= kHandleSize) {
                    hitHandle = k;
                    break;
                }
            }
        }
        if (hitHandle >= 0) {
            const importmeta::SpriteSlice& s = sprites[selected_];
            drag_ = Drag::Resize;
            dragHandle_ = hitHandle;
            dragStartRect_ = { s.x, s.y, s.w, s.h };
            model_.BeginGesture();
        } else {
            // 選択中の矩形を優先し、重なっていれば後ろ (手前に描く) のものを取る
            auto contains = [&](int i) {
                const importmeta::SpriteSlice& s = sprites[i];
                return px >= s.x && px < s.x + s.w && py >= s.y && py < s.y + s.h;
            };
            int hit = -1;
            if (selected_ >= 0 && contains(selected_)) {
                hit = selected_;
            } else {
                for (int i = spriteCount - 1; i >= 0; --i) {
                    if (contains(i)) {
                        hit = i;
                        break;
                    }
                }
            }
            selected_ = hit;
            if (hit >= 0) {
                const importmeta::SpriteSlice& s = sprites[hit];
                drag_ = Drag::Move;
                dragStartRect_ = { s.x, s.y, s.w, s.h };
                model_.BeginGesture();
            } else {
                drag_ = Drag::Create;
                createRect_ = { px, py, 0, 0 };
            }
        }
    }
    if (drag_ != Drag::None) {
        const int curX = static_cast<int>(std::lround(mouseTexX));
        const int curY = static_cast<int>(std::lround(mouseTexY));
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (drag_ == Drag::Move && selected_ >= 0) {
                IRect r = dragStartRect_;
                r.x += static_cast<int>(std::floor(mouseTexX)) - dragStartX_;
                r.y += static_cast<int>(std::floor(mouseTexY)) - dragStartY_;
                r.x = std::clamp(r.x, 0, (std::max)(model_.TexW() - r.w, 0)); // 大きさを保って収める
                r.y = std::clamp(r.y, 0, (std::max)(model_.TexH() - r.h, 0));
                model_.SetRect(selected_, r);
            } else if (drag_ == Drag::Resize && selected_ >= 0) {
                int left = dragStartRect_.x;
                int top = dragStartRect_.y;
                int right = dragStartRect_.x + dragStartRect_.w;
                int bottom = dragStartRect_.y + dragStartRect_.h;
                const float fx = kHandleFrac[dragHandle_][0];
                const float fy = kHandleFrac[dragHandle_][1];
                if (fx == 0.0f) {
                    left = (std::min)(curX, right - 1);
                } else if (fx == 1.0f) {
                    right = (std::max)(curX, left + 1);
                }
                if (fy == 0.0f) {
                    top = (std::min)(curY, bottom - 1);
                } else if (fy == 1.0f) {
                    bottom = (std::max)(curY, top + 1);
                }
                model_.SetRect(selected_, { left, top, right - left, bottom - top });
            } else if (drag_ == Drag::Create) {
                createRect_.w = std::floor(mouseTexX) >= static_cast<float>(createRect_.x)
                    ? static_cast<int>(std::floor(mouseTexX)) - createRect_.x + 1
                    : static_cast<int>(std::floor(mouseTexX)) - createRect_.x;
                createRect_.h = std::floor(mouseTexY) >= static_cast<float>(createRect_.y)
                    ? static_cast<int>(std::floor(mouseTexY)) - createRect_.y + 1
                    : static_cast<int>(std::floor(mouseTexY)) - createRect_.y;
            }
        } else {
            if (drag_ == Drag::Create) {
                // 負の幅 (右下から左上へのドラッグ) は正規化する。1 画素の揺れでは作らない
                IRect r = createRect_;
                if (r.w < 0) {
                    r.x += r.w + 1;
                    r.w = -r.w;
                }
                if (r.h < 0) {
                    r.y += r.h + 1;
                    r.h = -r.h;
                }
                if (r.w >= 2 || r.h >= 2) {
                    float pivotX = 0.5f;
                    float pivotY = 0.5f;
                    SlicePivot(pivotX, pivotY);
                    selected_ = model_.AddRect(r, pivotX, pivotY, stem_ + "_" + std::to_string(model_.NextId()));
                }
            }
            model_.EndGesture();
            drag_ = Drag::None;
        }
    }

    // ---- 描画 ----
    dl->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
    // 透明が分かる市松 (画像の範囲だけ)
    {
        const float cell = 8.0f;
        const float x0 = (std::max)(imgMin.x, origin.x);
        const float y0 = (std::max)(imgMin.y, origin.y);
        const float x1 = (std::min)(imgMax.x, origin.x + size.x);
        const float y1 = (std::min)(imgMax.y, origin.y + size.y);
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), Col(60, 60, 60));
        const int startI = static_cast<int>(std::floor((x0 - imgMin.x) / cell));
        const int startJ = static_cast<int>(std::floor((y0 - imgMin.y) / cell));
        for (int j = startJ; imgMin.y + static_cast<float>(j) * cell < y1; ++j) {
            for (int i = startI; imgMin.x + static_cast<float>(i) * cell < x1; ++i) {
                if (((i + j) & 1) == 0) {
                    continue;
                }
                dl->AddRectFilled(ImVec2((std::max)(imgMin.x + static_cast<float>(i) * cell, x0),
                                         (std::max)(imgMin.y + static_cast<float>(j) * cell, y0)),
                                  ImVec2((std::min)(imgMin.x + static_cast<float>(i + 1) * cell, x1),
                                         (std::min)(imgMin.y + static_cast<float>(j + 1) * cell, y1)),
                                  Col(90, 90, 90));
            }
        }
    }
    const AssetID texId = ctx.resources->textures.RequestLoadFileAsync(model_.Path());
    const Texture* tex = ctx.resources->textures.Get(texId);
    // 読み込み中は 1x1 のプレースホルダなので、実寸が 1x1 のときだけ描く
    if (tex != nullptr && tex->srv && (tex->width > 1 || (model_.TexW() == 1 && model_.TexH() == 1))) {
        dl->AddImage(reinterpret_cast<ImTextureID>(tex->srv.Get()), imgMin, imgMax);
    }
    dl->AddRect(imgMin, imgMax, Col(200, 200, 200, 120));

    for (int i = 0; i < spriteCount; ++i) {
        const importmeta::SpriteSlice& s = sprites[i];
        const bool isSelected = i == selected_;
        const ImVec2 a = toScreen(static_cast<float>(s.x), static_cast<float>(s.y));
        const ImVec2 b = toScreen(static_cast<float>(s.x + s.w), static_cast<float>(s.y + s.h));
        dl->AddRect(a, b, isSelected ? Col(0, 220, 255) : Col(255, 220, 60, 220), 0.0f, isSelected ? 2.0f : 1.0f);
        if (!isSelected && zoom_ >= 2.0f) {
            dl->AddText(ImVec2(a.x + 2.0f, a.y + 1.0f), Col(255, 220, 60), s.name);
        }
    }
    if (selected_ >= 0) {
        const importmeta::SpriteSlice& s = sprites[selected_];
        // border (緑)
        const ImU32 green = Col(80, 255, 120);
        const float x0 = static_cast<float>(s.x);
        const float y0 = static_cast<float>(s.y);
        const float x1 = static_cast<float>(s.x + s.w);
        const float y1 = static_cast<float>(s.y + s.h);
        if (s.borderL > 0) {
            dl->AddLine(toScreen(x0 + static_cast<float>(s.borderL), y0), toScreen(x0 + static_cast<float>(s.borderL), y1), green);
        }
        if (s.borderR > 0) {
            dl->AddLine(toScreen(x1 - static_cast<float>(s.borderR), y0), toScreen(x1 - static_cast<float>(s.borderR), y1), green);
        }
        if (s.borderT > 0) {
            dl->AddLine(toScreen(x0, y0 + static_cast<float>(s.borderT)), toScreen(x1, y0 + static_cast<float>(s.borderT)), green);
        }
        if (s.borderB > 0) {
            dl->AddLine(toScreen(x0, y1 - static_cast<float>(s.borderB)), toScreen(x1, y1 - static_cast<float>(s.borderB)), green);
        }
        // pivot (pivot は左下原点、画素は左上原点)
        const ImVec2 pv = toScreen(x0 + s.pivotX * static_cast<float>(s.w), y0 + (1.0f - s.pivotY) * static_cast<float>(s.h));
        dl->AddCircleFilled(pv, 4.0f, Col(255, 80, 80));
        dl->AddCircle(pv, 4.0f, Col(255, 255, 255));
        // ハンドル
        for (int k = 0; k < kHandleCount; ++k) {
            const ImVec2 c = toScreen(x0 + kHandleFrac[k][0] * static_cast<float>(s.w), y0 + kHandleFrac[k][1] * static_cast<float>(s.h));
            const float half = kHandleSize * 0.5f;
            dl->AddRectFilled(ImVec2(c.x - half, c.y - half), ImVec2(c.x + half, c.y + half), Col(255, 255, 255));
            dl->AddRect(ImVec2(c.x - half, c.y - half), ImVec2(c.x + half, c.y + half), Col(0, 0, 0));
        }
    }
    if (drag_ == Drag::Create) {
        IRect r = createRect_;
        if (r.w < 0) {
            r.x += r.w + 1;
            r.w = -r.w;
        }
        if (r.h < 0) {
            r.y += r.h + 1;
            r.h = -r.h;
        }
        dl->AddRect(toScreen(static_cast<float>(r.x), static_cast<float>(r.y)),
                    toScreen(static_cast<float>(r.x + r.w), static_cast<float>(r.y + r.h)), Col(0, 220, 255), 0.0f, 1.0f);
    }
    dl->PopClipRect();
}

// ---------------------------------------------------------------------------
// 右: 選択矩形の詳細
// ---------------------------------------------------------------------------

void SpriteEditorWindow::DrawDetail()
{
    const std::vector<importmeta::SpriteSlice>& sprites = model_.Sprites();
    char countText[48];
    std::snprintf(countText, sizeof(countText), Tr(StrId::Spr_Sprites), static_cast<int>(sprites.size()));
    ImGui::TextUnformatted(countText);
    // 一覧: 行をクリックで選択
    ImGui::BeginChild("##spr_list", ImVec2(0.0f, 130.0f), true);
    for (int i = 0; i < static_cast<int>(sprites.size()); ++i) {
        ImGui::PushID(i);
        if (ImGui::Selectable(sprites[i].name, i == selected_)) {
            selected_ = i;
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    if (selected_ < 0 || selected_ >= static_cast<int>(sprites.size())) {
        ImGui::TextWrapped("%s", Tr(StrId::Spr_NoSelection));
        return;
    }
    const importmeta::SpriteSlice s = sprites[selected_]; // 編集で配列が動いても今フレームは同じ値を見る
    ImGui::Separator();

    char name[sizeof(s.name)];
    std::snprintf(name, sizeof(name), "%s", s.name);
    ImGui::TextUnformatted(Tr(StrId::Spr_Name));
    ImGui::SetNextItemWidth(-1.0f);
    const bool nameChanged = ImGui::InputText("##spr_name", name, sizeof(name));
    GestureBegin(model_);
    if (nameChanged) {
        model_.SetName(selected_, name);
    }
    GestureEnd(model_);

    ImGui::TextUnformatted(Tr(StrId::Spr_Position));
    int pos[2] = { s.x, s.y };
    ImGui::SetNextItemWidth(-1.0f);
    const bool posChanged = ImGui::InputInt2("##spr_pos", pos);
    GestureBegin(model_);
    if (posChanged) {
        model_.SetRect(selected_, { pos[0], pos[1], s.w, s.h });
    }
    GestureEnd(model_);

    ImGui::TextUnformatted(Tr(StrId::Spr_Size));
    int dim[2] = { s.w, s.h };
    ImGui::SetNextItemWidth(-1.0f);
    const bool dimChanged = ImGui::InputInt2("##spr_size", dim);
    GestureBegin(model_);
    if (dimChanged) {
        model_.SetRect(selected_, { s.x, s.y, dim[0], dim[1] });
    }
    GestureEnd(model_);

    ImGui::TextUnformatted(Tr(StrId::Spr_Pivot));
    const int preset = spriteedit::PivotPresetOf(s.pivotX, s.pivotY);
    const char* pivotLabels[spriteedit::kPivotPresetCount + 1];
    for (int k = 0; k < spriteedit::kPivotPresetCount; ++k) {
        pivotLabels[k] = Tr(kPivotNames[k]);
    }
    pivotLabels[spriteedit::kPivotCustom] = Tr(StrId::Spr_PivotCustom);
    int presetEdit = preset;
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::Combo("##spr_pivot_preset", &presetEdit, pivotLabels, spriteedit::kPivotPresetCount + 1)
        && presetEdit != spriteedit::kPivotCustom) {
        float px = 0.5f;
        float py = 0.5f;
        spriteedit::PivotPreset(presetEdit, px, py);
        model_.SetPivot(selected_, px, py);
    }
    float pivot[2] = { s.pivotX, s.pivotY };
    ImGui::SetNextItemWidth(-1.0f);
    const bool pivotChanged = ImGui::DragFloat2("##spr_pivot", pivot, 0.005f, -1.0f, 2.0f, "%.3f");
    GestureBegin(model_);
    if (pivotChanged) {
        model_.SetPivot(selected_, pivot[0], pivot[1]);
    }
    GestureEnd(model_);

    ImGui::TextUnformatted(Tr(StrId::Spr_Border));
    int border[4] = { s.borderL, s.borderB, s.borderR, s.borderT };
    ImGui::SetNextItemWidth(-1.0f);
    const bool borderChanged = ImGui::InputInt4("##spr_border", border);
    GestureBegin(model_);
    if (borderChanged) {
        model_.SetBorder(selected_, border[0], border[1], border[2], border[3]);
    }
    GestureEnd(model_);

    ImGui::Spacing();
    if (ImGui::Button(Tr(StrId::Common_Delete), ImVec2(90, 0))) {
        model_.RemoveAt(selected_);
        selected_ = -1;
    }
}

} // namespace mye
