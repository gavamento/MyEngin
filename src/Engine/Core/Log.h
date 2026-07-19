#pragma once
#include <cstdint>
#include <cstdarg>
#include <cstddef>

namespace mye {

enum class LogLevel : uint8_t { Trace = 0, Info = 1, Warn = 2, Error = 3 };

// Console ウィンドウ (M2) が読むリングバッファの 1 エントリ。固定長 POD。
struct LogEntry {
    LogLevel level;
    uint64_t frame;        // 記録時のフレーム番号
    char     message[240]; // null 終端 (超過分は切り捨て)
};

namespace logging {

void Write(LogLevel level, const char* fmt, ...);
void WriteV(LogLevel level, const char* fmt, va_list args);

// EngineLoop が毎フレーム設定する (LogEntry::frame 用)
void SetCurrentFrame(uint64_t frame);

// リングバッファ読み出し (Console 用)。
// cursor: 呼び出し側が保持する「次に読む通し番号」。古すぎて上書き済みの場合は自動的に前進する。
// 戻り値: out に書いた件数。
size_t ReadSince(uint64_t& cursor, LogEntry* out, size_t maxOut);

uint64_t TotalWritten();
void Clear();

} // namespace logging
} // namespace mye

#define MYE_LOG_TRACE(...) ::mye::logging::Write(::mye::LogLevel::Trace, __VA_ARGS__)
#define MYE_LOG_INFO(...)  ::mye::logging::Write(::mye::LogLevel::Info,  __VA_ARGS__)
#define MYE_LOG_WARN(...)  ::mye::logging::Write(::mye::LogLevel::Warn,  __VA_ARGS__)
#define MYE_LOG_ERROR(...) ::mye::logging::Write(::mye::LogLevel::Error, __VA_ARGS__)
