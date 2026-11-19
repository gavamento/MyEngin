//====================================================================================
//                          TerminalVsEnvWin32.cpp
//  MyEngine/ 秋田蓮音                                                      11/19/2026
//                                          VS 開発者環境の取得・親環境の列挙・エディタの識別 (Win32)
//====================================================================================
#include "Editor/Tools/Terminal/TerminalVsEnv.h"

#include <filesystem>

#include <Windows.h>

#include "Editor/Tools/VisualStudioLauncher.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Platform/PathUtil.h"

namespace fs = std::filesystem;

namespace mye {
namespace {

// VsDevCmd.bat の、VS のインストール先からの相対パス (VS 2017 以降で共通)
constexpr const wchar_t* kVsDevCmdRelativePath = L"\\Common7\\Tools\\VsDevCmd.bat";
// cancel の確認・子の終了待ちの間隔
constexpr DWORD kPollIntervalMs = 15;
// `set` の出力の上限。環境変数全体は数十 KB だが、暴走した出力でメモリを食わないための安全弁
constexpr size_t kMaxOutputBytes = 8u * 1024u * 1024u;

// ハンドルを閉じる RAII。ワーカースレッドの早期 return で漏らさない
struct HandleCloser
{
    HANDLE handle = nullptr;
    HandleCloser() = default;
    explicit HandleCloser(HANDLE h) : handle(h) {}
    HandleCloser(const HandleCloser&) = delete;
    HandleCloser& operator=(const HandleCloser&) = delete;
    ~HandleCloser() { Reset(); }
    void Reset()
    {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
        handle = nullptr;
    }
};

TerminalVsEnvFetchResult MakeResult(TerminalVsEnvStatus status, std::string detail)
{
    TerminalVsEnvFetchResult result;
    result.status = status;
    result.detail = std::move(detail);
    return result;
}

// Job があれば孫ごと、無ければ cmd だけを終わらせる
void TerminateChildren(const HandleCloser& job, const HandleCloser& process)
{
    if (job.handle != nullptr) {
        TerminateJobObject(job.handle, 1);
    } else {
        TerminateProcess(process.handle, 1);
    }
}

std::wstring SystemCmdPath()
{
    wchar_t dir[MAX_PATH] = {};
    const UINT len = GetSystemDirectoryW(dir, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return L"cmd.exe";
    }
    return std::wstring(dir) + L"\\cmd.exe";
}

} // namespace

std::vector<TerminalEnvVar> GetTerminalParentEnvironment()
{
    std::vector<TerminalEnvVar> env;
    wchar_t* block = GetEnvironmentStringsW();
    if (block == nullptr) {
        return env;
    }
    for (const wchar_t* entry = block; *entry != L'\0'; entry += wcslen(entry) + 1) {
        const std::wstring_view text(entry);
        // "=C:=C:\dir" のような隠し変数は名前が '=' で始まる。区切りは 2 文字目以降の最初の '='
        const size_t pos = text.find(L'=', text.front() == L'=' ? 1 : 0);
        if (pos == std::wstring_view::npos || pos == 0) {
            continue;
        }
        env.push_back({ WideToUtf8(text.substr(0, pos)), WideToUtf8(text.substr(pos + 1)) });
    }
    FreeEnvironmentStringsW(block);
    return env;
}

void GetTerminalEditorIdentity(uint32_t& pid, uint64_t& startUnixSeconds)
{
    pid = GetCurrentProcessId();
    startUnixSeconds = 0;
    FILETIME creation = {};
    FILETIME exitTime = {};
    FILETIME kernel = {};
    FILETIME user = {};
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exitTime, &kernel, &user)) {
        // FILETIME は 1601-01-01 からの 100ns 単位。UNIX 時刻 (1970-01-01) との差は 11644473600 秒
        constexpr uint64_t kFileTimeUnitsPerSecond = 10000000ull;
        constexpr uint64_t kUnixEpochInFileTimeSeconds = 11644473600ull;
        const uint64_t ticks = (static_cast<uint64_t>(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
        const uint64_t seconds = ticks / kFileTimeUnitsPerSecond;
        startUnixSeconds = seconds > kUnixEpochInFileTimeSeconds ? seconds - kUnixEpochInFileTimeSeconds : 0;
    }
}

TerminalVsEnvFetchResult FetchTerminalVsDevEnvironment(const std::atomic<bool>& cancel)
{
    const std::wstring installPath = FindLatestVsInstallPath();
    if (installPath.empty()) {
        return MakeResult(TerminalVsEnvStatus::NotInstalled, "vswhere found no Visual Studio installation");
    }
    const std::wstring vsDevCmd = installPath + kVsDevCmdRelativePath;
    std::error_code ec;
    if (!fs::exists(vsDevCmd, ec)) {
        return MakeResult(TerminalVsEnvStatus::NotInstalled, "VsDevCmd.bat not found: " + WideToUtf8(vsDevCmd));
    }
    if (cancel) {
        return MakeResult(TerminalVsEnvStatus::Failed, "canceled");
    }

    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE readRaw = nullptr;
    HANDLE writeRaw = nullptr;
    if (!CreatePipe(&readRaw, &writeRaw, &sa, 0)) {
        return MakeResult(TerminalVsEnvStatus::Failed, "CreatePipe failed (" + std::to_string(GetLastError()) + ")");
    }
    HandleCloser readPipe(readRaw);
    HandleCloser writePipe(writeRaw);
    SetHandleInformation(readPipe.handle, HANDLE_FLAG_INHERIT, 0);   // 子へ渡すのは書き込み口だけ

    // 子の孫 (VsDevCmd が起こす powershell / git など) まで確実に終わらせるため、Job に入れて閉じたときに殺す
    HandleCloser job(CreateJobObjectW(nullptr, nullptr));
    if (job.handle != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job.handle, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }

    // chcp 65001: パイプへ出る `set` の文字コードを UTF-8 に揃える (既定は OEM コードページで、日本語のユーザー名が化ける)。
    // 目印 (echo) は VsDevCmd が成功したときだけ出る。失敗すれば目印が無く、解析が失敗として扱う。
    // /s により cmd は外側の " " を 1 組だけ外して残りをそのまま解釈する
    const std::wstring command = L"chcp 65001>nul && \"" + vsDevCmd + L"\" -arch=x64 -no_logo && echo " + Utf8ToWide(kTerminalVsEnvMarker) + L" && set";
    std::wstring cmdline = L"\"" + SystemCmdPath() + L"\" /s /c \"" + command + L"\"";

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe.handle;
    si.hStdError = writePipe.handle;
    si.hStdInput = INVALID_HANDLE_VALUE;
    PROCESS_INFORMATION pi = {};
    // ジョブへ入れる前に孫を起こされないよう、停止状態で作る。親環境をそのまま継がせる (差分の基準が親環境なので)
    if (!CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
        return MakeResult(TerminalVsEnvStatus::Failed, "CreateProcess failed (" + std::to_string(GetLastError()) + ")");
    }
    HandleCloser process(pi.hProcess);
    HandleCloser thread(pi.hThread);
    writePipe.Reset();   // 親側の書き込み口を閉じる (子と孫が全員閉じたとき読み取りが EOF になる)
    if (job.handle != nullptr) {
        AssignProcessToJobObject(job.handle, process.handle);
    }
    ResumeThread(thread.handle);

    std::string output;
    bool exited = false;
    for (;;) {
        DWORD available = 0;
        const BOOL peeked = PeekNamedPipe(readPipe.handle, nullptr, 0, nullptr, &available, nullptr);
        if (peeked && available > 0) {
            char buffer[16 * 1024];
            DWORD got = 0;
            if (ReadFile(readPipe.handle, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
                output.append(buffer, got);
                if (output.size() > kMaxOutputBytes) {
                    TerminateChildren(job, process);
                    return MakeResult(TerminalVsEnvStatus::Failed, "the output of `set` is unexpectedly large");
                }
            }
            continue;   // 出力が続くあいだは待たずに読む
        }
        if (exited) {
            break;      // 子の終了後に読み残しが無いことを確かめた
        }
        if (cancel) {
            TerminateChildren(job, process);
            return MakeResult(TerminalVsEnvStatus::Failed, "canceled");
        }
        exited = WaitForSingleObject(process.handle, kPollIntervalMs) == WAIT_OBJECT_0;
    }

    std::vector<TerminalEnvVar> vsEnv;
    if (!ParseTerminalSetOutput(output, kTerminalVsEnvMarker, vsEnv)) {
        DWORD exitCode = 0;
        GetExitCodeProcess(process.handle, &exitCode);
        return MakeResult(TerminalVsEnvStatus::Failed, "VsDevCmd.bat did not complete (exit code " + std::to_string(exitCode) + ")");
    }
    TerminalVsEnvFetchResult result;
    result.status = TerminalVsEnvStatus::Ok;
    result.diff = ComputeTerminalEnvironmentDiff(GetTerminalParentEnvironment(), vsEnv);
    result.detail = WideToUtf8(installPath);
    MYE_LOG_INFO("[terminal] Visual Studio developer environment loaded (%zu variables differ): %s", result.diff.size(), result.detail.c_str());
    return result;
}

} // namespace mye
