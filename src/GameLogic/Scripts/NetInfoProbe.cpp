//====================================================================================
//                          NetInfoProbe.cpp
//  MyEngin/ 秋田蓮音                                                     10/02/2026
//                                          v13 の Net* (NetIsConnected / NetPlayerCount) を sim へ書く検証専用 probe
//====================================================================================
// v13 の値は表示用 (機種依存) で、sim へ書き戻すのはゲームの誤用。それでもサーバ構成のゲームが書いたとき、
// ライブ (サーバ / クライアント) とオフライン再生が同じ値を読んで割れないことを server-net selftest が確かめる
// (spec D14 / V12)。読んだ値をハッシュ対象へ書くのが本体。
// どのシーンにも自動では付かない: selftest が名前で引いて付ける。
#include "Shared/ScriptAPI.h"

struct NetInfoProbe : Script<NetInfoProbe> {
    int32_t connectedTicks = 0;  // NetIsConnected が 1 だった tick の数
    uint32_t playerCountSeen = 0; // 直近 tick の NetPlayerCount
    uint64_t playerCountSum = 0;  // 全 tick の NetPlayerCount の和

    void Update(MyeUpdateContext& ctx)
    {
        connectedTicks += MyeNetIsConnected(ctx) ? 1 : 0;
        playerCountSeen = MyeNetPlayerCount(ctx);
        playerCountSum += playerCountSeen;
    }
};
REGISTER_SCRIPT(NetInfoProbe, FIELDS(connectedTicks, playerCountSeen, playerCountSum));
