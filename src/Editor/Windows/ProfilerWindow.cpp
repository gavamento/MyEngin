#include "Editor/Windows/ProfilerWindow.h"

#include "Engine/Core/Profiler.h"
#include "Engine/Engine/Particles/ParticleSystem.h"
#include "Engine/Engine/RenderSystem.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Renderer/RenderPath.h"

#include "imgui.h"

namespace mye {

void ProfilerWindow::OnImGui(EngineContext& ctx)
{
    frameHistory_[cursor_] = ctx.timings.frameMs;
    cursor_ = (cursor_ + 1) % kHistory;

    if (!open) {
        return;
    }
    if (!ImGui::Begin("Profiler", &open)) {
        ImGui::End();
        return;
    }
    const FrameTimings& t = ctx.timings;

    ImGui::Text("frame: %6.2f ms (%.0f fps)", t.frameMs,
                (t.frameMs > 0.001f) ? 1000.0f / t.frameMs : 0.0f);
    ImGui::PlotLines("##frame", frameHistory_, kHistory, cursor_, nullptr, 0.0f, 33.3f,
                     ImVec2(-1, 60));

    ImGui::Separator();
    ImGui::TextUnformatted("phase breakdown (CPU):");
    ImGui::Text("  2 hot reload : %6.3f ms", t.reloadMs);
    ImGui::Text("  3-5,7 ticks  : %6.3f ms (%d tick)", t.tickMs, t.ticksThisFrame);
    ImGui::Text("  6 render     : %6.3f ms", t.renderMs);
    ImGui::Text("  8 ui/present : %6.3f ms", t.presentMs);

    ImGui::Separator();
    ImGui::TextUnformatted("particles:");
    const ParticleStats cpu = ctx.particles->Cpu().Stats();
    const ParticleStats gpu = ctx.particles->Gpu().Stats();
    ImGui::Text("  CPU: %7u alive %7.3f ms", cpu.aliveTotal, cpu.updateMs);
    ImGui::Text("  GPU: (cap %6u) %7.3f ms (GpuTimer)", gpu.aliveTotal, gpu.updateMs);
    // M44d: ポストプロセス解決の GPU 時間 (複数ビューでは最後に完了した Resolve)
    if (ctx.renderSystem) {
        ImGui::Text("  postfx: %6.3f ms (GpuTimer)", ctx.renderSystem->PostFxGpuMs());
        // M46b: レイトレ (rtDebugMode が off のときは 0 のまま)
        if (ctx.renderSystem->rtDebugMode != 0) {
            ImGui::Text("  rt bvh: %5d inst / %7d tri / %6.3f ms (CPU)",
                        ctx.renderSystem->RtInstanceCount(),
                        ctx.renderSystem->RtTriangleCount(), ctx.renderSystem->RtBuildCpuMs());
            ImGui::Text("  rt debug: %6.3f ms (GpuTimer)", ctx.renderSystem->RtDebugGpuMs());
            if (ctx.renderSystem->rtDebugMode == 4) {
                ImGui::Text("  rt gi: %6.3f ms (GpuTimer, %.0f%% res, %d bounce)",
                            ctx.renderSystem->RtGiGpuMs(),
                            ctx.renderSystem->rtResolutionScale * 100.0f,
                            ctx.renderSystem->rtBounces);
            }
        }
    }

    ImGui::Separator();
    ImGui::TextUnformatted("CPU scopes (this frame):");
    for (const prof::ScopeRecord& s : prof::FrameScopes()) {
        ImGui::Text("%*s%-14s %6.3f ms", s.depth * 2, "", s.name, s.ms);
    }

    ImGui::Separator();
    const prof::RenderStats rs = prof::GetRenderStats();
    ImGui::Text("render: %d draw calls, %d tris, %d culled", rs.drawCalls, rs.triangles, rs.culled);

    const prof::MemStats mem = prof::GetMemoryStats();
    ImGui::Text("memory: %llu live allocs, %.1f MB total (%llu allocs / %llu frees)",
                static_cast<unsigned long long>(mem.liveAllocs),
                static_cast<double>(mem.totalBytes) / (1024.0 * 1024.0),
                static_cast<unsigned long long>(mem.totalAllocs),
                static_cast<unsigned long long>(mem.totalFrees));

    ImGui::Separator();
    ImGui::Text("render path: %s", ctx.renderPath ? ctx.renderPath->Name() : "?");
    ImGui::Text("entities: %u", ctx.scene->GetWorld().AliveCount());
    ImGui::End();
}

} // namespace mye
