#pragma once

namespace mye {

// プロファイラ (M102) のヘッドレス回帰テスト:
//   - スコープの生の記録が開始順 (親 → 子) で、親の添字が正しい
//   - フレーム確定時に「同じ親の下の同名」が 1 行へ合算され、回数が合う
//   - SetFramePhases の値が、次の BeginFrame で確定するフレームへ付く
//   - 履歴のリングが kFrameHistory で頭打ちになり、age 0 が直近を指す
//   - 記録数の上限を超えたぶんは捨てて数える (範囲外へ書かない)
bool RunProfilerSelfTest();

} // namespace mye
