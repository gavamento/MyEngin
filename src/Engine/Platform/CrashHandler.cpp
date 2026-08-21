#include "Engine/Platform/CrashHandler.h"

#include <crtdbg.h>
#include <cstdlib>
#include <cstring>
#include <exception>

#include <Windows.h>
// Windows.h の後でなければならない
#include <DbgHelp.h>

#include "Engine/Core/Log.h"
#include "Engine/Platform/PathUtil.h"

// ビルド時に埋め込まれる git ハッシュと構成 (build\Common.props の MyeBuildInfo)。
// リポジトリ外へ展開された等で生成されていない場合でもビルドは通す
#if __has_include("MyeBuildInfo.h")
#include "MyeBuildInfo.h"
#endif
#ifndef MYE_GIT_HASH
#define MYE_GIT_HASH "unknown"
#endif
#ifndef MYE_BUILD_CONFIG
#define MYE_BUILD_CONFIG "unknown"
#endif

namespace mye {
namespace {

// ---- 事前確保 (ハンドラ内では一切 new しない) ----
constexpr size_t kPathMax = 1024;
constexpr size_t kTextMax = 96 * 1024; // crash.txt 本文 (直近ログ 96 件が主)
constexpr size_t kRecentLogs = 96;

bool g_installed = false;
wchar_t g_crashRoot[kPathMax] = {};
wchar_t g_appName[128] = {}; // ウィンドウタイトル (プロジェクト名が付くので余裕を持たせる)
wchar_t g_bundleDir[kPathMax] = {};
wchar_t g_pathScratch[kPathMax] = {};
char g_text[kTextMax] = {};
LogEntry g_recent[kRecentLogs] = {};
// ★ハンドラ内の作業領域は全部ここ (スタックへ置かない)。
//   スタックオーバーフローで飛んできたときに残っているスタックは 1 ページ程度しか無く、
//   数 KB のローカル配列を積んだ瞬間に二重フォルトして報告ごと消える。
//   再入ガードで単一スレッド化されているので共有で問題ない
char g_utf8Scratch[kPathMax * 2] = {};
wchar_t g_wideScratch[kPathMax + 64] = {};
wchar_t g_exePath[kPathMax] = {};
wchar_t g_modName[128] = {};
wchar_t g_detail[kPathMax] = {};

const uint64_t* g_tickIndex = nullptr;
const uint64_t* g_frameIndex = nullptr;
const wchar_t* g_sceneLabel = nullptr;
CrashPayloadFn g_payload = nullptr;
void* g_payloadUser = nullptr;

LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;
std::terminate_handler g_prevTerminate = nullptr;

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                          PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION,
                                          PMINIDUMP_CALLBACK_INFORMATION);
MiniDumpWriteDumpFn g_miniDumpWriteDump = nullptr;

// 非 SEH 経路 (terminate / purecall / 不正パラメータ) 用の合成コンテキスト。
// CONTEXT は 16 バイト境界を要求する
alignas(16) CONTEXT g_synthContext = {};
EXCEPTION_RECORD g_synthRecord = {};
EXCEPTION_POINTERS g_synthPointers = {};

// 二重フォルト対策の再入ガード。ハンドラの中でまた落ちたら黙って諦める —
// そこで再帰するとスタックを食い潰して最初のバンドルまで失う
volatile LONG g_inHandler = 0;

using crashfmt::Sink;
using crashfmt::WSink;

void CopyW(wchar_t* dst, size_t cap, const wchar_t* src)
{
    size_t i = 0;
    if (cap == 0) {
        return;
    }
    while (src != nullptr && src[i] != L'\0' && i + 1 < cap) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = L'\0';
}

// UTF-16 → UTF-8 を事前確保バッファ経由で Sink へ (確保しない)
void AppendWide(Sink& s, const wchar_t* w)
{
    if (w == nullptr) {
        return;
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, g_utf8Scratch,
                                      static_cast<int>(sizeof(g_utf8Scratch)), nullptr, nullptr);
    if (n <= 0) {
        s.Ascii("<unprintable>");
        return;
    }
    s.Str(g_utf8Scratch);
}

// ハンドラ内から使える唯一の外向き出力 (stdout + デバッガ)。CRT を通さない
void EmitRawLine(const wchar_t* line)
{
    OutputDebugStringW(line);
    OutputDebugStringW(L"\n");
    const int n = WideCharToMultiByte(CP_UTF8, 0, line, -1, g_utf8Scratch,
                                      static_cast<int>(sizeof(g_utf8Scratch)), nullptr, nullptr);
    if (n <= 1) {
        return;
    }
    const HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h == nullptr || h == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    WriteFile(h, g_utf8Scratch, static_cast<DWORD>(n - 1), &written, nullptr);
    WriteFile(h, "\r\n", 2, &written, nullptr);
}

const char* ExceptionName(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "EXCEPTION_ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_BREAKPOINT: return "EXCEPTION_BREAKPOINT";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "EXCEPTION_DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_INVALID_OPERATION: return "EXCEPTION_FLT_INVALID_OPERATION";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case EXCEPTION_IN_PAGE_ERROR: return "EXCEPTION_IN_PAGE_ERROR";
    case EXCEPTION_PRIV_INSTRUCTION: return "EXCEPTION_PRIV_INSTRUCTION";
    case EXCEPTION_STACK_OVERFLOW: return "EXCEPTION_STACK_OVERFLOW";
    case 0xE06D7363u: return "C++ exception (unhandled throw)";
    case 0xC0000409u: return "STATUS_STACK_BUFFER_OVERRUN (__fastfail)";
    default: return "(unknown)";
    }
}

