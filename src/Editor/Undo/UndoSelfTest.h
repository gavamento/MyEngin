#pragma once

namespace mye {

// Undo/Redo コアの回帰テスト (Editor.exe --selftest で実行、M8)。
// - フィールド変更 / コンポーネント追加削除 / 親子付け を undo 全巻き戻し → WorldHash 初期一致、
//   redo 全再適用 → 編集後ハッシュ一致 (生成/破棄を含まない op は完全可逆)
// - 生成 / 破棄の undo/redo は構造 (存在・値) で検証 (generation は不可逆なためハッシュ非対象)
bool RunUndoSelfTest();

} // namespace mye
