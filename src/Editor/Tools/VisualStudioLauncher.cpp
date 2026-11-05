//====================================================================================
//                          VisualStudioLauncher.cpp
//  MyEngin/ 秋田蓮音                                                     11/02/2026
//                                          スクリプトを Visual Studio のソリューションごと開く
//====================================================================================
#include "Editor/Tools/VisualStudioLauncher.h"

#include <cwchar>
#include <filesystem>

#include <Windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <wrl/client.h>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Platform/PathUtil.h"

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

namespace mye {
namespace {

// ROT に登録される DTE の表示名は "!VisualStudio.DTE.<版>:<PID>"
constexpr const wchar_t* kDteMonikerPrefix = L"!VisualStudio.DTE.";
// EnvDTE の vsViewKindTextView (ItemOperations.OpenFile の第 2 引数)
constexpr const wchar_t* kViewKindTextView = L"{7651A703-06E5-11D1-8EBD-00A0C90F26EA}";
constexpr DWORD kVswhereTimeoutMs = 10000;
// 起動待ちの問い合わせ間隔。ROT の列挙 + DTE 呼び出しは main スレッドで数 ms かかるので毎フレームはやらない
constexpr uint64_t kPollIntervalMs = 1000;
// プロジェクト数がこの回数続けて変わらなければ「読み込みが落ち着いた」とみなす
constexpr int kStablePollsBeforeOpen = 2;
// 初回起動 (拡張機能の初期化を含む) を待つ上限
constexpr uint64_t kLaunchTimeoutMs = 180000;

// エディタ main スレッドは COM 未初期化なので都度初期化する (AssetOps の RecycleToBin と同じ方針。
// 既に別モデルで初期化済み = RPC_E_CHANGED_MODE ならそのまま使い、Uninitialize もしない)
class ScopedComInit {
public:
    ScopedComInit() : hr_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)) {}
    ~ScopedComInit()
    {
        if (SUCCEEDED(hr_)) {
            CoUninitialize();
        }
    }
    ScopedComInit(const ScopedComInit&) = delete;
    ScopedComInit& operator=(const ScopedComInit&) = delete;

private:
    HRESULT hr_;
};

struct ScopedVariant {
    VARIANT v;
    ScopedVariant() { VariantInit(&v); }
    ~ScopedVariant() { VariantClear(&v); }
    ScopedVariant(const ScopedVariant&) = delete;
    ScopedVariant& operator=(const ScopedVariant&) = delete;
};

struct ScopedBstr {
    BSTR s;
    explicit ScopedBstr(const std::wstring& w) : s(SysAllocString(w.c_str())) {}
    ~ScopedBstr() { SysFreeString(s); }
    ScopedBstr(const ScopedBstr&) = delete;
    ScopedBstr& operator=(const ScopedBstr&) = delete;
};

// IDispatch の遅延バインド呼び出し。args は COM の規約どおり**逆順** (最後の引数が [0])
HRESULT DispInvoke(IDispatch* obj, const wchar_t* name, WORD flags, VARIANT* args, UINT argc,
                   VARIANT* result)
{
    DISPID id = 0;
    LPOLESTR member = const_cast<LPOLESTR>(name);
    HRESULT hr = obj->GetIDsOfNames(IID_NULL, &member, 1, LOCALE_USER_DEFAULT, &id);
    if (FAILED(hr)) {
        return hr;
    }
    DISPPARAMS params = { args, nullptr, argc, 0 };
    return obj->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, flags, &params, result, nullptr, nullptr);
}

HRESULT GetDispatchProp(IDispatch* obj, const wchar_t* name, ComPtr<IDispatch>& out)
{
    ScopedVariant r;
    HRESULT hr = DispInvoke(obj, name, DISPATCH_PROPERTYGET, nullptr, 0, &r.v);
    if (FAILED(hr)) {
        return hr;
    }
    if (r.v.vt != VT_DISPATCH || r.v.pdispVal == nullptr) {
        return E_NOINTERFACE;
    }
    out = r.v.pdispVal; // ComPtr が AddRef する (r の解放と釣り合う)
    return S_OK;
}

