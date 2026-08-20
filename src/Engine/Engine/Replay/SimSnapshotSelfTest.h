#pragma once

namespace mye {

// sim スナップショット (M52d) の自己テスト。
// 「撮って壊して戻すとハッシュが元に戻る」だけでなく、ハッシュに出ない
// (が次の tick から効いてくる) EntityID の世代 / freeIndices の LIFO 順 /
// アーキタイプ生成順まで固定する。全項目成功で true
bool RunSimSnapshotSelfTest();

} // namespace mye
