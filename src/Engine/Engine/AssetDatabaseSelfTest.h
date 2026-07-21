#pragma once

namespace mye {

// AssetDatabase (M23) のヘッドレス回帰テスト:
//   - GUID 継承/安定性: 新規アセットの GUID = 現行 AssetID (HashStr(normpath)) と一致
//   - リネーム耐性: .meta を本体と共に移動すると GUID が永続する
bool RunAssetDatabaseSelfTest();

} // namespace mye
