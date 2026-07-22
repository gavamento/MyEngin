#pragma once

namespace mye {

// AssetOps のヘッドレス回帰テスト (M30d)。RenameAsset の複合サフィックス維持 /
// 衝突連番 / 不正文字拒否 / .meta 同伴 / フォルダリネームを一時ディレクトリで検証する。
// (D&D 移動の実行時テーブル更新は AssetDatabaseSelfTest が担当)
bool RunAssetOpsSelfTest();

} // namespace mye
