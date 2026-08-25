#include "Editor/ToastCenter.h"

#include <algorithm>
#include <cstdio>

#include "Engine/Renderer/ImGuiTheme.h" // themeColor (意味色)

#include "imgui.h"

#include "fontawesome/IconsFontAwesome6.h"

namespace mye {

namespace {

constexpr size_t kMaxToasts = 6;         // 画面に積む最大数 (超過は古いものから消す)
constexpr int kAggregateThreshold = 3;   // 1 フレームでこれを超えたら「N 件」に集約
constexpr float kFadeSeconds = 0.5f;

const char* LevelIcon(LogLevel level)
{
    switch (level) {
    case LogLevel::Warn: return ICON_FA_TRIANGLE_EXCLAMATION;
    case LogLevel::Error: return ICON_FA_CIRCLE_XMARK;
    default: return ICON_FA_CIRCLE_INFO;
    }
}

ImVec4 LevelColor(LogLevel level)
{
    switch (level) {
    case LogLevel::Warn: return themeColor::Warning;
    case LogLevel::Error: return themeColor::Error;
    default: return ImVec4(0.92f, 0.92f, 0.92f, 1.0f);
    }
}

} // namespace

void ToastCenter::Notify(LogLevel level, std::string text, float seconds)
{
    // 同一テキストが表示中ならタイマーだけ更新する (連打で積み上がらない)
    for (Toast& t : toasts_) {
        if (t.text == text) {
            t.remaining = seconds;
            t.level = level;
            return;
        }
    }
    toasts_.push_back(Toast{ level, std::move(text), seconds });
    if (toasts_.size() > kMaxToasts) {
        toasts_.erase(toasts_.begin());
    }
}

void ToastCenter::CollectLogs()
{
    LogEntry tmp[64];
    size_t n = 0;
    int warnCount = 0;
    int errorCount = 0;
    LogEntry lastImportant = {};
    while ((n = logging::ReadSince(logCursor_, tmp, 64)) > 0) {
        for (size_t i = 0; i < n; ++i) {
            if (tmp[i].level == LogLevel::Warn) {
                ++warnCount;
                lastImportant = tmp[i];
            } else if (tmp[i].level == LogLevel::Error) {
                ++errorCount;
                lastImportant = tmp[i];
            }
        }
    }
    const int total = warnCount + errorCount;
    if (total == 0) {
        return;
    }
    if (total > kAggregateThreshold) {
        char text[128];
        std::snprintf(text, sizeof(text), "警告 %d 件 / エラー %d 件 (Console 参照)", warnCount,
                      errorCount);
        Notify(errorCount > 0 ? LogLevel::Error : LogLevel::Warn, text, 5.0f);
    } else {
        // 少数ならメッセージをそのまま出す (最後の 1 件のみ。連続する場合は Notify が集約)
        Notify(lastImportant.level, lastImportant.message, 5.0f);
    }
}

void ToastCenter::OnImGui()
{
    CollectLogs();
    if (toasts_.empty()) {
        return;
    }

    const float dt = ImGui::GetIO().DeltaTime;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float y = vp->WorkPos.y + vp->WorkSize.y - 10.0f;
    const float x = vp->WorkPos.x + vp->WorkSize.x - 10.0f;

    int id = 0;
    for (Toast& t : toasts_) {
        t.remaining -= dt;
        if (t.remaining <= 0.0f) {
            continue;
        }
        const float alpha = std::min(1.0f, t.remaining / kFadeSeconds);
        ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
        ImGui::SetNextWindowBgAlpha(0.92f * alpha);
        char name[32];
        std::snprintf(name, sizeof(name), "##toast%d", id++);
        if (ImGui::Begin(name, nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize
                             | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing
                             | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking)) {
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
            ImGui::TextColored(LevelColor(t.level), "%s", LevelIcon(t.level));
            ImGui::SameLine();
            ImGui::TextUnformatted(t.text.c_str());
            ImGui::PopStyleVar();
            if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                t.remaining = 0.0f; // クリックで即時消去
            }
            t.height = ImGui::GetWindowSize().y;
        }
        ImGui::End();
        y -= t.height + 8.0f;
    }

    toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(),
                                 [](const Toast& t) { return t.remaining <= 0.0f; }),
                  toasts_.end());
}

} // namespace mye
