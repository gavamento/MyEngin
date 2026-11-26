//====================================================================================
//                          GameLiftFlowSelfTest.h
//  MyEngin/ 秋田蓮音                                                       11/27/2026
//                                          GameLift 窓の手順 (状態機械・Server.exe の子プロセス) の回帰テスト
//====================================================================================
#pragma once

namespace mye {

// 手順の状態機械を偽 CLI (プロセス内の差し替え) と偽ホストで回す試験: ボタンの有効 / 無効と理由・準備の冪等さと確認・
// サーバ起動の argv と環境・セッション参加の再試行と待ち・片付けの順序と待ち・取消・失敗・設定の往復 (トークンを書かない)・既定値の整え方
bool RunGameLiftFlowSelfTest();

// Server.exe の子プロセスの試験 (cmd.exe / powershell.exe を偽サーバにする。Server.exe は起動しない): 環境へのトークンの到達と
// 親の不変・ログのファイル化と ProcessReady の検出・stop イベントが立つこと・猶予内に終わる停止と強制終了・自然終了・起動失敗
bool RunGameLiftServerSelfTest();

} // namespace mye
