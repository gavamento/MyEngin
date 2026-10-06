//====================================================================================
//                          DeviceLostRescueSelfTest.h
//  MyEngine/ 秋田蓮音                                                      10/07/2026
//                                          デバイス消失時の退避保存の回帰テスト
//====================================================================================
#pragma once

namespace mye {

// デバイス消失時の退避保存 (M88) の回帰テスト。
// 編集状態 / Play 前状態の保存 → 再読込の一致、元ファイルを変えないこと、書けないときの失敗を検証する。
// **Editor 層に置く**のは PlayModeController と DeviceLostRescue が Editor 層だから
bool RunDeviceLostRescueSelfTest();

} // namespace mye
