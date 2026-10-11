//====================================================================================
//                          ComputeStateSelfTest.h
//  MyEngin/ 秋田蓮音                                                       10/12/2026
//                                          ForEach の State バッファ (スナップショット・ハッシュ・寿命) の回帰テスト宣言
//====================================================================================
#pragma once

namespace mye {

namespace compute {
class ComputeService;
}

// State バッファの SimSnapshot 往復・WorldHash・所有エンティティによる解放の回帰テスト (M91d)。
//   - ストア単体の保存/復元 (採番・世代・持ち主・中身、Fx は世代が合うものだけ残る、壊れた blob は拒む)
//   - 内容ゲート付きのハッシュ (使っていないシーンの値は動かない、中身・持ち主の違いで割れる、ダンプに出る)
//   - CaptureSimSnapshot / RestoreSimSnapshot の往復と、復元後に同じ操作から同じハンドルが出ること
//   - エンティティ破棄 (tick 末) で解放、全解放で採番が新規起動と同じに戻ること
bool RunComputeStateSelfTest();

// デバイス無しの ComputeService (HeadlessSim のもの) で State の ForEach が CPU で動き、
// EngineLoop と同じ結果になることを確かめる。カーネルの登録は関数内で行う (M91d、Server.exe --selftest 用)
bool CheckComputeStateRunsWithoutDevice(compute::ComputeService& service);

} // namespace mye
