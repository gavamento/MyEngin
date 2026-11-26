//====================================================================================
//                          ServerPackageSelfTest.h
//  MyEngin/ 秋田蓮音                                                       11/27/2026
//                                          GameLift 用サーバ版の組み立ての回帰テスト
//====================================================================================
#pragma once

namespace mye {

// BuildServerPackage の中身 (入れる / 入れない・バイト一致・install.bat)・足りない入力の全列挙・失敗時に出力が変わらないこと・
// 入れ替えの安全側の判定 (偽の書き出しフォルダ / 偽の exeDir / 偽の VS。実プロセスは起こさない)
bool RunServerPackageSelfTest();

// 実際の Server.exe (exeDir にあれば) と小さな assets で組んで局所起動確認 (exit 5 + ready) が通ること、
// 起動確認が組んだフォルダを汚さないこと、偽の Server.exe (cmd.exe) で失敗が検出できること。Server.exe が無ければ SKIP
bool RunServerPackageLocalSelfTest();

} // namespace mye
