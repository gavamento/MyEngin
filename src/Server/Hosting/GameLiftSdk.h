//====================================================================================
//                          GameLiftSdk.h
//  MyEngin/ 秋田蓮音                                                     10/02/2026
//                                          GameLift Server SDK 呼び出し部の差し替え口 (SDK 型を含まない)
//====================================================================================
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mye {

// GameLiftHosting が SDK に対して行う呼び出しの全部。実体 (AwsGameLiftSdk) だけが aws/gamelift を
// include し、selftest は偽物を差し込んで「SDK のスレッドから来たコールバックが tick 境界でだけ
// 処理されること」を確かめる。SDK の型はここへ漏らさない (規則 13-c の境界を細く保つ)。

struct GameLiftConnection {
    std::string webSocketUrl; // Anywhere: RegisterCompute が返す GameLiftServiceSdkEndpoint
    std::string authToken;    // 環境変数から読む (コマンドラインに出さない)。約 15 分で失効する
    std::string fleetId;
    std::string hostId;       // Anywhere: コンピュート名
    std::string processId;
};

struct GameLiftGameSession {
    std::string gameSessionId;
    int maxPlayers = 0;
    std::vector<std::pair<std::string, std::string>> properties; // CreateGameSession の GameProperties
};

// SDK の内部スレッドから呼ばれる。実装は受け取った値をキューへ積むだけで、すぐ戻ること
struct GameLiftCallbacks {
    std::function<void(GameLiftGameSession)> onStartGameSession;
    std::function<void()> onProcessTerminate;
    std::function<bool()> onHealthCheck; // 応答期限は SDK 側で約 50 秒。メインスレッドを待たずに即答する
};

struct GameLiftResult {
    bool ok = true;
    std::string error; // ok == false のとき 1 行の説明 (SDK のエラー名とメッセージ)
};

class IGameLiftSdk {
public:
    virtual ~IGameLiftSdk() = default;

    virtual GameLiftResult InitSdk(const GameLiftConnection& connection) = 0;
    virtual GameLiftResult ProcessReady(uint16_t port, const std::vector<std::string>& logPaths,
                                        const GameLiftCallbacks& callbacks) = 0;
    virtual GameLiftResult ActivateGameSession() = 0;
    virtual GameLiftResult AcceptPlayerSession(const std::string& playerSessionId) = 0;
    virtual GameLiftResult RemovePlayerSession(const std::string& playerSessionId) = 0;
    virtual GameLiftResult ProcessEnding() = 0;
    virtual GameLiftResult Destroy() = 0;
};

// 本物の SDK (Server SDK 5.x) を包んだ実装。GameLiftSdkAws.cpp
std::unique_ptr<IGameLiftSdk> CreateAwsGameLiftSdk();

} // namespace mye
