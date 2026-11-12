//====================================================================================
//                          LauncherUi.cpp
//  MyEngine/ 秋田蓮音                                                      11/10/2026
//                                          Launcher の画面と --update の流れの実装
//====================================================================================
#include "Launcher/LauncherUi.h"

#include <cstdio>
#include <filesystem>

#include <Windows.h>
#include <shellapi.h>
#include <shobjidl.h> // IFileOpenDialog (インストール先の選択)

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Localization/Localization.h"
#include "Engine/Engine/App/Project.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/ImGui/ImGuiTheme.h"
#include "Launcher/LauncherSelf.h"

#include "imgui.h"

#include "fontawesome/IconsFontAwesome6.h"

namespace mye {

namespace {

constexpr double kBytesPerMb = 1024.0 * 1024.0;

bool IsInstalled(const LauncherState& st, const EngineVersion& v)
{
    for (const InstalledEngine& e : st.installed) {
        if (CompareEngineVersion(e.version, v) == 0) {
            return true;
        }
    }
    return false;
}

void Rescan(LauncherState& st)
{
    st.installed = ScanInstalledEngines(st.options.root);
}

void SetMessage(LauncherState& st, std::string text, bool isError)
{
    st.message = std::move(text);
    st.messageIsError = isError;
    if (isError) {
        MYE_LOG_WARN("launcher: %s", st.message.c_str());
    }
}

// printf 系に Tr() を書式として渡さないための整形 (規則 10)
std::string Format1(StrId id, const std::string& arg)
{
    const char* fmt = Tr(id);
    const int n = snprintf(nullptr, 0, fmt, arg.c_str());
    std::string out(n > 0 ? static_cast<size_t>(n) : 0, '\0');
    if (n > 0) {
        snprintf(out.data(), out.size() + 1, fmt, arg.c_str());
    }
    return out;
}

// OS のフォルダ選択ダイアログ (ProjectManager の PickFolderDialog と同じ手。Launcher は Editor 層に依存しない)。
// 開いている間は描画ループが止まるが、ワーカーは動かしていない (押せるのは作業中でないときだけ)。キャンセルは空文字
std::wstring PickInstallFolder(const std::wstring& initialDir)
{
    std::wstring result;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
        return result;
    }
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(Utf8ToWide(Tr(StrId::Launcher_PickInstallDir)).c_str());
    IShellItem* initial = nullptr;
    std::error_code ec;
    if (std::filesystem::is_directory(initialDir, ec)
        && SUCCEEDED(SHCreateItemFromParsingName(initialDir.c_str(), nullptr, IID_PPV_ARGS(&initial)))) {
        dlg->SetFolder(initial);
        initial->Release();
    }
    if (SUCCEEDED(dlg->Show(GetActiveWindow()))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR psz = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &psz))) {
                result = psz;
                CoTaskMemFree(psz);
            }
            item->Release();
        }
    }
    dlg->Release();
    return result;
}

// 置き場を記録してからインストールを始める (記録できないと入れた版が一覧に出ないので、そのときは始めない)
void StartInstallTo(LauncherState& st, const ReleaseInfo& release, const std::wstring& enginesDir)
{
    if (IsInsideInstalledEngine(enginesDir)) {
        SetMessage(st, Tr(StrId::Launcher_InstallDirInsideEngine), true);
        return;
    }
    if (!RememberEnginesDir(st.options.root, enginesDir)) {
        SetMessage(st, Format1(StrId::Launcher_InstallFailed, "cannot record the install folder in "
                                                                  + WideToUtf8(st.options.root)),
                   true);
        return;
    }
    st.job.StartInstall(st.options.source, release, st.options.root, enginesDir);
}

const char* StageLabel(InstallStage stage)
{
    switch (stage) {
    case InstallStage::Checking: return Tr(StrId::Launcher_StageChecking);
    case InstallStage::Downloading: return Tr(StrId::Launcher_StageDownloading);
    case InstallStage::Verifying: return Tr(StrId::Launcher_StageVerifying);
    case InstallStage::Extracting: return Tr(StrId::Launcher_StageExtracting);
    case InstallStage::Finishing: return Tr(StrId::Launcher_StageFinishing);
    case InstallStage::Deleting: return Tr(StrId::Launcher_StageDeleting);
    case InstallStage::Idle: break;
    }
    return "";
}

