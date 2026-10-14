//====================================================================================
//                          HostServerProcess.cpp
//  MyEngin/ 秋田蓮音                                                     10/14/2026
//                                          オンラインのホストが自分の PC で動かす Server.exe (子プロセス)
//====================================================================================
#include "Engine/Engine/Online/HostServerProcess.h"

#include <filesystem>
#include <vector>

#include <Windows.h>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Platform/Net/UdpSocket.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {

namespace {

std::wstring Quote(const std::wstring& s)
{
    return L"\"" + s + L"\"";
}

std::wstring BuildCommandLine(const HostServerLaunch& l)
{
    std::wstring cmd = Quote(l.serverExe);
    cmd += L" --hosting local --loopback --exit-when-empty";
    cmd += L" --port " + std::to_wstring(l.port);
    cmd += L" --max-players " + std::to_wstring(l.maxPlayers);
    cmd += L" --net-delay " + std::to_wstring(l.inputDelay);
    if (!l.projectRoot.empty()) {
        cmd += L" --project " + Quote(l.projectRoot);
    }
    if (!l.sceneArgs.empty()) {
        cmd += L" " + l.sceneArgs;
    }
    // configBits に入るものはホストと同じにする (ServerSession が Hello で照合する)
    if (l.synthInput) {
        cmd += L" --synth-input";
    }
    if (!l.useJobs) {
        cmd += L" --no-jobs";
    }
    if (!l.useSimCache) {
        cmd += L" --no-sim-cache";
    }
    if (!l.useCookCache) {
        cmd += L" --no-cook-cache";
    }
    if (l.computeForceCpu) {
        cmd += L" --compute-force-cpu";
    }
    return cmd;
}

} // namespace

HostServerProcess::~HostServerProcess()
{
    Stop();
}

bool HostServerProcess::Start(const HostServerLaunch& launch)
{
    Stop();
    if (!std::filesystem::exists(launch.serverExe)) {
        MYE_LOG_ERROR("[online] Server.exe was not found: %s", WideToUtf8(launch.serverExe).c_str());
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(launch.logPath).parent_path(), ec);

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE log = CreateFileW(launch.logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE nulIn = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);

    // 親 (ホストの Runtime) のハンドルが閉じたら子も終わる。異常終了でサーバだけが残り、ポートと
    // セーブの置き場を掴み続けるのを防ぐ
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = log;
    si.hStdError = log;
    si.hStdInput = nulIn;
    PROCESS_INFORMATION pi = {};
    const std::wstring cmdline = BuildCommandLine(launch);
    std::vector<wchar_t> buffer(cmdline.begin(), cmdline.end());
    buffer.push_back(L'\0'); // CreateProcessW は書込可能バッファを要求する
    const std::wstring workDir = std::filesystem::path(launch.serverExe).parent_path().wstring();
    // CREATE_SUSPENDED: ジョブへ入れてから走らせる (入れる前に子が孫を作ると孫がジョブの外に出る)
    const BOOL ok = CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, workDir.c_str(), &si, &pi);
    const DWORD err = ok ? 0 : GetLastError();
    if (log != INVALID_HANDLE_VALUE) {
        CloseHandle(log);
    }
    if (nulIn != INVALID_HANDLE_VALUE) {
        CloseHandle(nulIn);
    }
    if (!ok) {
        MYE_LOG_ERROR("[online] could not start Server.exe (%lu): %s", err, WideToUtf8(cmdline).c_str());
        if (job != nullptr) {
            CloseHandle(job);
        }
        return false;
    }
    if (job != nullptr && !AssignProcessToJobObject(job, pi.hProcess)) {
        MYE_LOG_WARN("[online] Server.exe could not be put in a kill-on-close job (%lu); it may outlive this "
                     "process if this process crashes", GetLastError());
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    job_ = job;
    port_ = launch.port;
    MYE_LOG_INFO("[online] host server started (pid %lu, 127.0.0.1:%u, log %s)", pi.dwProcessId,
                 static_cast<unsigned>(launch.port), WideToUtf8(launch.logPath).c_str());
    return true;
}

bool HostServerProcess::Exited(uint32_t& exitCode)
{
    exitCode = 0;
    if (process_ == nullptr) {
        return true;
    }
    if (WaitForSingleObject(static_cast<HANDLE>(process_), 0) != WAIT_OBJECT_0) {
        return false;
    }
    DWORD code = 0;
    GetExitCodeProcess(static_cast<HANDLE>(process_), &code);
    exitCode = code;
    return true;
}

bool HostServerProcess::Running()
{
    uint32_t code = 0;
    return process_ != nullptr && !Exited(code);
}

void HostServerProcess::Stop()
{
    if (process_ != nullptr) {
        uint32_t code = 0;
        if (!Exited(code)) {
            TerminateProcess(static_cast<HANDLE>(process_), 1);
            WaitForSingleObject(static_cast<HANDLE>(process_), 2000);
            MYE_LOG_INFO("[online] host server stopped");
        }
        CloseHandle(static_cast<HANDLE>(process_));
        process_ = nullptr;
    }
    if (job_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(job_));
        job_ = nullptr;
    }
    port_ = 0;
}

uint16_t FindFreeLoopbackPort()
{
    UdpSocket probe;
    if (!probe.Open(0, true)) {
        return 0;
    }
    const uint16_t port = probe.LocalPort();
    probe.Close();
    return port;
}

std::wstring DefaultServerExePath()
{
    return GetExecutableDir() + L"\\Server.exe";
}

} // namespace mye
