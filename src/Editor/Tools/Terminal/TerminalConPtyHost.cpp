//====================================================================================
//                          TerminalConPtyHost.cpp
//  MyEngine/ 秋田蓮音                                                      11/19/2026
//                                          ConPTY (疑似コンソール) で子プロセスを動かすターミナルホスト (Win32)
//====================================================================================
#include "Editor/Tools/Terminal/TerminalHost.h"

#include <algorithm>
#include <array>
#include <thread>
#include <vector>

#include <Windows.h>

#include "Editor/ChangeTracking/TrackLaunch.h"
#include "Editor/Tools/Terminal/TerminalByteQueue.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {
namespace {

// HPCON と PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE は SDK 10.0.17763 以降かつ NTDDI_VERSION が RS5 以上でしか
// 宣言されない。対象 OS の下限を上げずに済むよう、自前の型と値で持つ (値は ProcThreadAttributeValue(22, FALSE, TRUE, FALSE))
using PseudoConsoleHandle = void*;
constexpr DWORD_PTR kProcThreadAttributePseudoConsole = 0x00020016;

using CreatePseudoConsoleFn = HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, PseudoConsoleHandle*);
using ResizePseudoConsoleFn = HRESULT(WINAPI*)(PseudoConsoleHandle, COORD);
using ClosePseudoConsoleFn = void(WINAPI*)(PseudoConsoleHandle);

// 読み取りスレッド → UI のキューの上限。1 フレームの投入上限 (TerminalSession の 256KB) の 16 倍あれば、
// UI が数フレーム止まっても子を待たせない。超えたら子の書き込みが待つ (背圧)
constexpr size_t kOutputQueueCapacity = 4u * 1024u * 1024u;
constexpr DWORD kReadChunkBytes = 64u * 1024u;

struct ConPtyApi
{
    CreatePseudoConsoleFn create = nullptr;
    ResizePseudoConsoleFn resize = nullptr;
    ClosePseudoConsoleFn close = nullptr;

    bool Available() const { return create != nullptr && resize != nullptr && close != nullptr; }
};

// 静的リンクすると 1809 より前の Windows で Editor 自体が起動しなくなる。実行時に引き、無ければ非対応として窓だけが諦める
ConPtyApi ResolveConPtyApi(TerminalConPtyApiSource source)
{
    ConPtyApi api;
    if (source == TerminalConPtyApiSource::Missing) {
        return api;
    }
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    if (kernel == nullptr) {
        return api;
    }
    api.create = reinterpret_cast<CreatePseudoConsoleFn>(reinterpret_cast<void*>(GetProcAddress(kernel, "CreatePseudoConsole")));
    api.resize = reinterpret_cast<ResizePseudoConsoleFn>(reinterpret_cast<void*>(GetProcAddress(kernel, "ResizePseudoConsole")));
    api.close = reinterpret_cast<ClosePseudoConsoleFn>(reinterpret_cast<void*>(GetProcAddress(kernel, "ClosePseudoConsole")));
    return api;
}

COORD ToCoord(int cols, int rows)
{
    COORD size = {};
    size.X = static_cast<SHORT>(cols);
    size.Y = static_cast<SHORT>(rows);
    return size;
}

void CloseIfValid(HANDLE& handle)
{
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
    }
    handle = nullptr;
}

// 引数の引用。空白・タブ・'"' を含まず空でもない引数は引用しない。cmd.exe は "/c" のように引用されたスイッチを
// スイッチとして扱わない (CRT の規則で読む子には引用の有無で結果は変わらない)。必要なときは QuoteCommandLineArg
std::wstring QuoteArgIfNeeded(const std::wstring& arg)
{
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) {
        return arg;
    }
    return QuoteCommandLineArg(arg);
}

std::string LastErrorText(const char* what)
{
    return std::string(what) + " failed (" + std::to_string(GetLastError()) + ")";
}

class ConPtyTerminalHost final : public TerminalHost
{
public:
    explicit ConPtyTerminalHost(TerminalConPtyApiSource source)
        : m_api(ResolveConPtyApi(source))
        , m_queue(kOutputQueueCapacity)
    {
    }

    ~ConPtyTerminalHost() override
    {
        Close(false);
    }

