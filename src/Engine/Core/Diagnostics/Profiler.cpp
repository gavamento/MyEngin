#include "Engine/Core/Diagnostics/Profiler.h"

#include <Windows.h>

#include <cstring>
#include <deque>
#include <string>

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
bool g_historyPaused = false;

std::vector<FrameRecord> g_captures; // 履歴と同じく最初の BeginFrame で 1 度だけ確保する
int g_captureHead = 0;
int g_captureCount = 0;
float g_captureThresholdMs = 0.0f;

// ---- 項目別の計測 ----
// エンティティ別の表は開番地法のハッシュで引く (スクリプト呼び出しごとに線形探索すると、
// 計測そのものが測りたい時間を歪める)。キーの名前は呼び出し側のポインタで比べ、表に残す名前は写しにする
constexpr int kItemSlots = 1024; // kMaxFrameItems の 4 倍 (2 の冪)。負荷率 25% 以下
struct ItemRow {
    const char* rawName = nullptr; // 呼び出し側のポインタ (今フレームの間だけキーに使う)
    ItemStat stat;
};
bool g_itemTiming = false;
ItemRow g_items[kMaxFrameItems];
int g_itemCount = 0;
int g_droppedItems = 0;
int16_t g_itemSlots[kItemSlots]; // 行の添字 + 1。0 = 空 (ゼロ初期化のままで「全部空」になる)
ItemStat g_itemNames[kMaxFrameItemNames];
int g_itemNameCount = 0;
// 名前の写し。deque なので要素のアドレスは変わらない。種類はスクリプト型の数程度なので消さない
std::deque<std::string> g_internedNames;

const char* InternName(const char* name)
{
    for (const std::string& known : g_internedNames) {
        if (known == name) {
            return known.c_str();
        }
    }
    g_internedNames.emplace_back(name);
    return g_internedNames.back().c_str();
}

void ResetItems()
{
    g_itemCount = 0;
    g_itemNameCount = 0;
    g_droppedItems = 0;
    std::memset(g_itemSlots, 0, sizeof(g_itemSlots));
}

GpuStat g_gpu[kMaxGpuPasses];
int g_gpuCount = 0;
float g_gpuFrameMs = 0.0f;

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
    out.itemCount = g_itemCount;
    out.itemNameCount = g_itemNameCount;
    out.droppedItems = g_droppedItems;
    for (int i = 0; i < g_itemCount; ++i) {
        out.items[i] = g_items[i].stat;
    }
    for (int i = 0; i < g_itemNameCount; ++i) {
        out.itemNames[i] = g_itemNames[i];
    }
    out.gpuFrameMs = g_gpuFrameMs;
    out.gpuCount = g_gpuCount;
    for (int i = 0; i < g_gpuCount; ++i) {
        out.gpu[i] = g_gpu[i];
    }
    out.render = g_render;
    for (int i = 0; i < kRenderStatsViewSlots; ++i) {
        out.renderByView[i] = g_renderByView[i];
    }
    if (g_captureThresholdMs > 0.0f && out.frameMs > g_captureThresholdMs) {
        g_captures[static_cast<size_t>(g_captureHead)] = out;
        g_captureHead = (g_captureHead + 1) % kMaxCaptures;
        if (g_captureCount < kMaxCaptures) {
            ++g_captureCount;
        }
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
        g_captures.resize(static_cast<size_t>(kMaxCaptures));
    }
    const int64_t now = Now();
    if (g_frameStart != 0 && !g_historyPaused) {
        SealFrame(now);
    }
    g_gpuCount = 0;
    g_gpuFrameMs = 0.0f;
    ResetItems();
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

void SetItemTimingEnabled(bool enabled)
{
    g_itemTiming = enabled;
}

bool ItemTimingEnabled()
{
    return g_itemTiming;
}

