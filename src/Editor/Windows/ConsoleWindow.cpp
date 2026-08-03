#include "Editor/Windows/ConsoleWindow.h"

#include <cctype>
#include <cstring>
#include <string>

#include <Windows.h>
#include <shellapi.h>

#include "Engine/Core/Localization.h"
#include "imgui.h"

namespace mye {
namespace {

// 大文字小文字を無視した部分一致
bool ContainsCI(const char* hay, const std::string& needleLower)
{
    if (needleLower.empty()) {
        return true;
    }
    std::string h = hay;
    for (char& c : h) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return h.find(needleLower) != std::string::npos;
}

} // namespace

void ConsoleWindow::JumpToSource(const LogEntry& e, const std::string& cmd)
{
    if (e.file[0] == '\0' || e.line <= 0 || cmd.empty()) {
        return;
    }
    std::string full = cmd;
    auto replaceAll = [](std::string& s, const std::string& from, const std::string& to) {
        for (size_t pos = s.find(from); pos != std::string::npos; pos = s.find(from, pos + to.size())) {
            s.replace(pos, from.size(), to);
        }
    };
    replaceAll(full, "{file}", e.file);
    replaceAll(full, "{line}", std::to_string(e.line));
    // cmd.exe /c 経由で実行 (PATH 上の code 等を解決)。パスは ASCII 前提
    const std::wstring wargs = L"/c " + std::wstring(full.begin(), full.end());
    ShellExecuteW(nullptr, L"open", L"cmd.exe", wargs.c_str(), nullptr, SW_HIDE);
}

void ConsoleWindow::OnImGui(const std::string& externalEditorCmd)
{
    // 新着ログを取り込む (リングバッファからの増分読み出し)
    LogEntry buf[256];
    size_t n;
    while ((n = logging::ReadSince(cursor_, buf, 256)) > 0) {
        entries_.insert(entries_.end(), buf, buf + n);
    }
    constexpr size_t kMaxLocal = 8192;
    if (entries_.size() > kMaxLocal) {
        entries_.erase(entries_.begin(),
                       entries_.begin() + static_cast<ptrdiff_t>(entries_.size() - kMaxLocal));
    }

    if (!open) {
        return;
    }
    if (!ImGui::Begin(Tr(StrId::Win_Console), &open)) {
        ImGui::End();
        return;
    }

    int warnCount = 0, errCount = 0;
    for (const LogEntry& e : entries_) {
        if (e.level == LogLevel::Warn) {
            ++warnCount;
        } else if (e.level == LogLevel::Error) {
            ++errCount;
        }
    }

    if (ImGui::Button("Clear")) {
        entries_.clear();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Trace", &showTrace_);
    ImGui::SameLine();
    ImGui::Checkbox("Info", &showInfo_);
    ImGui::SameLine();
    ImGui::Checkbox("Warn", &showWarn_);
    ImGui::SameLine();
    ImGui::Checkbox("Error", &showError_);
    ImGui::SameLine();
    ImGui::Checkbox("Collapse", &collapse_);
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &autoScroll_);
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.3f, 1.0f), "%d", warnCount);
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.35f, 1.0f), "%d", errCount);

    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##search", "Filter... (double-click a line to open source)", searchBuf_,
                             sizeof(searchBuf_));
    ImGui::Separator();

    std::string needle = searchBuf_;
    for (char& c : needle) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    if (ImGui::BeginChild("##log", ImVec2(0, 0), ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        for (size_t i = 0; i < entries_.size();) {
            const LogEntry& e = entries_[i];
            bool levelOn = false;
            ImVec4 color(1, 1, 1, 1);
            switch (e.level) {
            case LogLevel::Trace: levelOn = showTrace_; color = { 0.6f, 0.6f, 0.6f, 1.0f }; break;
            case LogLevel::Info:  levelOn = showInfo_;  color = { 0.85f, 0.85f, 0.85f, 1.0f }; break;
            case LogLevel::Warn:  levelOn = showWarn_;  color = { 0.95f, 0.85f, 0.3f, 1.0f }; break;
            case LogLevel::Error: levelOn = showError_; color = { 0.95f, 0.4f, 0.35f, 1.0f }; break;
            }
            if (!levelOn || !ContainsCI(e.message, needle)) {
                ++i;
                continue;
            }

            // Collapse: 連続する同一メッセージをまとめる
            size_t dup = 1;
            if (collapse_) {
                while (i + dup < entries_.size() && entries_[i + dup].level == e.level
                       && std::strcmp(entries_[i + dup].message, e.message) == 0) {
                    ++dup;
                }
            }

            ImGui::PushID(static_cast<int>(i));
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.6f, 1.0f), "[%llu]",
                               static_cast<unsigned long long>(e.frame));
            ImGui::SameLine();
            char label[300];
            if (dup > 1) {
                snprintf(label, sizeof(label), "(%zu) %s", dup, e.message);
            } else {
                snprintf(label, sizeof(label), "%s", e.message);
            }
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            ImGui::Selectable(label);
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                JumpToSource(e, externalEditorCmd);
            }
            if (e.file[0] != '\0' && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s:%d", e.file, e.line);
            }
            ImGui::PopID();
            i += dup;
        }
        if (autoScroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f) {
            ImGui::SetScrollHereY(1.0f);
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

} // namespace mye
