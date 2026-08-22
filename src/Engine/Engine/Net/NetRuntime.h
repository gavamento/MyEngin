#pragma once
#include <cstdint>

namespace mye {

// ネットセッションの「今の状態」を上位レイヤへ見せるための POD (M52i)。
//
// ★**この 1 個で Winsock も NetSession の実装も外へ出さない**のが目的。
//   読み手は 3 者:
//     ・エディタの NetWindow (Editor 層。Engine の内部型を掴ませたくない)
//     ・ABI v13 の NetXxx スロット (Script 層。DLL 境界は C ABI + POD のみ)
//     ・ログ / セルフテスト
//   EngineLoop が毎フレーム 1 回だけ書き、他は読むだけ。
//
// ★ここに載る値は**すべて機種依存 (実時間・ネットワーク状況・自分がどちら側か)**。
//   スクリプトから読めるようにはするが、**読んだ値を sim 状態へ書き戻してはいけない**。
//   書き戻した瞬間に 2 台のワールドハッシュが割れる — そして M52i の desync 検出は
//   まさにそれを毎 tick 見張っているので、誤用は「静かに壊れる」ではなく
//   「desync バンドルが出て止まる」形で必ず表面化する (それが唯一の防波堤)。
//   用途は表示・カメラ・UI といった描画レーンに限ること。
struct NetRuntimeInfo {
    bool active = false;        // --net-host / --net-join でセッションを張っている
    bool connected = false;     // ハンドシェイクが済んで入力交換中
    bool rollbackEnabled = false;
    int role = 0;               // NetRole の生値 (0=off 1=host 2=join)
    uint32_t localPlayer = 0;   // 自分が動かすレーン
    uint32_t playerCount = 1;
    uint32_t inputDelay = 0;
    float pingMs = 0.0f;

    // ---- 予測ロールバック ----
    uint32_t speculation = 0;      // いま何 tick 先行して予測実行しているか
    uint64_t predictedTicks = 0;   // 予測入力で走った tick の延べ数
    uint64_t rollbacks = 0;        // 巻き戻した回数
    uint64_t rollbackTicks = 0;    // 再シムした tick の延べ数
    uint64_t maxRollbackDepth = 0; // 一度に巻き戻した最大 tick 数

    // ---- 確定点と desync ----
    uint64_t confirmedTick = 0; // これ未満の tick は確定 (覆らない)
    uint64_t localHash = 0;     // 直近で確定した tick 末のワールドハッシュ
    uint64_t peerTick = 0;      // 相手が主張している確定 tick
    uint64_t peerHash = 0;
    bool desync = false;        // ハッシュが割れた (バンドルを吐いて停止済み)
    uint64_t desyncTick = 0;

    // ---- トランスポート統計 ----
    uint64_t packetsSent = 0;
    uint64_t packetsRecv = 0;
    uint64_t packetsDropped = 0; // --net-loss で故意に捨てた分
    uint64_t stalls = 0;
    double stallMs = 0.0;
};

} // namespace mye
