//====================================================================================
//                          BrowserCatalog.cpp
//  MyEngine/ 秋田蓮音                                                      11/18/2026
//                                          インストール済みブラウザの検出の実装
//====================================================================================
#include "Editor/Tools/Browser/BrowserCatalog.h"

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <filesystem>

#include <Windows.h>

#include "Engine/Platform/PathUtil.h"

namespace mye {
namespace {

std::wstring ToLower(std::wstring s)
{
    for (wchar_t& c : s) {
        c = static_cast<wchar_t>(std::towlower(c));
    }
    return s;
}

std::wstring FileNameLower(const std::wstring& path)
{
    const size_t pos = path.find_last_of(L"\\/");
    return ToLower(pos == std::wstring::npos ? path : path.substr(pos + 1));
}

// 重複判定用の正規化。存在しない exe でも比べられるよう、ファイルシステムは見ずに文字列だけで揃える
std::wstring DedupKey(const std::wstring& exePath)
{
    std::wstring key = ToLower(exePath);
    std::replace(key.begin(), key.end(), L'/', L'\\');
    return key;
}

// REG_SZ の値を読む。読めなければ空。view = KEY_WOW64_64KEY / KEY_WOW64_32KEY (WOW6432Node)
std::wstring ReadRegString(HKEY root, const std::wstring& subKey, const wchar_t* valueName, REGSAM view)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subKey.c_str(), 0, KEY_READ | view, &key) != ERROR_SUCCESS) {
        return std::wstring();
    }
    wchar_t buf[2048] = {};
    DWORD size = sizeof(buf) - sizeof(wchar_t);
    DWORD type = 0;
    std::wstring out;
    if (RegQueryValueExW(key, valueName, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &size) == ERROR_SUCCESS
        && (type == REG_SZ || type == REG_EXPAND_SZ)) {
        out.assign(buf, size / sizeof(wchar_t));
        while (!out.empty() && out.back() == L'\0') {
            out.pop_back();
        }
    }
    RegCloseKey(key);
    return out;
}

void ReadHive(HKEY root, const std::wstring& clientsKey, REGSAM view, std::vector<BrowserRawEntry>& out)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, clientsKey.c_str(), 0, KEY_READ | KEY_ENUMERATE_SUB_KEYS | view, &key) != ERROR_SUCCESS) {
        return;
    }
    // 列挙順はレジストリ依存。並びは BuildBrowserCandidates が決めるので、ここでは読んだ順のまま渡す
    for (DWORD index = 0;; ++index) {
        wchar_t name[256] = {};
        DWORD nameLen = 256;
        if (RegEnumKeyExW(key, index, name, &nameLen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
            break;
        }
        const std::wstring base = clientsKey + L"\\" + name;
        BrowserRawEntry entry;
        entry.id = name;
        entry.displayName = ReadRegString(root, base, nullptr, view);
        entry.command = ReadRegString(root, base + L"\\shell\\open\\command", nullptr, view);
        entry.httpsProgId = ReadRegString(root, base + L"\\Capabilities\\URLAssociations", L"https", view);
        out.push_back(std::move(entry));
    }
    RegCloseKey(key);
}

const char* FamilyName(BrowserFamily f)
{
    switch (f) {
    case BrowserFamily::Chromium: return "chromium";
    case BrowserFamily::Firefox: return "firefox";
    default: return "unknown";
    }
}

} // namespace

std::wstring ExtractBrowserExePath(const std::wstring& command)
{
    const size_t begin = command.find_first_not_of(L" \t");
    if (begin == std::wstring::npos) {
        return std::wstring();
    }
    if (command[begin] == L'"') {
        const size_t close = command.find(L'"', begin + 1);
        if (close == std::wstring::npos) {
            return std::wstring();
        }
        return command.substr(begin + 1, close - begin - 1);
    }
    // 引用符なし: 最初の ".exe" まで。後ろの引数・%1 は捨てる
    const std::wstring lower = ToLower(command);
    const size_t ext = lower.find(L".exe", begin);
    if (ext == std::wstring::npos) {
        return std::wstring();
    }
    return command.substr(begin, ext + 4 - begin);
}

