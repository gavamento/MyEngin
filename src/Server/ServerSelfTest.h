//====================================================================================
//                          ServerSelfTest.h
//  MyEngin/ 秋田蓮音                                                     10/02/2026
//                                          Server.exe --selftest (GameLift ホスティング / .rep の幕引き)
//====================================================================================
#pragma once

namespace mye {

// GameLiftHosting (偽 SDK) と、Terminate を受けたときに .rep が閉じられる経路 (偽 SDK + 実ループ /
// 実プロセスへの Ctrl+Break) の回帰テスト。全部通れば true。
// 実ループのテストは起動シーン (assets) を読むので、リポジトリのルートから実行すること
bool RunServerSelfTest();

} // namespace mye
