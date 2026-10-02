//====================================================================================
//                          ServerLoop.h
//  MyEngin/ 秋田蓮音                                                     10/02/2026
//                                          専用サーバの実運用ループ (UDP・60Hz のペース・ホスティング・.rep 記録)
//====================================================================================
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "Engine/Engine/Replay/Replay.h"
#include "Engine/Engine/Session/SessionTypes.h"
#include "Engine/Platform/Net/UdpSocket.h"
#include "Server/Hosting/IHostingProvider.h"

namespace mye {

class HeadlessSim;

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
    // 検証用: 確定 tick がこの数に達した直後に、意図的にプロセスを落とす (--crash-test / --crash-at-tick)。
    // 逐次記録した .rep とクラッシュバンドルの検証に使う。0 = 無効
    uint8_t crashTestKind = 0;       // CrashTestKind
    int64_t crashAtTick = 0;
};

// .rep の逐次書き出しの flush 間隔 (tick)。異常終了で失う記録は最大でこの長さ (60 tick = 1 秒)
inline constexpr uint32_t kServerReplayFlushTicks = 60;

// 終了コード
inline constexpr int kServerExitOk = 0;
inline constexpr int kServerExitFailed = 1;
inline constexpr int kServerExitTimeout = 5; // timeoutSec に達して打ち切った

// Poll が返した出来事の解釈結果。待機中の周回も主ループも同じ関数 (InterpretHostingEvents) を通す
struct HostingDecision {
    bool startSession = false;
    HostingSessionRequest session = {};
    bool terminate = false; // StartSession と同じ Poll で来ても落とさない (呼び出し側が始めてすぐ閉じる)
};
void InterpretHostingEvents(const std::vector<HostingEvent>& events, HostingDecision& decision);

// セッションの幕引き。記録中の .rep を閉じ (逐次モードなら tickCount を書き戻し)、そのあとで
// ホスティングへ終わりを知らせる。ホスティングの Terminate (Ctrl+C / GameLift の OnProcessTerminate)・
// 全員退出・tick 上限・タイムアウトのどれで終わるときも、この 1 関数を通る。
// 戻り値: .rep を正しく閉じられた (記録していなければ true)
bool CloseSession(ReplayRecorder& recorder, const std::wstring& recordPath, IHostingProvider& hosting);

// 宛先 (IPv4:port) → ServerSession の不透明な peer キー。キーは 1 始まりの通し番号で、再利用しない
// (ServerSession はレーンの持ち主をキーで覚えているので、古い持ち主と新しい宛先が同じキーを持つと取り違える)。
// 再接続は新しいエフェメラルポートから来るので、宛先は Sweep で回収しないと表が埋まり Hello を受けられなくなる
class PeerTable {
public:
    static constexpr uint32_t kMaxAddrs = 1024; // 見知らぬ宛先で表が膨らむのを止める上限 (回収後の同時保持数)

    // 既知ならそのキー。未知なら create のときだけ登録して返す。0 = 見つからない / 登録できない。
    // 満杯で Hello を捨てるときは ERROR ログ (回収されるまで 1 回だけ)
    uint32_t KeyOf(const NetAddress& from, bool create);
    bool AddressOf(uint32_t key, NetAddress& out) const;
    // keep(key) が false のキーの宛先を表から外す。外した数を返す
    template <class KeepFn>
    size_t Sweep(KeepFn&& keep)
    {
        const size_t before = entries_.size();
        for (size_t i = 0; i < entries_.size();) {
            if (keep(entries_[i].key)) {
                ++i;
            } else {
                entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
            }
        }
        if (entries_.size() < kMaxAddrs) {
            fullReported_ = false;
        }
        return before - entries_.size();
    }
    size_t Size() const { return entries_.size(); }
    uint64_t HellosIgnoredWhileFull() const { return ignoredWhileFull_; }

private:
    struct Entry {
        NetAddress addr;
        uint32_t key = 0;
    };
    std::vector<Entry> entries_;
    uint32_t nextKey_ = 1; // 0 は「無し」の予約
    bool fullReported_ = false;
    uint64_t ignoredWhileFull_ = 0;
};

// 実時間 60Hz で ServerSession を回す。1 周の処理順は spec 4.1.4 のとおり固定:
//   受信 → ホスティングの出来事 → 締め切り判定と確定 → RunTick → .rep へ記録 → 送信
// ★実時間・受信順・ホスティングの都合は、ServerSession::TryConfirm が返す確定入力
//   (レーン入力 + SystemInputTick) の値に変換されてここで尽きる。sim へ入る経路はそれ 1 本だけ。
// sim は呼び出し側が Init 済みであること (システム入力あり、レーン数 = session.playerCount)。
// 戻り値は kServerExit*
int RunServerLoop(HeadlessSim& sim, IHostingProvider& hosting, const ServerLoopConfig& cfg);

} // namespace mye
