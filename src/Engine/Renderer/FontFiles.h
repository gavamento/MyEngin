//====================================================================================
//                          FontFiles.h
//  MyEngine/ 秋田蓮音                                                      09/13/2026
//                                          プロジェクトフォントの選択規則（FontAtlas と計測表の共通口）
//====================================================================================
#pragma once
#include <string>
#include <vector>

namespace mye {
namespace fontfiles {

// <assetsRoot>\fonts\ 直下の .ttf / .ttc を**フルパスの名前順**で返す (拡張子は大小無視)。
// 先頭が FontAtlas の焼成に使われるフォント。M75d でフォント計測表 (sim が読む) の cook と
// ロード時の照合も同じ規則を通す — 規則を 2 本書くと「絵のフォント」と「表のフォント」が
// 黙ってずれる。assetsRoot が空 / fonts が無ければ空
std::vector<std::wstring> ListProjectFontFiles(const std::wstring& assetsRoot);

} // namespace fontfiles
} // namespace mye