// --update の締め: 最新のインストール済みの版でエディタを開いて終わる
bool FinishUpdateFlow(LauncherState& st)
{
    st.updateFlowDone = true;
    Rescan(st);
    if (st.installed.empty()) {
        return true; // 入っている版が無い。画面に理由を出したまま残る
    }
    std::string error;
    if (!LaunchInstalledEditor(st.installed.front().dir, st.options.launchProject, error)) {
        SetMessage(st, Format1(StrId::Launcher_LaunchFailed, error), true);
        return true;
    }
    return false;
}

void DrawProgress(LauncherState& st)
{
    InstallStage stage = InstallStage::Idle;
    uint64_t done = 0;
    uint64_t total = 0;
    st.job.Progress(stage, done, total);
    const float fraction = total > 0 ? static_cast<float>(static_cast<double>(done) / static_cast<double>(total)) : 0.0f;
    char overlay[96] = {};
    if (total > 0) {
        snprintf(overlay, sizeof(overlay), "%.1f / %.1f MB", static_cast<double>(done) / kBytesPerMb,
                 static_cast<double>(total) / kBytesPerMb);
    }
    ImGui::TextUnformatted(StageLabel(stage));
    if (total > 0) {
        ImGui::ProgressBar(fraction, ImVec2(-120.0f, 0.0f), overlay);
    } else {
        // 全体量が分からない段は時間で揺らす (止まっていないことだけ伝える)
        ImGui::ProgressBar(-1.0f * static_cast<float>(ImGui::GetTime()), ImVec2(-120.0f, 0.0f), "");
    }
    ImGui::SameLine();
    if (st.job.CurrentKind() == LauncherJob::Kind::Install && ImGui::Button(Tr(StrId::Common_Cancel))) {
        st.job.Cancel();
    }
}

void DrawLatest(LauncherState& st)
{
    ImGui::PushFont(EditorHeadingFont(), ImGui::GetStyle().FontSizeBase * 1.15f);
    ImGui::TextUnformatted(Tr(StrId::Launcher_Latest));
    ImGui::PopFont();

    const bool busy = st.job.Busy();
    if (st.checkDone && st.latest.found && !st.options.updateMode && LauncherUpdateAvailable(st.latest.release)) {
        ImGui::TextColored(themeColor::Warning, "%s",
                           Format1(StrId::Launcher_SelfUpdateAvail, st.latest.release.version.text).c_str());
        ImGui::SameLine();
        ImGui::BeginDisabled(busy);
        const std::string label = std::string(ICON_FA_ROTATE " ") + Tr(StrId::Launcher_SelfUpdate);
        if (ImGui::SmallButton(label.c_str())) {
            st.job.StartSelfUpdate(st.options.source, st.latest.release, RunningLauncherPath());
        }
        ImGui::EndDisabled();
    }
    if (!st.checkDone) {
        ImGui::TextDisabled("%s", Tr(StrId::Launcher_Checking));
        return;
    }
    if (!st.latest.ok) {
        ImGui::PushStyleColor(ImGuiCol_Text, themeColor::Error);
        ImGui::TextWrapped("%s", Format1(StrId::Launcher_CheckFailed, st.latest.error).c_str());
        ImGui::PopStyleColor();
    } else if (!st.latest.found) {
        ImGui::TextDisabled("%s", Tr(StrId::Launcher_NoRelease));
    } else {
        const ReleaseInfo& r = st.latest.release;
        const std::string date = r.publishedAt.substr(0, 10);
        char line[256] = {};
        snprintf(line, sizeof(line), Tr(StrId::Launcher_LatestLine), r.version.text.c_str(), date.c_str());
        ImGui::TextUnformatted(line);
        ImGui::SameLine();
        if (IsInstalled(st, r.version)) {
            ImGui::TextColored(themeColor::Success, ICON_FA_CHECK " %s", Tr(StrId::Launcher_AlreadyInstalled));
        } else {
            ImGui::BeginDisabled(busy);
            const std::string label = std::string(ICON_FA_DOWNLOAD " ") + Tr(StrId::Launcher_Install);
            if (ImGui::Button(label.c_str())) {
                // インストールのたびに置き場を選ぶ (既定は前回の置き場)。版は <選んだフォルダ>\<版>\ に入る
                const std::wstring picked = PickInstallFolder(PreferredEnginesDir(st.options.root));
                if (!picked.empty()) {
                    StartInstallTo(st, r, picked);
                }
            }
            ImGui::EndDisabled();
        }
        if (!r.notes.empty() && ImGui::CollapsingHeader(Tr(StrId::Launcher_ReleaseNotes))) {
            ImGui::BeginChild("##notes", ImVec2(0.0f, 140.0f), ImGuiChildFlags_Borders);
            ImGui::TextWrapped("%s", r.notes.c_str());
            ImGui::EndChild();
        }
    }
    ImGui::BeginDisabled(busy);
    if (ImGui::SmallButton(Tr(StrId::Launcher_Retry))) {
        st.checkDone = false;
        st.job.StartCheck(st.options.source, st.options.includePrerelease);
    }
    ImGui::EndDisabled();
}

