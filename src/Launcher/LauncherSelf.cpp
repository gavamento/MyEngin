//====================================================================================
//                          LauncherSelf.cpp
//  MyEngine/ 秋田蓮音                                                      11/10/2026
//                                          Launcher 自身の配置・自己更新・依存の導入の実装
//====================================================================================
#include "Launcher/LauncherSelf.h"

#include <filesystem>

#include <Windows.h>
#include <knownfolders.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include "nlohmann/json.hpp"

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Engine/App/Project.h"
#include "Engine/Engine/Update/InstallLayout.h"
#include "Engine/Platform/DownloadTrust.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Platform/Sha256.h"

namespace mye {

namespace {

namespace fs = std::filesystem;

constexpr const wchar_t* kVersionFile = L"launcher.json";
constexpr const wchar_t* kNewSuffix = L".new.exe";
constexpr const wchar_t* kOldSuffix = L".old.exe";
constexpr const wchar_t* kShortcutName = L"MyEngine Launcher.lnk";

std::wstring PlacedExe(const std::wstring& root)
{
    return LauncherDir(root) + L"\\" + kLauncherExeName;
}

// C:\x\MyEngineLauncher.exe → C:\x\MyEngineLauncher.new.exe
std::wstring SiblingPath(const std::wstring& exe, const wchar_t* suffix)
{
    const fs::path p(exe);
    return (p.parent_path() / (p.stem().wstring() + suffix)).wstring();
}

bool WriteVersionFile(const std::wstring& root, const std::string& version)
{
    nlohmann::json j;
    j["version"] = version;
    return WriteFileReplacing(LauncherDir(root) + L"\\" + kVersionFile, j.dump(2));
}

bool ScriptSupports(const std::wstring& script, const char* parameter)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, script.c_str(), L"rb") != 0 || !f) {
        return false;
    }
    std::string text;
    char buf[4096];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        text.append(buf, n);
    }
    fclose(f);
    return text.find(std::string("$") + parameter) != std::string::npos;
}

std::wstring StartMenuShortcutPath()
{
    PWSTR programs = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &programs))) {
        return {};
    }
    const std::wstring linkPath = std::wstring(programs) + L"\\" + kShortcutName;
    CoTaskMemFree(programs);
    return linkPath;
}

bool CreateStartMenuShortcut(const std::wstring& target)
{
    const std::wstring linkPath = StartMenuShortcutPath();
    if (linkPath.empty()) {
        return false;
    }
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) {
        return false;
    }
    link->SetPath(target.c_str());
    link->SetWorkingDirectory(fs::path(target).parent_path().c_str());
    link->SetDescription(L"MyEngine Launcher");
    link->SetIconLocation(target.c_str(), 0);
    IPersistFile* file = nullptr;
    bool ok = false;
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file)))) {
        ok = SUCCEEDED(file->Save(linkPath.c_str(), TRUE));
        file->Release();
    }
    link->Release();
    return ok;
}

} // namespace

std::wstring RunningLauncherPath()
{
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return buf;
}

EngineVersion PlacedLauncherVersion(const std::wstring& root)
{
    EngineVersion v;
    std::error_code ec;
    const fs::path path = fs::path(LauncherDir(root)) / kVersionFile;
    if (!fs::exists(path, ec)) {
        return v;
    }
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) {
        return v;
    }
    const nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
    fclose(f);
    if (j.is_object()) {
        ParseEngineVersion(j.value("version", std::string()), v);
    }
    return v;
}

bool AddLauncherToStartMenu(const std::wstring& root, std::string& error)
{
    const std::wstring running = RunningLauncherPath();
    const std::wstring placed = PlacedExe(root);
    std::error_code ec;
    if (NormalizePathKey(running) != NormalizePathKey(placed)) {
        const EngineVersion existing = PlacedLauncherVersion(root);
        const bool placedIsNewer = fs::exists(placed, ec) && existing.valid
            && CompareEngineVersion(existing, CurrentEngineVersion()) > 0;
        if (!placedIsNewer) {
            fs::create_directories(LauncherDir(root), ec);
            if (!CopyFileW(running.c_str(), placed.c_str(), FALSE)) {
                // 置き場の Launcher が動いていると上書きできない
                error = "cannot copy the Launcher to " + WideToUtf8(placed) + " (" + std::to_string(GetLastError()) + ")";
                return false;
            }
            WriteVersionFile(root, CurrentEngineVersion().text);
        }
    }
    if (!CreateStartMenuShortcut(placed)) {
        error = "cannot create the Start menu shortcut";
        return false;
    }
    RecordLauncherLocation(root, placed);
    MYE_LOG_INFO("launcher: added to the Start menu (%s)", WideToUtf8(placed).c_str());
    return true;
}

bool IsLauncherInStartMenu()
{
    const std::wstring linkPath = StartMenuShortcutPath();
    std::error_code ec;
    return !linkPath.empty() && fs::exists(linkPath, ec);
}

void CleanupOldLauncherFiles(const std::wstring& launcherExe)
{
    std::error_code ec;
    fs::remove(SiblingPath(launcherExe, kOldSuffix), ec);
    fs::remove(SiblingPath(launcherExe, kNewSuffix), ec);
}

