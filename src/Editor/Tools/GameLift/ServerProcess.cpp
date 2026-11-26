//====================================================================================
//                          ServerProcess.cpp
//  MyEngin/ 秋田蓮音                                                       11/27/2026
//                                          Server.exe を 1 本起動して監視する子プロセスの実装
//====================================================================================
#include "Editor/Tools/GameLift/ServerProcess.h"

#include <algorithm>
#include <filesystem>

#include <Windows.h>

namespace mye {
namespace {

constexpr UINT kKilledExitCode = 1;
constexpr size_t kMaxPartialLineBytes = 64 * 1024; // 改行が来ないまま溜まった分はここで 1 行として切る
constexpr int kMaxReadsPerPoll = 16;               // 1 回の Poll で読む量の上限 (16 KiB x 16)
constexpr DWORD kKillWaitMs = 2000;

// 起動途中の失敗経路でハンドルを閉じ忘れない
struct ScopedHandle
{
    HANDLE handle = nullptr;
    ScopedHandle() = default;
    ~ScopedHandle() { Close(); }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    void Close()
    {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
        handle = nullptr;
    }
    HANDLE Release()
    {
        HANDLE h = handle;
        handle = nullptr;
        return h;
    }
};

} // namespace

void ServerLaunchSpec::WipeSecrets()
{
    for (CliEnvVar& var : extraEnv) {
        if (!var.value.empty()) {
            SecureZeroMemory(var.value.data(), var.value.size() * sizeof(wchar_t));
        }
    }
    extraEnv.clear();
}

ServerProcess::~ServerProcess()
{
    if (started_ && !exited_) {
        Kill();
    }
    ReleaseHandles();
}

void ServerProcess::ReleaseHandles()
{
    // Job を先に閉じる (KILL_ON_JOB_CLOSE で取りこぼしの孫も止まる)
    for (void** handle : { &job_, &process_, &stopEvent_, &logRead_ }) {
        if (*handle != nullptr && *handle != INVALID_HANDLE_VALUE) {
            CloseHandle(static_cast<HANDLE>(*handle));
        }
        *handle = nullptr;
    }
}

void ServerProcess::Kill()
{
    if (job_ != nullptr) {
        TerminateJobObject(static_cast<HANDLE>(job_), kKilledExitCode);
    } else if (process_ != nullptr) {
        TerminateProcess(static_cast<HANDLE>(process_), kKilledExitCode);
    }
}

bool ServerProcess::Start(const ServerLaunchSpec& spec, std::string& error)
{
    if (started_ && !exited_) {
        error = "the server process is already running";
        return false;
    }
    ReleaseHandles();
    started_ = exited_ = ready_ = stopRequested_ = forcedKill_ = false;
    exitCode_ = 0;
    partialLine_.clear();
    tail_.clear();
    logPath_ = spec.logPath;

    if (spec.exePath.empty() || spec.logPath.empty()) {
        error = "exe path or log path is empty";
        return false;
    }

    // 停止イベントは子の起動の前に作る (子が --stop-event で開くとき、無ければ exit 1 になる)
    ScopedHandle stopEvent;
    if (!spec.stopEventName.empty()) {
        stopEvent.handle = CreateEventW(nullptr, TRUE, FALSE, spec.stopEventName.c_str());
        if (stopEvent.handle == nullptr) {
            error = "CreateEvent failed (" + std::to_string(GetLastError()) + ")";
            return false;
        }
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            error = "the stop event name is already in use";
            return false;
        }
    }

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(spec.logPath).parent_path(), ec);

    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;

    // 子へ渡す書き込み端。読み手 (この Poll・tail) が同時に開けるよう共有を許す
    ScopedHandle logWrite;
    logWrite.handle = CreateFileW(spec.logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  &inheritable, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (logWrite.handle == INVALID_HANDLE_VALUE) {
        logWrite.handle = nullptr;
        error = "could not create the log file (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    ScopedHandle logReadEnd;
    logReadEnd.handle = CreateFileW(spec.logPath.c_str(), GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (logReadEnd.handle == INVALID_HANDLE_VALUE) {
        logReadEnd.handle = nullptr;
        error = "could not open the log file for reading (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    ScopedHandle nulIn;
    nulIn.handle = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    if (nulIn.handle == INVALID_HANDLE_VALUE) {
        nulIn.handle = nullptr;
        error = "open NUL failed (" + std::to_string(GetLastError()) + ")";
        return false;
    }

    // 環境ブロックとコマンドラインは秘密 (トークン) を含みうる。起動したらゼロで埋める
    std::wstring envBlock = BuildEnvironmentBlock(ReadParentEnvironment(), spec.extraEnv);
    std::wstring commandLine = BuildCommandLine(spec.exePath, spec.args);
    commandLine.push_back(L'\0'); // CreateProcessW は書き換えうるので可変バッファで渡す
    const auto wipe = [&]() {
        SecureZeroMemory(envBlock.data(), envBlock.size() * sizeof(wchar_t));
        SecureZeroMemory(commandLine.data(), commandLine.size() * sizeof(wchar_t));
    };

    // 継がせるハンドルを NUL とログの 2 本に限る (CliProcess と同じ理由: 別の子へ漏らさない)
    HANDLE inheritList[2] = { nulIn.handle, logWrite.handle };
    SIZE_T attrBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrBytes);
    std::vector<char> attrStorage(attrBytes);
    auto* attrList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrStorage.data());
    if (!InitializeProcThreadAttributeList(attrList, 1, 0, &attrBytes) ||
        !UpdateProcThreadAttribute(attrList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inheritList, sizeof(inheritList),
                                   nullptr, nullptr)) {
        error = "attribute list setup failed (" + std::to_string(GetLastError()) + ")";
        wipe();
        return false;
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = nulIn.handle;
    startup.StartupInfo.hStdOutput = logWrite.handle;
    startup.StartupInfo.hStdError = logWrite.handle;
    startup.lpAttributeList = attrList;

    // SUSPENDED で作って Job へ入れてから動かす (入れる前に子が孫を作る隙を無くす)
    PROCESS_INFORMATION info{};
    const BOOL created =
        CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED,
                       envBlock.data(), spec.workDir.empty() ? nullptr : spec.workDir.c_str(), &startup.StartupInfo,
                       &info);
    const DWORD createError = GetLastError();
    DeleteProcThreadAttributeList(attrList);
    wipe();
    if (!created) {
        error = "CreateProcess failed (" + std::to_string(createError) + ")";
        return false;
    }

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
            !AssignProcessToJobObject(job, info.hProcess)) {
            CloseHandle(job);
            job = nullptr; // Job が使えなければ本体だけを止める (Kill が TerminateProcess へ落ちる)
        }
    }
    ResumeThread(info.hThread);
    CloseHandle(info.hThread);

    process_ = info.hProcess;
    job_ = job;
    stopEvent_ = stopEvent.Release();
    logRead_ = logReadEnd.Release();
    started_ = true;
    return true; // 子側の書き込み端 (logWrite) と NUL はデストラクタが閉じる
}

