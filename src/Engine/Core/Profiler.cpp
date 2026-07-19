#include "Engine/Core/Profiler.h"

#include <Windows.h>

namespace mye::prof {
namespace {

struct OpenScope {
    const char* name;
    int64_t start;
    int depth;
};

std::vector<ScopeRecord> g_records; // 今フレームの確定済み (深さ順ではなく Pop 順)
OpenScope g_stack[64];
int g_stackTop = 0;
int64_t g_freq = 0;

RenderStats g_render;

int64_t Now()
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}

} // namespace

void BeginFrame()
{
    if (g_freq == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_freq = f.QuadPart;
    }
    g_records.clear();
    g_stackTop = 0;
    g_render = {};
}

void PushScope(const char* name)
{
    if (g_stackTop < 64) {
        g_stack[g_stackTop] = { name, Now(), g_stackTop };
    }
    ++g_stackTop;
}

void PopScope()
{
    --g_stackTop;
    if (g_stackTop >= 0 && g_stackTop < 64) {
        const OpenScope& o = g_stack[g_stackTop];
        const float ms = g_freq ? static_cast<float>(Now() - o.start) * 1000.0f
                                      / static_cast<float>(g_freq)
                                : 0.0f;
        g_records.push_back({ o.name, ms, o.depth });
    }
}

const std::vector<ScopeRecord>& FrameScopes()
{
    return g_records;
}

void AddDraw(int triangles)
{
    ++g_render.drawCalls;
    g_render.triangles += triangles;
}

RenderStats GetRenderStats()
{
    return g_render;
}

} // namespace mye::prof
