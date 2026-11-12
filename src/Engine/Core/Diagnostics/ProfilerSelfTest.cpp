#include "Engine/Core/Diagnostics/ProfilerSelfTest.h"

#include <cstring>

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

    MYE_LOG_INFO("==== Profiler self test: %s ====", failCount == 0 ? "PASS" : "FAIL");
    return failCount == 0;
}

} // namespace mye
