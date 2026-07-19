#include "Engine/Core/Check.h"
#include "Engine/Core/Log.h"

#include <cstdarg>
#include <cstdio>

#include <Windows.h>
#include <intrin.h>

namespace mye::detail {

void CheckFailed(const char* expr, const char* file, int line, const char* fmt, ...)
{
    char detail[512] = {};
    if (fmt) {
        va_list args;
        va_start(args, fmt);
        vsnprintf(detail, sizeof(detail), fmt, args);
        va_end(args);
    }

    if (detail[0]) {
        MYE_LOG_ERROR("CHECK failed: (%s) %s [%s:%d]", expr, detail, file, line);
    } else {
        MYE_LOG_ERROR("CHECK failed: (%s) [%s:%d]", expr, file, line);
    }

    if (IsDebuggerPresent()) {
        __debugbreak();
    }
}

} // namespace mye::detail
