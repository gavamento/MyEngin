#include "Engine/Platform/Clock.h"

#include <Windows.h>

namespace mye {

void Clock::Init()
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    freq_ = f.QuadPart;
    start_ = c.QuadPart;
    last_ = c.QuadPart;
}

double Clock::BeginFrame()
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    const double dt = static_cast<double>(c.QuadPart - last_) / static_cast<double>(freq_);
    last_ = c.QuadPart;
    return dt;
}

double Clock::Now() const
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart - start_) / static_cast<double>(freq_);
}

} // namespace mye
