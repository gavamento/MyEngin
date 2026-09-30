//====================================================================================
//                          TagSelfTest.h
//  MyEngin/ 秋田蓮音                                                       09/17/2026
//                                          汎用タグの回帰テスト
//====================================================================================
#pragma once

namespace mye {

// 汎用タグ (TagComponent / Tags / TagNames / ABI v20) と、RT の適用範囲 (RayTracingComponent /
// タグ規則 / ResolveRtScope)、描画側の純関数 (インスタンス run の分割)、
// シェーダのバイトコードキャッシュの形式を検査する。
// D3D もウィンドウも作らない (Editor.exe --selftest)
bool RunTagSelfTest();

} // namespace mye
