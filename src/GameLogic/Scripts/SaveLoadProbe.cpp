//====================================================================================
//                          SaveLoadProbe.cpp
//  MyEngin/ 秋田蓮音                                                     10/02/2026
//                                          LoadGame / LoadPersist のネット中の境界を見る恒久 probe
//====================================================================================
// 指定 tick に LoadPersist / LoadGame を要求し、PersistStore の値を毎 tick sim 状態へ写す。
// ネット中・記録中・再生中はこの要求が no-op になる (TickServices::netLockstep の境界) ので、
// ディスクのセーブが sim へ入っていれば persistValue がハッシュに載って割れる。
// どのシーンにも自動では付かない: server-net selftest が名前で引いて付ける。
#include "Shared/ScriptAPI.h"

struct SaveLoadProbe : Script<SaveLoadProbe> {
    uint64_t loadPersistTick = 0; // この tick に LoadPersist を要求する (0 = しない)
    uint64_t loadGameTick = 0;    // この tick に LoadGame を要求する (0 = しない)
    int32_t slot = 0;
    int32_t requests = 0;         // 要求した回数
    int32_t persistValue = -1;    // 直近 tick の PersistStore "save_probe.value" (無ければ -1)

    void Update(MyeUpdateContext& ctx)
    {
        if (loadPersistTick != 0 && ctx.tickIndex == loadPersistTick) {
            MyeLoadPersist(ctx, slot);
            ++requests;
        }
        if (loadGameTick != 0 && ctx.tickIndex == loadGameTick) {
            MyeLoadGame(ctx, slot);
            ++requests;
        }
        persistValue = MyePersistGetInt(ctx, "save_probe.value", -1);
    }
};
REGISTER_SCRIPT(SaveLoadProbe, FIELDS(loadPersistTick, loadGameTick, slot, requests, persistValue));
