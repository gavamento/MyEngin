#include "Editor/Windows/ParticleSettingsWindow.h"

#include "Engine/Core/Localization.h"
#include "Engine/Engine/Particles/ParticleSystem.h"

#include "imgui.h"

namespace mye {

void ParticleSettingsWindow::OnImGui(EngineContext& ctx)
{
    if (!open) {
        return;
    }
    if (!ImGui::Begin(Tr(StrId::Win_ParticleSettings), &open)) {
        ImGui::End();
        return;
    }
    ParticleSystem& ps = *ctx.particles;

    // ---- バックエンド選択 (spec 7.4: ラジオボタン、プロジェクト設定に保存) ----
    ImGui::TextUnformatted(Tr(StrId::Particle_Backend));
    int kind = static_cast<int>(ps.ActiveKind());
    bool changed = false;
    changed |= ImGui::RadioButton(Tr(StrId::Particle_Cpu), &kind, 0);
    ImGui::SameLine();
    changed |= ImGui::RadioButton(Tr(StrId::Particle_Gpu), &kind, 1);
    if (changed) {
        ps.SetActiveKind(static_cast<ParticleBackendKind>(kind));
    }

    bool compare = ps.CompareMode();
    if (ImGui::Checkbox(Tr(StrId::Particle_Compare), &compare)) {
        ps.SetCompareMode(compare);
    }

    bool simd = ps.Cpu().SimdEnabled();
    if (ImGui::Checkbox(Tr(StrId::Particle_Simd), &simd)) {
        ps.Cpu().SetSimdEnabled(simd);
        ps.SaveSettings();
    }

    ImGui::Separator();

    // ---- 計測表示 ----
    if (ps.CompareMode()) {
        const ParticleStats cpu = ps.Cpu().Stats();
        const ParticleStats gpu = ps.Gpu().Stats();
        ImGui::Text("CPU: %7u alive  %6.3f ms", cpu.aliveTotal, cpu.updateMs);
        ImGui::Text("GPU: %7u alive  %6.3f ms", gpu.aliveTotal, gpu.updateMs);
        if (cpu.updateMs > 0.0001f && gpu.updateMs > 0.0001f) {
            ImGui::Text(Tr(StrId::Particle_Speedup), cpu.updateMs / gpu.updateMs);
        }
        ImGui::TextDisabled(Tr(StrId::Particle_GpuOffset), ps.CompareOffsetX());
    } else {
        const ParticleStats s = ps.Active().Stats();
        ImGui::Text("%s", ps.Active().Name());
        ImGui::Text(Tr(StrId::Particle_Alive), s.aliveTotal);
        ImGui::Text(Tr(StrId::Particle_Update), s.updateMs);
    }
    ImGui::TextDisabled("%s", Tr(StrId::Particle_EditHint));
    ImGui::End();
}

} // namespace mye
