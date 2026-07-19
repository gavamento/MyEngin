#include "Editor/Windows/ConsoleWindow.h"

#include "imgui.h"

namespace mye {

void ConsoleWindow::OnImGui()
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

    if (!ImGui::Begin("Console")) {
        ImGui::End();
        return;
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
    ImGui::Checkbox("Auto-scroll", &autoScroll_);
    ImGui::Separator();

    if (ImGui::BeginChild("##log", ImVec2(0, 0), ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        for (const LogEntry& e : entries_) {
            bool show = false;
            ImVec4 color(1, 1, 1, 1);
            switch (e.level) {
            case LogLevel::Trace: show = showTrace_; color = { 0.6f, 0.6f, 0.6f, 1.0f }; break;
            case LogLevel::Info:  show = showInfo_;  color = { 0.85f, 0.85f, 0.85f, 1.0f }; break;
            case LogLevel::Warn:  show = showWarn_;  color = { 0.95f, 0.85f, 0.3f, 1.0f }; break;
            case LogLevel::Error: show = showError_; color = { 0.95f, 0.4f, 0.35f, 1.0f }; break;
            }
            if (!show) {
                continue;
            }
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.6f, 1.0f), "[%llu]",
                               static_cast<unsigned long long>(e.frame));
            ImGui::SameLine();
            ImGui::TextColored(color, "%s", e.message);
        }
        if (autoScroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f) {
            ImGui::SetScrollHereY(1.0f);
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

} // namespace mye
