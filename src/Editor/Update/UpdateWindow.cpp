//====================================================================================
//                          UpdateWindow.cpp
//  MyEngine/ 秋田蓮音                                                      11/10/2026
//                                          エディタの更新確認・更新窓・Help メニュー・About の実装
//====================================================================================
#include "Editor/Update/UpdateWindow.h"

#include <cstdio>
#include <ctime>
#include <filesystem>

#include <Windows.h>
#include <shellapi.h>

#include "Editor/App/EditorGlobalSettings.h"
#include "Editor/Widgets/ToastCenter.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Localization/Localization.h"
#include "Engine/Engine/App/Project.h"
#include "Engine/Engine/Update/InstallLayout.h"
#include "Engine/Platform/CrashHandler.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/ImGui/ImGuiTheme.h"

#include "imgui.h"

#include "fontawesome/IconsFontAwesome6.h"

namespace mye {

namespace {

constexpr int64_t kAutoCheckIntervalSec = 24 * 60 * 60;

// Tr() を printf の書式として直接渡さないための整形 (規則 10)
std::string Format(StrId id, const std::string& a)
{
    char buf[512] = {};
    snprintf(buf, sizeof(buf), Tr(id), a.c_str());
    return buf;
}

std::string Format(StrId id, const std::string& a, const std::string& b)
{
    char buf[512] = {};
    snprintf(buf, sizeof(buf), Tr(id), a.c_str(), b.c_str());
    return buf;
}

int64_t NowUnix()
{
    return static_cast<int64_t>(std::time(nullptr));
}

bool IsNewer(const FetchReleaseResult& r)
{
    return r.ok && r.found && CompareEngineVersion(r.release.version, CurrentEngineVersion()) > 0;
}

void OpenReleasesPage()
{
    const std::string url = std::string("https://github.com/") + kReleaseRepoOwner + "/" + kReleaseRepoName
        + "/releases/latest";
    ShellExecuteW(nullptr, L"open", Utf8ToWide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

} // namespace

bool ShouldAutoCheckUpdate(bool enabled, int64_t lastCheckUnix, int64_t nowUnix)
{
    if (!enabled) {
        return false;
    }
    return lastCheckUnix <= 0 || nowUnix < lastCheckUnix || nowUnix - lastCheckUnix >= kAutoCheckIntervalSec;
}

std::wstring FindLauncherForUpdate(const std::wstring& installRoot, const std::wstring& exeDir)
{
    // 利用者が置いた場所 (Launcher が起動時に記録する) → スタートメニューに登録した置き場 → エディタの隣 (0.86 の zip)
    const std::wstring recorded = RecordedLauncherPath(installRoot);
    if (!recorded.empty()) {
        return recorded;
    }
    std::error_code ec;
    const std::wstring placed = LauncherDir(installRoot) + L"\\" + kLauncherExeName;
    if (std::filesystem::exists(placed, ec)) {
        return placed;
    }
    const std::wstring beside = exeDir + L"\\" + kLauncherExeName;
    if (std::filesystem::exists(beside, ec)) {
        return beside;
    }
    return {};
}

UpdateWindow::~UpdateWindow()
{
    cancel_.store(true);
    if (worker_.joinable()) {
        worker_.join();
    }
}

void UpdateWindow::StartCheck(bool manual)
{
    if (busy_.load()) {
        manual_ = manual_ || manual;
        return;
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    manual_ = manual;
    busy_.store(true);
    worker_ = std::thread([this] {
        // 取得元は MYE_RELEASE_SOURCE (検証用のローカルフォルダ) か GitHub
        FetchReleaseResult r = FetchLatestRelease(ResolveReleaseSource(std::wstring()), false, &cancel_);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_ = std::move(r);
            hasPending_ = true;
        }
        busy_.store(false);
    });
}

void UpdateWindow::StartupCheck()
{
    EditorGlobalSettings globals;
    globals.Load();
    autoCheck_ = globals.autoUpdateCheck;
    settingsLoaded_ = true;
    if (ShouldAutoCheckUpdate(globals.autoUpdateCheck, globals.lastUpdateCheckUnix, NowUnix())) {
        StartCheck(false);
    }
}

void UpdateWindow::PollResult(ToastCenter& toasts)
{
    FetchReleaseResult r;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!hasPending_) {
            return;
        }
        r = std::move(pending_);
        hasPending_ = false;
    }
    latest_ = std::move(r);
    checked_ = true;
    if (latest_.ok) {
        // 確認できたときだけ時刻を残す (失敗したら次の起動でまた試す)
        EditorGlobalSettings globals;
        globals.Load();
        globals.lastUpdateCheckUnix = NowUnix();
        globals.Save();
    } else {
        // 自動確認の失敗は通知しない (オフラインでも編集は続けられる)。手動なら窓に理由が出る
        MYE_LOG_INFO("update check failed: %s", latest_.error.c_str());
    }
    if (IsNewer(latest_) && !manual_) {
        toasts.Notify(LogLevel::Info, Format(StrId::Upd_Toast, latest_.release.version.text), 10.0f);
    }
}

void UpdateWindow::DrawHelpMenu()
{
    if (!ImGui::BeginMenu(Tr(StrId::Menu_Help))) {
        return;
    }
    if (ImGui::MenuItem(Tr(StrId::Menu_CheckUpdates))) {
        open = true;
        StartCheck(true);
    }
    if (ImGui::MenuItem(Tr(StrId::Menu_About))) {
        aboutOpenRequest_ = true;
    }
    ImGui::EndMenu();
}

bool UpdateWindow::OnImGui(ToastCenter& toasts)
{
    if (!settingsLoaded_) {
        EditorGlobalSettings globals;
        globals.Load();
        autoCheck_ = globals.autoUpdateCheck;
        settingsLoaded_ = true;
    }
    if (!managedChecked_) {
        managedDir_ = FindManagedInstallDir(GetExecutableDir());
        managedChecked_ = true;
    }
    PollResult(toasts);
    if (open) {
        DrawUpdateWindow();
    }
    DrawAbout();
    const bool request = requestUpdate_;
    requestUpdate_ = false;
    return request;
}

void UpdateWindow::DrawUpdateWindow()
{
    ImGui::SetNextWindowSize(ImVec2(540.0f, 480.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(Tr(StrId::Win_Update), &open)) {
        ImGui::End();
        return;
    }
    ImGui::TextUnformatted(Format(StrId::Upd_Current, kEngineVersion).c_str());
    ImGui::Separator();

    if (busy_.load()) {
        ImGui::TextDisabled("%s", Tr(StrId::Upd_Checking));
    } else if (!checked_) {
        ImGui::TextDisabled("%s", Tr(StrId::Upd_NotChecked));
    } else if (!latest_.ok) {
        ImGui::PushStyleColor(ImGuiCol_Text, themeColor::Error);
        ImGui::TextWrapped("%s", Format(StrId::Upd_Failed, latest_.error).c_str());
        ImGui::PopStyleColor();
    } else if (!latest_.found) {
        ImGui::TextDisabled("%s", Tr(StrId::Upd_NoRelease));
    } else if (!IsNewer(latest_)) {
        ImGui::TextColored(themeColor::Success, ICON_FA_CHECK " %s", Tr(StrId::Upd_UpToDate));
    } else {
        const ReleaseInfo& r = latest_.release;
        ImGui::TextColored(themeColor::Accent, "%s",
                           Format(StrId::Upd_Available, r.version.text, r.publishedAt.substr(0, 10)).c_str());
        if (!r.notes.empty()) {
            ImGui::TextUnformatted(Tr(StrId::Upd_Notes));
            ImGui::BeginChild("##notes", ImVec2(0.0f, 180.0f), ImGuiChildFlags_Borders);
            ImGui::TextWrapped("%s", r.notes.c_str());
            ImGui::EndChild();
        }
        // Launcher が入れた版でないと置き場ごとの入れ替えができない (リポジトリから動かしている開発ビルドなど)
        const bool managed = !managedDir_.empty();
        const bool launcherFound = !FindLauncherForUpdate(InstallRoot(), GetExecutableDir()).empty();
        ImGui::BeginDisabled(!managed || !launcherFound);
        const std::string label = std::string(ICON_FA_DOWNLOAD " ") + Tr(StrId::Upd_UpdateRestart);
        if (ImGui::Button(label.c_str())) {
            requestUpdate_ = true;
        }
        ImGui::EndDisabled();
        if (!managed) {
            ImGui::TextWrapped("%s", Tr(StrId::Upd_DevBuild));
        } else if (!launcherFound) {
            ImGui::TextWrapped("%s", Tr(StrId::Upd_NoLauncher));
        }
    }

    ImGui::Spacing();
    ImGui::BeginDisabled(busy_.load());
    if (ImGui::Button(Tr(StrId::Upd_CheckNow))) {
        StartCheck(true);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(Tr(StrId::Upd_OpenReleases))) {
        OpenReleasesPage();
    }
    if (ImGui::Checkbox(Tr(StrId::Upd_AutoCheck), &autoCheck_)) {
        EditorGlobalSettings globals;
        globals.Load(); // 他のキーを消さないよう読み直してから保存
        globals.autoUpdateCheck = autoCheck_;
        globals.Save();
    }
    ImGui::End();
}

void UpdateWindow::DrawAbout()
{
    const char* popupId = Tr(StrId::About_Title);
    if (aboutOpenRequest_) {
        aboutOpenRequest_ = false;
        ImGui::OpenPopup(popupId);
    }
    if (!ImGui::BeginPopupModal(popupId, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    ImGui::TextUnformatted("MyEngine");
    ImGui::TextUnformatted(Format(StrId::About_Version, kEngineVersion).c_str());
    ImGui::TextDisabled("%s", Format(StrId::About_Build, BuildGitHash(), BuildConfigName()).c_str());
    if (!managedDir_.empty()) {
        ImGui::TextDisabled("%s", Format(StrId::About_Install, WideToUtf8(managedDir_)).c_str());
    } else {
        ImGui::TextDisabled("%s", Tr(StrId::About_SourceBuild));
    }
    ImGui::Separator();
    if (ImGui::Button(Tr(StrId::Upd_OpenReleases))) {
        OpenReleasesPage();
    }
    ImGui::SameLine();
    if (ImGui::Button("OK")) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

bool UpdateWindow::LaunchUpdater(const std::wstring& projectRoot, std::string& error)
{
    const std::wstring launcher = FindLauncherForUpdate(InstallRoot(), GetExecutableDir());
    if (launcher.empty()) {
        error = "MyEngineLauncher.exe not found";
        return false;
    }
    std::wstring args = L"--update";
    if (!projectRoot.empty()) {
        args += L" --launch-project \"" + projectRoot + L"\"";
    }
    const std::wstring dir = std::filesystem::path(launcher).parent_path().wstring();
    const HINSTANCE r = ShellExecuteW(nullptr, L"open", launcher.c_str(), args.c_str(), dir.c_str(), SW_SHOWNORMAL);
    if (reinterpret_cast<intptr_t>(r) <= 32) {
        error = "ShellExecute failed (" + std::to_string(reinterpret_cast<intptr_t>(r)) + ")";
        return false;
    }
    MYE_LOG_INFO("update: started %s", WideToUtf8(launcher).c_str());
    return true;
}

} // namespace mye
