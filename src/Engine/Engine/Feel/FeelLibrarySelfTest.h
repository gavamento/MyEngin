//====================================================================================
//                          FeelLibrarySelfTest.h
//  MyEngin/ 秋田蓮音                                                       11/04/2026
//                                          手触りのプリセット資産と FeelTrigger (M94d) の回帰テスト
//====================================================================================
#pragma once

namespace mye {

// .feel.json の JSON 往復 (preset / custom のカーブ、欠けたキーの既定値、壊れた値の正規化)、
// 名前での検索 (同名はパスの序数比較で最小)、FeelTrigger の発火 (力積のしきい値・強さ・間隔) を確かめる
bool RunFeelLibrarySelfTest();

} // namespace mye
