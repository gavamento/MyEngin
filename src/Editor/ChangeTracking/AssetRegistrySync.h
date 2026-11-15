//====================================================================================
//                          AssetRegistrySync.h
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          復元が書いたファイルを、アセットの登録へ反映する (宣言)
//====================================================================================
#pragma once
#include <string>
#include <vector>

#include "Engine/Engine/HotReload/ReloadHub.h"

namespace mye {

class AssetDatabase;

// ファイルの復元が消した・作った・名前を戻したアセットを、実行時のアセットの登録 (GUID とパスの表) へ反映する。
// ReloadHub::EndBatch は削除を扱わず、新しいファイルも登録しないので、その後始末をここでする
// (Asset Browser の削除・移動と同じ AssetDatabase::MoveAsset / GuidForPath の経路。Engine 層は変えない)。
// changes の path は絶対パス。assets\ の外のパスは触らない。反映した件数を返す
int SyncAssetRegistry(AssetDatabase& db, const std::wstring& projectRoot, const std::vector<BatchChange>& changes);

} // namespace mye
