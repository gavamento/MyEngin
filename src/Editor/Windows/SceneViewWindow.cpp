#include "Editor/Windows/SceneViewWindow.h"

#include <algorithm>

#include "Engine/Engine/RenderSystem.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Renderer/RenderPath.h"

#include "imgui.h"

using namespace DirectX;

namespace mye {

void SceneViewWindow::OnRenderViews(EngineContext& ctx)
{
    if (desiredW_ <= 0 || desiredH_ <= 0) {
        return;
    }
    rt_.Resize(*ctx.device, desiredW_, desiredH_);
    if (!rt_.IsValid()) {
        return;
    }

    const XMMATRIX camWorld =
        XMMatrixRotationRollPitchYaw(camPitch_ * (3.14159265f / 180.0f),
                                     camYaw_ * (3.14159265f / 180.0f), 0.0f)
        * XMMatrixTranslation(camPos_.x, camPos_.y, camPos_.z);

    CameraOverride cam;
    XMStoreFloat4x4(&cam.view, XMMatrixInverse(nullptr, camWorld));
    cam.position = camPos_;
    cam.fovYDeg = 60.0f;

    FrameTarget target;
    target.rtv = rt_.RTV();
    target.dsv = rt_.DSV();
    target.width = rt_.Width();
    target.height = rt_.Height();
    ctx.renderSystem->Render(ctx.scene->GetWorld(), *ctx.device, *ctx.renderPath, *ctx.shaders,
                             *ctx.resources, target, &cam, ctx.particles);
}

void SceneViewWindow::OnImGui(EngineContext& ctx)
{
    (void)ctx;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    const bool visible = ImGui::Begin("Scene");
    ImGui::PopStyleVar();
    if (!visible) {
        ImGui::End();
        return;
    }

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    desiredW_ = static_cast<int>(std::max(avail.x, 16.0f));
    desiredH_ = static_cast<int>(std::max(avail.y, 16.0f));

    if (rt_.IsValid()) {
        ImGui::Image(reinterpret_cast<ImTextureID>(rt_.SRV()), avail);
    }

    // ---- エディタカメラ操作 (ウィンドウ上で RMB ドラッグ + WASDQE) ----
    // エディタ専用のためリアルタイム dt (io.DeltaTime) で良い — sim 状態に影響しない
    if (ImGui::IsWindowHovered()) {
        const ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            camYaw_ += io.MouseDelta.x * 0.25f;
            camPitch_ = std::clamp(camPitch_ + io.MouseDelta.y * 0.25f, -89.0f, 89.0f);

            const XMVECTOR rot = XMQuaternionRotationRollPitchYaw(
                camPitch_ * (3.14159265f / 180.0f), camYaw_ * (3.14159265f / 180.0f), 0.0f);
            const XMVECTOR forward = XMVector3Rotate(XMVectorSet(0, 0, 1, 0), rot);
            const XMVECTOR right = XMVector3Rotate(XMVectorSet(1, 0, 0, 0), rot);
            XMVECTOR move = XMVectorZero();
            if (ImGui::IsKeyDown(ImGuiKey_W)) { move = XMVectorAdd(move, forward); }
            if (ImGui::IsKeyDown(ImGuiKey_S)) { move = XMVectorSubtract(move, forward); }
            if (ImGui::IsKeyDown(ImGuiKey_D)) { move = XMVectorAdd(move, right); }
            if (ImGui::IsKeyDown(ImGuiKey_A)) { move = XMVectorSubtract(move, right); }
            if (ImGui::IsKeyDown(ImGuiKey_E)) { move = XMVectorAdd(move, XMVectorSet(0, 1, 0, 0)); }
            if (ImGui::IsKeyDown(ImGuiKey_Q)) { move = XMVectorSubtract(move, XMVectorSet(0, 1, 0, 0)); }
            if (XMVectorGetX(XMVector3LengthSq(move)) > 0.0001f) {
                const float speed = ImGui::IsKeyDown(ImGuiKey_LeftShift) ? 20.0f : 6.0f;
                move = XMVectorScale(XMVector3Normalize(move), speed * io.DeltaTime);
                XMVECTOR pos = XMLoadFloat3(&camPos_);
                XMStoreFloat3(&camPos_, XMVectorAdd(pos, move));
            }
        }
    }
    ImGui::End();
}

} // namespace mye
