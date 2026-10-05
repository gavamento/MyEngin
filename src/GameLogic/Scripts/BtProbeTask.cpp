//====================================================================================
//                          BtProbeTask.cpp
//  MyEngin/ 秋田蓮音                                                     10/06/2026
//                                          ABI v27 (BT の C++ タスク / BB / イベント) の恒久 probe
//====================================================================================
// targetTicks 回目の OnTick で outcome を返すタスク。数えた tick を BB (probeTicks) へ書き、入った回数 (probeStarts) と
// Abort された時点の tick (probeAbortedAt) も BB へ残す。終わるときは自分宛てに BtProbeDone を送る。
// どのシーンにも自動では付かない: BehaviorTreeSelfTest が .bt.json の CppTask ノードから名前で引く。
#include "Shared/ScriptAPI.h"

namespace {

int32_t ReadBbInt(const MyeBtTaskContext& ctx, const char* key)
{
    MyeBbValue value = {};
    if (ctx.api->BtGetBlackboard(ctx.api->engine, ctx.self, MyeNameHash(key), &value) == 0 || value.isSet == 0) {
        return 0;
    }
    return value.i;
}

void WriteBbInt(const MyeBtTaskContext& ctx, const char* key, int32_t v)
{
    MyeBbValue value = {};
    value.type = MYE_BB_INT;
    value.isSet = 1;
    value.i = v;
    ctx.api->BtSetBlackboard(ctx.api->engine, ctx.self, MyeNameHash(key), &value);
}

} // namespace

struct BtProbeTask {
    int32_t targetTicks = 3;           // 何回目の OnTick で終わるか
    int32_t outcome = MYE_BT_SUCCESS;  // 終わるときの戻り値 (MyeBtStatus)
    int32_t ticks = 0;                 // 数えた OnTick の回数 (状態)

    int32_t OnStart(MyeBtTaskContext& ctx)
    {
        ticks = 0;
        WriteBbInt(ctx, "probeStarts", ReadBbInt(ctx, "probeStarts") + 1);
        return MYE_BT_RUNNING;
    }

    int32_t OnTick(MyeBtTaskContext& ctx)
    {
        ++ticks;
        WriteBbInt(ctx, "probeTicks", ticks);
        if (ticks < targetTicks) {
            return MYE_BT_RUNNING;
        }
        MyeBtEventPayload payload = {};
        payload.intValue = ticks;
        ctx.api->BtSendEvent(ctx.api->engine, ctx.self, ctx.self, MyeNameHash("BtProbeDone"), &payload);
        return outcome;
    }

    void OnAbort(MyeBtTaskContext& ctx)
    {
        WriteBbInt(ctx, "probeAbortedAt", ticks);
    }
};
REGISTER_BT_TASK(BtProbeTask, FIELDS(targetTicks, outcome, ticks));
