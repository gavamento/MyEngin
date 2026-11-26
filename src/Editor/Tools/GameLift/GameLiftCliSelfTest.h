//====================================================================================
//                          GameLiftCliSelfTest.h
//  MyEngin/ 秋田蓮音                                                       11/27/2026
//                                          aws CLI 層 (argv・解析・子プロセス実行器) の回帰テスト
//====================================================================================
#pragma once

namespace mye {

// 純関数の試験: 引数の引用の往復・利用者入力の検査・argv の組み立て・aws.exe の探索順・環境ブロック・
// エラー分類・応答の解析 (fixture は公式ドキュメントの応答例の形)。プロセスは起こさない
bool RunGameLiftCliSelfTest();

// 実プロセスの試験 (cmd.exe を偽 CLI にする。aws.exe は起動しない): stdout / stderr の分離・終了コード・
// 1 MB 超の出力・両方が大量でも詰まらない・タイムアウト・取消・環境の到達と親の不変・起動失敗・再利用・秘密の消去
bool RunGameLiftCliProcessSelfTest();

} // namespace mye
