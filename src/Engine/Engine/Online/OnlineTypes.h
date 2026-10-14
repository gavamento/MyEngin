//====================================================================================
//                          OnlineTypes.h
//  MyEngin/ 秋田蓮音                                                     10/14/2026
//                                          オンラインサービス (ロビー・P2P・ボイス) の表示用 POD
//====================================================================================
#pragma once
#include <cstddef>
#include <cstdint>

namespace mye {

// ★ここにある値はすべて**機種依存・実時間依存で sim の外**。ロビーの人数、発話中、表示名を
//   ゲームの状態へ書き戻すと .rep が別 PC で再生できなくなる (NetRuntimeInfo と同じ契約)。
//   sim に入るのは、ホストの Server.exe が確定入力 (Join / Leave の SystemInputTick) へ変換した後だけ

inline constexpr uint32_t kOnlineMaxMembers = 4;    // ロビーの上限 = kMaxPlayers と同じ 4 人
inline constexpr size_t kOnlineIdChars = 64;        // ユーザー ID の文字数上限 (EOS の Product User ID は 32 桁の hex)
inline constexpr size_t kOnlineNameChars = 64;      // 表示名 (UTF-8)
inline constexpr size_t kOnlineJoinCodeChars = 6;   // 参加コードの文字数 (英大文字 + 数字、紛らわしい字は除く)
inline constexpr size_t kOnlineMaxPacket = 1170;    // P2P の 1 パケットの上限 (EOS_P2P_MAX_PACKET_SIZE と同じ値)

enum class OnlineState : uint8_t {
    Disabled,  // 設定が空、または SDK を入れずにビルドした
    LoggingIn,
    Ready,     // ログイン済み (ロビーを作る / 入れる)
    Failed,    // 初期化かログインに失敗した。LastError() に理由
};

enum class LobbyState : uint8_t {
    None,
    Creating,
    Searching, // 参加コードでロビーを探している
    Joining,
    InLobby,
    Failed,    // 作成 / 参加に失敗した。LastError() に理由。次の操作で None へ戻る
};

struct OnlineMember {
    char id[kOnlineIdChars] = {};
    char name[kOnlineNameChars] = {};
    bool isOwner = false;
    bool isLocal = false;
    bool speaking = false;   // ボイスの発話中 (ボイスが無効なら常に false)
    bool mutedByMe = false;  // 自分がこの人の受信を止めている
};

struct OnlineLobbyView {
    LobbyState state = LobbyState::None;
    char joinCode[kOnlineJoinCodeChars + 1] = {};
    char ownerId[kOnlineIdChars] = {};
    bool isOwner = false;
    uint32_t memberCount = 0;
    OnlineMember members[kOnlineMaxMembers] = {};
};

} // namespace mye
