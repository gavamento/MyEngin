//====================================================================================
//                          TerminalEnvironment.h
//  MyEngine/ 秋田蓮音                                                      11/19/2026
//                                          ターミナルの子プロセスへ渡す環境変数の組み立て (Windows 環境ブロックの規則)
//====================================================================================
#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace mye {

struct TerminalEnvVar
{
    std::string name;    // UTF-8
    std::string value;   // UTF-8
};

// 子プロセスの環境を組み立てる純関数。
// - parent: 親の環境 (GetEnvironmentStrings の順。VS 開発者環境の差分は呼び出し側で反映済み)。
//   名前が空・2 文字目以降に '=' を含む項目は捨てる。"=C:" のような '=' で始まる隠し変数は残す。
//   大文字小文字を無視して重複したら先に出た方を残す
// - additions: 追加する変数 (MYE_PROJECT_ROOT など)。親に同名 (大文字小文字無視) があれば上書きしない
// - pathPrefix: 空でなければ PATH の先頭へ "pathPrefix;" を付ける (PATH が無ければ作る)。既存の名前の綴り (Path 等) は保つ
// 戻り値は名前の大文字小文字を無視した昇順 (Windows の CreateProcess の環境ブロック規則)
std::vector<TerminalEnvVar> BuildTerminalEnvironment(const std::vector<TerminalEnvVar>& parent,
                                                     const std::vector<TerminalEnvVar>& additions,
                                                     std::string_view pathPrefix);

// "NAME=VALUE\0" を並べ、最後にもう 1 つ '\0' を付けた UTF-8 のブロックにする (空なら "\0\0")。
// UTF-16 への変換と CREATE_UNICODE_ENVIRONMENT は呼び出し側 (Win32 側)
std::string FormatTerminalEnvironmentBlock(const std::vector<TerminalEnvVar>& env);

// 大文字小文字を無視した名前の比較 (ASCII の範囲で畳む)。a < b なら負
int CompareTerminalEnvName(std::string_view a, std::string_view b);

} // namespace mye
