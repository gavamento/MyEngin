//====================================================================================
//                          ScenePreviewWindow.cpp
//  MyEngine/ 秋田蓮音                                                      11/16/2026
//                                          Before / After の 3D 比較の窓 (専用 RT に描く。実装)
//====================================================================================
#include "Editor/Windows/Project/ScenePreviewWindow.h"

#include <algorithm>
#include <cmath>

#include "imgui.h"

#include "Editor/ChangeTracking/ScenePreviewFormat.h"
#include "Engine/Core/Localization/Localization.h"
#include "Engine/Engine/Loop/EngineLoop.h"
#include "Engine/Renderer/Device/GpuResources.h"
#include "Engine/Renderer/ImGui/ImGuiTheme.h"

namespace mye {

using namespace DirectX;

namespace {

// 画像の最小の大きさ (これより狭い窓では描かない)
constexpr float kMinImageSize = 64.0f;
// 印の四角の半径と線の太さ (px)
constexpr float kMarkerHalfSize = 7.0f;
constexpr float kMarkerThickness = 2.0f;
// 背景色 (AnimationPreview と同じ。ポストプロセスを通さないので RT の形式が sRGB への変換を持つ)
constexpr float kClearColor[4] = { 0.0152f, 0.0152f, 0.0194f, 1.0f };

ImU32 ToImColor(const OverlayColor& color)
{
    return ImGui::ColorConvertFloat4ToU32(ImVec4(color.r, color.g, color.b, color.a));
}

} // namespace

void ScenePreviewWindow::SetFileIo(std::unique_ptr<RestoreFileIo> io)
{
    Close(); // 古い io を指す Session を先に閉じる
    io_ = std::move(io);
}

bool ScenePreviewWindow::Open(ChangeTrackingSession& session, ScenePreviewOpenArgs args)
{
    if (!io_) {
        io_ = MakeSystemRestoreFileIo();
    }
    ReleaseTargets();
    open = session_.Open(session.Client(), *io_, std::move(args));
    focusRequested_ = open;
    return open;
}

void ScenePreviewWindow::Close()
{
    session_.Close();
    ReleaseTargets();
    imageCount_ = 0;
    open = false;
}

void ScenePreviewWindow::ReleaseTargets()
{
    for (int i = 0; i < 2; ++i) {
        rt_[i].Release();
        if (render_[i]) {
            render_[i]->ReleaseGpu();
            render_[i].reset();
        }
    }
}

void ScenePreviewWindow::ReleaseGpu()
{
    // デバイス消失: GPU の資源だけ手放す。Scene は CPU 側にあるので残り、次の OnRenderViews で RT を作り直す
    ReleaseTargets();
}

void ScenePreviewWindow::OnImGui(ChangeTrackingSession& session)
{
    if (!open) {
        if (session_.IsOpen()) {
            Close(); // x で閉じた: Scene と RT を破棄する
        }
        return;
    }
    ImGui::SetNextWindowSize({ 960.0f, 680.0f }, ImGuiCond_FirstUseEver);
    if (focusRequested_) {
        ImGui::SetNextWindowFocus();
        focusRequested_ = false;
    }
    imageCount_ = 0; // 描かれなかったフレーム (折りたたみ・狭い窓) では RT を更新しない
    if (ImGui::Begin(Tr(StrId::Win_ScenePreview), &open)) {
        if (!session_.IsOpen()) {
            ImGui::TextWrapped("%s", Tr(StrId::ScPv_Empty));
        } else {
            session_.Tick(session.Client());
            DrawControls(session);
            DrawImages(session);
        }
    }
    ImGui::End();
    if (!open) {
        Close(); // x で閉じた: Scene と RT を破棄する
    }
}

void ScenePreviewWindow::DrawControls(ChangeTrackingSession& session)
{
    const ScenePreviewScreen screen = BuildScenePreviewScreen(session_);
    ImGui::TextUnformatted(screen.target.c_str());

    int mode = static_cast<int>(session_.Mode());
    ImGui::RadioButton(Tr(StrId::ScPv_ModeBefore), &mode, static_cast<int>(PreviewMode::Before));
    ImGui::SameLine();
    ImGui::RadioButton(Tr(StrId::ScPv_ModeAfter), &mode, static_cast<int>(PreviewMode::After));
    ImGui::SameLine();
    ImGui::RadioButton(Tr(StrId::ScPv_ModeOverlay), &mode, static_cast<int>(PreviewMode::Overlay));
    ImGui::SameLine();
    ImGui::RadioButton(Tr(StrId::ScPv_ModeSide), &mode, static_cast<int>(PreviewMode::SideBySide));
    session_.SetMode(static_cast<PreviewMode>(mode));
    ImGui::SameLine();
    if (ImGui::SmallButton(Tr(StrId::ScPv_FitCamera))) {
        session_.FitCamera();
    }

    int source = static_cast<int>(session_.AfterSource());
    ImGui::TextUnformatted(Tr(StrId::ScPv_LabelAfter));
    ImGui::SameLine();
    ImGui::RadioButton(Tr(StrId::ScPv_SrcRevision), &source, static_cast<int>(PreviewAfterSource::Revision));
    ImGui::SameLine();
    ImGui::RadioButton(Tr(StrId::ScPv_SrcDisk), &source, static_cast<int>(PreviewAfterSource::Disk));
    if (static_cast<PreviewAfterSource>(source) != session_.AfterSource()) {
        session_.SetAfterSource(static_cast<PreviewAfterSource>(source), session.Client());
    }

    const auto line = [](const std::string& text, bool failed) {
        if (failed) {
            ImGui::PushStyleColor(ImGuiCol_Text, themeColor::Warning);
        }
        ImGui::TextWrapped("%s", text.c_str());
        if (failed) {
            ImGui::PopStyleColor();
        }
    };
    line(screen.beforeLine, session_.Before().state == ScenePreviewSession::SideState::Failed);
    line(screen.afterLine, session_.After().state == ScenePreviewSession::SideState::Failed);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    for (const std::string& note : screen.notes) {
        ImGui::Bullet();
        ImGui::TextWrapped("%s", note.c_str());
    }
    ImGui::PopStyleColor();
}

void ScenePreviewWindow::DrawImages(ChangeTrackingSession& session)
{
    const PreviewMode mode = session_.Mode();
    // 重ねは After を土台にする。After が無い (削除) ときは Before
    const int overlayBase = session_.AfterScene() != nullptr ? kSideAfter : kSideBefore;
    int slots[2] = { kSideAfter, kSideAfter };
    int count = 1;
    switch (mode) {
    case PreviewMode::Before: slots[0] = kSideBefore; break;
    case PreviewMode::After: slots[0] = kSideAfter; break;
    case PreviewMode::Overlay: slots[0] = overlayBase; break;
    case PreviewMode::SideBySide:
        slots[0] = kSideBefore;
        slots[1] = kSideAfter;
        count = 2;
        break;
    }
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float cellWidth = std::floor((avail.x - spacing * static_cast<float>(count - 1)) / static_cast<float>(count));
    if (cellWidth < kMinImageSize || avail.y < kMinImageSize) {
        imageCount_ = 0;
        return;
    }
    imageCount_ = count;
    imageWidth_ = static_cast<int>(cellWidth);
    imageHeight_ = static_cast<int>(avail.y);
    for (int i = 0; i < count; ++i) {
        slotSide_[i] = slots[i];
        if (i > 0) {
            ImGui::SameLine();
        }
        const ImVec2 rectMin = ImGui::GetCursorScreenPos();
        const ImVec2 size(cellWidth, avail.y);
        PreviewScene* scene = slots[i] == kSideBefore ? session_.BeforeScene() : session_.AfterScene();
        if (scene != nullptr && rt_[i].IsValid() && rt_[i].SRV() != nullptr) {
            ImGui::Image(reinterpret_cast<ImTextureID>(rt_[i].SRV()), size);
        } else {
            ImGui::Dummy(size);
        }
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRect(rectMin, ImVec2(rectMin.x + size.x, rectMin.y + size.y), ImGui::GetColorU32(ImGuiCol_Border));
        if (scene == nullptr) {
            drawList->AddText(ImVec2(rectMin.x + 8.0f, rectMin.y + 8.0f), ImGui::GetColorU32(ImGuiCol_TextDisabled), Tr(StrId::ScPv_NoRender));
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", Tr(StrId::ScPv_Hint));
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                session_.OrbitCamera(ImGui::GetIO().MouseDelta.x, ImGui::GetIO().MouseDelta.y);
            }
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Right) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
                session_.PanCamera(ImGui::GetIO().MouseDelta.x, ImGui::GetIO().MouseDelta.y);
            }
            session_.ZoomCamera(ImGui::GetIO().MouseWheel);
        }
        const char* label = slots[i] == kSideBefore ? Tr(StrId::ScPv_ModeBefore) : Tr(StrId::ScPv_ModeAfter);
        if (mode == PreviewMode::Overlay) {
            label = Tr(StrId::ScPv_ModeOverlay);
        }
        drawList->AddText(ImVec2(rectMin.x + 8.0f, rectMin.y + (scene == nullptr ? 26.0f : 8.0f)), ImGui::GetColorU32(ImGuiCol_Text), label);
        if (mode == PreviewMode::Overlay && scene != nullptr) {
            DrawMarkers(session, rectMin.x, rectMin.y, size.x, size.y);
        }
    }
}

