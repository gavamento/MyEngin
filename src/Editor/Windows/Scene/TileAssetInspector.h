//====================================================================================
//                          TileAssetInspector.h
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                          タイルパレット / タイルマップ資産の Inspector 簡易表示
//====================================================================================
#pragma once
#include <string>

#include "Engine/Engine/Asset/AssetDatabase.h"

namespace mye {

// .tilepalette.json (タイル数) と .tilemap.json (レイヤー数・チャンク数・範囲・パレット) を読み取り専用で表示する。
// 編集は sub-12 のパレット窓 / sub-13 の UI が担当
void DrawTileAssetInspector(AssetType type, const std::wstring& path);

} // namespace mye
