//====================================================================================
//                          NavSurfaceSelfTest.h
//  MyEngin/ 秋田蓮音                                                     10/03/2026
//                                          NavMeshSurface のベイク・.mnav・読み込みの回帰テスト (M82b)
//====================================================================================
#pragma once

namespace mye {

// Editor 無しで World -> 入力収集 -> タイルベイク -> .mnav -> 読み込み -> 経路クエリ -> 輪郭の線 を通す。
// 守っている不変量:
//   - 入力収集は動く物 (Rigidbody) とトリガーを含めず、同じ World から同じ三角形列を返す
//   - 同じ入力のベイクは同じバイト列 (ハッシュを Debug / Release で同じ期待値と照合)
//   - NavBakeAsset の各層は、NavBakeTile を 1 枚ずつ呼んだ結果と一致する (実行時の再ベイクも同じ関数を呼ぶ)
//   - .mnav は書く -> 読む -> 書くでバイト一致し、壊れた入力は落ちずに失敗する
//   - NavSystem は Surface の .mnav を読み、Surface が無いシーンでは何も持たない
bool RunNavSurfaceSelfTest();

} // namespace mye
