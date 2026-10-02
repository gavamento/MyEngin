//====================================================================================
//                          GameLiftHosting.h
//  MyEngin/ 秋田蓮音                                                     10/02/2026
//                                          AWS GameLift (Server SDK 5.x) 向けのホスティング
//====================================================================================
#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <set>
#include <vector>

#include "Server/Hosting/GameLiftSdk.h"
#include "Server/Hosting/IHostingProvider.h"

namespace mye {

struct GameLiftHostingOptions {
    GameLiftConnection connection;
    uint32_t laneCount = 4;          // --max-players (sim のレーン数)。GameSession の最大人数との小さい方を入場の上限にする
    uint32_t healthStaleMs = 30000;  // Poll (メインループの周回) がこれより長く止まったらヘルスチェックに不健康と答える
    std::function<uint64_t()> nowMs; // 時計 (ms)。null = steady_clock。selftest が差し替える
};

// CreateGameSession の GameProperties で受け取る上書き値 (どちらも省略可)。
// キーにピリオドを使わない (GameLift の SearchGameSessions で検索できなくなる)
inline constexpr const char* kGameLiftPropDeadlineTicks = "myeDeadlineTicks";
inline constexpr const char* kGameLiftPropRejoinTimeoutTicks = "myeRejoinTimeoutTicks";

// GameLift 向けのホスティング。SDK 呼び出しは IGameLiftSdk 越し (本物は GameLiftSdkAws.cpp)。
// ★スレッド: SDK のコールバック (StartGameSession / ProcessTerminate / HealthCheck) は SDK の内部スレッドから
//   来る。前 2 つはミューテックス付きキューへ積むだけで、Poll (メインループ) が取り出す。ヘルスチェックだけは
//   メインスレッドを待たず、Poll の最終周回時刻 (atomic) を見て即答する。
//   SDK は Destroy 後もコールバックスレッドが生き残りうるので、共有状態は shared_ptr で
//   コールバック側にも持たせてある (本体が先に消えても読める)。
// ★Poll 以外のメソッドはすべてメインスレッドから呼ぶこと。
class GameLiftHosting final : public IHostingProvider {
public:
    GameLiftHosting(std::unique_ptr<IGameLiftSdk> sdk, GameLiftHostingOptions options);
    ~GameLiftHosting() override;

    const char* Name() const override { return "gamelift"; }
    // InitSDK。失敗したら false で、理由は LastError() に 1 行で入る
    bool Init() override;
    // ProcessReady。以後 StartGameSession / Terminate のコールバックが来うる
    bool NotifyReady(uint16_t port, const std::vector<std::wstring>& logPaths) override;
    void Poll(std::vector<HostingEvent>& out) override;
    // AcceptPlayerSession の成否。承認済みの ID (Hello の再送・再接続) は SDK を呼ばずに通す
    bool ValidatePlayer(const char* playerSessionId) override;
    // 一時的な切断では予約を残す (再接続できる)。GameLift へは何もしない
    void PlayerLeft(const char* playerSessionId) override;
    // 予約の解放 (切断確定): RemovePlayerSession
    void PlayerReleased(const char* playerSessionId) override;
    // プロセスの終了を GameLift へ知らせる (ProcessEnding)。2 回目以降は何もしない
    void NotifySessionEnded() override;
    // ProcessEnding が未送信なら送ってから Destroy。何度呼んでもよい
    void Shutdown() override;

    const std::string& LastError() const { return lastError_; }

private:
    struct Shared; // コールバック側と共有する状態 (キュー・ヘルス時刻)
    uint64_t NowMs() const;
    bool Fail(const char* what, const GameLiftResult& r);

    std::unique_ptr<IGameLiftSdk> sdk_;
    GameLiftHostingOptions opt_;
    std::shared_ptr<Shared> shared_;
    std::string lastError_;
    bool initialised_ = false;
    bool readySent_ = false;
    bool endingSent_ = false;
    bool destroyed_ = false;
    bool sessionStarted_ = false;
    bool terminateEmitted_ = false;
    uint32_t admissionLimit_ = 0; // 承認できる player session 数の上限 (min(GameSession の最大人数, レーン数))
    std::set<std::string> accepted_; // 承認済みで未解放の player session ID (メインスレッドのみ)
};

} // namespace mye
