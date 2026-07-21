#include "Editor/Windows/ParticleSettingsWindow.h"

#include "Engine/Engine/Particles/ParticleSystem.h"

#include "imgui.h"

namespace mye {

void ParticleSettingsWindow::OnImGui(EngineContext& ctx)
{
    if (!open) {
        return;
    }
    if (!ImGui::Begin("Particle Settings", &open)) {
        ImGui::End();
        return;
    }
    ParticleSystem& ps = *ctx.particles;

    // ---- バックエンド選択 (spec 7.4: ラジオボタン、プロジェクト設定に保存) ----
    ImGui::TextUnformatted("Backend");
    int kind = static_cast<int>(ps.ActiveKind());
    bool changed = false;
    changed |= ImGui::RadioButton("CPU (SIMD)", &kind, 0);
    ImGui::SameLine();
    changed |= ImGui::RadioButton("GPU (Compute)", &kind, 1);
    if (changed) {
        ps.SetActiveKind(static_cast<ParticleBackendKind>(kind));
    }

    bool compare = ps.CompareMode();
    if (ImGui::Checkbox("Compare mode (side by side)", &compare)) {
        ps.SetCompareMode(compare);
    }

    bool simd = ps.Cpu().SimdEnabled();
    if (ImGui::Checkbox("CPU SIMD (SSE)", &simd)) {
        ps.Cpu().SetSimdEnabled(simd);
        ps.SaveSettings();
    }

    ImGui::Separator();

    // ---- 計測表示 ----
    if (ps.CompareMode()) {
        const ParticleStats cpu = ps.Cpu().Stats();
        const ParticleStats gpu = ps.Gpu().Stats();
        ImGui::Text("CPU: %7u alive  %6.3f ms", cpu.aliveTotal, cpu.updateMs);
        ImGui::Text("GPU: (cap %6u)  %6.3f ms", gpu.aliveTotal, gpu.updateMs);
        if (cpu.updateMs > 0.0001f && gpu.updateMs > 0.0001f) {
            ImGui::Text("speedup: x%.1f", cpu.updateMs / gpu.updateMs);
        }
        ImGui::TextDisabled("GPU cloud is offset +%.1f on X", ps.CompareOffsetX());
    } else {
        const ParticleStats s = ps.Active().Stats();
        ImGui::Text("%s", ps.Active().Name());
        ImGui::Text("alive: %u", s.aliveTotal);
        ImGui::Text("update: %.3f ms", s.updateMs);
    }
    ImGui::TextDisabled("(emitter properties are edited in the Inspector)");
    ImGui::End();
}

} // namespace mye
