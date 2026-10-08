//====================================================================================
//                          AnimatorControllerEditSelfTest.h
//  MyEngin/ 秋田蓮音                                                       10/08/2026
//                                  コントローラ窓のステート編集の回帰テスト
//====================================================================================
#pragma once

namespace mye {

// M89g: 骨の駆動の種類の切り替え・骨クリップ名とハッシュの同期・ブレンドツリーの子の追加と、
// 編集結果が .controller.json の往復で保たれること
bool RunAnimatorControllerEditSelfTest();

} // namespace mye