// DTE.Solution.FullName。ソリューション未オープンの VS は空文字を返す
HRESULT GetSolutionPath(IDispatch* dte, std::wstring& out)
{
    ComPtr<IDispatch> solution;
    HRESULT hr = GetDispatchProp(dte, L"Solution", solution);
    if (FAILED(hr)) {
        return hr;
    }
    ScopedVariant r;
    hr = DispInvoke(solution.Get(), L"FullName", DISPATCH_PROPERTYGET, nullptr, 0, &r.v);
    if (FAILED(hr)) {
        return hr;
    }
    out = (r.v.vt == VT_BSTR && r.v.bstrVal != nullptr) ? std::wstring(r.v.bstrVal) : std::wstring();
    return S_OK;
}

bool SamePath(const std::wstring& a, const std::wstring& b)
{
    const std::wstring na = fs::path(a).lexically_normal().wstring();
    const std::wstring nb = fs::path(b).lexically_normal().wstring();
    return CompareStringOrdinal(na.c_str(), static_cast<int>(na.size()), nb.c_str(),
                                static_cast<int>(nb.size()), TRUE)
        == CSTR_EQUAL;
}

// 起動中の VS に file を開かせて line へ移動し、前面に出す
HRESULT OpenInRunningDte(IDispatch* dte, DWORD pid, const std::wstring& file, int line)
{
    ComPtr<IDispatch> itemOps;
    HRESULT hr = GetDispatchProp(dte, L"ItemOperations", itemOps);
    if (FAILED(hr)) {
        return hr;
    }
    {
        ScopedBstr fileArg(file);
        ScopedBstr viewArg(kViewKindTextView);
        VARIANT args[2];
        args[0].vt = VT_BSTR; // ViewKind
        args[0].bstrVal = viewArg.s;
        args[1].vt = VT_BSTR; // FileName
        args[1].bstrVal = fileArg.s;
        ScopedVariant r;
        hr = DispInvoke(itemOps.Get(), L"OpenFile", DISPATCH_METHOD, args, 2, &r.v);
        if (FAILED(hr)) {
            return hr;
        }
    }
    if (line > 0) {
        ScopedBstr cmdArg(L"Edit.GoTo");
        ScopedBstr lineArg(std::to_wstring(line));
        VARIANT args[2];
        args[0].vt = VT_BSTR; // CommandArgs
        args[0].bstrVal = lineArg.s;
        args[1].vt = VT_BSTR; // CommandName
        args[1].bstrVal = cmdArg.s;
        // 行移動の失敗 (行数超過など) はファイルが開けている以上、成功扱いにする
        DispInvoke(dte, L"ExecuteCommand", DISPATCH_METHOD, args, 2, nullptr);
    }
    // 前面化は VS 側のプロセスが行うので、フォアグラウンドを持つエディタから権利を渡しておく
    AllowSetForegroundWindow(pid);
    ComPtr<IDispatch> mainWindow;
    if (SUCCEEDED(GetDispatchProp(dte, L"MainWindow", mainWindow))) {
        DispInvoke(mainWindow.Get(), L"Activate", DISPATCH_METHOD, nullptr, 0, nullptr);
    }
    return S_OK;
}

// DTE.Solution.Projects.Count。読み込み中は増えていく。失敗は -1
int GetProjectCount(IDispatch* dte)
{
    ComPtr<IDispatch> solution;
    ComPtr<IDispatch> projects;
    if (FAILED(GetDispatchProp(dte, L"Solution", solution))
        || FAILED(GetDispatchProp(solution.Get(), L"Projects", projects))) {
        return -1;
    }
    ScopedVariant r;
    if (FAILED(DispInvoke(projects.Get(), L"Count", DISPATCH_PROPERTYGET, nullptr, 0, &r.v)) || r.v.vt != VT_I4) {
        return -1;
    }
    return r.v.lVal;
}

enum class DteSearch { Found, NotFound, Busy };

