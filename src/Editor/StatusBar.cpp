#include "Editor/StatusBar.h"

#include <cstdio>
#include <filesystem>

#include "Engine/Core/Format.h"
#include "Engine/Core/Localization.h"
#include "Engine/Engine/EngineLoop.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/ImGuiTheme.h"

#include "imgui.h"
#include "imgui_internal.h" // BeginViewportSideBar

#include "fontawesome/IconsFontAwesome6.h"

namespace mye {

namespace {

ImVec4 LevelColor(LogLevel level)
{
    switch (level) {
    case LogLevel::Warn: return themeColor::Warning;
    case LogLevel::Error: return themeColor::Error;
    default: return ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
    }
}

const char* LevelIcon(LogLevel level)
{
    switch (level) {
    case LogLevel::Warn: return ICON_FA_TRIANGLE_EXCLAMATION;
    case LogLevel::Error: return ICON_FA_CIRCLE_XMARK;
    default: return ICON_FA_CIRCLE_INFO;
    }
}

} // namespace

void StatusBar::OnImGui(EngineContext& ctx, const std::string& projectName,
                        const std::wstring& scenePath, bool dirty, PlayState state,
                        bool* consoleOpen)
{
    // 最新ログを増分読みして 1 行だけ保持する
    LogEntry tmp[64];
    size_t n = 0;
    while ((n = logging::ReadSince(logCursor_, tmp, 64)) > 0) {
        last_ = tmp[n - 1];
        hasLast_ = true;
    }

    if (ImGui::BeginViewportSideBar("##mye_statusbar", ImGui::GetMainViewport(), ImGuiDir_Down,
                                    ImGui::GetFrameHeight(),
                                    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar
                                        | ImGuiWindowFlags_NoDecoration
                                        | ImGuiWindowFlags_NoDocking)) {
        // ---- 右側の情報 (先に幅を計算して右寄せ) ----
        // アイコンとラベルは実行時連結 (ラベルが Tr 経由でコンパイル時に決まらない)
        char stateBuf[64];
        std::snprintf(stateBuf, sizeof(stateBuf), "%s%s",
                      (state == PlayState::Playing)  ? ICON_FA_PLAY " "
                      : (state == PlayState::Paused) ? ICON_FA_PAUSE " "
                                                     : "",
                      Tr((state == PlayState::Playing)  ? StrId::Status_Playing
                         : (state == PlayState::Paused) ? StrId::Status_Paused
                                                        : StrId::Status_Editing));
        const char* stateText = stateBuf;
        const std::string sceneName =
            WideToUtf8(std::filesystem::path(scenePath).filename().wstring())
            + (dirty ? "*" : "");
        // M47b: 旧実装は char[256] 固定で、日本語のプロジェクト名/シーン名だと溢れていた
        const float fps = (ctx.timings.frameMs > 0.01f) ? 1000.0f / ctx.timings.frameMs : 0.0f;
        const std::string info =
            Format(Tr(StrId::Status_Info), projectName.c_str(),
                   projectName.empty() ? "" : " | ", sceneName.c_str(),
                   ctx.scene->GetWorld().AliveCount(), fps);
        const float rightWidth =
            ImGui::CalcTextSize(info.c_str()).x + ImGui::CalcTextSize(stateText).x + 16.0f;

        // ---- 左側: 最新ログ (クリックで Console) ----
        const float logWidth = ImGui::GetContentRegionAvail().x - rightWidth;
        if (hasLast_ && logWidth > 50.0f) {
            ImGui::PushStyleColor(ImGuiCol_Text, LevelColor(last_.level));
            const std::string line =
                Format("%s %s", LevelIcon(last_.level), last_.message);
            if (ImGui::Selectable(line.c_str(), false, ImGuiSelectableFlags_None,
                                  ImVec2(logWidth, 0))
                && consoleOpen) {
                *consoleOpen = true;
            }
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", Tr(StrId::Status_TipConsole));
            }
        }

        ImGui::SameLine(ImGui::GetWindowWidth() - rightWidth);
        ImGui::TextDisabled("%s", info.c_str());
        ImGui::SameLine(0.0f, 0.0f);
        if (state == PlayState::Editing) {
            ImGui::TextDisabled("%s", stateText);
        } else {
            ImGui::TextColored(themeColor::PlayAccent, "%s", stateText);
        }
    }
    ImGui::End();
}

} // namespace mye
