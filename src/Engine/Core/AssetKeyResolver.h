#pragma once
#include <cstdint>
#include <string>
#include <string_view>

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

// ---- モデル由来サブアセットのキー (M74a) ----
// メッシュ / マテリアル / スキン / 埋め込みテクスチャの登録名は "guid://<16hex>#mesh3#part0" の形。
// 16hex は Resolve(NormalizePathKey(modelPath)) = 同伴 .meta の GUID (.meta が無いファイルと
// resolver 未設定の selftest では従来の path-hash に落ちる)。AssetID はこの登録名の HashStr。
// ★M74 以前は接頭辞が**正規化した絶対パスそのもの**だったので、シーン JSON に保存した
//   サブアセット ID がチェックアウト先に依存した。2 台の clone 先が違うと、互いが置いたモデルが
//   ログも無く描画から消える (三校のステージで踏んだ)。GUID は .meta ごとコミットされるので、
//   どこに clone しても・アセットを移動しても同じ ID になる
inline constexpr std::string_view kSubAssetKeyScheme = "guid://";
std::string SubAssetKeyPrefix(const std::wstring& modelPath); // 正規化は内部で行う
// 登録名が SubAssetKeyPrefix 形式なら GUID を取り出す (builtin:// や旧形式の絶対パスは false)
bool ParseSubAssetKey(std::string_view key, uint64_t& guidOut);

} // namespace assetkey
} // namespace mye
