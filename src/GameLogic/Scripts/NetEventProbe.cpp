//====================================================================================
//                          NetEventProbe.cpp
//  MyEngin/ 秋田蓮音                                                     10/02/2026
//                                          ABI v23 (レーン状態 / 参加・離脱イベント) の恒久 probe
//====================================================================================
// 毎 tick システムイベントとレーン状態を読み、数えた結果を sim 状態 (登録フィールド) へ書く。
// 呼ぶだけでは「全員が同じ tick に同じイベントを読んだ」ことを WorldHash が見られないので、
// 読んだ値をハッシュ対象へ書き戻すのが本体 (FractureDamageProbe と同じ流儀)。
// どのシーンにも自動では付かない: server-net selftest が名前で引いて付ける。
#include "Shared/ScriptAPI.h"

struct NetEventProbe : Script<NetEventProbe> {
    int32_t joinCount = 0;        // Join を読んだ回数
    int32_t leaveCount = 0;       // Leave を読んだ回数
    int32_t rejoinCount = 0;
    int32_t releaseCount = 0;
    uint64_t lastEventSeq = 0;    // 直近に読んだイベントの eventSeq
    uint64_t lastEventTick = 0;   // そのイベントを読んだ tick
    uint32_t laneMask = 0;        // 直近 tick の Connected レーン
    uint64_t playerIdSum = 0;     // 直近 tick の全レーンの playerId の和

    void Update(MyeUpdateContext& ctx)
    {
        const uint32_t count = MyeNetSystemEventCount(ctx);
        for (uint32_t i = 0; i < count; ++i) {
            MyeNetSystemEvent e = {};
            if (!MyeNetGetSystemEvent(ctx, i, &e)) {
                continue;
            }
            switch (e.kind) {
            case MYE_NET_EVENT_JOIN: ++joinCount; break;
            case MYE_NET_EVENT_LEAVE: ++leaveCount; break;
            case MYE_NET_EVENT_REJOIN: ++rejoinCount; break;
            case MYE_NET_EVENT_RELEASE: ++releaseCount; break;
            default: break;
            }
            lastEventSeq = e.eventSeq;
            lastEventTick = ctx.tickIndex;
        }
        laneMask = MyeNetLaneMask(ctx);
        playerIdSum = 0;
        for (uint32_t lane = 0; lane < 4; ++lane) {
            playerIdSum += MyeNetLanePlayerId(ctx, lane);
        }
    }
};
REGISTER_SCRIPT(NetEventProbe,
                FIELDS(joinCount, leaveCount, rejoinCount, releaseCount, lastEventSeq, lastEventTick,
                       laneMask, playerIdSum));