BrowserFamily ClassifyBrowserFamily(const std::wstring& exePath)
{
    static const wchar_t* const kChromium[] = {
        L"chrome.exe", L"msedge.exe", L"brave.exe", L"vivaldi.exe", L"opera.exe", L"chromium.exe",
        L"iridium.exe", L"yandex.exe",
    };
    static const wchar_t* const kFirefox[] = {
        L"firefox.exe", L"librewolf.exe", L"waterfox.exe", L"floorp.exe", L"zen.exe", L"palemoon.exe",
    };
    const std::wstring name = FileNameLower(exePath);
    for (const wchar_t* n : kChromium) {
        if (name == n) {
            return BrowserFamily::Chromium;
        }
    }
    for (const wchar_t* n : kFirefox) {
        if (name == n) {
            return BrowserFamily::Firefox;
        }
    }
    return BrowserFamily::Unknown;
}

std::wstring BuildBrowserLaunchArgs(BrowserFamily family, const std::wstring& url)
{
    // URL は 1 つの引数として引用符で包む (空白や & を含んでも 1 引数のまま)
    const std::wstring quoted = url.empty() ? std::wstring() : (L"\"" + url + L"\"");
    switch (family) {
    case BrowserFamily::Chromium:
        return quoted.empty() ? std::wstring(L"--new-window") : (L"--new-window " + quoted);
    case BrowserFamily::Firefox:
        return quoted.empty() ? std::wstring(L"-new-window") : (L"-new-window " + quoted);
    default:
        return quoted;
    }
}

std::vector<BrowserCandidate> BuildBrowserCandidates(const std::vector<BrowserRawEntry>& raw,
                                                     const std::wstring& defaultProgId,
                                                     const std::function<bool(const std::wstring&)>& exeExists)
{
    std::vector<BrowserCandidate> out;
    std::vector<std::wstring> seen;
    for (const BrowserRawEntry& e : raw) {
        const std::wstring exe = ExtractBrowserExePath(e.command);
        if (exe.empty() || FileNameLower(exe) == L"iexplore.exe" || !exeExists(exe)) {
            continue;
        }
        const std::wstring key = DedupKey(exe);
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) {
            continue; // 先に読んだもの (HKCU → HKLM → WOW) を残す
        }
        seen.push_back(key);
        BrowserCandidate c;
        c.id = e.id;
        c.displayName = e.displayName.empty() ? e.id : e.displayName;
        c.exePath = exe;
        c.family = ClassifyBrowserFamily(exe);
        c.isDefault = !defaultProgId.empty() && !e.httpsProgId.empty() && e.httpsProgId == defaultProgId;
        out.push_back(std::move(c));
    }
    std::stable_sort(out.begin(), out.end(), [](const BrowserCandidate& a, const BrowserCandidate& b) {
        const std::wstring an = ToLower(a.displayName);
        const std::wstring bn = ToLower(b.displayName);
        if (an != bn) {
            return an < bn;
        }
        return DedupKey(a.exePath) < DedupKey(b.exePath);
    });
    return out;
}

std::vector<BrowserCandidate> DetectInstalledBrowsers()
{
    const std::wstring clients = L"SOFTWARE\\Clients\\StartMenuInternet";
    std::vector<BrowserRawEntry> raw;
    ReadHive(HKEY_CURRENT_USER, clients, KEY_WOW64_64KEY, raw);
    ReadHive(HKEY_LOCAL_MACHINE, clients, KEY_WOW64_64KEY, raw);
    ReadHive(HKEY_LOCAL_MACHINE, clients, KEY_WOW64_32KEY, raw); // WOW6432Node
    const std::wstring defaultProgId = ReadRegString(
        HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\Shell\\Associations\\UrlAssociations\\https\\UserChoice",
        L"ProgId", 0);
    return BuildBrowserCandidates(raw, defaultProgId, [](const std::wstring& exe) {
        std::error_code ec;
        return std::filesystem::is_regular_file(std::filesystem::path(exe), ec);
    });
}

int RunBrowserListCli()
{
    const std::vector<BrowserCandidate> list = DetectInstalledBrowsers();
    std::printf("[browser-list] %zu candidate(s)\n", list.size());
    for (const BrowserCandidate& c : list) {
        std::printf("[browser-list] id='%s' name='%s' exe='%s' family=%s default=%d\n", WideToUtf8(c.id).c_str(),
                    WideToUtf8(c.displayName).c_str(), WideToUtf8(c.exePath).c_str(), FamilyName(c.family),
                    c.isDefault ? 1 : 0);
    }
    std::fflush(stdout);
    return 0;
}

} // namespace mye
