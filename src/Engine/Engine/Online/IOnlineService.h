//====================================================================================
//                          IOnlineService.h
//  MyEngin/ 秋田蓮音                                                     10/14/2026
//                                          ログイン・ロビー・P2P データグラム・ボイスの抽象
//====================================================================================
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>

#include "Engine/Engine/Online/OnlineSettings.h"
#include "Engine/Engine/Online/OnlineTypes.h"

namespace mye {

// オンラインサービスの実装の差し替え口。実装は 3 つ:
//   ・EosOnlineService   (Online\Eos\。SDK を入れてビルドしたときだけ存在する)
//   ・LocalOnlineService (同じ PC のプロセス同士を UDP で繋ぐ開発用の代役)
//   ・無効               (CreateOnlineService が nullptr を返す)
//
// ★呼び出しは**すべてメインスレッドから**。SDK のコールバックは Tick() の中で処理し、
//   ここで返す値はフレームの途中で変わらない。
// ★この層は sim を知らない。ゲームの通信は ServerSession / ClientSession の既存プロトコルを
//   SendPacket / ReceivePacket で運ぶだけで、パケットの中身は見ない (OnlineSession が中継する)。
class IOnlineService {
public:
    virtual ~IOnlineService() = default;

    // 失敗しても false を返すだけで落ちない (オフラインで遊べるようにする)。理由は LastError()
    virtual bool Init(const OnlineSettings& settings) = 0;
    // 1 フレームに 1 回。SDK を回し、溜まったコールバックを処理する
    virtual void Tick() = 0;
    virtual void Shutdown() = 0;

    virtual OnlineState State() const = 0;
    virtual const char* LocalId() const = 0;   // ログイン前は ""
    virtual const char* LastError() const = 0; // 直近の失敗の理由 (無ければ "")
    virtual const char* BackendName() const = 0;

    // ---- ロビー ----
    // 作成者がオーナー。joinCode は呼び出し側が作る (OnlineSession::MakeJoinCode)
    virtual void CreateLobby(const char* joinCode, uint32_t maxMembers) = 0;
    virtual void JoinLobbyByCode(const char* joinCode) = 0;
    virtual void LeaveLobby() = 0;
    virtual const OnlineLobbyView& Lobby() const = 0;

    // ---- P2P データグラム (順序・到達の保証なし。ゲームのプロトコル側が再送する) ----
    // 宛先はロビーのメンバーだけ。size は kOnlineMaxPacket 以下
    virtual bool SendPacket(const char* peerId, const void* data, size_t size) = 0;
    // 受信キューから 1 つ取り出す。無ければ false
    virtual bool ReceivePacket(char (&outPeerId)[kOnlineIdChars], void* buf, size_t capacity, size_t& outSize) = 0;

    // ---- ボイス (ロビー全員に聞こえる無線。距離減衰なし) ----
    // この実装 / この実行でボイスが使えるか (local 実装は常に false、EOS は RTC を初期化できたとき)
    virtual bool VoiceAvailable() const = 0;
    virtual void SetVoiceSending(bool enabled) = 0;
    virtual bool VoiceSending() const = 0;
    virtual void SetMemberMuted(const char* memberId, bool muted) = 0;
};

// settings.backend に応じた実装を作る。None / SDK 無しの Eos → nullptr (Eos のときは理由を WARN)。
// ★Editor / Runtime の main から呼んで EngineConfig へ渡す。Engine.lib の中から呼ばないのは、
//   Server.exe (Engine.lib をリンクする) に EOS SDK の import を持ち込まないため
std::unique_ptr<IOnlineService> CreateOnlineService(OnlineBackend backend);

} // namespace mye
