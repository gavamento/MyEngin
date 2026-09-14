#include "Engine/Engine/Replay/TimeTravelSelfTest.h"

#include <cstdint>
#include <vector>

#include "Engine/Core/Components.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/Replay/GhostTrack.h"
#include "Engine/Engine/Replay/InputOverride.h"
#include "Engine/Engine/Replay/SimSnapshot.h"
#include "Engine/Engine/Replay/TimeTravel.h"
#include "Engine/Engine/Replay/WorldHasher.h"
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
        tt.OnTickEnd(refs, tick, in, 2, simulated, TimeTravel::HashOf(refs));
        ++tick;
    };
    // 「シークで tick t へ戻った」の代わり: そのレーンの最寄りスナップショットを復元する
    // (テストは再シムを持たないので、スナップショットそのものの tick へしか戻れない)
    const auto RestoreOn = [&](TimeTravel& tt, uint32_t lane, uint64_t t) {
        uint64_t got = 0;
        const std::vector<std::byte>* blob = tt.SnapshotAtOrBeforeOn(lane, t, got);
        return blob != nullptr && got == t && RestoreSimSnapshot(refs, blob->data(), blob->size());
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
    check(!tt.NeedsBoundaryCheck(100), "a fresh ring needs no boundary check");

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
    check(tt.HashAtTick(160) == tt.Entry(159)->hashAfter,
          "a snapshot tick hashes the same through the snapshot and through the entry");
    check(!tt.NeedsBoundaryCheck(190), "a running ring needs no boundary check at its end");

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
    check(tt.NeedsBoundaryCheck(290), "after a paused tick the next boundary is checked (edits)");
    const uint64_t oldEndHash = tt.HashAtTick(290);
    const uint64_t oldHash130 = tt.HashAtTick(130);

    // ---- 縮退経路: Fork を呼ばずにリングの途中で tick が走る = 未来は分岐へ移る (M72a) ----
    const uint64_t branchHash = tt.HashAtTick(150);
    tick = 150;
    RunTick(tt, true);
    check(tt.EndTick() == 151, "re-running tick 150 ends the live lane at 151");
    check(tt.SnapshotCount() == 2, "live snapshots after the fork point move away (100 / 130 stay)");
    check(tt.HashAtTick(150) == branchHash,
          "the state *before* the branch tick is unchanged (its hash stays valid)");
    check(tt.SnapshotAtOrBefore(150, snapTick) != nullptr && snapTick == 130,
          "seeking into the branch point still finds 130");
    check(tt.BranchCount() == 1, "the recorded future is kept as one branch");
    const uint32_t b1 = tt.BranchCount() == 1 ? tt.Branches().front().id : 0;
    check(b1 == 1 && tt.ForkTickOn(b1) == 150 && tt.EndTickOn(b1) == 290,
          "branch B1 owns [150, 290)");
    check(tt.FindBranch(b1) != nullptr && tt.FindBranch(b1)->parent == TimeTravel::kLiveLane,
          "B1 hangs off the live lane");
    check(tt.EntryOn(b1, 200) != nullptr && !tt.EntryOn(b1, 200)->simulated
              && tt.EntryOn(b1, 200)->inputs[0].mouseX == 200,
          "EntryOn(B1, t) reads the moved future");
    check(tt.EntryOn(b1, 120) != nullptr && tt.EntryOn(b1, 120)->inputs[0].mouseX == 120,
          "EntryOn(B1, t) walks to the parent before the fork tick");
    check(tt.HashAtTickOn(b1, 150) == branchHash && tt.HashAtTickOn(b1, 290) == oldEndHash,
          "HashAtTickOn(B1) covers the fork tick and the old end");
    check(tt.SnapshotAtOrBeforeOn(b1, 175, snapTick) != nullptr && snapTick == 160,
          "SnapshotAtOrBeforeOn(B1, 175) uses the moved 160");
    check(tt.SnapshotAtOrBeforeOn(b1, 150, snapTick) != nullptr && snapTick == 130,
          "SnapshotAtOrBeforeOn(B1, 150) walks to the parent's 130");
    check(tt.SnapshotAtOrBefore(175, snapTick) != nullptr && snapTick == 130,
          "the live lane no longer sees the branch's snapshots");
    {
        const DivergenceReport d = tt.FirstDivergence(TimeTravel::kLiveLane, b1);
        check(d.comparable && d.diverged && d.firstTick == 151,
              "the re-run tick moved the world differently -> first divergence at 151");
    }

    // ---- 正規経路: Fork (未来あり・編集なし) ----
    check(RestoreOn(tt, TimeTravel::kLiveLane, 130), "restore the live snapshot at 130");
    tick = 130;
    check(tt.NeedsBoundaryCheck(130), "a tick with recorded future needs the boundary check");
    const uint32_t b2 = tt.Fork(refs, 130);
    check(b2 == 2, "Fork(130) with a recorded future creates B2");
    check(tt.EndTick() == 130 && tt.EndTickOn(b2) == 151 && tt.ForkTickOn(b2) == 130,
          "live ends at 130, B2 owns [130, 151)");
    check(tt.SnapshotCount() == 2 && tt.HashAtTick(130) == oldHash130,
          "live keeps 100 + a pinned re-capture at 130 with the same hash (no edit)");
    check(tt.FindBranch(b1) != nullptr && tt.FindBranch(b1)->parent == b2,
          "B1 (forked later at 150) is re-parented under B2");
    check(tt.EntryOn(b1, 140) != nullptr && tt.EntryOn(b1, 140)->inputs[0].mouseX == 140,
          "B1 reads [130, 150) through B2");
    check(tt.SnapshotAtOrBeforeOn(b1, 149, snapTick) != nullptr && snapTick == 130,
          "B1's nearest snapshot for 149 is B2's 130 (moved with the suffix)");
    check(tt.FindBranch(b2) != nullptr && !tt.FindBranch(b2)->snapshots.empty()
              && tt.FindBranch(b2)->snapshots.front().pinned,
          "the fork-tick snapshot a branch inherits is pinned");
    check(!tt.NeedsBoundaryCheck(130), "right after Fork the boundary is settled");

    // ---- 正規経路: Fork (未来なし・編集あり) = 潜在バグの回帰 ----
    if (auto* t = scene.GetWorld().GetComponent<LocalTransform>(mover)) {
        t->position.y += 5.0f; // ポーズ中に Inspector が触った、の代わり
    }
    const uint64_t editedHash = TimeTravel::HashOf(refs);
    check(editedHash != oldHash130, "the edit changes the world hash");
    const uint32_t none = tt.Fork(refs, 130);
    check(none == 0, "Fork(130) with no future creates no branch");
    check(tt.SnapshotCount() == 2 && tt.HashAtTick(130) == editedHash,
          "...but re-captures the edited state (the stale snapshot at 130 is replaced)");
    // M73b: 撮り直した pinned は前進シークの「編集点」— 手前から跨ぐシークは復元から行く
    check(tt.HasEditPointBetween(100, 130) && tt.HasEditPointBetween(129, 130)
              && !tt.HasEditPointBetween(130, 200) && !tt.HasEditPointBetween(100, 129),
          "the re-captured snapshot is an edit point for forward seeks that cross it");
    {
        const DivergenceReport d = tt.FirstDivergence(TimeTravel::kLiveLane, b2);
        check(d.comparable && d.diverged && d.firstTick == 130,
              "the edit itself shows up as a divergence at the fork tick");
    }
    check(RestoreOn(tt, b2, 130) && TimeTravel::HashOf(refs) == oldHash130,
          "B2's own snapshot at 130 is still the pre-edit state");
    check(RestoreOn(tt, TimeTravel::kLiveLane, 130) && TimeTravel::HashOf(refs) == editedHash,
          "seeking back to the fork tick on the live lane keeps the edit (M52e lost it)");
    for (int i = 0; i < 20; ++i) {
        RunTick(tt, true); // 130..149 を編集後の世界で走らせる
    }
    check(tt.EndTick() == 150, "20 ticks on the edited lane -> [100, 150)");
    check(tt.FirstDivergence(TimeTravel::kLiveLane, b2).firstTick == 130,
          "the divergence stays at the edit point");

    // ---- レーン切替: B1 をライブへ (経路 B2 -> B1 を継ぎ足し、いまのライブは分岐に降格) ----
    uint64_t seekTarget = 0;
    check(tt.SwitchToBranch(b1, 150, seekTarget), "SwitchToBranch(B1) succeeds");
    check(seekTarget == 150, "the seek target is min(now, new end)");
    check(tt.EndTick() == 290 && tt.SnapshotCount() == 4,
          "live is [100, 290) again with snapshots 100 / 130 / 160 / 190");
    check(tt.HashAtTick(290) == oldEndHash && tt.HashAtTick(130) == oldHash130,
          "the grafted lane hashes like the original run");
    check(tt.FindBranch(b1) == nullptr && tt.FindBranch(b2) == nullptr,
          "the grafted branches are gone");
    check(tt.BranchCount() == 2, "the edited run and B2's own tail survive as branches");
    uint32_t edited = 0;
    uint32_t tail = 0;
    for (const TimeTravelBranch& b : tt.Branches()) {
        if (b.forkTick == 130) {
            edited = b.id;
        } else if (b.forkTick == 150) {
            tail = b.id;
        }
    }
    check(edited != 0 && tt.EndTickOn(edited) == 150 && tt.HashAtTickOn(edited, 130) == editedHash
              && tt.FindBranch(edited)->parent == TimeTravel::kLiveLane,
          "the demoted live lane is a branch at 130 that starts from the edited state");
    check(tail != 0 && tt.EndTickOn(tail) == 151 && tt.FindBranch(tail)->parent == TimeTravel::kLiveLane,
          "B2's continuation past 150 became its own branch off the live lane");
    check(tt.FirstDivergence(TimeTravel::kLiveLane, edited).firstTick == 130,
          "live vs edited branch: diverges at the edit");
    {
        const DivergenceReport d = tt.FirstDivergence(TimeTravel::kLiveLane, tail);
        check(d.comparable && d.diverged && d.firstTick == 151,
              "live vs tail: same state before 150, different after");
    }

    // ---- 同一分岐の畳み込み: 同じ入力で同じ未来をなぞったら分岐は消える ----
    check(RestoreOn(tt, TimeTravel::kLiveLane, 190), "restore the live snapshot at 190");
    tick = 190;
    const uint32_t b5 = tt.Fork(refs, 190);
    check(b5 != 0 && tt.EndTick() == 190 && tt.BranchCount() == 3, "Fork(190) keeps [190, 290) as B5");
    for (int i = 0; i < 100; ++i) {
        RunTick(tt, false); // 元の 190..289 もポーズ tick だった
    }
    check(tt.EndTick() == 290 && tt.HashAtTick(290) == oldEndHash,
          "re-tracing the paused stretch reproduces the old end hash");
    check(tt.FindBranch(b5) == nullptr && tt.BranchCount() == 2,
          "an identical branch collapses when the live lane reaches its end");

    // ---- 部分木の削除 ----
    tt.DeleteBranch(edited);
    check(tt.FindBranch(edited) == nullptr && tt.BranchCount() == 1, "DeleteBranch removes it");

    // ---- 追い出し: 上限を超えたら古い方から捨て、戻れない範囲は見せない ----
    TimeTravelConfig tight;
    tight.snapshotInterval = 10;
    tight.maxSnapshots = 3;
    TimeTravel ring2;
    ring2.Configure(tight);
    tick = 0;
    ring2.Begin(refs, tick);
    for (int i = 0; i < 50; ++i) {
        RunTick(ring2, true);
    }
    // 50 tick 走った時点で最古は 30 (30 / 40 / 50)。その範囲内の 40 で縮退分岐させる
    tick = 40;
    RunTick(ring2, true); // 縮退経路で [40, 50) が分岐になる
    check(ring2.BranchCount() == 1 && ring2.ForkTickOn(ring2.Branches().front().id) == 40,
          "ring2 forked a branch at 40");
    for (int i = 0; i < 100; ++i) {
        RunTick(ring2, true);
    }
    check(ring2.SnapshotCount() == 3, "at most maxSnapshots are kept");
    check(ring2.FirstTick() == 120, "the range shrinks to the oldest snapshot (120 / 130 / 140)");
    check(ring2.EndTick() == 141, "the newest end is unchanged");
    check(ring2.Entry(119) == nullptr, "ticks we can no longer reach are dropped");
    check(ring2.HashAtTick(120) != 0, "the new first tick keeps a valid start hash");
    check(ring2.BranchCount() == 0, "a branch whose fork tick left the ring is pruned");
    // 追い出し後も「最寄り探索 → その tick の入力が揃っている」が崩れないこと。
    // ここが崩れると再シムが途中で入力を見失う (シークが Failed になる)
    bool inputsIntact = true;
    if (ring2.SnapshotAtOrBefore(135, snapTick) == nullptr) {
        inputsIntact = false;
    } else {
        for (uint64_t t = snapTick; t < 135; ++t) {
            if (ring2.Entry(t) == nullptr) {
                inputsIntact = false;
            }
        }
    }
    check(inputsIntact, "every tick between the chosen snapshot and the target still has input");

    // ---- 分岐の本数上限: 最古の葉から消える (いま作ったものは守る) ----
    TimeTravelConfig few;
    few.snapshotInterval = 10;
    few.maxBranches = 2;
    TimeTravel ring3;
    ring3.Configure(few);
    tick = 0;
    ring3.Begin(refs, tick);
    for (int i = 0; i < 30; ++i) {
        RunTick(ring3, true);
    }
    for (int k = 0; k < 3; ++k) {
        tick = 10;
        RunTick(ring3, true); // 毎回 [10, ...) が分岐になる
    }
    check(ring3.BranchCount() == 2, "no more than maxBranches survive");
    check(ring3.FindBranch(1) == nullptr && ring3.FindBranch(3) != nullptr,
          "the oldest leaf went, the newest fork is protected");

    // ---- ゴーストの採取 (M72d): 疎化 / KeyAt / 墓標 / 予算 ----
    {
        GameObject g = scene.CreateGameObjectTracked("Ghosted");
        if (g.GetComponent<WorldMatrixComponent>() == nullptr) {
            g.AddComponent<WorldMatrixComponent>();
        }
        g.AddComponent<MeshRendererComponent>();
        scene.GetWorld().ApplyStructuralChanges();
        World& w = scene.GetWorld();
        const auto SetX = [&](float x) {
            if (auto* wm = w.GetComponent<WorldMatrixComponent>(g.Id())) {
                wm->value._41 = x;
            }
        };
        GhostTrack ghost;
        ghost.Begin(10);
        SetX(0.0f);
        ghost.Sample(w, 10, 1 << 20);
        ghost.Sample(w, 11, 1 << 20);
        ghost.Sample(w, 12, 1 << 20);
        size_t slot = ghost.entities.size();
        for (size_t i = 0; i < ghost.entities.size(); ++i) {
            if (ghost.entities[i].id == g.Id()) {
                slot = i;
            }
        }
        check(slot < ghost.entities.size(), "Sample registers an entity with WorldMatrix + MeshRenderer");
        GhostTrack tiny;
        tiny.Begin(10);
        check(!tiny.Sample(w, 10, 1) && tiny.truncated, "a budget of 1 byte truncates immediately");
        check(slot < ghost.entities.size() && ghost.entities[slot].keys.size() == 1,
              "an unmoved entity keeps a single key across ticks (sparse)");
        SetX(3.0f);
        ghost.Sample(w, 13, 1 << 20);
        check(slot < ghost.entities.size() && ghost.entities[slot].keys.size() == 2,
              "a moved entity gets a new key");
        const GhostKey* k12 = ghost.KeyAt(slot, 12);
        const GhostKey* k13 = ghost.KeyAt(slot, 13);
        const GhostKey* k99 = ghost.KeyAt(slot, 99);
        check(k12 != nullptr && k12->tick == 10 && k12->m[9] == 0.0f, "KeyAt(12) is the first key");
        check(k13 != nullptr && k13->tick == 13 && k13->m[9] == 3.0f, "KeyAt(13) is the moved key");
        check(k99 == k13 && ghost.KeyAt(slot, 9) == nullptr,
              "KeyAt holds the last key forward and nothing before the first");
        check(ghost.MovingCount() == 1 && ghost.lastTick == 13, "MovingCount / lastTick follow");
        DirectX::XMFLOAT4X4 m;
        GhostTrack::ToMatrix(*k13, m);
        check(m._41 == 3.0f && m._22 == 1.0f && m._44 == 1.0f, "ToMatrix rebuilds the 4x4");
        w.DestroyEntity(g.Id());
        w.ApplyStructuralChanges();
        ghost.Sample(w, 14, 1 << 20);
        const GhostKey* k14 = ghost.KeyAt(slot, 14);
        check(k14 != nullptr && k14->alive == 0 && k14->tick == 14,
              "a destroyed entity gets one tombstone key");
        ghost.Sample(w, 15, 1 << 20);
        check(slot < ghost.entities.size() && ghost.entities[slot].keys.size() == 3,
              "...and only one (no tombstone per tick)");
    }

    // ---- 入力の上書き (M72f): 経路の選択 / OR と置換 / レーンと tick の範囲 ----
    {
        InputActionDef keyAct;
        keyAct.name = "Jump";
        keyAct.keys.push_back(0x20); // Space
        keyAct.padMask = 0x1000;     // A も持つが、キーが先に選ばれる
        InputActionDef padAct;
        padAct.name = "Fire";
        padAct.padMask = 0x0300; // LB | RB → 最下位の LB (0x0100)
        InputAxisDef axis;
        axis.name = "MoveX";
        axis.posKey = 0x44; // D
        axis.negKey = 0x41; // A
        axis.padAxis = PadAxis::LX;
        InputAxisDef stick;
        stick.name = "LookX";
        stick.padAxis = PadAxis::RX;

        InputOverrideSet set;
        set.items.push_back(InputOverride::HoldAction(keyAct, 0, 10, 20));
        set.items.push_back(InputOverride::HoldAction(padAct, 1, 10, 20));
        set.items.push_back(InputOverride::HoldAxis(axis, -1.0f, 0, 15, 16));
        set.items.push_back(InputOverride::HoldAxis(stick, 0.5f, 0, 10, 20));
        InputSnapshot lanes[kMaxPlayers] = {};
        lanes[0].padRX = 123;
        set.Apply(9, lanes, 2);
        check(!lanes[0].KeyDown(0x20) && lanes[1].padButtons == 0 && lanes[0].padRX == 123,
              "before fromTick nothing is applied");
        set.Apply(10, lanes, 2);
        check(lanes[0].KeyDown(0x20) && lanes[0].padButtons == 0,
              "HoldAction prefers the key binding over the pad button");
        check(lanes[1].padButtons == 0x0100 && lanes[1].padConnected == 1,
              "a pad-only action holds its lowest button and marks the pad connected");
        check(lanes[0].padRX == 16384 && !lanes[0].KeyDown(0x44) && !lanes[0].KeyDown(0x41),
              "HoldAxis on a stick-only axis replaces the stick value");
        InputSnapshot again[kMaxPlayers] = {};
        set.Apply(15, again, 2);
        check(again[0].KeyDown(0x41) && !again[0].KeyDown(0x44),
              "HoldAxis with a negative value presses the negKey");
        InputSnapshot one[kMaxPlayers] = {};
        set.Apply(10, one, 1);
        check(one[1].padButtons == 0, "lanes beyond playerCount are left alone");
        InputSnapshot late[kMaxPlayers] = {};
        set.Apply(20, late, 2);
        check(!late[0].KeyDown(0x20) && late[1].padButtons == 0, "toTick is exclusive");
        check(std::string(set.items[0].label) == "Jump", "the label carries the action name");
    }

    // ---- フィールド差分の報告行 (M72g): DiffHashDumps の outReport ----
    {
        World& w = scene.GetWorld();
        HashDump a;
        HashDump b;
        HashWorldDump(w, {}, 7, a);
        if (auto* t = w.GetComponent<LocalTransform>(mover)) {
            t->position.z += 2.0f;
        }
        HashWorldDump(w, {}, 7, b);
        std::vector<std::string> rows;
        const HashDumpDiff d = DiffHashDumps(a, b, 8, &rows);
        check(d.valueDiffs == 1 && rows.size() == 1, "one changed field -> one report row");
        check(!rows.empty() && rows.front().find("Mover") != std::string::npos
                  && rows.front().find("LocalTransform.position") != std::string::npos,
              "the row names the entity and the component.field");
        int tabs = 0;
        for (char c : rows.empty() ? std::string() : rows.front()) {
            tabs += (c == '\t') ? 1 : 0;
        }
        check(tabs == 4, "the row has 5 tab-separated columns");
    }

    // ---- ホールドとステップ (M73a) ----
    // エディタの Pause = Hold (tick を止める)、Step = RequestStep(1) → 1 tick →
    // ConsumeStepBudget が真 → Hold。要点は「ホールド中の Inspector 編集を再開時の Fork が
    // 拾う」= Hold が境界チェックを要求すること (上の「ポーズ tick」の検査と対になる —
    // ホールドはポーズ tick を積まないので、第 3 条件の代わりに scrubbedSinceLastTick_ で拾う)
    {
        TimeTravel ringHold;
        ringHold.Configure(cfg);
        ringHold.Hold();
        check(!ringHold.Scrubbing(), "Hold before Begin is a no-op (no ring = nothing to hold)");
        tick = 5000;
        ringHold.SetEnabled(true);
        ringHold.Begin(refs, tick);
        for (int i = 0; i < 5; ++i) {
            RunTick(ringHold, true);
        }
        check(!ringHold.NeedsBoundaryCheck(ringHold.EndTick()),
              "simulated ticks at the ring end need no boundary check (baseline)");
        ringHold.Hold();
        check(ringHold.Scrubbing(), "Hold stops the ticks (Scrubbing)");
        check(ringHold.NeedsBoundaryCheck(ringHold.EndTick()),
              "a hold requests the boundary check (edits during the hold are re-captured by Fork)");
        check(!ringHold.ConsumeStepBudget(), "no step budget while merely held");
        ringHold.RequestStep(1);
        check(!ringHold.Scrubbing(), "RequestStep releases the hold for the budget");
        RunTick(ringHold, true);
        check(ringHold.ConsumeStepBudget(), "a 1-tick budget is exhausted by exactly one tick");
        ringHold.Hold(); // EngineLoop が tick 末でこう呼ぶ
        check(ringHold.Scrubbing() && ringHold.EndTick() == 5006,
              "...and the ring is held again one tick later");
        check(ringHold.NeedsBoundaryCheck(ringHold.EndTick()),
              "the re-hold requests the boundary check again (OnTickEnd cleared it, Hold set it back)");
        check(!ringHold.ConsumeStepBudget(), "a consumed budget stays consumed");
        ringHold.RequestStep(2);
        RunTick(ringHold, true);
        check(!ringHold.ConsumeStepBudget(), "a 2-tick budget is not exhausted after one tick");
        ringHold.EndScrub();
        RunTick(ringHold, true);
        check(!ringHold.ConsumeStepBudget(), "EndScrub (Play) drops the remaining budget");
        ringHold.Hold();
        ringHold.Clear();
        check(!ringHold.Scrubbing() && !ringHold.ConsumeStepBudget(),
              "Clear drops the hold and the budget");
    }

    // ---- 停止 ----
    ring2.SetEnabled(false);
    check(!ring2.Enabled() && ring2.SnapshotCount() == 0 && ring2.EntryCount() == 0
              && ring2.BranchCount() == 0,
          "SetEnabled(false) drops the whole ring (Stop must not leak the play session)");

    if (failCount == 0) {
        MYE_LOG_INFO("==== TimeTravel self test: ALL PASS ====");
    } else {
        MYE_LOG_ERROR("==== TimeTravel self test: %d FAILED ====", failCount);
    }
    return failCount == 0;
}

} // namespace mye
