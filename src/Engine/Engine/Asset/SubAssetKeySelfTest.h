#pragma once

namespace mye {

// モデル由来サブアセットのキー (M74a: guid:// 接頭辞 = チェックアウト非依存) と
// 旧 ID の移行 (M74b: --migrate-subasset-ids) の自己テスト。全項目成功で true
bool RunSubAssetKeySelfTest();

} // namespace mye
