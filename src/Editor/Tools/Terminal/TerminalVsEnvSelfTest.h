//====================================================================================
//                          TerminalVsEnvSelfTest.h
//  MyEngine/ 秋田蓮音                                                      11/19/2026
//                                          VS 開発者環境の解析・差分・非同期読み込みとエンジン環境変数の回帰テスト (宣言)
//====================================================================================
#pragma once

namespace mye {

// Fast (suite "terminalvsenv"): Win32 も VS も子プロセスも使わない。`set` 出力の解析 (目印・CRLF・値の '=' ・空値・"=C:" 系の隠し変数・
// Unicode・重複・末尾の空白) / 親環境との差分 (新規・変更・同値・大文字小文字・隠し変数・順序) / 差分の適用 / エンジン環境変数の組み立て
// (親の MYE_* と TERM_PROGRAM の置換・COLORTERM の非上書き・PATH 前置・区切りの扱い・整列) / MYE_TERMINAL_ID の書式 /
// 非同期ローダー (偽の fetch: Loading → 各結果・1 回だけ起動・時間切れと遅れた成功の無視・例外・破棄時の cancel と切り離し)
bool RunTerminalVsEnvSelfTest();

} // namespace mye