    TerminalHostStartResult Start(const TerminalLaunchSpec& spec, int cols, int rows, std::string& errorOut) override
    {
        errorOut.clear();
        if (!m_api.Available()) {
            errorOut = "ConPTY (CreatePseudoConsole) is not available on this Windows";
            return TerminalHostStartResult::Unsupported;
        }
        if (m_console != nullptr) {
            errorOut = "terminal host is already running";
            return TerminalHostStartResult::Failed;
        }
        m_queue.Reset();

        // inputRead / outputWrite は疑似コンソール (conhost) 側の端。CreatePseudoConsole が複製して持つので、
        // 子の起動後にこちらの分は閉じる。★outputWrite を持ち続けると、conhost が終わっても ReadFile が EOF にならず
        // 読み取りスレッドが join できない
        HANDLE inputRead = nullptr;
        HANDLE outputWrite = nullptr;
        if (!CreatePipe(&inputRead, &m_inputWrite, nullptr, 0)) {
            errorOut = LastErrorText("CreatePipe(input)");
            return TerminalHostStartResult::Failed;
        }
        if (!CreatePipe(&m_outputRead, &outputWrite, nullptr, 0)) {
            errorOut = LastErrorText("CreatePipe(output)");
            CloseIfValid(inputRead);
            ReleaseHandles();
            return TerminalHostStartResult::Failed;
        }
        const HRESULT hr = m_api.create(ToCoord(cols, rows), inputRead, outputWrite, 0, &m_console);
        CloseIfValid(inputRead);
        CloseIfValid(outputWrite);
        if (FAILED(hr)) {
            errorOut = "CreatePseudoConsole failed (hr=" + std::to_string(static_cast<long>(hr)) + ")";
            m_console = nullptr;
            ReleaseHandles();
            return TerminalHostStartResult::Failed;
        }

        // 読み取りは子の起動より先に始める。conhost は起動直後から出力を書くので、読まれないとパイプが詰まりうる
        m_reader = std::thread([this] { ReadLoop(); });

        if (!LaunchChild(spec, errorOut)) {
            Close(false);
            return TerminalHostStartResult::Failed;
        }
        return TerminalHostStartResult::Started;
    }

    size_t ReadOutput(std::string& out, size_t maxBytes) override
    {
        return m_queue.PopUpTo(out, maxBytes);
    }

    bool Write(std::string_view bytes) override
    {
        if (m_inputWrite == nullptr) {
            return false;
        }
        // 同期書き込み。conhost は入力を即座に読むので通常は待たないが、巨大な貼付で子が読まないと UI が待つ
        size_t offset = 0;
        while (offset < bytes.size()) {
            const DWORD chunk = static_cast<DWORD>(std::min<size_t>(bytes.size() - offset, kReadChunkBytes));
            DWORD written = 0;
            if (!WriteFile(m_inputWrite, bytes.data() + offset, chunk, &written, nullptr) || written == 0) {
                return false;
            }
            offset += written;
        }
        return true;
    }

    bool Resize(int cols, int rows) override
    {
        if (m_console == nullptr) {
            return false;
        }
        return SUCCEEDED(m_api.resize(m_console, ToCoord(cols, rows)));
    }

    bool PollExit(uint32_t& exitCode) override
    {
        exitCode = 0;
        if (m_process == nullptr || WaitForSingleObject(m_process, 0) != WAIT_OBJECT_0) {
            return false;
        }
        DWORD code = 0;
        GetExitCodeProcess(m_process, &code);
        exitCode = static_cast<uint32_t>(code);
        return true;
    }

