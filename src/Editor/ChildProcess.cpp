#include "Editor/ChildProcess.h"

#include <vector>

#include <Windows.h>

#include "Engine/Core/Log.h"

namespace mye {

void* StartChildProcess(const std::wstring& cmdline, const std::wstring& workDir, const std::wstring& logPath,
                        const char* logTag, const char* forWhat)
{
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE log = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE nulIn = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = log;
    si.hStdError = log;
    si.hStdInput = nulIn;
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> buffer(cmdline.begin(), cmdline.end());
    buffer.push_back(L'\0'); // CreateProcessW は書込可能バッファを要求する
    const BOOL ok = CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                   workDir.c_str(), &si, &pi);
    if (log != INVALID_HANDLE_VALUE) {
        CloseHandle(log); // 子が継承済み — 親側は即クローズでよい
    }
    if (nulIn != INVALID_HANDLE_VALUE) {
        CloseHandle(nulIn);
    }
    if (!ok) {
        MYE_LOG_ERROR("%s CreateProcess failed%s (%lu)", logTag, forWhat, GetLastError());
        return nullptr;
    }
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

bool PollChildProcess(void* process, uint32_t& exitCode)
{
    if (WaitForSingleObject(process, 0) != WAIT_OBJECT_0) {
        return false;
    }
    DWORD code = 1;
    GetExitCodeProcess(process, &code);
    exitCode = code;
    return true;
}

void CloseChildProcess(void* process)
{
    if (process != nullptr) {
        CloseHandle(process);
    }
}

} // namespace mye
