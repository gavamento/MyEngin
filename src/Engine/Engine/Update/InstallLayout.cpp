//====================================================================================
//                          InstallLayout.cpp
//  MyEngine/ 秋田蓮音                                                      11/10/2026
//                                          Launcher が入れた配布版の置き場と印の実装
//====================================================================================
#include "Engine/Engine/Update/InstallLayout.h"

#include <algorithm>
#include <cstdlib>
#include <cwchar>
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

constexpr const wchar_t* kEngineDirsFile = L"engine_dirs.json";

// 比較用の正規化: 絶対パス化・. と .. の除去・末尾の区切りを外す
std::wstring NormalizeDir(const std::wstring& dir)
{
    std::error_code ec;
    fs::path p = fs::absolute(fs::path(dir), ec);
    if (ec) {
        p = fs::path(dir);
    }
    std::wstring s = p.lexically_normal().wstring();
    while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/')) {
        s.pop_back();
    }
    return s;
}

struct EngineDirsRecord
{
    std::vector<std::wstring> dirs; // 既定の <root>\Engine は含めない
    std::wstring last;
};

EngineDirsRecord ReadEngineDirs(const std::wstring& root)
{
    EngineDirsRecord rec;
    std::ifstream f(fs::path(root) / kEngineDirsFile, std::ios::binary);
    if (!f) {
        return rec;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    const nlohmann::json j = nlohmann::json::parse(ss.str(), nullptr, false);
    if (!j.is_object()) {
        return rec;
    }
    const auto dirs = j.find("dirs");
    if (dirs != j.end() && dirs->is_array()) {
        for (const nlohmann::json& d : *dirs) {
            if (d.is_string() && !d.get<std::string>().empty()) {
                rec.dirs.push_back(Utf8ToWide(d.get<std::string>()));
            }
        }
    }
    rec.last = Utf8ToWide(j.value("last", std::string()));
    return rec;
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

bool SameEnginesDir(const std::wstring& a, const std::wstring& b)
{
    return _wcsicmp(NormalizeDir(a).c_str(), NormalizeDir(b).c_str()) == 0;
}

std::vector<std::wstring> KnownEnginesDirs(const std::wstring& root)
{
    std::vector<std::wstring> out{ EnginesDir(root) };
    for (const std::wstring& d : ReadEngineDirs(root).dirs) {
        const bool known = std::any_of(out.begin(), out.end(), [&](const std::wstring& o) { return SameEnginesDir(o, d); });
        if (!known) {
            out.push_back(NormalizeDir(d));
        }
    }
    return out;
}

std::wstring PreferredEnginesDir(const std::wstring& root)
{
    const EngineDirsRecord rec = ReadEngineDirs(root);
    return rec.last.empty() ? EnginesDir(root) : NormalizeDir(rec.last);
}

bool RememberEnginesDir(const std::wstring& root, const std::wstring& dir)
{
    EngineDirsRecord rec = ReadEngineDirs(root);
    const std::wstring normalized = NormalizeDir(dir);
    const bool isDefault = SameEnginesDir(normalized, EnginesDir(root));
    const bool known = std::any_of(rec.dirs.begin(), rec.dirs.end(),
                                   [&](const std::wstring& d) { return SameEnginesDir(d, normalized); });
    if (!isDefault && !known) {
        rec.dirs.push_back(normalized);
    }
    rec.last = normalized;

    nlohmann::json j;
    j["dirs"] = nlohmann::json::array();
    for (const std::wstring& d : rec.dirs) {
        j["dirs"].push_back(WideToUtf8(d));
    }
    j["last"] = WideToUtf8(rec.last);
    std::error_code ec;
    fs::create_directories(root, ec);
    return WriteFileReplacing((fs::path(root) / kEngineDirsFile).wstring(), j.dump(2));
}

std::vector<InstalledEngine> ScanInstalledEngines(const std::wstring& root)
{
    std::vector<InstalledEngine> out;
    for (const std::wstring& enginesDir : KnownEnginesDirs(root)) {
        // 取り外したドライブなどで見えない置き場は黙って飛ばす (記録は残し、戻れば再び一覧に出る)
        std::error_code ec;
        for (fs::directory_iterator it(enginesDir, ec), end; !ec && it != end; it.increment(ec)) {
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
    }
    // 同じ版が複数の置き場にあれば、置き場の一覧の順 (既定が先) で並べる
    std::stable_sort(out.begin(), out.end(), [](const InstalledEngine& a, const InstalledEngine& b) {
        return CompareEngineVersion(a.version, b.version) > 0;
    });
    return out;
}

bool IsInsideInstalledEngine(const std::wstring& path)
{
    std::error_code ec;
    for (fs::path p = fs::path(NormalizeDir(path)); !p.empty(); p = p.parent_path()) {
        if (fs::exists(p / kInstallMarkerFile, ec)) {
            return true;
        }
        if (p == p.root_path()) {
            break;
        }
    }
    return false;
}

bool IsInstallWorkFolderName(const std::wstring& name)
{
    for (const wchar_t* infix : { kStagingInfix, kDeletingInfix }) {
        const size_t at = name.find(infix);
        if (at == std::wstring::npos || at == 0) {
            continue;
        }
        const std::wstring pid = name.substr(at + wcslen(infix));
        const bool pidIsNumber = !pid.empty()
            && std::all_of(pid.begin(), pid.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
        EngineVersion v;
        if (pidIsNumber && ParseEngineVersion(WideToUtf8(name.substr(0, at)), v)) {
            return true;
        }
    }
    return false;
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
