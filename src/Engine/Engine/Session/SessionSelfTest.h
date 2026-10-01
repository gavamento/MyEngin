//====================================================================================
//                          SessionSelfTest.h
//  MyEngin/ 秋田蓮音                                                     10/02/2026
//                                          セッション型 / .rep v9 / SessionLanes の自己テスト
//====================================================================================
#pragma once

namespace mye {

// M81b の回帰テスト: レーン状態の純関数、.rep v9 の往復と v8 の読込、
// スナップショットとハッシュ上の SessionLanes、CrashRing の v9 レコード。全項目成功で true
bool RunSessionSelfTest();

} // namespace mye