const char* LevelTag(LogLevel level)
{
    switch (level) {
    case LogLevel::Trace: return "TRACE";
    case LogLevel::Info: return "INFO ";
    case LogLevel::Warn: return "WARN ";
    case LogLevel::Error: return "ERROR";
    }
    return "?????";
}

// アドレスの属するモジュールを、ローダロックを避けて特定する。
// VirtualQuery の AllocationBase がそのままモジュールのベースアドレスになる
void DescribeAddress(const void* addr, uintptr_t& outBase, wchar_t* outName, size_t nameCap)
{
    outBase = 0;
    if (nameCap > 0) {
        outName[0] = L'\0';
    }
    MEMORY_BASIC_INFORMATION mbi = {};
    if (addr == nullptr || VirtualQuery(addr, &mbi, sizeof(mbi)) == 0) {
        return;
    }
    outBase = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
    if (outBase == 0) {
        return;
    }
    wchar_t full[kPathMax];
    const DWORD n = GetModuleFileNameW(reinterpret_cast<HMODULE>(mbi.AllocationBase), full,
                                       static_cast<DWORD>(kPathMax));
    if (n == 0 || n >= kPathMax) {
        return;
    }
    // ファイル名だけ残す (フルパスは exe 行に別途出す)
    const wchar_t* leaf = full;
    for (const wchar_t* p = full; *p != L'\0'; ++p) {
        if (*p == L'\\' || *p == L'/') {
            leaf = p + 1;
        }
    }
    CopyW(outName, nameCap, leaf);
}

