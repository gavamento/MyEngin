#include "Engine/Core/Log.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

#include <Windows.h>

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

} // namespace

void WriteV(LogLevel level, const char* fmt, va_list args)
{
    char buffer[1024];
    const int len = vsnprintf(buffer, sizeof(buffer), fmt, args);
    if (len < 0) {
        return;
    }

    // デバッガ / コンソール出力 (リングバッファ外のフル長)
    char line[1100];
    snprintf(line, sizeof(line), "[%s] %s\n", LevelTag(level), buffer);
    OutputDebugStringA(line);
    fputs(line, level >= LogLevel::Warn ? stderr : stdout);

    std::lock_guard<std::mutex> lock(g_mutex);
    LogEntry& e = g_ring[g_totalWritten & (kCapacity - 1)];
    e.level = level;
    e.frame = g_currentFrame.load(std::memory_order_relaxed);
    strncpy_s(e.message, buffer, _TRUNCATE);
    ++g_totalWritten;
}

void Write(LogLevel level, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    WriteV(level, fmt, args);
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
