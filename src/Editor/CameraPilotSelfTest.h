#pragma once

namespace mye {

// カメラ操縦モード (PilotApplyLook) のヘッドレス回帰テスト。
// ★このスイートが**唯一の機械検査**になる — 操縦は ImGui のマウス入力で駆動される
//   エディタ機能なので、リプレイにもスクショ回帰にも一切載らない。とくに
//   「ロール (視線軸まわりの傾き) が保たれる」は絵から読み取るのが難しく、
//   壊れても「なんとなく水平に戻った」としか見えない
bool RunCameraPilotSelfTest();

} // namespace mye
