#include "Editor/Windows/PerformanceCompareWindow.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "Editor/ChildProcess.h"
#include "Engine/Core/Localization.h"
#include "Engine/Platform/PathUtil.h"
#include "nlohmann/json.hpp"
#include "imgui.h"

namespace mye {

namespace {
std::string ShortSha(const std::string& sha)
{
    return sha.substr(0, (std::min)(size_t(8), sha.size()));
}

bool ValidSha(const std::string& sha)
{
    return sha.size() == 40 && sha.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
}

std::wstring TempLog(const wchar_t* name)
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return (std::filesystem::temp_directory_path() / (std::wstring(name) + std::to_wstring(stamp) + L".log")).wstring();
}
}

PerformanceCompareWindow::~PerformanceCompareWindow()
{
    CloseChildProcess(perfProcess_);
    CloseChildProcess(historyProcess_);
}

bool PerformanceCompareWindow::LoadPerformanceResult()
{
    static constexpr const char* kMetrics[] = {
        "ecs_update_10k", "transform_10k", "broadphase_10k", "xpbd_particles_10k",
        "draw_submission_1000", "rollback_snapshot", "replay_hash", "asset_database_10k"
    };
    try {
        const std::filesystem::path resultDir(perfResultDir_);
        std::ifstream targetFile(resultDir / L"target.json", std::ios::binary);
        if (!targetFile) return false;
        nlohmann::json target = nlohmann::json::parse(targetFile);
        if (target.at("schema") != 1 || !target.at("metrics").is_object()
            || target.at("metrics").size() != std::size(kMetrics)) return false;
        nlohmann::json base;
        std::ifstream baseFile(resultDir / L"base.json", std::ios::binary);
        const bool hasBase = static_cast<bool>(baseFile);
        if (hasBase) {
            base = nlohmann::json::parse(baseFile);
            if (base.at("schema") != 1 || !base.at("metrics").is_object()) return false;
        }
        const std::string currentCommit = target.at("commit").get<std::string>();
        const std::string baseCommit = hasBase ? base.at("commit").get<std::string>() : std::string();
        if (currentCommit.size() != 40 || (hasBase && baseCommit.size() != 40)) return false;
        std::vector<PerfRow> rows;
        for (const char* name : kMetrics) {
            const auto& metric = target.at("metrics").at(name);
            PerfRow row;
            row.name = name;
            row.currentMs = metric.at("median_ms").get<double>();
            row.targetMs = metric.at("target_ms").get<double>();
            row.hasBase = hasBase;
            if (hasBase) row.baseMs = base.at("metrics").at(name).at("median_ms").get<double>();
            if (!std::isfinite(row.currentMs) || !std::isfinite(row.targetMs)
                || row.currentMs < 0.0 || row.targetMs <= 0.0
                || (hasBase && (!std::isfinite(row.baseMs) || row.baseMs <= 0.0))) return false;
            rows.push_back(std::move(row));
        }
        if (currentCommit != launchedTarget_ || (hasBase && baseCommit != launchedBase_)) return false;
        perfRows_ = std::move(rows);
        perfCurrentCommit_ = currentCommit;
        perfBaseCommit_ = baseCommit;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void PerformanceCompareWindow::PollPerformanceCi()
{
    if (perfProcess_ == nullptr) return;
    std::ifstream log(perfLogPath_, std::ios::binary);
    std::string line;
    while (std::getline(log, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "PERF_STATUS=waiting") perfStatus_ = Tr(StrId::Prof_PerfWaiting);
        else if (line == "PERF_STATUS=downloading") perfStatus_ = Tr(StrId::Prof_PerfDownloading);
        else if (line.starts_with("PERF_URL=")) perfUrl_ = line.substr(9);
        else if (line.starts_with("PERF_ERROR=")) { perfStatus_ = line.substr(11); perfErrorSeen_ = true; }
        else if (line.starts_with("PERF_RESULT_DIR=")) perfResultDir_ = Utf8ToWide(line.substr(16));
    }
    uint32_t exitCode = 1;
    if (!PollChildProcess(perfProcess_, exitCode)) return;
    CloseChildProcess(perfProcess_);
    perfProcess_ = nullptr;
    if (exitCode == 0 && !perfResultDir_.empty() && LoadPerformanceResult()) {
        perfStatus_ = Tr(StrId::Prof_PerfComplete);
    } else if (exitCode == 0) {
        perfStatus_ = Tr(StrId::Prof_PerfInvalidResult);
    } else if (!perfErrorSeen_) {
        perfStatus_ = Tr(StrId::Prof_PerfFailed);
    }
}

void PerformanceCompareWindow::RequestHistory()
{
    if (historyProcess_ != nullptr) return;
    historyRequested_ = true;
    historyError_.clear();
    commits_.clear();
    headSha_.clear();
    firstParentSha_.clear();
    historyLogPath_ = TempLog(L"mye_perf_history_");
    historyProcess_ = StartChildProcess(L"pwsh -NoProfile -ExecutionPolicy Bypass -File tools\\perf_history.ps1 -Count 500",
                                        repoRoot_, historyLogPath_, "[perf-ci]", " history");
    if (historyProcess_ == nullptr) historyError_ = Tr(StrId::Perf_HistoryError);
}

void PerformanceCompareWindow::BuildLanes()
{
    std::vector<std::string> active;
    for (CommitRow& row : commits_) {
        auto it = std::find(active.begin(), active.end(), row.sha);
        if (it == active.end()) {
            active.push_back(row.sha);
            it = active.end() - 1;
        }
        row.lanesBefore = active;
        row.lane = static_cast<int>(it - active.begin());
        if (row.parents.empty()) active.erase(it);
        else {
            *it = row.parents.front();
            active.insert(active.begin() + row.lane + 1, row.parents.begin() + 1, row.parents.end());
        }
        for (size_t i = 0; i < active.size(); ++i) {
            if (std::find(active.begin(), active.begin() + i, active[i]) != active.begin() + i) {
                active.erase(active.begin() + i);
                --i;
            }
        }
        row.lanesAfter = active;
    }
}

void PerformanceCompareWindow::PollHistory()
{
    if (historyProcess_ == nullptr) return;
    uint32_t code = 1;
    if (!PollChildProcess(historyProcess_, code)) return;
    CloseChildProcess(historyProcess_);
    historyProcess_ = nullptr;
    std::ifstream log(historyLogPath_, std::ios::binary);
    std::string line;
    while (std::getline(log, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.starts_with("HEAD=")) headSha_ = line.substr(5);
        else if (line.starts_with("PARENT=")) firstParentSha_ = line.substr(7);
        else if (line.starts_with("ERROR=")) historyError_ = line.substr(6);
        else if (line.starts_with("COMMIT=")) {
            std::string value = line.substr(7);
            const size_t a = value.find('\x1f');
            const size_t b = a == std::string::npos ? a : value.find('\x1f', a + 1);
            const size_t c = b == std::string::npos ? b : value.find('\x1f', b + 1);
            if (c == std::string::npos) continue;
            CommitRow row;
            row.sha = value.substr(0, a);
            if (!ValidSha(row.sha)) continue;
            std::istringstream parents(value.substr(a + 1, b - a - 1));
            std::string parent;
            while (parents >> parent) { if (ValidSha(parent)) row.parents.push_back(parent); }
            row.date = value.substr(b + 1, c - b - 1);
            row.subject = value.substr(c + 1);
            commits_.push_back(std::move(row));
        }
    }
    if (code != 0 || !ValidSha(headSha_)) historyError_ = Tr(StrId::Perf_HistoryError);
    else BuildLanes();
}

void PerformanceCompareWindow::DrawGitline(float height)
{
    if (ImGui::BeginChild("##PerfGitline", ImVec2(0, height), ImGuiChildFlags_Borders)) {
        const ImU32 colors[] = { IM_COL32(95, 185, 245, 255), IM_COL32(250, 170, 85, 255),
                                 IM_COL32(145, 210, 125, 255), IM_COL32(210, 140, 215, 255) };
        size_t graphSlots = 1;
        for (const CommitRow& row : commits_) {
            graphSlots = (std::max)({ graphSlots, row.lanesBefore.size(), row.lanesAfter.size() });
        }
        const float step = 14.0f;
        const float rowHeight = ImGui::GetFrameHeight();
        const float halfSpan = (rowHeight + ImGui::GetStyle().ItemSpacing.y) * 0.5f;
        for (const CommitRow& row : commits_) {
            ImGui::PushID(row.sha.c_str());
            const float y = ImGui::GetCursorScreenPos().y + rowHeight * 0.5f;
            const float x = ImGui::GetCursorScreenPos().x + 12.0f;
            ImDrawList* draw = ImGui::GetWindowDrawList();
            for (size_t i = 0; i < row.lanesBefore.size(); ++i) {
                draw->AddLine(ImVec2(x + step * i, y - halfSpan), ImVec2(x + step * i, y),
                              colors[i % 4], 2.0f);
            }
            for (size_t i = 0; i < row.lanesAfter.size(); ++i) {
                auto it = std::find(row.lanesBefore.begin(), row.lanesBefore.end(), row.lanesAfter[i]);
                const int from = it == row.lanesBefore.end() ? row.lane
                    : static_cast<int>(it - row.lanesBefore.begin());
                draw->AddLine(ImVec2(x + step * from, y), ImVec2(x + step * i, y + halfSpan),
                              colors[i % 4], 2.0f);
            }
            for (const std::string& parent : row.parents) {
                auto it = std::find(row.lanesAfter.begin(), row.lanesAfter.end(), parent);
                if (it == row.lanesAfter.end()) continue;
                const size_t parentLane = static_cast<size_t>(it - row.lanesAfter.begin());
                draw->AddLine(ImVec2(x + step * row.lane, y),
                              ImVec2(x + step * parentLane, y + halfSpan),
                              colors[parentLane % 4], 2.0f);
            }
            draw->AddCircleFilled(ImVec2(x + step * row.lane, y), 4.0f, colors[row.lane % 4]);
            ImGui::Dummy(ImVec2(static_cast<float>(graphSlots + 1) * step, rowHeight));
            ImGui::SameLine();
            std::string label = ShortSha(row.sha) + "  " + row.date.substr(0, 10) + "  " + row.subject;
            if (ImGui::Selectable(label.c_str(), selectedSha_ == row.sha, 0, ImVec2(0, rowHeight))) {
                selectedSha_ = row.sha;
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

void PerformanceCompareWindow::RunComparison()
{
    launchedTarget_ = headSha_;
    launchedBase_ = selectedSha_.empty() ? firstParentSha_ : selectedSha_;
    perfRows_.clear();
    perfErrorSeen_ = false;
    perfUrl_.clear();
    perfStatus_ = Tr(StrId::Prof_PerfRunning);
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto folder = std::filesystem::temp_directory_path() / (L"mye_perf_ci_" + std::to_wstring(stamp));
    perfLogPath_ = folder.wstring() + L".log";
    perfResultDir_ = (folder / L"result").wstring();
    const std::wstring command = L"pwsh -NoProfile -ExecutionPolicy Bypass -File tools\\perf_dispatch.ps1 -TargetSha "
        + Utf8ToWide(launchedTarget_) + L" -BaseSha " + Utf8ToWide(launchedBase_)
        + L" -ResultDir \"" + perfResultDir_ + L"\"";
    perfProcess_ = StartChildProcess(command, repoRoot_, perfLogPath_, "[perf-ci]", " dispatch");
    if (perfProcess_ == nullptr) perfStatus_ = Tr(StrId::Prof_PerfFailed);
}

void PerformanceCompareWindow::OnImGui()
{
    PollHistory();
    PollPerformanceCi();
    if (!open) return;
    if (!ImGui::Begin(Tr(StrId::Win_PerformanceCompare), &open)) { ImGui::End(); return; }
    if (repoRoot_.empty()) repoRoot_ = FindEngineRepoRoot();
    if (repoRoot_.empty()) {
        ImGui::TextDisabled("%s", Tr(StrId::Prof_PerfNoRepo));
        ImGui::End();
        return;
    }
    if (!historyRequested_) RequestHistory();
    ImGui::BeginDisabled(historyProcess_ != nullptr);
    if (ImGui::Button(Tr(StrId::Perf_Refresh))) RequestHistory();
    ImGui::EndDisabled();
    if (historyProcess_ != nullptr) ImGui::TextDisabled("%s", Tr(StrId::Scm_Loading));
    if (!historyError_.empty()) ImGui::TextWrapped("%s", historyError_.c_str());
    const float availableHeight = ImGui::GetContentRegionAvail().y;
    const float maxHistoryHeight = (std::max)(120.0f, availableHeight - 120.0f);
    const float historyHeight = std::clamp(availableHeight * historyRatio_, 120.0f, maxHistoryHeight);
    DrawGitline(historyHeight);
    ImGui::InvisibleButton("##PerfGitlineSplitter", ImVec2(-1.0f, 8.0f));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    if (ImGui::IsItemActive() && availableHeight > 0.0f) {
        historyRatio_ = std::clamp(historyRatio_ + ImGui::GetIO().MouseDelta.y / availableHeight,
                                   0.15f, 0.85f);
    }
    const ImVec2 splitterMin = ImGui::GetItemRectMin();
    const ImVec2 splitterMax = ImGui::GetItemRectMax();
    const float splitterY = (splitterMin.y + splitterMax.y) * 0.5f;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(splitterMin.x, splitterY),
                                        ImVec2(splitterMax.x, splitterY),
                                        ImGui::GetColorU32(ImGuiCol_Separator), 2.0f);
    ImGui::Text("%s: %s", Tr(StrId::Prof_PerfTargetCommit), headSha_.c_str());
    const std::string base = selectedSha_.empty() ? firstParentSha_ : selectedSha_;
    ImGui::Text("%s: %s", Tr(StrId::Prof_PerfBaseCommit), base.c_str());
    if (selectedSha_.empty()) ImGui::TextDisabled("%s", Tr(StrId::Perf_FirstParent));
    if (!selectedSha_.empty() && ImGui::Button(Tr(StrId::Perf_UseFirstParent))) selectedSha_.clear();
    ImGui::BeginDisabled(perfProcess_ != nullptr || !ValidSha(headSha_) || !ValidSha(base));
    if (ImGui::Button(Tr(StrId::Prof_PerfRun))) RunComparison();
    ImGui::EndDisabled();
    if (!launchedTarget_.empty()) {
        ImGui::Text("%s: %s", Tr(StrId::Perf_LaunchedTarget), launchedTarget_.c_str());
        ImGui::Text("%s: %s", Tr(StrId::Perf_LaunchedBase), launchedBase_.c_str());
    }
    if (!perfStatus_.empty()) ImGui::TextWrapped("%s", perfStatus_.c_str());
    if (!perfUrl_.empty()) {
        ImGui::TextWrapped("%s", perfUrl_.c_str());
        if (ImGui::SmallButton(Tr(StrId::Prof_PerfCopyLink))) ImGui::SetClipboardText(perfUrl_.c_str());
    }
    if (!perfRows_.empty() && ImGui::BeginTable("##PerfCiResults", 5,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn(Tr(StrId::Prof_PerfMetric));
        ImGui::TableSetupColumn(Tr(StrId::Prof_PerfCurrent));
        ImGui::TableSetupColumn(Tr(StrId::Prof_PerfBaseline));
        ImGui::TableSetupColumn(Tr(StrId::Prof_PerfChange));
        ImGui::TableSetupColumn(Tr(StrId::Prof_PerfGoal));
        ImGui::TableHeadersRow();
        for (const PerfRow& row : perfRows_) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(row.name.c_str());
            ImGui::TableSetColumnIndex(1); ImGui::Text("%.4f", row.currentMs);
            ImGui::TableSetColumnIndex(2);
            if (row.hasBase) ImGui::Text("%.4f", row.baseMs); else ImGui::TextUnformatted("--");
            ImGui::TableSetColumnIndex(3);
            if (row.hasBase) ImGui::Text("%+.1f%%", 100.0 * (row.currentMs / row.baseMs - 1.0));
            else ImGui::TextUnformatted("--");
            ImGui::TableSetColumnIndex(4); ImGui::Text("%.2fx", row.currentMs / row.targetMs);
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

} // namespace mye
