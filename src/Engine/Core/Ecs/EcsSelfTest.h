#pragma once

namespace mye {

// ECS の回帰テスト (Editor.exe --selftest で実行)。
// 遅延 Destroy / 世代ハンドル / コマンドバッファ / アーキタイプ移動のデータ保持 / 階層破棄 /
// サブツリー走査 / RNG / クエリキャッシュと Transform スキップの透過性を機械検証する。
// 戻り値: 全テスト成功なら true
bool RunEcsSelfTest();

} // namespace mye