void AddItemSample(const char* category, const char* name, uint32_t entityIndex, uint32_t entityGeneration,
                   float ms)
{
    // ---- エンティティ別 ----
    // 分類はハッシュに混ぜない: 同じ綴りのリテラルでも呼び出し箇所ごとにアドレスが違いうる
    // (Debug は文字列プールが無効) ので、分類は下で文字列として比べる
    const uint64_t mixed = (reinterpret_cast<uintptr_t>(name) >> 3) * 0x9E3779B97F4A7C15ull
                           ^ (static_cast<uint64_t>(entityIndex) * 0xC2B2AE3D27D4EB4Full);
    uint32_t slot = static_cast<uint32_t>(mixed >> 40) & (kItemSlots - 1);
    const char* interned = nullptr;
    for (;;) {
        const int row = g_itemSlots[slot] - 1;
        if (row < 0) {
            interned = InternName(name);
            if (g_itemCount < kMaxFrameItems) {
                g_itemSlots[slot] = static_cast<int16_t>(g_itemCount + 1);
                ItemRow& added = g_items[g_itemCount++];
                added.rawName = name;
                added.stat = ItemStat{ category, interned, entityIndex, entityGeneration, ms, 1 };
            } else {
                ++g_droppedItems;
            }
            break;
        }
        ItemRow& existing = g_items[row];
        if (existing.rawName == name && SameName(existing.stat.category, category)
            && existing.stat.entityIndex == entityIndex && existing.stat.entityGeneration == entityGeneration) {
            existing.stat.ms += ms;
            ++existing.stat.calls;
            interned = existing.stat.name;
            break;
        }
        slot = (slot + 1) & (kItemSlots - 1);
    }

    // ---- 名前別 (写しのポインタで比べる。種類が少ないので線形で足りる) ----
    for (int i = 0; i < g_itemNameCount; ++i) {
        if (g_itemNames[i].name == interned && SameName(g_itemNames[i].category, category)) {
            g_itemNames[i].ms += ms;
            ++g_itemNames[i].calls;
            return;
        }
    }
    if (g_itemNameCount < kMaxFrameItemNames) {
        g_itemNames[g_itemNameCount++] = ItemStat{ category, interned, 0, 0, ms, 1 };
    }
}

ItemScope::ItemScope(const char* category, const char* name, uint32_t entityIndex, uint32_t entityGeneration)
    : category_(category)
    , name_(name)
    , entityIndex_(entityIndex)
    , entityGeneration_(entityGeneration)
    , start_(g_itemTiming ? Now() : 0)
{
}

ItemScope::~ItemScope()
{
    if (start_ != 0) {
        AddItemSample(category_, name_, entityIndex_, entityGeneration_, TicksToMs(Now() - start_));
    }
}

void ReportGpu(const char* name, float ms, bool ran)
{
    for (int i = 0; i < g_gpuCount; ++i) {
        if (SameName(g_gpu[i].name, name)) {
            g_gpu[i] = GpuStat{ name, ms, ran };
            return;
        }
    }
    if (g_gpuCount < kMaxGpuPasses) {
        g_gpu[g_gpuCount++] = GpuStat{ name, ms, ran };
    }
}

void ReportGpuFrame(float ms)
{
    g_gpuFrameMs = ms;
}

void SetHistoryPaused(bool paused)
{
    g_historyPaused = paused;
}

bool IsHistoryPaused()
{
    return g_historyPaused;
}

void SetCaptureThresholdMs(float ms)
{
    g_captureThresholdMs = ms;
}

float CaptureThresholdMs()
{
    return g_captureThresholdMs;
}

void ClearCaptures()
{
    g_captureHead = 0;
    g_captureCount = 0;
}

int CaptureCount()
{
    return g_captureCount;
}

const FrameRecord& Capture(int index)
{
    if (index < 0 || index >= g_captureCount) {
        return g_emptyFrame;
    }
    const int oldest = (g_captureHead - g_captureCount + kMaxCaptures) % kMaxCaptures;
    return g_captures[static_cast<size_t>((oldest + index) % kMaxCaptures)];
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