void ScenePreviewWindow::DrawMarkers(ChangeTrackingSession& session, float rectX, float rectY, float width, float height)
{
    const XMMATRIX viewProj = XMMatrixMultiply(session_.ViewMatrix(), session_.ProjMatrix(width / (std::max)(height, 1.0f)));
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(ImVec2(rectX, rectY), ImVec2(rectX + width, rectY + height), true);
    const OverlayPalette& palette = session.Overlay().Palette();
    for (const PreviewMarker& marker : session_.Markers()) {
        const XMVECTOR clip = XMVector4Transform(XMVectorSet(marker.position.x, marker.position.y, marker.position.z, 1.0f), viewProj);
        const float w = XMVectorGetW(clip);
        if (w <= 1e-4f) {
            continue; // カメラの後ろ
        }
        const float x = rectX + (XMVectorGetX(clip) / w * 0.5f + 0.5f) * width;
        const float y = rectY + (0.5f - XMVectorGetY(clip) / w * 0.5f) * height;
        const ImU32 color = ToImColor(palette.Get(marker.color));
        const ImVec2 low(x - kMarkerHalfSize, y - kMarkerHalfSize);
        const ImVec2 high(x + kMarkerHalfSize, y + kMarkerHalfSize);
        drawList->AddRect(low, high, color, 0.0f, kMarkerThickness);
        std::string text = marker.name;
        if (marker.fromBefore) {
            // 削除されたものは Before の位置。斜線で「実体が無い」ことを示す
            drawList->AddLine(low, high, color, kMarkerThickness);
            text += std::string(" ") + Tr(StrId::ScPv_MarkerRemoved);
        }
        drawList->AddText(ImVec2(x + kMarkerHalfSize + 3.0f, y - 7.0f), color, text.c_str());
    }
    drawList->PopClipRect();
}

