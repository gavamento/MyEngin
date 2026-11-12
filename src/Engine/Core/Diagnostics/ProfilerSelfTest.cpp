#include "Engine/Core/Diagnostics/ProfilerSelfTest.h"

#include <chrono>
#include <cstring>
#include <string>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Diagnostics/Profiler.h"

namespace mye {

namespace {

// 確定フレームから「親 parent の下の name」を探す。無ければ -1
int FindStat(const prof::FrameRecord& frame, const char* name, int parent)
{
    for (int i = 0; i < frame.scopeCount; ++i) {
        if (frame.scopes[i].parent == parent && std::strcmp(frame.scopes[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

} // namespace

bool RunProfilerSelfTest()
{
    MYE_LOG_INFO("==== Profiler self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    // ---- 生の記録: 開始順と親の添字 ----
    prof::BeginFrame();
    {
        MYE_PROFILE_SCOPE("tick");
        {
            MYE_PROFILE_SCOPE("physics");
            MYE_PROFILE_SCOPE("phys.solve");
        }
        MYE_PROFILE_SCOPE("animation");
    }
    {
        // 2 tick 目: physics は同じ親の下の同名なので合算される
        MYE_PROFILE_SCOPE("tick");
        MYE_PROFILE_SCOPE("physics");
    }
    {
        // ルート直下の physics は「親が違う」ので別の行になる
        MYE_PROFILE_SCOPE("physics");
    }
    const std::vector<prof::ScopeRecord>& raw = prof::FrameScopes();
    check(raw.size() == 7, "raw records: one per PushScope");
    if (raw.size() == 7) {
        check(std::strcmp(raw[0].name, "tick") == 0 && raw[0].parent == -1 && raw[0].depth == 0,
              "raw[0] is the root scope (start order, not pop order)");
        check(std::strcmp(raw[1].name, "physics") == 0 && raw[1].parent == 0 && raw[1].depth == 1,
              "raw[1] is the first child, parent index 0");
        check(std::strcmp(raw[2].name, "phys.solve") == 0 && raw[2].parent == 1 && raw[2].depth == 2,
              "raw[2] is the grandchild, parent index 1");
        check(std::strcmp(raw[3].name, "animation") == 0 && raw[3].parent == 0,
              "raw[3] is the second child of the root");
        check(raw[6].parent == -1 && raw[6].depth == 0, "raw[6] is a root-level scope");
    }

    prof::FramePhases phases;
    phases.tickMs = 1.5f;
    phases.presentMs = 2.5f;
    phases.ticks = 2;
    prof::SetFramePhases(phases, 77, 123);
    prof::BeginFrame(); // ここで上のフレームが確定する

    // ---- 確定フレーム: 合算 ----
    const prof::FrameRecord& sealed = prof::History(0);
    check(sealed.frameIndex == 77 && sealed.tickIndex == 123, "sealed frame carries frame / tick index");
    check(sealed.phases.tickMs == 1.5f && sealed.phases.presentMs == 2.5f && sealed.phases.ticks == 2,
          "sealed frame carries the phases set before BeginFrame");
    check(sealed.frameMs >= 0.0f, "sealed frame has a wall-clock duration");
    check(sealed.scopeCount == 5, "aggregated: tick, physics, phys.solve, animation, root physics");
    const int tick = FindStat(sealed, "tick", -1);
    check(tick >= 0 && sealed.scopes[tick].calls == 2, "tick x2 merged into one row");
    const int physics = FindStat(sealed, "physics", tick);
    check(physics >= 0 && sealed.scopes[physics].calls == 2 && sealed.scopes[physics].depth == 1,
          "physics under tick x2 merged");
    check(FindStat(sealed, "phys.solve", physics) >= 0, "grandchild keeps its aggregated parent");
    const int rootPhysics = FindStat(sealed, "physics", -1);
    check(rootPhysics >= 0 && rootPhysics != physics && sealed.scopes[rootPhysics].calls == 1,
          "same name under a different parent stays a separate row");
    bool parentsFirst = true;
    for (int i = 0; i < sealed.scopeCount; ++i) {
        parentsFirst = parentsFirst && sealed.scopes[i].parent < i;
    }
    check(parentsFirst, "every aggregated row comes after its parent");
    check(sealed.droppedScopes == 0, "nothing dropped in a small frame");

    // SetFramePhases を呼ばなかったフレームは 0 のまま (前フレームの値を持ち越さない)
    prof::BeginFrame();
    check(prof::History(0).phases.tickMs == 0.0f && prof::History(0).scopeCount == 0,
          "a frame without SetFramePhases / scopes seals as empty");
    check(prof::History(1).frameIndex == 77, "age 1 is the frame before the latest");

    // ---- 上限: 生の記録 ----
    for (int i = 0; i < prof::kMaxRawScopes + 10; ++i) {
        MYE_PROFILE_SCOPE("flood");
    }
    check(prof::FrameScopes().size() == static_cast<size_t>(prof::kMaxRawScopes),
          "raw records stop at kMaxRawScopes");
    prof::BeginFrame();
    check(prof::History(0).scopeCount == 1 && prof::History(0).scopes[0].calls == prof::kMaxRawScopes
              && prof::History(0).droppedScopes == 10,
          "overflowing raw records are dropped and counted");

    // ---- 履歴のリング ----
    for (int i = 0; i < prof::kFrameHistory + 5; ++i) {
        prof::SetFramePhases(prof::FramePhases{}, static_cast<uint64_t>(1000 + i), 0);
        prof::BeginFrame();
    }
    check(prof::HistoryCount() == prof::kFrameHistory, "history is capped at kFrameHistory");
    check(prof::History(0).frameIndex == static_cast<uint64_t>(1000 + prof::kFrameHistory + 4),
          "age 0 is the most recent frame after wrap-around");
    check(prof::History(prof::kFrameHistory - 1).frameIndex == 1005, "oldest kept frame after wrap-around");
    check(prof::History(prof::kFrameHistory).scopeCount == 0 && prof::History(-1).frameIndex == 0,
          "out-of-range age returns the empty record");

    // ---- GPU パスの申告 ----
    prof::ReportGpu("ssr", 1.0f, false);
    prof::ReportGpu("gbuffer", 2.0f, true);
    prof::ReportGpu("ssr", 3.0f, true); // 同名は後勝ち
    prof::ReportGpuFrame(5.0f);
    prof::BeginFrame();
    {
        const prof::FrameRecord& gpuFrame = prof::History(0);
        check(gpuFrame.gpuCount == 2 && gpuFrame.gpuFrameMs == 5.0f, "gpu passes are sealed with the frame");
        check(gpuFrame.gpuCount == 2 && std::strcmp(gpuFrame.gpu[0].name, "ssr") == 0 && gpuFrame.gpu[0].ms == 3.0f
                  && gpuFrame.gpu[0].ran,
              "a pass reported twice keeps the last report");
    }
    prof::BeginFrame();
    check(prof::History(0).gpuCount == 0 && prof::History(0).gpuFrameMs == 0.0f,
          "gpu passes do not carry over to the next frame");

    // ---- 一時停止 ----
    const uint64_t beforePause = prof::History(0).frameIndex;
    prof::SetHistoryPaused(true);
    prof::SetFramePhases(prof::FramePhases{}, 9999, 0);
    prof::BeginFrame();
    check(prof::IsHistoryPaused() && prof::History(0).frameIndex == beforePause,
          "a paused history does not seal new frames");
    prof::SetHistoryPaused(false);

    // ---- 重いフレームの控え ----
    // しきい値を「必ず超える」「絶対に超えない」の両端に置いて、実時間に依存させない
    prof::ClearCaptures();
    prof::SetCaptureThresholdMs(1.0e9f);
    prof::BeginFrame();
    prof::BeginFrame();
    check(prof::CaptureCount() == 0, "frames under the threshold are not captured");
    prof::SetCaptureThresholdMs(1.0e-9f);
    for (int i = 0; i < prof::kMaxCaptures + 3; ++i) {
        prof::SetFramePhases(prof::FramePhases{}, static_cast<uint64_t>(5000 + i), 0);
        // フレームの長さが時計の分解能未満 (0 ms) だと「超えた」にならないので、時計が進むまで回す
        // (sleep は Windows のタイマー分解能で 1 回 15 ms 前後かかり、スイートが遅くなる)
        const auto spinStart = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - spinStart < std::chrono::microseconds(20)) {
        }
        prof::BeginFrame();
    }
    check(prof::CaptureCount() == prof::kMaxCaptures, "captures are capped at kMaxCaptures");
    check(prof::Capture(0).frameIndex == 5003 && prof::Capture(prof::kMaxCaptures - 1).frameIndex
                                                      == static_cast<uint64_t>(5000 + prof::kMaxCaptures + 2),
          "the oldest captures are dropped first");
    check(prof::Capture(prof::kMaxCaptures).frameIndex == 0, "out-of-range capture returns the empty record");
    prof::SetCaptureThresholdMs(0.0f);
    prof::BeginFrame();
    check(prof::CaptureCount() == prof::kMaxCaptures, "threshold 0 turns capturing off");
    prof::ClearCaptures();
    check(prof::CaptureCount() == 0, "ClearCaptures empties the list");

    // ---- 項目別の計測 ----
    prof::BeginFrame();
    {
        // 名前は呼び出しの間だけ生きていればよい: 別々のバッファに同じ文字列を入れて渡す
        const std::string nameA = "Enemy";
        const std::string nameACopy = "Enemy";
        prof::AddItemSample("script", nameA.c_str(), 7, 1, 1.0f);
        prof::AddItemSample("script", nameA.c_str(), 7, 1, 2.0f);     // 同じ組 → 合算
        prof::AddItemSample("script", nameACopy.c_str(), 8, 1, 4.0f); // 別エンティティ、別ポインタの同名
        prof::AddItemSample("script.cs", nameA.c_str(), 7, 1, 8.0f);  // 分類が違えば別の行
    }
    prof::BeginFrame();
    {
        const prof::FrameRecord& frame = prof::History(0);
        check(frame.itemCount == 3 && frame.droppedItems == 0, "items: one row per (category, name, entity)");
        check(frame.itemNameCount == 2, "item names: one row per (category, name)");
        float scriptTotal = 0.0f;
        int scriptCalls = 0;
        bool nameSurvives = false;
        for (int i = 0; i < frame.itemNameCount; ++i) {
            if (std::strcmp(frame.itemNames[i].category, "script") == 0) {
                scriptTotal = frame.itemNames[i].ms;
                scriptCalls = frame.itemNames[i].calls;
                nameSurvives = std::strcmp(frame.itemNames[i].name, "Enemy") == 0;
            }
        }
        check(scriptTotal == 7.0f && scriptCalls == 3, "item names: same text from different buffers is merged");
        check(nameSurvives, "item names outlive the caller's string (prof keeps a copy)");
        bool merged = false;
        for (int i = 0; i < frame.itemCount; ++i) {
            merged = merged || (frame.items[i].entityIndex == 7 && frame.items[i].calls == 2
                                && frame.items[i].ms == 3.0f);
        }
        check(merged, "items: repeated samples for one entity are summed");
    }
    // 上限: エンティティ別は捨てて数え、名前別の合計は正確なまま
    for (int i = 0; i < prof::kMaxFrameItems + 5; ++i) {
        prof::AddItemSample("script", "Flood", static_cast<uint32_t>(i), 1, 1.0f);
    }
    prof::BeginFrame();
    check(prof::History(0).itemCount == prof::kMaxFrameItems && prof::History(0).droppedItems == 5,
          "items: rows past the limit are dropped and counted");
    check(prof::History(0).itemNameCount == 1
              && prof::History(0).itemNames[0].calls == prof::kMaxFrameItems + 5,
          "item names: totals stay exact when entity rows overflow");
    prof::BeginFrame();
    check(prof::History(0).itemCount == 0 && prof::History(0).itemNameCount == 0,
          "items do not carry over to the next frame");
    // ItemScope は計測が止まっていれば何も積まない
    prof::SetItemTimingEnabled(false);
    {
        const prof::ItemScope off("script", "Off", 1, 1);
    }
    prof::SetItemTimingEnabled(true);
    {
        const prof::ItemScope on("script", "On", 1, 1);
    }
    prof::SetItemTimingEnabled(false);
    prof::BeginFrame();
    check(prof::History(0).itemCount == 1 && std::strcmp(prof::History(0).items[0].name, "On") == 0,
          "ItemScope records only while item timing is enabled");

    MYE_LOG_INFO("==== Profiler self test: %s ====", failCount == 0 ? "PASS" : "FAIL");
    return failCount == 0;
}

} // namespace mye
