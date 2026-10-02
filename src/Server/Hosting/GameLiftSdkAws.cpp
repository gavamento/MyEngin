//====================================================================================
//                          GameLiftSdkAws.cpp
//  MyEngin/ 秋田蓮音                                                     10/02/2026
//                                          GameLift Server SDK 5.x を IGameLiftSdk に包む実装
//====================================================================================
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include <Windows.h>

#include <aws/gamelift/server/GameLiftServerAPI.h>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Platform/PathUtil.h"
#include "Server/Hosting/GameLiftSdk.h"

namespace mye {
namespace {

namespace Gl = Aws::GameLift;

GameLiftResult FromOutcome(const Gl::GenericOutcome& o)
{
    GameLiftResult r;
    if (!o.IsSuccess()) {
        r.ok = false;
        r.error = o.GetError().GetErrorName() + ": " + o.GetError().GetErrorMessage();
    }
    return r;
}

// InitSDK を待つ上限。接続先に繋がらないと SDK は 3 分近く再試行を続けて戻らない
constexpr std::chrono::seconds kInitTimeout{ 30 };

// SDK の文字列は OS のエラーメッセージ (ANSI コードページ) を含むことがある。UTF-8 として不正なら
// ANSI からの変換を試みる (ログは UTF-8 で統一している)
std::string ToUtf8Text(const char* text)
{
    if (text == nullptr || text[0] == '\0') {
        return {};
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0) > 0) {
        return text;
    }
    const int wideLen = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    if (wideLen <= 0) {
        return text;
    }
    std::wstring wide(static_cast<size_t>(wideLen), L'\0');
    MultiByteToWideChar(CP_ACP, 0, text, -1, wide.data(), wideLen);
    wide.resize(static_cast<size_t>(wideLen) - 1); // 終端の NUL
    return WideToUtf8(wide);
}

// SDK のログをエンジンのログへ流す。任意のスレッドから来るが、Log は内部で施錠する。
// ★SDK は Destroy 後もコールバックを呼びうるので、グローバルだけに触る関数にしてある (userData 無し)。
//   message は書式文字列にしない
void SdkLogSink(Gl::Server::LogLevel level, const char* message, void*)
{
    const std::string text = ToUtf8Text(message);
    switch (level) {
    case Gl::Server::LogLevel::Trace:
    case Gl::Server::LogLevel::Debug:
        MYE_LOG_TRACE("[gamelift-sdk] %s", text.c_str());
        break;
    case Gl::Server::LogLevel::Info:
        MYE_LOG_INFO("[gamelift-sdk] %s", text.c_str());
        break;
    case Gl::Server::LogLevel::Warn:
        MYE_LOG_WARN("[gamelift-sdk] %s", text.c_str());
        break;
    default:
        MYE_LOG_ERROR("[gamelift-sdk] %s", text.c_str());
        break;
    }
}

class AwsGameLiftSdk final : public IGameLiftSdk {
public:
    GameLiftResult InitSdk(const GameLiftConnection& c) override
    {
        // ログの経路はプロセスで 1 度だけ決まる (InitSDK 後は変えられない)。失敗しても致命ではない
        const Gl::GenericOutcome logOutcome =
            Gl::Server::InitCustomLogger(Gl::Server::CustomLoggerConfiguration(&SdkLogSink, nullptr,
                                                                               Gl::Server::LogLevel::Info));
        if (!logOutcome.IsSuccess()) {
            MYE_LOG_WARN("[gamelift] InitCustomLogger failed (%s); the SDK logs to its own file / stdout",
                         logOutcome.GetError().GetErrorMessage().c_str());
        }
        // InitSDK は同期で、繋がらないと戻らない。別スレッドで走らせて待ち時間に上限を付ける。
        // 時間切れのスレッドは回収できないので切り離す (呼び出し側はプロセスを畳むこと)
        const Gl::Server::Model::ServerParameters params(c.webSocketUrl, c.authToken, c.fleetId, c.hostId,
                                                         c.processId);
        auto promise = std::make_shared<std::promise<GameLiftResult>>();
        std::future<GameLiftResult> future = promise->get_future();
        std::thread([promise, params]() {
            const Gl::Server::InitSDKOutcome o = Gl::Server::InitSDK(params);
            GameLiftResult r;
            if (!o.IsSuccess()) {
                r.ok = false;
                r.error = o.GetError().GetErrorName() + ": " + o.GetError().GetErrorMessage();
            }
            promise->set_value(std::move(r));
        }).detach();
        if (future.wait_for(kInitTimeout) != std::future_status::ready) {
            GameLiftResult r;
            r.ok = false;
            r.error = "InitSDK did not finish within " + std::to_string(kInitTimeout.count())
                + " s (the SDK is still retrying the WebSocket connection; check --gamelift-ws-url, the fleet / "
                  "compute IDs and that the auth token has not expired)";
            return r;
        }
        return future.get();
    }

    GameLiftResult ProcessReady(uint16_t port, const std::vector<std::string>& logPaths,
                                const GameLiftCallbacks& callbacks) override
    {
        // std::function をそのまま持たせる (GAMELIFT_USE_STD)。呼ばれるのは SDK のスレッド
        const GameLiftCallbacks cb = callbacks;
        auto onStart = [cb](Gl::Server::Model::GameSession gs) {
            GameLiftGameSession s;
            s.gameSessionId = gs.GetGameSessionId();
            s.maxPlayers = gs.GetMaximumPlayerSessionCount();
            for (const Gl::Server::Model::GameProperty& p : gs.GetGameProperties()) {
                s.properties.emplace_back(p.GetKey(), p.GetValue());
            }
            if (cb.onStartGameSession) {
                cb.onStartGameSession(std::move(s));
            }
        };
        auto onTerminate = [cb]() {
            if (cb.onProcessTerminate) {
                cb.onProcessTerminate();
            }
        };
        auto onHealth = [cb]() { return cb.onHealthCheck ? cb.onHealthCheck() : true; };
        const Gl::Server::ProcessParameters pp(onStart, onTerminate, onHealth, static_cast<int>(port),
                                               Gl::Server::LogParameters(logPaths));
        return FromOutcome(Gl::Server::ProcessReady(pp));
    }

    GameLiftResult ActivateGameSession() override { return FromOutcome(Gl::Server::ActivateGameSession()); }
    GameLiftResult AcceptPlayerSession(const std::string& id) override
    {
        return FromOutcome(Gl::Server::AcceptPlayerSession(id));
    }
    GameLiftResult RemovePlayerSession(const std::string& id) override
    {
        return FromOutcome(Gl::Server::RemovePlayerSession(id));
    }
    GameLiftResult ProcessEnding() override { return FromOutcome(Gl::Server::ProcessEnding()); }
    GameLiftResult Destroy() override { return FromOutcome(Gl::Server::Destroy()); }
};

} // namespace

std::unique_ptr<IGameLiftSdk> CreateAwsGameLiftSdk()
{
    return std::make_unique<AwsGameLiftSdk>();
}

} // namespace mye
