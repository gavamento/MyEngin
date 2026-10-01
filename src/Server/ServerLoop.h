//====================================================================================
//                          ServerLoop.h
//  MyEngin/ 秋田蓮音                                                     10/02/2026
//                                          専用サーバの実運用ループ (UDP・60Hz のペース・ホスティング・.rep 記録)
//====================================================================================
#pragma once
#include <cstdint>
#include <string>

#include "Engine/Engine/Session/SessionTypes.h"

namespace mye {

class HeadlessSim;
class IHostingProvider;

struct ServerLoopConfig {
    uint16_t port = 7777;
    // role = Server。人数 / 入力遅延 / 締め切り / 予約期間 / configBits / UI 基準解像度 / フォント計測表は埋めてあること
    SessionConfig session = {};
    uint32_t lossPercent = 0;        // 送信パケットを故意に捨てる割合 (検証用。sim には入らない)
    std::wstring replayRecordPath;   // 空でなければ確定 tick を .rep (v9、開始スナップショット埋め込み) へ記録する
    int64_t tickLimit = 0;           // > 0: 確定 tick がこの数に達したら終了 (検証用の打ち切り)
    bool exitWhenEmpty = false;      // 1 人でも参加したあと、全員が出ていったら終了する
    uint32_t emptyGraceMs = 2000;    // exitWhenEmpty の判定を安定させる猶予
    uint32_t timeoutSec = 0;         // > 0: 実時間でこれを超えたら必ず終了する (検証の保険)
    uint32_t statsIntervalSec = 5;   // 統計ログの間隔 (sim の外。0 = 終了時だけ)
};

// 終了コード
inline constexpr int kServerExitOk = 0;
inline constexpr int kServerExitFailed = 1;
inline constexpr int kServerExitTimeout = 5; // timeoutSec に達して打ち切った

// 実時間 60Hz で ServerSession を回す。1 周の処理順は spec 4.1.4 のとおり固定:
//   受信 → ホスティングの出来事 → 締め切り判定と確定 → RunTick → .rep へ記録 → 送信
// ★実時間・受信順・ホスティングの都合は、ServerSession::TryConfirm が返す確定入力
//   (レーン入力 + SystemInputTick) の値に変換されてここで尽きる。sim へ入る経路はそれ 1 本だけ。
// sim は呼び出し側が Init 済みであること (システム入力あり、レーン数 = session.playerCount)。
// 戻り値は kServerExit*
int RunServerLoop(HeadlessSim& sim, IHostingProvider& hosting, const ServerLoopConfig& cfg);

} // namespace mye
