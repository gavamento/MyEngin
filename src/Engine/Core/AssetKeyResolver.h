#pragma once
#include <cstdint>
#include <string>

namespace mye {
namespace assetkey {

// path → AssetID キー解決のグローバルフック (M30c)。
// 既定 = HashStr(WideToUtf8(normalizedPath)) — 従来の path-hash と同一 (フック未設定時は
// 完全に従来挙動 = 既存シーン/リプレイはビット不変)。
// AssetDatabase (Engine 層) が起動時に Install し、移動/リネーム済みアセットには同伴 .meta の
// GUID を返す → ライブラリのキーがファイル移動を跨いで安定し、シーンの AssetRef が壊れない。
// 層規約: Core は Engine を知らない — 関数ポインタ注入で逆依存を回避 (prof::/jobs:: と同じ流儀)。
// 引数は NormalizePathKey 済みのパスであること (呼び出し側の責務)。
// スレッド規約: Install/Resolve はメインスレッド専用 (IdForFile 等の呼び出し元と同じ)。
using ResolverFn = uint64_t (*)(void* user, const std::wstring& normalizedPath);

void Install(ResolverFn fn, void* user); // fn=null で既定 (path-hash) に戻す
uint64_t Resolve(const std::wstring& normalizedPath);

} // namespace assetkey
} // namespace mye
