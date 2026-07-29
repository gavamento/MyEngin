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

// エンジンリポジトリのルートを exe から上方向に探す (最大 6 階層、src\Shared\ScriptAPI.h が目印)。
// C++ スクリプトのコンパイルに必要な Shared ヘッダの位置解決に使う。
// 見つからなければ空 (リポジトリから切り離して配布された exe など)
std::wstring FindEngineRepoRoot();

// エンジン組込みシェーダ (<engineRepo>\assets\shaders) の絶対パス。
// プロジェクトの assets\shaders に無いシェーダはここから解決される (2 ルート化)。
// 配布済み Runtime など、リポジトリが見つからない環境では空
std::wstring FindEngineShaderDir();

std::string WideToUtf8(std::wstring_view w);
std::wstring Utf8ToWide(std::string_view s);

// パス比較用の正規化: 絶対化 + 小文字化 + '\\' 統一。
// AssetID のキーやホットリロードの照合は必ずこの結果を使う
std::wstring NormalizePathKey(const std::wstring& path);

} // namespace mye
