#include "Editor/ProjectManager.h"

#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>

#include <Windows.h>
#include <shellapi.h> // ShellExecuteW (自 exe 再起動)
#include <shobjidl.h> // IFileOpenDialog (フォルダピッカー)

#include "Editor/ProjectRegistry.h"
#include "Editor/ProjectTemplates.h"
#include "Engine/Core/Localization.h"
#include "Engine/Core/Log.h"
#include "Engine/Engine/Project.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Platform/Win32Window.h"
#include "Engine/Renderer/GraphicsDevice.h"
#include "Engine/Renderer/ImGuiRenderer.h"
#include "Engine/Renderer/ImGuiTheme.h" // themeColor (意味色)
#include "Engine/Renderer/SwapChain.h"

#include "imgui.h"

#include "fontawesome/IconsFontAwesome6.h"

namespace mye {

namespace {

// OS のフォルダ選択ダイアログ。キャンセルは空文字
std::wstring PickFolderDialog(void* ownerHwnd)
{
    std::wstring result;
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&dlg)))) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        if (SUCCEEDED(dlg->Show(static_cast<HWND>(ownerHwnd)))) {
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
    }
    return result;
}

std::wstring DefaultProjectsDir()
{
    wchar_t* profile = nullptr;
    size_t len = 0;
    std::wstring base;
    if (_wdupenv_s(&profile, &len, L"USERPROFILE") == 0 && profile) {
        base = profile;
        free(profile);
    }
    if (base.empty()) {
        return L"C:\\MyEngineProjects";
    }
    return base + L"\\Documents\\MyEngineProjects";
}

// フォルダをごみ箱へ移動する (FOF_ALLOWUNDO)。SHFileOperationW は pFrom を
// 二重 null 終端で要求するので push_back(L'\0') + c_str() で満たす
bool MoveToRecycleBin(const std::wstring& path)
{
    std::wstring from = path;
    from.push_back(L'\0');
    SHFILEOPSTRUCTW op = {};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
    const int r = SHFileOperationW(&op);
    return r == 0 && !op.fAnyOperationsAborted;
}

// Hub 画面の編集状態 (プロセス内で 1 回しか走らないのでフレームを跨いで保持するだけ)
struct HubState {
    ProjectRegistry registry;
    char newName[128] = "MyGame";
    char newLocation[512] = {};
    int newTemplate = 1; // 0=Empty 1=3D Demo
    char openPath[512] = {};
    std::string errorText; // 非空でエラーモーダルを開く
    // 削除確認モーダルの対象 (非空でモーダルを開く)
    std::wstring deleteTargetPath;
    std::string deleteTargetLabel;
    // リネームモーダルの対象 (非空でモーダルを開く)
    std::wstring renameTargetPath;
    char renameBuf[128] = {};
    // プロジェクトごとの engineVersion キャッシュ (毎フレームのファイル IO を避ける)
    std::unordered_map<std::wstring, std::string> versionCache;
};

const char* EntryEngineVersion(HubState& st, const ProjectRegistryEntry& e)
{
    const std::wstring key = NormalizePathKey(e.path);
    auto it = st.versionCache.find(key);
    if (it == st.versionCache.end()) {
        ProjectManifest m;
        std::string ver;
        if (!e.missing && LoadProjectManifest(e.path, m)) {
            ver = m.engineVersion;
        }
        it = st.versionCache.emplace(key, std::move(ver)).first;
    }
    return it->second.c_str();
}