// solution を開いている VS の DTE を ROT から探す。onlyPid != 0 ならその PID の VS だけを見る
DteSearch FindDte(const std::wstring& solution, DWORD onlyPid, ComPtr<IDispatch>& dteOut, DWORD& pidOut)
{
    ComPtr<IRunningObjectTable> rot;
    ComPtr<IEnumMoniker> monikers;
    if (FAILED(GetRunningObjectTable(0, &rot)) || FAILED(rot->EnumRunning(&monikers))) {
        return DteSearch::NotFound;
    }
    bool anyBusy = false;
    ComPtr<IMoniker> moniker;
    while (monikers->Next(1, moniker.ReleaseAndGetAddressOf(), nullptr) == S_OK) {
        ComPtr<IBindCtx> bindCtx;
        LPOLESTR displayName = nullptr;
        if (FAILED(CreateBindCtx(0, &bindCtx))
            || FAILED(moniker->GetDisplayName(bindCtx.Get(), nullptr, &displayName))) {
            continue;
        }
        const std::wstring name = displayName;
        CoTaskMemFree(displayName);
        if (name.rfind(kDteMonikerPrefix, 0) != 0) {
            continue;
        }
        const size_t colon = name.rfind(L':');
        const DWORD pid =
            colon == std::wstring::npos ? 0 : static_cast<DWORD>(std::wcstoul(name.c_str() + colon + 1, nullptr, 10));
        if (onlyPid != 0 && pid != onlyPid) {
            continue;
        }

        ComPtr<IUnknown> unk;
        ComPtr<IDispatch> dte;
        if (FAILED(rot->GetObject(moniker.Get(), &unk)) || FAILED(unk.As(&dte))) {
            continue;
        }
        std::wstring opened;
        const HRESULT hr = GetSolutionPath(dte.Get(), opened);
        if (FAILED(hr)) {
            anyBusy = anyBusy || hr == RPC_E_CALL_REJECTED || hr == RPC_E_SERVERCALL_RETRYLATER;
            continue;
        }
        if (opened.empty() || !SamePath(opened, solution)) {
            continue;
        }
        dteOut = dte;
        pidOut = pid;
        return DteSearch::Found;
    }
    // 応答しなかった VS がこのソリューションを開いている可能性がある。新しく起動すると
    // 同じソリューションの VS が 2 つになるので、ここで止める
    return anyBusy ? DteSearch::Busy : DteSearch::NotFound;
}

// vswhere の 1 行目 (productPath) を返す。失敗は空
std::wstring RunVswhere()
{
    wchar_t* programFilesX86 = nullptr;
    size_t len = 0;
    std::wstring vswhere;
    if (_wdupenv_s(&programFilesX86, &len, L"ProgramFiles(x86)") == 0 && programFilesX86) {
        vswhere = std::wstring(programFilesX86) + L"\\Microsoft Visual Studio\\Installer\\vswhere.exe";
        free(programFilesX86);
    }
    std::error_code ec;
    if (vswhere.empty() || !fs::exists(vswhere, ec)) {
        return {};
    }

    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
        return {};
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi = {};
    std::wstring cmdline = L"\"" + vswhere + L"\" -latest -prerelease -utf8 -property productPath";
    const BOOL started = CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                        nullptr, nullptr, &si, &pi);
    CloseHandle(writePipe); // 子の終了で ReadFile が EOF を返すよう、親側の書き込み口は閉じる
    std::string out;
    if (started) {
        char buf[512];
        DWORD got = 0;
        while (ReadFile(readPipe, buf, sizeof(buf), &got, nullptr) && got > 0) {
            out.append(buf, got);
        }
        WaitForSingleObject(pi.hProcess, kVswhereTimeoutMs);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    CloseHandle(readPipe);

    const size_t end = out.find_first_of("\r\n");
    std::wstring path = Utf8ToWide(out.substr(0, end));
    if (path.empty() || !fs::exists(path, ec)) {
        return {};
    }
    return path;
}

} // namespace

const std::wstring& FindLatestDevenv()
{
    static const std::wstring devenv = RunVswhere();
    return devenv;
}

