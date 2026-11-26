//====================================================================================
//                          CliProcess.cpp
//  MyEngin/ 秋田蓮音                                                       11/27/2026
//                                          外部 CLI を 1 本走らせて出力をメモリへ取る子プロセス実行器の実装
//====================================================================================
#include "Editor/Tools/GameLift/CliProcess.h"

#include <algorithm>

#include <Windows.h>

namespace mye {
namespace {

constexpr size_t kMaxCapturePerStream = 64u * 1024u * 1024u;
constexpr uint64_t kDrainGraceMs = 2000; // 子が終わってからパイプが閉じるまでの猶予。超えたら孫ごと止める
constexpr UINT kKilledExitCode = 1;
constexpr DWORD kPipeBufferBytes = 64 * 1024;

// ハンドルの寿命を閉じる小さな RAII (起動途中の失敗経路で閉じ忘れない)
struct ScopedHandle
{
    HANDLE handle = nullptr;
    ScopedHandle() = default;
    explicit ScopedHandle(HANDLE h) : handle(h) {}
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

// CommandLineToArgvW が元へ戻す引用: 空・空白・引用符を含むときだけ囲み、引用符の直前と閉じ引用符の直前の
// バックスラッシュの連なりを倍にする
void AppendQuotedArg(std::wstring& out, const std::wstring& arg)
{
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        out += arg;
        return;
    }
    out += L'"';
    for (auto it = arg.begin();; ++it) {
        size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == arg.end()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(backslashes, L'\\');
            out.push_back(*it);
        }
    }
    out += L'"';
}

bool NameEquals(const std::wstring& a, const std::wstring& b)
{
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) ==
           CSTR_EQUAL;
}

bool NameLess(const CliEnvVar& a, const CliEnvVar& b)
{
    return CompareStringOrdinal(a.name.c_str(), static_cast<int>(a.name.size()), b.name.c_str(),
                                static_cast<int>(b.name.size()), TRUE) == CSTR_LESS_THAN;
}

} // namespace

std::wstring BuildCommandLine(const std::wstring& exePath, const std::vector<std::wstring>& args)
{
    std::wstring line;
    // 先頭の実行ファイル名は CommandLineToArgvW が別規則 (囲みの中は逐語) で読むので、空白があるときだけ囲む
    if (exePath.find_first_of(L" \t") != std::wstring::npos) {
        line += L'"';
        line += exePath;
        line += L'"';
    } else {
        line += exePath;
    }
    for (const std::wstring& arg : args) {
        line += L' ';
        AppendQuotedArg(line, arg);
    }
    return line;
}

std::vector<CliEnvVar> FixedCliEnvironment()
{
    return {
        { L"PYTHONIOENCODING", L"utf-8" },
        { L"AWS_CLI_FILE_ENCODING", L"UTF-8" },
        { L"AWS_PAGER", L"" },
    };
}

std::vector<CliEnvVar> ReadParentEnvironment()
{
    std::vector<CliEnvVar> vars;
    LPWCH block = GetEnvironmentStringsW();
    if (block == nullptr) {
        return vars;
    }
    for (const wchar_t* p = block; *p != L'\0'; p += wcslen(p) + 1) {
        const std::wstring entry(p);
        // "=C:=C:\path" のように名前が '=' で始まる項目があるので、検索は 2 文字目から
        const size_t eq = entry.find(L'=', 1);
        if (eq == std::wstring::npos) {
            continue;
        }
        vars.push_back({ entry.substr(0, eq), entry.substr(eq + 1) });
    }
    FreeEnvironmentStringsW(block);
    return vars;
}

