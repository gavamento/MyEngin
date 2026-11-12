#include "Editor/Windows/Debug/ProfilerWindow.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <iterator>

#include "Editor/App/EditorSettings.h"
#include "Editor/Widgets/ToastCenter.h"
#include "Editor/Windows/Debug/ProfilerWidgets.h"
#include "Engine/Core/Localization/Localization.h"
#include "Engine/Renderer/ImGui/ImGuiTheme.h"

#include "imgui.h"

namespace mye {

namespace {

constexpr float kGraphHeight = 96.0f;
constexpr float kGraphMinScaleMs = 36.0f; // 縦軸の下限 (30 fps の線が必ず入る)
constexpr float kBudget60Ms = 1000.0f / 60.0f;
constexpr float kBudget30Ms = 1000.0f / 30.0f;
constexpr int kRankingRows = 12;
// 予算超過は直近この数フレームの平均で判定する (単発の跳ねで鳴らさない)
constexpr int kBudgetCheckFrames = 30;
constexpr double kBudgetToastCooldownSec = 10.0;
constexpr int kAvgWindowChoices[] = { 1, 30, 60, 120, prof::kFrameHistory };

// タブの並び = EditorSettings::profilerTab の値
enum Tab {
    kTabOverview,
    kTabCpu,
    kTabGpu,
    kTabRender,
    kTabMemory,
    kTabAssets,
    kTabJobs,
    kTabSystems,
    kTabHistory,
    kTabCount
};

// フレームグラフの系列 (下から積む順)
enum class Series { Tick, Render, ImGui, Reload, Present, Other, Count };
constexpr int kSeriesCount = static_cast<int>(Series::Count);

// 系列の色。themeColor の意味色は「状態」を表すので系列には使わず、同じ帯 (S ≒ 0.45 / V ≒ 0.80) から
// 色相だけ変えて取る (ImGuiTheme.h の配色ルール 4 = カテゴリ色は持ち主の表に置く、と同じ流儀)。
// Other は「測れていない残り」なので無彩色にする
ImVec4 SeriesColor(Series series)
{
    switch (series) {
    case Series::Tick: return ImVec4(0.44f, 0.80f, 0.80f, 1.0f);    // 青緑
    case Series::Render: return ImVec4(0.62f, 0.44f, 0.80f, 1.0f);  // 紫
    case Series::ImGui: return ImVec4(0.80f, 0.44f, 0.68f, 1.0f);   // 桃
    case Series::Reload: return ImVec4(0.71f, 0.80f, 0.44f, 1.0f);  // 黄緑
    case Series::Present: return ImVec4(0.44f, 0.59f, 0.80f, 1.0f); // 青
    default: return themeColor::TextDim;
    }
}

StrId SeriesLabel(Series series)
{
    switch (series) {
    case Series::Tick: return StrId::Prof_PhaseTick;
    case Series::Render: return StrId::Prof_PhaseRenderCpu;
    case Series::ImGui: return StrId::Prof_PhaseImGui;
    case Series::Reload: return StrId::Prof_PhaseReload;
    case Series::Present: return StrId::Prof_PhasePresent;
    default: return StrId::Prof_PhaseOther;
    }
}

// 1 フレームの壁時計を系列へ分ける。other = フレーム全体 - 測れた和 (負にはしない)
void SplitFrame(const prof::FrameRecord& frame, float out[kSeriesCount])
{
    out[static_cast<int>(Series::Tick)] = frame.phases.tickMs;
    out[static_cast<int>(Series::Render)] = frame.phases.renderMs;
    out[static_cast<int>(Series::ImGui)] = frame.phases.imguiMs;
    out[static_cast<int>(Series::Reload)] = frame.phases.reloadMs;
    out[static_cast<int>(Series::Present)] = frame.phases.presentMs;
    const float measured = frame.phases.tickMs + frame.phases.renderMs + frame.phases.imguiMs
                           + frame.phases.reloadMs + frame.phases.presentMs;
    out[static_cast<int>(Series::Other)] = (std::max)(0.0f, frame.frameMs - measured);
}

bool SameName(const char* a, const char* b)
{
    return a == b || std::strcmp(a, b) == 0;
}

// 大文字小文字を無視した部分一致 (ASCII だけ畳む。スコープ名とパス名は ASCII の識別子)
bool ContainsNoCase(const char* text, const char* needle)
{
    const size_t needleLen = std::strlen(needle);
    if (needleLen == 0) {
        return true;
    }
    auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
    for (const char* p = text; *p != '\0'; ++p) {
        size_t k = 0;
        while (k < needleLen && p[k] != '\0' && lower(p[k]) == lower(needle[k])) {
            ++k;
        }
        if (k == needleLen) {
            return true;
        }
    }
    return false;
}

} // namespace

bool ProfilerWindow::PassesFilter(const char* name) const
{
    return ContainsNoCase(name, filter_);
}

void ProfilerWindow::BuildStats(int window)
{
    const prof::FrameRecord& shown = Shown();
    // 控えは前後のフレームを持たないので、平均も最大もそのフレームの値になる
    const int available = (std::max)(prof::HistoryCount() - selectedAge_, 1);
    const int frames = (captureIndex_ >= 0) ? 1 : std::clamp(window, 1, available);

    phases_ = {};
    for (int i = 0; i < shown.scopeCount; ++i) {
        scopeStats_[i] = {};
    }
    int gpuSamples[prof::kMaxGpuPasses] = {};
    for (int g = 0; g < shown.gpuCount; ++g) {
        gpuStats_[g] = {};
    }

    auto add = [](RowStats& stats, float value, bool first) {
        if (first) {
            stats.now = value;
        }
        stats.avg += value; // ここでは和。最後に割る
        stats.max = (std::max)(stats.max, value);
    };

    // 表示中フレームの各行が、過去フレームのどの行に当たるか (親をたどって同じ場所の同名を探す)
    static int match[prof::kMaxFrameScopes];
    for (int f = 0; f < frames; ++f) {
        const bool first = (f == 0);
        const prof::FrameRecord& frame = first ? shown : prof::History(selectedAge_ + f);
        add(phases_.frame, frame.frameMs, first);
        add(phases_.reload, frame.phases.reloadMs, first);
        add(phases_.tick, frame.phases.tickMs, first);
        add(phases_.render, frame.phases.renderMs, first);
        add(phases_.imgui, frame.phases.imguiMs, first);
        add(phases_.present, frame.phases.presentMs, first);
        add(phases_.gpuFrame, frame.gpuFrameMs, first);

        for (int i = 0; i < shown.scopeCount; ++i) {
            const prof::ScopeStat& row = shown.scopes[i];
            const int parentMatch = (row.parent >= 0) ? match[row.parent] : -1;
            int found = -1;
            if (row.parent < 0 || parentMatch >= 0) {
                for (int j = 0; j < frame.scopeCount && found < 0; ++j) {
                    if (frame.scopes[j].parent == parentMatch && SameName(frame.scopes[j].name, row.name)) {
                        found = j;
                    }
                }
            }
            match[i] = found;
            add(scopeStats_[i], (found >= 0) ? frame.scopes[found].ms : 0.0f, first);
        }

        // GPU は「走ったフレーム」だけで平均する (走らなかったフレームの値は前回値の残り)
        for (int g = 0; g < shown.gpuCount; ++g) {
            for (int j = 0; j < frame.gpuCount; ++j) {
                if (SameName(frame.gpu[j].name, shown.gpu[g].name)) {
                    if (first) {
                        gpuStats_[g].now = frame.gpu[j].ms;
                    }
                    if (frame.gpu[j].ran) {
                        gpuStats_[g].avg += frame.gpu[j].ms;
                        gpuStats_[g].max = (std::max)(gpuStats_[g].max, frame.gpu[j].ms);
                        ++gpuSamples[g];
                    }
                    break;
                }
            }
        }
    }

    const float inv = 1.0f / static_cast<float>(frames);
    for (RowStats* stats : { &phases_.frame, &phases_.reload, &phases_.tick, &phases_.render, &phases_.imgui,
                             &phases_.present, &phases_.gpuFrame }) {
        stats->avg *= inv;
    }
    for (int i = 0; i < shown.scopeCount; ++i) {
        scopeStats_[i].avg *= inv;
        scopeSelfAvg_[i] = scopeStats_[i].avg;
    }
    for (int i = 0; i < shown.scopeCount; ++i) {
        if (shown.scopes[i].parent >= 0) {
            scopeSelfAvg_[shown.scopes[i].parent] -= scopeStats_[i].avg;
        }
    }
    for (int i = 0; i < shown.scopeCount; ++i) {
        scopeSelfAvg_[i] = (std::max)(0.0f, scopeSelfAvg_[i]);
    }
    for (int g = 0; g < shown.gpuCount; ++g) {
        gpuStats_[g].avg = (gpuSamples[g] > 0) ? gpuStats_[g].avg / static_cast<float>(gpuSamples[g]) : 0.0f;
    }
}

void ProfilerWindow::OnImGui(EngineContext& ctx, EditorSettings& settings, ToastCenter& toasts,
                             Selection& selection)
{
    // 項目別の計測は窓を開いている間だけ (閉じている間まで呼び出しごとのコストを払わない)
    prof::SetItemTimingEnabled(open && settings.profilerItemTiming);
    prof::SetCaptureThresholdMs(settings.profilerCaptureEnabled ? settings.profilerCaptureMs : 0.0f);
    UpdateMemoryHistory();
    UpdateJobHistory();
    CheckBudgets(settings, toasts);
    if (!open) {
        // 見ていない間に履歴を止めたままにしない (次に開いたとき古いフレームが出る)
        if (prof::IsHistoryPaused()) {
            prof::SetHistoryPaused(false);
            selectedAge_ = 0;
            captureIndex_ = -1;
        }
        return;
    }
    if (!ImGui::Begin(Tr(StrId::Win_Profiler), &open)) {
        ImGui::End();
        return;
    }
    selectedAge_ = std::clamp(selectedAge_, 0, (std::max)(prof::HistoryCount() - 1, 0));
    if (captureIndex_ >= prof::CaptureCount()) {
        captureIndex_ = -1; // 控えを消去した
    }
    BuildStats(settings.profilerAvgFrames);
    DrawHeader(settings);

    const StrId tabLabels[kTabCount] = { StrId::Prof_TabOverview, StrId::Prof_TabCpu, StrId::Prof_TabGpu,
                                         StrId::Prof_TabRender, StrId::Prof_TabMemory, StrId::Prof_TabAssets,
                                         StrId::Prof_TabJobs, StrId::Prof_TabSystems,
                                         StrId::Prof_TabHistory };
    if (ImGui::BeginTabBar("###ProfTabs")) {
        for (int tab = 0; tab < kTabCount; ++tab) {
            // 保存してあるタブを初回だけ選び直す。以後は選ばれたタブを保存する
            const ImGuiTabItemFlags flags =
                (restoreTab_ && settings.profilerTab == tab) ? ImGuiTabItemFlags_SetSelected : 0;
            if (!ImGui::BeginTabItem(Tr(tabLabels[tab]), nullptr, flags)) {
                continue;
            }
            if (!restoreTab_ && settings.profilerTab != tab) {
                settings.profilerTab = tab;
                settings.Save();
            }
            // 上の帯を固定したまま、タブの中身だけスクロールさせる
            if (ImGui::BeginChild("##ProfTabBody")) {
                switch (tab) {
                case kTabOverview: DrawOverview(settings); break;
                case kTabCpu: DrawCpu(ctx, settings, selection); break;
                case kTabGpu: DrawGpu(settings); break;
                case kTabRender: DrawRender(ctx); break;
                case kTabMemory: DrawMemory(ctx, settings); break;
                case kTabAssets: DrawAssets(ctx, settings); break;
                case kTabJobs: DrawJobs(settings); break;
                case kTabSystems: DrawSystems(ctx); break;
                default: DrawHistory(settings); break;
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        restoreTab_ = false;
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void ProfilerWindow::DrawHeader(EditorSettings& settings)
{
    const prof::FrameRecord& shown = Shown();
    const float frameMs = phases_.frame.avg;
    const float cpuMs = phases_.reload.avg + phases_.tick.avg + phases_.render.avg + phases_.imgui.avg;
    const float otherMs = (std::max)(0.0f, frameMs - cpuMs - phases_.present.avg);
    ImGui::Text(Tr(StrId::Prof_FrameSummary), static_cast<double>(frameMs),
                static_cast<double>((frameMs > profui::kZeroMs) ? 1000.0f / frameMs : 0.0f));
    ImGui::SameLine();
    ImGui::TextColored(themeColor::TextDim, Tr(StrId::Prof_SummarySplit), static_cast<double>(cpuMs),
                       static_cast<double>(phases_.gpuFrame.avg), static_cast<double>(phases_.present.avg),
                       static_cast<double>(otherMs));

    const bool paused = prof::IsHistoryPaused();
    if (ImGui::Button(Tr(paused ? StrId::Prof_Resume : StrId::Prof_Pause))) {
        prof::SetHistoryPaused(!paused);
        selectedAge_ = 0;
        captureIndex_ = -1;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
    char preview[32];
    auto windowLabel = [&](int frames) {
        if (frames <= 1) {
            std::snprintf(preview, sizeof(preview), "%s", Tr(StrId::Prof_AvgInstant));
        } else {
            std::snprintf(preview, sizeof(preview), Tr(StrId::Prof_AvgFrames), frames);
        }
        return preview;
    };
    if (ImGui::BeginCombo(Tr(StrId::Prof_AvgWindow), windowLabel(settings.profilerAvgFrames))) {
        for (const int choice : kAvgWindowChoices) {
            if (ImGui::Selectable(windowLabel(choice), settings.profilerAvgFrames == choice)) {
                settings.profilerAvgFrames = choice;
                settings.Save();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Checkbox(Tr(StrId::Prof_SortByCost), &settings.profilerSortByCost)) {
        settings.Save();
    }
    ImGui::SameLine();
    if (ImGui::Checkbox(Tr(StrId::Prof_HideZero), &settings.profilerHideZero)) {
        settings.Save();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
    ImGui::InputTextWithHint("##ProfFilter", Tr(StrId::Prof_Filter), filter_, sizeof(filter_));
    if (paused) {
        ImGui::SameLine();
        ImGui::TextColored(themeColor::Warning,
                           Tr((captureIndex_ >= 0) ? StrId::Prof_ViewingCapture : StrId::Prof_PausedAt),
                           static_cast<unsigned long long>(shown.frameIndex),
                           static_cast<unsigned long long>(shown.tickIndex));
    }
}

void ProfilerWindow::DrawFrameGraph()
{
    const ImVec2 size(ImGui::GetContentRegionAvail().x, kGraphHeight);
    if (size.x < 8.0f) {
        return;
    }
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);
    ImGui::InvisibleButton("##ProfFrameGraph", size);
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked();

    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(p0, p1, ImGui::GetColorU32(ImGuiCol_FrameBg));

    const int count = prof::HistoryCount();
    float scaleMs = kGraphMinScaleMs;
    for (int age = 0; age < count; ++age) {
        scaleMs = (std::max)(scaleMs, prof::History(age).frameMs * 1.1f);
    }
    const float barWidth = size.x / static_cast<float>(prof::kFrameHistory);
    auto yOf = [&](float ms) { return p1.y - size.y * (std::min)(ms / scaleMs, 1.0f); };

    ImU32 colors[kSeriesCount];
    for (int s = 0; s < kSeriesCount; ++s) {
        colors[s] = ImGui::ColorConvertFloat4ToU32(SeriesColor(static_cast<Series>(s)));
    }
    // 新しいフレームが右端。棒 1 本 = 1 フレーム
    for (int age = 0; age < count; ++age) {
        float parts[kSeriesCount];
        SplitFrame(prof::History(age), parts);
        const float x1 = p1.x - barWidth * static_cast<float>(age);
        const float x0 = x1 - (std::max)(barWidth - 1.0f, 1.0f);
        float stacked = 0.0f;
        for (int s = 0; s < kSeriesCount; ++s) {
            if (parts[s] <= 0.0f) {
                continue;
            }
            draw->AddRectFilled(ImVec2(x0, yOf(stacked + parts[s])), ImVec2(x1, yOf(stacked)), colors[s]);
            stacked += parts[s];
        }
    }
    // GPU は CPU と時間が重なるので積まず、線で重ねる
    const ImU32 gpuColor = ImGui::ColorConvertFloat4ToU32(themeColor::Text);
    for (int age = 0; age + 1 < count; ++age) {
        const float xa = p1.x - barWidth * (static_cast<float>(age) + 0.5f);
        draw->AddLine(ImVec2(xa, yOf(prof::History(age).gpuFrameMs)),
                      ImVec2(xa - barWidth, yOf(prof::History(age + 1).gpuFrameMs)), gpuColor, 1.0f);
    }
    // 目安の線 (60 fps / 30 fps)
    const ImU32 guideColor = ImGui::ColorConvertFloat4ToU32(themeColor::TextDim);
    for (const float budget : { kBudget60Ms, kBudget30Ms }) {
        const float y = yOf(budget);
        draw->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), guideColor, 1.0f);
        char label[16];
        std::snprintf(label, sizeof(label), "%.1f ms", static_cast<double>(budget));
        draw->AddText(ImVec2(p0.x + 4.0f, y - ImGui::GetTextLineHeight()), guideColor, label);
    }
    // 表示中のフレーム (一時停止中だけ)
    if (prof::IsHistoryPaused() && selectedAge_ < count) {
        const float x1 = p1.x - barWidth * static_cast<float>(selectedAge_);
        draw->AddRect(ImVec2(x1 - barWidth, p0.y), ImVec2(x1, p1.y),
                      ImGui::ColorConvertFloat4ToU32(themeColor::Accent));
    }

    if (hovered && count > 0) {
        const int age = std::clamp(static_cast<int>((p1.x - ImGui::GetIO().MousePos.x) / barWidth), 0, count - 1);
        const prof::FrameRecord& frame = prof::History(age);
        float parts[kSeriesCount];
        SplitFrame(frame, parts);
        if (ImGui::BeginTooltip()) {
            ImGui::Text(Tr(StrId::Prof_GraphFrame), static_cast<unsigned long long>(frame.frameIndex),
                        static_cast<unsigned long long>(frame.tickIndex), static_cast<double>(frame.frameMs));
            for (int s = 0; s < kSeriesCount; ++s) {
                ImGui::TextColored(SeriesColor(static_cast<Series>(s)), "%s: %.3f ms",
                                   Tr(SeriesLabel(static_cast<Series>(s))), static_cast<double>(parts[s]));
            }
            ImGui::Text("%s: %.3f ms", Tr(StrId::Prof_GraphGpuLine), static_cast<double>(frame.gpuFrameMs));
            ImGui::TextColored(themeColor::TextDim, "%s", Tr(StrId::Prof_GraphClickHint));
            ImGui::EndTooltip();
        }
        if (clicked) {
            prof::SetHistoryPaused(true);
            selectedAge_ = age;
            captureIndex_ = -1;
        }
    }
}

void ProfilerWindow::DrawOverview(const EditorSettings& settings)
{
    DrawFrameGraph();

    const prof::FrameRecord& shown = Shown();
    const float frameMs = phases_.frame.avg;

    // ---- フェーズの内訳 (グラフの凡例を兼ねる) ----
    const RowStats* phaseRows[kSeriesCount] = { &phases_.tick, &phases_.render, &phases_.imgui, &phases_.reload,
                                                &phases_.present, nullptr };
    const float measuredAvg = phases_.tick.avg + phases_.render.avg + phases_.imgui.avg + phases_.reload.avg
                              + phases_.present.avg;
    if (ImGui::BeginTable("##ProfPhases", 5, profui::kTableFlags)) {
        ImGui::TableSetupColumn(Tr(StrId::Prof_KindPhase), ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn(Tr(StrId::Prof_ColNow), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(Tr(StrId::Prof_ColAvg), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(Tr(StrId::Prof_ColMax), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(Tr(StrId::Prof_ColShare), ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableHeadersRow();
        float nowParts[kSeriesCount];
        SplitFrame(shown, nowParts);
        for (int s = 0; s < kSeriesCount; ++s) {
            const Series series = static_cast<Series>(s);
            const float avg = phaseRows[s] ? phaseRows[s]->avg : (std::max)(0.0f, frameMs - measuredAvg);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(s);
            ImGui::ColorButton("##swatch", SeriesColor(series),
                               ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                               ImVec2(ImGui::GetTextLineHeight(), ImGui::GetTextLineHeight()));
            ImGui::PopID();
            ImGui::SameLine();
            ImGui::TextUnformatted(Tr(SeriesLabel(series)));
            ImGui::TableSetColumnIndex(1);
            profui::MsCell(nowParts[s]);
            ImGui::TableSetColumnIndex(2);
            profui::MsCell(avg);
            ImGui::TableSetColumnIndex(3);
            if (phaseRows[s]) {
                profui::MsCell(phaseRows[s]->max);
            }
            ImGui::TableSetColumnIndex(4);
            profui::ShareBar(avg, frameMs);
        }
        ImGui::EndTable();
    }

    // ---- 予算超過 (超過が 1 つも無ければ節ごと出さない) ----
    bool anyOver = false;
    auto overRow = [&](StrId kind, const char* kindKey, const char* name, float ms) {
        const float budget = BudgetMs(settings, kindKey, name);
        if (budget <= 0.0f || ms <= budget) {
            return;
        }
        if (!anyOver) {
            anyOver = true;
            ImGui::SeparatorText(Tr(StrId::Prof_OverBudgetHeader));
        }
        ImGui::TextColored(themeColor::Error, "%s  %s: %.3f ms / %.3f ms", Tr(kind), name,
                           static_cast<double>(ms), static_cast<double>(budget));
    };
    for (int i = 0; i < shown.scopeCount; ++i) {
        overRow(StrId::Prof_KindCpu, "cpu", shown.scopes[i].name, scopeStats_[i].avg);
    }
    for (int g = 0; g < shown.gpuCount; ++g) {
        if (shown.gpu[g].ran) {
            overRow(StrId::Prof_KindGpu, "gpu", shown.gpu[g].name, gpuStats_[g].avg);
        }
    }

    // ---- 重い項目 ----
    // CPU スコープは「自身の時間」(子に含まれないぶん) で並べる。包含時間で並べると親が必ず上に来て、
    // どこが重いかの答えにならない
    struct Entry {
        StrId kind;
        const char* name;
        float ms;
    };
    Entry entries[prof::kMaxFrameScopes + prof::kMaxGpuPasses];
    int entryCount = 0;
    for (int i = 0; i < shown.scopeCount; ++i) {
        if (PassesFilter(shown.scopes[i].name)) {
            entries[entryCount++] = Entry{ StrId::Prof_KindCpu, shown.scopes[i].name, scopeSelfAvg_[i] };
        }
    }
    for (int g = 0; g < shown.gpuCount; ++g) {
        if (shown.gpu[g].ran && PassesFilter(shown.gpu[g].name)) {
            entries[entryCount++] = Entry{ StrId::Prof_KindGpu, shown.gpu[g].name, gpuStats_[g].avg };
        }
    }
    std::sort(entries, entries + entryCount, [](const Entry& a, const Entry& b) { return a.ms > b.ms; });

    ImGui::SeparatorText(Tr(StrId::Prof_RankingHeader));
    if (ImGui::BeginTable("##ProfRanking", 4, profui::kTableFlags)) {
        ImGui::TableSetupColumn(Tr(StrId::Prof_ColKind), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(Tr(StrId::Prof_ColName), ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn(Tr(StrId::Prof_ColAvg), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(Tr(StrId::Prof_ColShare), ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableHeadersRow();
        int shownRows = 0;
        for (int i = 0; i < entryCount && shownRows < kRankingRows; ++i) {
            if (settings.profilerHideZero && entries[i].ms < profui::kZeroMs) {
                continue;
            }
            ++shownRows;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(themeColor::TextDim, "%s", Tr(entries[i].kind));
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(entries[i].name);
            ImGui::TableSetColumnIndex(2);
            profui::MsCell(entries[i].ms);
            ImGui::TableSetColumnIndex(3);
            profui::ShareBar(entries[i].ms, frameMs);
        }
        ImGui::EndTable();
    }
}

float ProfilerWindow::BudgetMs(const EditorSettings& settings, const char* kind, const char* name)
{
    char key[160];
    std::snprintf(key, sizeof(key), "%s/%s", kind, name);
    for (const EditorSettings::ProfilerBudget& budget : settings.profilerBudgets) {
        if (budget.key == key) {
            return budget.ms;
        }
    }
    return 0.0f;
}

void ProfilerWindow::BudgetPopup(EditorSettings& settings, const char* kind, const char* name)
{
    if (!ImGui::BeginPopupContextItem("##ProfBudgetPopup")) {
        return;
    }
    char key[160];
    std::snprintf(key, sizeof(key), "%s/%s", kind, name);
    float value = BudgetMs(settings, kind, name);
    ImGui::TextUnformatted(name);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
    if (ImGui::InputFloat(Tr(StrId::Prof_BudgetEdit), &value, 0.0f, 0.0f, "%.3f")) {
        value = (std::max)(value, 0.0f);
        auto& budgets = settings.profilerBudgets;
        const auto it = std::find_if(budgets.begin(), budgets.end(),
                                     [&](const EditorSettings::ProfilerBudget& b) { return b.key == key; });
        if (value <= 0.0f) {
            if (it != budgets.end()) {
                budgets.erase(it);
            }
        } else if (it != budgets.end()) {
            it->ms = value;
        } else {
            budgets.push_back(EditorSettings::ProfilerBudget{ key, value });
        }
        settings.Save();
    }
    ImGui::EndPopup();
}

void ProfilerWindow::CheckBudgets(const EditorSettings& settings, ToastCenter& toasts)
{
    if (settings.profilerBudgets.empty() || prof::IsHistoryPaused()) {
        return;
    }
    const double now = ImGui::GetTime();
    if (now - lastBudgetToast_ < kBudgetToastCooldownSec) {
        return;
    }
    if (prof::HistoryCount() < kBudgetCheckFrames) {
        return; // 起動直後の数フレームだけで判定しない
    }
    for (const EditorSettings::ProfilerBudget& budget : settings.profilerBudgets) {
        const size_t slash = budget.key.find('/');
        if (slash == std::string::npos) {
            continue;
        }
        const bool gpu = (budget.key.compare(0, slash, "gpu") == 0);
        const char* name = budget.key.c_str() + slash + 1;
        float sum = 0.0f;
        int samples = 0;
        for (int age = 0; age < kBudgetCheckFrames; ++age) {
            const prof::FrameRecord& frame = prof::History(age);
            if (gpu) {
                // 走ったフレームだけで平均する (表示と同じ)
                for (int g = 0; g < frame.gpuCount; ++g) {
                    if (frame.gpu[g].ran && SameName(frame.gpu[g].name, name)) {
                        sum += frame.gpu[g].ms;
                        ++samples;
                        break;
                    }
                }
            } else {
                // 同名が親違いで複数あっても、最初の 1 行で判定する
                for (int i = 0; i < frame.scopeCount; ++i) {
                    if (SameName(frame.scopes[i].name, name)) {
                        sum += frame.scopes[i].ms;
                        break;
                    }
                }
                ++samples;
            }
        }
        const float avg = (samples > 0) ? sum / static_cast<float>(samples) : 0.0f;
        if (avg > budget.ms) {
            char text[256];
            std::snprintf(text, sizeof(text), Tr(StrId::Prof_BudgetToast), name, static_cast<double>(avg),
                          static_cast<double>(budget.ms));
            toasts.Notify(LogLevel::Warn, text, 5.0f);
            lastBudgetToast_ = now;
            return; // 1 度に 1 件だけ (残りは次の間隔で)
        }
    }
}

void ProfilerWindow::UpdateMemoryHistory()
{
    const prof::MemStats current = prof::GetMemoryStats();
    // 一時停止中は積まない (他のタブと同じ時点で止める)。差の基準だけは進めておく
    if (memPreviousValid_ && !prof::IsHistoryPaused()) {
        MemFrame& frame = memHistory_[memHead_];
        for (int tag = 0; tag < prof::kMemTagCount; ++tag) {
            frame.allocs[tag] = static_cast<uint32_t>(current.tagAllocs[tag] - memPrevious_.tagAllocs[tag]);
            frame.bytes[tag] = current.tagBytes[tag] - memPrevious_.tagBytes[tag];
        }
        memHead_ = (memHead_ + 1) % prof::kFrameHistory;
        memCount_ = (std::min)(memCount_ + 1, prof::kFrameHistory);
    }
    memPrevious_ = current;
    memPreviousValid_ = true;
}

void ProfilerWindow::UpdateJobHistory()
{
    const jobs::JobSystem& jobSystem = jobs::System();
    const jobs::JobSystem::TimingStats timing = jobSystem.GetTimingStats();
    const jobs::JobSystem::Stats stats = jobSystem.GetStats();
    if (jobPreviousValid_ && !prof::IsHistoryPaused()) {
        const auto toMs = [](uint64_t ns) { return static_cast<float>(static_cast<double>(ns) / 1.0e6); };
        JobFrame& frame = jobHistory_[jobHead_];
        for (int i = 0; i < jobs::JobSystem::kMaxWorkers; ++i) {
            frame.ms[i] = toMs(timing.workerBusyNs[i] - jobPreviousTiming_.workerBusyNs[i]);
        }
        frame.ms[kJobRowCallerBusy] = toMs(timing.callerBusyNs - jobPreviousTiming_.callerBusyNs);
        frame.ms[kJobRowCallerWait] = toMs(timing.callerWaitNs - jobPreviousTiming_.callerWaitNs);
        frame.batches = static_cast<uint32_t>(stats.parallelBatches - jobPreviousStats_.parallelBatches);
        frame.workerChunks = static_cast<uint32_t>(stats.workerChunks - jobPreviousStats_.workerChunks);
        jobHead_ = (jobHead_ + 1) % prof::kFrameHistory;
        jobCount_ = (std::min)(jobCount_ + 1, prof::kFrameHistory);
    }
    jobPreviousTiming_ = timing;
    jobPreviousStats_ = stats;
    jobPreviousValid_ = true;
}

} // namespace mye