bool VisualStudioLauncher::Open(const std::wstring& solution, const std::wstring& file, int line)
{
    // 起動待ちの最中に同じソリューションへの要求が来たら、開く対象だけ差し替える (二重起動しない)
    if (pending_ && SamePath(pending_->solution, solution)) {
        pending_->file = file;
        pending_->line = line;
        return true;
    }

    DteSearch found = DteSearch::NotFound;
    {
        const ScopedComInit com;
        ComPtr<IDispatch> dte;
        DWORD pid = 0;
        found = FindDte(solution, 0, dte, pid);
        if (found == DteSearch::Found) {
            const HRESULT hr = OpenInRunningDte(dte.Get(), pid, file, line);
            if (SUCCEEDED(hr)) {
                return true;
            }
            MYE_LOG_WARN("[ide] Visual Studio (pid %lu) refused to open %s (hr=0x%08lx)", pid,
                         WideToUtf8(file).c_str(), static_cast<unsigned long>(hr));
            found = DteSearch::Busy;
        }
    }
    if (found == DteSearch::Busy) {
        // 応答しなかった VS がこのソリューションを開いている可能性がある。新しく起動すると
        // 同じソリューションの VS が 2 つになるので、ここで止める
        MYE_LOG_WARN("[ide] Visual Studio is busy (a dialog may be open) - close it and try again: %s",
                     WideToUtf8(file).c_str());
        return false;
    }

    const std::wstring& devenv = FindLatestDevenv();
    if (devenv.empty()) {
        MYE_LOG_WARN("[ide] Visual Studio (devenv.exe) not found via vswhere");
        return false;
    }
    const std::wstring args = L"\"" + solution + L"\"";
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"open";
    sei.lpFile = devenv.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei) || sei.hProcess == nullptr) {
        MYE_LOG_WARN("[ide] failed to start %s (%lu)", WideToUtf8(devenv).c_str(), GetLastError());
        return false;
    }
    PendingOpen p;
    p.solution = solution;
    p.file = file;
    p.line = line;
    p.pid = GetProcessId(sei.hProcess);
    CloseHandle(sei.hProcess);
    const uint64_t now = GetTickCount64();
    p.deadlineMs = now + kLaunchTimeoutMs;
    p.nextPollMs = now + kPollIntervalMs;
    pending_ = std::move(p);
    MYE_LOG_INFO("[ide] starting Visual Studio: %s (the file opens once the solution has loaded)",
                 WideToUtf8(solution).c_str());
    return true;
}

void VisualStudioLauncher::Poll()
{
    if (!pending_) {
        return;
    }
    const uint64_t now = GetTickCount64();
    if (now < pending_->nextPollMs) {
        return;
    }
    pending_->nextPollMs = now + kPollIntervalMs;
    if (now > pending_->deadlineMs) {
        MYE_LOG_WARN("[ide] Visual Studio did not finish loading %s in time - open the file from there",
                     WideToUtf8(pending_->solution).c_str());
        pending_.reset();
        return;
    }
    // 起動した VS が閉じられたら待つのをやめる
    if (const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pending_->pid)) {
        const bool exited = WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
        CloseHandle(process);
        if (exited) {
            pending_.reset();
            return;
        }
    } else {
        pending_.reset();
        return;
    }

    const ScopedComInit com;
    ComPtr<IDispatch> dte;
    DWORD pid = 0;
    if (FindDte(pending_->solution, pending_->pid, dte, pid) != DteSearch::Found) {
        return; // まだ ROT に居ない / 読み込み中で呼び出しを拒否している
    }
    const int count = GetProjectCount(dte.Get());
    if (count <= 0 || count != pending_->lastProjectCount) {
        pending_->lastProjectCount = count;
        pending_->stablePolls = 0;
        return;
    }
    if (++pending_->stablePolls < kStablePollsBeforeOpen) {
        return;
    }
    const HRESULT hr = OpenInRunningDte(dte.Get(), pid, pending_->file, pending_->line);
    if (FAILED(hr)) {
        if (hr == RPC_E_CALL_REJECTED || hr == RPC_E_SERVERCALL_RETRYLATER) {
            return; // まだ忙しい。次の問い合わせで再試行
        }
        MYE_LOG_WARN("[ide] Visual Studio (pid %lu) refused to open %s (hr=0x%08lx)", pid,
                     WideToUtf8(pending_->file).c_str(), static_cast<unsigned long>(hr));
    }
    pending_.reset();
}

} // namespace mye
