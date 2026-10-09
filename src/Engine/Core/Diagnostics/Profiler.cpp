#include "Engine/Core/Diagnostics/Profiler.h"

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

RenderStats g_render;                              // 全ビューの累積値
RenderStats g_renderByView[kRenderStatsViewSlots]; // viewKey 別 (和が g_render と一致する)
int g_currentView = 0;

// 累積値と現在ビューの集計の両方へ同じ加算をかける (和の一致をここ 1 箇所で保つ)
template <typename Fn>
void Accumulate(Fn&& fn)
{
    fn(g_render);
    fn(g_renderByView[g_currentView]);
}

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
    for (RenderStats& v : g_renderByView) {
        v = {};
    }
    g_currentView = 0;
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

void AddDraw(int triangles, int lod, int instances)
{
    const int slot = (lod < 0) ? 0 : ((lod >= kRenderStatsLodSlots) ? kRenderStatsLodSlots - 1 : lod);
    Accumulate([&](RenderStats& r) {
        ++r.drawCalls;
        r.triangles += triangles;
        r.lodDraws[slot] += instances;
        r.lodTriangles[slot] += triangles;
    });
}

void AddCulled(int n)
{
    Accumulate([&](RenderStats& r) { r.culled += n; });
}

void AddShadowDraw(int triangles, int cascade)
{
    Accumulate([&](RenderStats& r) {
        ++r.shadowDrawCalls;
        r.shadowTriangles += triangles;
        if (cascade >= 0 && cascade < kRenderStatsCascadeSlots) {
            ++r.shadowCascadeDraws[cascade];
        }
    });
}

void AddRenderStats(const RenderStats& d)
{
    Accumulate([&](RenderStats& r) {
        r.drawCalls += d.drawCalls;
        r.triangles += d.triangles;
        r.culled += d.culled;
        r.shadowDrawCalls += d.shadowDrawCalls;
        r.shadowTriangles += d.shadowTriangles;
        for (int i = 0; i < kRenderStatsCascadeSlots; ++i) {
            r.shadowCascadeDraws[i] += d.shadowCascadeDraws[i];
        }
        r.shadowCasterCandidates += d.shadowCasterCandidates;
        for (int i = 0; i < kRenderStatsCascadeSlots; ++i) {
            r.shadowCascadeCasters[i] += d.shadowCascadeCasters[i];
        }
        for (int i = 0; i < kRenderStatsLodSlots; ++i) {
            r.lodDraws[i] += d.lodDraws[i];
            r.lodTriangles[i] += d.lodTriangles[i];
        }
        r.paletteEvaluated += d.paletteEvaluated;
        r.paletteReused += d.paletteReused;
        r.occlusionPhase1Draws += d.occlusionPhase1Draws;
        r.occlusionPhase2Draws += d.occlusionPhase2Draws;
        r.occluded += d.occluded;
    });
}

void SetRenderStatsView(uint32_t viewKey)
{
    g_currentView = (viewKey < static_cast<uint32_t>(kRenderStatsViewSlots)) ? static_cast<int>(viewKey) : 0;
}

RenderStats GetRenderStats()
{
    return g_render;
}

RenderStats GetRenderStatsForView(uint32_t viewKey)
{
    return (viewKey < static_cast<uint32_t>(kRenderStatsViewSlots)) ? g_renderByView[viewKey] : RenderStats{};
}

} // namespace mye::prof
