#pragma once

namespace mye {

// GameLogic.dll の書き込み完了プローブ (ProbeWritable / WaitUntilWritable) の回帰テスト。
// net_verify case A/D のフレーク (2 プロセス同時起動でプローブ同士が衝突) の再発防止
bool RunDllReloaderSelfTest();

} // namespace mye
