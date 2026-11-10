//====================================================================================
//                          InstallLayout.cpp
//  MyEngine/ 秋田蓮音                                                      11/10/2026
//                                          Launcher が入れた配布版の置き場と印の実装
//====================================================================================
#include "Engine/Engine/Update/InstallLayout.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "nlohmann/json.hpp"

#include "Engine/Platform/PathUtil.h"

namespace mye {

namespace {

namespace fs = std::filesystem;

std::wstring ReadEnv(const wchar_t* name)
{
    wchar_t* value = nullptr;
    size_t len = 0;
    std::wstring out;
    if (_wdupenv_s(&value, &len, name) == 0 && value) {
        out = value;
        free(value);
    }
    return out;
}

} // namespace

std::wstring InstallRoot()
{
    const std::wstring overrideRoot = ReadEnv(kInstallRootEnv);
    if (!overrideRoot.empty()) {
        return overrideRoot;
    }
    // EditorGlobalSettings の MachineLocalDir と同じ場所 (editor_global.json の隣)
    const std::wstring localAppData = ReadEnv(L"LOCALAPPDATA");
    return (localAppData.empty() ? GetExecutableDir() : localAppData) + L"\\MyEngine";
}

std::wstring EnginesDir(const std::wstring& root)
{
    return root + L"\\Engine";
}

std::wstring LauncherDir(const std::wstring& root)
{
    return root + L"\\Launcher";
}

std::wstring DownloadsDir(const std::wstring& root)
{
    return root + L"\\Downloads";
}

std::wstring InstalledBinDir(const std::wstring& installDir)
{
    return installDir + L"\\bin\\x64\\Release";
}

std::wstring InstalledEditorPath(const std::wstring& installDir)
{
    return InstalledBinDir(installDir) + L"\\Editor.exe";
}

bool ReadInstallMarker(const std::wstring& installDir, InstallMarker& out)
{
    out = InstallMarker{};
    std::ifstream f(fs::path(installDir) / kInstallMarkerFile, std::ios::binary);
    if (!f) {
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    const nlohmann::json root = nlohmann::json::parse(ss.str(), nullptr, false);
    if (!root.is_object()) {
        return false;
    }
    out.version = root.value("version", std::string());
    out.tag = root.value("tag", std::string());
    out.sha256 = root.value("sha256", std::string());
    out.installedAt = root.value("installedAt", std::string());
    return !out.version.empty();
}

bool WriteInstallMarker(const std::wstring& installDir, const InstallMarker& marker)
{
    nlohmann::json root;
    root["version"] = marker.version;
    root["tag"] = marker.tag;
    root["sha256"] = marker.sha256;
    root["installedAt"] = marker.installedAt;
    return WriteFileReplacing((fs::path(installDir) / kInstallMarkerFile).wstring(), root.dump(2));
}

std::vector<InstalledEngine> ScanInstalledEngines(const std::wstring& root)
{
    std::vector<InstalledEngine> out;
    std::error_code ec;
    for (fs::directory_iterator it(EnginesDir(root), ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) {
            continue;
        }
        const std::wstring name = it->path().filename().wstring();
        if (name.find(kStagingInfix) != std::wstring::npos || name.find(kDeletingInfix) != std::wstring::npos) {
            continue;
        }
        InstalledEngine e;
        e.dir = it->path().wstring();
        if (!ReadInstallMarker(e.dir, e.marker) || !ParseEngineVersion(e.marker.version, e.version)
            || Utf8ToWide(e.version.text) != name) {
            continue; // 印の無いフォルダや名前と版が食い違うフォルダは Launcher の管理外として触らない
        }
        out.push_back(std::move(e));
    }
    std::sort(out.begin(), out.end(), [](const InstalledEngine& a, const InstalledEngine& b) {
        return CompareEngineVersion(a.version, b.version) > 0;
    });
    return out;
}

bool RecordLauncherLocation(const std::wstring& root, const std::wstring& launcherExe)
{
    nlohmann::json j;
    j["path"] = WideToUtf8(launcherExe);
    std::error_code ec;
    fs::create_directories(root, ec);
    return WriteFileReplacing((fs::path(root) / L"launcher_location.json").wstring(), j.dump(2));
}

std::wstring RecordedLauncherPath(const std::wstring& root)
{
    std::ifstream f(fs::path(root) / L"launcher_location.json", std::ios::binary);
    if (!f) {
        return {};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    const nlohmann::json j = nlohmann::json::parse(ss.str(), nullptr, false);
    if (!j.is_object()) {
        return {};
    }
    const std::wstring path = Utf8ToWide(j.value("path", std::string()));
    std::error_code ec;
    return !path.empty() && fs::exists(path, ec) ? path : std::wstring();
}

std::wstring FindManagedInstallDir(const std::wstring& exeDir)
{
    // <install>\bin\x64\Release の 3 段上
    const fs::path install = fs::path(exeDir).parent_path().parent_path().parent_path();
    std::error_code ec;
    if (install.empty() || !fs::exists(install / kInstallMarkerFile, ec)) {
        return {};
    }
    return install.wstring();
}

} // namespace mye