std::wstring BuildEnvironmentBlock(const std::vector<CliEnvVar>& parent, const std::vector<CliEnvVar>& overrides)
{
    std::vector<CliEnvVar> merged = parent;
    for (const CliEnvVar& over : overrides) {
        if (over.name.empty() || over.name.find(L'=') != std::wstring::npos) {
            continue;
        }
        auto hit = std::find_if(merged.begin(), merged.end(),
                                [&](const CliEnvVar& v) { return NameEquals(v.name, over.name); });
        if (hit != merged.end()) {
            *hit = over;
        } else {
            merged.push_back(over);
        }
    }
    std::stable_sort(merged.begin(), merged.end(), NameLess);

    std::wstring block;
    for (const CliEnvVar& v : merged) {
        block += v.name;
        block += L'=';
        block += v.value;
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    if (merged.empty()) {
        block.push_back(L'\0');
    }
    return block;
}

CliProcess::~CliProcess()
{
    if (state_ == CliRunState::Running) {
        Terminate();
    }
    ReleaseHandles();
}

uint64_t CliProcess::ElapsedMs() const
{
    if (state_ == CliRunState::Idle || state_ == CliRunState::StartFailed) {
        return 0;
    }
    const uint64_t end = (state_ == CliRunState::Running) ? GetTickCount64() : endTick_;
    return end - startTick_;
}

std::string CliProcess::StdoutText() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return out_;
}

std::string CliProcess::StderrText() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return err_;
}

void CliProcess::ClearOutput()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!out_.empty()) {
        SecureZeroMemory(out_.data(), out_.size());
    }
    if (!err_.empty()) {
        SecureZeroMemory(err_.data(), err_.size());
    }
    out_.clear();
    err_.clear();
}

void CliProcess::ReadLoop(void* pipe, std::string* sink)
{
    char buffer[16 * 1024];
    DWORD got = 0;
    while (ReadFile(static_cast<HANDLE>(pipe), buffer, sizeof(buffer), &got, nullptr) && got > 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (sink->size() + got <= kMaxCapturePerStream) {
            sink->append(buffer, got);
        } else {
            truncated_.store(true);
        }
    }
    SecureZeroMemory(buffer, sizeof(buffer));
    readersDone_.fetch_add(1);
}

void CliProcess::Terminate()
{
    if (job_ != nullptr) {
        TerminateJobObject(static_cast<HANDLE>(job_), kKilledExitCode);
    } else if (process_ != nullptr) {
        TerminateProcess(static_cast<HANDLE>(process_), kKilledExitCode);
    }
}

// 先に Job を閉じる (KILL_ON_JOB_CLOSE で取りこぼしの孫も止まり、読み取りが EOF になる) → スレッドを join → 読み取り端を閉じる
void CliProcess::ReleaseHandles()
{
    if (job_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(job_));
        job_ = nullptr;
    }
    if (outThread_.joinable()) {
        outThread_.join();
    }
    if (errThread_.joinable()) {
        errThread_.join();
    }
    if (outRead_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(outRead_));
        outRead_ = nullptr;
    }
    if (errRead_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(errRead_));
        errRead_ = nullptr;
    }
    if (process_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(process_));
        process_ = nullptr;
    }
}

