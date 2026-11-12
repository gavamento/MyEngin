#include "Engine/Core/Diagnostics/Profiler.h"

#include <Windows.h>

#include <cstring>

namespace mye::prof {
namespace {

constexpr int kMaxScopeDepth = 64;

struct OpenScope {
    int64_t start;
    int record; // g_records の添字。記録数の上限を超えて捨てたスコープは -1
};

std::vector<ScopeRecord> g_records; // 今フレームの生の記録 (開始順)
OpenScope g_stack[kMaxScopeDepth];
int g_stackTop = 0;
int g_droppedScopes = 0;
int64_t g_freq = 0;

// 履歴のリング。最初の BeginFrame で 1 度だけ確保する (以降は確保しない)
std::vector<FrameRecord> g_history;
int g_historyHead = 0;  // 次に書く位置
int g_historyCount = 0;
int64_t g_frameStart = 0; // 今フレームの BeginFrame 時刻 (0 = まだ 1 度も呼ばれていない)
FramePhases g_pendingPhases;
uint64_t g_pendingFrameIndex = 0;
uint64_t g_pendingTickIndex = 0;
const FrameRecord g_emptyFrame{};

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

float TicksToMs(int64_t ticks)
{
    return g_freq ? static_cast<float>(ticks) * 1000.0f / static_cast<float>(g_freq) : 0.0f;
}

bool SameName(const char* a, const char* b)
{
    return a == b || std::strcmp(a, b) == 0;
}

// 生の記録 (開始順) を「同じ親の下の同名」で合算して out.scopes へ詰める。
// 親は必ず子より先に現れるので、生の添字 → 合算後の添字の対応を前から順に引ける
void AggregateScopes(FrameRecord& out)
{
    static int rawToStat[kMaxRawScopes]; // メインスレッド専用の作業領域 (スタックに置くには大きい)
    out.scopeCount = 0;
    out.droppedScopes = g_droppedScopes;
    const int rawCount = static_cast<int>(g_records.size());
    for (int i = 0; i < rawCount; ++i) {
        const ScopeRecord& raw = g_records[static_cast<size_t>(i)];
        const int parentStat = (raw.parent >= 0) ? rawToStat[raw.parent] : -1;
        if (raw.parent >= 0 && parentStat < 0) {
            rawToStat[i] = -1; // 親が上限で落ちたので子も数えない
            ++out.droppedScopes;
            continue;
        }
        int found = -1;
        for (int s = 0; s < out.scopeCount; ++s) {
            if (out.scopes[s].parent == parentStat && SameName(out.scopes[s].name, raw.name)) {
                found = s;
                break;
            }
        }
        if (found < 0) {
            if (out.scopeCount >= kMaxFrameScopes) {
                rawToStat[i] = -1;
                ++out.droppedScopes;
                continue;
            }
            found = out.scopeCount++;
            out.scopes[found] = ScopeStat{ raw.name, 0.0f, 0, raw.depth, parentStat };
        }
        out.scopes[found].ms += raw.ms;
        ++out.scopes[found].calls;
        rawToStat[i] = found;
    }
}

// 今フレームの記録を履歴へ確定する。frameMs は BeginFrame の間隔 (壁時計)
void SealFrame(int64_t now)
{
    FrameRecord& out = g_history[static_cast<size_t>(g_historyHead)];
    out.frameIndex = g_pendingFrameIndex;
    out.tickIndex = g_pendingTickIndex;
    out.frameMs = TicksToMs(now - g_frameStart);
    out.phases = g_pendingPhases;
    AggregateScopes(out);
    out.render = g_render;
    for (int i = 0; i < kRenderStatsViewSlots; ++i) {
        out.renderByView[i] = g_renderByView[i];
    }
    g_historyHead = (g_historyHead + 1) % kFrameHistory;
    if (g_historyCount < kFrameHistory) {
        ++g_historyCount;
    }
}

} // namespace

void BeginFrame()
{
    if (g_freq == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_freq = f.QuadPart;
        g_records.reserve(static_cast<size_t>(kMaxRawScopes));
        g_history.resize(static_cast<size_t>(kFrameHistory));
    }
    const int64_t now = Now();
    if (g_frameStart != 0) {
        SealFrame(now);
    }
    g_frameStart = now;
    g_pendingPhases = {};
    g_pendingFrameIndex = 0;
    g_pendingTickIndex = 0;
    g_records.clear();
    g_stackTop = 0;
    g_droppedScopes = 0;
    g_render = {};
    for (RenderStats& v : g_renderByView) {
        v = {};
    }
    g_currentView = 0;
}

void PushScope(const char* name)
{
    if (g_stackTop >= 0 && g_stackTop < kMaxScopeDepth) {
        int record = -1;
        if (g_records.size() < static_cast<size_t>(kMaxRawScopes)) {
            // 親は「記録が残っている直近の祖先」。捨てた祖先は飛ばす
            int parent = -1;
            for (int i = g_stackTop - 1; i >= 0 && parent < 0; --i) {
                parent = g_stack[i].record;
            }
            record = static_cast<int>(g_records.size());
            g_records.push_back({ name, 0.0f, g_stackTop, parent });
        } else {
            ++g_droppedScopes;
        }
        g_stack[g_stackTop] = { Now(), record };
    }
    ++g_stackTop;
}

void PopScope()
{
    --g_stackTop;
    if (g_stackTop >= 0 && g_stackTop < kMaxScopeDepth) {
        const OpenScope& o = g_stack[g_stackTop];
        if (o.record >= 0) {
            g_records[static_cast<size_t>(o.record)].ms = TicksToMs(Now() - o.start);
        }
    }
}

const std::vector<ScopeRecord>& FrameScopes()
{
    return g_records;
}

void SetFramePhases(const FramePhases& phases, uint64_t frameIndex, uint64_t tickIndex)
{
    g_pendingPhases = phases;
    g_pendingFrameIndex = frameIndex;
    g_pendingTickIndex = tickIndex;
}

int HistoryCount()
{
    return g_historyCount;
}

const FrameRecord& History(int age)
{
    if (age < 0 || age >= g_historyCount) {
        return g_emptyFrame;
    }
    const int index = (g_historyHead - 1 - age + kFrameHistory * 2) % kFrameHistory;
    return g_history[static_cast<size_t>(index)];
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
        r.culledDistance += d.culledDistance;
        r.culledScreenSize += d.culledScreenSize;
        r.culledNonMesh += d.culledNonMesh;
        r.unculledGpuEmitters += d.unculledGpuEmitters;
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
