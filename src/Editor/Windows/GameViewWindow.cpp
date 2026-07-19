#include "Editor/Windows/GameViewWindow.h"

#include <algorithm>

#include "Engine/Engine/RenderSystem.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Renderer/RenderPath.h"

#include "imgui.h"

namespace mye {

void GameViewWindow::OnRenderViews(EngineContext& ctx)
{
    if (desiredW_ <= 0 || desiredH_ <= 0) {
        return;
    }
    rt_.Resize(*ctx.device, desiredW_, desiredH_);
    if (!rt_.IsValid()) {
        return;
    }
    FrameTarget target;
    target.rtv = rt_.RTV();
    target.dsv = rt_.DSV();
    target.width = rt_.Width();
    target.height = rt_.Height();
    hasCamera_ = ctx.renderSystem->Render(ctx.scene->GetWorld(), *ctx.device, *ctx.renderPath,
                                          *ctx.shaders, *ctx.resources, target, nullptr,
                                          ctx.particles);
}

void GameViewWindow::OnImGui(EngineContext& ctx)
{
    (void)ctx;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    const bool visible = ImGui::Begin("Game");
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
        if (!hasCamera_) {
            const ImVec2 pos = ImGui::GetWindowPos();
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(pos.x + avail.x * 0.5f - 60.0f, pos.y + avail.y * 0.5f),
                IM_COL32(255, 200, 80, 255), "No CameraComponent in scene");
        }
    }
    ImGui::End();
}

} // namespace mye