void DrawInstalled(LauncherState& st)
{
    ImGui::PushFont(EditorHeadingFont(), ImGui::GetStyle().FontSizeBase * 1.15f);
    ImGui::TextUnformatted(Tr(StrId::Launcher_InstalledList));
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextWrapped("%s",
                       Format1(StrId::Launcher_InstallPlace, WideToUtf8(PreferredEnginesDir(st.options.root))).c_str());
    ImGui::PopStyleColor();

    if (st.installed.empty()) {
        ImGui::TextDisabled("%s", Tr(StrId::Launcher_NoneInstalled));
        return;
    }
    const bool busy = st.job.Busy();
    if (!ImGui::BeginTable("##installed", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        return;
    }
    ImGui::TableSetupColumn("version", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("actions", ImGuiTableColumnFlags_WidthFixed);
    for (size_t i = 0; i < st.installed.size(); ++i) {
        const InstalledEngine& e = st.installed[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("MyEngine %s", e.version.text.c_str());
        if (i == 0) {
            ImGui::SameLine();
            ImGui::TextColored(themeColor::Accent, "%s", Tr(StrId::Launcher_NewestMark));
        }
        // 置き場は複数ありうるので、どこに入っているかを行ごとに出す
        ImGui::SameLine();
        const std::string place = WideToUtf8(std::filesystem::path(e.dir).parent_path().wstring());
        ImGui::TextDisabled("%s", place.c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", place.c_str()); // 長いパスは列の幅で切れるので全体を出す
        }
        ImGui::TableNextColumn();
        if (ImGui::Button(ICON_FA_PLAY)) {
            std::string error;
            if (!LaunchInstalledEditor(e.dir, std::wstring(), error)) {
                SetMessage(st, Format1(StrId::Launcher_LaunchFailed, error), true);
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", Tr(StrId::Launcher_Launch));
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_WRENCH)) {
            std::string error;
            if (!RunSetupScript(e.dir, error)) {
                SetMessage(st, Format1(StrId::Launcher_LaunchFailed, error), true);
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", Tr(StrId::Launcher_RunSetup));
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_FOLDER_OPEN)) {
            ShellExecuteW(nullptr, L"open", e.dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", Tr(StrId::Launcher_OpenFolder));
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(busy);
        if (ImGui::Button(ICON_FA_TRASH)) {
            st.deleteTarget = e.dir;
            st.deleteLabel = e.version.text;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", Tr(StrId::Common_Delete));
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void DrawDeleteConfirm(LauncherState& st)
{
    const char* popupId = Tr(StrId::Launcher_DeleteTitle);
    if (!st.deleteTarget.empty() && !ImGui::IsPopupOpen(popupId)) {
        ImGui::OpenPopup(popupId);
    }
    if (ImGui::BeginPopupModal(popupId, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(Format1(StrId::Launcher_DeleteMsg, st.deleteLabel).c_str());
        ImGui::Separator();
        if (ImGui::Button(Tr(StrId::Common_Delete))) {
            st.job.StartUninstall(st.deleteTarget);
            st.deleteTarget.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(Tr(StrId::Common_Cancel))) {
            st.deleteTarget.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

} // namespace

void LauncherBegin(LauncherState& st)
{
    st.inStartMenu = IsLauncherInStartMenu();
    Rescan(st);
    st.job.StartCheck(st.options.source, st.options.includePrerelease);
}

bool LauncherTick(LauncherState& st)
{
    FetchReleaseResult check;
    if (st.job.TakeCheckResult(check)) {
        st.latest = std::move(check);
        st.checkDone = true;
        if (st.options.updateMode && !st.updateFlowDone) {
            if (st.latest.ok && st.latest.found && !IsInstalled(st, st.latest.release.version)) {
                // エディタからの更新は確認を挟まず、前回の置き場に入れる
                StartInstallTo(st, st.latest.release, PreferredEnginesDir(st.options.root));
                if (!st.job.Busy()) {
                    st.updateFlowDone = true; // 始められなかった。画面に理由を出して手で選んでもらう
                }
            } else if (st.latest.ok) {
                return FinishUpdateFlow(st); // もう最新が入っている
            } else {
                st.updateFlowDone = true; // 確認に失敗した。画面に理由を出して手で選んでもらう
            }
        }
    }
    InstallOutcome outcome;
    if (st.job.TakeInstallResult(outcome)) {
        const LauncherJob::Kind kind = st.job.CurrentKind();
        // この機械で初めて入れた版なら依存の導入 (Setup.ps1) も走らせる。エディタからの更新では入っている前提
        const bool firstInstall = st.installed.empty();
        Rescan(st);
        if (kind == LauncherJob::Kind::SelfUpdate && outcome.ok) {
            std::string error;
            if (ApplyLauncherUpdate(RunningLauncherPath(), st.options.root, outcome.version, true, error)) {
                return false; // 新しい Launcher が起動した
            }
            SetMessage(st, Format1(StrId::Launcher_SelfUpdateFailed, error), true);
            return true;
        }
        if (kind == LauncherJob::Kind::Install && outcome.ok && !outcome.alreadyInstalled && firstInstall
            && !st.options.updateMode) {
            std::string error;
            if (!RunSetupScript(outcome.installDir, error)) {
                MYE_LOG_WARN("launcher: setup did not start: %s", error.c_str());
            }
        }
        if (outcome.cancelled) {
            SetMessage(st, Tr(StrId::Launcher_Cancelled), false);
        } else if (!outcome.ok) {
            const StrId id = kind == LauncherJob::Kind::Uninstall    ? StrId::Launcher_DeleteFailed
                           : kind == LauncherJob::Kind::SelfUpdate ? StrId::Launcher_SelfUpdateFailed
                                                                   : StrId::Launcher_InstallFailed;
            SetMessage(st, Format1(id, outcome.error), true);
        } else if (kind == LauncherJob::Kind::Install) {
            SetMessage(st, Format1(StrId::Launcher_InstallDone, outcome.version), false);
        } else {
            SetMessage(st, std::string(), false);
        }
        if (st.options.updateMode && !st.updateFlowDone && kind == LauncherJob::Kind::Install) {
            if (outcome.ok) {
                return FinishUpdateFlow(st);
            }
            st.updateFlowDone = true;
        }
    }
    return true;
}

bool DrawLauncherUi(LauncherState& st)
{
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##launcher", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
                     | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus
                     | ImGuiWindowFlags_NoSavedSettings);

    ImGui::PushFont(EditorHeadingFont(), ImGui::GetStyle().FontSizeBase * 1.4f);
    ImGui::TextUnformatted(Tr(StrId::Launcher_Title));
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", Format1(StrId::Launcher_SelfVersion, kEngineVersion).c_str());
    // スタートメニューへの登録は押したときだけ (自動で自分を写して居座らない、ADR-037 D7)
    if (!st.inStartMenu) {
        ImGui::SameLine();
        const std::string label = std::string(ICON_FA_THUMBTACK " ") + Tr(StrId::Launcher_AddToStartMenu);
        if (ImGui::SmallButton(label.c_str())) {
            std::string error;
            if (AddLauncherToStartMenu(st.options.root, error)) {
                st.inStartMenu = true;
                SetMessage(st, Tr(StrId::Launcher_AddedToStartMenu), false);
            } else {
                SetMessage(st, Format1(StrId::Launcher_AddToStartMenuFailed, error), true);
            }
        }
    }
    if (st.options.updateMode && !st.updateFlowDone) {
        const std::string project = st.options.launchProject.empty()
            ? std::string("Hub")
            : WideToUtf8(std::filesystem::path(st.options.launchProject).filename().wstring());
        ImGui::TextColored(themeColor::Accent, "%s", Format1(StrId::Launcher_UpdateMode, project).c_str());
    }
    ImGui::Separator();
    ImGui::Spacing();

    DrawLatest(st);
    ImGui::Spacing();
    if (st.job.Busy() && st.job.CurrentKind() != LauncherJob::Kind::Check) {
        DrawProgress(st);
    }
    if (!st.message.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, st.messageIsError ? themeColor::Error : themeColor::Success);
        ImGui::TextWrapped("%s", st.message.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    DrawInstalled(st);
    DrawDeleteConfirm(st);

    ImGui::End();
    return true;
}

} // namespace mye