bool LauncherUpdateAvailable(const ReleaseInfo& release)
{
    const std::string hashName = std::string(kLauncherAssetName) + ".sha256";
    return release.version.valid && CompareEngineVersion(release.version, CurrentEngineVersion()) > 0
        && FindReleaseAsset(release, kLauncherAssetName) && FindReleaseAsset(release, hashName);
}

InstallOutcome DownloadLauncherUpdate(const ReleaseSource& source, const ReleaseInfo& release,
                                      const std::wstring& targetExe, const InstallProgressFn& progress,
                                      const std::atomic<bool>* cancel)
{
    InstallOutcome out;
    out.version = release.version.text;
    const ReleaseAsset* exe = FindReleaseAsset(release, kLauncherAssetName);
    const ReleaseAsset* hash = FindReleaseAsset(release, std::string(kLauncherAssetName) + ".sha256");
    if (!exe || !hash) {
        out.error = "the release has no Launcher";
        return out;
    }
    std::string hashText;
    std::string expected;
    const HttpResult h = FetchReleaseAssetText(source, *hash, hashText, cancel);
    if (!h.ok || !ParseSha256Text(hashText, expected)) {
        out.cancelled = h.cancelled;
        out.error = h.ok ? "the Launcher .sha256 is malformed" : h.error;
        return out;
    }
    std::error_code ec;
    const std::wstring newPath = SiblingPath(targetExe, kNewSuffix);
    const HttpResult d = FetchReleaseAsset(
        source, *exe, newPath,
        [&](uint64_t done, uint64_t total) {
            if (progress) {
                progress(InstallStage::Downloading, done, total != 0 ? total : exe->size);
            }
        },
        cancel);
    if (!d.ok) {
        out.cancelled = d.cancelled;
        out.error = d.error;
        return out;
    }
    if (exe->url.rfind("https://", 0) == 0) {
        // Launcher 自身もブラウザと同じく印を付けてスキャンを通す (ADR-037 D9)
        const DownloadTrustResult trust = MarkAndScanDownload(newPath, exe->url);
        if (!trust.ok) {
            fs::remove(newPath, ec);
            out.error = trust.error;
            return out;
        }
    }
    std::string actual;
    if (!Sha256FileHex(newPath, actual) || actual != expected) {
        fs::remove(newPath, ec);
        out.error = "SHA-256 mismatch for the Launcher";
        return out;
    }
    out.ok = true;
    out.installDir = newPath;
    return out;
}

bool ApplyLauncherUpdate(const std::wstring& targetExe, const std::wstring& root, const std::string& newVersion,
                         bool startNew, std::string& error)
{
    const std::wstring newPath = SiblingPath(targetExe, kNewSuffix);
    const std::wstring oldPath = SiblingPath(targetExe, kOldSuffix);
    std::error_code ec;
    if (!fs::exists(newPath, ec)) {
        error = "no downloaded Launcher";
        return false;
    }
    fs::remove(oldPath, ec);
    // 実行中の exe は消せないが rename はできる。退避 → 差し込みの順で、失敗したら戻す
    const bool hadTarget = fs::exists(targetExe, ec);
    if (hadTarget && !MoveFileExW(targetExe.c_str(), oldPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        error = "cannot move the current Launcher aside (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    if (!MoveFileExW(newPath.c_str(), targetExe.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        error = "cannot put the new Launcher in place (" + std::to_string(GetLastError()) + ")";
        if (hadTarget) {
            MoveFileExW(oldPath.c_str(), targetExe.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        return false;
    }
    if (NormalizePathKey(targetExe) == NormalizePathKey(PlacedExe(root))) {
        WriteVersionFile(root, newVersion);
    }
    if (startNew) {
        const std::wstring dir = fs::path(targetExe).parent_path().wstring();
        const HINSTANCE r = ShellExecuteW(nullptr, L"open", targetExe.c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL);
        if (reinterpret_cast<intptr_t>(r) <= 32) {
            error = "the new Launcher did not start";
            return false;
        }
    }
    return true;
}

bool RunSetupScript(const std::wstring& installDir, std::string& error)
{
    const std::wstring script = installDir + L"\\Setup.ps1";
    std::error_code ec;
    if (!fs::exists(script, ec)) {
        error = "missing " + WideToUtf8(script);
        return false;
    }
    // 配布先に pwsh があるとは限らないので Windows 標準の PowerShell 5.1 で動かす (Setup.ps1 は 5.1 対応)
    // -NoExit: 終わっても窓を閉じない (どの依存が入らなかったかを読めるように)
    std::wstring args = L"-NoExit -NoProfile -ExecutionPolicy Bypass -File \"" + script + L"\" -NoLaunch";
    // -NoUnblock: Launcher が付けた「インターネット由来」の印を Setup に外させない (自動で印を外す動きは
    // 防御の回避と見なされる)。0.86 の Setup.ps1 はこの引数を知らないので、対応している版にだけ渡す
    if (ScriptSupports(script, "NoUnblock")) {
        args += L" -NoUnblock";
    }
    const HINSTANCE r = ShellExecuteW(nullptr, L"open", L"powershell.exe", args.c_str(), installDir.c_str(),
                                      SW_SHOWNORMAL);
    if (reinterpret_cast<intptr_t>(r) <= 32) {
        error = "powershell.exe did not start";
        return false;
    }
    return true;
}

} // namespace mye
