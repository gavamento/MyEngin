#include "Editor/Windows/ProfilerWindow.h"

#include "Engine/Core/Localization.h"
#include "Engine/Core/Profiler.h"
#include "Engine/Engine/Acoustic/AcousticField.h" // M65d: 残光の統計行
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
    if (!ImGui::Begin(Tr(StrId::Win_Profiler), &open)) {
        ImGui::End();
        return;
    }
    const FrameTimings& t = ctx.timings;

    ImGui::Text(Tr(StrId::Prof_Frame), t.frameMs,
                (t.frameMs > 0.001f) ? 1000.0f / t.frameMs : 0.0f);
    ImGui::PlotLines("##frame", frameHistory_, kHistory, cursor_, nullptr, 0.0f, 33.3f,
                     ImVec2(-1, 60));

    ImGui::Separator();
    ImGui::TextUnformatted(Tr(StrId::Prof_PhaseHeader));
    ImGui::Text(Tr(StrId::Prof_PhaseHotReload), t.reloadMs);
    ImGui::Text(Tr(StrId::Prof_PhaseTicks), t.tickMs, t.ticksThisFrame);
    ImGui::Text(Tr(StrId::Prof_PhaseRender), t.renderMs);
    ImGui::Text(Tr(StrId::Prof_PhaseUi), t.presentMs);

    ImGui::Separator();
    ImGui::TextUnformatted(Tr(StrId::Prof_Particles));
    const ParticleStats cpu = ctx.particles->Cpu().Stats();
    const ParticleStats gpu = ctx.particles->Gpu().Stats();
    ImGui::Text("  CPU: %7u alive %7.3f ms", cpu.aliveTotal, cpu.updateMs);
    ImGui::Text("  GPU: %7u alive %7.3f ms (GpuTimer)", gpu.aliveTotal, gpu.updateMs);
    // M44d: ポストプロセス解決の GPU 時間 (複数ビューでは最後に完了した Resolve)
    if (ctx.renderSystem) {
        ImGui::Text("  postfx: %6.3f ms (GpuTimer)", ctx.renderSystem->PostFxGpuMs());
        // M54d: 影 (csm = 平行光 3 カスケード / atlas = 局所ライトのタイル)。
        // 点光源 1 本 = 6 タイルなので、tiles と draws がアトラスの重さの実体。
        // culled はタイル毎の視錐台カリングで省いた draw と、シーン AABB に触れない面の数
        ImGui::Text("  shadow: %6.3f ms csm / %6.3f ms atlas (GpuTimer, %d tiles, %d draws, "
                    "culled %d draws / %d faces)",
                    ctx.renderSystem->ShadowCsmGpuMs(), ctx.renderSystem->ShadowAtlasGpuMs(),
                    ctx.renderSystem->ShadowAtlasTiles(), ctx.renderSystem->ShadowAtlasDraws(),
                    ctx.renderSystem->ShadowAtlasCulledDraws(),
                    ctx.renderSystem->ShadowAtlasCulledFaces());
        // M56c: HZB (min-Z ピラミッド)。組んでいないフレームは GpuTimer に前の値が
        // 残るので、**組む条件が立っていないときは行ごと出さない** (RT と同じ流儀)。
        // 出ている間の値は「全段の縮小ディスパッチ 1 フレーム分」の合計
        if (ctx.renderSystem->hzbDebugMip != 0) {
            ImGui::Text("  hzb: %6.3f ms (GpuTimer, mip %d)", ctx.renderSystem->HzbGpuMs(),
                        ctx.renderSystem->hzbDebugMip - 1);
        }
        // M56d: SSR。HZB とセットで出す — SSR の総コストは「ピラミッド構築 + トレース」の
        // 和で、片方だけ見ても支配項が分からない (可視化が off でも HZB は組まれている)
        if (ctx.renderSystem->enableSsr) {
            ImGui::Text("  ssr: %6.3f ms trace + %6.3f ms hzb (GpuTimer)",
                        ctx.renderSystem->SsrGpuMs(), ctx.renderSystem->HzbGpuMs());
        }
        // M57: フロクセル (--froxel。off のときは行ごと出さない)。
        // cells が 0 = ボリューム未確保 = まだ 1 度も注入していない (正射影のビュー等)。
        // hist=0 が続くなら履歴が毎フレーム捨てられている (通番が飛んでいる)
        if (ctx.renderSystem->enableFroxel) {
            ImGui::Text("  froxel: %6.3f inject / %6.3f temporal / %6.3f integrate ms "
                        "(GpuTimer, %d cells, density %.3f, g %.2f, jitter %.3f, hist %d)",
                        ctx.renderSystem->FroxelInjectGpuMs(),
                        ctx.renderSystem->FroxelTemporalGpuMs(),
                        ctx.renderSystem->FroxelIntegrateGpuMs(),
                        ctx.renderSystem->FroxelCellCount(),
                        ctx.renderSystem->froxelSettings.density,
                        ctx.renderSystem->froxelSettings.anisotropy,
                        ctx.renderSystem->FroxelSliceJitter(),
                        ctx.renderSystem->FroxelHistoryValid() ? 1 : 0);
        }
        // M65d: 音響の残光ボリューム (シーンに AcousticVolume が無ければ行ごと出さない)。
        // ★upload ms が 0.000 でも「速い」とは限らない — 内容の通番が前フレームと同じで
        //   転送を**省いた**フレームも 0 になる。supplied=0 が続くなら
        //   「一度も音が鳴っていない」か「テクスチャを作れなかった」のどちらか
        if (ctx.renderSystem->acousticField != nullptr
            && ctx.renderSystem->AcousticCellCount() > 0) {
            int acousticWaves = 0;
            for (const AcousticField::Wave& w : ctx.renderSystem->acousticField->Waves()) {
                acousticWaves += (w.active != 0) ? 1 : 0;
            }
            ImGui::Text("  acoustic: %6.3f ms upload (CPU, %d cells, waves %d, supplied %d)",
                        ctx.renderSystem->AcousticUploadMs(),
                        ctx.renderSystem->AcousticCellCount(), acousticWaves,
                        ctx.renderSystem->AcousticSupplied() ? 1 : 0);
        }
        // M46b: レイトレ (デバッグ表示も GI 合成も off のときは行ごと出さない)
        const bool rtGiOn = ctx.renderSystem->enableRtGi;             // M46f
        const bool rtShadowOn = ctx.renderSystem->enableRtShadow;     // M46g
        const bool rtReflOn = ctx.renderSystem->enableRtRefl;         // M46h
        if (ctx.renderSystem->rtDebugMode != 0 || rtGiOn || rtShadowOn || rtReflOn) {
            ImGui::Text("  rt bvh: %5d inst / %7d tri / %6.3f ms (CPU)",
                        ctx.renderSystem->RtInstanceCount(),
                        ctx.renderSystem->RtTriangleCount(), ctx.renderSystem->RtBuildCpuMs());
            if (ctx.renderSystem->rtDebugMode != 0) {
                ImGui::Text("  rt debug: %6.3f ms (GpuTimer)", ctx.renderSystem->RtDebugGpuMs());
            }
            if (ctx.renderSystem->rtDebugMode >= 4 || rtGiOn) {
                ImGui::Text("  rt gi: %6.3f ms (GpuTimer, %.0f%% res, %d bounce)",
                            ctx.renderSystem->RtGiGpuMs(),
                            ctx.renderSystem->rtResolutionScale * 100.0f,
                            ctx.renderSystem->rtBounces);
                // M46d: テンポラル蓄積 (off のときは前回値のまま = 参考値)
                ImGui::Text("  rt temporal: %6.3f ms (GpuTimer, %s)",
                            ctx.renderSystem->RtTemporalGpuMs(),
                            ctx.renderSystem->rtTemporal ? "on" : "off");
                // M46e: 分散推定 + A-Trous 全反復の合計 (蓄積 off では動かない)
                ImGui::Text("  rt svgf: %6.3f ms (GpuTimer, %s)", ctx.renderSystem->RtSvgfGpuMs(),
                            (ctx.renderSystem->rtSvgf && ctx.renderSystem->rtTemporal) ? "on"
                                                                                       : "off");
            }
            // M46g: 影レイと分離型空間フィルタ (どちらもフル解像度)
            if (ctx.renderSystem->rtDebugMode == 9 || rtShadowOn) {
                ImGui::Text("  rt shadow: %6.3f ms trace / %6.3f ms filter (GpuTimer, full res)",
                            ctx.renderSystem->RtShadowGpuMs(),
                            ctx.renderSystem->RtShadowFilterGpuMs());
            }
            // M46h: 反射レイと、そのデノイズ (蓄積 + 分散推定 + A-Trous の合計)
            if (ctx.renderSystem->rtDebugMode == 10 || ctx.renderSystem->rtDebugMode == 11
                || rtReflOn) {
                ImGui::Text("  rt refl: %6.3f ms trace / %6.3f ms denoise (GpuTimer, %.0f%% res)",
                            ctx.renderSystem->RtReflGpuMs(),
                            ctx.renderSystem->RtReflDenoiseGpuMs(),
                            ctx.renderSystem->rtResolutionScale * 100.0f);
            }
        }
    }

    ImGui::Separator();
    ImGui::TextUnformatted(Tr(StrId::Prof_ScopesHeader));
    for (const prof::ScopeRecord& s : prof::FrameScopes()) {
        ImGui::Text("%*s%-14s %6.3f ms", s.depth * 2, "", s.name, s.ms);
    }

    ImGui::Separator();
    const prof::RenderStats rs = prof::GetRenderStats();
    ImGui::Text(Tr(StrId::Prof_Draw), rs.drawCalls, rs.triangles, rs.culled);

    const prof::MemStats mem = prof::GetMemoryStats();
    ImGui::Text(Tr(StrId::Prof_Memory),
                static_cast<unsigned long long>(mem.liveAllocs),
                static_cast<double>(mem.totalBytes) / (1024.0 * 1024.0),
                static_cast<unsigned long long>(mem.totalAllocs),
                static_cast<unsigned long long>(mem.totalFrees));

    ImGui::Separator();
    ImGui::Text(Tr(StrId::Prof_RenderPath), ctx.renderPath ? ctx.renderPath->Name() : "?");
    ImGui::Text(Tr(StrId::Prof_Entities), ctx.scene->GetWorld().AliveCount());
    ImGui::End();
}

} // namespace mye
