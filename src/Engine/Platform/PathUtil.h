#pragma once
#include <string>
#include <string_view>

namespace mye {

// 実行ファイルのあるディレクトリ (末尾セパレータなし)
std::wstring GetExecutableDir();

// exe ディレクトリから上方向に "assets" フォルダを探す (最大 6 階層)。
// VS からの F5 実行 (bin\x64\Debug\) でもリポジトリ直下の assets\ を見つけられる。
// 見つからなければカレントディレクトリ基準の "assets" を返す
std::wstring FindAssetsRoot();

std::string WideToUtf8(std::wstring_view w);
std::wstring Utf8ToWide(std::string_view s);

} // namespace mye
