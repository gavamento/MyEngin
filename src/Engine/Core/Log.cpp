#include "Engine/Core/Log.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <mutex>

#include <Windows.h>

#include "Engine/Core/Utf8.h"

namespace mye::logging {
namespace {

constexpr size_t kCapacity = 4096; // 2 のべき乗であること

std::mutex g_mutex;
LogEntry g_ring[kCapacity];
uint64_t g_totalWritten = 0;
std::atomic<uint64_t> g_currentFrame{0};

const char* LevelTag(LogLevel level)
{
    switch (level) {
    case LogLevel::Trace: return "TRACE";
    case LogLevel::Info:  return "INFO ";
    case LogLevel::Warn:  return "WARN ";
    case LogLevel::Error: return "ERROR";
    }
    return "?????";
}

// UTF-8 のまま外へ出す (M47a)。ログには日本語が混ざるので:
//   - OutputDebugStringA は ANSI (CP932) として解釈するため W 版へ UTF-16 で渡す
//   - コンソールに直結しているときは WriteConsoleW。リダイレクト/パイプのときは
//     UTF-8 バイトをそのまま流す (受け側のエンコーディングに委ねる)
// SetConsoleOutputCP(CP_UTF8) は使わない — コードページは「コンソール」側の状態で
// プロセス終了後も残る。AttachConsole(ATTACH_PARENT_PROCESS) で親の cmd に相乗り
// している以上、呼び出し元のシェルを 65001 のまま壊してしまう
void EmitUtf8(const char* utf8, bool isError)
{
    wchar_t wide[1200];
    const int wlen =
        MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, static_cast<int>(std::size(wide)));
    if (wlen <= 0) {
        // 変換不能 (不正 UTF-8 / 長すぎ) は従来経路へフォールバック
        OutputDebugStringA(utf8);
        fputs(utf8, isError ? stderr : stdout);
        return;
    }
    OutputDebugStringW(wide);

    const HANDLE h = GetStdHandle(isError ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (h != nullptr && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode) != 0) {
        DWORD written = 0;
        WriteConsoleW(h, wide, static_cast<DWORD>(wlen - 1), &written, nullptr); // -1 = 終端を除く
    } else {
        fputs(utf8, isError ? stderr : stdout);
    }
}

} // namespace

void WriteSrcV(LogLevel level, const char* file, int line, const char* fmt, va_list args)
{
    char buffer[1024];
    const int len = vsnprintf(buffer, sizeof(buffer), fmt, args);
    if (len < 0) {
        return;
    }

    // デバッガ / コンソール出力 (リングバッファ外のフル長)
    char out[1100];
    snprintf(out, sizeof(out), "[%s] %s\n", LevelTag(level), buffer);
    EmitUtf8(out, level >= LogLevel::Warn);

    std::lock_guard<std::mutex> lock(g_mutex);
    LogEntry& e = g_ring[g_totalWritten & (kCapacity - 1)];
    e.level = level;
    e.frame = g_currentFrame.load(std::memory_order_relaxed);
    // strncpy_s(_TRUNCATE) はバイト境界で切るのでマルチバイト列を分断しうる。
    // Console/StatusBar/Toast は UTF-8 前提なので、文字単位で落とす (M47a)
    utf8::CopyTruncated(e.message, sizeof(e.message), buffer);
    if (file) {
        utf8::CopyTruncated(e.file, sizeof(e.file), file);
    } else {
        e.file[0] = '\0';
    }
    e.line = line;
    ++g_totalWritten;
}

void WriteSrc(LogLevel level, const char* file, int line, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    WriteSrcV(level, file, line, fmt, args);
    va_end(args);
}

void WriteV(LogLevel level, const char* fmt, va_list args)
{
    WriteSrcV(level, nullptr, 0, fmt, args);
}

void Write(LogLevel level, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    WriteSrcV(level, nullptr, 0, fmt, args);
    va_end(args);
}

void SetCurrentFrame(uint64_t frame)
{
    g_currentFrame.store(frame, std::memory_order_relaxed);
}

size_t ReadSince(uint64_t& cursor, LogEntry* out, size_t maxOut)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint64_t oldest = (g_totalWritten > kCapacity) ? g_totalWritten - kCapacity : 0;
    if (cursor < oldest) {
        cursor = oldest; // 上書きで失われた分はスキップ
    }
    size_t count = 0;
    while (cursor < g_totalWritten && count < maxOut) {
        out[count++] = g_ring[cursor & (kCapacity - 1)];
        ++cursor;
    }
    return count;
}

uint64_t TotalWritten()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_totalWritten;
}

void Clear()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_totalWritten = 0;
}

} // namespace mye::logging
