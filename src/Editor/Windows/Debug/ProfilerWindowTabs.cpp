#include "Editor/Windows/Debug/ProfilerWindow.h"

#include <algorithm>
#include <iterator>

#include "Editor/App/EditorSettings.h"
#include "Editor/Windows/Debug/ProfilerWidgets.h"
#include "Engine/Core/Localization/Localization.h"
#include "Engine/Engine/Acoustic/AcousticField.h"
#include "Engine/Engine/Audio/Playback/AudioSourceSystem.h"
#include "Engine/Engine/Particles/ParticleSystem.h"
#include "Engine/Engine/Rendering/RenderSystem.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Renderer/ImGui/ImGuiTheme.h"
#include "Engine/Renderer/Pipeline/RenderPath.h"

#include "imgui.h"

// CPU / GPU / 描画 / システムの各タブ (概要と帯は ProfilerWindow.cpp)
namespace mye {

using profui::kZeroMs;
using profui::MsCell;
using profui::ShareBar;

void ProfilerWindow::DrawCpuChildren(int parent, const EditorSettings& settings)
{
    const prof::FrameRecord& shown = Shown();
    int children[prof::kMaxFrameScopes];
    int childCount = 0;
    for (int i = 0; i < shown.scopeCount; ++i) {
        if (shown.scopes[i].parent != parent) {
            continue;
        }
        if (settings.profilerHideZero && scopeStats_[i].avg < kZeroMs && scopeStats_[i].now < kZeroMs) {
            continue;
        }
        children[childCount++] = i;
    }
    if (settings.profilerSortByCost) {
        std::sort(children, children + childCount,
                  [&](int a, int b) { return scopeStats_[a].avg > scopeStats_[b].avg; });
    }
    for (int c = 0; c < childCount; ++c) {
        const int i = children[c];
        bool hasChildren = false;
        for (int j = i + 1; j < shown.scopeCount && !hasChildren; ++j) {
            hasChildren = (shown.scopes[j].parent == i);
        }
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::PushID(i);
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;
        if (!hasChildren) {
            flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        }
        const bool nodeOpen = ImGui::TreeNodeEx("##scope", flags, "%s", shown.scopes[i].name);
        DrawCpuRowValues(i);
        if (nodeOpen && hasChildren) {
            DrawCpuChildren(i, settings);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}

void ProfilerWindow::DrawCpuRowValues(int i)
{
    const prof::FrameRecord& shown = Shown();
    ImGui::TableSetColumnIndex(1);
    MsCell(scopeStats_[i].now);
    ImGui::TableSetColumnIndex(2);
    MsCell(scopeStats_[i].avg);
    ImGui::TableSetColumnIndex(3);
    MsCell(scopeStats_[i].max);
    ImGui::TableSetColumnIndex(4);
    MsCell(scopeSelfAvg_[i]);
    ImGui::TableSetColumnIndex(5);
    ImGui::Text("%d", shown.scopes[i].calls);
    ImGui::TableSetColumnIndex(6);
    ShareBar(scopeStats_[i].avg, phases_.frame.avg);
}

void ProfilerWindow::DrawCpu(const EditorSettings& settings)
{
    const prof::FrameRecord& shown = Shown();
    if (shown.droppedScopes > 0) {
        ImGui::TextColored(themeColor::Warning, Tr(StrId::Prof_DroppedScopes), shown.droppedScopes);
    }
    if (shown.scopeCount == 0) {
        ImGui::TextColored(themeColor::TextDim, "%s", Tr(StrId::Prof_NoScopes));
        return;
    }
    if (!ImGui::BeginTable("##ProfCpu", 7, profui::kTableFlags)) {
        return;
    }
    ImGui::TableSetupColumn(Tr(StrId::Prof_ColName), ImGuiTableColumnFlags_WidthStretch, 4.0f);
    ImGui::TableSetupColumn(Tr(StrId::Prof_ColNow), ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn(Tr(StrId::Prof_ColAvg), ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn(Tr(StrId::Prof_ColMax), ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn(Tr(StrId::Prof_ColSelf), ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn(Tr(StrId::Prof_ColCalls), ImGuiTableColumnFlags_WidthStretch, 0.7f);
    ImGui::TableSetupColumn(Tr(StrId::Prof_ColShare), ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableHeadersRow();
    if (filter_[0] == '\0') {
        DrawCpuChildren(-1, settings);
    } else {
        // 絞り込み中は木をたたんで、一致した行だけを平らに並べる (親が一致しなくても子を出すため)
        int rows[prof::kMaxFrameScopes];
        int rowCount = 0;
        for (int i = 0; i < shown.scopeCount; ++i) {
            if (PassesFilter(shown.scopes[i].name)) {
                rows[rowCount++] = i;
            }
        }
        if (settings.profilerSortByCost) {
            std::sort(rows, rows + rowCount, [&](int a, int b) { return scopeStats_[a].avg > scopeStats_[b].avg; });
        }
        for (int r = 0; r < rowCount; ++r) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(shown.scopes[rows[r]].name);
            DrawCpuRowValues(rows[r]);
        }
    }
    ImGui::EndTable();
    ImGui::TextColored(themeColor::TextDim, "%s", Tr(StrId::Prof_SelfHelp));
}

void ProfilerWindow::DrawGpu(const EditorSettings& settings)
{
    const prof::FrameRecord& shown = Shown();
    ImGui::TextColored(themeColor::TextDim, "%s", Tr(StrId::Prof_GpuLatency));
    if (!ImGui::BeginTable("##ProfGpu", 5, profui::kTableFlags)) {
        return;
    }
    ImGui::TableSetupColumn(Tr(StrId::Prof_ColName), ImGuiTableColumnFlags_WidthStretch, 3.0f);
    ImGui::TableSetupColumn(Tr(StrId::Prof_ColNow), ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn(Tr(StrId::Prof_ColAvg), ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn(Tr(StrId::Prof_ColMax), ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn(Tr(StrId::Prof_ColShare), ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableHeadersRow();

    int rows[prof::kMaxGpuPasses];
    int rowCount = 0;
    float passTotal = 0.0f;
    for (int g = 0; g < shown.gpuCount; ++g) {
        if (shown.gpu[g].ran) {
            passTotal += gpuStats_[g].avg;
        }
        if (!PassesFilter(shown.gpu[g].name)) {
            continue;
        }
        if (settings.profilerHideZero && (!shown.gpu[g].ran || gpuStats_[g].avg < kZeroMs)) {
            continue;
        }
        rows[rowCount++] = g;
    }
    if (settings.profilerSortByCost) {
        // 走っていないパスは下へ寄せる
        std::sort(rows, rows + rowCount, [&](int a, int b) {
            if (shown.gpu[a].ran != shown.gpu[b].ran) {
                return shown.gpu[a].ran;
            }
            return gpuStats_[a].avg > gpuStats_[b].avg;
        });
    }

    // 先頭 2 行は要約: Render 1 回ぶん全体と、申告されたパスの和
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextColored(themeColor::TextDim, "%s", Tr(StrId::Prof_GpuFrame));
    ImGui::TableSetColumnIndex(1);
    MsCell(phases_.gpuFrame.now);
    ImGui::TableSetColumnIndex(2);
    MsCell(phases_.gpuFrame.avg);
    ImGui::TableSetColumnIndex(3);
    MsCell(phases_.gpuFrame.max);
    ImGui::TableSetColumnIndex(4);
    ShareBar(phases_.gpuFrame.avg, phases_.frame.avg);
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextColored(themeColor::TextDim, "%s", Tr(StrId::Prof_GpuPassTotal));
    ImGui::TableSetColumnIndex(2);
    MsCell(passTotal);
    ImGui::TableSetColumnIndex(4);
    ShareBar(passTotal, phases_.frame.avg);

    for (int r = 0; r < rowCount; ++r) {
        const int g = rows[r];
        const bool ran = shown.gpu[g].ran;
        ImGui::TableNextRow();
        if (!ran) {
            ImGui::PushStyleColor(ImGuiCol_Text, themeColor::TextDim);
        }
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(shown.gpu[g].name);
        if (!ran && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", Tr(StrId::Prof_GpuNotRun));
        }
        ImGui::TableSetColumnIndex(1);
        MsCell(gpuStats_[g].now);
        ImGui::TableSetColumnIndex(2);
        MsCell(gpuStats_[g].avg);
        ImGui::TableSetColumnIndex(3);
        MsCell(gpuStats_[g].max);
        if (ran) {
            ImGui::TableSetColumnIndex(4);
            ShareBar(gpuStats_[g].avg, phases_.frame.avg);
        } else {
            ImGui::PopStyleColor();
        }
    }
    ImGui::EndTable();
}

void ProfilerWindow::DrawRender(EngineContext& ctx)
{
    const prof::FrameRecord& shown = Shown();
    const StrId viewLabels[] = { StrId::Prof_ViewAll, StrId::Prof_ViewRuntime, StrId::Prof_ViewScene,
                                 StrId::Prof_ViewGame };
    // コンボの 0 番 = 全ビュー、1.. = viewKey (RenderSystem の viewKey 規約: 1 runtime / 2 SceneView / 3 GameView)
    const int viewChoices = (std::min)(static_cast<int>(std::size(viewLabels)), prof::kRenderStatsViewSlots);
    renderView_ = std::clamp(renderView_, 0, viewChoices - 1);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    if (ImGui::BeginCombo(Tr(StrId::Prof_View), Tr(viewLabels[renderView_]))) {
        for (int v = 0; v < viewChoices; ++v) {
            if (ImGui::Selectable(Tr(viewLabels[v]), renderView_ == v)) {
                renderView_ = v;
            }
        }
        ImGui::EndCombo();
    }
    const prof::RenderStats& rs = (renderView_ == 0) ? shown.render : shown.renderByView[renderView_];
    const char* itemHeader = Tr(StrId::Prof_ColItem);
    const char* valueHeader = Tr(StrId::Prof_ColValue);

    ImGui::SeparatorText(Tr(StrId::Prof_SecDraw));
    if (profui::BeginValueTable("##ProfDraw", itemHeader, valueHeader)) {
        profui::ValueRowInt(Tr(StrId::Prof_RowDrawCalls), rs.drawCalls);
        profui::ValueRowInt(Tr(StrId::Prof_RowTriangles), rs.triangles);
        profui::EndValueTable();
    }

    ImGui::SeparatorText(Tr(StrId::Prof_SecCull));
    if (profui::BeginValueTable("##ProfCull", itemHeader, valueHeader)) {
        profui::ValueRowInt(Tr(StrId::Prof_RowCulledFrustum), rs.culled);
        profui::ValueRowInt(Tr(StrId::Prof_RowCulledDistance), rs.culledDistance);
        profui::ValueRowInt(Tr(StrId::Prof_RowCulledScreenSize), rs.culledScreenSize);
        profui::ValueRowInt(Tr(StrId::Prof_RowCulledNonMesh), rs.culledNonMesh);
        profui::ValueRowInt(Tr(StrId::Prof_RowUnculledEmitters), rs.unculledGpuEmitters);
        if (ctx.renderSystem && ctx.renderSystem->enableOcclusionCulling) {
            profui::ValueRowInt(Tr(StrId::Prof_RowOccluded), rs.occluded);
            profui::ValueRowInt(Tr(StrId::Prof_RowOcclusionPhase1), rs.occlusionPhase1Draws);
            profui::ValueRowInt(Tr(StrId::Prof_RowOcclusionPhase2), rs.occlusionPhase2Draws);
        }
        profui::EndValueTable();
    }

    ImGui::SeparatorText(Tr(StrId::Prof_SecShadow));
    if (profui::BeginValueTable("##ProfShadow", itemHeader, valueHeader)) {
        profui::ValueRowInt(Tr(StrId::Prof_RowShadowDrawCalls), rs.shadowDrawCalls);
        profui::ValueRowInt(Tr(StrId::Prof_RowShadowTriangles), rs.shadowTriangles);
        profui::ValueRowLabel(Tr(StrId::Prof_RowCascadeDraws));
        ImGui::Text("%d / %d / %d", rs.shadowCascadeDraws[0], rs.shadowCascadeDraws[1], rs.shadowCascadeDraws[2]);
        profui::EndValueTable();
    }

    ImGui::SeparatorText(Tr(StrId::Prof_SecLod));
    if (ImGui::BeginTable("##ProfLod", 3, profui::kTableFlags)) {
        ImGui::TableSetupColumn(Tr(StrId::Prof_ColLod));
        ImGui::TableSetupColumn(Tr(StrId::Prof_ColObjects));
        ImGui::TableSetupColumn(Tr(StrId::Prof_RowTriangles));
        ImGui::TableHeadersRow();
        for (int lod = 0; lod < prof::kRenderStatsLodSlots; ++lod) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("L%d", lod);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%d", rs.lodDraws[lod]);
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%d", rs.lodTriangles[lod]);
        }
        ImGui::EndTable();
    }
}

void ProfilerWindow::DrawSystems(EngineContext& ctx)
{
    // このタブは履歴ではなく各システムの現在値を読む (一時停止中のフレームとは時点が違う)
    ImGui::TextColored(themeColor::TextDim, "%s", Tr(StrId::Prof_SysLive));
    const char* itemHeader = Tr(StrId::Prof_ColItem);
    const char* valueHeader = Tr(StrId::Prof_ColValue);
    auto yesNo = [](bool value) { return Tr(value ? StrId::Prof_Yes : StrId::Prof_No); };

    ImGui::SeparatorText(Tr(StrId::Prof_SecGeneral));
    if (profui::BeginValueTable("##ProfGeneral", itemHeader, valueHeader)) {
        profui::ValueRowText(Tr(StrId::Prof_RowRenderPath), ctx.renderPath ? ctx.renderPath->Name() : "?");
        profui::ValueRowInt(Tr(StrId::Prof_RowEntities), ctx.scene->GetWorld().AliveCount());
        profui::EndValueTable();
    }

    ImGui::SeparatorText(Tr(StrId::Prof_SecParticles));
    if (profui::BeginValueTable("##ProfParticles", itemHeader, valueHeader)) {
        const ParticleStats cpu = ctx.particles->Cpu().Stats();
        const ParticleStats gpu = ctx.particles->Gpu().Stats();
        profui::ValueRowInt(Tr(StrId::Prof_RowCpuAlive), cpu.aliveTotal);
        profui::ValueRowFloat(Tr(StrId::Prof_RowCpuUpdateMs), static_cast<double>(cpu.updateMs));
        profui::ValueRowInt(Tr(StrId::Prof_RowGpuAlive), gpu.aliveTotal);
        profui::EndValueTable();
    }

    const prof::MemStats mem = prof::GetMemoryStats();
    ImGui::SeparatorText(Tr(StrId::Prof_SecMemory));
    if (profui::BeginValueTable("##ProfMemory", itemHeader, valueHeader)) {
        profui::ValueRowInt(Tr(StrId::Prof_RowMemLive), static_cast<long long>(mem.liveAllocs));
        profui::ValueRowFloat(Tr(StrId::Prof_RowMemTotalMb), static_cast<double>(mem.totalBytes) / (1024.0 * 1024.0),
                              1);
        profui::ValueRowInt(Tr(StrId::Prof_RowMemAllocs), static_cast<long long>(mem.totalAllocs));
        profui::ValueRowInt(Tr(StrId::Prof_RowMemFrees), static_cast<long long>(mem.totalFrees));
        profui::EndValueTable();
    }

    if (!ctx.renderSystem) {
        return;
    }
    const RenderSystem& rs = *ctx.renderSystem;

    // 局所ライトの影アトラス。点光源 1 本 = 6 タイルなので、tiles と draws がアトラスの重さの実体
    ImGui::SeparatorText(Tr(StrId::Prof_SecShadow));
    if (profui::BeginValueTable("##ProfAtlas", itemHeader, valueHeader)) {
        profui::ValueRowInt(Tr(StrId::Prof_RowAtlasTiles), rs.ShadowAtlasTiles());
        profui::ValueRowInt(Tr(StrId::Prof_RowAtlasDraws), rs.ShadowAtlasDraws());
        profui::ValueRowInt(Tr(StrId::Prof_RowAtlasCulledDraws), rs.ShadowAtlasCulledDraws());
        profui::ValueRowInt(Tr(StrId::Prof_RowAtlasCulledFaces), rs.ShadowAtlasCulledFaces());
        profui::EndValueTable();
    }

    // フロクセル。cells が 0 = まだ 1 度も注入していない。履歴が無効のままなら毎フレーム捨てられている
    if (rs.enableFroxel) {
        ImGui::SeparatorText(Tr(StrId::Prof_SecFroxel));
        if (profui::BeginValueTable("##ProfFroxel", itemHeader, valueHeader)) {
            profui::ValueRowInt(Tr(StrId::Prof_RowFroxelCells), rs.FroxelCellCount());
            profui::ValueRowFloat(Tr(StrId::Prof_RowFroxelDensity), static_cast<double>(rs.froxelSettings.density));
            profui::ValueRowFloat(Tr(StrId::Prof_RowFroxelAnisotropy),
                                  static_cast<double>(rs.froxelSettings.anisotropy), 2);
            profui::ValueRowFloat(Tr(StrId::Prof_RowFroxelJitter), static_cast<double>(rs.FroxelSliceJitter()));
            profui::ValueRowText(Tr(StrId::Prof_RowFroxelHistory), yesNo(rs.FroxelHistoryValid()));
            profui::EndValueTable();
        }
    }

    // 音響の残光ボリューム (シーンに AcousticVolume が無ければ出さない)。
    // 供給が「いいえ」のままなら、一度も音が鳴っていないか、テクスチャを作れなかった
    if (rs.acousticField != nullptr && rs.AcousticCellCount() > 0) {
        int activeWaves = 0;
        for (const AcousticField::Wave& wave : rs.acousticField->Waves()) {
            activeWaves += (wave.active != 0) ? 1 : 0;
        }
        ImGui::SeparatorText(Tr(StrId::Prof_SecAcoustic));
        if (profui::BeginValueTable("##ProfAcoustic", itemHeader, valueHeader)) {
            profui::ValueRowFloat(Tr(StrId::Prof_RowAcousticUploadMs), static_cast<double>(rs.AcousticUploadMs()));
            profui::ValueRowInt(Tr(StrId::Prof_RowAcousticCells), rs.AcousticCellCount());
            profui::ValueRowInt(Tr(StrId::Prof_RowAcousticWaves), activeWaves);
            profui::ValueRowText(Tr(StrId::Prof_RowAcousticSupplied), yesNo(rs.AcousticSupplied()));
            profui::EndValueTable();
        }
    }

    // 音響 × オーディオ。--no-audio では Update が丸ごと戻るので active が立たず、節ごと出ない
    if (ctx.audioSources != nullptr && ctx.audioSources->AcousticStats().active) {
        const AcousticAudioStats& stats = ctx.audioSources->AcousticStats();
        ImGui::SeparatorText(Tr(StrId::Prof_SecAcousticAudio));
        if (profui::BeginValueTable("##ProfAcousticAudio", itemHeader, valueHeader)) {
            profui::ValueRowFloat(Tr(StrId::Prof_RowAaProbeMs), static_cast<double>(stats.probeMsLast));
            profui::ValueRowInt(Tr(StrId::Prof_RowAaRebuilds), stats.rebuilds);
            profui::ValueRowInt(Tr(StrId::Prof_RowAaBoxCells), stats.boxCells);
            profui::ValueRowInt(Tr(StrId::Prof_RowAaShaped), stats.shaped);
            profui::ValueRowFloat(Tr(StrId::Prof_RowAaOpenness), static_cast<double>(stats.openness), 2);
            profui::ValueRowInt(Tr(StrId::Prof_RowAaShots), stats.shots);
            profui::ValueRowFloat(Tr(StrId::Prof_RowAaRoomT), static_cast<double>(stats.roomT), 2);
            profui::EndValueTable();
        }
    }

    // レイトレ (デバッグ表示も GI・影・反射の合成も off のときは BVH を組まないので出さない)
    if (rs.rtDebugMode != rtdebug::kOff || rs.enableRtGi || rs.enableRtShadow || rs.enableRtRefl) {
        ImGui::SeparatorText(Tr(StrId::Prof_SecRt));
        if (profui::BeginValueTable("##ProfRt", itemHeader, valueHeader)) {
            profui::ValueRowInt(Tr(StrId::Prof_RowRtInstances), rs.RtInstanceCount());
            profui::ValueRowInt(Tr(StrId::Prof_RowRtTriangles), rs.RtTriangleCount());
            profui::ValueRowFloat(Tr(StrId::Prof_RowRtBuildMs), static_cast<double>(rs.RtBuildCpuMs()));
            profui::ValueRowFloat(Tr(StrId::Prof_RowRtResolution), static_cast<double>(rs.rtResolutionScale) * 100.0,
                                  0);
            profui::ValueRowInt(Tr(StrId::Prof_RowRtBounces), rs.rtBounces);
            profui::EndValueTable();
        }
    }
}

} // namespace mye
