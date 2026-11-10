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

std::wstring SiblingPath(const std::wstring& root, const wchar_t* suffix)
{
    // MyEngineLauncher.exe → MyEngineLauncher.new.exe
    std::wstring name = kLauncherExeName;
    name = name.substr(0, name.size() - 4) + suffix;
    return LauncherDir(root) + L"\\" + name;
}

std::wstring RunningExePath()
{
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return buf;
}

bool WriteVersionFile(const std::wstring& root, const std::string& version)
{
    nlohmann::json j;
    j["version"] = version;
    return WriteFileReplacing(LauncherDir(root) + L"\\" + kVersionFile, j.dump(2));
}

bool CreateStartMenuShortcut(const std::wstring& target)
{
    PWSTR programs = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &programs))) {
        return false;
    }
    const std::wstring linkPath = std::wstring(programs) + L"\\" + kShortcutName;
    CoTaskMemFree(programs);

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

void PlaceLauncherCopy(const std::wstring& root, bool createShortcut)
{
    const std::wstring running = RunningExePath();
    const std::wstring placed = PlacedExe(root);
    if (NormalizePathKey(running) == NormalizePathKey(placed)) {
        return; // もう置き場から動いている
    }
    const EngineVersion current = CurrentEngineVersion();
    const EngineVersion existing = PlacedLauncherVersion(root);
    std::error_code ec;
    if (fs::exists(placed, ec) && existing.valid && CompareEngineVersion(existing, current) > 0) {
        return; // 置いてある方が新しい (古い Launcher を落とし直して開いた場合)
    }
    fs::create_directories(LauncherDir(root), ec);
    // 置き場の Launcher が動いていると上書きできない。そのときは今回は諦める (次に開いたときに置く)
    if (!CopyFileW(running.c_str(), placed.c_str(), FALSE)) {
        MYE_LOG_WARN("launcher: could not place a copy at %s (%lu)", WideToUtf8(placed).c_str(), GetLastError());
        return;
    }
    WriteVersionFile(root, current.text);
    if (createShortcut && !CreateStartMenuShortcut(placed)) {
        MYE_LOG_WARN("launcher: could not create the Start menu shortcut");
    }
    MYE_LOG_INFO("launcher: placed a copy at %s", WideToUtf8(placed).c_str());
}

void CleanupOldLauncherFiles(const std::wstring& root)
{
    std::error_code ec;
    fs::remove(SiblingPath(root, kOldSuffix), ec);
    fs::remove(SiblingPath(root, kNewSuffix), ec);
}

bool LauncherUpdateAvailable(const ReleaseInfo& release)
{
    const std::string hashName = std::string(kLauncherAssetName) + ".sha256";
    return release.version.valid && CompareEngineVersion(release.version, CurrentEngineVersion()) > 0
        && FindReleaseAsset(release, kLauncherAssetName) && FindReleaseAsset(release, hashName);
}

InstallOutcome DownloadLauncherUpdate(const ReleaseSource& source, const ReleaseInfo& release, const std::wstring& root,
                                      const InstallProgressFn& progress, const std::atomic<bool>* cancel)
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
    fs::create_directories(LauncherDir(root), ec);
    const std::wstring newPath = SiblingPath(root, kNewSuffix);
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

bool ApplyLauncherUpdate(const std::wstring& root, const std::string& newVersion, bool startNew, std::string& error)
{
    const std::wstring placed = PlacedExe(root);
    const std::wstring newPath = SiblingPath(root, kNewSuffix);
    const std::wstring oldPath = SiblingPath(root, kOldSuffix);
    std::error_code ec;
    if (!fs::exists(newPath, ec)) {
        error = "no downloaded Launcher";
        return false;
    }
    fs::remove(oldPath, ec);
    // 実行中の exe は消せないが rename はできる。退避 → 差し込みの順で、失敗したら戻す
    const bool hadPlaced = fs::exists(placed, ec);
    if (hadPlaced && !MoveFileExW(placed.c_str(), oldPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        error = "cannot move the current Launcher aside (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    if (!MoveFileExW(newPath.c_str(), placed.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        error = "cannot put the new Launcher in place (" + std::to_string(GetLastError()) + ")";
        if (hadPlaced) {
            MoveFileExW(oldPath.c_str(), placed.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        return false;
    }
    WriteVersionFile(root, newVersion);
    if (startNew) {
        const HINSTANCE r = ShellExecuteW(nullptr, L"open", placed.c_str(), nullptr, LauncherDir(root).c_str(),
                                          SW_SHOWNORMAL);
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
    const std::wstring args = L"-NoExit -NoProfile -ExecutionPolicy Bypass -File \"" + script + L"\" -NoLaunch";
    const HINSTANCE r = ShellExecuteW(nullptr, L"open", L"powershell.exe", args.c_str(), installDir.c_str(),
                                      SW_SHOWNORMAL);
    if (reinterpret_cast<intptr_t>(r) <= 32) {
        error = "powershell.exe did not start";
        return false;
    }
    return true;
}

} // namespace mye
