//====================================================================================
//                          TerminalCoreSelfTest.h
//  MyEngine/ 秋田蓮音                                                      11/19/2026
//                                          ターミナルコア (画面モデル・キー変換・シェル解決・環境組立) の回帰テスト (宣言)
//====================================================================================
#pragma once

namespace mye {

// Fast (suite "terminalcore"): Win32 も子プロセスも使わない。画面モデル (UTF-8・全角・SGR 16/256/truecolor・属性・
// 代替画面・カーソル表示・DA 応答・スクロールバック上限と ED 3・リサイズと最小サイズ・分割された UTF-8/エスケープ列・
// 空入力・大量出力) / キー→VT (Enter/BS/Tab/Esc/矢印/DECCKM/Home/End/PgUp/PgDn/Ins/Del/F1-12/Ctrl/Alt/Shift+Tab/貼付) /
// シェル解決 (自動検出順・明示指定の優先と非フォールバック・引用・引数) / 環境組立 (並び・重複・非上書き・PATH 前置・二重 NUL)
bool RunTerminalCoreSelfTest();

} // namespace mye
