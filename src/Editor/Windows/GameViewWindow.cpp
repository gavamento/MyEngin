#include "Editor/Windows/GameViewWindow.h"

#include <algorithm>
#include <cstdio>

#include "Engine/Core/Localization.h"
#include "Engine/Engine/RenderSystem.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Engine/UI/UIRenderer.h"
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
    target.depthSRV = rt_.DepthSRV();       // M42a
    target.dsvReadOnly = rt_.DSVReadOnly();
    target.viewKey = 3; // GameView
    hasCamera_ = ctx.renderSystem->Render(ctx.scene->GetWorld(), *ctx.device, *ctx.renderPath,
                                          *ctx.shaders, *ctx.resources, target, nullptr,
                                          ctx.particles, ctx.vfx);
    // M21: ゲーム内 UI を GameView RT に重ねる。hover はエディタでは無効 (mouse=-1)
    if (ctx.uiRenderer) {
        ctx.uiRenderer->Render(ctx.scene->GetWorld(), *ctx.device, *ctx.shaders, *ctx.resources,
                               target.rtv, target.width, target.height, -1, -1, false);
    }
}

void GameViewWindow::OnImGui(EngineContext& ctx)
{
    if (!open) {
        return;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    const bool visible = ImGui::Begin(Tr(StrId::Win_Game), &open);
    ImGui::PopStyleVar();
    if (!visible) {
        ImGui::End();
        return;
    }

    // ---- ツールバー (アスペクト比 / 統計オーバーレイ) ----
    static const char* kAspects[] = { "Free", "16:9", "4:3", "1:1" };
    ImGui::SetNextItemWidth(90);
    ImGui::Combo("##aspect", &aspectMode_, kAspects, IM_ARRAYSIZE(kAspects));
    ImGui::SameLine();
    ImGui::Checkbox(Tr(StrId::GameView_Stats), &showStats_);
    ImGui::Separator();

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    // 目標アスペクトに合わせてレターボックス (Free は avail 全体)
    float ratio = 0.0f;
    switch (aspectMode_) {
    case 1: ratio = 16.0f / 9.0f; break;
    case 2: ratio = 4.0f / 3.0f; break;
    case 3: ratio = 1.0f; break;
    default: break;
    }
    ImVec2 imgSize = avail;
    if (ratio > 0.0f && avail.x > 1 && avail.y > 1) {
        if (avail.x / avail.y > ratio) {
            imgSize = ImVec2(avail.y * ratio, avail.y); // 横に余白
        } else {
            imgSize = ImVec2(avail.x, avail.x / ratio); // 縦に余白
        }
    }
    desiredW_ = static_cast<int>(std::max(imgSize.x, 16.0f));
    desiredH_ = static_cast<int>(std::max(imgSize.y, 16.0f));

    if (rt_.IsValid()) {
        // レターボックス: 余白分だけカーソルを中央寄せ
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        const ImVec2 off((avail.x - imgSize.x) * 0.5f, (avail.y - imgSize.y) * 0.5f);
        ImGui::SetCursorScreenPos(ImVec2(cursor.x + off.x, cursor.y + off.y));
        const ImVec2 imgPos = ImGui::GetCursorScreenPos();
        ImGui::Image(reinterpret_cast<ImTextureID>(rt_.SRV()), imgSize);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (showStats_) {
            const ImGuiIO& io = ImGui::GetIO();
            char buf[128];
            snprintf(buf, sizeof(buf), "%.0f fps  %.2f ms\n%u entities  tick %llu", io.Framerate,
                     1000.0f / io.Framerate, ctx.scene->GetWorld().AliveCount(),
                     static_cast<unsigned long long>(ctx.tickIndex));
            dl->AddText(ImVec2(imgPos.x + 8, imgPos.y + 6), IM_COL32(120, 255, 140, 255), buf);
        }
        if (!hasCamera_) {
            dl->AddText(ImVec2(imgPos.x + imgSize.x * 0.5f - 60.0f, imgPos.y + imgSize.y * 0.5f),
                        IM_COL32(255, 200, 80, 255), "No CameraComponent in scene");
        }
    }
    ImGui::End();
}

} // namespace mye