// 戻り値 true = ループ継続。outcome が OpenProject になったら呼び出し側が終了する
bool DrawHubUi(HubState& st, void* hwnd, ProjectManagerOutcome& outcome)
{
    bool keepRunning = true;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##hub", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
                     | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus
                     | ImGuiWindowFlags_NoSavedSettings);

    ImGui::TextUnformatted(Tr(StrId::Hub_Title));
    ImGui::SameLine();
    ImGui::TextDisabled(Tr(StrId::Hub_EngineVer), kEngineVersion);
    ImGui::Separator();

    const float footer = ImGui::GetFrameHeightWithSpacing();
    const float listWidth = ImGui::GetContentRegionAvail().x * 0.56f;

    // ---- 左: プロジェクト一覧 (pinned → lastOpened 降順) ----
    ImGui::BeginChild("##projects", ImVec2(listWidth, -footer), ImGuiChildFlags_Borders);
    if (st.registry.Entries().empty()) {
        ImGui::TextDisabled("%s", Tr(StrId::Hub_Empty));
    }
    // 表示中の変更操作を安全にするためコピーで回す (Touch/Remove が並びを変える)
    const std::vector<ProjectRegistryEntry> entries = st.registry.Entries();
    for (size_t i = 0; i < entries.size(); ++i) {
        const ProjectRegistryEntry& e = entries[i];
        ImGui::PushID(static_cast<int>(i));

        // ピン留めトグル (非ピンは薄く表示)
        if (!e.pinned) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        }
        if (ImGui::SmallButton(ICON_FA_THUMBTACK)) {
            st.registry.SetPinned(e.path, !e.pinned);
        }
        if (!e.pinned) {
            ImGui::PopStyleColor();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(e.pinned ? "ピン留めを外す" : "先頭にピン留め");
        }
        ImGui::SameLine();

        const std::string label =
            (e.name.empty() ? WideToUtf8(std::filesystem::path(e.path).filename().wstring())
                            : e.name);
        if (e.missing) {
            ImGui::TextDisabled("%s  (missing)", label.c_str());
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60.0f);
            if (ImGui::SmallButton(Tr(StrId::Hub_Remove))) {
                st.registry.Remove(e.path);
            }
        } else {
            if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick)) {
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    outcome.action = ProjectManagerAction::OpenProject;
                    outcome.projectRoot = e.path;
                    keepRunning = false;
                }
            }
            // 行の右クリックメニュー (M33a)。entries はコピーなので Remove しても安全
            if (ImGui::BeginPopupContextItem("##projctx")) {
                if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN " Show in Explorer")) {
                    ShellExecuteW(nullptr, L"open", e.path.c_str(), nullptr, nullptr,
                                  SW_SHOWNORMAL);
                }
                if (ImGui::MenuItem(ICON_FA_PEN " 名前を変更...")) {
                    st.renameTargetPath = e.path;
                    strncpy_s(st.renameBuf, label.c_str(), _TRUNCATE);
                }
                if (ImGui::MenuItem(Tr(StrId::Hub_RemoveFromList))) {
                    st.registry.Remove(e.path);
                }
                ImGui::Separator();
                if (ImGui::MenuItem(ICON_FA_TRASH " プロジェクトを削除...")) {
                    st.deleteTargetPath = e.path;
                    st.deleteTargetLabel = label;
                }
                ImGui::EndPopup();
            }
            const char* ver = EntryEngineVersion(st, e);
            if (ver[0] != '\0' && std::strcmp(ver, kEngineVersion) != 0) {
                ImGui::SameLine();
                ImGui::TextColored(themeColor::Warning, "[engine %s]", ver);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(Tr(StrId::Hub_TipEngineVer), ver, kEngineVersion);
                }
            }
        }
        ImGui::Indent();
        ImGui::TextDisabled("%s", WideToUtf8(e.path).c_str());
        if (!e.lastOpenedIso.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("| %s", e.lastOpenedIso.c_str());
        }
        ImGui::Unindent();
        ImGui::Spacing();
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // ---- 右: 新規作成 / 既存を開く ----
    ImGui::BeginChild("##actions", ImVec2(0, -footer));
    ImGui::SeparatorText(Tr(StrId::Hub_NewProject));
    ImGui::InputText(Tr(StrId::Hub_NameField), st.newName, sizeof(st.newName));
    ImGui::InputText(Tr(StrId::Hub_Location), st.newLocation, sizeof(st.newLocation));
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_FOLDER_OPEN "##loc")) {
        const std::wstring picked = PickFolderDialog(hwnd);
        if (!picked.empty()) {
            const std::string utf8 = WideToUtf8(picked);
            strncpy_s(st.newLocation, utf8.c_str(), _TRUNCATE);
        }
    }
    ImGui::Combo(Tr(StrId::Hub_Template), &st.newTemplate, "Empty\0" "3D Demo\0");
    if (ImGui::Button(Tr(StrId::Common_Create), ImVec2(120, 0))) {
        const std::wstring dir =
            Utf8ToWide(st.newLocation) + L"\\" + Utf8ToWide(st.newName);
        std::string err;
        const ProjectTemplate tmpl =
            (st.newTemplate == 1) ? ProjectTemplate::Demo3D : ProjectTemplate::Empty;
        if (CreateProject(dir, st.newName, tmpl, FindAssetsRoot(), &err)) {
            st.registry.Touch(dir, st.newName);
            outcome.action = ProjectManagerAction::OpenProject;
            outcome.projectRoot = dir;
            keepRunning = false;
        } else {
            st.errorText = err;
        }
    }

    ImGui::Spacing();
    ImGui::SeparatorText(Tr(StrId::Hub_OpenExisting));
    ImGui::InputText(Tr(StrId::Hub_PathField), st.openPath, sizeof(st.openPath));
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_FOLDER_OPEN "##open")) {
        const std::wstring picked = PickFolderDialog(hwnd);
        if (!picked.empty()) {
            const std::string utf8 = WideToUtf8(picked);
            strncpy_s(st.openPath, utf8.c_str(), _TRUNCATE);
        }
    }
    if (ImGui::Button(Tr(StrId::Hub_Open), ImVec2(120, 0))) {
        const std::wstring dir = Utf8ToWide(st.openPath);
        ProjectManifest m;
        if (IsProjectRoot(dir) && LoadProjectManifest(dir, m)) {
            st.registry.Touch(dir, m.name);
            outcome.action = ProjectManagerAction::OpenProject;
            outcome.projectRoot = dir;
            keepRunning = false;
        } else {
            st.errorText = "Not a MyEngine project (project.mye.json not found):\n"
                           + std::string(st.openPath);
        }
    }
    ImGui::EndChild();

    ImGui::TextDisabled("%s", Tr(StrId::Hub_TipOpen));

    // ---- 削除確認モーダル (M33a) ----
    if (!st.deleteTargetPath.empty() && !ImGui::IsPopupOpen(Tr(StrId::Popup_DeleteProject))) {
        ImGui::OpenPopup(Tr(StrId::Popup_DeleteProject));
    }
    if (ImGui::BeginPopupModal(Tr(StrId::Popup_DeleteProject), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("プロジェクト \"%s\" を削除します", st.deleteTargetLabel.c_str());
        ImGui::TextDisabled("%s", WideToUtf8(st.deleteTargetPath).c_str());
        ImGui::TextUnformatted(Tr(StrId::Hub_TrashNote));
        ImGui::Separator();
        if (ImGui::Button(ICON_FA_TRASH " ごみ箱へ移動", ImVec2(160, 0))) {
            if (MoveToRecycleBin(st.deleteTargetPath)) {
                st.registry.Remove(st.deleteTargetPath);
            } else {
                st.errorText = "削除に失敗しました:\n" + WideToUtf8(st.deleteTargetPath);
            }
            st.deleteTargetPath.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(Tr(StrId::Common_Cancel), ImVec2(120, 0))) {
            st.deleteTargetPath.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemDefaultFocus(); // 破壊的操作なので既定フォーカスはキャンセル側
        ImGui::EndPopup();
    }

    // ---- リネームモーダル: 表示名 + project.mye.json の name のみ (フォルダ名は変えない) ----
    if (!st.renameTargetPath.empty() && !ImGui::IsPopupOpen(Tr(StrId::Popup_RenameProject))) {
        ImGui::OpenPopup(Tr(StrId::Popup_RenameProject));
    }
    if (ImGui::BeginPopupModal(Tr(StrId::Popup_RenameProject), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextDisabled("%s", WideToUtf8(st.renameTargetPath).c_str());
        ImGui::SetNextItemWidth(280.0f);
        const bool enter = ImGui::InputText(Tr(StrId::Hub_NameField), st.renameBuf, sizeof(st.renameBuf),
                                            ImGuiInputTextFlags_EnterReturnsTrue);
        std::string newName = st.renameBuf;
        while (!newName.empty() && (newName.back() == ' ' || newName.back() == '\t')) {
            newName.pop_back(); // 末尾空白は除去 (表示崩れ防止)
        }
        const bool doRename = ImGui::Button(Tr(StrId::Hub_Change), ImVec2(120, 0)) || enter;
        ImGui::SameLine();
        const bool cancelRename = ImGui::Button(Tr(StrId::Common_Cancel), ImVec2(120, 0));
        if (doRename && !newName.empty()) {
            // SaveProjectManifest は構造体から全書き出しするため、engineVersion/bootScene を
            // 保持するには先にロードが必須 (未知キーは現状存在しない)
            ProjectManifest m;
            if (LoadProjectManifest(st.renameTargetPath, m)) {
                m.name = newName;
                if (SaveProjectManifest(st.renameTargetPath, m)) {
                    st.registry.SetName(st.renameTargetPath, m.name);
                } else {
                    st.errorText = "project.mye.json の保存に失敗しました:\n"
                                   + WideToUtf8(st.renameTargetPath);
                }
            } else {
                st.errorText = "project.mye.json が読めません:\n"
                               + WideToUtf8(st.renameTargetPath);
            }
            st.renameTargetPath.clear();
            ImGui::CloseCurrentPopup();
        } else if (cancelRename) {
            st.renameTargetPath.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // ---- エラーモーダル ----
    if (!st.errorText.empty() && !ImGui::IsPopupOpen(Tr(StrId::Popup_Error))) {
        ImGui::OpenPopup(Tr(StrId::Popup_Error));
    }
    if (ImGui::BeginPopupModal(Tr(StrId::Popup_Error), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(st.errorText.c_str());
        if (ImGui::Button(Tr(StrId::Common_Ok), ImVec2(120, 0))) {
            st.errorText.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::End();
    return keepRunning;
}

} // namespace

ProjectManagerOutcome RunProjectManager(int testFrames, const std::wstring& shotPath)
{
    ProjectManagerOutcome outcome;

    // フォルダピッカー (IFileOpenDialog) 用。エンジン本体は起動しないのでここで初期化する
    const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    Win32Window window;
    GraphicsDevice device;
    SwapChain swapChain;
    ImGuiRenderer imgui;

    WindowDesc wd;
    wd.title = L"MyEngine Hub";
    wd.width = 960;
    wd.height = 600;
    if (!window.Create(wd) || !device.Init()
        || !swapChain.Init(device, window.Hwnd(), window.Width(), window.Height())) {
        MYE_LOG_ERROR("project manager: init failed");
        return outcome;
    }
    ImGuiInitOptions imguiOpts;
    imguiOpts.disableIni = true; // Hub のレイアウトは毎回固定 (保存しない)
    // 高 DPI 対応は本体 (EngineLoop) と同じ規約: 撮影中はスケールしない
    imguiOpts.dpiScale = shotPath.empty() ? window.DpiScale() : 1.0f;
    if (!imgui.Init(window, device, imguiOpts)) {
        return outcome;
    }

    HubState st;
    st.registry.Load();
    {
        const std::string loc = WideToUtf8(DefaultProjectsDir());
        strncpy_s(st.newLocation, loc.c_str(), _TRUNCATE);
    }

    int frame = 0;
    bool running = true;
    while (running) {
        if (!window.PumpMessages()) {
            break;
        }
        if (window.ConsumeResize()) {
            swapChain.Resize(window.Width(), window.Height());
        }
        if (window.IsMinimized()) {
            Sleep(10);
            continue;
        }

        ID3D11DeviceContext* dc = device.Context();
        ID3D11RenderTargetView* rtv = swapChain.BackbufferRTV();
        dc->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT vp = {};
        vp.Width = static_cast<float>(swapChain.Width());
        vp.Height = static_cast<float>(swapChain.Height());
        vp.MaxDepth = 1.0f;
        dc->RSSetViewports(1, &vp);
        const float clear[4] = { 0.06f, 0.06f, 0.07f, 1.0f };
        dc->ClearRenderTargetView(rtv, clear);

        imgui.BeginFrame();
        running = DrawHubUi(st, window.Hwnd(), outcome) && running;
        imgui.EndFrame();

        ++frame;
        const bool lastTestFrame = (testFrames > 0 && frame >= testFrames);
        if (!shotPath.empty() && (lastTestFrame || !running)) {
            swapChain.SaveBackbufferPng(shotPath); // Present 前に撮る
        }
        swapChain.Present(true);
        if (lastTestFrame) {
            running = false;
        }
    }

    imgui.Shutdown();
    swapChain.Shutdown();
    window.Destroy();
    if (coInit == S_OK || coInit == S_FALSE) {
        CoUninitialize();
    }
    return outcome;
}

bool RelaunchSelfWithProject(const std::wstring& projectRoot)
{
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    const std::wstring args = L"--project \"" + projectRoot + L"\"";
    // AssetOps::RebuildGameLogic と同じ ShellExecuteW パターン (引数 + 作業ディレクトリ)
    const HINSTANCE r =
        ShellExecuteW(nullptr, L"open", exePath, args.c_str(), projectRoot.c_str(), SW_SHOWNORMAL);
    const bool ok = reinterpret_cast<intptr_t>(r) > 32;
    if (!ok) {
        MYE_LOG_ERROR("relaunch failed: %s", WideToUtf8(exePath).c_str());
    }
    return ok;
}

} // namespace mye
