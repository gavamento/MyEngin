//====================================================================================
//                          IHostingProvider.h
//  MyEngin/ 秋田蓮音                                                     10/02/2026
//                                          ホスティング抽象 (ローカル / GameLift を差し替える口)
//====================================================================================
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace mye {

// ホスティング先の差を吸収する抽象 (M81、spec 4.1.9)。ServerLoop は具体的なホスティングを知らない。
// ★ホスティングの出来事 (開始・終了・ヘルスチェック) は Poll() が返す列としてだけ ServerLoop へ入る。
//   SDK のコールバックなど別スレッドから来るものは実装側がキューへ積み、Poll (メインスレッドの
//   ループ先頭) で渡すこと。sim には ServerSession を通した確定入力以外の経路で何も入らない。

// StartSession が運ぶ上書き値。0 = CLI / 既定のまま。人数(レーン数)は sim の構築時に決まるので上書きできない
struct HostingSessionRequest {
    uint32_t deadlineTicks = 0;
    uint32_t rejoinTimeoutTicks = 0;
};

enum class HostingEventKind : uint8_t {
    StartSession, // セッションを始めてよい (これまでは待機。tick を回さない)
    Terminate,    // 終了要求 (SIGINT / プロセス終了通知)。進行中のセッションは記録を閉じて終わる
    HealthCheck,  // 生きているかの問い合わせ。ループが回っていること自体が応答なので、ServerLoop は何もしない
};

struct HostingEvent {
    HostingEventKind kind = HostingEventKind::HealthCheck;
    HostingSessionRequest session = {};
};

class IHostingProvider {
public:
    virtual ~IHostingProvider() = default;

    virtual const char* Name() const = 0;
    virtual bool Init() = 0;
    // 待受を始めたのでセッションを割り当ててよい、と知らせる。logPaths = 回収してほしいログのパス
    virtual bool NotifyReady(uint16_t port, const std::vector<std::wstring>& logPaths) = 0;
    // 前回の Poll 以降の出来事を out へ追記する (発生順)。ブロックしない
    virtual void Poll(std::vector<HostingEvent>& out) = 0;
    // 参加者の認証 (player session ID)。false なら Hello を拒否する
    virtual bool ValidatePlayer(const char* playerSessionId) = 0;
    // 接続が切れた (再接続の猶予はまだある。レーンは予約されたまま)
    virtual void PlayerLeft(const char* playerSessionId) = 0;
    // 予約が解放された (猶予切れ = 切断確定)。ホスティング側の player session もここで片付ける
    virtual void PlayerReleased(const char*) {}
    // セッションの終わりをホスティングへ知らせる (GameLift なら ProcessEnding)。プロセスはこの後終了する
    virtual void NotifySessionEnded() = 0;
    virtual void Shutdown() = 0;
};

} // namespace mye
