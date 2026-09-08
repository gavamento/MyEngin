#pragma once

namespace mye {

// GameLogic.dll の書き込み完了プローブ (ProbeWritable / WaitUntilWritable) の回帰テスト。
// net_verify case A/D のフレーク (2 プロセス同時起動でプローブ同士が衝突) の再発防止。
// M70e で「ロードできない DLL を 500ms ごとに再試行して棚を積まない」契約も固定した
// (壊れたファイルを DLL として読ませる。D3D もウィンドウも要らない)
bool RunDllReloaderSelfTest();

} // namespace mye
