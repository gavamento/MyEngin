#pragma once

namespace mye {

// ECS の回帰テスト (Editor.exe --selftest で実行)。
// M1 完了条件の機械検証: 遅延 Destroy / 世代ハンドル / コマンドバッファ /
// アーキタイプ移動のデータ保持 / 階層破棄。M6 でゴールデンリプレイに置き換わるまでの砦。
// 戻り値: 全テスト成功なら true
bool RunEcsSelfTest();

} // namespace mye
