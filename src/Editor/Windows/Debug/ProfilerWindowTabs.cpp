#include "Editor/Windows/Debug/ProfilerWindow.h"

#include <algorithm>
#include <cstdio>
#include <iterator>

#include "Editor/App/EditorSettings.h"
#include "Editor/Scene/Selection.h"
#include "Editor/Windows/Debug/ProfilerWidgets.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Localization/Localization.h"
#include "Engine/Engine/Acoustic/AcousticField.h"
#include "Engine/Engine/Audio/Playback/AudioSourceSystem.h"
#include "Engine/Engine/Particles/ParticleSystem.h"
#include "Engine/Engine/Rendering/RenderSystem.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Renderer/Device/GpuMemoryStats.h"
#include "Engine/Renderer/Device/GpuResources.h"
#include "Engine/Renderer/ImGui/ImGuiTheme.h"
#include "Engine/Renderer/Pipeline/RenderPath.h"
#include "Engine/Renderer/Shader/ShaderManager.h"

#include "imgui.h"

// CPU / GPU / 描画 / システムの各タブ (概要と帯は ProfilerWindow.cpp)
namespace mye {

using profui::kZeroMs;
using profui::MsCell;
using profui::ShareBar;

void ProfilerWindow::DrawCpuChildren(int parent, EditorSettings& settings)
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
        BudgetPopup(settings, "cpu", shown.scopes[i].name);
        DrawCpuRowValues(i, settings);
        if (nodeOpen && hasChildren) {
            DrawCpuChildren(i, settings);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}

void ProfilerWindow::DrawCpuRowValues(int i, const EditorSettings& settings)
{
    const prof::FrameRecord& shown = Shown();
    const float budget = BudgetMs(settings, "cpu", shown.scopes[i].name);
    ImVec4 budgetColor = themeColor::Text;
    const bool flagged = profui::BudgetColor(scopeStats_[i].avg, budget, &budgetColor);
    ImGui::TableSetColumnIndex(1);
    MsCell(scopeStats_[i].now);
    ImGui::TableSetColumnIndex(2);
    if (flagged) {
        ImGui::TextColored(budgetColor, "%.3f", static_cast<double>(scopeStats_[i].avg));
    } else {
        MsCell(scopeStats_[i].avg);
    }
    ImGui::TableSetColumnIndex(3);
    MsCell(scopeStats_[i].max);
    ImGui::TableSetColumnIndex(4);
    MsCell(scopeSelfAvg_[i]);
    ImGui::TableSetColumnIndex(5);
    ImGui::Text("%d", shown.scopes[i].calls);
    ImGui::TableSetColumnIndex(6);
    profui::BudgetCell(scopeStats_[i].avg, budget);
    ImGui::TableSetColumnIndex(7);
    ShareBar(scopeStats_[i].avg, phases_.frame.avg);
}

void ProfilerWindow::DrawCpu(EngineContext& ctx, EditorSettings& settings, Selection& selection)
{
    DrawScopes(settings);
    DrawItems(ctx, settings, selection);
}

void ProfilerWindow::DrawScopes(EditorSettings& settings)
{
    const prof::FrameRecord& shown = Shown();
    if (shown.droppedScopes > 0) {
        ImGui::TextColored(themeColor::Warning, Tr(StrId::Prof_DroppedScopes), shown.droppedScopes);
    }
    if (shown.scopeCount == 0) {
        ImGui::TextColored(themeColor::TextDim, "%s", Tr(StrId::Prof_NoScopes));
        return;
    }
    if (!ImGui::BeginTable("##ProfCpu", 8, profui::kTableFlags)) {
        return;
    }
    ImGui::TableSetupColumn(Tr(StrId::Prof_ColName), ImGuiTableColumnFlags_WidthStretch, 4.0f);
    ImGui::TableSetupColumn(Tr(StrId::Prof_ColNow), ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn(Tr(StrId::Prof_ColAvg), ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn(Tr(StrId::Prof_ColMax), ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn(Tr(StrId::Prof_ColSelf), ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn(Tr(StrId::Prof_ColCalls), ImGuiTableColumnFlags_WidthStretch, 0.7f);
    ImGui::TableSetupColumn(Tr(StrId::Prof_ColBudget), ImGuiTableColumnFlags_WidthStretch, 1.0f);
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
            ImGui::PushID(rows[r]);
            ImGui::TextUnformatted(shown.scopes[rows[r]].name);
            BudgetPopup(settings, "cpu", shown.scopes[rows[r]].name);
            DrawCpuRowValues(rows[r], settings);
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
    ImGui::TextColored(themeColor::TextDim, "%s %s", Tr(StrId::Prof_SelfHelp), Tr(StrId::Prof_BudgetHint));
}

void ProfilerWindow::DrawGpu(EditorSettings& settings)
{
    const prof::FrameRecord& shown = Shown();
    ImGui::TextColored(themeColor::TextDim, "%s %s", Tr(StrId::Prof_GpuLatency), Tr(StrId::Prof_BudgetHint));
    if (!ImGui::BeginTable("##ProfGpu", 6, profui::kTableFlags)) {
        return;
    }
    ImGui::TableSetupColumn(Tr(StrId::Prof_ColName), ImGuiTableColumnFlags_WidthStretch, 3.0f);
    ImGui::TableSetupColumn(Tr(StrId::Prof_ColNow), ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn(Tr(StrId::Prof_ColAvg), ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn(Tr(StrId::Prof_ColMax), ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn(Tr(StrId::Prof_ColBudget), ImGuiTableColumnFlags_WidthStretch, 1.0f);
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
    ImGui::TableSetColumnIndex(5);
    ShareBar(phases_.gpuFrame.avg, phases_.frame.avg);
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextColored(themeColor::TextDim, "%s", Tr(StrId::Prof_GpuPassTotal));
    ImGui::TableSetColumnIndex(2);
    MsCell(passTotal);
    ImGui::TableSetColumnIndex(5);
    ShareBar(passTotal, phases_.frame.avg);

    for (int r = 0; r < rowCount; ++r) {
        const int g = rows[r];
        const bool ran = shown.gpu[g].ran;
        ImGui::TableNextRow();
        if (!ran) {
            ImGui::PushStyleColor(ImGuiCol_Text, themeColor::TextDim);
        }
        const float budget = BudgetMs(settings, "gpu", shown.gpu[g].name);
        ImGui::TableSetColumnIndex(0);
        ImGui::PushID(g);
        ImGui::TextUnformatted(shown.gpu[g].name);
        if (!ran && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", Tr(StrId::Prof_GpuNotRun));
        }
        BudgetPopup(settings, "gpu", shown.gpu[g].name);
        ImGui::PopID();
        ImGui::TableSetColumnIndex(1);
        MsCell(gpuStats_[g].now);
        ImGui::TableSetColumnIndex(2);
        MsCell(gpuStats_[g].avg);
        ImGui::TableSetColumnIndex(3);
        MsCell(gpuStats_[g].max);
        if (ran) {
            ImGui::TableSetColumnIndex(4);
            profui::BudgetCell(gpuStats_[g].avg, budget);
            ImGui::TableSetColumnIndex(5);
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

void ProfilerWindow::DrawHistory(EditorSettings& settings)
{
    if (ImGui::Checkbox(Tr(StrId::Prof_CaptureEnable), &settings.profilerCaptureEnabled)) {
        settings.Save();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
    ImGui::InputFloat("ms##ProfCaptureMs", &settings.profilerCaptureMs, 0.0f, 0.0f, "%.1f");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        settings.profilerCaptureMs = (std::max)(settings.profilerCaptureMs, 1.0f);
        settings.Save();
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr(StrId::Prof_CaptureClear))) {
        prof::ClearCaptures();
        captureIndex_ = -1;
    }
    const int count = prof::CaptureCount();
    if (count == 0) {
        ImGui::TextColored(themeColor::TextDim, "%s", Tr(StrId::Prof_CaptureEmpty));
        return;
    }
    ImGui::TextColored(themeColor::TextDim, "%s", Tr(StrId::Prof_CaptureHelp));
    if (!ImGui::BeginTable("##ProfCaptures", 4, profui::kTableFlags)) {
        return;
    }
    ImGui::TableSetupColumn(Tr(StrId::Prof_ColFrame), ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn(Tr(StrId::Prof_ColTick), ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn(Tr(StrId::Prof_ColFrameMs), ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn(Tr(StrId::Prof_ColHeaviest), ImGuiTableColumnFlags_WidthStretch, 3.0f);
    ImGui::TableHeadersRow();
    // 新しい控えを上に
    for (int index = count - 1; index >= 0; --index) {
        const prof::FrameRecord& frame = prof::Capture(index);
        // そのフレームでいちばん重い項目 = CPU スコープの自身の時間と、走った GPU パスのうちの最大
        float self[prof::kMaxFrameScopes];
        for (int i = 0; i < frame.scopeCount; ++i) {
            self[i] = frame.scopes[i].ms;
        }
        for (int i = 0; i < frame.scopeCount; ++i) {
            if (frame.scopes[i].parent >= 0) {
                self[frame.scopes[i].parent] -= frame.scopes[i].ms;
            }
        }
        const char* heaviest = "";
        float heaviestMs = 0.0f;
        for (int i = 0; i < frame.scopeCount; ++i) {
            if (self[i] > heaviestMs) {
                heaviestMs = self[i];
                heaviest = frame.scopes[i].name;
            }
        }
        for (int g = 0; g < frame.gpuCount; ++g) {
            if (frame.gpu[g].ran && frame.gpu[g].ms > heaviestMs) {
                heaviestMs = frame.gpu[g].ms;
                heaviest = frame.gpu[g].name;
            }
        }
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        char label[48];
        std::snprintf(label, sizeof(label), "%llu##cap%d", static_cast<unsigned long long>(frame.frameIndex), index);
        if (ImGui::Selectable(label, captureIndex_ == index, ImGuiSelectableFlags_SpanAllColumns)) {
            captureIndex_ = index;
            prof::SetHistoryPaused(true);
        }
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%llu", static_cast<unsigned long long>(frame.tickIndex));
        ImGui::TableSetColumnIndex(2);
        MsCell(frame.frameMs);
        ImGui::TableSetColumnIndex(3);
        if (heaviestMs > 0.0f) {
            ImGui::Text("%s (%.3f ms)", heaviest, static_cast<double>(heaviestMs));
        }
    }
    ImGui::EndTable();
}

void ProfilerWindow::DrawItems(EngineContext& ctx, EditorSettings& settings, Selection& selection)
{
    ImGui::Spacing();
    if (ImGui::Checkbox(Tr(StrId::Prof_ItemTiming), &settings.profilerItemTiming)) {
        settings.Save();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", Tr(StrId::Prof_ItemTimingHelp));
    }
    const prof::FrameRecord& shown = Shown();
    if (shown.itemNameCount == 0) {
        if (!settings.profilerItemTiming) {
            ImGui::TextColored(themeColor::TextDim, "%s", Tr(StrId::Prof_ItemsOff));
        }
        return;
    }
    const float frameMs = shown.frameMs;

    // ---- 名前別 (このフレームの合計。行数が少ないので全部出す) ----
    int nameRows[prof::kMaxFrameItemNames];
    int nameRowCount = 0;
    for (int i = 0; i < shown.itemNameCount; ++i) {
        if (PassesFilter(shown.itemNames[i].name)) {
            nameRows[nameRowCount++] = i;
        }
    }
    std::sort(nameRows, nameRows + nameRowCount,
              [&](int a, int b) { return shown.itemNames[a].ms > shown.itemNames[b].ms; });
    ImGui::SeparatorText(Tr(StrId::Prof_ItemsByName));
    if (ImGui::BeginTable("##ProfItemNames", 5, profui::kTableFlags)) {
        ImGui::TableSetupColumn(Tr(StrId::Prof_ColCategory), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(Tr(StrId::Prof_ColName), ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn(Tr(StrId::Prof_ColMs), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(Tr(StrId::Prof_ColCalls), ImGuiTableColumnFlags_WidthStretch, 0.7f);
        ImGui::TableSetupColumn(Tr(StrId::Prof_ColShare), ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableHeadersRow();
        for (int r = 0; r < nameRowCount; ++r) {
            const prof::ItemStat& item = shown.itemNames[nameRows[r]];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(themeColor::TextDim, "%s", item.category);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(item.name);
            ImGui::TableSetColumnIndex(2);
            MsCell(item.ms);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%d", item.calls);
            ImGui::TableSetColumnIndex(4);
            ShareBar(item.ms, frameMs);
        }
        ImGui::EndTable();
    }

    // ---- エンティティ別 (重い順に上位だけ) ----
    constexpr int kEntityRows = 40;
    int rows[prof::kMaxFrameItems];
    int rowCount = 0;
    for (int i = 0; i < shown.itemCount; ++i) {
        if (PassesFilter(shown.items[i].name)) {
            rows[rowCount++] = i;
        }
    }
    std::sort(rows, rows + rowCount, [&](int a, int b) { return shown.items[a].ms > shown.items[b].ms; });
    rowCount = (std::min)(rowCount, kEntityRows);

    ImGui::SeparatorText(Tr(StrId::Prof_ItemsByEntity));
    if (shown.droppedItems > 0) {
        ImGui::TextColored(themeColor::Warning, Tr(StrId::Prof_ItemsDropped), shown.droppedItems);
    }
    World& world = ctx.scene->GetWorld();
    if (ImGui::BeginTable("##ProfItemEntities", 6, profui::kTableFlags)) {
        ImGui::TableSetupColumn(Tr(StrId::Prof_ColEntity), ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn(Tr(StrId::Prof_ColCategory), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(Tr(StrId::Prof_ColName), ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn(Tr(StrId::Prof_ColMs), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(Tr(StrId::Prof_ColCalls), ImGuiTableColumnFlags_WidthStretch, 0.7f);
        ImGui::TableSetupColumn(Tr(StrId::Prof_ColShare), ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableHeadersRow();
        for (int r = 0; r < rowCount; ++r) {
            const prof::ItemStat& item = shown.items[rows[r]];
            const EntityID entity{ item.entityIndex, item.entityGeneration };
            // 記録したフレームの後で破棄されていることがある (世代が合わなければ別物)
            const bool alive = world.IsAlive(entity);
            uint64_t fileId = 0;
            const char* entityName = Tr(StrId::Prof_EntityDead);
            if (alive) {
                const FileIdComponent* fileIdComponent = world.GetComponent<FileIdComponent>(entity);
                fileId = fileIdComponent ? fileIdComponent->value : 0;
                entityName = world.GetName(entity);
                if (entityName == nullptr || entityName[0] == '\0') {
                    entityName = Tr(StrId::Prof_EntityUnnamed);
                }
            }
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(rows[r]);
            // fileId を持つエンティティだけ選択できる。ここで採番 (EnsureFileId) すると Play 中の
            // ワールドへコンポーネントを足すことになるので、持っていないものは選択させない
            ImGui::BeginDisabled(fileId == 0);
            char label[96];
            std::snprintf(label, sizeof(label), "%s  #%u", entityName, item.entityIndex);
            if (ImGui::Selectable(label, fileId != 0 && selection.Contains(fileId),
                                  ImGuiSelectableFlags_SpanAllColumns)) {
                selection.SelectOnly(fileId);
            }
            ImGui::EndDisabled();
            ImGui::PopID();
            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(themeColor::TextDim, "%s", item.category);
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(item.name);
            ImGui::TableSetColumnIndex(3);
            MsCell(item.ms);
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%d", item.calls);
            ImGui::TableSetColumnIndex(5);
            ShareBar(item.ms, frameMs);
        }
        ImGui::EndTable();
    }
    ImGui::TextColored(themeColor::TextDim, "%s", Tr(StrId::Prof_ItemsSelectHint));
    ImGui::TextColored(themeColor::TextDim, "%s", Tr(StrId::Prof_ItemsNotCovered));
}

void ProfilerWindow::DrawMemory(EngineContext& ctx, const EditorSettings& settings)
{
    const StrId tagLabels[prof::kMemTagCount] = { StrId::Prof_MemTagGeneral, StrId::Prof_MemTagAsset,
                                                  StrId::Prof_MemTagTick,    StrId::Prof_MemTagPhysics,
                                                  StrId::Prof_MemTagScript,  StrId::Prof_MemTagRender,
                                                  StrId::Prof_MemTagEditorUi };
    // 直近 window フレームの今回 / 平均 / 最大 (履歴のリングを新しい方からたどる)
    const int frames = std::clamp(settings.profilerAvgFrames, 1, (std::max)(memCount_, 1));
    struct TagRow {
        uint32_t now = 0;
        double avgAllocs = 0.0;
        uint32_t maxAllocs = 0;
        double avgBytes = 0.0;
    };
    TagRow rows[prof::kMemTagCount];
    TagRow total;
    const int used = (std::min)(frames, memCount_);
    for (int f = 0; f < used; ++f) {
        const MemFrame& frame = memHistory_[(memHead_ - 1 - f + prof::kFrameHistory * 2) % prof::kFrameHistory];
        uint32_t frameAllocs = 0;
        uint64_t frameBytes = 0;
        for (int tag = 0; tag < prof::kMemTagCount; ++tag) {
            if (f == 0) {
                rows[tag].now = frame.allocs[tag];
            }
            rows[tag].avgAllocs += frame.allocs[tag];
            rows[tag].maxAllocs = (std::max)(rows[tag].maxAllocs, frame.allocs[tag]);
            rows[tag].avgBytes += static_cast<double>(frame.bytes[tag]);
            frameAllocs += frame.allocs[tag];
            frameBytes += frame.bytes[tag];
        }
        if (f == 0) {
            total.now = frameAllocs;
        }
        total.avgAllocs += frameAllocs;
        total.maxAllocs = (std::max)(total.maxAllocs, frameAllocs);
        total.avgBytes += static_cast<double>(frameBytes);
    }
    const double inv = (used > 0) ? 1.0 / used : 0.0;

    ImGui::SeparatorText(Tr(StrId::Prof_MemPerFrame));
    if (ImGui::BeginTable("##ProfMemTags", 5, profui::kTableFlags)) {
        ImGui::TableSetupColumn(Tr(StrId::Prof_ColCategory), ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn(Tr(StrId::Prof_ColAllocNow), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(Tr(StrId::Prof_ColAllocAvg), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(Tr(StrId::Prof_ColAllocMax), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(Tr(StrId::Prof_ColBytesAvg), ImGuiTableColumnFlags_WidthStretch, 1.5f);
        ImGui::TableHeadersRow();
        auto drawRow = [&](const char* label, const TagRow& row, bool dim) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (dim) {
                ImGui::TextColored(themeColor::TextDim, "%s", label);
            } else {
                ImGui::TextUnformatted(label);
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%u", row.now);
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%.1f", row.avgAllocs * inv);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%u", row.maxAllocs);
            ImGui::TableSetColumnIndex(4);
            profui::BytesText(row.avgBytes * inv);
        };
        drawRow(Tr(StrId::Prof_MemTagTotal), total, true);
        for (int tag = 0; tag < prof::kMemTagCount; ++tag) {
            if (settings.profilerHideZero && rows[tag].maxAllocs == 0) {
                continue;
            }
            drawRow(Tr(tagLabels[tag]), rows[tag], false);
        }
        ImGui::EndTable();
    }
    ImGui::TextColored(themeColor::TextDim, "%s", Tr(StrId::Prof_MemScopeNote));

    const prof::MemStats mem = prof::GetMemoryStats();
    ImGui::SeparatorText(Tr(StrId::Prof_SecMemTotals));
    if (profui::BeginValueTable("##ProfMemTotals", Tr(StrId::Prof_ColItem), Tr(StrId::Prof_ColValue))) {
        profui::ValueRowLabel(Tr(StrId::Prof_RowMemLiveBytes));
        profui::BytesText(static_cast<double>(mem.liveBytes));
        profui::ValueRowInt(Tr(StrId::Prof_RowMemLive), static_cast<long long>(mem.liveAllocs));
        profui::ValueRowLabel(Tr(StrId::Prof_RowMemTotalBytes));
        profui::BytesText(static_cast<double>(mem.totalBytes));
        profui::ValueRowInt(Tr(StrId::Prof_RowMemAllocs), static_cast<long long>(mem.totalAllocs));
        profui::ValueRowInt(Tr(StrId::Prof_RowMemFrees), static_cast<long long>(mem.totalFrees));
        profui::EndValueTable();
    }
    DrawGpuMemory(ctx);
}

void ProfilerWindow::DrawGpuMemory(EngineContext& ctx)
{
    const StrId kindLabels[gpumem::kKindCount] = { StrId::Prof_GpuKindTexture, StrId::Prof_GpuKindRenderTarget,
                                                   StrId::Prof_GpuKindBuffer, StrId::Prof_GpuKindMesh,
                                                   StrId::Prof_GpuKindVolume };
    const gpumem::Totals totals = gpumem::GetTotals();
    uint64_t trackedBytes = 0;
    uint32_t trackedCount = 0;
    for (int kind = 0; kind < gpumem::kKindCount; ++kind) {
        trackedBytes += totals.bytes[kind];
        trackedCount += totals.count[kind];
    }

    ImGui::SeparatorText(Tr(StrId::Prof_SecGpuMemory));
    if (ImGui::BeginTable("##ProfGpuMem", 3, profui::kTableFlags)) {
        ImGui::TableSetupColumn(Tr(StrId::Prof_ColCategory), ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn(Tr(StrId::Prof_ColCount), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(Tr(StrId::Prof_ColSize), ImGuiTableColumnFlags_WidthStretch, 1.5f);
        ImGui::TableHeadersRow();
        auto row = [](const char* label, bool dim, bool showCount, uint32_t count, double bytes) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (dim) {
                ImGui::TextColored(themeColor::TextDim, "%s", label);
            } else {
                ImGui::TextUnformatted(label);
            }
            if (showCount) {
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%u", count);
            }
            ImGui::TableSetColumnIndex(2);
            profui::BytesText(bytes);
        };
        for (int kind = 0; kind < gpumem::kKindCount; ++kind) {
            row(Tr(kindLabels[kind]), false, true, totals.count[kind], static_cast<double>(totals.bytes[kind]));
        }
        row(Tr(StrId::Prof_GpuMemTracked), true, true, trackedCount, static_cast<double>(trackedBytes));
        // プロセス全体との差 = 追跡口を通っていないリソースと、ドライバの内部確保
        if (ctx.device) {
            const gpumem::VideoMemory video = gpumem::QueryVideoMemory(*ctx.device);
            if (video.valid) {
                row(Tr(StrId::Prof_GpuMemProcess), true, false, 0, static_cast<double>(video.usageBytes));
                if (video.usageBytes > trackedBytes) {
                    row(Tr(StrId::Prof_GpuMemUntracked), true, false, 0,
                        static_cast<double>(video.usageBytes - trackedBytes));
                }
                row(Tr(StrId::Prof_GpuMemBudget), true, false, 0, static_cast<double>(video.budgetBytes));
            }
        }
        ImGui::EndTable();
    }
    ImGui::TextColored(themeColor::TextDim, "%s", Tr(StrId::Prof_GpuMemNote));

    constexpr int kLargestRows = 24;
    gpumem::Entry largest[kLargestRows];
    const int largestCount = gpumem::CopyLargest(largest, kLargestRows);
    ImGui::SeparatorText(Tr(StrId::Prof_GpuMemLargest));
    if (ImGui::BeginTable("##ProfGpuMemLargest", 3, profui::kTableFlags)) {
        ImGui::TableSetupColumn(Tr(StrId::Prof_ColName), ImGuiTableColumnFlags_WidthStretch, 4.0f);
        ImGui::TableSetupColumn(Tr(StrId::Prof_ColCategory), ImGuiTableColumnFlags_WidthStretch, 1.5f);
        ImGui::TableSetupColumn(Tr(StrId::Prof_ColSize), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableHeadersRow();
        for (int i = 0; i < largestCount; ++i) {
            if (!PassesFilter(largest[i].name)) {
                continue;
            }
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(largest[i].name);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(themeColor::TextDim, "%s", Tr(kindLabels[static_cast<int>(largest[i].kind)]));
            ImGui::TableSetColumnIndex(2);
            profui::BytesText(static_cast<double>(largest[i].bytes));
        }
        ImGui::EndTable();
    }
}

void ProfilerWindow::DrawAssets(EngineContext& ctx, const EditorSettings& settings)
{
    ImGui::SeparatorText(Tr(StrId::Prof_SecAssetQueues));
    if (profui::BeginValueTable("##ProfAssetQueues", Tr(StrId::Prof_ColItem), Tr(StrId::Prof_ColValue))) {
        if (ctx.resources) {
            profui::ValueRowInt(Tr(StrId::Prof_RowAsyncTextures), ctx.resources->textures.PendingAsyncCount());
        }
        if (ctx.shaders) {
            profui::ValueRowInt(Tr(StrId::Prof_RowAsyncShaders), ctx.shaders->PendingCompileCount());
        }
        profui::ValueRowInt(Tr(StrId::Prof_RowAssetTotal), static_cast<long long>(prof::AssetLoadTotal()));
        profui::EndValueTable();
    }

    ImGui::SeparatorText(Tr(StrId::Prof_SecAssetLoads));
    const int count = prof::AssetLoadCount();
    if (count == 0) {
        ImGui::TextColored(themeColor::TextDim, "%s", Tr(StrId::Prof_AssetLoadsEmpty));
        return;
    }
    // 記録は別スレッドからも増えるので、まず写しを取ってから並べる
    static prof::AssetLoadRecord records[prof::kMaxAssetLoads]; // メインスレッド専用の作業領域 (スタックには大きい)
    int rowCount = 0;
    for (int i = 0; i < count; ++i) {
        const prof::AssetLoadRecord record = prof::AssetLoad(i);
        if (record.ms > 0.0f && (PassesFilter(record.name) || PassesFilter(record.kind))) {
            records[rowCount++] = record;
        }
    }
    if (settings.profilerSortByCost) {
        std::sort(records, records + rowCount,
                  [](const prof::AssetLoadRecord& a, const prof::AssetLoadRecord& b) { return a.ms > b.ms; });
    }
    const double now = prof::AssetClockSeconds();
    if (ImGui::BeginTable("##ProfAssetLoads", 4, profui::kTableFlags)) {
        ImGui::TableSetupColumn(Tr(StrId::Prof_ColAgo), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(Tr(StrId::Prof_ColCategory), ImGuiTableColumnFlags_WidthStretch, 1.5f);
        ImGui::TableSetupColumn(Tr(StrId::Prof_ColName), ImGuiTableColumnFlags_WidthStretch, 6.0f);
        ImGui::TableSetupColumn(Tr(StrId::Prof_ColMs), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableHeadersRow();
        for (int r = 0; r < rowCount; ++r) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%.1f", now - records[r].atSeconds);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(themeColor::TextDim, "%s", records[r].kind);
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(records[r].name);
            ImGui::TableSetColumnIndex(3);
            MsCell(records[r].ms);
        }
        ImGui::EndTable();
    }
    ImGui::TextColored(themeColor::TextDim, "%s", Tr(StrId::Prof_AssetLoadsNote));
}

} // namespace mye