    void Close(bool keepPendingOutput) override
    {
        // 順序 (spec 4.4): キューを待たない状態にする → ClosePseudoConsole → 読み取りスレッド join → ハンドル解放。
        // ClosePseudoConsole は conhost が出力を吐き切るまで返らないことがある。読み取りを先に止めると UI が固まる
        m_queue.SetMode(keepPendingOutput ? TerminalByteQueue::Mode::Unbounded : TerminalByteQueue::Mode::Discard);
        if (m_console != nullptr) {
            m_api.close(m_console);
            m_console = nullptr;
        }
        // conhost が終わると出力パイプの書き込み端が全部閉じ、ReadFile が失敗して ReadLoop が抜ける
        if (m_reader.joinable()) {
            m_reader.join();
        }
        ReleaseHandles();
    }

private:
    bool LaunchChild(const TerminalLaunchSpec& spec, std::string& errorOut)
    {
        SIZE_T attrSize = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
        std::vector<unsigned char> attrStorage(attrSize);
        auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrStorage.data());
        if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize)) {
            errorOut = LastErrorText("InitializeProcThreadAttributeList");
            return false;
        }
        // lpValue は HPCON の値そのもの (ポインタのアドレスではない)。Microsoft の ConPTY 手順どおり
        if (!UpdateProcThreadAttribute(attrs, 0, kProcThreadAttributePseudoConsole, m_console, sizeof(m_console), nullptr, nullptr)) {
            errorOut = LastErrorText("UpdateProcThreadAttribute");
            DeleteProcThreadAttributeList(attrs);
            return false;
        }

        STARTUPINFOEXW si = {};
        si.StartupInfo.cb = sizeof(si);
        si.lpAttributeList = attrs;

        std::wstring cmdline = QuoteCommandLineArg(Utf8ToWide(spec.exePath));
        for (const std::string& arg : spec.args) {
            cmdline += L' ';
            cmdline += QuoteArgIfNeeded(Utf8ToWide(arg));
        }
        std::vector<wchar_t> cmdBuffer(cmdline.begin(), cmdline.end());
        cmdBuffer.push_back(L'\0'); // CreateProcessW は書込可能バッファを要求する

        // FormatTerminalEnvironmentBlock は NUL 区切り + 二重 NUL 終端。Utf8ToWide は長さ指定なので途中の NUL も保つ
        std::wstring envBlock;
        if (!spec.environment.empty()) {
            envBlock = Utf8ToWide(FormatTerminalEnvironmentBlock(spec.environment));
        }
        const std::wstring workDir = Utf8ToWide(spec.workDir);

        // 親 (Editor) のハンドルが閉じたら子孫ごと終わる。Editor が落ちてもシェルや孫 (git, claude) を残さない
        m_job = CreateJobObjectW(nullptr, nullptr);
        if (m_job != nullptr) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(m_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        }

        // bInheritHandles = FALSE: 疑似コンソールは属性で渡る。Editor のハンドル (ログ等) を子へ漏らさない。
        // CREATE_SUSPENDED: ジョブへ入れてから走らせる (入れる前に子が孫を作ると孫がジョブの外に出る)
        PROCESS_INFORMATION pi = {};
        const DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED;
        const BOOL ok = CreateProcessW(nullptr, cmdBuffer.data(), nullptr, nullptr, FALSE, flags,
                                       envBlock.empty() ? nullptr : envBlock.data(),
                                       workDir.empty() ? nullptr : workDir.c_str(), &si.StartupInfo, &pi);
        const DWORD err = ok ? 0 : GetLastError();
        DeleteProcThreadAttributeList(attrs);
        if (!ok) {
            errorOut = "CreateProcess failed (" + std::to_string(err) + "): " + WideToUtf8(cmdline);
            return false;
        }
        if (m_job != nullptr && !AssignProcessToJobObject(m_job, pi.hProcess)) {
            MYE_LOG_WARN("[terminal] shell could not be put in a kill-on-close job (%lu); its children may outlive the editor",
                         GetLastError());
        }
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        m_process = pi.hProcess;
        MYE_LOG_INFO("[terminal] started pid %lu: %s", pi.dwProcessId, WideToUtf8(cmdline).c_str());
        return true;
    }

    // 読み取りスレッド。触るのは m_outputRead (Start で作り Close の join 後に閉じる) と m_queue だけ
    void ReadLoop()
    {
        std::array<char, kReadChunkBytes> buffer = {};
        for (;;) {
            DWORD read = 0;
            if (!ReadFile(m_outputRead, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) || read == 0) {
                return;
            }
            m_queue.Push(std::string_view(buffer.data(), read));
        }
    }

    void ReleaseHandles()
    {
        CloseIfValid(m_inputWrite);
        CloseIfValid(m_outputRead);
        CloseIfValid(m_process);
        // KILL_ON_JOB_CLOSE: ここで残っている子孫 (シェルが待たずに残した孫など) が終わる
        CloseIfValid(m_job);
    }

    ConPtyApi m_api;
    TerminalByteQueue m_queue;
    PseudoConsoleHandle m_console = nullptr;
    HANDLE m_inputWrite = nullptr;
    HANDLE m_outputRead = nullptr;
    HANDLE m_process = nullptr;
    HANDLE m_job = nullptr;
    std::thread m_reader;
};

} // namespace

std::unique_ptr<TerminalHost> CreateConPtyTerminalHost(TerminalConPtyApiSource apiSource)
{
    return std::make_unique<ConPtyTerminalHost>(apiSource);
}

std::string FindTerminalExecutable(const std::string& nameOrPath)
{
    if (nameOrPath.empty()) {
        return {};
    }
    const std::wstring wide = Utf8ToWide(nameOrPath);
    std::wstring found(MAX_PATH, L'\0');
    DWORD len = SearchPathW(nullptr, wide.c_str(), nullptr, static_cast<DWORD>(found.size()), found.data(), nullptr);
    if (len >= found.size()) {
        found.resize(len);
        len = SearchPathW(nullptr, wide.c_str(), nullptr, static_cast<DWORD>(found.size()), found.data(), nullptr);
    }
    if (len == 0 || len >= found.size()) {
        return {};
    }
    found.resize(len);
    // SearchPathW はディレクトリにも一致しうる。シェルとして起動できないので弾く
    const DWORD attrs = GetFileAttributesW(found.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return {};
    }
    return WideToUtf8(found);
}

} // namespace mye