void ScenePreviewWindow::OnRenderViews(EngineContext& ctx)
{
    if (!open || !session_.IsOpen() || imageCount_ <= 0 || ctx.device == nullptr || ctx.renderPathForward == nullptr || ctx.shaders == nullptr
        || ctx.resources == nullptr) {
        return;
    }
    const XMMATRIX view = session_.ViewMatrix();
    for (int i = imageCount_; i < 2; ++i) {
        rt_[i].Release(); // 左右から 1 枚の表示に戻した: 使わない枠の RT を持ち続けない
    }
    for (int i = 0; i < imageCount_; ++i) {
        const int side = slotSide_[i];
        PreviewScene* scene = side == kSideBefore ? session_.BeforeScene() : session_.AfterScene();
        if (scene == nullptr) {
            continue;
        }
        // ポストプロセスを通さないので、sRGB への変換は RT の形式に任せる (AnimationPreview / AssetPreviewCache と同じ)
        rt_[i].Resize(*ctx.device, imageWidth_, imageHeight_, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
        if (!rt_[i].IsValid()) {
            continue;
        }
        World& world = scene->scene.GetWorld();
        scene->transforms.Update(world);
        CameraOverride camera;
        XMStoreFloat4x4(&camera.view, view);
        XMStoreFloat3(&camera.position, XMMatrixInverse(nullptr, view).r[3]);
        camera.fovYDeg = kPreviewFovYDeg;
        camera.nearZ = kPreviewNearZ;
        camera.farZ = kPreviewFarZ;
        if (!render_[side]) {
            render_[side] = std::make_unique<RenderSystem>();
        }
        RenderSystem& render = *render_[side];
        render.enablePostFx = false;
        render.enableShadows = false;
        FrameTarget target;
        target.rtv = rt_[i].RTV();
        target.dsv = rt_[i].DSV();
        target.width = rt_[i].Width();
        target.height = rt_[i].Height();
        for (int c = 0; c < 4; ++c) {
            target.clearColor[c] = kClearColor[c];
        }
        render.Render(world, *ctx.device, *ctx.renderPathForward, *ctx.shaders, *ctx.resources, target, &camera, nullptr);
    }
}

} // namespace mye
