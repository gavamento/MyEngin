//====================================================================================
//                          DisturbanceFieldSelfTest.h
//  MyEngin/ 秋田蓮音                                                       11/08/2026
//                                          かき乱しフィールド (M96) の回帰テスト
//====================================================================================
#pragma once

namespace mye {

// 静止状態の不変、減衰と復元、スナップショット往復、境界、源の速度の 3 経路、波紋の対称性、
// 場の寿命、2 ワールド並走の決定論、場の無いシーンのハッシュ不変を確かめる
bool RunDisturbanceFieldSelfTest();

// 重い 4 項目 (128x128 を 600 tick、1200 tick の復元、10000 tick の波紋、2 ワールド 600 tick と焼き込みハッシュ)。Full 層
bool RunDisturbanceFieldLongSelfTest();

// --field-bench: ヘッドレス計測。128x128・全チャネル・20 反復の step とハッシュの ms (jobs なし / あり)、
// スナップショット 1 枚のバイト数、焼き込みハッシュの再採取値、ソルバの断言 (発散・渦・風) を MYE_LOG_INFO へ出す。
// selftest の層とは独立に回せる計測専用コマンドで、戻り値は常に 0
int RunDisturbanceFieldBenchmark();

} // namespace mye
