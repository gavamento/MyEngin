#include "Engine/Engine/Replay/TimeTravelSelfTest.h"

#include <cstdint>
#include <vector>

#include "Engine/Core/Components.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/Replay/TimeTravel.h"
#include "Engine/Engine/Scene.h"

namespace mye {
namespace {

// 被験シーン: ハッシュが tick ごとに変わるよう 1 つだけ動かす対象を持つ
EntityID BuildScene(Scene& scene)
{
    GameObject root = scene.CreateGameObjectTracked("Root");
    GameObject mover = scene.CreateGameObjectTracked("Mover");
    mover.SetParent(root);
    scene.GetWorld().ApplyStructuralChanges(); // 撮影点の前提 (構造変更が空)
    return mover.Id();
}

} // namespace

bool RunTimeTravelSelfTest()
{
    MYE_LOG_INFO("==== TimeTravel (ring policy) self test ====");
    int failCount = 0;
    const auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    Scene scene;
    const EntityID mover = BuildScene(scene);
    SimRefs refs;
    refs.scene = &scene;

    uint64_t tick = 100;
    // その tick が「走った」ことにして 1 本分をリングへ積む。
    // 世界を動かしてから積むので、entry.hashAfter は tick ごとに異なる値になる
    const auto RunTick = [&](TimeTravel& tt, bool simulated) {
        if (simulated) {
            if (auto* t = scene.GetWorld().GetComponent<LocalTransform>(mover)) {
                t->position.x += 1.0f;
            }
        }
        // M52g: レーン 2 本で積む。レーン 1 に別の目印を入れて「レーン 0 の値が
        // 全レーンへ配られていないか」までここで固定する
        InputSnapshot in[2] = {};
        in[0].mouseX = static_cast<int32_t>(tick); // 入力の同一性を後で照合するための目印
        in[1].mouseX = static_cast<int32_t>(tick) + 1000;
        tt.OnTickEnd(refs, tick, in, 2, simulated);
        ++tick;
    };

    TimeTravel tt;
    TimeTravelConfig cfg;
    cfg.snapshotInterval = 30;
    cfg.maxSnapshots = 120;
    tt.Configure(cfg);

    // ---- 開始 ----
    tt.SetEnabled(true);
    check(tt.BeginPending(), "SetEnabled(true) only asks - the ring starts at a tick boundary");
    tt.Begin(refs, tick);
    check(tt.Enabled() && !tt.BeginPending(), "Begin() starts the ring");
    check(tt.FirstTick() == 100 && tt.EndTick() == 100, "an empty ring is [100, 100)");
    check(tt.SnapshotCount() == 1, "Begin() takes the first snapshot");
    const uint64_t startHash = tt.HashAtTick(100);
    check(startHash != 0, "HashAtTick(firstTick) is the state before that tick runs");

    // ---- 90 tick 積む (30 tick ごとにスナップショット) ----
    for (int i = 0; i < 90; ++i) {
        RunTick(tt, true);
    }
    check(tt.EndTick() == 190, "90 ticks -> [100, 190)");
    check(tt.EntryCount() == 90, "one entry per tick");
    check(tt.SnapshotCount() == 4, "snapshots at 100 / 130 / 160 / 190");
    check(tt.Entry(100) != nullptr && tt.Entry(100)->inputs[0].mouseX == 100,
          "Entry(t) returns the input that tick consumed");
    check(tt.Entry(189) != nullptr && tt.Entry(189)->inputs[0].mouseX == 189,
          "...for the last tick");
    check(tt.Entry(150) != nullptr && tt.Entry(150)->inputs[1].mouseX == 1150,
          "each lane keeps its own snapshot (M52g)");
    check(tt.Entry(150) != nullptr && tt.Entry(150)->inputs[2].mouseX == 0,
          "lanes beyond playerCount stay zero");
    check(tt.Entry(190) == nullptr, "EndTick() itself has no entry (it has not run yet)");
    check(tt.HashAtTick(190) == tt.Entry(189)->hashAfter,
          "HashAtTick(t) is the hash after tick t-1");
    check(tt.HashAtTick(100) == startHash, "the first tick's hash is kept separately");
    check(tt.HashAtTick(150) != tt.HashAtTick(151), "a moving world hashes differently per tick");

    uint64_t snapTick = 0;
    check(tt.SnapshotAtOrBefore(175, snapTick) != nullptr && snapTick == 160,
          "SnapshotAtOrBefore(175) picks 160");
    check(tt.SnapshotAtOrBefore(160, snapTick) != nullptr && snapTick == 160,
          "an exact hit picks that snapshot (0 ticks to re-simulate)");
    check(tt.SnapshotAtOrBefore(99, snapTick) == nullptr, "before the ring there is nothing");

    // ---- ポーズ tick は撮影間隔に数えない ----
    const size_t snapsBeforePause = tt.SnapshotCount();
    for (int i = 0; i < 100; ++i) {
        RunTick(tt, false);
    }
    check(tt.SnapshotCount() == snapsBeforePause,
          "paused ticks do not take snapshots (a frozen world is not worth 148 KB a piece)");
    check(tt.EndTick() == 290, "...but they are still recorded (prevTickInput moves)");
    check(tt.Entry(200) != nullptr && !tt.Entry(200)->simulated,
          "a paused tick is recorded as simulated=false");

    // ---- 分岐: シークで戻った後に走った tick は未来を捨てる ----
    const uint64_t branchHash = tt.HashAtTick(150);
    tick = 150;
    RunTick(tt, true);
    check(tt.EndTick() == 151, "re-running tick 150 truncates the recorded future");
    check(tt.SnapshotCount() == 2, "snapshots after the branch point are dropped (100 / 130)");
    check(tt.HashAtTick(150) == branchHash,
          "the state *before* the branch tick is unchanged (its snapshot stays valid)");
    check(tt.SnapshotAtOrBefore(150, snapTick) != nullptr && snapTick == 130,
          "seeking into the branch still finds 130");

    // ---- 追い出し: 上限を超えたら古い方から捨て、戻れない範囲は見せない ----
    TimeTravelConfig tight;
    tight.snapshotInterval = 10;
    tight.maxSnapshots = 3;
    TimeTravel ring2;
    ring2.Configure(tight);
    tick = 0;
    ring2.Begin(refs, tick);
    for (int i = 0; i < 100; ++i) {
        RunTick(ring2, true);
    }
    check(ring2.SnapshotCount() == 3, "at most maxSnapshots are kept");
    check(ring2.FirstTick() == 80, "the range shrinks to the oldest snapshot (80 / 90 / 100)");
    check(ring2.EndTick() == 100, "the newest end is unchanged");
    check(ring2.Entry(79) == nullptr, "ticks we can no longer reach are dropped");
    check(ring2.HashAtTick(80) != 0 && ring2.HashAtTick(80) == ring2.HashAtTick(80),
          "the new first tick keeps a valid start hash");
    // 追い出し後も「最寄り探索 → その tick の入力が揃っている」が崩れないこと。
    // ここが崩れると再シムが途中で入力を見失う (シークが Failed になる)
    bool inputsIntact = true;
    if (ring2.SnapshotAtOrBefore(95, snapTick) == nullptr) {
        inputsIntact = false;
    } else {
        for (uint64_t t = snapTick; t < 95; ++t) {
            if (ring2.Entry(t) == nullptr) {
                inputsIntact = false;
            }
        }
    }
    check(inputsIntact, "every tick between the chosen snapshot and the target still has input");

    // ---- 停止 ----
    ring2.SetEnabled(false);
    check(!ring2.Enabled() && ring2.SnapshotCount() == 0 && ring2.EntryCount() == 0,
          "SetEnabled(false) drops the whole ring (Stop must not leak the play session)");

    if (failCount == 0) {
        MYE_LOG_INFO("==== TimeTravel self test: ALL PASS ====");
    } else {
        MYE_LOG_ERROR("==== TimeTravel self test: %d FAILED ====", failCount);
    }
    return failCount == 0;
}

} // namespace mye
