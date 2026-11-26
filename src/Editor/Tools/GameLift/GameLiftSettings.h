//====================================================================================
//                          GameLiftSettings.h
//  MyEngin/ 秋田蓮音                                                       11/27/2026
//                                          GameLift 窓の個人設定 (editor_settings.json の gameLift) と既定値の整え方
//====================================================================================
#pragma once

#include <string>

namespace mye {

// <project>\.mye\editor_settings.json の "gameLift" オブジェクト (個人設定。AWS アカウントは人ごとなので共有しない)。
// ★認証トークン・資格情報は持たない (保存してはならない)。ID は AWS が返した文字列で、窓が作成・削除のたびに書き換える
struct GameLiftSettings
{
    std::string region;                          // 空 = 未設定 (aws configure get region で埋める)
    std::string locationSuffix = "mye-dev";      // API 名は "custom-" + 接尾辞
    std::string anywhereFleetName = "mye-anywhere";
    std::string computeName;                     // 空 = この PC の名前から整えた既定 (DefaultComputeName)
    std::string ipAddress = "127.0.0.1";         // クライアントがサーバへ届く IP (register-compute --ip-address)
    int port = 7777;
    int maxPlayers = 4;                          // 1〜4
    std::string anywhereFleetId;                 // 準備済みの Anywhere フリート ID。空 = 未準備
};

// 読み込んだ値のうち不正なもの (許可文字・範囲外) を既定値へ戻す。壊れた設定で窓が使えなくならないように
void SanitizeGameLiftSettings(GameLiftSettings& settings);

// PC 名 → コンピュート名の既定。英数と '-' 以外 ('_' '.' を含む) は '-' に置き換え、先頭の '-' は落とす。空なら "mye-dev-pc"
std::string DefaultComputeName(const std::wstring& computerName);

} // namespace mye
