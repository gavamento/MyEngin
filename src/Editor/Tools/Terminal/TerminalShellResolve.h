//====================================================================================
//                          TerminalShellResolve.h
//  MyEngine/ 秋田蓮音                                                      11/19/2026
//                                          ターミナルで起動するシェルの解決 (設定値と存在確認から実行パスと引数を決める)
//====================================================================================
#pragma once
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace mye {

// 自動検出で試す順 (spec 4.1)。PowerShell 7 → Windows PowerShell → cmd
inline constexpr const char* kTerminalAutoShells[] = { "pwsh.exe", "powershell.exe", "cmd.exe" };

enum class TerminalShellError
{
    None,
    // 設定で明示したシェルが見つからない。自動検出へは落とさない (明示指定を尊重する)
    ConfiguredNotFound,
    // 設定の引用符が閉じていない
    ConfiguredMalformed,
    // 自動検出の候補が 1 つも見つからない
    NoAutoShell,
};

struct TerminalShellCommand
{
    TerminalShellError error = TerminalShellError::None;
    std::string path;                // 実行ファイルの絶対パス (UTF-8)。error が None のときだけ有効
    std::vector<std::string> args;   // 実行ファイルの後ろに付ける引数 (引用前の生の値)。引用は呼び出し側 (QuoteCommandLineArg)
};

// 名前またはパスを受け、存在すれば絶対パスを、無ければ空を返す関数。PATH / 既定位置の探索は呼び出し側 (Win32 側) が持つ
using TerminalExecutableFinder = std::function<std::string(const std::string& nameOrPath)>;

// setting: 設定 terminalShell の値 (UTF-8)。空 (空白だけを含む) なら自動検出。
//   "\"C:\\Program Files\\PowerShell\\7\\pwsh.exe\" -NoLogo" のように引用した先頭をパス、残りを引数とする。
//   引用が無ければ、まず全体を 1 つのパスとして探し、無ければ最初の空白までをパス・残りを引数とする
// 純関数: finder 以外の外部状態を読まない
TerminalShellCommand ResolveTerminalShell(std::string_view setting, const TerminalExecutableFinder& finder);

} // namespace mye
