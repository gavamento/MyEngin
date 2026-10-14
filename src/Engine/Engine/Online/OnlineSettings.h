//====================================================================================
//                          OnlineSettings.h
//  MyEngin/ 秋田蓮音                                                     10/14/2026
//                                          project_settings.json の "online" 節
//====================================================================================
#pragma once
#include <cstdint>
#include <string>

namespace mye {

// どのオンラインサービスを使うか
enum class OnlineBackend : uint8_t {
    None,  // オンライン機能を使わない (既定)
    Eos,   // Epic Online Services。SDK を入れてビルドしたときだけ使える
    Local, // 同じ PC の別プロセス同士を UDP で繋ぐ開発用の代役 (EOS 無しでロビー → 接続の流れを試す)
};

// project_settings.json:
//   "online": {
//     "backend": "eos" | "local" | "none",
//     "displayName": "",
//     "eos": { "productId": "", "sandboxId": "", "deploymentId": "", "clientId": "", "clientSecret": "" },
//     "voice": { "enabled": true, "pushToTalk": false }
//   }
// ★EOS の 5 つの値は Peer2Peer クライアントポリシーでは**実行ファイルに同梱される前提**の値で、
//   秘密鍵ではない。ただし公開リポジトリには空で置き、配布ビルドの直前に埋める運用にする
//   (docs\eos-setup.md)
struct OnlineSettings {
    OnlineBackend backend = OnlineBackend::None;
    std::string displayName; // 空なら Windows のユーザー名
    std::string productId;
    std::string sandboxId;
    std::string deploymentId;
    std::string clientId;
    std::string clientSecret;
    bool voiceEnabled = true;
    bool pushToTalk = false; // false = 常時送信 (ミュートは手動)
};

// assetsRoot の project_settings.json から読む。節が無い / 壊れている → 既定値 (None) で false。
// 値の欠け (EOS の ID が空など) はここでは咎めず、サービスの Init が理由付きで失敗する
bool LoadOnlineSettings(const std::wstring& assetsRoot, OnlineSettings& out);

const char* OnlineBackendName(OnlineBackend backend);

} // namespace mye
