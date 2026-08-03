#include "Engine/Core/Format.h"

#include <cstdio>
#include <vector>

namespace mye {

std::string FormatV(const char* fmt, va_list args)
{
    if (fmt == nullptr) {
        return std::string();
    }
    // vsnprintf は args を消費するのでコピーしてから長さを測る
    va_list probe;
    va_copy(probe, args);
    const int len = std::vsnprintf(nullptr, 0, fmt, probe);
    va_end(probe);
    if (len <= 0) {
        return std::string();
    }
    std::vector<char> buf(static_cast<size_t>(len) + 1);
    std::vsnprintf(buf.data(), buf.size(), fmt, args);
    return std::string(buf.data(), static_cast<size_t>(len));
}

std::string Format(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    std::string out = FormatV(fmt, args);
    va_end(args);
    return out;
}

} // namespace mye