// そのアドレスを実際に読んで良いか (コミット済み + 読める保護属性か)。
// ★ハンドラの中で AV を起こすと再入ガードに弾かれて**報告ごと消える**ので、
//   素性の分からないポインタは必ずここを通してから触る
bool IsReadable(const void* p, size_t size)
{
    MEMORY_BASIC_INFORMATION mbi = {};
    if (p == nullptr || VirtualQuery(p, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT) {
        return false;
    }
    const DWORD noRead = PAGE_NOACCESS | PAGE_EXECUTE | PAGE_GUARD;
    if ((mbi.Protect & noRead) != 0 || mbi.Protect == 0) {
        return false;
    }
    const auto begin = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    const auto want = reinterpret_cast<uintptr_t>(p);
    return want + size <= begin + mbi.RegionSize;
}

// メモリ上の PE ヘッダから TimeDateStamp を読む (pdb 突き合わせのキー)。
// base は VirtualQuery の AllocationBase 由来 = PE とは限らないので、
// 1 バイトでも触る前に読めることを確かめる
uint32_t ModuleTimeDateStamp(uintptr_t base)
{
    if (base == 0 || !IsReadable(reinterpret_cast<const void*>(base), sizeof(IMAGE_DOS_HEADER))) {
        return 0;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    // PE ヘッダは先頭 1 ページに収まる。範囲外の e_lfanew はゴミとして捨てる
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew >= 0x1000) {
        return 0;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (!IsReadable(nt, sizeof(IMAGE_NT_HEADERS)) || nt->Signature != IMAGE_NT_SIGNATURE) {
        return 0;
    }
    return nt->FileHeader.TimeDateStamp;
}

// <crashRoot>\crash\<yyyyMMdd_HHmmss>[_n] を作って g_bundleDir へ入れる
bool MakeBundleDir()
{
    WSink dir(g_bundleDir, kPathMax);
    dir.Str(g_crashRoot);
    dir.Ascii("\\crash");
    CreateDirectoryW(g_crashRoot, nullptr);
    CreateDirectoryW(g_bundleDir, nullptr);

    SYSTEMTIME st = {};
    GetLocalTime(&st);
    const size_t baseLen = dir.Length(); // "<root>\crash" までの長さ
    for (int attempt = 0; attempt < 16; ++attempt) {
        // 同一秒に 2 回落ちた場合に備えて、末尾だけを作り直して連番を振る。
        // Sink は「先頭から書く」ので、継ぎ足しは baseLen 以降を別 Sink で扱う
        WSink tail(g_bundleDir + baseLen, kPathMax - baseLen);
        tail.Ascii("\\");
        tail.U64(st.wYear, 4);
        tail.U64(st.wMonth, 2);
        tail.U64(st.wDay, 2);
        tail.Ascii("_");
        tail.U64(st.wHour, 2);
        tail.U64(st.wMinute, 2);
        tail.U64(st.wSecond, 2);
        if (attempt > 0) {
            tail.Ascii("_");
            tail.U64(static_cast<uint64_t>(attempt));
        }
        if (CreateDirectoryW(g_bundleDir, nullptr) != 0) {
            return true;
        }
        if (GetLastError() != ERROR_ALREADY_EXISTS) {
            return false;
        }
    }
    return false;
}

void BundlePath(const wchar_t* leaf)
{
    WSink s(g_pathScratch, kPathMax);
    s.Str(g_bundleDir);
    s.Ascii("\\");
    s.Str(leaf);
}

// minidump を書くのに必要な材料。別スレッドへ渡すのでファイルスコープに置く
struct MiniDumpJob {
    MINIDUMP_EXCEPTION_INFORMATION info;
    HANDLE file;
};
MiniDumpJob g_dumpJob = {};

DWORD WINAPI MiniDumpThread(LPVOID)
{
    // Normal + スレッド情報 + アンロード済みモジュール。
    // メモリを丸ごと入れるオプション (WithFullMemory) は数百 MB になるので使わない —
    // 「ハンドラ内で数百 MB を書くのは自殺行為」(計画の★罠)
    const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
        MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
    const BOOL ok = g_miniDumpWriteDump(
        GetCurrentProcess(), GetCurrentProcessId(), g_dumpJob.file, type,
        g_dumpJob.info.ExceptionPointers != nullptr ? &g_dumpJob.info : nullptr, nullptr, nullptr);
    return ok != FALSE ? 1u : 0u;
}

void WriteMiniDump(EXCEPTION_POINTERS* ep)
{
    if (g_miniDumpWriteDump == nullptr) {
        return;
    }
    BundlePath(L"minidump.dmp");
    const HANDLE h = CreateFileW(g_pathScratch, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    g_dumpJob.info = {};
    g_dumpJob.info.ThreadId = GetCurrentThreadId(); // ★**落ちた**スレッドの id を渡す
    g_dumpJob.info.ExceptionPointers = ep;
    g_dumpJob.info.ClientPointers = FALSE;
    g_dumpJob.file = h;

    // ★dbghelp を**別スレッド**で回す。この関数はハンドラの最後の仕事で、
    //   スタックオーバーフローで飛んできた場合、残りスタックは 1 ページ程度しか無く
    //   MiniDumpWriteDump がその中で落ちる (実測: 0 バイトのダンプが残り、
    //   入れ子フォルトでプロセスが即死するので後始末すら走らなかった)。
    //   新しいスレッドは新品のスタックを持つので、そこだけは正しく書ける。
    //   crash.txt と crash.rep はこの時点で書き終わっているので、ここで失敗しても
    //   バンドルの本体は失われない。
    bool ok = false;
    const HANDLE thread = CreateThread(nullptr, 0, &MiniDumpThread, nullptr, 0, nullptr);
    if (thread != nullptr) {
        // 落ちているプロセスなので待ちは有限に切る (dbghelp が固まっても諦める)
        if (WaitForSingleObject(thread, 20000) == WAIT_OBJECT_0) {
            DWORD code = 0;
            ok = GetExitCodeThread(thread, &code) != FALSE && code == 1u;
        }
        CloseHandle(thread);
    }
    CloseHandle(h);
    if (!ok) {
        // ★空の minidump.dmp を残さない。0 バイトのダンプはデバッガに食わせて初めて
        //   分かる = 人の時間を捨てさせるので、「無い」と分かる形にする
        DeleteFileW(g_pathScratch);
    }
}

void WriteCrashText(EXCEPTION_POINTERS* ep, const char* kind, const wchar_t* detail)
{
    Sink s(g_text, kTextMax);
    s.Ascii("\xEF\xBB\xBF"); // UTF-8 BOM (ログ行に日本語が混ざるため)
    s.Ascii("MyEngine crash report\n");
    s.Ascii("app          : ");
    AppendWide(s, g_appName);
    s.Ascii("\nbuild        : " MYE_BUILD_CONFIG " (git " MYE_GIT_HASH ")\n");

    g_exePath[0] = L'\0';
    GetModuleFileNameW(nullptr, g_exePath, static_cast<DWORD>(kPathMax));
    const uintptr_t exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    s.Ascii("exe          : ");
    AppendWide(s, g_exePath);
    s.Ascii("\nexe base     : 0x");
    s.Hex(exeBase, 16);
    s.Ascii("\nexe stamp    : 0x");
    s.Hex(ModuleTimeDateStamp(exeBase), 8);
    s.Ascii("  (PE TimeDateStamp — pdb の突き合わせキー)\n");

    SYSTEMTIME st = {};
    GetLocalTime(&st);
    s.Ascii("time (local) : ");
    s.U64(st.wYear, 4);
    s.Ascii("-");
    s.U64(st.wMonth, 2);
    s.Ascii("-");
    s.U64(st.wDay, 2);
    s.Ascii(" ");
    s.U64(st.wHour, 2);
    s.Ascii(":");
    s.U64(st.wMinute, 2);
    s.Ascii(":");
    s.U64(st.wSecond, 2);
    s.Ascii("\nprocess / thread : ");
    s.U64(GetCurrentProcessId());
    s.Ascii(" / ");
    s.U64(GetCurrentThreadId());

    s.Ascii("\nkind         : ");
    s.Ascii(kind);
    if (detail != nullptr && detail[0] != L'\0') {
        s.Ascii("\ndetail       : ");
        AppendWide(s, detail);
    }

    if (ep != nullptr && ep->ExceptionRecord != nullptr) {
        const EXCEPTION_RECORD& r = *ep->ExceptionRecord;
        s.Ascii("\nexception    : 0x");
        s.Hex(r.ExceptionCode, 8);
        s.Ascii(" ");
        s.Ascii(ExceptionName(r.ExceptionCode));
        if (r.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r.NumberParameters >= 2) {
            s.Ascii("\n  access     : ");
            const ULONG_PTR op = r.ExceptionInformation[0];
            s.Ascii(op == 0 ? "read" : (op == 1 ? "write" : "execute"));
            s.Ascii(" at 0x");
            s.Hex(static_cast<uint64_t>(r.ExceptionInformation[1]), 16);
        }
        const void* addr = r.ExceptionAddress;
        s.Ascii("\naddress      : 0x");
        s.Hex(reinterpret_cast<uintptr_t>(addr), 16);
        uintptr_t modBase = 0;
        DescribeAddress(addr, modBase, g_modName, 128);
        s.Ascii("\nfaulting mod : ");
        AppendWide(s, g_modName[0] != L'\0' ? g_modName : L"(unknown)");
        s.Ascii("  base 0x");
        s.Hex(modBase, 16);
        s.Ascii("  rva 0x");
        s.Hex(modBase != 0 ? (reinterpret_cast<uintptr_t>(addr) - modBase) : 0, 8);
        s.Ascii("  stamp 0x");
        s.Hex(ModuleTimeDateStamp(modBase), 8);
    }

    s.Ascii("\ntick         : ");
    s.U64(g_tickIndex != nullptr ? *g_tickIndex : 0);
    s.Ascii("\nframe        : ");
    s.U64(g_frameIndex != nullptr ? *g_frameIndex : 0);
    s.Ascii("\nscene        : ");
    AppendWide(s, (g_sceneLabel != nullptr && g_sceneLabel[0] != L'\0')
                      ? g_sceneLabel
                      : L"(built in memory - no source file)");
    s.Ascii("\ncommand line : ");
    AppendWide(s, GetCommandLineW());
    s.Ascii("\n\n");
    s.Ascii("再現手順:\n");
    s.Ascii("  1. 同じコミット (git " MYE_GIT_HASH ") の " MYE_BUILD_CONFIG " ビルドを用意する\n");
    s.Ascii("  2. Runtime.exe --replay-verify \"");
    AppendWide(s, g_bundleDir);
    s.Ascii("\\crash.rep\"\n");
    s.Ascii("     crash.rep は開始スナップショットを埋め込んでいるので、起動シーンに依らず\n");
    s.Ascii("     落ちる直前の tick まで丸ごと再現する (最後の tick は未完了 = 期待ハッシュ無し)。\n");
    s.Ascii("  3. RVA を file:line へ落とすときは minidump.dmp + 上の stamp と一致する pdb。\n");
    s.Ascii("\n---- recent log (oldest first) ----\n");

    const size_t n = logging::ReadRecentUnsafe(g_recent, kRecentLogs);
    for (size_t i = 0; i < n; ++i) {
        const LogEntry& e = g_recent[i];
        s.Ascii("[");
        s.Ascii(LevelTag(e.level));
        s.Ascii("] f");
        s.U64(e.frame);
        s.Ascii(" ");
        s.Str(e.message);
        if (e.file[0] != '\0') {
            s.Ascii("   (");
            s.Str(e.file);
            s.Ascii(":");
            s.U64(static_cast<uint64_t>(e.line < 0 ? 0 : e.line));
            s.Ascii(")");
        }
        s.Ascii("\n");
    }
    if (s.Truncated()) {
        s.Ascii("\n(!! crash.txt truncated)\n");
    }

    BundlePath(L"crash.txt");
    CrashWriteFileRaw(g_pathScratch, s.Text(), s.Length());
}

// バンドル本体。**ここから下では確保もロックもしない**
void WriteBundle(EXCEPTION_POINTERS* ep, const char* kind, const wchar_t* detail)
{
    if (InterlockedCompareExchange(&g_inHandler, 1, 0) != 0) {
        return; // 二重フォルト: 最初の 1 回に賭ける
    }
    if (!g_installed || !MakeBundleDir()) {
        return;
    }
    WriteCrashText(ep, kind, detail);
    if (g_payload != nullptr) {
        g_payload(g_payloadUser, g_bundleDir); // crash.rep / scene.json
    }
    WriteMiniDump(ep); // 一番重いので最後 (ここで死んでも .txt と .rep は残る)

    WSink msg(g_wideScratch, kPathMax + 64);
    msg.Ascii("[crash] bundle written: ");
    msg.Str(g_bundleDir);
    EmitRawLine(msg.Text());
}

// 非 SEH 経路 (terminate / purecall / 不正パラメータ) 用に現在地の CONTEXT を合成する。
// これが無いと minidump にスタックが載らず「どこで死んだか」が残らない
EXCEPTION_POINTERS* SynthesizePointers(DWORD code)
{
    RtlCaptureContext(&g_synthContext);
    g_synthRecord = {};
    g_synthRecord.ExceptionCode = code;
    g_synthRecord.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
    g_synthRecord.ExceptionAddress = reinterpret_cast<PVOID>(g_synthContext.Rip);
    g_synthPointers.ExceptionRecord = &g_synthRecord;
    g_synthPointers.ContextRecord = &g_synthContext;
    return &g_synthPointers;
}

LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* ep)
{
    WriteBundle(ep, "SEH (unhandled exception)", nullptr);
    // EXCEPTION_EXECUTE_HANDLER でプロセスを即終了する。
    // EXCEPTION_CONTINUE_SEARCH に落とすと WER のダイアログが出て、
    // GUI サブシステムの exe を bat から回している CI/検証がそこで固まる
    return EXCEPTION_EXECUTE_HANDLER;
}

void TerminateHandlerFn()
{
    WriteBundle(SynthesizePointers(0xE0004D59u), "std::terminate", nullptr);
    TerminateProcess(GetCurrentProcess(), 3);
}

void PureCallHandlerFn()
{
    WriteBundle(SynthesizePointers(0xE0004D5Au), "pure virtual call (_purecall)", nullptr);
    TerminateProcess(GetCurrentProcess(), 3);
}

void InvalidParamHandlerFn(const wchar_t* expression, const wchar_t* function, const wchar_t* file,
                           unsigned int line, uintptr_t)
{
    WSink d(g_detail, kPathMax);
    d.Str(function != nullptr ? function : L"(unknown function)");
    d.Ascii("  expr=");
    d.Str(expression != nullptr ? expression : L"(none)");
    d.Ascii("  at ");
    d.Str(file != nullptr ? file : L"(no file)");
    d.Ascii(":");
    d.U64(line);
    WriteBundle(SynthesizePointers(0xE0004D5Bu), "CRT invalid parameter", d.Text());
    TerminateProcess(GetCurrentProcess(), 3);
}

// ---- --crash-test の実体 ----
struct PureCallBase {
    PureCallBase() { Invoke(); }
    virtual ~PureCallBase() = default;
    virtual void Fn() = 0;
    // 基底の構築中に仮想呼び出しを通す = _purecall。最適化で解決されないよう noinline
    __declspec(noinline) void Invoke() { Fn(); }
};
struct PureCallDerived : PureCallBase {
    void Fn() override {}
};

// スタックを食い潰す。末尾呼び出しに畳まれないよう戻り値を使い、
// volatile な作業配列でフレームを確実に太らせる
#pragma warning(push)
#pragma warning(disable : 4717) // 全経路で再帰 = スタックオーバーフローする (それが目的)
__declspec(noinline) int BlowTheStack(int depth)
{
    volatile char pad[1024];
    pad[0] = static_cast<char>(depth & 0x7F);
    return pad[0] + BlowTheStack(depth + 1);
}
#pragma warning(pop)

} // namespace

bool CrashWriteFileRaw(const wchar_t* path, const void* data, size_t size)
{
    const HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    bool ok = true;
    const auto* p = static_cast<const unsigned char*>(data);
    size_t left = size;
    while (left > 0) {
        const DWORD chunk = static_cast<DWORD>(left > 0x10000000ull ? 0x10000000ull : left);
        DWORD written = 0;
        if (WriteFile(h, p, chunk, &written, nullptr) == 0 || written == 0) {
            ok = false;
            break;
        }
        p += written;
        left -= written;
    }
    CloseHandle(h);
    return ok;
}

void InstallCrashHandler(const CrashHandlerConfig& config)
{
    CopyW(g_crashRoot, kPathMax, config.crashRoot.c_str());
    CopyW(g_appName, 128, config.appName.c_str());
    g_tickIndex = config.tickIndex;
    g_frameIndex = config.frameIndex;
    g_sceneLabel = config.sceneLabel;
    g_payload = config.payload;
    g_payloadUser = config.payloadUser;
    if (g_installed) {
        return; // 2 回目以降は設定の差し替えだけ
    }

    // ★dbghelp はここで解決する。ハンドラ内の LoadLibrary はローダロックを取りに行くので、
    //   ローダ絡みで落ちたときに確実に固まる
    if (const HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll")) {
        g_miniDumpWriteDump =
            reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
    }

    g_prevFilter = SetUnhandledExceptionFilter(UnhandledFilter);
    g_prevTerminate = std::set_terminate(TerminateHandlerFn);
    _set_purecall_handler(PureCallHandlerFn);
    _set_invalid_parameter_handler(InvalidParamHandlerFn);
    // abort() のダイアログを出さない (bat から回している検証が止まるため)
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    // ★Debug CRT は不正パラメータで**先にアサートを報告してから**ハンドラを呼ぶ。
    //   既定の報告先はモーダルダイアログなので、そこで止まると
    //   _set_invalid_parameter_handler は永遠に呼ばれない = Debug ではこの経路が
    //   実質死んでいる (実測: Runtime.exe が固まってバンドルが出なかった)。
    //   デバッガが付いていないときだけ報告先を落として、ハンドラまで到達させる。
    //   ※Release では crtdbg.h がこの 2 本を no-op マクロにするので #ifdef は要らない
    //     (規則 1: _DEBUG でロジックを分岐しない)
    if (IsDebuggerPresent() == FALSE) {
        _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_DEBUG | _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
        _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_DEBUG | _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    }
    g_installed = true;

    MYE_LOG_INFO("[crash] handler installed (build %s, git %s) -> %s\\crash\\",
                 MYE_BUILD_CONFIG, MYE_GIT_HASH, WideToUtf8(g_crashRoot).c_str());
}

void UninstallCrashHandler()
{
    if (!g_installed) {
        return;
    }
    SetUnhandledExceptionFilter(g_prevFilter);
    std::set_terminate(g_prevTerminate);
    _set_purecall_handler(nullptr);
    _set_invalid_parameter_handler(nullptr);
    g_installed = false;
    g_payload = nullptr;
    g_payloadUser = nullptr;
    g_tickIndex = nullptr;
    g_frameIndex = nullptr;
    g_sceneLabel = nullptr;
}

CrashTestKind ParseCrashTestKind(const wchar_t* s)
{
    if (s == nullptr) {
        return CrashTestKind::None;
    }
    if (wcscmp(s, L"av") == 0) {
        return CrashTestKind::AccessViolation;
    }
    if (wcscmp(s, L"purecall") == 0) {
        return CrashTestKind::PureCall;
    }
    if (wcscmp(s, L"terminate") == 0) {
        return CrashTestKind::Terminate;
    }
    if (wcscmp(s, L"invalidparam") == 0) {
        return CrashTestKind::InvalidParam;
    }
    if (wcscmp(s, L"stackoverflow") == 0) {
        return CrashTestKind::StackOverflow;
    }
    return CrashTestKind::None;
}

const char* CrashTestKindName(CrashTestKind kind)
{
    switch (kind) {
    case CrashTestKind::AccessViolation: return "av";
    case CrashTestKind::PureCall: return "purecall";
    case CrashTestKind::Terminate: return "terminate";
    case CrashTestKind::InvalidParam: return "invalidparam";
    case CrashTestKind::StackOverflow: return "stackoverflow";
    case CrashTestKind::None: break;
    }
    return "none";
}

void TriggerTestCrash(CrashTestKind kind)
{
    switch (kind) {
    case CrashTestKind::AccessViolation: {
        // volatile 経由でないと最適化で「到達不能」に畳まれて落ちない構成がある
        volatile int* p = nullptr;
        *p = 0x0BADC0DE;
        break;
    }
    case CrashTestKind::PureCall: {
        PureCallDerived d;
        (void)d;
        break;
    }
    case CrashTestKind::Terminate:
        std::terminate();
        break;
    case CrashTestKind::InvalidParam: {
        // secure CRT は sizeInBytes > 0 を検証する = 不正パラメータハンドラへ入る
        char buf[4] = {};
        strcpy_s(buf, 0, "x");
        break;
    }
    case CrashTestKind::StackOverflow:
        (void)BlowTheStack(0);
        break;
    case CrashTestKind::None:
        break;
    }
}

} // namespace mye
