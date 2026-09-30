#pragma once

namespace mye {

// シーンシリアライズの回帰テスト (Editor.exe --selftest で実行)。
// M2 完了条件: 保存 → 読込 → 再保存 のラウンドトリップ一致
bool RunSceneSerializerSelfTest();

} // namespace mye
