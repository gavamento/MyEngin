#pragma once

namespace mye {

// JobSystem (M25) のヘッドレス回帰テスト:
//   - ParallelFor/ParallelRanges が [0,total) を「漏れなく重複なく」処理する
//   - enabled(並列) と disabled(直列) の出力がビット単位で一致する (決定論)
bool RunJobSystemSelfTest();

} // namespace mye
