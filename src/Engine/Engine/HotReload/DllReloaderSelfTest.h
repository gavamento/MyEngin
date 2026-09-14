#pragma once

namespace mye {

// GameLogic.dll の書き込み完了プローブ (ProbeWritable / WaitUntilWritable) の回帰テスト。
// net_verify case A/D のフレーク (2 プロセス同時起動でプローブ同士が衝突) の再発防止。
// ロードできない DLL を書き直されるまで再試行しない契約も固定する (理由は DllReloader.cpp の TryCopyAndLoad)
// (壊れたファイルを DLL として読ませる。D3D もウィンドウも要らない)
bool RunDllReloaderSelfTest();

} // namespace mye
