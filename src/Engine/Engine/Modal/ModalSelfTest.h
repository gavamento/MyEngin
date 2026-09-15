//====================================================================================
//                          ModalSelfTest.h
//  MyEngine/ 秋田蓮音                                                      09/16/2026
//                                          Deep-Modal ボクセライザのヘッドレス回帰テスト
//====================================================================================
#pragma once

namespace mye {

// Voxelizer (SAT 表面判定 / flood-fill 内部充填 / cell 変換 / .mvox 往復) を検証する。
// **ウィンドウも D3D も一切使わない** (純関数だけを叩く)。
// 推論バックエンドや ModalSoundLibrary のテストは sub-05/06 でこのファイルに積み増す
bool RunModalSelfTest();

} // namespace mye