bool CliProcess::Start(const CliRunSpec& spec, std::string& error)
{
    if (state_ == CliRunState::Running) {
        error = "a process is already running";
        return false;
    }
    ReleaseHandles();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        out_.clear();
        err_.clear();
    }
    exited_ = cancelRequested_ = timedOut_ = false;
    exitCode_ = 0;
    readersDone_.store(0);
    truncated_.store(false);
    secret_ = spec.secret;
    timeoutMs_ = spec.timeoutMs;
    startTick_ = endTick_ = exitTick_ = GetTickCount64();
    state_ = CliRunState::StartFailed;

    if (spec.exePath.empty()) {
        error = "exe path is empty";
        return false;
    }

    std::vector<CliEnvVar> overrides = FixedCliEnvironment();
    overrides.insert(overrides.end(), spec.extraEnv.begin(), spec.extraEnv.end());
    std::wstring envBlock = BuildEnvironmentBlock(ReadParentEnvironment(), overrides);
    std::wstring commandLine = BuildCommandLine(spec.exePath, spec.args);
    commandLine.push_back(L'\0'); // CreateProcessW は書き換えうるので可変バッファで渡す
    // 秘密 (子の環境に入れたトークンなど) をこのプロセスのメモリへ残さないため、起動したら両方ゼロで埋める
    const auto wipe = [&]() {
        if (spec.secret) {
            SecureZeroMemory(envBlock.data(), envBlock.size() * sizeof(wchar_t));
            SecureZeroMemory(commandLine.data(), commandLine.size() * sizeof(wchar_t));
        }
    };

    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;

    ScopedHandle outReadEnd, outWriteEnd, errReadEnd, errWriteEnd, nulIn;
    HANDLE hOutRead = nullptr, hOutWrite = nullptr, hErrRead = nullptr, hErrWrite = nullptr;
    if (!CreatePipe(&hOutRead, &hOutWrite, &inheritable, kPipeBufferBytes)) {
        error = "CreatePipe(stdout) failed (" + std::to_string(GetLastError()) + ")";
        wipe();
        return false;
    }
    outReadEnd.handle = hOutRead;
    outWriteEnd.handle = hOutWrite;
    if (!CreatePipe(&hErrRead, &hErrWrite, &inheritable, kPipeBufferBytes)) {
        error = "CreatePipe(stderr) failed (" + std::to_string(GetLastError()) + ")";
        wipe();
        return false;
    }
    errReadEnd.handle = hErrRead;
    errWriteEnd.handle = hErrWrite;
    // 読み取り端は子へ継がせない (継ぐと子が閉じるまで EOF が来ない)
    SetHandleInformation(outReadEnd.handle, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errReadEnd.handle, HANDLE_FLAG_INHERIT, 0);

    nulIn.handle = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    if (nulIn.handle == INVALID_HANDLE_VALUE) {
        nulIn.handle = nullptr;
        error = "open NUL failed (" + std::to_string(GetLastError()) + ")";
        wipe();
        return false;
    }

    // 継がせるハンドルを 3 本に限る。bInheritHandles=TRUE だけだと、別スレッドが同時に起こす別の子へ
    // このパイプの書き込み端が漏れ、こちらの読み取りが EOF にならなくなる
    HANDLE inheritList[3] = { nulIn.handle, outWriteEnd.handle, errWriteEnd.handle };
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
    startup.StartupInfo.hStdOutput = outWriteEnd.handle;
    startup.StartupInfo.hStdError = errWriteEnd.handle;
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
            job = nullptr; // Job が使えなければ本体だけを止める (Terminate が TerminateProcess へ落ちる)
        }
    }
    ResumeThread(info.hThread);
    CloseHandle(info.hThread);

    process_ = info.hProcess;
    job_ = job;
    outRead_ = outReadEnd.Release();
    errRead_ = errReadEnd.Release();
    // 子側の書き込み端は親では閉じる (閉じないと子が終わっても EOF が来ない)。ScopedHandle のデストラクタが閉じる
    outWriteEnd.Close();
    errWriteEnd.Close();
    nulIn.Close();

    outThread_ = std::thread([this]() { ReadLoop(outRead_, &out_); });
    errThread_ = std::thread([this]() { ReadLoop(errRead_, &err_); });

    state_ = CliRunState::Running;
    return true;
}

void CliProcess::Cancel()
{
    if (state_ != CliRunState::Running || exited_ || cancelRequested_) {
        return;
    }
    // 自然終了と競ったときは取消にしない (終了コードを正本にする)
    if (WaitForSingleObject(static_cast<HANDLE>(process_), 0) == WAIT_OBJECT_0) {
        return;
    }
    cancelRequested_ = true;
    Terminate();
}

void CliProcess::Finalize()
{
    ReleaseHandles();
    endTick_ = GetTickCount64();
    if (cancelRequested_) {
        state_ = CliRunState::Cancelled;
    } else if (timedOut_) {
        state_ = CliRunState::TimedOut;
    } else {
        state_ = CliRunState::Finished;
    }
}

CliRunState CliProcess::Poll()
{
    if (state_ != CliRunState::Running) {
        return state_;
    }
    const uint64_t now = GetTickCount64();
    if (!exited_) {
        if (WaitForSingleObject(static_cast<HANDLE>(process_), 0) == WAIT_OBJECT_0) {
            DWORD code = 0;
            GetExitCodeProcess(static_cast<HANDLE>(process_), &code);
            exitCode_ = code;
            exited_ = true;
            exitTick_ = now;
        } else if (timeoutMs_ != 0 && !timedOut_ && !cancelRequested_ && now - startTick_ >= timeoutMs_) {
            timedOut_ = true;
            Terminate();
        }
    }
    if (!exited_) {
        return state_;
    }
    if (readersDone_.load() >= 2) {
        Finalize();
        return state_;
    }
    if (now - exitTick_ >= kDrainGraceMs) {
        Terminate(); // 孫がパイプを握り続けている。Job ごと止めれば読み取りが終わる
    }
    return state_;
}

} // namespace mye
