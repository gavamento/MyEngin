//====================================================================================
//                          TrackLaunch.cpp
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          追跡サービスの探索と切り離し起動 (ハンドルを継がせない)
//====================================================================================
#include "Editor/ChangeTracking/TrackLaunch.h"

#include <filesystem>
#include <vector>

#include <Windows.h>

namespace mye {

namespace {

// 起動時のウィンドウ・Job の扱い。CREATE_BREAKAWAY_FROM_JOB は Job が許さないと ERROR_ACCESS_DENIED になる
constexpr DWORD kBaseFlags = CREATE_NO_WINDOW;

} // namespace

std::wstring FindTrackExe(const std::wstring& exeDir)
{
    std::error_code ec;
    const DWORD size = GetEnvironmentVariableW(kTrackExeEnvVar, nullptr, 0);
    if (size > 1) {
        std::wstring value(size, L'\0');
        const DWORD written = GetEnvironmentVariableW(kTrackExeEnvVar, value.data(), size);
        value.resize(written);
        // 設定されているときはそれだけを使う (検証でサービスの不在を決定的に作るため)
        return std::filesystem::is_regular_file(value, ec) ? value : std::wstring();
    }
    const std::filesystem::path beside = std::filesystem::path(exeDir) / L"mye_track.exe";
    return std::filesystem::is_regular_file(beside, ec) ? beside.wstring() : std::wstring();
}

std::wstring QuoteCommandLineArg(const std::wstring& arg)
{
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (const wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\'); // 直前の '\' を二重にして '"' を escape する
        } else {
            out.append(backslashes, L'\\');
        }
        backslashes = 0;
        out.push_back(c);
    }
    out.append(backslashes * 2, L'\\'); // 閉じの '"' の直前
    out.push_back(L'"');
    return out;
}

bool LaunchTrackService(const std::wstring& exePath, const std::wstring& projectRoot, uint32_t* errorOut)
{
    std::wstring cmdline = QuoteCommandLineArg(exePath) + L" serve --project " + QuoteCommandLineArg(projectRoot);
    std::vector<wchar_t> buffer(cmdline.begin(), cmdline.end());
    buffer.push_back(L'\0'); // CreateProcessW は書込可能バッファを要求する
    const std::wstring workDir = std::filesystem::path(exePath).parent_path().wstring();

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    // bInheritHandles = FALSE: Editor のハンドルを 1 つも子へ渡さない
    BOOL ok = CreateProcessW(exePath.c_str(), buffer.data(), nullptr, nullptr, FALSE,
                             kBaseFlags | CREATE_BREAKAWAY_FROM_JOB, nullptr,
                             workDir.empty() ? nullptr : workDir.c_str(), &si, &pi);
    if (!ok && GetLastError() == ERROR_ACCESS_DENIED) {
        // Editor が BREAKAWAY を許さない Job の中で動いている。Job に残ったまま起動し直す
        ok = CreateProcessW(exePath.c_str(), buffer.data(), nullptr, nullptr, FALSE, kBaseFlags, nullptr,
                            workDir.empty() ? nullptr : workDir.c_str(), &si, &pi);
    }
    if (!ok) {
        if (errorOut != nullptr) {
            *errorOut = GetLastError();
        }
        return false;
    }
    // 待たない・Job に入れない。ハンドルは持たずに閉じる
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

} // namespace mye
