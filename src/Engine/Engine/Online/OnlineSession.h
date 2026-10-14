//====================================================================================
//                          OnlineSession.h
//  MyEngin/ 秋田蓮音                                                     10/14/2026
//                                          ロビーとゲームのセッションをつなぐ (ホストのサーバ起動・P2P 中継)
//====================================================================================
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Engine/Engine/Online/HostServerProcess.h"
#include "Engine/Engine/Online/IOnlineService.h"
#include "Engine/Platform/Net/UdpSocket.h"

namespace mye {

// EngineLoop へ渡す「ここへ ClientSession で繋げ」という要求。フレーム境界で StartClient が消費する
struct OnlineConnectRequest {
    std::wstring target;         // 常に 127.0.0.1:<port> (ホスト = 自分の Server.exe、参加側 = 中継のポート)
    std::string playerSessionId; // オンラインのユーザー ID (サーバのログと再接続の照合に使う)
    uint32_t connectTimeoutMs = 0;
};

enum class OnlinePhase : uint8_t {
    Offline,        // ロビーに居ない (1 人で遊んでいる)
    CreatingLobby,  // ホスト: ロビーを作っている
    JoiningLobby,   // 参加側: 参加コードでロビーへ入っている
    InSession,      // ゲームのセッションへ繋いだ (中継が回っている)
    Error,          // 失敗した。Message() に理由。Leave() で Offline へ戻る
};

// ロビーの出来事をゲームのセッションの操作に変える。トポロジはホスト兼サーバ (ADR-031):
//
//   ホスト:   自分の Runtime ─UDP─ 127.0.0.1:P ─ Server.exe (子プロセス)
//             参加者 X ─オンラインの P2P─ ホストの Runtime ─UDP (X 専用ソケット)─ 127.0.0.1:P
//   参加側:   自分の Runtime ─UDP─ 127.0.0.1:B (中継) ─オンラインの P2P─ ホスト
//
// ★ゲームのパケットの中身は見ない。ServerSession / ClientSession の既存プロトコルをそのまま運ぶ。
//   P2P の 1 パケットの上限 (kOnlineMaxPacket) を超えるもの (Confirmed は最大 1.3 KB 程度) だけ分割する。
// ★ホストのサーバは参加者ごとに別の UDP ソケットから受ける = 宛先が分かれるので、サーバから見ると
//   普通の 4 クライアントと区別が付かない (Server.exe の PeerTable がそのまま効く)
class OnlineSession {
public:
    OnlineSession();
    ~OnlineSession();
    OnlineSession(const OnlineSession&) = delete;
    OnlineSession& operator=(const OnlineSession&) = delete;

    // service が null ならオンライン機能は無い (Phase は常に Offline)。launch の port は使わない (起動時に選ぶ)
    void Attach(IOnlineService* service, const HostServerLaunch& launch, const OnlineSettings& settings);
    IOnlineService* Service() const { return service_; }

    // codeOverride が空なら参加コードを作る (検証では --online-code で固定する)
    void Host(const std::string& codeOverride = std::string());
    void Join(const char* joinCode);
    void Leave();
    // service->Tick() の後、1 フレームに 1 回
    void Update();
    // 接続の要求を 1 つ取り出す (無ければ false)
    bool TakeConnectRequest(OnlineConnectRequest& out);
    // ゲームのセッションが終わった (切断・拒否) ことを EngineLoop が知らせる。
    // worldEnded = 参加後に終わった (世界はセッションのものなので止めた。1 人で遊ぶには再起動が要る)
    void NotifySessionEnded(const char* reason, bool worldEnded);
    // 窓の「退出」。自分のクライアントに Bye を送らせてから中継を畳む順序があるので、EngineLoop が
    // フレーム境界で TakeLeaveRequest を見て Leave() を呼ぶ
    void RequestLeave() { leaveRequested_ = true; }
    bool TakeLeaveRequest();
    bool WorldEnded() const { return worldEnded_; }

    // ボイスの設定 (窓の表示用)。送信の on / off とミュートはサービスへ直接
    bool VoiceEnabled() const { return settings_.voiceEnabled && service_ != nullptr && service_->VoiceAvailable(); }
    bool PushToTalk() const { return settings_.pushToTalk; }
    const char* PushToTalkKeyName() const { return pttKeyName_; }
    // 押している間だけ送る (PushToTalk のとき)。EngineLoop が出力レーンで生のキー状態を渡す。
    // ★sim の入力 (InputSnapshot) には載せない: 載せると .rep の中身がボイスの操作で変わる
    void UpdatePushToTalk(bool keyDown);
    char PushToTalkKey() const { return settings_.pushToTalkKey; }

    OnlinePhase Phase() const { return phase_; }
    const std::string& Message() const { return message_; }
    bool IsHost() const { return isHost_; }

    // 参加コード (英大文字 + 数字、I / O / 0 / 1 は除く)。entropy は sim の外の値 (時刻やプロセス ID) でよい
    static std::string MakeJoinCode(uint64_t entropy);

    // ---- 分割 (selftest 用に公開) ----
    // 1 つのゲームパケットを P2P の 1 パケット以下の断片に分ける。seq は送り手ごとの通し番号
    static void Fragment(const uint8_t* data, size_t size, uint8_t seq, std::vector<std::vector<uint8_t>>& out);
    // 断片を組み立てる。揃ったら true で whole に入れる (断片が欠けたパケットは捨てる = UDP の欠落と同じ扱い)
    struct Reassembler {
        bool Push(const uint8_t* frag, size_t size, std::vector<uint8_t>& whole);
        uint8_t seq = 0;
        uint8_t count = 0;
        uint8_t got = 0;          // 受け取った断片のビット
        std::vector<uint8_t> parts[4];
    };
    static constexpr size_t kMaxFragments = 4;

private:
    struct HostPeer;
    void Fail(const std::string& reason);
    void PumpHost();
    void PumpJoin();
    void SendToPeer(const char* peerId, const uint8_t* data, size_t size, uint8_t& seq);
    void ResetTransport();

    IOnlineService* service_ = nullptr;
    HostServerLaunch launch_;
    OnlineSettings settings_;
    char pttKeyName_[2] = {};
    bool pttDown_ = false;
    bool leaveRequested_ = false;
    bool worldEnded_ = false;
    OnlinePhase phase_ = OnlinePhase::Offline;
    std::string message_;
    bool isHost_ = false;
    bool hasRequest_ = false;
    OnlineConnectRequest request_;

    // ホスト
    HostServerProcess server_;
    NetAddress serverAddr_ = {};
    std::vector<std::unique_ptr<HostPeer>> peers_;

    // 参加側
    UdpSocket bridge_;
    NetAddress clientAddr_ = {};
    uint8_t toHostSeq_ = 0;
    std::unique_ptr<Reassembler> fromHost_;

    std::vector<uint8_t> buf_ = std::vector<uint8_t>(8192);
};

} // namespace mye