bool ServerProcess::ProcessExited()
{
    if (exited_) {
        return true;
    }
    if (process_ == nullptr || WaitForSingleObject(static_cast<HANDLE>(process_), 0) != WAIT_OBJECT_0) {
        return false;
    }
    DWORD code = 0;
    GetExitCodeProcess(static_cast<HANDLE>(process_), &code);
    exitCode_ = code;
    exited_ = true;
    return true;
}

void ServerProcess::AppendLogText(const char* data, size_t size)
{
    for (size_t i = 0; i < size; ++i) {
        const char c = data[i];
        if (c != '\n') {
            partialLine_.push_back(c);
            if (partialLine_.size() < kMaxPartialLineBytes) {
                continue;
            }
        }
        if (!partialLine_.empty() && partialLine_.back() == '\r') {
            partialLine_.pop_back();
        }
        if (partialLine_.find(kServerReadyMarker) != std::string::npos) {
            ready_ = true;
        }
        tail_.push_back(std::move(partialLine_));
        partialLine_.clear();
        if (tail_.size() > kServerLogTailLines) {
            tail_.pop_front();
        }
    }
}

void ServerProcess::ReadLog()
{
    if (logRead_ == nullptr) {
        return;
    }
    char buffer[16 * 1024];
    for (int i = 0; i < kMaxReadsPerPoll; ++i) {
        DWORD got = 0;
        if (!ReadFile(static_cast<HANDLE>(logRead_), buffer, sizeof(buffer), &got, nullptr) || got == 0) {
            break;
        }
        AppendLogText(buffer, got);
    }
}

void ServerProcess::Poll()
{
    if (!started_) {
        return;
    }
    const bool wasExited = exited_;
    ReadLog();
    if (ProcessExited() && !wasExited) {
        ReadLog(); // 終了までに書かれた分を取り切る
        ReadLog();
        if (!partialLine_.empty()) {
            AppendLogText("\n", 1); // 改行なしで終わった最後の行を確定する
        }
        return;
    }
    if (!exited_ && stopRequested_ && GetTickCount64() - stopTick_ >= graceMs_) {
        forcedKill_ = true;
        Kill(); // 終了は次の Poll で見る
    }
}

void ServerProcess::RequestStop(uint32_t graceMs)
{
    if (!started_ || ProcessExited() || stopRequested_) {
        return;
    }
    stopRequested_ = true;
    graceMs_ = graceMs;
    stopTick_ = GetTickCount64();
    if (stopEvent_ != nullptr) {
        SetEvent(static_cast<HANDLE>(stopEvent_));
    } else {
        graceMs_ = 0; // 止める手段が強制終了しかない
    }
}

void ServerProcess::StopBlocking(uint32_t graceMs)
{
    if (!started_ || ProcessExited()) {
        return;
    }
    RequestStop(graceMs);
    if (WaitForSingleObject(static_cast<HANDLE>(process_), graceMs_) != WAIT_OBJECT_0) {
        forcedKill_ = true;
        Kill();
        WaitForSingleObject(static_cast<HANDLE>(process_), kKillWaitMs);
    }
    Poll();
}

ServerView ServerProcess::View() const
{
    ServerView view;
    view.logPath = logPath_;
    view.exitCode = exitCode_;
    view.forcedKill = forcedKill_;
    view.logTail.assign(tail_.begin(), tail_.end());
    if (!started_) {
        view.phase = ServerPhase::NotStarted;
    } else if (exited_) {
        view.phase = ServerPhase::Exited;
    } else if (stopRequested_) {
        view.phase = ServerPhase::Stopping;
    } else if (ready_) {
        view.phase = ServerPhase::Ready;
    } else {
        view.phase = ServerPhase::Starting;
    }
    return view;
}

} // namespace mye
